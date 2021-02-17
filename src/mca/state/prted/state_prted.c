/*
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
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
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/output.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/util/session_dir.h"
#include "src/threads/threads.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_data_server.h"
#include "src/runtime/prte_quit.h"

#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"
#include "state_prted.h"

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

/******************
 * PRTED module
 ******************/
prte_state_base_module_t prte_state_prted_module = {
    init,
    finalize,
    prte_state_base_activate_job_state,
    prte_state_base_add_job_state,
    prte_state_base_set_job_state_callback,
    prte_state_base_set_job_state_priority,
    prte_state_base_remove_job_state,
    prte_state_base_activate_proc_state,
    prte_state_base_add_proc_state,
    prte_state_base_set_proc_state_callback,
    prte_state_base_set_proc_state_priority,
    prte_state_base_remove_proc_state
};

/* Local functions */
static void track_jobs(int fd, short argc, void *cbdata);
static void track_procs(int fd, short argc, void *cbdata);
static int pack_state_update(pmix_data_buffer_t *buf, prte_job_t *jdata);

/* defined default state machines */
static prte_job_state_t job_states[] = {
    PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE,
    PRTE_JOB_STATE_READY_FOR_DEBUG
};
static prte_state_cbfunc_t job_callbacks[] = {
    track_jobs,
    track_jobs
};

static prte_proc_state_t proc_states[] = {
    PRTE_PROC_STATE_RUNNING,
    PRTE_PROC_STATE_REGISTERED,
    PRTE_PROC_STATE_IOF_COMPLETE,
    PRTE_PROC_STATE_WAITPID_FIRED,
    PRTE_PROC_STATE_TERMINATED
};
static prte_state_cbfunc_t proc_callbacks[] = {
    track_procs,
    track_procs,
    track_procs,
    track_procs,
    track_procs
};

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int num_states, i, rc;

    /* setup the state machine */
    PRTE_CONSTRUCT(&prte_job_states, prte_list_t);
    PRTE_CONSTRUCT(&prte_proc_states, prte_list_t);

    num_states = sizeof(job_states) / sizeof(prte_job_state_t);
    for (i=0; i < num_states; i++) {
        if (PRTE_SUCCESS != (rc = prte_state.add_job_state(job_states[i],
                                                           job_callbacks[i],
                                                           PRTE_SYS_PRI))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    /* add a default error response */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_FORCED_EXIT,
                                                       prte_quit, PRTE_ERROR_PRI))) {
        PRTE_ERROR_LOG(rc);
    }
    /* add a state for when we are ordered to terminate */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_DAEMONS_TERMINATED,
                                                       prte_quit, PRTE_SYS_PRI))) {
        PRTE_ERROR_LOG(rc);
    }
    if (5 < prte_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_job_state_machine();
    }

    /* populate the proc state machine to allow us to
     * track proc lifecycle changes
     */
    num_states = sizeof(proc_states) / sizeof(prte_proc_state_t);
    for (i=0; i < num_states; i++) {
        if (PRTE_SUCCESS != (rc = prte_state.add_proc_state(proc_states[i],
                                                            proc_callbacks[i],
                                                            PRTE_SYS_PRI))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    if (5 < prte_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_proc_state_machine();
    }
    return PRTE_SUCCESS;
}

static int finalize(void)
{
    prte_list_item_t *item;

    /* cleanup the state machines */
    while (NULL != (item = prte_list_remove_first(&prte_job_states))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_job_states);
    while (NULL != (item = prte_list_remove_first(&prte_proc_states))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_proc_states);

    return PRTE_SUCCESS;
}

static void track_jobs(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    pmix_data_buffer_t *alert = NULL;
    prte_plm_cmd_flag_t cmd;
    int rc, i;
    prte_proc_state_t running = PRTE_PROC_STATE_RUNNING;
    prte_proc_t *child;
    pmix_rank_t null=PMIX_RANK_INVALID;

    PRTE_ACQUIRE_OBJECT(caddy);

    switch (caddy->job_state) {
        case PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE:
            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                "%s state:prted:track_jobs sending local launch complete for job %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PRTE_JOBID_PRINT(caddy->jdata->nspace)));
            /* update the HNP with all proc states for this job */
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
            rc = PMIx_Data_pack(NULL, alert, &caddy->jdata->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL == (child = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
                    continue;
                }
                /* if this child is part of the job... */
                if (PMIX_CHECK_NSPACE(child->name.nspace, caddy->jdata->nspace)) {
                    /* pack the child's vpid */
                    rc = PMIx_Data_pack(NULL, alert, &child->name.rank, 1, PMIX_PROC_RANK);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                    /* pack the pid */
                    rc = PMIx_Data_pack(NULL, alert, &child->pid, 1, PMIX_PID);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                    /* If this proc failed to start, then send that info.
                     * However if it normally terminated then do not send the info.
                     * Instead report it as running here, and the child waitpid
                     * function will send back the normal terminated state when the
                     * the job is complete.
                     */
                    if (PRTE_PROC_STATE_TERMINATED < child->state) {
                        rc = PMIx_Data_pack(NULL, alert, &child->state, 1, PMIX_UINT32);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_DATA_BUFFER_RELEASE(alert);
                            goto cleanup;
                        }
                    } else {
                        /* pack the RUNNING state to avoid any race conditions */
                        rc = PMIx_Data_pack(NULL, alert, &running, 1, PMIX_UINT32);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_DATA_BUFFER_RELEASE(alert);
                            goto cleanup;
                        }
                    }
                    /* pack its exit code */
                    rc = PMIx_Data_pack(NULL, alert, &child->exit_code, 1, PMIX_INT32);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                }
            }

            /* flag that this job is complete so the receiver can know */
            rc = PMIx_Data_pack(NULL, alert, &null, 1, PMIX_PROC_RANK);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            break;

        case PRTE_JOB_STATE_READY_FOR_DEBUG:
            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                 "%s state:prted:track_jobs sending ready for debug for job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(caddy->jdata->nspace)));
            /* update the HNP with all proc states for this job */
            PMIX_DATA_BUFFER_CREATE(alert);
            running = PRTE_PROC_STATE_READY_FOR_DEBUG;
            /* pack update state command */
            cmd = PRTE_PLM_UPDATE_PROC_STATE;
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack the jobid */
            rc = PMIx_Data_pack(NULL, alert, &caddy->jdata->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL == (child = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
                    continue;
                }
                /* if this child is part of the job... */
                if (PMIX_CHECK_NSPACE(child->name.nspace, caddy->jdata->nspace)) {
                    /* pack the child's vpid */
                    rc = PMIx_Data_pack(NULL, alert, &child->name.rank, 1, PMIX_PROC_RANK);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                    /* pack the pid */
                    rc = PMIx_Data_pack(NULL, alert, &child->pid, 1, PMIX_PID);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                    /* pack the RUNNING state to avoid any race conditions */
                    rc = PMIx_Data_pack(NULL, alert, &running, 1, PMIX_UINT32);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                    /* pack its exit code */
                    rc = PMIx_Data_pack(NULL, alert, &child->exit_code, 1, PMIX_INT32);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                }
            }
            break;

        default:
            break;
    }

    if (NULL != alert) {
        /* send it */
        if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, alert,
                                              PRTE_RML_TAG_PLM,
                                              prte_rml_send_callback, NULL))) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(alert);
        }
    }

  cleanup:
    PRTE_RELEASE(caddy);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t*)cbdata;

    PRTE_POST_OBJECT(lk);
    lk->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lk);
}
static void track_procs(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    pmix_proc_t *proc;
    prte_proc_state_t state;
    prte_job_t *jdata;
    prte_proc_t *pdata, *pptr;
    pmix_data_buffer_t *alert;
    int rc, i;
    prte_plm_cmd_flag_t cmd;
    int32_t index;
    prte_job_map_t *map;
    prte_node_t *node;
    pmix_proc_t target;
    prte_pmix_lock_t lock;

    PRTE_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                         "%s state:prted:track_procs called for proc %s state %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(proc),
                         prte_proc_state_to_str(state)));

    /* get the job object for this proc */
    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto cleanup;
    }
    pdata = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, proc->rank);
    if (NULL == pdata) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto cleanup;
    }

    if (PRTE_PROC_STATE_RUNNING == state) {
        /* update the proc state */
        pdata->state = state;
        jdata->num_launched++;
        if (jdata->num_launched == jdata->num_local_procs) {
            /* tell the state machine that all local procs for this job
             * were launched so that it can do whatever it needs to do,
             * like send a state update message for all procs to the HNP
             */
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE);
        }
        /* don't update until we are told that all are done */
    } else if (PRTE_PROC_STATE_REGISTERED == state) {
        /* update the proc state */
        pdata->state = state;
        jdata->num_reported++;
        if (jdata->num_reported == jdata->num_local_procs) {
            /* once everyone registers, notify the HNP */

            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                 "%s state:prted: notifying HNP all local registered",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

            PMIX_DATA_BUFFER_CREATE(alert);
            /* pack registered command */
            cmd = PRTE_PLM_REGISTERED_CMD;
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack the jobid */
            rc = PMIx_Data_pack(NULL, alert, &proc->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }

            /* pack all the local child vpids */
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL == (pptr = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
                    continue;
                }
                if (PMIX_CHECK_NSPACE(pptr->name.nspace, proc->nspace)) {
                    rc = PMIx_Data_pack(NULL, alert, &pptr->name.rank, 1, PMIX_PROC_RANK);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(alert);
                        goto cleanup;
                    }
                }
            }
            /* send it */
            if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, alert,
                                                  PRTE_RML_TAG_PLM,
                                                  prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
            } else {
                rc = PRTE_SUCCESS;
            }
        }
    } else if (PRTE_PROC_STATE_IOF_COMPLETE == state) {
        /* do NOT update the proc state as this can hit
         * while we are still trying to notify the HNP of
         * successful launch for short-lived procs
         */
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_IOF_COMPLETE);
        /* Release the stdin IOF file descriptor for this child, if one
         * was defined. File descriptors for the other IOF channels - stdout,
         * stderr, and stddiag - were released when their associated pipes
         * were cleared and closed due to termination of the process
         * Do this after we handle termination in case the IOF needs
         * to check to see if all procs from the job are actually terminated
         */
        if (NULL != prte_iof.close) {
            prte_iof.close(proc, PRTE_IOF_STDALL);
        }
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_WAITPID) &&
            !PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_RECORDED)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_WAITPID_FIRED == state) {
        /* do NOT update the proc state as this can hit
         * while we are still trying to notify the HNP of
         * successful launch for short-lived procs
         */
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_WAITPID);
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_IOF_COMPLETE) &&
            !PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_RECORDED)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_TERMINATED == state) {
        /* if this proc has not already recorded as terminated, then
         * update the accounting here */
        if (!PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_RECORDED)) {
            jdata->num_terminated++;
        }
        /* update the proc state */
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_RECORDED);
        PRTE_FLAG_UNSET(pdata, PRTE_PROC_FLAG_ALIVE);
        pdata->state = state;
        /* Clean up the session directory as if we were the process
         * itself.  This covers the case where the process died abnormally
         * and didn't cleanup its own session directory.
         */
        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
            !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
            prte_session_dir_finalize(proc);
        }
        /* if we are trying to terminate and our routes are
         * gone, then terminate ourselves IF no local procs
         * remain (might be some from another job)
         */
        if (prte_prteds_term_ordered &&
            0 == prte_routed.num_routes()) {
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL != (pdata = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i)) &&
                    PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                         "%s state:prted all routes gone but proc %s still alive",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&pdata->name)));
                    goto cleanup;
                }
            }
            /* call our appropriate exit procedure */
            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                 "%s state:prted all routes and children gone - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            goto cleanup;
        }
        /* track job status */
        if (jdata->num_terminated == jdata->num_local_procs &&
            !prte_get_attribute(&jdata->attributes, PRTE_JOB_TERM_NOTIFIED, NULL, PMIX_BOOL)) {
            /* pack update state command */
            cmd = PRTE_PLM_UPDATE_PROC_STATE;
            PMIX_DATA_BUFFER_CREATE(alert);
            rc = PMIx_Data_pack(NULL, alert, &cmd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* pack the job info */
            if (PRTE_SUCCESS != (rc = pack_state_update(alert, jdata))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(alert);
                goto cleanup;
            }
            /* send it */
            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                 "%s state:prted: SENDING JOB LOCAL TERMINATION UPDATE FOR JOB %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(jdata->nspace)));
            if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, alert,
                                                  PRTE_RML_TAG_PLM,
                                                  prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(rc);
            }
            /* mark that we sent it so we ensure we don't do it again */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TERM_NOTIFIED, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
            /* cleanup the procs as these are gone */
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL == (pptr = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
                    continue;
                }
                /* if this child is part of the job... */
                if (PMIX_CHECK_NSPACE(pptr->name.nspace, jdata->nspace)) {
                    /* clear the entry in the local children */
                    prte_pointer_array_set_item(prte_local_children, i, NULL);
                    PRTE_RELEASE(pptr);  // maintain accounting
                }
            }
            /* tell the IOF that the job is complete */
            if (NULL != prte_iof.complete) {
                prte_iof.complete(jdata);
            }

            /* tell the PMIx subsystem the job is complete */
            PRTE_PMIX_CONSTRUCT_LOCK(&lock);
            PMIx_server_deregister_nspace(jdata->nspace, opcbfunc, &lock);
            PRTE_PMIX_WAIT_THREAD(&lock);
            PRTE_PMIX_DESTRUCT_LOCK(&lock);

            /* release the resources */
            if (NULL != jdata->map) {
                map = jdata->map;
                for (index = 0; index < map->nodes->size; index++) {
                    if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, index))) {
                        continue;
                    }
                    PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                         "%s state:prted releasing procs from node %s",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         node->name));
                    for (i = 0; i < node->procs->size; i++) {
                        if (NULL == (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                            continue;
                        }
                        if (!PMIX_CHECK_NSPACE(pptr->name.nspace, jdata->nspace)) {
                            /* skip procs from another job */
                            continue;
                        }
                        if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
                            !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                            node->slots_inuse--;
                            node->num_procs--;
                        }
                        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                             "%s state:prted releasing proc %s from node %s",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                             PRTE_NAME_PRINT(&pptr->name), node->name));
                        /* set the entry in the node array to NULL */
                        prte_pointer_array_set_item(node->procs, i, NULL);
                        /* release the proc once for the map entry */
                        PRTE_RELEASE(pptr);
                    }
                    /* set the node location to NULL */
                    prte_pointer_array_set_item(map->nodes, index, NULL);
                    /* maintain accounting */
                    PRTE_RELEASE(node);
                    /* flag that the node is no longer in a map */
                    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
                }
                PRTE_RELEASE(map);
                jdata->map = NULL;
            }

            /* if requested, check fd status for leaks */
            if (prte_state_base_run_fdcheck) {
                prte_state_base_check_fds(jdata);
            }

            /* if ompi-server is around, then notify it to purge
             * any session-related info */
            if (NULL != prte_data_server_uri) {
                PMIX_LOAD_PROCID(&target, jdata->nspace, PMIX_RANK_WILDCARD);
                prte_state_base_notify_data_server(&target);
            }

            /* cleanup the job info */
            prte_pointer_array_set_item(prte_job_data, jdata->index, NULL);
            PRTE_RELEASE(jdata);
        }
    }

  cleanup:
    PRTE_RELEASE(caddy);
}

static int pack_state_for_proc(pmix_data_buffer_t *alert, prte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    rc = PMIx_Data_pack(NULL, alert, &child->name.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
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

static int pack_state_update(pmix_data_buffer_t *alert, prte_job_t *jdata)
{
    int i, rc;
    prte_proc_t *child;
    pmix_rank_t null=PMIX_RANK_INVALID;

    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, alert, &jdata->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    for (i=0; i < prte_local_children->size; i++) {
        if (NULL == (child = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (PMIX_CHECK_NSPACE(child->name.nspace, jdata->nspace)) {
            if (PRTE_SUCCESS != (rc = pack_state_for_proc(alert, child))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    /* flag that this job is complete so the receiver can know */
    rc = PMIx_Data_pack(NULL, alert, &null, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}
