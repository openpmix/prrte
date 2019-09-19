/*
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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
#include "src/dss/dss.h"
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
#include "src/runtime/prrte_data_server.h"
#include "src/runtime/prrte_quit.h"

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
 * PRRTED module
 ******************/
prrte_state_base_module_t prrte_state_prted_module = {
    init,
    finalize,
    prrte_state_base_activate_job_state,
    prrte_state_base_add_job_state,
    prrte_state_base_set_job_state_callback,
    prrte_state_base_set_job_state_priority,
    prrte_state_base_remove_job_state,
    prrte_state_base_activate_proc_state,
    prrte_state_base_add_proc_state,
    prrte_state_base_set_proc_state_callback,
    prrte_state_base_set_proc_state_priority,
    prrte_state_base_remove_proc_state
};

/* Local functions */
static void track_jobs(int fd, short argc, void *cbdata);
static void track_procs(int fd, short argc, void *cbdata);
static int pack_state_update(prrte_buffer_t *buf, prrte_job_t *jdata);

/* defined default state machines */
static prrte_job_state_t job_states[] = {
    PRRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE,
};
static prrte_state_cbfunc_t job_callbacks[] = {
    track_jobs
};

static prrte_proc_state_t proc_states[] = {
    PRRTE_PROC_STATE_RUNNING,
    PRRTE_PROC_STATE_REGISTERED,
    PRRTE_PROC_STATE_IOF_COMPLETE,
    PRRTE_PROC_STATE_WAITPID_FIRED,
    PRRTE_PROC_STATE_TERMINATED
};
static prrte_state_cbfunc_t proc_callbacks[] = {
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
    PRRTE_CONSTRUCT(&prrte_job_states, prrte_list_t);
    PRRTE_CONSTRUCT(&prrte_proc_states, prrte_list_t);

    num_states = sizeof(job_states) / sizeof(prrte_job_state_t);
    for (i=0; i < num_states; i++) {
        if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(job_states[i],
                                                           job_callbacks[i],
                                                           PRRTE_SYS_PRI))) {
            PRRTE_ERROR_LOG(rc);
        }
    }
    /* add a default error response */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_FORCED_EXIT,
                                                       prrte_quit, PRRTE_ERROR_PRI))) {
        PRRTE_ERROR_LOG(rc);
    }
    /* add a state for when we are ordered to terminate */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_DAEMONS_TERMINATED,
                                                       prrte_quit, PRRTE_SYS_PRI))) {
        PRRTE_ERROR_LOG(rc);
    }
    if (5 < prrte_output_get_verbosity(prrte_state_base_framework.framework_output)) {
        prrte_state_base_print_job_state_machine();
    }

    /* populate the proc state machine to allow us to
     * track proc lifecycle changes
     */
    num_states = sizeof(proc_states) / sizeof(prrte_proc_state_t);
    for (i=0; i < num_states; i++) {
        if (PRRTE_SUCCESS != (rc = prrte_state.add_proc_state(proc_states[i],
                                                            proc_callbacks[i],
                                                            PRRTE_SYS_PRI))) {
            PRRTE_ERROR_LOG(rc);
        }
    }
    if (5 < prrte_output_get_verbosity(prrte_state_base_framework.framework_output)) {
        prrte_state_base_print_proc_state_machine();
    }
    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    prrte_list_item_t *item;

    /* cleanup the state machines */
    while (NULL != (item = prrte_list_remove_first(&prrte_job_states))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_job_states);
    while (NULL != (item = prrte_list_remove_first(&prrte_proc_states))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_proc_states);

    return PRRTE_SUCCESS;
}

static void track_jobs(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_buffer_t *alert;
    prrte_plm_cmd_flag_t cmd;
    int rc, i;
    prrte_proc_state_t running = PRRTE_PROC_STATE_RUNNING;
    prrte_proc_t *child;
    prrte_vpid_t null=PRRTE_VPID_INVALID;

    PRRTE_ACQUIRE_OBJECT(caddy);

    if (PRRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE == caddy->job_state) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                            "%s state:prted:track_jobs sending local launch complete for job %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_JOBID_PRINT(caddy->jdata->jobid)));
        /* update the HNP with all proc states for this job */
       alert = PRRTE_NEW(prrte_buffer_t);
         /* pack update state command */
        cmd = PRRTE_PLM_UPDATE_PROC_STATE;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(alert);
            goto cleanup;
        }
        /* pack the jobid */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &caddy->jdata->jobid, 1, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(alert);
            goto cleanup;
        }
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                continue;
            }
            /* if this child is part of the job... */
            if (child->name.jobid == caddy->jdata->jobid) {
                /* pack the child's vpid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &(child->name.vpid), 1, PRRTE_VPID))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(alert);
                    goto cleanup;
                }
                /* pack the pid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->pid, 1, PRRTE_PID))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(alert);
                    goto cleanup;
                }
                /* if this proc failed to start, then send that info */
                if (PRRTE_PROC_STATE_UNTERMINATED < child->state) {
                    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->state, 1, PRRTE_PROC_STATE))) {
                        PRRTE_ERROR_LOG(rc);
                        PRRTE_RELEASE(alert);
                        goto cleanup;
                    }
                } else {
                    /* pack the RUNNING state to avoid any race conditions */
                    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &running, 1, PRRTE_PROC_STATE))) {
                        PRRTE_ERROR_LOG(rc);
                        PRRTE_RELEASE(alert);
                        goto cleanup;
                    }
                }
                /* pack its exit code */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->exit_code, 1, PRRTE_EXIT_CODE))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(alert);
                    goto cleanup;
                }
            }
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
        }
    }

  cleanup:
    PRRTE_RELEASE(caddy);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_POST_OBJECT(lk);
    lk->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lk);
}
static void track_procs(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_process_name_t *proc;
    prrte_proc_state_t state;
    prrte_job_t *jdata;
    prrte_proc_t *pdata, *pptr;
    prrte_buffer_t *alert;
    int rc, i;
    prrte_plm_cmd_flag_t cmd;
    prrte_std_cntr_t index;
    prrte_job_map_t *map;
    prrte_node_t *node;
    prrte_process_name_t target;
    pmix_proc_t pname;
    prrte_pmix_lock_t lock;

    PRRTE_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                         "%s state:prted:track_procs called for proc %s state %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc),
                         prrte_proc_state_to_str(state)));

    /* get the job object for this proc */
    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        goto cleanup;
    }
    pdata = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid);

    if (PRRTE_PROC_STATE_RUNNING == state) {
        /* update the proc state */
        pdata->state = state;
        jdata->num_launched++;
        if (jdata->num_launched == jdata->num_local_procs) {
            /* tell the state machine that all local procs for this job
             * were launched so that it can do whatever it needs to do,
             * like send a state update message for all procs to the HNP
             */
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE);
        }
        /* don't update until we are told that all are done */
    } else if (PRRTE_PROC_STATE_REGISTERED == state) {
        /* update the proc state */
        pdata->state = state;
        jdata->num_reported++;
        if (jdata->num_reported == jdata->num_local_procs) {
            /* once everyone registers, notify the HNP */

            PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                 "%s state:prted: notifying HNP all local registered",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

            alert = PRRTE_NEW(prrte_buffer_t);
            /* pack registered command */
            cmd = PRRTE_PLM_REGISTERED_CMD;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* pack the jobid */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &proc->jobid, 1, PRRTE_JOBID))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* pack all the local child vpids */
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                    continue;
                }
                if (pptr->name.jobid == proc->jobid) {
                    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &pptr->name.vpid, 1, PRRTE_VPID))) {
                        PRRTE_ERROR_LOG(rc);
                        goto cleanup;
                    }
                }
            }
            /* send it */
            if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                                  PRRTE_RML_TAG_PLM,
                                                  prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
            } else {
                rc = PRRTE_SUCCESS;
            }
        }
    } else if (PRRTE_PROC_STATE_IOF_COMPLETE == state) {
        /* do NOT update the proc state as this can hit
         * while we are still trying to notify the HNP of
         * successful launch for short-lived procs
         */
        PRRTE_FLAG_SET(pdata, PRRTE_PROC_FLAG_IOF_COMPLETE);
        /* Release the stdin IOF file descriptor for this child, if one
         * was defined. File descriptors for the other IOF channels - stdout,
         * stderr, and stddiag - were released when their associated pipes
         * were cleared and closed due to termination of the process
         * Do this after we handle termination in case the IOF needs
         * to check to see if all procs from the job are actually terminated
         */
        if (NULL != prrte_iof.close) {
            prrte_iof.close(proc, PRRTE_IOF_STDALL);
        }
        if (PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_WAITPID) &&
            !PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_RECORDED)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRRTE_PROC_STATE_WAITPID_FIRED == state) {
        /* do NOT update the proc state as this can hit
         * while we are still trying to notify the HNP of
         * successful launch for short-lived procs
         */
        PRRTE_FLAG_SET(pdata, PRRTE_PROC_FLAG_WAITPID);
        if (PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
            !PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_RECORDED)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRRTE_PROC_STATE_TERMINATED == state) {
        /* if this proc has not already recorded as terminated, then
         * update the accounting here */
        if (!PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_RECORDED)) {
            jdata->num_terminated++;
        }
        /* update the proc state */
        PRRTE_FLAG_SET(pdata, PRRTE_PROC_FLAG_RECORDED);
        PRRTE_FLAG_UNSET(pdata, PRRTE_PROC_FLAG_ALIVE);
        pdata->state = state;
        /* Clean up the session directory as if we were the process
         * itself.  This covers the case where the process died abnormally
         * and didn't cleanup its own session directory.
         */
        prrte_session_dir_finalize(proc);
        /* if we are trying to terminate and our routes are
         * gone, then terminate ourselves IF no local procs
         * remain (might be some from another job)
         */
        if (prrte_prteds_term_ordered &&
            0 == prrte_routed.num_routes()) {
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL != (pdata = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                    PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                         "%s state:prted all routes gone but proc %s still alive",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         PRRTE_NAME_PRINT(&pdata->name)));
                    goto cleanup;
                }
            }
            /* call our appropriate exit procedure */
            PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                 "%s state:prted all routes and children gone - exiting",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            goto cleanup;
        }
        /* track job status */
        if (jdata->num_terminated == jdata->num_local_procs &&
            !prrte_get_attribute(&jdata->attributes, PRRTE_JOB_TERM_NOTIFIED, NULL, PRRTE_BOOL)) {
            /* pack update state command */
            cmd = PRRTE_PLM_UPDATE_PROC_STATE;
            alert = PRRTE_NEW(prrte_buffer_t);
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* pack the job info */
            if (PRRTE_SUCCESS != (rc = pack_state_update(alert, jdata))) {
                PRRTE_ERROR_LOG(rc);
            }
            /* send it */
            PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                 "%s state:prted: SENDING JOB LOCAL TERMINATION UPDATE FOR JOB %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(jdata->jobid)));
            if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, alert,
                                                  PRRTE_RML_TAG_PLM,
                                                  prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
            }
            /* mark that we sent it so we ensure we don't do it again */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_TERM_NOTIFIED, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);
            /* cleanup the procs as these are gone */
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                    continue;
                }
                /* if this child is part of the job... */
                if (pptr->name.jobid == jdata->jobid) {
                    /* clear the entry in the local children */
                    prrte_pointer_array_set_item(prrte_local_children, i, NULL);
                    PRRTE_RELEASE(pptr);  // maintain accounting
                }
            }
            /* tell the IOF that the job is complete */
            if (NULL != prrte_iof.complete) {
                prrte_iof.complete(jdata);
            }

            /* tell the PMIx subsystem the job is complete */
            PRRTE_PMIX_CONVERT_JOBID(pname.nspace, jdata->jobid);
            PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
            PMIx_server_deregister_nspace(pname.nspace, opcbfunc, &lock);
            PRRTE_PMIX_WAIT_THREAD(&lock);
            PRRTE_PMIX_DESTRUCT_LOCK(&lock);

            /* release the resources */
            if (NULL != jdata->map) {
                map = jdata->map;
                for (index = 0; index < map->nodes->size; index++) {
                    if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, index))) {
                        continue;
                    }
                    PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                         "%s state:prted releasing procs from node %s",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         node->name));
                    for (i = 0; i < node->procs->size; i++) {
                        if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                            continue;
                        }
                        if (pptr->name.jobid != jdata->jobid) {
                            /* skip procs from another job */
                            continue;
                        }
                        if (!PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_TOOL)) {
                            node->slots_inuse--;
                            node->num_procs--;
                        }
                        PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                             "%s state:prted releasing proc %s from node %s",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             PRRTE_NAME_PRINT(&pptr->name), node->name));
                        /* set the entry in the node array to NULL */
                        prrte_pointer_array_set_item(node->procs, i, NULL);
                        /* release the proc once for the map entry */
                        PRRTE_RELEASE(pptr);
                    }
                    /* set the node location to NULL */
                    prrte_pointer_array_set_item(map->nodes, index, NULL);
                    /* maintain accounting */
                    PRRTE_RELEASE(node);
                    /* flag that the node is no longer in a map */
                    PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
                }
                PRRTE_RELEASE(map);
                jdata->map = NULL;
            }

            /* if requested, check fd status for leaks */
            if (prrte_state_base_run_fdcheck) {
                prrte_state_base_check_fds(jdata);
            }

            /* if ompi-server is around, then notify it to purge
             * any session-related info */
            if (NULL != prrte_data_server_uri) {
                target.jobid = jdata->jobid;
                target.vpid = PRRTE_VPID_WILDCARD;
                prrte_state_base_notify_data_server(&target);
            }

            /* cleanup the job info */
            prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, NULL);
            PRRTE_RELEASE(jdata);
        }
    }

  cleanup:
    PRRTE_RELEASE(caddy);
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

static int pack_state_update(prrte_buffer_t *alert, prrte_job_t *jdata)
{
    int i, rc;
    prrte_proc_t *child;
    prrte_vpid_t null=PRRTE_VPID_INVALID;

    /* pack the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &jdata->jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (child->name.jobid == jdata->jobid) {
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
