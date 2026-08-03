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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "src/include/constants.h"

#include <stdlib.h>

#include "src/mca/plm/plm_types.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

pmix_status_t prte_pmix_convert_rc(int rc)
{
    switch (rc) {

    case PRTE_ERR_HEARTBEAT_ALERT:
    case PRTE_ERR_FILE_ALERT:
    case PRTE_ERR_HEARTBEAT_LOST:
    case PRTE_ERR_SENSOR_LIMIT_EXCEEDED:
        return PMIX_ERR_JOB_SENSOR_BOUND_EXCEEDED;

    case PRTE_ERR_NO_EXE_SPECIFIED:
    case PRTE_ERR_NO_APP_SPECIFIED:
        return PMIX_ERR_JOB_NO_EXE_SPECIFIED;

    case PRTE_ERR_SLOT_LIST_RANGE:
    case PRTE_ERR_TOPO_SOCKET_NOT_SUPPORTED:
    case PRTE_ERR_INVALID_PHYS_CPU:
    case PRTE_ERR_TOPO_CORE_NOT_SUPPORTED:
    case PRTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED:
    case PRTE_ERR_MULTIPLE_AFFINITIES:
    case PRTE_ERR_FAILED_TO_MAP:
        return PMIX_ERR_JOB_FAILED_TO_MAP;

    case PRTE_ERR_JOB_CANCELLED:
        return PMIX_ERR_JOB_CANCELED;

    case PRTE_ERR_DEBUGGER_RELEASE:
        return PMIX_ERR_DEBUGGER_RELEASE;

    case PRTE_ERR_HANDLERS_COMPLETE:
        return PMIX_EVENT_ACTION_COMPLETE;

    case PRTE_ERR_PROC_ABORTED:
        return PMIX_ERR_PROC_ABORTED;

    case PRTE_ERR_PROC_REQUESTED_ABORT:
        return PMIX_ERR_PROC_REQUESTED_ABORT;

    case PRTE_ERR_PROC_ABORTING:
        return PMIX_ERR_PROC_ABORTING;

    case PRTE_ERR_NODE_DOWN:
        return PMIX_ERR_NODE_DOWN;

    case PRTE_ERR_NODE_OFFLINE:
        return PMIX_ERR_NODE_OFFLINE;

    case PRTE_ERR_JOB_TERMINATED:
        return PMIX_EVENT_JOB_END;

    case PRTE_ERR_PROC_RESTART:
        return PMIX_ERR_PROC_RESTART;

    case PRTE_ERR_PROC_CHECKPOINT:
        return PMIX_ERR_PROC_CHECKPOINT;

    case PRTE_ERR_PROC_MIGRATE:
        return PMIX_ERR_PROC_MIGRATE;

    case PRTE_ERR_EVENT_REGISTRATION:
        return PMIX_ERR_EVENT_REGISTRATION;

    case PRTE_ERR_NOT_IMPLEMENTED:
    case PRTE_ERR_NOT_SUPPORTED:
        return PMIX_ERR_NOT_SUPPORTED;

    case PRTE_ERR_NOT_FOUND:
        return PMIX_ERR_NOT_FOUND;

    /* keep "you may not" distinct from "I could not get there". They used to
     * share PMIX_ERR_UNREACH, so a job refused for want of permission - the
     * ownership check on an allocation is the live case - reached the user as
     * a connectivity failure, which is neither true nor actionable. */
    case PRTE_ERR_PERM:
        return PMIX_ERR_NO_PERMISSIONS;

    case PRTE_ERR_UNREACH:
    case PRTE_ERR_SERVER_NOT_AVAIL:
        return PMIX_ERR_UNREACH;

    case PRTE_ERR_BAD_PARAM:
        return PMIX_ERR_BAD_PARAM;

    case PRTE_ERR_SYS_LIMITS_PIPES:
    case PRTE_ERR_SYS_LIMITS_CHILDREN:
    case PRTE_ERR_SOCKET_NOT_AVAILABLE:
    case PRTE_ERR_NOT_ENOUGH_CORES:
    case PRTE_ERR_NOT_ENOUGH_SOCKETS:
        return PMIX_ERR_JOB_INSUFFICIENT_RESOURCES;

    case PRTE_ERR_PIPE_READ_FAILURE:
        return PMIX_ERR_JOB_SYS_OP_FAILED;

    case PRTE_ERR_OUT_OF_RESOURCE:
        return PMIX_ERR_OUT_OF_RESOURCE;

    case PRTE_ERR_RESOURCE_BUSY:
        return PMIX_ERR_RESOURCE_BUSY;

    case PRTE_ERR_NOT_AVAILABLE:
        return PMIX_ERR_NOT_AVAILABLE;

    case PRTE_ERR_COMM_FAILURE:
        return PMIX_ERR_COMM_FAILURE;

    /* PRTE_ERR_SILENT means "this failure has already been reported to
     * the user, do not report it again". Dropping it on the floor here
     * turned every silent abort into a second, redundant error at the
     * tool. */
    case PRTE_ERR_SILENT:
        return PMIX_ERR_SILENT;

    /* the pack/unpack family - see the note on prte_pmix_convert_status() */
    case PRTE_ERR_PACK_FAILURE:
        return PMIX_ERR_PACK_FAILURE;

    case PRTE_ERR_UNPACK_FAILURE:
        return PMIX_ERR_UNPACK_FAILURE;

    case PRTE_ERR_UNPACK_INADEQUATE_SPACE:
        return PMIX_ERR_UNPACK_INADEQUATE_SPACE;

    case PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER:
        return PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER;

    case PRTE_ERR_TYPE_MISMATCH:
        return PMIX_ERR_TYPE_MISMATCH;

    case PRTE_ERR_UNKNOWN_DATA_TYPE:
        return PMIX_ERR_UNKNOWN_DATA_TYPE;

    case PRTE_ERR_DATA_VALUE_NOT_FOUND:
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;

    case PRTE_ERR_WDIR_NOT_FOUND:
        return PMIX_ERR_JOB_WDIR_NOT_FOUND;

    case PRTE_ERR_EXE_NOT_FOUND:
    case PRTE_ERR_EXE_NOT_ACCESSIBLE:
        return PMIX_ERR_JOB_EXE_NOT_FOUND;

    case PRTE_ERR_TIMEOUT:
        return PMIX_ERR_TIMEOUT;

    case PRTE_ERR_WOULD_BLOCK:
        return PMIX_ERR_WOULD_BLOCK;

    case PRTE_EXISTS:
        return PMIX_EXISTS;

    case PRTE_ERR_PARTIAL_SUCCESS:
        return PMIX_QUERY_PARTIAL_SUCCESS;

    case PRTE_ERR_MODEL_DECLARED:
        return PMIX_MODEL_DECLARED;

    case PRTE_ERR_OP_IN_PROGRESS:
        return PMIX_OPERATION_IN_PROGRESS;

    case PRTE_ERROR:
        return PMIX_ERROR;
    case PRTE_SUCCESS:
        return PMIX_SUCCESS;

    default:
        return PMIX_ERROR;
    }
}

/* The two code spaces overlap numerically: a PRRTE error is
 * PRTE_ERR_BASE - n for n in [1,72] and a PMIx status runs from -1 down
 * past -160, so most PMIx statuses are *also* the value of some real
 * PRRTE constant. Falling through with the input unconverted therefore
 * does not produce a recognisably foreign code, it produces a
 * confidently wrong one - PMIX_ERR_PACK_FAILURE arrived as
 * PRTE_ERR_FILE_OPEN_FAILURE, PMIX_ERR_EMPTY as PRTE_ERR_NODE_DOWN.
 * Every case must land on a PRRTE constant, and the default must too. */
int prte_pmix_convert_status(pmix_status_t status)
{
    switch (status) {
    case PMIX_ERR_DEBUGGER_RELEASE:
        return PRTE_ERR_DEBUGGER_RELEASE;

    case PMIX_EVENT_ACTION_COMPLETE:
        return PRTE_ERR_HANDLERS_COMPLETE;

    case PMIX_ERR_PROC_ABORTED:
        return PRTE_ERR_PROC_ABORTED;

    case PMIX_ERR_PROC_REQUESTED_ABORT:
        return PRTE_ERR_PROC_REQUESTED_ABORT;

    case PMIX_ERR_PROC_ABORTING:
        return PRTE_ERR_PROC_ABORTING;

    case PMIX_ERR_NODE_DOWN:
        return PRTE_ERR_NODE_DOWN;

    case PMIX_ERR_NODE_OFFLINE:
        return PRTE_ERR_NODE_OFFLINE;

    case PMIX_EVENT_JOB_END:
        return PRTE_ERR_JOB_TERMINATED;

    case PMIX_ERR_PROC_RESTART:
        return PRTE_ERR_PROC_RESTART;

    case PMIX_ERR_PROC_CHECKPOINT:
        return PRTE_ERR_PROC_CHECKPOINT;

    case PMIX_ERR_PROC_MIGRATE:
        return PRTE_ERR_PROC_MIGRATE;

    case PMIX_ERR_EVENT_REGISTRATION:
        return PRTE_ERR_EVENT_REGISTRATION;

    case PMIX_ERR_NOT_SUPPORTED:
        return PRTE_ERR_NOT_SUPPORTED;

    case PMIX_ERR_NOT_FOUND:
        return PRTE_ERR_NOT_FOUND;

    case PMIX_ERR_OUT_OF_RESOURCE:
        return PRTE_ERR_OUT_OF_RESOURCE;

    case PMIX_ERR_INIT:
        return PRTE_ERROR;

    case PMIX_ERR_BAD_PARAM:
        return PRTE_ERR_BAD_PARAM;

    case PMIX_ERR_UNREACH:
        return PRTE_ERR_UNREACH;

    case PMIX_ERR_NO_PERMISSIONS:
        return PRTE_ERR_PERM;

    case PMIX_ERR_TIMEOUT:
        return PRTE_ERR_TIMEOUT;

    case PMIX_ERR_WOULD_BLOCK:
        return PRTE_ERR_WOULD_BLOCK;

    case PMIX_ERR_LOST_CONNECTION:
        return PRTE_ERR_COMM_FAILURE;

    case PMIX_EXISTS:
        return PRTE_EXISTS;

    case PMIX_QUERY_PARTIAL_SUCCESS:
        return PRTE_ERR_PARTIAL_SUCCESS;

    case PMIX_MONITOR_HEARTBEAT_ALERT:
        return PRTE_ERR_HEARTBEAT_ALERT;

    case PMIX_MONITOR_FILE_ALERT:
        return PRTE_ERR_FILE_ALERT;

    case PMIX_MODEL_DECLARED:
        return PRTE_ERR_MODEL_DECLARED;

    /* the pack/unpack family. These are far and away the most common
     * thing this function is handed - PRRTE routes essentially every
     * bfrops return through here - and every one of them used to fall
     * through to the raw-status default and land on an unrelated
     * file-I/O code. */
    case PMIX_ERR_PACK_FAILURE:
        return PRTE_ERR_PACK_FAILURE;
    case PMIX_ERR_UNPACK_FAILURE:
        return PRTE_ERR_UNPACK_FAILURE;
    case PMIX_ERR_UNPACK_INADEQUATE_SPACE:
        return PRTE_ERR_UNPACK_INADEQUATE_SPACE;
    case PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER:
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    case PMIX_ERR_TYPE_MISMATCH:
        return PRTE_ERR_TYPE_MISMATCH;
    case PMIX_ERR_UNKNOWN_DATA_TYPE:
        return PRTE_ERR_UNKNOWN_DATA_TYPE;

    case PMIX_ERR_NOMEM:
        return PRTE_ERR_OUT_OF_RESOURCE;
    case PMIX_ERR_RESOURCE_BUSY:
        return PRTE_ERR_RESOURCE_BUSY;
    case PMIX_ERR_NOT_AVAILABLE:
        return PRTE_ERR_NOT_AVAILABLE;
    case PMIX_ERR_COMM_FAILURE:
        return PRTE_ERR_COMM_FAILURE;
    case PMIX_ERR_DATA_VALUE_NOT_FOUND:
        return PRTE_ERR_DATA_VALUE_NOT_FOUND;
    case PMIX_ERR_EXISTS_OUTSIDE_SCOPE:
        return PRTE_EXISTS;
    case PMIX_ERR_JOB_APP_NOT_EXECUTABLE:
        return PRTE_ERR_EXE_NOT_ACCESSIBLE;
    case PMIX_ERR_JOB_NO_EXE_SPECIFIED:
        return PRTE_ERR_NO_EXE_SPECIFIED;
    case PMIX_ERR_JOB_FAILED_TO_MAP:
        return PRTE_ERR_FAILED_TO_MAP;
    case PMIX_ERR_JOB_CANCELED:
        return PRTE_ERR_JOB_CANCELLED;

    case PMIX_ERROR:
        return PRTE_ERROR;
    case PMIX_ERR_SILENT:
        return PRTE_ERR_SILENT;
    case PMIX_SUCCESS:
    case PMIX_OPERATION_SUCCEEDED:
        return PRTE_SUCCESS;

    default:
        /* an unrecognized PMIx status is an error we have no PRRTE name
         * for. It is NOT a PRRTE error code, so do not hand it back as
         * though it were. */
        return PRTE_ERROR;
    }
}

/* Never spell these cases as bare integers. The two state spaces share
 * a numbering scheme but not a numbering: PRRTE has no PREPPED, so its
 * pre-termination states sit one below PMIx's all the way up to
 * CONNECTED, while from PRTE_PROC_STATE_ERROR/PMIX_PROC_STATE_ERROR on
 * up the offsets do line up. Writing "case 59" rather than the name is
 * how PRTE_PROC_STATE_HEARTBEAT_FAILED came to report itself as
 * PMIX_PROC_STATE_MIGRATING while PRTE_PROC_STATE_MIGRATING fell
 * through to UNDEF. */
pmix_proc_state_t prte_pmix_convert_state(int state)
{
    switch (state) {
    case PRTE_PROC_STATE_UNDEF:
        return PMIX_PROC_STATE_UNDEF;
    case PRTE_PROC_STATE_INIT:
        return PMIX_PROC_STATE_LAUNCH_UNDERWAY;
    case PRTE_PROC_STATE_RESTART:
        return PMIX_PROC_STATE_RESTART;
    case PRTE_PROC_STATE_TERMINATE:
        return PMIX_PROC_STATE_TERMINATE;
    case PRTE_PROC_STATE_RUNNING:
        return PMIX_PROC_STATE_RUNNING;
    case PRTE_PROC_STATE_REGISTERED:
        return PMIX_PROC_STATE_CONNECTED;

    /* PRRTE tracks a few more milestones on the way to reaping a proc
     * than PMIx names. All of them sit below the UNTERMINATED boundary,
     * so the proc is still alive as far as a querying tool is
     * concerned - saying UNDEF would be a lie. */
    case PRTE_PROC_STATE_IOF_COMPLETE:
    case PRTE_PROC_STATE_WAITPID_FIRED:
    case PRTE_PROC_STATE_MODEX_READY:
    case PRTE_PROC_STATE_READY_FOR_DEBUG:
        return PMIX_PROC_STATE_RUNNING;

    case PRTE_PROC_STATE_UNTERMINATED:
        return PMIX_PROC_STATE_UNTERMINATED;
    case PRTE_PROC_STATE_TERMINATED:
        return PMIX_PROC_STATE_TERMINATED;

    case PRTE_PROC_STATE_KILLED_BY_CMD:
        return PMIX_PROC_STATE_KILLED_BY_CMD;
    case PRTE_PROC_STATE_ABORTED:
        return PMIX_PROC_STATE_ABORTED;
    case PRTE_PROC_STATE_FAILED_TO_START:
        return PMIX_PROC_STATE_FAILED_TO_START;
    case PRTE_PROC_STATE_ABORTED_BY_SIG:
        return PMIX_PROC_STATE_ABORTED_BY_SIG;
    case PRTE_PROC_STATE_TERM_WO_SYNC:
        return PMIX_PROC_STATE_TERM_WO_SYNC;
    case PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED:
        return PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED;
    case PRTE_PROC_STATE_CALLED_ABORT:
        return PMIX_PROC_STATE_CALLED_ABORT;
    case PRTE_PROC_STATE_HEARTBEAT_FAILED:
        return PMIX_PROC_STATE_HEARTBEAT_FAILED;
    case PRTE_PROC_STATE_MIGRATING:
        return PMIX_PROC_STATE_MIGRATING;
    case PRTE_PROC_STATE_CANNOT_RESTART:
        return PMIX_PROC_STATE_CANNOT_RESTART;
    case PRTE_PROC_STATE_TERM_NON_ZERO:
        return PMIX_PROC_STATE_TERM_NON_ZERO;
    case PRTE_PROC_STATE_FAILED_TO_LAUNCH:
        return PMIX_PROC_STATE_FAILED_TO_LAUNCH;

    /* every way PRRTE describes losing touch with a peer. PMIx has
     * the one bucket - the same collapse prte_pmix_convert_proc_state_to_error()
     * makes onto PMIX_ERR_COMM_FAILURE. */
    case PRTE_PROC_STATE_COMM_FAILED:
    case PRTE_PROC_STATE_UNABLE_TO_SEND_MSG:
    case PRTE_PROC_STATE_LIFELINE_LOST:
    case PRTE_PROC_STATE_NO_PATH_TO_TARGET:
    case PRTE_PROC_STATE_FAILED_TO_CONNECT:
    case PRTE_PROC_STATE_PEER_UNKNOWN:
        return PMIX_PROC_STATE_COMM_FAILED;

    default:
        return PMIX_PROC_STATE_UNDEF;
    }
}

int prte_pmix_convert_pstate(pmix_proc_state_t state)
{
    switch (state) {
    case PMIX_PROC_STATE_UNDEF:
        return PRTE_PROC_STATE_UNDEF;
    case PMIX_PROC_STATE_PREPPED:
    case PMIX_PROC_STATE_LAUNCH_UNDERWAY:
        return PRTE_PROC_STATE_INIT;
    case PMIX_PROC_STATE_RESTART:
        return PRTE_PROC_STATE_RESTART;
    case PMIX_PROC_STATE_TERMINATE:
        return PRTE_PROC_STATE_TERMINATE;
    case PMIX_PROC_STATE_RUNNING:
        return PRTE_PROC_STATE_RUNNING;
    case PMIX_PROC_STATE_CONNECTED:
        return PRTE_PROC_STATE_REGISTERED;
    case PMIX_PROC_STATE_UNTERMINATED:
        return PRTE_PROC_STATE_UNTERMINATED;
    case PMIX_PROC_STATE_TERMINATED:
        return PRTE_PROC_STATE_TERMINATED;
    case PMIX_PROC_STATE_KILLED_BY_CMD:
        return PRTE_PROC_STATE_KILLED_BY_CMD;
    case PMIX_PROC_STATE_ABORTED:
        return PRTE_PROC_STATE_ABORTED;
    case PMIX_PROC_STATE_FAILED_TO_START:
        return PRTE_PROC_STATE_FAILED_TO_START;
    case PMIX_PROC_STATE_ABORTED_BY_SIG:
        return PRTE_PROC_STATE_ABORTED_BY_SIG;
    case PMIX_PROC_STATE_TERM_WO_SYNC:
        return PRTE_PROC_STATE_TERM_WO_SYNC;
    case PMIX_PROC_STATE_COMM_FAILED:
        return PRTE_PROC_STATE_COMM_FAILED;
    case PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED:
        return PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED;
    case PMIX_PROC_STATE_CALLED_ABORT:
        return PRTE_PROC_STATE_CALLED_ABORT;
    case PMIX_PROC_STATE_HEARTBEAT_FAILED:
        return PRTE_PROC_STATE_HEARTBEAT_FAILED;
    case PMIX_PROC_STATE_MIGRATING:
        return PRTE_PROC_STATE_MIGRATING;
    case PMIX_PROC_STATE_CANNOT_RESTART:
        return PRTE_PROC_STATE_CANNOT_RESTART;
    case PMIX_PROC_STATE_TERM_NON_ZERO:
        return PRTE_PROC_STATE_TERM_NON_ZERO;
    case PMIX_PROC_STATE_FAILED_TO_LAUNCH:
        return PRTE_PROC_STATE_FAILED_TO_LAUNCH;
    default:
        return PRTE_PROC_STATE_UNDEF;
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
        /* pre-positioning happens before mapping, so a job that fails
         * there never launched either - without this it fell to the
         * default and the requesting tool was told only "PMIX_ERROR" */
        case PRTE_JOB_STATE_FILES_POSN_FAILED:
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

        case PRTE_JOB_STATE_NON_ZERO_TERM:
            return PMIX_ERR_EXIT_NONZERO_TERM;

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

/* handler side of prte_pmix_shifted_wakeup - executes on the
 * PRRTE progress thread */
static void shifted_wakeup(int fd, short args, void *cbdata)
{
    prte_pmix_wakeup_caddy_t *cd = (prte_pmix_wakeup_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(cd);
    cd->lock->status = cd->status;
    if (NULL != cd->msg) {
        /* transfer ownership to the lock - the caddy destructor
         * must not free it. A lock reused across operations may still
         * be holding the previous message. */
        if (NULL != cd->lock->msg) {
            free(cd->lock->msg);
        }
        cd->lock->msg = cd->msg;
        cd->msg = NULL;
    }
    PRTE_PMIX_WAKEUP_THREAD(cd->lock);
    PMIX_RELEASE(cd);
}

void prte_pmix_shifted_wakeup(prte_pmix_lock_t *lock,
                              pmix_status_t status,
                              char *msg)
{
    prte_pmix_wakeup_caddy_t *cd;

    cd = PMIX_NEW(prte_pmix_wakeup_caddy_t);
    cd->lock = lock;
    cd->status = status;
    cd->msg = msg;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, shifted_wakeup, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
}

/* CLASS INSTANTIATIONS */
static void acon(prte_pmix_app_t *p)
{
    PMIX_APP_CONSTRUCT(&p->app);
    PMIX_INFO_LIST_START(p->info);
    p->mapby = NULL;
    p->rankby = NULL;
    p->bindto = NULL;
    p->output = NULL;
    p->display = NULL;
    p->rtos = NULL;
}
static void ades(prte_pmix_app_t *p)
{
    PMIX_APP_DESTRUCT(&p->app);
    PMIX_INFO_LIST_RELEASE(p->info);
    free(p->mapby);
    free(p->rankby);
    free(p->bindto);
    free(p->output);
    free(p->display);
    free(p->rtos);
}
PMIX_CLASS_INSTANCE(prte_pmix_app_t, pmix_list_item_t, acon, ades);

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

static void wkcon(prte_pmix_wakeup_caddy_t *p)
{
    p->lock = NULL;
    p->status = PMIX_SUCCESS;
    p->msg = NULL;
}
static void wkdes(prte_pmix_wakeup_caddy_t *p)
{
    if (NULL != p->msg) {
        free(p->msg);
    }
}
PRTE_EXPORT PMIX_CLASS_INSTANCE(prte_pmix_wakeup_caddy_t, pmix_object_t, wkcon, wkdes);
