/*
 * Copyright (c) 2009-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2004-2023 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/threads/pmix_threads.h"
#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/ess/ess.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/plm_types.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/errmgr.h"

#include "errmgr_prted.h"

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);
static void prted_abort(int error_code, char *fmt, ...);

/******************
 * prted module
 ******************/
prte_errmgr_base_module_t prte_errmgr_prted_module = {
    .init = init,
    .finalize = finalize,
    .logfn = prte_errmgr_base_log
};

/* Local functions */
static void failed_start(prte_job_t *jobdat);
static void killprocs(pmix_nspace_t job, pmix_rank_t vpid);

static void job_errors(int fd, short args, void *cbdata);
static void proc_errors(int fd, short args, void *cbdata);

/************************
 * API Definitions
 ************************/
static int init(void)
{
    /* setup state machine to trap job errors */
    prte_state.add_job_state(PRTE_JOB_STATE_ERROR, job_errors);

    /* set the lost connection state to run at MSG priority so
     * we can process any last messages from the proc
     */
    prte_state.add_proc_state(PRTE_PROC_STATE_COMM_FAILED, proc_errors);

    /* setup state machine to trap proc errors */
    prte_state.add_proc_state(PRTE_PROC_STATE_ERROR, proc_errors);

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    return PRTE_SUCCESS;
}

static void wakeup(int sd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(sd, args, cbdata);
    /* nothing more we can do */
    PMIX_ACQUIRE_OBJECT(cbdata);
    prte_quit(0, 0, NULL);
}

/* this function only gets called when FORCED_TERMINATE
 * has been invoked, which means that there is some
 * internal failure (e.g., to pack/unpack a correct value).
 * We could just exit, but that doesn't result in any
 * meaningful error message to the user. Likewise, just
 * printing something to stdout/stderr won't necessarily
 * get back to the user. Instead, we will send an error
 * report to mpirun and give it a chance to order our
 * termination. In order to ensure we _do_ terminate,
 * we set a timer - if it fires before we receive the
 * termination command, then we will exit on our own. This
 * protects us in the case that the failure is in the
 * messaging system itself */
static void prted_abort(int error_code, char *fmt, ...)
{
    va_list arglist;
    char *outmsg = NULL;
    prte_plm_cmd_flag_t cmd;
    pmix_data_buffer_t *alert;
    pmix_rank_t null = PMIX_RANK_INVALID;
    prte_proc_state_t state = PRTE_PROC_STATE_CALLED_ABORT;
    prte_timer_t *timer;
    int rc;

    /* only do this once */
    if (prte_abnormal_term_ordered) {
        return;
    }

    /* set the aborting flag */
    prte_abnormal_term_ordered = true;

    /* If there was a message, construct it */
    va_start(arglist, fmt);
    if (NULL != fmt) {
        pmix_vasprintf(&outmsg, fmt, arglist);
    }
    va_end(arglist);

    /* use the show-help system to get the message out.  Callers are not
     * required to supply a message, and the help topic has a %s in it, so
     * substitute something printable rather than handing show_help a NULL */
    prte_show_help("help-errmgr-base.txt", "simple-message", true,
                   NULL == outmsg ? "(no further information available)" : outmsg);
    if (NULL != outmsg) {
        free(outmsg);
        outmsg = NULL;
    }

    /* tell the HNP we are in distress */
    PMIX_DATA_BUFFER_CREATE(alert);

    /* pack update state command */
    cmd = PRTE_PLM_UPDATE_PROC_STATE;
    rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, alert, &PRTE_PROC_MY_NAME->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack our vpid */
    rc = PMIx_Data_pack(NULL, alert, &PRTE_PROC_MY_NAME->rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack our pid */
    rc = PMIx_Data_pack(NULL, alert, &prte_process_info.pid, 1, PMIX_PID);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack our state */
    rc = PMIx_Data_pack(NULL, alert, &state, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack our exit code */
    rc = PMIx_Data_pack(NULL, alert, &error_code, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* flag that this job is complete so the receiver can know */
    rc = PMIx_Data_pack(NULL, alert, &null, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }

    /* send it */
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        /* we can't communicate, so give up */
        prte_quit(0, 0, NULL);
        return;
    }

cleanup:
    /* set a timer for exiting - this also gives the message a chance
     * to get out! */
    if (NULL == (timer = PMIX_NEW(prte_timer_t))) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return;
    }
    timer->tv.tv_sec = 5;
    timer->tv.tv_usec = 0;
    prte_event_evtimer_set(prte_event_base, timer->ev, wakeup, NULL);
    PMIX_POST_OBJECT(timer);
    prte_event_evtimer_add(timer->ev, &timer->tv);
}

static void job_errors(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_job_state_t jobstate;
    int rc;
    prte_plm_cmd_flag_t cmd;
    pmix_data_buffer_t *alert;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /*
     * if prte is trying to shutdown, just let it
     */
    if (prte_finalizing) {
        PMIX_RELEASE(caddy);
        return;
    }

    /* if the jdata is NULL, then it is referencing the daemon job */
    if (NULL == caddy->jdata) {
        caddy->jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        if (NULL == caddy->jdata) {
            /* we are too far gone to have a daemon job - there is nothing
             * this handler can do, and every line below dereferences it */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PMIX_RELEASE(caddy);
            return;
        }
        PMIX_RETAIN(caddy->jdata);
    }

    /* update the state */
    jdata = caddy->jdata;
    jobstate = caddy->job_state;
    jdata->state = jobstate;

    PMIX_OUTPUT_VERBOSE((1, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:prted: job %s reported error state %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                         prte_job_state_to_str(jobstate)));

    switch (jobstate) {
    case PRTE_JOB_STATE_FAILED_TO_START:
        failed_start(jdata);
        break;
    case PRTE_JOB_STATE_COMM_FAILED:
        /* kill all local procs */
        killprocs(NULL, PMIX_RANK_WILDCARD);
        /* order termination */
        prted_abort(PRTE_ERROR_DEFAULT_EXIT_CODE, "Daemon %s: comm failure",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        goto cleanup;
    case PRTE_JOB_STATE_HEARTBEAT_FAILED:
        /* let the HNP handle this */
        goto cleanup;

    default:
        break;
    }
    PMIX_DATA_BUFFER_CREATE(alert);
    /* pack update state command */
    cmd = PRTE_PLM_UPDATE_PROC_STATE;
    rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* pack the job info */
    if (PMIX_SUCCESS != (rc = prte_plm_base_pack_state_update(alert, jdata, false))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
        goto cleanup;
    }
    /* send it */
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(alert);
    }

cleanup:
    PMIX_RELEASE(caddy);
}

static void proc_errors(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    pmix_proc_t *proc;
    prte_proc_state_t state;
    prte_proc_t *child;
    pmix_data_buffer_t *alert;
    prte_plm_cmd_flag_t cmd;
    int rc = PRTE_SUCCESS;
    prte_wait_tracker_t *t2;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* nothing in the caddy may be read before this: it is the barrier
     * pairing with the PMIX_POST_OBJECT on whichever thread activated the
     * state, and on a weakly-ordered machine a read hoisted above it can
     * see the field as the constructor left it rather than as the activator
     * set it */
    PMIX_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:prted:proc_errors process %s error state %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                         prte_proc_state_to_str(state)));

    /*
     * if prte is trying to shutdown, just let it
     */
    if (prte_finalizing) {
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors finalizing - ignoring error",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    /* if this is a heartbeat failure, let the HNP handle it */
    if (PRTE_PROC_STATE_HEARTBEAT_FAILED == state) {
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors heartbeat failed - ignoring error",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    /* A send failure is reported against the peer we could not reach, which is
     * very often *not* our lifeline: the RML raises UNABLE_TO_SEND_MSG /
     * NO_PATH_TO_TARGET / PEER_UNKNOWN against the next hop of whatever message
     * failed, and that hop may be one of our routed children.  Taking this
     * daemon down for that turns one unreachable peer into the loss of our
     * whole subtree - every local proc killed and every daemon below us
     * orphaned - which is precisely the failure the HNP-side handler was
     * already taught to ignore (see the "not yet reported for duty" comment in
     * errmgr/dvm).  The same thing happens here: a daemon relaying an xcast to
     * a peer that has been recorded but has not finished coming up, the common
     * case during an elastic grow, has no address to send to yet.
     *
     * So only a genuinely lost lifeline ends this daemon.  For the rest:
     *
     *  - UNABLE_TO_SEND_MSG / NO_PATH_TO_TARGET / PEER_UNKNOWN against another
     *    daemon are transient by nature - the peer may simply not be listening
     *    yet - so we ignore them.  A real departure arrives separately as a
     *    dropped connection, which the OOB already turns into a route repair
     *    plus COMM_FAILED.
     *  - FAILED_TO_CONNECT means the OOB has *given up* on that peer
     *    (connect_max_time / max_recon_attempts exhausted), so there will be no
     *    later notice: heal the routing tree around it here, exactly as the
     *    lost-connection path does.  prte_rml_route_lost() returns an error
     *    only for the HNP itself, in which case we do exit.
     *  - Anything against a peer outside the daemon job (a tool, an app proc)
     *    is never a reason for a daemon to die. */
    if (PRTE_PROC_STATE_UNABLE_TO_SEND_MSG == state
        || PRTE_PROC_STATE_NO_PATH_TO_TARGET == state
        || PRTE_PROC_STATE_PEER_UNKNOWN == state
        || PRTE_PROC_STATE_FAILED_TO_CONNECT == state) {
        if (!PMIX_CHECK_NSPACE(proc->nspace, PRTE_PROC_MY_NAME->nspace)) {
            PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted unable to communicate with non-daemon %s "
                                 "- ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
            goto cleanup;
        }
        if (PRTE_PROC_STATE_FAILED_TO_CONNECT == state) {
            if (PRTE_SUCCESS == prte_rml_route_lost(proc->rank)) {
                PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                     "%s errmgr:prted gave up connecting to daemon %s "
                                     "- routed around it",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
                goto cleanup;
            }
        } else if (PRTE_PROC_MY_HNP->rank != proc->rank) {
            PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted unable to send to daemon %s "
                                 "- it may not be up yet, ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
            goto cleanup;
        }
    }

    /* our lifeline is gone - we cannot continue */
    if (PRTE_PROC_STATE_LIFELINE_LOST == state || PRTE_PROC_STATE_UNABLE_TO_SEND_MSG == state
        || PRTE_PROC_STATE_NO_PATH_TO_TARGET == state || PRTE_PROC_STATE_PEER_UNKNOWN == state
        || PRTE_PROC_STATE_FAILED_TO_CONNECT == state) {
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted lifeline lost or unable to communicate - exiting",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* set our exit status */
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
        /* kill our children */
        killprocs(NULL, PMIX_RANK_WILDCARD);
        /* terminate - our routed children will see
         * us leave and automatically die
         */
        prte_quit(0, 0, NULL);
        goto cleanup;
    }

    /* get the job object */
    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        /* must already be complete */
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors NULL jdata - ignoring error",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    if (PRTE_PROC_STATE_COMM_FAILED == state) {
        /* if it is our own connection, ignore it */
        if (PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_PROC_MY_NAME, proc)) {
            PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted:proc_errors comm_failed to self - ignoring error",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* was it a daemon? */
        if (!PMIX_CHECK_NSPACE(proc->nspace, PRTE_PROC_MY_NAME->nspace)) {
            /* nope - we can't seem to trust that we will catch the waitpid
             * in this situation, so push this over to be handled as if
             * it were a waitpid trigger so we don't create a bunch of
             * duplicate code */
            PMIX_OUTPUT_VERBOSE(
                (2, prte_errmgr_base_framework.framework_output,
                 "%s errmgr:prted:proc_errors comm_failed to non-daemon - handling as waitpid",
                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* get the proc_t */
            if (NULL
                == (child = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs,
                                                                        proc->rank))) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                goto cleanup;
            }
            /* leave the exit code alone - process this as a waitpid */
            t2 = PMIX_NEW(prte_wait_tracker_t);
            PMIX_RETAIN(child); // protect against race conditions
            t2->child = child;
            prte_event_set(prte_event_base, &t2->ev, -1, PRTE_EV_WRITE,
                           prte_odls_base_default_wait_local_proc, t2);
            prte_event_active(&t2->ev, PRTE_EV_WRITE, 1);
            goto cleanup;
        }
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:default:prted daemon %s exited",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));

        if (prte_prteds_term_ordered) {
            /* are any of my children still alive */
            if (prte_errmgr_base_any_live_children(NULL)) {
                goto cleanup;
            }
            /* if all my routes and children are gone, then terminate
               ourselves nicely (i.e., this is a normal termination) */
            if (0 == prte_rml_base.n_children) {
                PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted all routes gone - exiting",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            } else {
                PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted not exiting, num_routes() == %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     prte_rml_base.n_children));
            }
        }
        /* if not, then we can continue */
        goto cleanup;
    }

    if (NULL == (child = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, proc->rank))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        goto cleanup;
    }

    if (PRTE_PROC_STATE_CALLED_ABORT == state) {
        /* update the state */
        child->state = state;
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED, NULL, PMIX_BOOL)) {
            PMIX_DATA_BUFFER_CREATE(alert);
            /* pack update state command */
            cmd = PRTE_PLM_UPDATE_PROC_STATE;
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack only the data for this proc - have to start with the jobid
             * so the receiver can unpack it correctly
             */
            rc = PMIx_Data_pack(NULL, alert, &proc->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }

            /* now pack the child's info */
            if (PMIX_SUCCESS != (rc = prte_plm_base_pack_state_for_proc(alert, child))) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* send it */
            PMIX_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted reporting proc %s called abort with "
                                 "non-zero status (local procs = %d)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&child->name),
                                 jdata->num_local_procs));
            PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
            }
            /* mark that we notified the HNP for this job so we don't do it again;
             * recoverable jobs need to receive every notifications, though. */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL)) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED, PRTE_ATTR_LOCAL, NULL,
                                   PMIX_BOOL);
            }
        }
        goto cleanup;
    }

    /* if this is not a local proc for this job, we can
     * ignore this call
     */
    if (!PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_LOCAL)) {
        PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors proc is not local - ignoring error",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    PMIX_OUTPUT_VERBOSE(
        (2, prte_errmgr_base_framework.framework_output, "%s errmgr:prted got state %s for proc %s",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_proc_state_to_str(state), PRTE_NAME_PRINT(proc)));

    if (PRTE_PROC_STATE_TERM_NON_ZERO == state) {
        /* update the state */
        child->state = state;
        /* report this as abnormal termination to the HNP, unless we already have
         * done so for this job */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED, NULL, PMIX_BOOL)) {
            PMIX_DATA_BUFFER_CREATE(alert);
            /* pack update state command */
            cmd = PRTE_PLM_UPDATE_PROC_STATE;
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack only the data for this proc - have to start with the jobid
             * so the receiver can unpack it correctly
             */
            rc = PMIx_Data_pack(NULL, alert, &proc->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }

            /* now pack the child's info */
            if (PMIX_SUCCESS != (rc = prte_plm_base_pack_state_for_proc(alert, child))) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* send it */
            PMIX_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted reporting proc %s abnormally terminated with "
                                 "non-zero status (local procs = %d)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&child->name),
                                 jdata->num_local_procs));
            PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
            }
            /* mark that we notified the HNP for this job so we don't do it again;
             * recoverable jobs need to receive every notifications, though. */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL)) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED, PRTE_ATTR_LOCAL, NULL,
                                   PMIX_BOOL);
            }
        }
        /* if the proc has terminated, notify the state machine */
        if (PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_IOF_COMPLETE)
            && PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_WAITPID)
            && !PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_RECORDED)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
        goto cleanup;
    }

    if (PRTE_PROC_STATE_FAILED_TO_START == state || PRTE_PROC_STATE_FAILED_TO_LAUNCH == state) {
        /* update the proc state */
        child->state = state;
        /* Count the proc as having "terminated" - and record that we have
         * counted it.  Without the flag it gets counted twice: failed_start()
         * drives each of these procs on to PRTE_PROC_STATE_TERMINATED, and
         * state/prted's track_procs() counts any proc that is not yet
         * RECORDED.  num_terminated then reaches 2 x num_local_procs, its
         * "num_terminated == num_local_procs" test never fires, and the
         * daemon-local completion work behind that test never runs for a job
         * that failed to start: the children stay in prte_local_children, the
         * IOF is never told the job is over, and the PMIx server is never told
         * to release the nspace.  (The HNP still hears about the failure -
         * job_errors sends its own consolidated report - which is why this
         * only ever showed up as a leak.)  The two other places this handler
         * counts a termination take the same guard. */
        if (!PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_RECORDED)) {
            PRTE_FLAG_SET(child, PRTE_PROC_FLAG_RECORDED);
            jdata->num_terminated++;
        }
        /* leave the error report in this case to the
         * state machine, which will receive notice
         * when all local procs have attempted to start
         * so that we send a consolidated error report
         * back to the HNP
         */
        if (jdata->num_local_procs == jdata->num_terminated) {
            /* let the state machine know */
            if (PRTE_PROC_STATE_FAILED_TO_START == state) {
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START);
            } else {
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            }
        }
        goto cleanup;
    }

    if (PRTE_PROC_STATE_TERMINATED < state) {
        /* if we were ordered to terminate, see if
         * any of our routes or local children remain alive - if not, then
         * terminate ourselves. */
        if (prte_prteds_term_ordered) {
            /* mark the child as no longer alive and update the counters, if necessary.
             * we have to do this here as we aren't going to send this to the state
             * machine, and we want to keep the bookkeeping accurate just in case */
            if (PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {
                PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_ALIVE);
            }
            if (!PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_RECORDED)) {
                PRTE_FLAG_SET(child, PRTE_PROC_FLAG_RECORDED);
                jdata->num_terminated++;
            }
            /* NOTE: ask the base rather than scanning prte_local_children
             * inline here.  An inline scan wants a loop variable, and the
             * obvious one - "child" - is the proc whose error we are handling
             * and the one the keep_going path below reports to the HNP.
             * Clobbering it reported some *other*, still-running local proc as
             * having abnormally terminated, and overwrote its state. */
            if (prte_errmgr_base_any_live_children(NULL)) {
                goto keep_going;
            }
            /* if all my routes and children are gone, then terminate
               ourselves nicely (i.e., this is a normal termination) */
            if (0 == prte_rml_base.n_children) {
                PMIX_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted all routes gone - exiting",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
            /* no need to alert the HNP - we are already on our way out */
            goto cleanup;
        }

    keep_going:
        /* if the job hasn't completed and the state is abnormally
         * terminated, then we need to alert the HNP right away - but
         * only do this once!
         */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED, NULL, PMIX_BOOL)) {
            PMIX_DATA_BUFFER_CREATE(alert);
            /* pack update state command */
            cmd = PRTE_PLM_UPDATE_PROC_STATE;
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack only the data for this proc - have to start with the jobid
             * so the receiver can unpack it correctly
             */
            rc = PMIx_Data_pack(NULL, alert, &proc->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            child->state = state;
            /* now pack the child's info */
            if (PMIX_SUCCESS != (rc = prte_plm_base_pack_state_for_proc(alert, child))) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            pmix_output_verbose(5, prte_errmgr_base_framework.framework_output,
                                "%s errmgr:prted reporting proc %s aborted to HNP (local procs = %d)",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&child->name),
                                jdata->num_local_procs);
            /* send it */
            PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
            }
            /* mark that we reported termination of this proc so we
             * don't do it again */
            PRTE_FLAG_SET(child, PRTE_PROC_FLAG_TERM_REPORTED);
            /* mark that we notified the HNP for this job so we don't do it again;
             * recoverable jobs need to receive every notifications, though. */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL)) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FAIL_NOTIFIED,
                                   PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
            }
        }
        /* if the proc has terminated, notify the state machine */
        if (PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_IOF_COMPLETE) &&
            PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_WAITPID) &&
            !PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_RECORDED)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
        goto cleanup;
    }

    /* Nothing else can arrive here.  This handler is registered for
     * PRTE_PROC_STATE_COMM_FAILED and PRTE_PROC_STATE_ERROR, and the state
     * machine only falls back on the ERROR registration for states ABOVE
     * PRTE_PROC_STATE_ERROR (50) - all of which are above
     * PRTE_PROC_STATE_TERMINATED (20) and so were taken by the branch just
     * above.  A plain TERMINATED is handled by state/prted's track_procs,
     * which is where the "job is complete on this node" bookkeeping lives:
     * the PRTE_JOB_TERM_NOTIFIED dedup, the local-children cleanup, the IOF
     * completion, and the PMIx notification.  This function used to carry a
     * partial copy of that - one with no dedup, no IOF, and a bare
     * PMIX_RELEASE(jdata) - which had been unreachable for as long as the
     * ERROR states have been numbered above TERMINATED. */

cleanup:
    PMIX_RELEASE(caddy);
}

/*****************
 * Local Functions
 *****************/
static void failed_start(prte_job_t *jobdat)
{
    int i;
    prte_proc_t *child;

    /* set the state */
    jobdat->state = PRTE_JOB_STATE_FAILED_TO_START;

    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL == (child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        /* is this child part of the specified job? */
        if (PMIX_CHECK_NSPACE(child->name.nspace, jobdat->nspace)) {
            if (PRTE_PROC_STATE_FAILED_TO_START == child->state) {
                /* this proc never launched - flag that the iof
                 * is complete or else we will hang waiting for
                 * pipes to close that were never opened
                 */
                PRTE_FLAG_SET(child, PRTE_PROC_FLAG_IOF_COMPLETE);
                /* ditto for waitpid */
                PRTE_FLAG_SET(child, PRTE_PROC_FLAG_WAITPID);
                PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_TERMINATED);
            }
        }
    }
    PMIX_OUTPUT_VERBOSE((1, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:hnp: job %s reported incomplete start",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jobdat->nspace)));
    return;
}

static void killprocs(pmix_nspace_t job, pmix_rank_t vpid)
{
    pmix_pointer_array_t cmd;
    prte_proc_t proc;
    int rc;

    if (PMIX_NSPACE_INVALID(job) && PMIX_RANK_WILDCARD == vpid) {
        if (PRTE_SUCCESS != (rc = prte_odls.kill_local_procs(NULL))) {
            PRTE_ERROR_LOG(rc);
        }
        return;
    }

    PMIX_CONSTRUCT(&cmd, pmix_pointer_array_t);
    PMIX_CONSTRUCT(&proc, prte_proc_t);
    PMIX_LOAD_PROCID(&proc.name, job, vpid);
    pmix_pointer_array_add(&cmd, &proc);
    if (PRTE_SUCCESS != (rc = prte_odls.kill_local_procs(&cmd))) {
        PRTE_ERROR_LOG(rc);
    }
    PMIX_DESTRUCT(&cmd);
    PMIX_DESTRUCT(&proc);
}
