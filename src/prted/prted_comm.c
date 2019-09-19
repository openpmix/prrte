/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2016 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009      Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>


#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/util/path.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/compress/compress.h"

#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"

#include "src/mca/odls/base/odls_private.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_quit.h"

#include "src/prted/prted.h"

/*
 * Globals
 */
static char *get_prted_comm_cmd_str(int command);

static void _notify_release(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_PMIX_WAKEUP_THREAD(lk);
}

static prrte_pointer_array_t *procs_prev_ordered_to_terminate = NULL;

void prrte_daemon_recv(int status, prrte_process_name_t* sender,
                      prrte_buffer_t *buffer, prrte_rml_tag_t tag,
                      void* cbdata)
{
    prrte_daemon_cmd_flag_t command;
    prrte_buffer_t *relay_msg;
    int ret;
    prrte_std_cntr_t n;
    int32_t signal, cnt;
    prrte_jobid_t job;
    prrte_buffer_t data, *answer;
    prrte_job_t *jdata;
    prrte_process_name_t proc;
    int32_t i, num_replies;
    prrte_pointer_array_t procarray;
    prrte_proc_t *proct;
    char *cmd_str = NULL;
    prrte_pointer_array_t *procs_to_kill = NULL;
    prrte_std_cntr_t num_procs, num_new_procs = 0, p;
    prrte_proc_t *cur_proc = NULL, *prev_proc = NULL;
    bool found = false;
    prrte_node_t *node;
    FILE *fp;
    char gscmd[256], path[1035], *pathptr;
    char string[256], *string_ptr = string;
    float pss;
    prrte_pstats_t pstat;
    char *coprocessors;
    prrte_job_map_t *map;
    int8_t flag;
    uint8_t *cmpdata;
    size_t cmplen;
    uint32_t u32;
    void *nptr;
    prrte_pmix_lock_t lk;
    pmix_data_buffer_t pbkt;
    pmix_proc_t pdmn, pname;
    prrte_byte_object_t *bo, *bo2;
    prrte_process_name_t dmn;
    pmix_status_t pstatus;
    pmix_info_t *info;
    size_t n2, ninfo;
    prrte_buffer_t wireup;

    /* unpack the command */
    n = 1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &command, &n, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(ret);
        return;
    }

    cmd_str = get_prted_comm_cmd_str(command);
    PRRTE_OUTPUT_VERBOSE((1, prrte_debug_output,
                         "%s prted:comm:process_commands() Processing Command: %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), cmd_str));
    free(cmd_str);
    cmd_str = NULL;

    /* now process the command locally */
    switch(command) {

        /****    NULL    ****/
    case PRRTE_DAEMON_NULL_CMD:
        ret = PRRTE_SUCCESS;
        break;

        /****    KILL_LOCAL_PROCS   ****/
    case PRRTE_DAEMON_KILL_LOCAL_PROCS:
        num_replies = 0;

        /* construct the pointer array */
        PRRTE_CONSTRUCT(&procarray, prrte_pointer_array_t);
        prrte_pointer_array_init(&procarray, num_replies, PRRTE_GLOBAL_ARRAY_MAX_SIZE, 16);

        /* unpack the proc names into the array */
        while (PRRTE_SUCCESS == (ret = prrte_dss.unpack(buffer, &proc, &n, PRRTE_NAME))) {
            proct = PRRTE_NEW(prrte_proc_t);
            proct->name.jobid = proc.jobid;
            proct->name.vpid = proc.vpid;

            prrte_pointer_array_add(&procarray, proct);
            num_replies++;
        }
        if (PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
            PRRTE_ERROR_LOG(ret);
            goto KILL_PROC_CLEANUP;
        }

        if (0 == num_replies) {
            /* kill everything */
            if (PRRTE_SUCCESS != (ret = prrte_odls.kill_local_procs(NULL))) {
                PRRTE_ERROR_LOG(ret);
            }
            break;
        } else {
            /* kill the procs */
            if (PRRTE_SUCCESS != (ret = prrte_odls.kill_local_procs(&procarray))) {
                PRRTE_ERROR_LOG(ret);
            }
        }

        /* cleanup */
    KILL_PROC_CLEANUP:
        for (i=0; i < procarray.size; i++) {
            if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(&procarray, i))) {
                free(proct);
            }
        }
        PRRTE_DESTRUCT(&procarray);
        break;

        /****    SIGNAL_LOCAL_PROCS   ****/
    case PRRTE_DAEMON_SIGNAL_LOCAL_PROCS:
        /* unpack the jobid */
        n = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &job, &n, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* look up job data object */
        jdata = prrte_get_job_data_object(job);

        /* get the signal */
        n = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &signal, &n, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* Convert SIGTSTP to SIGSTOP so we can suspend a.out */
        if (SIGTSTP == signal) {
            if (prrte_debug_daemons_flag) {
                prrte_output(0, "%s prted_cmd: converted SIGTSTP to SIGSTOP before delivering",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            }
            signal = SIGSTOP;
            if (NULL != jdata) {
                jdata->state |= PRRTE_JOB_STATE_SUSPENDED;
            }
        } else if (SIGCONT == signal && NULL != jdata) {
            jdata->state &= ~PRRTE_JOB_STATE_SUSPENDED;
        }

        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received signal_local_procs, delivering signal %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        signal);
        }

        /* signal them */
        if (PRRTE_SUCCESS != (ret = prrte_odls.signal_local_procs(NULL, signal))) {
            PRRTE_ERROR_LOG(ret);
        }
        break;

    case PRRTE_DAEMON_PASS_NODE_INFO_CMD:
        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received pass_node_info",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }
        if (!PRRTE_PROC_IS_MASTER) {
            if (PRRTE_SUCCESS != (ret = prrte_util_decode_nidmap(buffer))) {
                PRRTE_ERROR_LOG(ret);
                goto CLEANUP;
            }
            if (PRRTE_SUCCESS != (ret = prrte_util_parse_node_info(buffer))) {
                PRRTE_ERROR_LOG(ret);
                goto CLEANUP;
            }
            /* unpack the wireup byte object */
            cnt=1;
            if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &bo, &cnt, PRRTE_BYTE_OBJECT))) {
                PRRTE_ERROR_LOG(ret);
                goto CLEANUP;
            }
            if (0 < bo->size) {
                PRRTE_PMIX_CONVERT_NAME(&pname, PRRTE_PROC_MY_NAME);
                /* load it into a buffer */
                PRRTE_CONSTRUCT(&wireup, prrte_buffer_t);
                prrte_dss.load(&wireup, bo->bytes, bo->size);
                cnt=1;
                while (PRRTE_SUCCESS == (ret = prrte_dss.unpack(&wireup, &dmn, &cnt, PRRTE_NAME))) {
                    /* unpack the byte object containing the contact info */
                    cnt = 1;
                    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(&wireup, &bo2, &cnt, PRRTE_BYTE_OBJECT))) {
                        PRRTE_ERROR_LOG(ret);
                        break;
                    }
                    /* load into a PMIx buffer for unpacking */
                    PMIX_DATA_BUFFER_LOAD(&pbkt, bo2->bytes, bo2->size);
                    /* unpack the number of info's provided */
                    cnt = 1;
                    if (PMIX_SUCCESS != (pstatus = PMIx_Data_unpack(&pname, &pbkt, &ninfo, &cnt, PMIX_SIZE))) {
                        PMIX_ERROR_LOG(pstatus);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        ret = PRRTE_ERR_UNPACK_FAILURE;
                        break;
                    }
                    /* unpack the infos */
                    PMIX_INFO_CREATE(info, ninfo);
                    cnt = ninfo;
                    if (PMIX_SUCCESS != (pstatus = PMIx_Data_unpack(&pname, &pbkt, info, &cnt, PMIX_INFO))) {
                        PMIX_ERROR_LOG(pstatus);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        PMIX_INFO_FREE(info, ninfo);
                        ret = PRRTE_ERR_UNPACK_FAILURE;
                        break;
                    }

                    /* store them locally */
                    PRRTE_PMIX_CONVERT_NAME(&pdmn, &dmn);
                    for (n2=0; n2 < ninfo; n2++) {
                        pstatus = PMIx_Store_internal(&pdmn, info[n2].key, &info[n2].value);
                        if (PMIX_SUCCESS != pstatus) {
                            PMIX_ERROR_LOG(pstatus);
                            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                            PMIX_INFO_FREE(info, ninfo);
                            ret = PRRTE_ERR_UNPACK_FAILURE;
                            goto CLEANUP;
                        }
                    }
                    PMIX_INFO_FREE(info, ninfo);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                }
                if (PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
                    PRRTE_ERROR_LOG(ret);
                }
                /* done with the wireup buffer - dump it */
                PRRTE_DESTRUCT(&wireup);
            }
            free(bo);
        }
        break;


        /****    ADD_LOCAL_PROCS   ****/
    case PRRTE_DAEMON_ADD_LOCAL_PROCS:
    case PRRTE_DAEMON_DVM_ADD_PROCS:
        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received add_local_procs",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }

        /* launch the processes */
        if (PRRTE_SUCCESS != (ret = prrte_odls.launch_local_procs(buffer))) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_debug_output,
                                 "%s prted:comm:add_procs failed to launch on error %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_ERROR_NAME(ret)));
        }
        break;

    case PRRTE_DAEMON_ABORT_PROCS_CALLED:
        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received abort_procs report",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }

        /* Number of processes */
        n = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &num_procs, &n, PRRTE_STD_CNTR)) ) {
            PRRTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* Retrieve list of processes */
        procs_to_kill = PRRTE_NEW(prrte_pointer_array_t);
        prrte_pointer_array_init(procs_to_kill, num_procs, INT32_MAX, 2);

        /* Keep track of previously terminated, so we don't keep ordering the
         * same processes to die.
         */
        if( NULL == procs_prev_ordered_to_terminate ) {
            procs_prev_ordered_to_terminate = PRRTE_NEW(prrte_pointer_array_t);
            prrte_pointer_array_init(procs_prev_ordered_to_terminate, num_procs+1, INT32_MAX, 8);
        }

        num_new_procs = 0;
        for( i = 0; i < num_procs; ++i) {
            cur_proc = PRRTE_NEW(prrte_proc_t);

            n = 1;
            if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &(cur_proc->name), &n, PRRTE_NAME)) ) {
                PRRTE_ERROR_LOG(ret);
                goto CLEANUP;
            }

            /* See if duplicate */
            found = false;
            for( p = 0; p < procs_prev_ordered_to_terminate->size; ++p) {
                if( NULL == (prev_proc = (prrte_proc_t*)prrte_pointer_array_get_item(procs_prev_ordered_to_terminate, p))) {
                    continue;
                }
                if(PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL,
                                                               &cur_proc->name,
                                                               &prev_proc->name) ) {
                    found = true;
                    break;
                }
            }

            PRRTE_OUTPUT_VERBOSE((2, prrte_debug_output,
                                 "%s prted:comm:abort_procs Application %s requests term. of %s (%2d of %2d) %3s.",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(sender),
                                 PRRTE_NAME_PRINT(&(cur_proc->name)), i, num_procs,
                                 (found ? "Dup" : "New") ));

            /* If not a duplicate, then add to the to_kill list */
            if( !found ) {
                prrte_pointer_array_add(procs_to_kill, (void*)cur_proc);
                PRRTE_RETAIN(cur_proc);
                prrte_pointer_array_add(procs_prev_ordered_to_terminate, (void*)cur_proc);
                num_new_procs++;
            }
        }

        /*
         * Send the request to terminate
         */
        if( num_new_procs > 0 ) {
            PRRTE_OUTPUT_VERBOSE((2, prrte_debug_output,
                                 "%s prted:comm:abort_procs Terminating application requested processes (%2d / %2d).",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 num_new_procs, num_procs));
            prrte_plm.terminate_procs(procs_to_kill);
        } else {
            PRRTE_OUTPUT_VERBOSE((2, prrte_debug_output,
                                 "%s prted:comm:abort_procs No new application processes to terminating from request (%2d / %2d).",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 num_new_procs, num_procs));
        }

        break;

        /****    EXIT COMMAND    ****/
    case PRRTE_DAEMON_EXIT_CMD:
        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received exit cmd",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }
        if (prrte_do_not_launch) {
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            return;
        }
        /* kill the local procs */
        prrte_odls.kill_local_procs(NULL);
        /* flag that prteds were ordered to terminate */
        prrte_prteds_term_ordered = true;
        /* if all my routes and local children are gone, then terminate ourselves */
        if (0 == (ret = prrte_routed.num_routes())) {
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                    PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    if (prrte_debug_daemons_flag) {
                        prrte_output(0, "%s prted_cmd: exit cmd, but proc %s is alive",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                    PRRTE_NAME_PRINT(&proct->name));
                    }
                    return;
                }
            }
            /* call our appropriate exit procedure */
            if (prrte_debug_daemons_flag) {
                prrte_output(0, "%s prted_cmd: all routes and children gone - exiting",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            }
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
        } else if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: exit cmd, %d routes still exist",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret);
        }
        return;
        break;

        /****    HALT VM COMMAND    ****/
    case PRRTE_DAEMON_HALT_VM_CMD:
        if (prrte_debug_daemons_flag) {
            prrte_output(0, "%s prted_cmd: received halt_vm cmd",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }
        /* this is an abnormal termination */
        prrte_abnormal_term_ordered = true;

        if (prrte_do_not_launch) {
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            return;
        }
        /* kill the local procs */
        prrte_odls.kill_local_procs(NULL);
        /* cycle thru our known jobs to find any that are tools - these
         * may not have been killed if, for example, we didn't start
         * them */
        i = prrte_hash_table_get_first_key_uint32(prrte_job_data, &u32, (void **)&jdata, &nptr);
        while (PRRTE_SUCCESS == i) {
            if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_TOOL) &&
                0 < prrte_list_get_size(&jdata->children)) {
                pmix_info_t info[3];
                bool flag;
                prrte_job_t *jd;
                pmix_status_t xrc = PMIX_ERR_JOB_TERMINATED;
                /* we need to notify this job that its CHILD job terminated
                 * as that is the job it is looking for */
                jd = (prrte_job_t*)prrte_list_get_first(&jdata->children);
                /* must notify this tool of termination so it can
                 * cleanly exit - otherwise, it may hang waiting for
                 * some kind of notification */
                /* ensure this only goes to the job terminated event handler */
                flag = true;
                PMIX_INFO_LOAD(&info[0], PMIX_EVENT_NON_DEFAULT, &flag, PMIX_BOOL);
                /* provide the status */
                PMIX_INFO_LOAD(&info[1], PMIX_JOB_TERM_STATUS, &xrc, PMIX_STATUS);
                /* tell the requestor which job */
                PRRTE_PMIX_CONVERT_JOBID(pname.nspace, jd->jobid);
                pname.rank = PMIX_RANK_WILDCARD;
                PMIX_INFO_LOAD(&info[2], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
                PRRTE_PMIX_CONSTRUCT_LOCK(&lk);
                PMIx_Notify_event(PMIX_ERR_JOB_TERMINATED, &pname, PMIX_RANGE_SESSION,
                                  info, 3, _notify_release, &lk);
                PRRTE_PMIX_WAIT_THREAD(&lk);
                PRRTE_PMIX_DESTRUCT_LOCK(&lk);
            }
            i = prrte_hash_table_get_next_key_uint32(prrte_job_data, &u32, (void **)&jdata, nptr, &nptr);
        }
        /* flag that prteds were ordered to terminate */
        prrte_prteds_term_ordered = true;
        if (PRRTE_PROC_IS_MASTER) {
            /* if all my routes and local children are gone, then terminate ourselves */
            if (0 == prrte_routed.num_routes()) {
                for (i=0; i < prrte_local_children->size; i++) {
                    if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                        PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                        /* at least one is still alive */
                        return;
                    }
                }
                /* call our appropriate exit procedure */
                if (prrte_debug_daemons_flag) {
                    prrte_output(0, "%s prted_cmd: all routes and children gone - exiting",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                }
                PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        } else {
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
        return;
        break;

        /****     DVM CLEANUP JOB COMMAND    ****/
    case PRRTE_DAEMON_DVM_CLEANUP_JOB_CMD:
        /* unpack the jobid */
        n = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &job, &n, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* look up job data object */
        if (NULL == (jdata = prrte_get_job_data_object(job))) {
            /* we can safely ignore this request as the job
             * was already cleaned up, or it was a tool */
            goto CLEANUP;
        }
        /* convert the jobid */
        PRRTE_PMIX_CONVERT_JOBID(pname.nspace, job);

        /* release all resources (even those on other nodes) that we
         * assigned to this job */
        if (NULL != jdata->map) {
            map = (prrte_job_map_t*)jdata->map;
            for (n = 0; n < map->nodes->size; n++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, n))) {
                    continue;
                }
                for (i = 0; i < node->procs->size; i++) {
                    if (NULL == (proct = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                        continue;
                    }
                    if (proct->name.jobid != jdata->jobid) {
                        /* skip procs from another job */
                        continue;
                    }
                    if (!PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_TOOL)) {
                        node->slots_inuse--;
                        node->num_procs--;
                    }
                    /* deregister this proc - will be ignored if already done */
                    PRRTE_PMIX_CONSTRUCT_LOCK(&lk);
                    pname.rank = proct->name.vpid;
                    PMIx_server_deregister_client(&pname, _notify_release, &lk);
                    PRRTE_PMIX_WAIT_THREAD(&lk);
                    PRRTE_PMIX_DESTRUCT_LOCK(&lk);
                    /* set the entry in the node array to NULL */
                    prrte_pointer_array_set_item(node->procs, i, NULL);
                    /* release the proc once for the map entry */
                    PRRTE_RELEASE(proct);
                }
                /* set the node location to NULL */
                prrte_pointer_array_set_item(map->nodes, n, NULL);
                /* maintain accounting */
                PRRTE_RELEASE(node);
                /* flag that the node is no longer in a map */
                PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
            }
            PRRTE_RELEASE(map);
            jdata->map = NULL;
        }
        PRRTE_PMIX_CONSTRUCT_LOCK(&lk);
        PMIx_server_deregister_nspace(pname.nspace, _notify_release, &lk);
        PRRTE_PMIX_WAIT_THREAD(&lk);
        PRRTE_PMIX_DESTRUCT_LOCK(&lk);

        PRRTE_RELEASE(jdata);
        break;


        /****     REPORT TOPOLOGY COMMAND    ****/
    case PRRTE_DAEMON_REPORT_TOPOLOGY_CMD:
        PRRTE_CONSTRUCT(&data, prrte_buffer_t);
        /* pack the topology signature */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(&data, &prrte_topo_signature, 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_DESTRUCT(&data);
            goto CLEANUP;
        }
        /* pack the topology */
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(&data, &prrte_hwloc_topology, 1, PRRTE_HWLOC_TOPO))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_DESTRUCT(&data);
            goto CLEANUP;
        }

        /* detect and add any coprocessors */
        coprocessors = prrte_hwloc_base_find_coprocessors(prrte_hwloc_topology);
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(&data, &coprocessors, 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
        }
        if (NULL != coprocessors) {
            free(coprocessors);
        }
        /* see if I am on a coprocessor */
        coprocessors = prrte_hwloc_base_check_on_coprocessor();
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(&data, &coprocessors, 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
        }
        if (NULL!= coprocessors) {
            free(coprocessors);
        }
        answer = PRRTE_NEW(prrte_buffer_t);
        if (prrte_compress.compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                                         &cmpdata, &cmplen)) {
            /* the data was compressed - mark that we compressed it */
            flag = 1;
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &flag, 1, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the compressed length */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &cmplen, 1, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the uncompressed length */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &data.bytes_used, 1, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the compressed info */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, cmpdata, cmplen, PRRTE_UINT8))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            PRRTE_DESTRUCT(&data);
            free(cmpdata);
        } else {
            /* mark that it was not compressed */
            flag = 0;
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &flag, 1, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_DESTRUCT(&data);
                free(cmpdata);
                PRRTE_RELEASE(answer);
                goto CLEANUP;
            }
            /* transfer the payload across */
            prrte_dss.copy_payload(answer, &data);
            PRRTE_DESTRUCT(&data);
        }
        /* send the data */
        if (0 > (ret = prrte_rml.send_buffer_nb(sender, answer, PRRTE_RML_TAG_TOPOLOGY_REPORT,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
        }
        break;

    case PRRTE_DAEMON_GET_STACK_TRACES:
        /* prep the response */
        answer = PRRTE_NEW(prrte_buffer_t);
        pathptr = path;

        // Try to find the "gstack" executable.  Failure to find the
        // executable will be handled below, because the receiver
        // expects to have the process name, hostname, and PID in the
        // buffer before finding an error message.
        char *gstack_exec;
        gstack_exec = prrte_find_absolute_path("gstack");

        /* hit each local process with a gstack command */
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                relay_msg = PRRTE_NEW(prrte_buffer_t);
                if (PRRTE_SUCCESS != prrte_dss.pack(relay_msg, &proct->name, 1, PRRTE_NAME) ||
                    PRRTE_SUCCESS != prrte_dss.pack(relay_msg, &proct->node->name, 1, PRRTE_STRING) ||
                    PRRTE_SUCCESS != prrte_dss.pack(relay_msg, &proct->pid, 1, PRRTE_PID)) {
                    PRRTE_RELEASE(relay_msg);
                    break;
                }

                // If we were able to find the gstack executable,
                // above, then run the command here.
                fp = NULL;
                if (NULL != gstack_exec) {
                    (void) snprintf(gscmd, sizeof(gscmd), "%s %lu",
                                    gstack_exec, (unsigned long) proct->pid);
                    fp = popen(gscmd, "r");
                }

                // If either we weren't able to find or run the gstack
                // exectuable, send back a nice error message here.
                if (NULL == gstack_exec || NULL == fp) {
                    (void) snprintf(string, sizeof(string),
                                    "Failed to %s \"%s\" on %s to obtain stack traces",
                                    (NULL == gstack_exec) ? "find" : "run",
                                    (NULL == gstack_exec) ? "gstack" : gstack_exec,
                                    proct->node->name);
                    if (PRRTE_SUCCESS ==
                        prrte_dss.pack(relay_msg, &string_ptr, 1, PRRTE_STRING)) {
                        prrte_dss.pack(answer, &relay_msg, 1, PRRTE_BUFFER);
                    }
                    PRRTE_RELEASE(relay_msg);
                    break;
                }
                /* Read the output a line at a time and pack it for transmission */
                memset(path, 0, sizeof(path));
                while (fgets(path, sizeof(path)-1, fp) != NULL) {
                    if (PRRTE_SUCCESS != prrte_dss.pack(relay_msg, &pathptr, 1, PRRTE_STRING)) {
                        PRRTE_RELEASE(relay_msg);
                        break;
                    }
                    memset(path, 0, sizeof(path));
                }
                /* close */
                pclose(fp);
                /* transfer this load */
                if (PRRTE_SUCCESS != prrte_dss.pack(answer, &relay_msg, 1, PRRTE_BUFFER)) {
                    PRRTE_RELEASE(relay_msg);
                    break;
                }
                PRRTE_RELEASE(relay_msg);
            }
        }
        if (NULL != gstack_exec) {
            free(gstack_exec);
        }
        /* always send our response */
        if (0 > (ret = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, answer,
                                               PRRTE_RML_TAG_STACK_TRACE,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
        }
        break;

    case PRRTE_DAEMON_GET_MEMPROFILE:
        answer = PRRTE_NEW(prrte_buffer_t);
        /* pack our hostname so they know where it came from */
        prrte_dss.pack(answer, &prrte_process_info.nodename, 1, PRRTE_STRING);
        /* collect my memory usage */
        PRRTE_CONSTRUCT(&pstat, prrte_pstats_t);
        prrte_pstat.query(prrte_process_info.pid, &pstat, NULL);
        prrte_dss.pack(answer, &pstat.pss, 1, PRRTE_FLOAT);
        PRRTE_DESTRUCT(&pstat);
        /* collect the memory usage of all my children */
        pss = 0.0;
        num_replies = 0;
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                /* collect the stats on this proc */
                PRRTE_CONSTRUCT(&pstat, prrte_pstats_t);
                if (PRRTE_SUCCESS == prrte_pstat.query(proct->pid, &pstat, NULL)) {
                    pss += pstat.pss;
                    ++num_replies;
                }
                PRRTE_DESTRUCT(&pstat);
            }
        }
        /* compute the average value */
        if (0 < num_replies) {
            pss /= (float)num_replies;
        }
        prrte_dss.pack(answer, &pss, 1, PRRTE_FLOAT);
        /* send it back */
        if (0 > (ret = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, answer,
                                               PRRTE_RML_TAG_MEMPROFILE,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
        }
        break;

    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
    }

 CLEANUP:
    return;
}

static char *get_prted_comm_cmd_str(int command)
{
    switch(command) {
    case PRRTE_DAEMON_KILL_LOCAL_PROCS:
        return strdup("PRRTE_DAEMON_KILL_LOCAL_PROCS");
    case PRRTE_DAEMON_SIGNAL_LOCAL_PROCS:
        return strdup("PRRTE_DAEMON_SIGNAL_LOCAL_PROCS");
    case PRRTE_DAEMON_ADD_LOCAL_PROCS:
        return strdup("PRRTE_DAEMON_ADD_LOCAL_PROCS");

    case PRRTE_DAEMON_EXIT_CMD:
        return strdup("PRRTE_DAEMON_EXIT_CMD");
    case PRRTE_DAEMON_PROCESS_AND_RELAY_CMD:
        return strdup("PRRTE_DAEMON_PROCESS_AND_RELAY_CMD");
    case PRRTE_DAEMON_NULL_CMD:
        return strdup("NULL");

    case PRRTE_DAEMON_HALT_VM_CMD:
        return strdup("PRRTE_DAEMON_HALT_VM_CMD");

    case PRRTE_DAEMON_ABORT_PROCS_CALLED:
        return strdup("PRRTE_DAEMON_ABORT_PROCS_CALLED");

    case PRRTE_DAEMON_DVM_NIDMAP_CMD:
        return strdup("PRRTE_DAEMON_DVM_NIDMAP_CMD");
    case PRRTE_DAEMON_DVM_ADD_PROCS:
        return strdup("PRRTE_DAEMON_DVM_ADD_PROCS");

    case PRRTE_DAEMON_GET_STACK_TRACES:
        return strdup("PRRTE_DAEMON_GET_STACK_TRACES");

    case PRRTE_DAEMON_GET_MEMPROFILE:
        return strdup("PRRTE_DAEMON_GET_MEMPROFILE");

    case PRRTE_DAEMON_DVM_CLEANUP_JOB_CMD:
        return strdup("PRRTE_DAEMON_DVM_CLEANUP_JOB_CMD");

    case PRRTE_DAEMON_PASS_NODE_INFO_CMD:
        return strdup("PRRTE_DAEMON_PASS_NODE_INFO_CMD");

    default:
        return strdup("Unknown Command!");
    }
}
