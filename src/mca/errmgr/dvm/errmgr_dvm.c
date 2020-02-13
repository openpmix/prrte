/*
 * Copyright (c) 2009-2011 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011      Oracle and/or all its affiliates.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
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
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"

#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"

#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

#include "errmgr_dvm.h"

static int init(void);
static int finalize(void);

/******************
 * dvm module
 ******************/
prrte_errmgr_base_module_t prrte_errmgr_dvm_module = {
    .init = init,
    .finalize = finalize,
    .logfn = prrte_errmgr_base_log,
    .abort = prrte_errmgr_base_abort,
    .abort_peers = prrte_errmgr_base_abort_peers
};


/*
 * Local functions
 */
static void job_errors(int fd, short args, void *cbdata);
static void proc_errors(int fd, short args, void *cbdata);

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

static void _terminate_job(prrte_jobid_t jobid)
{
    prrte_pointer_array_t procs;
    prrte_proc_t pobj;

    PRRTE_CONSTRUCT(&procs, prrte_pointer_array_t);
    prrte_pointer_array_init(&procs, 1, 1, 1);
    PRRTE_CONSTRUCT(&pobj, prrte_proc_t);
    pobj.name.jobid = jobid;
    pobj.name.vpid = PRRTE_VPID_WILDCARD;
    prrte_pointer_array_add(&procs, &pobj);
    prrte_plm.terminate_procs(&procs);
    PRRTE_DESTRUCT(&procs);
    PRRTE_DESTRUCT(&pobj);
}

static void job_errors(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_job_state_t jobstate;
    prrte_buffer_t *answer;
    int32_t rc, ret;
    int room, *rmptr;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /*
     * if prrte is trying to shutdown, just let it
     */
    if (prrte_finalizing) {
        return;
    }

    /* if the jdata is NULL, then we ignore it as this
     * is reporting an unrecoverable error
     */
    if (NULL == caddy->jdata) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* update the state */
    jdata = caddy->jdata;
    jobstate = caddy->job_state;
    jdata->state = jobstate;

    PRRTE_OUTPUT_VERBOSE((1, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm: job %s reported state %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid),
                         prrte_job_state_to_str(jobstate)));

    if (jdata->jobid == PRRTE_PROC_MY_NAME->jobid) {
        /* if the daemon job aborted and we haven't heard from everyone yet,
         * then this could well have been caused by a daemon not finding
         * a way back to us. In this case, output a message indicating a daemon
         * died without reporting. Otherwise, say nothing as we
         * likely already output an error message */
        if (PRRTE_JOB_STATE_ABORTED == jobstate &&
            jdata->num_procs != jdata->num_reported) {
            prrte_routing_is_enabled = false;
            prrte_show_help("help-errmgr-base.txt", "failed-daemon", true);
        }
        /* there really isn't much else we can do since the problem
         * is in the DVM itself, so best just to terminate */
        jdata->num_terminated = jdata->num_procs;
        /* activate the terminated state so we can exit */
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* all other cases involve jobs submitted to the DVM - therefore,
     * we only inform the submitter of the problem, but do NOT terminate
     * the DVM itself */

    rc = prrte_pmix_convert_job_state_to_error(jobstate);
    answer = PRRTE_NEW(prrte_buffer_t);
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &rc, 1, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(caddy);
        return;
    }
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &jdata->jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* pack the room number */
    rmptr = &room;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_ROOM_NUM, (void**)&rmptr, PRRTE_INT)) {
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &room, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(caddy);
            return;
        }
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm sending notification of job %s failure to %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid),
                         PRRTE_NAME_PRINT(&jdata->originator)));
    if (0 > (ret = prrte_rml.send_buffer_nb(&jdata->originator, answer,
                                           PRRTE_RML_TAG_LAUNCH_RESP,
                                           prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(answer);
    }
    /* ensure we terminate any processes left running in the DVM */
    _terminate_job(jdata->jobid);

    /* all jobs were spawned by a requestor, so ensure that requestor
     * has been notified that the spawn completed - otherwise, a quick-failing
     * job might not generate a spawn response */
    rc = prrte_plm_base_spawn_reponse(PRRTE_SUCCESS, jdata);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
    }

    /* if the job never launched, then we need to let the
     * state machine know this job failed - it has no
     * other means of being alerted since no proc states
     * will be triggered */
    if (PRRTE_JOB_STATE_FAILED_TO_START == jdata->state ||
        PRRTE_JOB_STATE_NEVER_LAUNCHED == jdata->state ||
        PRRTE_JOB_STATE_FAILED_TO_LAUNCH == jdata->state ||
        PRRTE_JOB_STATE_ALLOC_FAILED == jdata->state ||
        PRRTE_JOB_STATE_MAP_FAILED == jdata->state ||
        PRRTE_JOB_STATE_CANNOT_LAUNCH == jdata->state) {
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
    }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

static void proc_errors(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_proc_t *pptr, *proct;
    prrte_process_name_t *proc = &caddy->name;
    prrte_proc_state_t state = caddy->proc_state;
    int i, rc;
    int32_t i32, *i32ptr;

    PRRTE_ACQUIRE_OBJECT(caddy);

    PRRTE_OUTPUT_VERBOSE((1, prrte_errmgr_base_framework.framework_output,
                         "%s errmgr:dvm: for proc %s state %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc),
                         prrte_proc_state_to_str(state)));

    /*
     * if prrte is trying to shutdown, just let it
     */
    if (prrte_finalizing) {
        goto cleanup;
    }

    /* get the job object */
    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        /* could be a race condition */
        goto cleanup;
    }
    pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid);

    /* we MUST handle a communication failure before doing anything else
     * as it requires some special care to avoid normal termination issues
     * for local application procs
     */
    if (PRRTE_PROC_STATE_COMM_FAILED == state) {
        /* is this to a daemon? */
        if (PRRTE_PROC_MY_NAME->jobid != proc->jobid) {
            /* nope - ignore it */
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                 "%s Comm failure to non-daemon proc - ignoring it",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* if this is my own connection, ignore it */
        if (PRRTE_PROC_MY_NAME->vpid == proc->vpid) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                 "%s Comm failure on my own connection - ignoring it",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto cleanup;
        }
        /* mark the daemon as gone */
        PRRTE_FLAG_UNSET(pptr, PRRTE_PROC_FLAG_ALIVE);
        /* update the state */
        pptr->state = state;
        /* adjust our num_procs */
        --prrte_process_info.num_procs;
        /* if we have ordered orteds to terminate or abort
         * is in progress, record it */
        if (prrte_prteds_term_ordered || prrte_abnormal_term_ordered) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                 "%s Comm failure: daemons terminating - recording daemon %s as gone",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(proc)));
            /* remove from dependent routes, if it is one */
            prrte_routed.route_lost(proc);
            /* if all my routes and local children are gone, then terminate ourselves */
            if (0 == prrte_routed.num_routes()) {
                for (i=0; i < prrte_local_children->size; i++) {
                    if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                        PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_ALIVE) && proct->state < PRRTE_PROC_STATE_UNTERMINATED) {
                        /* at least one is still alive */
                        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                             "%s Comm failure: at least one proc (%s) still alive",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             PRRTE_NAME_PRINT(&proct->name)));
                        goto cleanup;
                    }
                }
                /* call our appropriate exit procedure */
                PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                     "%s errmgr_dvm: all routes and children gone - ordering exit",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
        } else {
                PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                     "%s Comm failure: %d routes remain alive",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     (int)prrte_routed.num_routes()));
            }
            goto cleanup;
        }
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s Comm failure: daemon %s - aborting",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(proc)));
        /* record the first one to fail */
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            /* output an error message so the user knows what happened */
            prrte_show_help("help-errmgr-base.txt", "node-died", true,
                           PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                           prrte_process_info.nodename,
                           PRRTE_NAME_PRINT(proc),
                           pptr->node->name);
            /* mark the daemon job as failed */
            jdata->state = PRRTE_JOB_STATE_COMM_FAILED;
            /* point to the lowest rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            /* update our exit code */
            jdata->exit_code = pptr->exit_code;
            /* just in case the exit code hadn't been set, do it here - this
             * won't override any reported exit code */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRRTE_ERR_COMM_FAILURE;
            }
        }
        goto cleanup;
    }

    /* update the proc state - can get multiple reports on a proc
     * depending on circumstances, so ensure we only do this once
     */
    if (pptr->state < PRRTE_PROC_STATE_TERMINATED) {
        pptr->state = state;
    }

    /* if we were ordered to terminate, mark this proc as dead and see if
     * any of our routes or local  children remain alive - if not, then
     * terminate ourselves. */
    if (prrte_prteds_term_ordered) {
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                if (PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                    goto keep_going;
                }
            }
        }
        /* if all my routes and children are gone, then terminate
           ourselves nicely (i.e., this is a normal termination) */
        if (0 == prrte_routed.num_routes()) {
            PRRTE_OUTPUT_VERBOSE((2, prrte_errmgr_base_framework.framework_output,
                                 "%s errmgr:default:dvm all routes gone - exiting",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
    }

  keep_going:
    /* if this is a continuously operating job, then there is nothing more
     * to do - we let the job continue to run */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CONTINUOUS_OP, NULL, PRRTE_BOOL) ||
        PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RECOVERABLE)) {
        /* always mark the waitpid as having fired */
        PRRTE_ACTIVATE_PROC_STATE(&pptr->name, PRRTE_PROC_STATE_WAITPID_FIRED);
        /* if this is a remote proc, we won't hear anything more about it
         * as the default behavior would be to terminate the job. So be sure to
         * mark the IOF as having completed too so we correctly mark this proc
         * as dead and notify everyone as required */
        if (!PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_LOCAL)) {
            PRRTE_ACTIVATE_PROC_STATE(&pptr->name, PRRTE_PROC_STATE_IOF_COMPLETE);
        }
        goto cleanup;
    }

    /* all jobs were spawned by a requestor, so ensure that requestor
     * has been notified that the spawn completed - otherwise, a quick-failing
     * job might not generate a spawn response */
    rc = prrte_plm_base_spawn_reponse(PRRTE_SUCCESS, jdata);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
    }

    /* ensure we record the failed proc properly so we can report
     * the error once we terminate
     */
    switch (state) {
    case PRRTE_PROC_STATE_KILLED_BY_CMD:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s killed by cmd",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        /* we ordered this proc to die, so it isn't an abnormal termination
         * and we don't flag it as such
         */
        if (jdata->num_terminated >= jdata->num_procs) {
            /* this job has terminated */
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
        }
        /* don't abort the job as this isn't an abnormal termination */
        break;

    case PRRTE_PROC_STATE_ABORTED:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s aborted",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRRTE_JOB_STATE_ABORTED;
            /* point to the first rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->jobid);
        }
        break;

    case PRRTE_PROC_STATE_ABORTED_BY_SIG:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s aborted by signal",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRRTE_JOB_STATE_ABORTED_BY_SIG;
            /* point to the first rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->jobid);
        }
        break;

    case PRRTE_PROC_STATE_TERM_WO_SYNC:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s terminated without sync",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRRTE_JOB_STATE_ABORTED_WO_SYNC;
            /* point to the first rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* now treat a special case - if the proc exit'd without a required
             * sync, it may have done so with a zero exit code. We want to ensure
             * that the user realizes there was an error, so in this -one- case,
             * we overwrite the process' exit code with the default error code
             */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRRTE_ERROR_DEFAULT_EXIT_CODE;
            }
             /* kill the job */
            _terminate_job(jdata->jobid);
       }
        break;

    case PRRTE_PROC_STATE_FAILED_TO_START:
    case PRRTE_PROC_STATE_FAILED_TO_LAUNCH:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc),
                             prrte_proc_state_to_str(state)));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            prrte_buffer_t *answer;
            int id, *idptr, ret;

            if (PRRTE_PROC_STATE_FAILED_TO_START) {
                jdata->state = PRRTE_JOB_STATE_FAILED_TO_START;
            } else {
                jdata->state = PRRTE_JOB_STATE_FAILED_TO_LAUNCH;
            }
            /* point to the first rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* update our exit code */
            jdata->exit_code = pptr->exit_code;
            /* just in case the exit code hadn't been set, do it here - this
             * won't override any reported exit code */
            if (0 == jdata->exit_code) {
                jdata->exit_code = PRRTE_ERR_FAILED_TO_START;
            }
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            /* send a notification to the requestor - indicate that this is a spawn response */
            answer = PRRTE_NEW(prrte_buffer_t);
            /* pack the return status */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &pptr->exit_code, 1, PRRTE_INT32))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the jobid to be returned */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &jdata->jobid, 1, PRRTE_JOBID))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            idptr = &id;
            if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_ROOM_NUM, (void**)&idptr, PRRTE_INT)) {
                /* pack the sender's index to the tracking object */
                if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, idptr, 1, PRRTE_INT))) {
                    PRRTE_ERROR_LOG(ret);
                    PRRTE_RELEASE(answer);
                    goto CLEANUP;
                }
            }
            if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FIXED_DVM, NULL, PRRTE_BOOL)) {
                /* we need to send the requestor more info about what happened */
                prrte_dss.pack(answer, &jdata->state, 1, PRRTE_JOB_STATE_T);
                prrte_dss.pack(answer, &pptr, 1, PRRTE_PROC);
                prrte_dss.pack(answer, &pptr->node, 1, PRRTE_NODE);
            }
            /* return response */
            if (0 > (ret = prrte_rml.send_buffer_nb(&jdata->originator, answer,
                                                   PRRTE_RML_TAG_LAUNCH_RESP,
                                                   prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_RELEASE(answer);
            }
          CLEANUP:
            /* kill the job */
            _terminate_job(jdata->jobid);
        }
        /* if this was a daemon, report it */
        if (jdata->jobid == PRRTE_PROC_MY_NAME->jobid) {
            /* output a message indicating we failed to launch a daemon */
            prrte_show_help("help-errmgr-base.txt", "failed-daemon-launch", true);
        }
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
        break;

    case PRRTE_PROC_STATE_CALLED_ABORT:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s called abort with exit code %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc), pptr->exit_code));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRRTE_JOB_STATE_CALLED_ABORT;
            /* point to the first proc to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->jobid);
        }
        break;

    case PRRTE_PROC_STATE_TERM_NON_ZERO:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s exited with non-zero status %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc),
                             pptr->exit_code));
        jdata->exit_code = pptr->exit_code;
        /* track the number of non-zero exits */
        i32 = 0;
        i32ptr = &i32;
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_NUM_NONZERO_EXIT, (void**)&i32ptr, PRRTE_INT32);
        ++i32;
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NUM_NONZERO_EXIT, PRRTE_ATTR_LOCAL, i32ptr, PRRTE_INT32);
        if (prrte_abort_non_zero_exit) {
            if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
                jdata->state = PRRTE_JOB_STATE_NON_ZERO_TERM;
                /* point to the first rank to cause the problem */
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
                /* retain the object so it doesn't get free'd */
                PRRTE_RETAIN(pptr);
                PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
                /* kill the job */
                _terminate_job(jdata->jobid);
            }
        } else {
            /* user requested we consider this normal termination */
            if (jdata->num_terminated >= jdata->num_procs) {
                /* this job has terminated */
                PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
            }
        }
        break;

    case PRRTE_PROC_STATE_HEARTBEAT_FAILED:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s heartbeat failed",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_ABORTED)) {
            jdata->state = PRRTE_JOB_STATE_HEARTBEAT_FAILED;
            /* point to the first rank to cause the problem */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, PRRTE_ATTR_LOCAL, pptr, PRRTE_PTR);
            /* retain the object so it doesn't get free'd */
            PRRTE_RETAIN(pptr);
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_ABORTED);
            jdata->exit_code = pptr->exit_code;
            /* kill the job */
            _terminate_job(jdata->jobid);
        }
        /* remove from dependent routes, if it is one */
        prrte_routed.route_lost(proc);
        break;

    case PRRTE_PROC_STATE_UNABLE_TO_SEND_MSG:
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: unable to send message to proc %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc)));
        /* if this proc is one of my daemons, then we are truly
         * hosed - so just exit out
         */
        if (PRRTE_PROC_MY_NAME->jobid == proc->jobid) {
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            break;
        }
        break;

    default:
        /* shouldn't get this, but terminate job if required */
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                             "%s errmgr:dvm: proc %s default error %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(proc),
                             prrte_proc_state_to_str(state)));
        if (jdata->num_terminated == jdata->num_procs) {
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
        }
        break;
    }
    /* if the waitpid fired, be sure to let the state machine know */
    if (PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_WAITPID)) {
        PRRTE_ACTIVATE_PROC_STATE(&pptr->name, PRRTE_PROC_STATE_WAITPID_FIRED);
    }

 cleanup:
    PRRTE_RELEASE(caddy);
}
