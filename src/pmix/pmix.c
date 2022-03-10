/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "src/include/constants.h"

#include <regex.h>

#include <string.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/class/pmix_hash_table.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/proc_info.h"

// Map additional prrte specific error
// codes to PMIx. This mapping isn't perfect,
// but in most cases it'll drop to the default
// and return the mapped PMIx error code.
pmix_status_t prte_pmix_convert_rc(int rc)
{
    switch (rc) {

    case PRTE_ERR_NOT_IMPLEMENTED:
    case PRTE_ERR_NOT_SUPPORTED:
        return PMIX_ERR_NOT_SUPPORTED;

    case PRTE_ERR_SYS_LIMITS_PIPES:
    case PRTE_ERR_SYS_LIMITS_CHILDREN:
    case PRTE_ERR_SYS_LIMITS_SOCKETS:
    case PRTE_ERR_SOCKET_NOT_AVAILABLE:
        return PMIX_ERR_OUT_OF_RESOURCE;

    case PRTE_ERR_NOT_AVAILABLE:
    case PRTE_ERR_CONNECTION_REFUSED:
        return PMIX_ERR_UNREACH;

    case PRTE_ERR_NOT_INITIALIZED:
    case PRTE_ERR_VALUE_OUT_OF_BOUNDS:
    case PRTE_ERR_ADDRESSEE_UNKNOWN:
    case PRTE_ERR_PIPE_SETUP_FAILURE:
        return PMIX_ERR_BAD_PARAM;

    case PRTE_ERR_ALLOCATION_PENDING:
        return PMIX_OPERATION_IN_PROGRESS;

    case PRTE_ERR_NO_PATH_TO_TARGET:
        return PMIX_ERR_COMM_FAILURE;

    case PRTE_ERR_FILE_OPEN_FAILURE:
    case PRTE_ERR_FILE_WRITE_FAILURE:
    case PRTE_ERR_FILE_READ_FAILURE:
       return PMIX_ERR_IOF_FAILURE;

    case PRTE_ERR_TAKE_NEXT_OPTION:
        return PMIX_ERR_SILENT; // No good mapping for this.

    case PRTE_ERR_FATAL:
        return PMIX_ERROR;

    default:
        // Mode PRRTE status codes map to PMIx codes,
        // so if that is the case there is no conversion needed.
        return rc;
    }
}

// All PMIx codes should be mapped to PRRTE,
// so very little conversion is needed.
int prte_pmix_convert_status(pmix_status_t status)
{
    switch (status) {

    case PMIX_OPERATION_SUCCEEDED:
        return PRTE_SUCCESS;

    default:
        return status;
    }
}

pmix_proc_state_t prte_pmix_convert_state(int state)
{
    switch (state) {
    case 0:
        return PMIX_PROC_STATE_UNDEF;
    case 1:
        return PMIX_PROC_STATE_LAUNCH_UNDERWAY;
    case 2:
        return PMIX_PROC_STATE_RESTART;
    case 3:
        return PMIX_PROC_STATE_TERMINATE;
    case 4:
        return PMIX_PROC_STATE_RUNNING;
    case 5:
        return PMIX_PROC_STATE_CONNECTED;
    case 51:
        return PMIX_PROC_STATE_KILLED_BY_CMD;
    case 52:
        return PMIX_PROC_STATE_ABORTED;
    case 53:
        return PMIX_PROC_STATE_FAILED_TO_START;
    case 54:
        return PMIX_PROC_STATE_ABORTED_BY_SIG;
    case 55:
        return PMIX_PROC_STATE_TERM_WO_SYNC;
    case 56:
        return PMIX_PROC_STATE_COMM_FAILED;
    case 58:
        return PMIX_PROC_STATE_CALLED_ABORT;
    case 59:
        return PMIX_PROC_STATE_MIGRATING;
    case 61:
        return PMIX_PROC_STATE_CANNOT_RESTART;
    case 62:
        return PMIX_PROC_STATE_TERM_NON_ZERO;
    case 63:
        return PMIX_PROC_STATE_FAILED_TO_LAUNCH;
    default:
        return PMIX_PROC_STATE_UNDEF;
    }
}

int prte_pmix_convert_pstate(pmix_proc_state_t state)
{
    switch (state) {
    case PMIX_PROC_STATE_UNDEF:
        return 0;
    case PMIX_PROC_STATE_PREPPED:
    case PMIX_PROC_STATE_LAUNCH_UNDERWAY:
        return 1;
    case PMIX_PROC_STATE_RESTART:
        return 2;
    case PMIX_PROC_STATE_TERMINATE:
        return 3;
    case PMIX_PROC_STATE_RUNNING:
        return 4;
    case PMIX_PROC_STATE_CONNECTED:
        return 5;
    case PMIX_PROC_STATE_UNTERMINATED:
        return 15;
    case PMIX_PROC_STATE_TERMINATED:
        return 20;
    case PMIX_PROC_STATE_KILLED_BY_CMD:
        return 51;
    case PMIX_PROC_STATE_ABORTED:
        return 52;
    case PMIX_PROC_STATE_FAILED_TO_START:
        return 53;
    case PMIX_PROC_STATE_ABORTED_BY_SIG:
        return 54;
    case PMIX_PROC_STATE_TERM_WO_SYNC:
        return 55;
    case PMIX_PROC_STATE_COMM_FAILED:
        return 56;
    case PMIX_PROC_STATE_CALLED_ABORT:
        return 58;
    case PMIX_PROC_STATE_MIGRATING:
        return 60;
    case PMIX_PROC_STATE_CANNOT_RESTART:
        return 61;
    case PMIX_PROC_STATE_TERM_NON_ZERO:
        return 62;
    case PMIX_PROC_STATE_FAILED_TO_LAUNCH:
        return 63;
    default:
        return 0; // undef
    }
}

pmix_status_t prte_pmix_convert_job_state_to_error(int state)
{
    switch (state) {
        case PRTE_JOB_STATE_ALLOC_FAILED:
            return PMIX_ERR_JOB_ALLOC_FAILED;

        case PRTE_JOB_STATE_MAP_FAILED:
            return PMIX_ERR_JOB_FAILED_TO_MAP;

        case PRTE_JOB_STATE_NEVER_LAUNCHED:
        case PRTE_JOB_STATE_FAILED_TO_LAUNCH:
        case PRTE_JOB_STATE_FAILED_TO_START:
        case PRTE_JOB_STATE_CANNOT_LAUNCH:
            return PMIX_ERR_JOB_FAILED_TO_LAUNCH;

        case PRTE_JOB_STATE_KILLED_BY_CMD:
            return PMIX_ERR_JOB_CANCELED;

        case PRTE_JOB_STATE_ABORTED:
        case PRTE_JOB_STATE_CALLED_ABORT:
        case PRTE_JOB_STATE_SILENT_ABORT:
            return PMIX_ERR_JOB_ABORTED;

        case PRTE_JOB_STATE_ABORTED_BY_SIG:
            return PMIX_ERR_JOB_ABORTED_BY_SIG;

        case PRTE_JOB_STATE_ABORTED_WO_SYNC:
            return PMIX_ERR_JOB_TERM_WO_SYNC;

        case PRTE_JOB_STATE_TERMINATED:
            return PMIX_EVENT_JOB_END;

        default:
            return PMIX_ERROR;
    }
}

pmix_status_t prte_pmix_convert_proc_state_to_error(int state)
{
    switch (state) {
        case PRTE_PROC_STATE_KILLED_BY_CMD:
            return PMIX_ERR_JOB_CANCELED;

        case PRTE_PROC_STATE_ABORTED:
        case PRTE_PROC_STATE_CALLED_ABORT:
            return PMIX_ERR_JOB_ABORTED;

        case PRTE_PROC_STATE_ABORTED_BY_SIG:
            return PMIX_ERR_JOB_ABORTED_BY_SIG;

        case PRTE_PROC_STATE_FAILED_TO_LAUNCH:
        case PRTE_PROC_STATE_FAILED_TO_START:
            return PMIX_ERR_JOB_FAILED_TO_LAUNCH;

        case PRTE_PROC_STATE_TERM_WO_SYNC:
            return PMIX_ERR_JOB_TERM_WO_SYNC;

        case PRTE_PROC_STATE_COMM_FAILED:
        case PRTE_PROC_STATE_UNABLE_TO_SEND_MSG:
        case PRTE_PROC_STATE_LIFELINE_LOST:
        case PRTE_PROC_STATE_NO_PATH_TO_TARGET:
        case PRTE_PROC_STATE_FAILED_TO_CONNECT:
        case PRTE_PROC_STATE_PEER_UNKNOWN:
            return PMIX_ERR_COMM_FAILURE;

        case PRTE_PROC_STATE_CANNOT_RESTART:
            return PMIX_ERR_PROC_RESTART;

        case PRTE_PROC_STATE_TERM_NON_ZERO:
            return PMIX_ERR_JOB_NON_ZERO_TERM;

        case PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED:
            return PMIX_ERR_JOB_SENSOR_BOUND_EXCEEDED;

        default:
            return PMIX_ERROR;
    }
}

static void cleanup_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                           pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t *) cbdata;

    PMIX_POST_OBJECT(lk);

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    lk->status = status;
    PRTE_PMIX_WAKEUP_THREAD(lk);
}

int prte_pmix_register_cleanup(char *path, bool directory, bool ignore, bool jobscope)
{
    prte_pmix_lock_t lk;
    pmix_info_t pinfo[3];
    size_t n, ninfo = 0;
    pmix_status_t rc, ret;

    PRTE_PMIX_CONSTRUCT_LOCK(&lk);

    if (ignore) {
        /* they want this path ignored */
        PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_CLEANUP_IGNORE, path, PMIX_STRING);
        ++ninfo;
    } else {
        if (directory) {
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_REGISTER_CLEANUP_DIR, path, PMIX_STRING);
            ++ninfo;
            /* recursively cleanup directories */
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_CLEANUP_RECURSIVE, NULL, PMIX_BOOL);
            ++ninfo;
        } else {
            /* order cleanup of the provided path */
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_REGISTER_CLEANUP, path, PMIX_STRING);
            ++ninfo;
        }
    }

    /* if they want this applied to the job, then indicate so */
    if (jobscope) {
        rc = PMIx_Job_control_nb(NULL, 0, pinfo, ninfo, cleanup_cbfunc, (void *) &lk);
    } else {
        rc = PMIx_Job_control_nb(PRTE_PROC_MY_NAME, 1, pinfo, ninfo, cleanup_cbfunc, (void *) &lk);
    }
    if (PMIX_SUCCESS != rc) {
        ret = rc;
    } else {
        PRTE_PMIX_WAIT_THREAD(&lk);
        ret = lk.status;
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lk);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&pinfo[n]);
    }
    return ret;
}

/* CLASS INSTANTIATIONS */
static void acon(prte_pmix_app_t *p)
{
    PMIX_APP_CONSTRUCT(&p->app);
    PMIX_INFO_LIST_START(p->info);
}
static void ades(prte_pmix_app_t *p)
{
    PMIX_APP_DESTRUCT(&p->app);
    PMIX_INFO_LIST_RELEASE(p->info);
}
PMIX_CLASS_INSTANCE(prte_pmix_app_t, pmix_list_item_t, acon, ades);

static void dsicon(prte_ds_info_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->source);
    p->info = NULL;
    p->persistence = PMIX_PERSIST_INVALID;
}
PRTE_EXPORT PMIX_CLASS_INSTANCE(prte_ds_info_t, pmix_list_item_t, dsicon, NULL);

static void infoitmcon(prte_info_item_t *p)
{
    PMIX_INFO_CONSTRUCT(&p->info);
}
static void infoitdecon(prte_info_item_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info);
}
PRTE_EXPORT PMIX_CLASS_INSTANCE(prte_info_item_t, pmix_list_item_t, infoitmcon, infoitdecon);

static void arritmcon(prte_info_array_item_t *p)
{
    PMIX_CONSTRUCT(&p->infolist, pmix_list_t);
}
static void arritdecon(prte_info_array_item_t *p)
{
    PMIX_LIST_DESTRUCT(&p->infolist);
}
PRTE_EXPORT PMIX_CLASS_INSTANCE(prte_info_array_item_t, pmix_list_item_t, arritmcon, arritdecon);

static void pvcon(prte_value_t *p)
{
    PMIX_VALUE_CONSTRUCT(&p->value);
}
static void pvdes(prte_value_t *p)
{
    PMIX_VALUE_DESTRUCT(&p->value);
}
PRTE_EXPORT PMIX_CLASS_INSTANCE(prte_value_t, pmix_list_item_t, pvcon, pvdes);
