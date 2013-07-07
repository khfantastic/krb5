/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* #pragma ident        "@(#)kdb_log.c  1.3     04/02/23 SMI" */

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <k5-int.h>
#include <stdlib.h>
#include <limits.h>
#include <syslog.h>
#include "kdb5.h"
#include "kdb_log.h"
#include "kdb5int.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

/*
 * This modules includes all the necessary functions that create and
 * modify the Kerberos principal update and header logs.
 */

#define getpagesize()   sysconf(_SC_PAGESIZE)

static int              pagesize = 0;

#define INIT_ULOG(ctx)                          \
    log_ctx = ctx->kdblog_context;              \
    assert(log_ctx != NULL);                    \
    ulog = log_ctx->ulog;                       \
    assert(ulog != NULL)

/* XXX */
typedef unsigned long ulong_t;
typedef unsigned int uint_t;

static int extend_file_to(int fd, uint_t new_size);

krb5_error_code
ulog_lock(krb5_context ctx, int mode)
{
    kdb_log_context *log_ctx = NULL;
    kdb_hlog_t *ulog = NULL;

    if (ctx == NULL)
        return KRB5_LOG_ERROR;
    if (ctx->kdblog_context == NULL || ctx->kdblog_context->iproprole == IPROP_NULL)
        return 0;
    INIT_ULOG(ctx);
    return krb5_lock_file(ctx, log_ctx->ulogfd, mode);
}

/*
 * Sync update entry to disk.
 */
static krb5_error_code
ulog_sync_update(kdb_hlog_t *ulog, kdb_ent_header_t *upd)
{
    ulong_t             start, end, size;
    krb5_error_code     retval;

    if (ulog == NULL)
        return (KRB5_LOG_ERROR);

    if (!pagesize)
        pagesize = getpagesize();

    start = ((ulong_t)upd) & (~(pagesize-1));

    end = (((ulong_t)upd) + ulog->kdb_block +
           (pagesize-1)) & (~(pagesize-1));

    size = end - start;
    if ((retval = msync((caddr_t)start, size, MS_SYNC))) {
        return (retval);
    }

    return (0);
}

/*
 * Sync memory to disk for the update log header.
 */
static void
ulog_sync_header(kdb_hlog_t *ulog)
{

    if (!pagesize)
        pagesize = getpagesize();

    if (msync((caddr_t)ulog, pagesize, MS_SYNC)) {
        /*
         * Couldn't sync to disk, let's panic
         */
        syslog(LOG_ERR, _("ulog_sync_header: could not sync to disk"));
        abort();
    }
}

/*
 * Resizes the array elements.  We reinitialize the update log rather than
 * unrolling the the log and copying it over to a temporary log for obvious
 * performance reasons.  Slaves will subsequently do a full resync, but
 * the need for resizing should be very small.
 */
static krb5_error_code
ulog_resize(kdb_hlog_t *ulog, uint32_t ulogentries, int ulogfd, uint_t recsize)
{
    uint_t              new_block, new_size;

    if (ulog == NULL)
        return (KRB5_LOG_ERROR);

    new_size = sizeof (kdb_hlog_t);

    if (recsize > ULOG_BLOCK) {
        new_block = (recsize + ULOG_BLOCK - 1) / ULOG_BLOCK;
        new_block *= ULOG_BLOCK;
    }
    else
	new_block = ULOG_BLOCK;

    new_size += ulogentries * new_block;

    if (new_size <= MAXLOGLEN) {
        /*
         * Reinit log with new block size
         */
        (void) memset(ulog, 0, sizeof (kdb_hlog_t));

        ulog->kdb_hmagic = KDB_ULOG_HDR_MAGIC;
        ulog->db_version_num = KDB_VERSION;
        ulog->kdb_state = KDB_STABLE;
        ulog->kdb_block = new_block;

        ulog_sync_header(ulog);

        /*
         * Time to expand log considering new block size
         */
        if (extend_file_to(ulogfd, new_size) < 0)
            return errno;
    } else {
        /*
         * Can't map into file larger than MAXLOGLEN
         */
        return (KRB5_LOG_ERROR);
    }

    return (0);
}

/*
 * Adds an entry to the update log.
 * The layout of the update log looks like:
 *
 * header log -> [ update header -> xdr(kdb_incr_update_t) ], ...
 */
krb5_error_code
ulog_add_update(krb5_context context, kdb_incr_update_t *upd)
{
    XDR         xdrs;
    kdbe_time_t ktime;
    struct timeval      timestamp;
    kdb_ent_header_t *indx_log;
    uint_t              i, recsize;
    ulong_t             upd_size;
    krb5_error_code     retval;
    kdb_sno_t   cur_sno;
    kdb_log_context     *log_ctx;
    kdb_hlog_t  *ulog = NULL;
    uint32_t    ulogentries;
    int         ulogfd;

    INIT_ULOG(context);
    ulogentries = log_ctx->ulogentries;
    ulogfd = log_ctx->ulogfd;

    if (upd == NULL)
        return (KRB5_LOG_ERROR);

    (void) gettimeofday(&timestamp, NULL);
    ktime.seconds = timestamp.tv_sec;
    ktime.useconds = timestamp.tv_usec;

    upd_size = xdr_sizeof((xdrproc_t)xdr_kdb_incr_update_t, upd);

    recsize = sizeof (kdb_ent_header_t) + upd_size;

    if (recsize > ulog->kdb_block) {
        if ((retval = ulog_resize(ulog, ulogentries, ulogfd, recsize))) {
            /* Resize element array failed */
            return (retval);
        }
    }

    cur_sno = ulog->kdb_last_sno;

    /*
     * We need to overflow our sno, replicas will do full
     * resyncs once they see their sno > than the masters.
     */
    if (cur_sno == ULONG_MAX || cur_sno <= 0 || ulog->kdb_num > ulogentries) {
        ulog->kdb_num = 0;
        cur_sno = 1;
    } else
        cur_sno++;

    /*
     * We squirrel this away for finish_update() to index
     */
    upd->kdb_entry_sno = cur_sno;

    i = (cur_sno - 1) % ulogentries;

    indx_log = (kdb_ent_header_t *)INDEX(ulog, i);

    (void) memset(indx_log, 0, ulog->kdb_block);

    indx_log->kdb_umagic = KDB_ULOG_MAGIC;
    indx_log->kdb_entry_size = upd_size;
    indx_log->kdb_entry_sno = cur_sno;
    indx_log->kdb_time = upd->kdb_time = ktime;
    indx_log->kdb_commit = upd->kdb_commit = FALSE;

    ulog->kdb_state = KDB_UNSTABLE;

    xdrmem_create(&xdrs, (char *)indx_log->entry_data,
                  indx_log->kdb_entry_size, XDR_ENCODE);
    if (!xdr_kdb_incr_update_t(&xdrs, upd))
        return (KRB5_LOG_CONV);

    if ((retval = ulog_sync_update(ulog, indx_log)))
        return (retval);

    ulog->kdb_last_sno = cur_sno;
    ulog->kdb_last_time = ktime;

    /* This is a circular array; when we wrap, we have to adjust first_sno */

    if (ulog->kdb_num < ulogentries)
        ulog->kdb_num++;
    else {
        i = cur_sno % ulogentries;
        indx_log = (kdb_ent_header_t *)INDEX(ulog, i);
        ulog->kdb_first_sno = indx_log->kdb_entry_sno;
        ulog->kdb_first_time = indx_log->kdb_time;
    }

    if (ulog->kdb_num == 1) {
        ulog->kdb_first_sno = cur_sno;
        ulog->kdb_first_time = ktime;
    }

    retval = ulog_finish_update(context, upd);
    return (retval);
}

/*
 * Mark the log entry as committed and sync the memory mapped log
 * to file.
 */
krb5_error_code
ulog_finish_update(krb5_context context, kdb_incr_update_t *upd)
{
    krb5_error_code     retval;
    kdb_ent_header_t    *indx_log;
    uint_t              i;
    kdb_log_context     *log_ctx;
    kdb_hlog_t          *ulog = NULL;
    uint32_t            ulogentries;

    INIT_ULOG(context);
    ulogentries = log_ctx->ulogentries;

    i = (upd->kdb_entry_sno - 1) % ulogentries;

    indx_log = (kdb_ent_header_t *)INDEX(ulog, i);

    indx_log->kdb_commit = TRUE;

    ulog->kdb_state = KDB_STABLE;

    if ((retval = ulog_sync_update(ulog, indx_log)))
        return (retval);

    ulog_sync_header(ulog);

    return (0);
}

/*
 * Delete an entry to the update log.
 */
krb5_error_code
ulog_delete_update(krb5_context context, kdb_incr_update_t *upd)
{

    upd->kdb_deleted = TRUE;

    return (ulog_add_update(context, upd));
}

/*
 * Used by the slave or master (during ulog_check) to update its hash db from
 * the incr update log.
 *
 * Must be called with lock held.
 */
krb5_error_code
ulog_replay(krb5_context context, kdb_incr_result_t *incr_ret, char **db_args)
{
    krb5_db_entry       *entry = NULL;
    kdb_incr_update_t   *upd = NULL, *fupd;
    int                 i, no_of_updates;
    krb5_error_code     retval;
    krb5_principal      dbprinc = NULL;
    kdb_last_t          errlast;
    char                *dbprincstr = NULL;
    kdb_log_context     *log_ctx;
    kdb_hlog_t          *ulog = NULL;

    INIT_ULOG(context);

    if (log_ctx && log_ctx->iproprole == IPROP_SLAVE) {
        if ((retval = ulog_lock(context, KRB5_LOCKMODE_EXCLUSIVE)))
            return (retval);
    }

    no_of_updates = incr_ret->updates.kdb_ulog_t_len;
    upd = incr_ret->updates.kdb_ulog_t_val;
    fupd = upd;

    if ((retval = krb5_db_open(context, db_args,
                               KRB5_KDB_OPEN_RW|KRB5_KDB_SRV_TYPE_ADMIN)))
        goto cleanup;

    for (i = 0; i < no_of_updates; i++) {
#if 0
        if (!upd->kdb_commit)
            continue;
#endif

        if (upd->kdb_deleted) {
            dbprincstr = malloc((upd->kdb_princ_name.utf8str_t_len
                                 + 1) * sizeof (char));

            if (dbprincstr == NULL) {
                retval = ENOMEM;
                goto cleanup;
            }

            (void) strncpy(dbprincstr,
                           (char *)upd->kdb_princ_name.utf8str_t_val,
                           (upd->kdb_princ_name.utf8str_t_len + 1));
            dbprincstr[upd->kdb_princ_name.utf8str_t_len] = 0;

            if ((retval = krb5_parse_name(context, dbprincstr,
                                          &dbprinc))) {
                goto cleanup;
            }

            free(dbprincstr);

            retval = krb5int_delete_principal_no_log(context, dbprinc);

            if (dbprinc) {
                krb5_free_principal(context, dbprinc);
                dbprinc = NULL;
            }

            if (retval)
                goto cleanup;
        } else {
            entry = (krb5_db_entry *)malloc(sizeof (krb5_db_entry));

            if (!entry) {
                retval = errno;
                goto cleanup;
            }

            (void) memset(entry, 0, sizeof (krb5_db_entry));

            if ((retval = ulog_conv_2dbentry(context, &entry, upd)))
                goto cleanup;

            retval = krb5int_put_principal_no_log(context, entry);

            if (entry) {
                krb5_db_free_principal(context, entry);
                entry = NULL;
            }
            if (retval)
                goto cleanup;
        }

        if (log_ctx && log_ctx->iproprole == IPROP_SLAVE &&
            upd->kdb_entry_sno >= 1 &&
            ulog->kdb_hmagic == KDB_ULOG_HDR_MAGIC &&
            ulog->kdb_state == KDB_STABLE) {

            uint32_t            ulogentries, indx, upd_size, recsize;
            int                 ulogfd;
            kdb_ent_header_t    *indx_log;
            XDR                 xdrs;

            ulogfd = log_ctx->ulogfd;

            ulogentries = log_ctx->ulogentries;
            indx = (upd->kdb_entry_sno - 1) % ulogentries;

            upd_size = xdr_sizeof((xdrproc_t)xdr_kdb_incr_update_t, upd);
            recsize = sizeof(kdb_ent_header_t) + upd_size;

            if (recsize > ulog->kdb_block) {
                if ((retval = ulog_resize(ulog, ulogentries, ulogfd, recsize)))
                    goto cleanup;
                ulog_sync_header(ulog);
            }

            indx_log = (kdb_ent_header_t *)INDEX(ulog, indx);
            (void) memset(indx_log, 0, ulog->kdb_block);

            indx_log->kdb_umagic = KDB_ULOG_MAGIC;
            indx_log->kdb_entry_size = upd_size;
            indx_log->kdb_entry_sno = upd->kdb_entry_sno;
            indx_log->kdb_time = upd->kdb_time;
            indx_log->kdb_commit = TRUE;

            xdrmem_create(&xdrs, (char *)indx_log->entry_data,
                          indx_log->kdb_entry_size, XDR_ENCODE);

            if (!xdr_kdb_incr_update_t(&xdrs, upd)) {
                retval = KRB5_LOG_CONV;
                goto cleanup;
            }

            if ((retval = ulog_sync_update(ulog, indx_log)))
                goto cleanup;

            if (ulog->kdb_num < 0 || ulog->kdb_num > ulogentries)
                (void) ulog_init_header(context, recsize);

            ulog->kdb_last_sno = upd->kdb_entry_sno;
            ulog->kdb_last_time = upd->kdb_time;

            /*
             * Allow the first entry to not reside in the ulog.
             * As the first update is received, populate the first entry
             * with the last serial info received from the full resync.
             */
            if (ulog->kdb_first_sno == 0 && ulog->kdb_num == 0) {
                ulog->kdb_first_sno = upd->kdb_entry_sno;
                ulog->kdb_first_time = upd->kdb_time;
            }

            if (ulog->kdb_num < ulogentries)
                ulog->kdb_num++;
            else {
                /*
                 * The ulog is full, so we should update the first_sno
                 * based on the first entry in the circular buffer.
                 */
                ulog->kdb_first_sno = ulog->kdb_last_sno - ulogentries + 1;
                indx = (ulog->kdb_first_sno - 1) % ulogentries;
                indx_log = (kdb_ent_header_t *)INDEX(ulog, indx);

                if (indx_log->kdb_umagic == KDB_ULOG_MAGIC &&
                    indx_log->kdb_entry_sno == ulog->kdb_first_sno) {
                    ulog->kdb_first_time = indx_log->kdb_time;
                } else {
                    /*
                     * The individual entry is corrupt; just reset the
                     * ulog to the last entry rather than disable the log.
                     * Hopefully, this won't ever occur.
                     */
                    ulog->kdb_first_sno = ulog->kdb_last_sno;
                    ulog->kdb_first_time = ulog->kdb_last_time;
                    ulog->kdb_num = 1;
                }
                (void) ulog_sync_header(ulog);
            }
        }
        
        upd++;
    }

cleanup:
    if (fupd)
        ulog_free_entries(fupd, no_of_updates);

    if (log_ctx && (log_ctx->iproprole == IPROP_SLAVE)) {
        (void) ulog_sync_header(ulog);
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
    }

    return (retval);
}

/*
 * Validate the log file and resync any uncommitted update entries
 * to the principal database.
 *
 * Must be called with lock held.
 */
static krb5_error_code
ulog_check(krb5_context context, kdb_hlog_t *ulog, char **db_args)
{
    XDR                 xdrs;
    krb5_error_code     retval = 0;
    unsigned int        i, j, ulogentries, start_sno;
    kdb_ent_header_t    *indx_log;
    kdb_incr_update_t   *upd = NULL;
    kdb_incr_result_t   *incr_ret = NULL;

    ulog->kdb_state = KDB_STABLE;

    ulogentries = context->kdblog_context->ulogentries;

    /*
     * Edge condition/optimization.
     * Reset ulog->kdb_first_* to equal the last entry if the log is empty.
     */
    if (ulog->kdb_num == 0 && ulog->kdb_first_sno == 0 &&
        ulog->kdb_last_sno > 0) {

        ulog->kdb_first_sno = ulog->kdb_last_sno;
        ulog->kdb_first_time = ulog->kdb_last_time;
    }

    /* On a slave, the circular buffer may not begin at index 0. */
    start_sno = ulog->kdb_last_sno - ulog->kdb_num + 1;
    
    for (i = 0; i < ulog->kdb_num; i++) {
        j = (start_sno + i - 1) % ulogentries;
        indx_log = (kdb_ent_header_t *)INDEX(ulog, j);

        if (indx_log->kdb_umagic != KDB_ULOG_MAGIC) {
            /*
             * Update entry corrupted we should scream and die
             */
            ulog->kdb_state = KDB_CORRUPT;
            retval = KRB5_LOG_CORRUPT;
            break;
        }

        if (indx_log->kdb_commit == FALSE) {
            ulog->kdb_state = KDB_UNSTABLE;

            incr_ret = (kdb_incr_result_t *)
                malloc(sizeof (kdb_incr_result_t));
            if (incr_ret == NULL) {
                retval = errno;
                goto error;
            }

            upd = (kdb_incr_update_t *)
                malloc(sizeof (kdb_incr_update_t));
            if (upd == NULL) {
                retval = errno;
                goto error;
            }

            (void) memset(upd, 0, sizeof (kdb_incr_update_t));
            xdrmem_create(&xdrs, (char *)indx_log->entry_data,
                          indx_log->kdb_entry_size, XDR_DECODE);
            if (!xdr_kdb_incr_update_t(&xdrs, upd)) {
                retval = KRB5_LOG_CONV;
                goto error;
            }

            incr_ret->updates.kdb_ulog_t_len = 1;
            incr_ret->updates.kdb_ulog_t_val = upd;

            upd->kdb_commit = TRUE;

            /*
             * We don't want to readd this update and just use the
             * existing update to be propagated later on
             */
            ulog_set_role(context, IPROP_NULL);
            retval = ulog_replay(context, incr_ret, db_args);

            /*
             * upd was freed by ulog_replay, we NULL
             * the pointer in case we subsequently break from loop.
             */
            upd = NULL;
            if (incr_ret) {
                free(incr_ret);
                incr_ret = NULL;
            }
            ulog_set_role(context, IPROP_MASTER);

            if (retval)
                goto error;

            /*
             * We flag this as committed since this was
             * the last entry before kadmind crashed, ergo
             * the slaves have not seen this update before
             */
            indx_log->kdb_commit = TRUE;
            retval = ulog_sync_update(ulog, indx_log);
            if (retval)
                goto error;

            ulog->kdb_state = KDB_STABLE;
        }
    }

error:
    if (upd)
        ulog_free_entries(upd, 1);

    free(incr_ret);

    ulog_sync_header(ulog);

    return (retval);
}

/*
 * Re-init the log header.
 */
krb5_error_code
ulog_init_header(krb5_context context, uint32_t recsize)
{
    kdb_log_context     *log_ctx;
    kdb_hlog_t          *ulog = NULL;
    krb5_error_code     retval;
    uint_t              block_min;
    
    
    INIT_ULOG(context);
    
    /* The caller should already have a lock, but ensure it is exclusive. */
    if ((retval = ulog_lock(context, KRB5_LOCKMODE_EXCLUSIVE)))
        return retval;
    ulog_reset(ulog);

    block_min = ULOG_BLOCK;  /* Should this be min(ULOG_BLOCK, pagesize)? */
    if (recsize > block_min)
        ulog->kdb_block = ((recsize + block_min - 1) & (~(block_min-1)));

    ulog_sync_header(ulog);

    /* The caller is responsible for unlocking... */
    return (0);
}

/*
 * Map the log file to memory for performance and simplicity.
 *
 * Called by: if iprop_enabled then ulog_map();
 * Assumes that the caller will terminate on ulog_map, hence munmap and
 * closing of the fd are implicitly performed by the caller.
 * Returns 0 on success else failure.
 */
krb5_error_code
ulog_map(krb5_context context, const char *logname, uint32_t ulogentries,
         int caller, char **db_args)
{
    struct stat st;
    krb5_error_code     retval;
    uint32_t    ulog_filesize;
    kdb_log_context     *log_ctx;
    kdb_hlog_t  *ulog = NULL;
    int         ulogfd = -1;

    ulog_filesize = sizeof (kdb_hlog_t);

    if (stat(logname, &st) == -1) {

        if (caller == FKPROPLOG) {
            /* File doesn't exist so we exit with kproplog */
            return (errno);
        }

        if ((ulogfd = open(logname, O_RDWR+O_CREAT, 0600)) == -1) {
            return (errno);
        }

        if (lseek(ulogfd, 0L, SEEK_CUR) == -1) {
            return (errno);
        }

        if ((caller == FKADMIND) || (caller == FKCOMMAND) || (caller == FKPROPD)) {
            if (ulogentries < 2)                /* Min log entries = 2 */
                return (EINVAL);
            
            ulog_filesize += ulogentries * ULOG_BLOCK;
        }

        if (extend_file_to(ulogfd, ulog_filesize) < 0)
            return errno;
    } else {

        ulogfd = open(logname, O_RDWR, 0600);
        if (ulogfd == -1)
            /*
             * Can't open existing log file
             */
            return errno;
    }

    if (caller == FKPROPLOG) {
        if (fstat(ulogfd, &st) < 0) {
            close(ulogfd);
            return errno;
        }
        ulog_filesize = st.st_size;

        ulog = (kdb_hlog_t *)mmap(0, ulog_filesize,
                                  PROT_READ+PROT_WRITE, MAP_PRIVATE, ulogfd, 0);
    } else {
        /*
         * else kadmind, kpropd, & kcommands should udpate stores
         */
        ulog = (kdb_hlog_t *)mmap(0, MAXLOGLEN,
                                  PROT_READ+PROT_WRITE, MAP_SHARED, ulogfd, 0);
    }

    if (ulog == MAP_FAILED) {
        /*
         * Can't map update log file to memory
         */
        close(ulogfd);
        return (errno);
    }

    if (!context->kdblog_context) {
        if (!(log_ctx = malloc(sizeof (kdb_log_context))))
            return (errno);
        memset(log_ctx, 0, sizeof(*log_ctx));
        context->kdblog_context = log_ctx;
    } else
        log_ctx = context->kdblog_context;
    log_ctx->ulog = ulog;
    log_ctx->ulogentries = ulogentries;
    log_ctx->ulogfd = ulogfd;

    if (ulog->kdb_hmagic != KDB_ULOG_HDR_MAGIC) {
        if (ulog->kdb_hmagic == 0) {
            /*
             * New update log
             */
            (void) memset(ulog, 0, sizeof (kdb_hlog_t));

            ulog->kdb_hmagic = KDB_ULOG_HDR_MAGIC;
            ulog->db_version_num = KDB_VERSION;
            ulog->kdb_state = KDB_STABLE;
            ulog->kdb_block = ULOG_BLOCK;
            if (!(caller == FKPROPLOG))
                ulog_sync_header(ulog);
        } else {
            return (KRB5_LOG_CORRUPT);
        }
    }

    if (caller == FKADMIND || caller == FKPROPD) {
        retval = ulog_lock(context, KRB5_LOCKMODE_EXCLUSIVE);
        if (retval)
            return retval;
        switch (ulog->kdb_state) {
        case KDB_STABLE:
        case KDB_UNSTABLE:
            /*
             * Log is currently un/stable, check anyway
             */
            retval = ulog_check(context, ulog, db_args);
            ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            if (retval == KRB5_LOG_CORRUPT) {
                return (retval);
            }
            break;
        case KDB_CORRUPT:
            ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            return (KRB5_LOG_CORRUPT);
        default:
            /*
             * Invalid db state
             */
            ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            return (KRB5_LOG_ERROR);
        }
    } else if (caller == FKPROPLOG) {
        /* kproplog doesn't need to do anything else */
        return (0);
    }

    if (ulog->kdb_num == 0 && ulog->kdb_first_sno == 0 && ulog->kdb_last_sno) {
        ulog->kdb_first_sno = ulog->kdb_last_sno;
        ulog->kdb_first_time = ulog->kdb_last_time;
    }

    /*
     * Reinit ulog if the log is being truncated or expanded after
     * we have circled.
     */
    retval = ulog_lock(context, KRB5_LOCKMODE_EXCLUSIVE);
    if (retval)
        return retval;
    if (ulog->kdb_num != ulogentries) {
        if (ulog->kdb_num > ulogentries ||
            ulog->kdb_first_sno < ulog->kdb_last_sno - ulog->kdb_num) {

            (void) memset(ulog, 0, sizeof (kdb_hlog_t));

            ulog->kdb_hmagic = KDB_ULOG_HDR_MAGIC;
            ulog->db_version_num = KDB_VERSION;
            ulog->kdb_state = KDB_STABLE;
            ulog->kdb_block = ULOG_BLOCK;

            ulog_sync_header(ulog);
        }

        /*
         * Expand ulog if we have specified a greater size
         */
        if (ulog->kdb_num < ulogentries) {
            ulog_filesize = sizeof (kdb_hlog_t);
            ulog_filesize += ulogentries * ulog->kdb_block;

            if (extend_file_to(ulogfd, ulog_filesize) < 0) {
                ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
                return errno;
            }
        }
    }
    ulog_lock(context, KRB5_LOCKMODE_UNLOCK);

    return (0);
}

/*
 * Get the last set of updates seen, (last+1) to n is returned.
 */
krb5_error_code
ulog_get_entries(krb5_context context,          /* input - krb5 lib config */
                 kdb_last_t last,               /* input - slave's last sno */
                 kdb_incr_result_t *ulog_handle) /* output - incr result for slave */
{
    XDR                 xdrs;
    kdb_ent_header_t    *indx_log;
    kdb_incr_update_t   *upd;
    uint_t              indx, count, tdiff;
    uint32_t            sno;
    krb5_error_code     retval;
    struct timeval      timestamp;
    kdb_log_context     *log_ctx;
    kdb_hlog_t          *ulog = NULL;
    uint32_t            ulogentries;

    INIT_ULOG(context);
    ulogentries = log_ctx->ulogentries;

    retval = ulog_lock(context, KRB5_LOCKMODE_SHARED | KRB5_LOCKMODE_DONTBLOCK);
    if (0
#ifdef EWOULDBLOCK
        || retval == EWOULDBLOCK
#endif
#ifdef EAGAIN
        || retval == EAGAIN
#endif
        ) {
        ulog_handle->ret = UPDATE_BUSY;
        return (0);
    }
    if (retval)
        return retval;

    /*
     * Check to make sure we don't have a corrupt ulog first.
     */
    if (ulog->kdb_state == KDB_CORRUPT) {
        ulog_handle->ret = UPDATE_ERROR;
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        return (KRB5_LOG_CORRUPT);
    }

    gettimeofday(&timestamp, NULL);

    tdiff = timestamp.tv_sec - ulog->kdb_last_time.seconds;
    if (tdiff <= ULOG_IDLE_TIME) {
        ulog_handle->ret = UPDATE_BUSY;
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        return (0);
    }

    /*
     * Defer a full resync when no log history is present to avoid looping.
     */
    if (ulog->kdb_num == 0 && ulog->kdb_last_sno == 0) {
        ulog_handle->ret = UPDATE_BUSY;
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        return (0);
    } else if (ulog->kdb_last_sno <= 0) {
        /*
         * This should never happen. last_sno should only be zero if there
         * are no log entries and should never be negative.
         */
        ulog_handle->ret = UPDATE_ERROR;
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        return (KRB5_LOG_CORRUPT);
    }

    /*
     * We need to lock out other processes here, such as kadmin.local,
     * since we are looking at the last_sno and looking up updates.  So
     * we can share with other readers.
     */
    retval = krb5_db_lock(context, KRB5_LOCKMODE_SHARED);
    if (retval) {
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        return (retval);
    }

    /*
     * We may have overflowed the update log or we shrunk the log, or
     * the client's ulog has just been created.
     */
    if ((last.last_sno > ulog->kdb_last_sno) ||
        (last.last_sno < ulog->kdb_first_sno) ||
        (last.last_sno == 0)) {
        ulog_handle->lastentry.last_sno = ulog->kdb_last_sno;
        ulog_handle->lastentry.last_time = ulog->kdb_last_time;
        
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        (void) krb5_db_unlock(context);
        ulog_handle->ret = UPDATE_FULL_RESYNC_NEEDED;
        return (0);
    }

    sno = last.last_sno;

    /*
     * Validate the client state, including timestamp, against the ulog.
     * Note: The first entry may not be in the ulog if the slave was
     * promoted to a master and only has a limited ulog history.
     */
    if (sno == ulog->kdb_last_sno) {
        if (last.last_time.seconds == ulog->kdb_last_time.seconds &&
            last.last_time.useconds == ulog->kdb_last_time.useconds) {

            (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            (void) krb5_db_unlock(context);
            ulog_handle->ret = UPDATE_NIL;
            return (0);
        } else {

            ulog_handle->lastentry.last_sno = ulog->kdb_last_sno;
            ulog_handle->lastentry.last_time = ulog->kdb_last_time;

            (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            (void) krb5_db_unlock(context);
            ulog_handle->ret = UPDATE_FULL_RESYNC_NEEDED;
            return (0);
        }
    }
    if (sno == ulog->kdb_first_sno) {
        if (last.last_time.seconds != ulog->kdb_first_time.seconds ||
            last.last_time.useconds != ulog->kdb_first_time.useconds) {

            ulog_handle->lastentry.last_sno = ulog->kdb_last_sno;
            ulog_handle->lastentry.last_time = ulog->kdb_last_time;

            (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            (void) krb5_db_unlock(context);
            ulog_handle->ret = UPDATE_FULL_RESYNC_NEEDED;
            return (0);
        }
    } else {
        indx = (sno - 1) % ulogentries;
        indx_log = (kdb_ent_header_t *)INDEX(ulog, indx);

        if (indx_log->kdb_time.seconds != last.last_time.seconds ||
            indx_log->kdb_time.useconds != last.last_time.useconds) {

            ulog_handle->lastentry.last_sno = ulog->kdb_last_sno;
            ulog_handle->lastentry.last_time = ulog->kdb_last_time;

            (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            (void) krb5_db_unlock(context);
            ulog_handle->ret = UPDATE_FULL_RESYNC_NEEDED;
            return (0);
        }
    }

    /*
     * We have now determined the client needs a list of incremental updates.
     */
    count = ulog->kdb_last_sno - sno;

    ulog_handle->updates.kdb_ulog_t_val =
        (kdb_incr_update_t *)malloc(sizeof (kdb_incr_update_t) * count);

    upd = ulog_handle->updates.kdb_ulog_t_val;

    if (upd == NULL) {
        (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
        (void) krb5_db_unlock(context);
        ulog_handle->ret = UPDATE_ERROR;
        return (errno);
    }

    while (sno < ulog->kdb_last_sno) {
        indx = sno % ulogentries;
        indx_log = (kdb_ent_header_t *)INDEX(ulog, indx);

        (void) memset(upd, 0, sizeof (kdb_incr_update_t));
        xdrmem_create(&xdrs,
                      (char *)indx_log->entry_data,
                      indx_log->kdb_entry_size, XDR_DECODE);
        if (!xdr_kdb_incr_update_t(&xdrs, upd)) {
            (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
            (void) krb5_db_unlock(context);

            ulog_free_entries(ulog_handle->updates.kdb_ulog_t_val,
                              sno - last.last_sno);
            ulog_handle->updates.kdb_ulog_t_val = NULL;
            
            ulog_handle->ret = UPDATE_ERROR;
            return (KRB5_LOG_CONV);
        }
        upd->kdb_commit = TRUE;
#if 0
        upd->kdb_commit = indx_log->kdb_commit;
#endif

        upd++;
        sno++;
    } /* while */

    ulog_handle->updates.kdb_ulog_t_len = count;

    ulog_handle->lastentry.last_sno = ulog->kdb_last_sno;
    ulog_handle->lastentry.last_time = ulog->kdb_last_time;
    ulog_handle->ret = UPDATE_OK;

    (void) ulog_lock(context, KRB5_LOCKMODE_UNLOCK);
    (void) krb5_db_unlock(context);

    return (0);
}

krb5_error_code
ulog_set_role(krb5_context ctx, iprop_role role)
{
    kdb_log_context     *log_ctx;

    if (!ctx->kdblog_context) {
        if (!(log_ctx = malloc(sizeof (kdb_log_context))))
            return (errno);
        memset(log_ctx, 0, sizeof(*log_ctx));
        ctx->kdblog_context = log_ctx;
    } else
        log_ctx = ctx->kdblog_context;

    log_ctx->iproprole = role;

    return (0);
}

/*
 * Extend update log file.
 */
static int extend_file_to(int fd, uint_t new_size)
{
    off_t current_offset;
    static const char zero[512] = { 0, };

    current_offset = lseek(fd, 0, SEEK_END);
    if (current_offset < 0)
        return -1;
    if (new_size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    while (current_offset < (off_t)new_size) {
        int write_size, wrote_size;
        write_size = new_size - current_offset;
        if (write_size > 512)
            write_size = 512;
        wrote_size = write(fd, zero, write_size);
        if (wrote_size < 0)
            return -1;
        if (wrote_size == 0) {
            errno = EINVAL;     /* XXX ?? */
            return -1;
        }
        current_offset += wrote_size;
        write_size = new_size - current_offset;
    }
    return 0;
}
