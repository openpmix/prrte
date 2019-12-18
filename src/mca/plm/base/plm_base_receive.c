/* -*- C -*-
 *
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */

/*
 * includes
 */
#include "prrte_config.h"

#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "src/mca/mca.h"
#include "src/dss/dss.h"
#include "src/threads/threads.h"
#include "src/util/argv.h"
#include "src/util/prrte_environ.h"

#include "constants.h"
#include "types.h"
#include "src/util/proc_info.h"
#include "src/util/error_strings.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/ras/base/base.h"
#include "src/util/name_fns.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_quit.h"

#include "src/mca/plm/plm_types.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/base/base.h"

static bool recv_issued=false;

int prrte_plm_base_comm_start(void)
{
    if (recv_issued) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:receive start comm",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_PLM,
                            PRRTE_RML_PERSISTENT,
                            prrte_plm_base_recv,
                            NULL);
    if (PRRTE_PROC_IS_MASTER) {
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                                PRRTE_RML_TAG_PRRTED_CALLBACK,
                                PRRTE_RML_PERSISTENT,
                                prrte_plm_base_daemon_callback, NULL);
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                                PRRTE_RML_TAG_REPORT_REMOTE_LAUNCH,
                                PRRTE_RML_PERSISTENT,
                                prrte_plm_base_daemon_failed, NULL);
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                                PRRTE_RML_TAG_TOPOLOGY_REPORT,
                                PRRTE_RML_PERSISTENT,
                                prrte_plm_base_daemon_topology, NULL);
    }
    recv_issued = true;

    return PRRTE_SUCCESS;
}


int prrte_plm_base_comm_stop(void)
{
    if (!recv_issued) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:receive stop comm",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_PLM);
    if (PRRTE_PROC_IS_MASTER) {
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_PRRTED_CALLBACK);
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_REPORT_REMOTE_LAUNCH);
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_TOPOLOGY_REPORT);
    }
    recv_issued = false;

    return PRRTE_SUCCESS;
}


/* process incoming messages in order of receipt */
void prrte_plm_base_recv(int status, prrte_process_name_t* sender,
                        prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                        void* cbdata)
{
    prrte_plm_cmd_flag_t command;
    prrte_std_cntr_t count;
    prrte_jobid_t job;
    prrte_job_t *jdata, *parent, jb;
    prrte_buffer_t *answer;
    prrte_vpid_t vpid;
    prrte_proc_t *proc;
    prrte_proc_state_t state;
    prrte_exit_code_t exit_code;
    int32_t rc=PRRTE_SUCCESS, ret;
    prrte_app_context_t *app, *child_app;
    prrte_process_name_t name, *nptr;
    pid_t pid;
    bool running;
    int i, room;
    char **env;
    char *prefix_dir;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:receive processing msg",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &command, &count, PRRTE_PLM_CMD))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEANUP;
    }

    switch (command) {
    case PRRTE_PLM_ALLOC_JOBID_CMD:
        /* set default return value */
        job = PRRTE_JOBID_INVALID;

        /* unpack the room number of the request so we can return it to them */
        count = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &room, &count, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            goto CLEANUP;
        }
        /* get the new jobid */
        PRRTE_CONSTRUCT(&jb, prrte_job_t);
        rc = prrte_plm_base_create_jobid(&jb);
        if (PRRTE_SUCCESS == rc) {
            job = jb.jobid;
        }
        PRRTE_DESTRUCT(&jb);

        /* setup the response */
        answer = PRRTE_NEW(prrte_buffer_t);

        /* pack the status to be returned */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &rc, 1, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
        }

        /* pack the jobid */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &job, 1, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(ret);
        }

        /* pack the room number of the request */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &room, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(ret);
        }

        /* send the response back to the sender */
        if (0 > (ret = prrte_rml.send_buffer_nb(sender, answer, PRRTE_RML_TAG_LAUNCH_RESP,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
        }
        break;

    case PRRTE_PLM_LAUNCH_JOB_CMD:
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:receive job launch command from %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(sender)));

        /* unpack the job object */
        count = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &jdata, &count, PRRTE_JOB))) {
            PRRTE_ERROR_LOG(rc);
            goto ANSWER_LAUNCH;
        }

        /* record the sender so we know who to respond to */
        jdata->originator.jobid = sender->jobid;
        jdata->originator.vpid = sender->vpid;

        /* get the name of the actual spawn parent - i.e., the proc that actually
         * requested the spawn */
        nptr = &name;
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_LAUNCH_PROXY, (void**)&nptr, PRRTE_NAME)) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            rc = PRRTE_ERR_NOT_FOUND;
            goto ANSWER_LAUNCH;
        }

        /* get the parent's job object */
        if (NULL != (parent = prrte_get_job_data_object(name.jobid))) {
            /* if the prefix was set in the parent's job, we need to transfer
             * that prefix to the child's app_context so any further launch of
             * orteds can find the correct binary. There always has to be at
             * least one app_context in both parent and child, so we don't
             * need to check that here. However, be sure not to overwrite
             * the prefix if the user already provided it!
             */
            app = (prrte_app_context_t*)prrte_pointer_array_get_item(parent->apps, 0);
            child_app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0);
            prefix_dir = NULL;
            if (prrte_get_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&prefix_dir, PRRTE_STRING) &&
                !prrte_get_attribute(&child_app->attributes, PRRTE_APP_PREFIX_DIR, NULL, PRRTE_STRING)) {
                prrte_set_attribute(&child_app->attributes, PRRTE_APP_PREFIX_DIR, PRRTE_ATTR_GLOBAL, prefix_dir, PRRTE_STRING);
            }
            if (NULL != prefix_dir) {
                free(prefix_dir);
            }
            /* link the spawned job to the spawner */
            PRRTE_RETAIN(jdata);
            prrte_list_append(&parent->children, &jdata->super);
            /* connect the launcher as well */
            if (PRRTE_JOBID_INVALID == parent->launcher) {
                /* we are an original spawn */
                jdata->launcher = name.jobid;
            } else {
                jdata->launcher = parent->launcher;
            }
        }

        /* if the user asked to forward any envars, cycle through the app contexts
         * in the comm_spawn request and add them
         */
        if (NULL != prrte_forwarded_envars) {
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                env = prrte_environ_merge(prrte_forwarded_envars, app->env);
                prrte_argv_free(app->env);
                app->env = env;
            }
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:receive adding hosts",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

        /* process any add-hostfile and add-host options that were provided */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_add_hosts(jdata))) {
            PRRTE_ERROR_LOG(rc);
            goto ANSWER_LAUNCH;
        }

        if (NULL != parent) {
            if (NULL == parent->bookmark) {
                /* find the sender's node in the job map */
                if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(parent->procs, sender->vpid))) {
                    /* set the bookmark so the child starts from that place - this means
                     * that the first child process could be co-located with the proc
                     * that called comm_spawn, assuming slots remain on that node. Otherwise,
                     * the procs will start on the next available node
                     */
                    jdata->bookmark = proc->node;
                }
            } else {
                jdata->bookmark = parent->bookmark;
            }
            /* provide the parent's last object */
            jdata->bkmark_obj = parent->bkmark_obj;
        }

        /* launch it */
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:receive calling spawn",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        if (PRRTE_SUCCESS != (rc = prrte_plm.spawn(jdata))) {
            PRRTE_ERROR_LOG(rc);
            goto ANSWER_LAUNCH;
        }
        break;
    ANSWER_LAUNCH:
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:receive - error on launch: %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), rc));

        /* setup the response */
        answer = PRRTE_NEW(prrte_buffer_t);

        /* pack the error code to be returned */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &rc, 1, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
        }

        /* pack an invalid jobid */
        job = PRRTE_JOBID_INVALID;
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &job, 1, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(ret);
        }
        /* pack the room number of the request */
        if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_ROOM_NUM, (void**)&room, PRRTE_INT)) {
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &room, 1, PRRTE_INT))) {
                PRRTE_ERROR_LOG(ret);
            }
        }

        /* send the response back to the sender */
        if (0 > (ret = prrte_rml.send_buffer_nb(sender, answer, PRRTE_RML_TAG_LAUNCH_RESP,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
        }
        break;

    case PRRTE_PLM_UPDATE_PROC_STATE:
        prrte_output_verbose(5, prrte_plm_base_framework.framework_output,
                            "%s plm:base:receive update proc state command from %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(sender));
        count = 1;
        while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(buffer, &job, &count, PRRTE_JOBID))) {

            prrte_output_verbose(5, prrte_plm_base_framework.framework_output,
                                "%s plm:base:receive got update_proc_state for job %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_JOBID_PRINT(job));

            name.jobid = job;
            running = false;
            /* get the job object */
            jdata = prrte_get_job_data_object(job);
            count = 1;
            while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(buffer, &vpid, &count, PRRTE_VPID))) {
                if (PRRTE_VPID_INVALID == vpid) {
                    /* flag indicates that this job is complete - move on */
                    break;
                }
                name.vpid = vpid;
                /* unpack the pid */
                count = 1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &pid, &count, PRRTE_PID))) {
                    PRRTE_ERROR_LOG(rc);
                    goto CLEANUP;
                }
                /* unpack the state */
                count = 1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &state, &count, PRRTE_PROC_STATE))) {
                    PRRTE_ERROR_LOG(rc);
                    goto CLEANUP;
                }
                if (PRRTE_PROC_STATE_RUNNING == state) {
                    running = true;
                }
                /* unpack the exit code */
                count = 1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &exit_code, &count, PRRTE_EXIT_CODE))) {
                    PRRTE_ERROR_LOG(rc);
                    goto CLEANUP;
                }

                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s plm:base:receive got update_proc_state for vpid %lu state %s exit_code %d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     (unsigned long)vpid, prrte_proc_state_to_str(state), (int)exit_code));

                if (NULL != jdata) {
                    /* get the proc data object */
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, vpid))) {
                        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                        goto CLEANUP;
                    }
                    /* NEVER update the proc state before activating the state machine - let
                     * the state cbfunc update it as it may need to compare this
                     * state against the prior proc state */
                    proc->pid = pid;
                    proc->exit_code = exit_code;
                    PRRTE_ACTIVATE_PROC_STATE(&name, state);
                }
            }
            /* record that we heard back from a daemon during app launch */
            if (running && NULL != jdata) {
                jdata->num_daemons_reported++;
                if (prrte_report_launch_progress) {
                    if (0 == jdata->num_daemons_reported % 100 ||
                        jdata->num_daemons_reported == prrte_process_info.num_procs) {
                        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_REPORT_PROGRESS);
                    }
                }
            }
            /* prepare for next job */
            count = 1;
        }
        if (PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PRRTE_ERROR_LOG(rc);
        } else {
            rc = PRRTE_SUCCESS;
        }
        break;

    case PRRTE_PLM_REGISTERED_CMD:
        count=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &job, &count, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            goto CLEANUP;
        }
        name.jobid = job;
        /* get the job object */
        if (NULL == (jdata = prrte_get_job_data_object(job))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            rc = PRRTE_ERR_NOT_FOUND;
            goto CLEANUP;
        }
        count=1;
        while (PRRTE_SUCCESS == prrte_dss.unpack(buffer, &vpid, &count, PRRTE_VPID)) {
            name.vpid = vpid;
            PRRTE_ACTIVATE_PROC_STATE(&name, PRRTE_PROC_STATE_REGISTERED);
            count=1;
        }
        break;

    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_VALUE_OUT_OF_BOUNDS);
        rc = PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        break;
    }

  CLEANUP:
    /* see if an error occurred - if so, wakeup the HNP so we can exit */
    if (PRRTE_PROC_IS_MASTER && PRRTE_SUCCESS != rc) {
        jdata = NULL;
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:receive done processing commands",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
}

/* where HNP messages come */
void prrte_plm_base_receive_process_msg(int fd, short event, void *data)
{
    assert(0);
}
