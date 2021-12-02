/*
 * Copyright (c) 2009-2011 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011      Oracle and/or all its affiliates.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include "src/pmix/pmix-internal.h"
#include <pmix.h>
#include <pmix_server.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/mca/ess/ess.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/odls/odls.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/mca/state/state.h"

#include "src/threads/threads.h"
#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/errmgr.h"

#include "errmgr_dvm.h"
#include "src/mca/propagate/propagate.h"

static int init(void);
static int finalize(void);

/******************
 * dvm module
 ******************/
prte_errmgr_base_module_t prte_errmgr_dvm_module = {
    .init = init,
    .finalize = finalize,
    .logfn = prte_errmgr_base_log,
    .abort = prte_errmgr_base_abort,
    .abort_peers = prte_errmgr_base_abort_peers,
    .enable_detector = NULL
};

/*
 * Local functions
 */
static void job_errors(int fd, short args, void *cbdata);
static void proc_errors(int fd, short args, void *cbdata);

/* propagate error handler callbacks */
static int pack_state_for_proc(pmix_data_buffer_t *alert, prte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    rc = PMIx_Data_pack(NULL, alert, &(child->name.rank), 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pck the pid */
    rc = PMIx_Data_pack(NULL, alert, &child->pid, 1, PMIX_PID);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    rc = PMIx_Data_pack(NULL, alert, &child->state, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    rc = PMIx_Data_pack(NULL, alert, &child->exit_code, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}

static void register_cbfunc(int status, size_t errhndler, void *cbdata)
{

    if (NULL != prte_propagate.register_cb) {
        prte_propagate.register_cb();
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "errmgr:dvm:event register cbfunc with status %d ", status));
    }
}

static void error_notify_cbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                                const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                pmix_info_t *results, size_t nresults,
                                pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    int rc = PRTE_SUCCESS;
    prte_proc_t *temp_orte_proc;
    pmix_data_buffer_t *alert;
    prte_job_t *jdata;
    prte_plm_cmd_flag_t cmd;
    size_t n;

    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PMIX_XFER_PROCID(&proc, info[n].value.data.proc);

                if (prte_get_proc_daemon_vpid(&proc) != PRTE_PROC_MY_NAME->rank) {
                    return;
                }
                PRTE_OUTPUT_VERBOSE(
                    (5, prte_errmgr_base_framework.framework_output,
                     "%s errmgr: dvm: error proc %s with key-value %s notified from %s",
                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc), info[n].key,
                     PRTE_NAME_PRINT(source)));

                if (NULL == (jdata = prte_get_job_data_object(proc.nspace))) {
                    /* must already be complete */
                    PRTE_OUTPUT_VERBOSE(
                        (5, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm:error_notify_callback NULL jdata - ignoring error",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    return;
                }
                temp_orte_proc = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs,
                                                                             proc.rank);
                if (NULL == temp_orte_proc) {
                    /* must already be gone */
                    return;
                }

                PMIX_DATA_BUFFER_CREATE(alert);
                /* pack update state command */
                cmd = PRTE_PLM_UPDATE_PROC_STATE;
                rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }

                /* pack jobid */
                rc = PMIx_Data_pack(NULL, alert, &proc.nspace, 1, PMIX_PROC_NSPACE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }

                /* proc state now is PRTE_PROC_STATE_ABORTED_BY_SIG, cause odls set state to this;
                 * code is 128+9 */
                temp_orte_proc->state = PRTE_PROC_STATE_ABORTED_BY_SIG;
                /* now pack the child's info */
                if (PRTE_SUCCESS != (rc = pack_state_for_proc(alert, temp_orte_proc))) {
                    PRTE_ERROR_LOG(rc);
                    return;
                }

                /* send this process's info to hnp */
                if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, alert, PRTE_RML_TAG_PLM,
                                                      prte_rml_send_callback, NULL))) {
                    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                         "%s errmgr:dvm: send to hnp failed",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(alert);
                }
                if (PRTE_FLAG_TEST(temp_orte_proc, PRTE_PROC_FLAG_IOF_COMPLETE)
                    && PRTE_FLAG_TEST(temp_orte_proc, PRTE_PROC_FLAG_WAITPID)
                    && !PRTE_FLAG_TEST(temp_orte_proc, PRTE_PROC_FLAG_RECORDED)) {
                    PRTE_ACTIVATE_PROC_STATE(&proc, PRTE_PROC_STATE_TERMINATED);
                }

                prte_propagate.prp(source->nspace, source, &proc, PRTE_ERR_PROC_ABORTED);
                break;
            }
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PRTE_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static int init(void)
{
    /* setup state machine to trap job errors */
    prte_state.add_job_state(PRTE_JOB_STATE_ERROR, job_errors, PRTE_ERROR_PRI);

    /* set the lost connection state to run at MSG priority so
     * we can process any last messages from the proc
     */
    prte_state.add_proc_state(PRTE_PROC_STATE_COMM_FAILED, proc_errors, PRTE_MSG_PRI);

    if (prte_enable_ft_detector) {
        /* setup state machine to trap proc errors */
        pmix_status_t pcode = prte_pmix_convert_rc(PRTE_ERR_PROC_ABORTED);

        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: register evhandler in errmgr",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIx_Register_event_handler(&pcode, 1, NULL, 0, error_notify_cbfunc, register_cbfunc, NULL);
    }

    prte_state.add_proc_state(PRTE_PROC_STATE_ERROR, proc_errors, PRTE_ERROR_PRI);

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    return PRTE_SUCCESS;
}

static void _terminate_job(pmix_nspace_t jobid)
{
    prte_pointer_array_t procs;
    prte_proc_t pobj;

    PRTE_CONSTRUCT(&procs, prte_pointer_array_t);
    prte_pointer_array_init(&procs, 1, 1, 1);
    PRTE_CONSTRUCT(&pobj, prte_proc_t);
    PMIX_LOAD_PROCID(&pobj.name, jobid, PMIX_RANK_WILDCARD);
    prte_pointer_array_add(&procs, &pobj);
    prte_plm.terminate_procs(&procs);
    PRTE_DESTRUCT(&procs);
    PRTE_DESTRUCT(&pobj);
}

static void job_errors(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_job_state_t jobstate;
    int32_t rc;

    PRTE_ACQUIRE_OBJECT(caddy);

    /*
     * if prte is trying to shutdown, just let it
     */
    if (prte_finalizing) {
        return;
    }

    /* if the jdata is NULL, then it is referencing the daemon job */
    if (NULL == caddy->jdata) {
        caddy->jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        PRTE_RETAIN(caddy->jdata);
    }

    /* update the state */
    jdata = caddy->jdata;
    jobstate = caddy->job_state;
    jdata->state = jobstate;

    PRTE_OUTPUT_VERBOSE((1, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm: job %s reported state %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                         prte_job_state_to_str(jobstate)));

    if (PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        if (PRTE_JOB_STATE_FAILED_TO_START == jdata->state
            || PRTE_JOB_STATE_NEVER_LAUNCHED == jdata->state
            || PRTE_JOB_STATE_FAILED_TO_LAUNCH == jdata->state
            || PRTE_JOB_STATE_CANNOT_LAUNCH == jdata->state) {
            prte_routing_is_enabled = false;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            PRTE_RELEASE(caddy);
            return;
        }
        /* if the daemon job aborted and we haven't heard from everyone yet,
         * then this could well have been caused by a daemon not finding
         * a way back to us. In this case, output a message indicating a daemon
         * died without reporting. Otherwise, say nothing as we
         * likely already output an error message */
        if (PRTE_JOB_STATE_ABORTED == jobstate && jdata->num_procs != jdata->num_reported) {
            prte_routing_is_enabled = false;
            prte_show_help("help-errmgr-base.txt", "failed-daemon", true);
        }
        /* there really isn't much else we can do since the problem
         * is in the DVM itself, so best just to terminate */
        jdata->num_terminated = jdata->num_procs;
        /* activate the terminated state so we can exit */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
        PRTE_RELEASE(caddy);
        return;
    }

    /* all other cases involve jobs submitted to the DVM - therefore,
     * we only inform the submitter of the problem, but do NOT terminate
     * the DVM itself */

    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm sending notification of job %s failure to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                         PRTE_NAME_PRINT(&jdata->originator)));

    /* all jobs were spawned by a requestor, so ensure that requestor
     * has been notified that the spawn completed - otherwise, a quick-failing
     * job might not generate a spawn response */
    rc = prte_pmix_convert_job_state_to_error(jobstate);
    rc = prte_plm_base_spawn_response(rc, jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* ensure we terminate any processes left running in the DVM */
    _terminate_job(jdata->nspace);

    /* if the job never launched, then we need to let the
     * state machine know this job failed - it has no
     * other means of being alerted since no proc states
     * will be triggered */
    if (PRTE_JOB_STATE_FAILED_TO_START == jdata->state
        || PRTE_JOB_STATE_NEVER_LAUNCHED == jdata->state
        || PRTE_JOB_STATE_FAILED_TO_LAUNCH == jdata->state
        || PRTE_JOB_STATE_ALLOC_FAILED == jdata->state
        || PRTE_JOB_STATE_MAP_FAILED == jdata->state
        || PRTE_JOB_STATE_CANNOT_LAUNCH == jdata->state) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
    }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

static void proc_errors(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_proc_t *pptr, *proct;
    pmix_proc_t *proc = &caddy->name;
    prte_proc_state_t state = caddy->proc_state;
    int i;
    int32_t i32, *i32ptr;

    PRTE_ACQUIRE_OBJECT(caddy);

    PRTE_OUTPUT_VERBOSE((1, prte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm: for proc %s state %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(proc), prte_proc_state_to_str(state)));

    /* get the job object */
    if (prte_finalizing || NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        /* could be a race condition */
        PRTE_RELEASE(caddy);
        return;
    }
    pptr = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, proc->rank);
    if (NULL == pptr) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto cleanup;
    }

    /* we MUST handle a communication failure before doing anything else
     * as it requires some special care to avoid normal termination issues
     * for local application procs
     */
    if (PRTE_PROC_STATE_COMM_FAILED == state) {
        /* is this to a daemon? */
        if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, proc->nspace)) {
            /* nope - ignore it */
            PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                 "%s Comm failure to non-daemon proc - ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* if this is my own connection, ignore it */
        if (PRTE_PROC_MY_NAME->rank == proc->rank) {
            PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                 "%s Comm failure on my own connection - ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* mark the daemon as gone */
        PRTE_FLAG_UNSET(pptr, PRTE_PROC_FLAG_ALIVE);
        /* update the state */
        pptr->state = state;
        /* adjust our num_procs */
        --prte_process_info.num_daemons;
        /* if we have ordered orteds to terminate or abort
         * is in progress, record it */
        if (prte_prteds_term_ordered || prte_abnormal_term_ordered) {
            PRTE_OUTPUT_VERBOSE(
                (5, prte_errmgr_base_framework.framework_output,
                 "%s Comm failure: daemons terminating - recording daemon %s as gone",
                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
            /* remove from dependent routes, if it is one */
            prte_routed.route_lost(proc);
            /* if all my routes and local children are gone, then terminate ourselves */
            if (0 == prte_routed.num_routes()) {
                for (i = 0; i < prte_local_children->size; i++) {
                    if (NULL
                            != (proct = (prte_proc_t *)
                                    prte_pointer_array_get_item(prte_local_children, i))
                        && PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_ALIVE)
                        && proct->state < PRTE_PROC_STATE_UNTERMINATED) {
                        /* at least one is still alive */
                        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                             "%s Comm failure: at least one proc (%s) still alive",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                             PRTE_NAME_PRINT(&proct->name)));
                        goto cleanup;
                    }
                }
                /* call our appropriate exit procedure */
                PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                     "%s errmgr_dvm: all routes and children gone - ordering exit",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            } else {
                PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                     "%s Comm failure: %d routes remain alive",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     (int) prte_routed.num_routes()));
            }
            goto cleanup;
        }
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s Comm failure: daemon %s - aborting",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        /* record the first one to fail */
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            /* output an error message so the user knows what happened */
            prte_show_help("help-errmgr-base.txt", "node-died", true,
                           PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_process_info.nodename,
                           PRTE_NAME_PRINT(proc), pptr->node->name);
            /* mark the daemon job as failed */
            jdata->state = PRTE_JOB_STATE_COMM_FAILED;
            /* point to the lowest rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            /* update our exit code */
            jdata->exit_code = pptr->exit_code;
            /* just in case the exit code hadn't been set, do it here - this
             * won't override any reported exit code */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRTE_ERR_COMM_FAILURE;
            }
        }
        goto cleanup;
    }

    /* update the proc state - can get multiple reports on a proc
     * depending on circumstances, so ensure we only do this once
     */
    if (pptr->state < PRTE_PROC_STATE_TERMINATED) {
        pptr->state = state;
    }

    /* if we were ordered to terminate, mark this proc as dead and see if
     * any of our routes or local  children remain alive - if not, then
     * terminate ourselves. */
    if (prte_prteds_term_ordered) {
        for (i = 0; i < prte_local_children->size; i++) {
            if (NULL
                != (proct = (prte_proc_t *) prte_pointer_array_get_item(prte_local_children, i))) {
                if (PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_ALIVE)) {
                    goto keep_going;
                }
            }
        }
        /* if all my routes and children are gone, then terminate
           ourselves nicely (i.e., this is a normal termination) */
        if (0 == prte_routed.num_routes()) {
            PRTE_OUTPUT_VERBOSE((2, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr:default:dvm all routes gone - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
    }

keep_going:
    /* if this is a continuously operating job, then there is nothing more
     * to do - we let the job continue to run */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS_OP, NULL, PMIX_BOOL)
        || PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE)) {
        /* always mark the waitpid as having fired */
        PRTE_ACTIVATE_PROC_STATE(&pptr->name, PRTE_PROC_STATE_WAITPID_FIRED);
        /* if this is a remote proc, we won't hear anything more about it
         * as the default behavior would be to terminate the job. So be sure to
         * mark the IOF as having completed too so we correctly mark this proc
         * as dead and notify everyone as required */
        if (!PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_LOCAL)) {
            PRTE_ACTIVATE_PROC_STATE(&pptr->name, PRTE_PROC_STATE_IOF_COMPLETE);
        }
        goto cleanup;
    }

    /* ensure we record the failed proc properly so we can report
     * the error once we terminate
     */
    switch (state) {
    case PRTE_PROC_STATE_KILLED_BY_CMD:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s killed by cmd",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        /* we ordered this proc to die, so it isn't an abnormal termination
         * and we don't flag it as such
         */
        if (jdata->num_terminated >= jdata->num_procs) {
            /* this job has terminated */
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
        }
        /* don't abort the job as this isn't an abnormal termination */
        break;

    case PRTE_PROC_STATE_ABORTED:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s aborted", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(proc)));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRTE_JOB_STATE_ABORTED;
            /* point to the first rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE)) {
                _terminate_job(jdata->nspace);
            }
        }
        break;

    case PRTE_PROC_STATE_ABORTED_BY_SIG:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s aborted by signal",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRTE_JOB_STATE_ABORTED_BY_SIG;
            /* point to the first rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* do not kill the job if it is recoverable */
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE)) {
                _terminate_job(jdata->nspace);
            }
        }
        break;

    case PRTE_PROC_STATE_TERM_WO_SYNC:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s terminated without sync",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRTE_JOB_STATE_ABORTED_WO_SYNC;
            /* point to the first rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* now treat a special case - if the proc exit'd without a required
             * sync, it may have done so with a zero exit code. We want to ensure
             * that the user realizes there was an error, so in this -one- case,
             * we overwrite the process' exit code with the default error code
             */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRTE_ERROR_DEFAULT_EXIT_CODE;
            }
            /* kill the job */
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE) && !prte_allowed_exit_without_sync) {
                _terminate_job(jdata->nspace);
            }
        }
        break;

    case PRTE_PROC_STATE_FAILED_TO_START:
    case PRTE_PROC_STATE_FAILED_TO_LAUNCH:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(proc), prte_proc_state_to_str(state)));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            if (PRTE_PROC_STATE_FAILED_TO_START) {
                jdata->state = PRTE_JOB_STATE_FAILED_TO_START;
            } else {
                jdata->state = PRTE_JOB_STATE_FAILED_TO_LAUNCH;
            }
            /* point to the first rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* update our exit code */
            jdata->exit_code = pptr->exit_code;
            /* just in case the exit code hadn't been set, do it here - this
             * won't override any reported exit code */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRTE_ERR_FAILED_TO_START;
            }
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            /* kill the job */
            _terminate_job(jdata->nspace);
        }
        /* if this was a daemon, report it */
        if (PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
            /* output a message indicating we failed to launch a daemon */
            prte_show_help("help-errmgr-base.txt", "failed-daemon-launch", true);
        }
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START);
        break;

    case PRTE_PROC_STATE_CALLED_ABORT:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s called abort with exit code %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                             pptr->exit_code));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRTE_JOB_STATE_CALLED_ABORT;
            /* point to the first proc to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->nspace);
        }
        break;

    case PRTE_PROC_STATE_TERM_NON_ZERO:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s exited with non-zero status %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                             pptr->exit_code));
        jdata->exit_code = pptr->exit_code;
        /* track the number of non-zero exits */
        i32 = 0;
        i32ptr = &i32;
        prte_get_attribute(&jdata->attributes, PRTE_JOB_NUM_NONZERO_EXIT, (void **) &i32ptr,
                           PMIX_INT32);
        ++i32;
        prte_set_attribute(&jdata->attributes, PRTE_JOB_NUM_NONZERO_EXIT, PRTE_ATTR_LOCAL, i32ptr,
                           PMIX_INT32);
        if (prte_abort_non_zero_exit) {
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
                jdata->state = PRTE_JOB_STATE_NON_ZERO_TERM;
                /* point to the first rank to cause the problem */
                prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                                   PMIX_POINTER);
                /* retain the object so it doesn't get free'd */
                PRTE_RETAIN(pptr);
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
                /* kill the job */
                _terminate_job(jdata->nspace);
            }
        } else {
            /* user requested we consider this normal termination */
            if (jdata->num_terminated >= jdata->num_procs) {
                /* this job has terminated */
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
            }
        }
        break;

    case PRTE_PROC_STATE_HEARTBEAT_FAILED:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s heartbeat failed",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRTE_JOB_STATE_HEARTBEAT_FAILED;
            /* point to the first rank to cause the problem */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, PRTE_ATTR_LOCAL, pptr,
                               PMIX_POINTER);
            /* retain the object so it doesn't get free'd */
            PRTE_RETAIN(pptr);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->nspace);
        }
        /* remove from dependent routes, if it is one */
        prte_routed.route_lost(proc);
        break;

    case PRTE_PROC_STATE_UNABLE_TO_SEND_MSG:
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: unable to send message to proc %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
        /* if this proc is one of my daemons, then we are truly
         * hosed - so just exit out
         */
        if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, proc->nspace)) {
            /* do not kill the job if it is recoverable */
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE)) {
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        }
        /* remove from dependent routes, if it is one */
        prte_routed.route_lost(proc);
        break;

    default:
        /* shouldn't get this, but terminate job if required */
        PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s default error %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                             prte_proc_state_to_str(state)));
        if (jdata->num_terminated == jdata->num_procs) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
        }
        break;
    }
    /* if the waitpid fired, be sure to let the state machine know */
    if (PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_WAITPID)) {
        PRTE_ACTIVATE_PROC_STATE(&pptr->name, PRTE_PROC_STATE_WAITPID_FIRED);
    }

cleanup:
    PRTE_RELEASE(caddy);
}
