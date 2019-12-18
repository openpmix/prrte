/*
 * Copyright (c) 2009-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/dss/dss.h"

#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/plm/plm_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"

#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

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
prrte_errmgr_base_module_t prrte_errmgr_prted_module = {
    .init = init,
    .finalize = finalize,
    .logfn = prrte_errmgr_base_log,
    .abort = prted_abort,
    .abort_peers = prrte_errmgr_base_abort_peers
};

/* Local functions */
static bool any_live_children(prrte_jobid_t job);
static int pack_state_update(prrte_buffer_t *alert, prrte_job_t *jobdat);
static int pack_state_for_proc(prrte_buffer_t *alert, prrte_proc_t *child);
static void failed_start(prrte_job_t *jobdat);
static void killprocs(prrte_jobid_t job, prrte_vpid_t vpid);

static void job_errors(int fd, short args, void *cbdata);
static void proc_errors(int fd, short args, void *cbdata);

/************************
 * API Definitions
 ************************/
static int init(void)
{
    /* setup state machine to trap job errors */
    prrte_state.add_job_state(PRRTE_JOB_STATE_ERROR, job_errors, PRRTE_ERROR_PRI);

    /* set the lost connection state to run at MSG priority so
     * we can process any last messages from the proc
     */
    prrte_state.add_proc_state(PRRTE_PROC_STATE_COMM_FAILED, proc_errors, PRRTE_MSG_PRI);

    /* setup state machine to trap proc errors */
    prrte_state.add_proc_state(PRRTE_PROC_STATE_ERROR, proc_errors, PRRTE_ERROR_PRI);

    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    return PRRTE_SUCCESS;
}

static void wakeup(int sd, short args, void *cbdata)
{
    /* nothing more we can do */
    PRRTE_ACQUIRE_OBJECT(cbdata);
    prrte_quit(0, 0, NULL);
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
    prrte_plm_cmd_flag_t cmd;
    prrte_buffer_t *alert;
    prrte_vpid_t null=PRRTE_VPID_INVALID;
    prrte_proc_state_t state = PRRTE_PROC_STATE_CALLED_ABORT;
    prrte_timer_t *timer;
    int rc;

    /* only do this once */
    if (prrte_abnormal_term_ordered) {
        return;
    }

    /* set the aborting flag */
    prrte_abnormal_term_ordered = true;

    /* If there was a message, construct it */
    va_start(arglist, fmt);
    if (NULL != fmt) {
        prrte_vasprintf(&outmsg, fmt, arglist);
    }
    va_end(arglist);

    /* use the show-help system to get the message out */
    prrte_show_help("help-errmgr-base.txt", "simple-message", true, outmsg);

    /* tell the HNP we are in distress */
    alert = PRRTE_NEW(prrte_buffer_t);
    /* pack update state command */
    cmd = PRRTE_PLM_UPDATE_PROC_STATE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &PRRTE_PROC_MY_NAME->jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack our vpid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &PRRTE_PROC_MY_NAME->vpid, 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack our pid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &prrte_process_info.pid, 1, PRRTE_PID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack our state */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &state, 1, PRRTE_PROC_STATE))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack our exit code */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &error_code, 1, PRRTE_EXIT_CODE))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* flag that this job is complete so the receiver can know */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &null, 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }

    /* send it */
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                          PRRTE_RML_TAG_PLM,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        /* we can't communicate, so give up */
        prrte_quit(0, 0, NULL);
        return;
    }

  cleanup:
    /* set a timer for exiting - this also gives the message a chance
     * to get out! */
    if (NULL == (timer = PRRTE_NEW(prrte_timer_t))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return;
    }
    timer->tv.tv_sec = 5;
    timer->tv.tv_usec = 0;
    prrte_event_evtimer_set(prrte_event_base, timer->ev, wakeup, NULL);
    prrte_event_set_priority(timer->ev, PRRTE_ERROR_PRI);
    PRRTE_POST_OBJECT(timer);
    prrte_event_evtimer_add(timer->ev, &timer->tv);

}

static void job_errors(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_job_state_t jobstate;
    int rc;
    prrte_plm_cmd_flag_t cmd;
    prrte_buffer_t *alert;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /*
     * if prrte is trying to shutdown, just let it
     */
    if (prrte_finalizing) {
        return;
    }

    /* if the jdata is NULL, then we abort as this
     * is reporting an unrecoverable error
     */
    if (NULL == caddy->jdata) {
        PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_FORCED_EXIT);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* update the state */
    jdata = caddy->jdata;
    jobstate = caddy->job_state;
    jdata->state = jobstate;

    PRRTE_OUTPUT_VERBOSE((1, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:prted: job %s repprted error state %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid),
                         prrte_job_state_to_str(jobstate)));

    switch (jobstate) {
    case PRRTE_JOB_STATE_FAILED_TO_START:
        failed_start(jdata);
        break;
    case PRRTE_JOB_STATE_COMM_FAILED:
        /* kill all local procs */
        killprocs(PRRTE_JOBID_WILDCARD, PRRTE_VPID_WILDCARD);
        /* order termination */
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        goto cleanup;
        break;
    case PRRTE_JOB_STATE_HEARTBEAT_FAILED:
        /* let the HNP handle this */
        goto cleanup;
        break;

    default:
        break;
    }
    alert = PRRTE_NEW(prrte_buffer_t);
    /* pack update state command */
    cmd = PRRTE_PLM_UPDATE_PROC_STATE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* pack the job info */
    if (PRRTE_SUCCESS != (rc = pack_state_update(alert, jdata))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
        goto cleanup;
    }
    /* send it */
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                          PRRTE_RML_TAG_PLM,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(alert);
    }

 cleanup:
    PRRTE_RELEASE(caddy);
}

static void proc_errors(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_process_name_t *proc = &caddy->name;
    prrte_proc_state_t state = caddy->proc_state;
    prrte_proc_t *child, *ptr;
    prrte_buffer_t *alert;
    prrte_plm_cmd_flag_t cmd;
    int rc=PRRTE_SUCCESS;
    int i;
    prrte_wait_tracker_t *t2;

    PRRTE_ACQUIRE_OBJECT(caddy);

    PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:prted:proc_errors process %s error state %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc),
                         prrte_proc_state_to_str(state)));

    /*
     * if prrte is trying to shutdown, just let it
     */
    if (prrte_finalizing) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors finalizing - ignoring error",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    /* if this is a heartbeat failure, let the HNP handle it */
    if (PRRTE_PROC_STATE_HEARTBEAT_FAILED == state) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors heartbeat failed - ignoring error",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    /* if this was a failed comm, then see if it was to our
     * lifeline
     */
    if (PRRTE_PROC_STATE_LIFELINE_LOST == state ||
        PRRTE_PROC_STATE_UNABLE_TO_SEND_MSG == state ||
        PRRTE_PROC_STATE_NO_PATH_TO_TARGET == state ||
        PRRTE_PROC_STATE_PEER_UNKNOWN == state ||
        PRRTE_PROC_STATE_FAILED_TO_CONNECT == state) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted lifeline lost or unable to communicate - exiting",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        /* set our exit status */
        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        /* kill our children */
        killprocs(PRRTE_JOBID_WILDCARD, PRRTE_VPID_WILDCARD);
        /* terminate - our routed children will see
         * us leave and automatically die
         */
        prrte_quit(0, 0, NULL);
        goto cleanup;
    }

    /* get the job object */
    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        /* must already be complete */
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors NULL jdata - ignoring error",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    if (PRRTE_PROC_STATE_COMM_FAILED == state) {
        /* if it is our own connection, ignore it */
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_NAME, proc)) {
            PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted:proc_errors comm_failed to self - ignoring error",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* was it a daemon? */
        if (proc->jobid != PRRTE_PROC_MY_NAME->jobid) {
            /* nope - we can't seem to trust that we will catch the waitpid
             * in this situation, so push this over to be handled as if
             * it were a waitpid trigger so we don't create a bunch of
             * duplicate code */
            PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted:proc_errors comm_failed to non-daemon - handling as waitpid",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            /* get the proc_t */
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid))) {
                PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                goto cleanup;
            }
            /* leave the exit code alone - process this as a waitpid */
            t2 = PRRTE_NEW(prrte_wait_tracker_t);
            PRRTE_RETAIN(child);  // protect against race conditions
            t2->child = child;
            t2->evb = prrte_event_base;
            prrte_event_set(t2->evb, &t2->ev, -1,
                           PRRTE_EV_WRITE, prrte_odls_base_default_wait_local_proc, t2);
            prrte_event_set_priority(&t2->ev, PRRTE_MSG_PRI);
            prrte_event_active(&t2->ev, PRRTE_EV_WRITE, 1);
            goto cleanup;
        }
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:default:prted daemon %s exited",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));

        if (prrte_prteds_term_ordered) {
            /* are any of my children still alive */
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL != (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                    if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
                        PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                             "%s errmgr:default:prted[%s(%d)] proc %s is alive",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             __FILE__, __LINE__,
                                             PRRTE_NAME_PRINT(&child->name)));
                        goto cleanup;
                    }
                }
            }
            /* if all my routes and children are gone, then terminate
               ourselves nicely (i.e., this is a normal termination) */
            if (0 == prrte_routed.num_routes()) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted all routes gone - exiting",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            } else {
                PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted not exiting, num_routes() == %d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     (int)prrte_routed.num_routes()));
            }
        }
        /* if not, then we can continue */
        goto cleanup;
    }

    if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        goto cleanup;
    }
    /* if this is not a local proc for this job, we can
     * ignore this call
     */
    if (!PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_LOCAL)) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted:proc_errors proc is not local - ignoring error",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto cleanup;
    }

    PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:prted got state %s for proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         prrte_proc_state_to_str(state),
                         PRRTE_NAME_PRINT(proc)));

    if (PRRTE_PROC_STATE_TERM_NON_ZERO == state) {
        /* update the state */
        child->state = state;
        /* report this as abnormal termination to the HNP, unless we already have
         * done so for this job */
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FAIL_NOTIFIED, NULL, PRRTE_BOOL)) {
            alert = PRRTE_NEW(prrte_buffer_t);
            /* pack update state command */
            cmd = PRRTE_PLM_UPDATE_PROC_STATE;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            /* pack only the data for this proc - have to start with the jobid
             * so the receiver can unpack it correctly
             */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &proc->jobid, 1, PRRTE_JOBID))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }

            /* now pack the child's info */
            if (PRRTE_SUCCESS != (rc = pack_state_for_proc(alert, child))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            /* send it */
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted reporting proc %s abnormally terminated with non-zero status (local procs = %d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name),
                                 jdata->num_local_procs));
            if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                                  PRRTE_RML_TAG_PLM,
                                                  prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(alert);
            }
            /* mark that we notified the HNP for this job so we don't do it again */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FAIL_NOTIFIED, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);
        }
        /* if the proc has terminated, notify the state machine */
        if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
            PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_WAITPID) &&
            !PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_RECORDED)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
        goto cleanup;
    }

    if (PRRTE_PROC_STATE_FAILED_TO_START == state ||
        PRRTE_PROC_STATE_FAILED_TO_LAUNCH == state) {
        /* update the proc state */
        child->state = state;
        /* count the proc as having "terminated" */
        jdata->num_terminated++;
        /* leave the error report in this case to the
         * state machine, which will receive notice
         * when all local procs have attempted to start
         * so that we send a consolidated error report
         * back to the HNP
         */
        if (jdata->num_local_procs == jdata->num_terminated) {
            /* let the state machine know */
            if (PRRTE_PROC_STATE_FAILED_TO_START == state) {
                PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_FAILED_TO_START);
            } else {
                PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_FAILED_TO_LAUNCH);
            }
        }
        goto cleanup;
    }

    if (PRRTE_PROC_STATE_TERMINATED < state) {
        /* if we were ordered to terminate, see if
         * any of our routes or local children remain alive - if not, then
         * terminate ourselves. */
        if (prrte_prteds_term_ordered) {
            /* mark the child as no longer alive and update the counters, if necessary.
             * we have to do this here as we aren't going to send this to the state
             * machine, and we want to keep the bookkeeping accurate just in case */
            if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
                PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_ALIVE);
            }
            if (!PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_RECORDED)) {
                PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_RECORDED);
                jdata->num_terminated++;
            }
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL != (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                    if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
                        goto keep_going;
                    }
                }
            }
            /* if all my routes and children are gone, then terminate
               ourselves nicely (i.e., this is a normal termination) */
            if (0 == prrte_routed.num_routes()) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                     "%s errmgr:default:prted all routes gone - exiting",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
            /* no need to alert the HNP - we are already on our way out */
            goto cleanup;
        }

    keep_going:
        /* if the job hasn't completed and the state is abnormally
         * terminated, then we need to alert the HNP right away - but
         * only do this once!
         */
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FAIL_NOTIFIED, NULL, PRRTE_BOOL)) {
            alert = PRRTE_NEW(prrte_buffer_t);
            /* pack update state command */
            cmd = PRRTE_PLM_UPDATE_PROC_STATE;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            /* pack only the data for this proc - have to start with the jobid
             * so the receiver can unpack it correctly
             */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &proc->jobid, 1, PRRTE_JOBID))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            child->state = state;
            /* now pack the child's info */
            if (PRRTE_SUCCESS != (rc = pack_state_for_proc(alert, child))) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                 "%s errmgr:prted reporting proc %s abprted to HNP (local procs = %d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name),
                                 jdata->num_local_procs));
            /* send it */
            if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                                  PRRTE_RML_TAG_PLM,
                                                  prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
            }
            /* mark that we notified the HNP for this job so we don't do it again */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FAIL_NOTIFIED, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);
        }
        /* if the proc has terminated, notify the state machine */
        if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
            PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_WAITPID) &&
            !PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_RECORDED)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
        goto cleanup;
    }

    /* only other state is terminated - see if anyone is left alive */
    if (!any_live_children(proc->jobid)) {
        alert = PRRTE_NEW(prrte_buffer_t);
        /* pack update state command */
        cmd = PRRTE_PLM_UPDATE_PROC_STATE;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
            PRRTE_ERROR_LOG(rc);
            return;
        }
        /* pack the data for the job */
        if (PRRTE_SUCCESS != (rc = pack_state_update(alert, jdata))) {
            PRRTE_ERROR_LOG(rc);
            return;
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:prted reporting all procs in %s terminated",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_JOBID_PRINT(jdata->jobid)));

        /* remove all of this job's children from the global list */
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL == (ptr = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                continue;
            }
            if (jdata->jobid == ptr->name.jobid) {
                prrte_pointer_array_set_item(prrte_local_children, i, NULL);
                PRRTE_RELEASE(ptr);
            }
        }

        /* ensure the job's local session directory tree is removed */
        prrte_session_dir_cleanup(jdata->jobid);

        /* remove this job from our local job data since it is complete */
        PRRTE_RELEASE(jdata);

        /* send it */
        if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                              PRRTE_RML_TAG_PLM,
                                              prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(rc);
        }
        return;
    }

  cleanup:
    PRRTE_RELEASE(caddy);
}

/*****************
 * Local Functions
 *****************/
static bool any_live_children(prrte_jobid_t job)
{
    int i;
    prrte_proc_t *child;

    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        /* is this child part of the specified job? */
        if ((job == child->name.jobid || PRRTE_JOBID_WILDCARD == job) &&
            PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
            return true;
        }
    }

    /* if we get here, then nobody is left alive from that job */
    return false;

}

static int pack_state_for_proc(prrte_buffer_t *alert, prrte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &(child->name.vpid), 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->pid, 1, PRRTE_PID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->state, 1, PRRTE_PROC_STATE))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->exit_code, 1, PRRTE_EXIT_CODE))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}

static int pack_state_update(prrte_buffer_t *alert, prrte_job_t *jobdat)
{
    int rc, i;
    prrte_proc_t *child;
    prrte_vpid_t null=PRRTE_VPID_INVALID;

    /* pack the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &jobdat->jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (child->name.jobid == jobdat->jobid) {
            if (PRRTE_SUCCESS != (rc = pack_state_for_proc(alert, child))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    /* flag that this job is complete so the receiver can know */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &null, 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}

static void failed_start(prrte_job_t *jobdat)
{
    int i;
    prrte_proc_t *child;

    /* set the state */
    jobdat->state = PRRTE_JOB_STATE_FAILED_TO_START;

    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        /* is this child part of the specified job? */
        if (child->name.jobid == jobdat->jobid) {
            if (PRRTE_PROC_STATE_FAILED_TO_START == child->state) {
                /* this proc never launched - flag that the iof
                 * is complete or else we will hang waiting for
                 * pipes to close that were never opened
                 */
                PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_IOF_COMPLETE);
                /* ditto for waitpid */
                PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_WAITPID);
            }
        }
    }
    PRRTE_OUTPUT_VERBOSE((1, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:hnp: job %s repprted incomplete start",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jobdat->jobid)));
    return;
}

static void killprocs(prrte_jobid_t job, prrte_vpid_t vpid)
{
    prrte_pointer_array_t cmd;
    prrte_proc_t proc;
    int rc;

    if (PRRTE_JOBID_WILDCARD == job
        && PRRTE_VPID_WILDCARD == vpid) {
        if (PRRTE_SUCCESS != (rc = prrte_odls.kill_local_procs(NULL))) {
            PRRTE_ERROR_LOG(rc);
        }
        return;
    }

    PRRTE_CONSTRUCT(&cmd, prrte_pointer_array_t);
    PRRTE_CONSTRUCT(&proc, prrte_proc_t);
    proc.name.jobid = job;
    proc.name.vpid = vpid;
    prrte_pointer_array_add(&cmd, &proc);
    if (PRRTE_SUCCESS != (rc = prrte_odls.kill_local_procs(&cmd))) {
        PRRTE_ERROR_LOG(rc);
    }
    PRRTE_DESTRUCT(&cmd);
    PRRTE_DESTRUCT(&proc);
}
