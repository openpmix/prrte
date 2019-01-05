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
 * Copyright (c) 2016-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

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


#include "opal/event/event-internal.h"
#include "opal/mca/base/base.h"
#include "opal/mca/pstat/pstat.h"
#include "opal/util/output.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_progress.h"
#include "opal/dss/dss.h"
#include "opal/pmix/pmix-internal.h"

#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/name_fns.h"
#include "orte/util/compress.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/grpcomm/base/base.h"
#include "orte/mca/iof/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/base.h"
#include "orte/mca/oob/base/base.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/state/state.h"

#include "orte/mca/odls/base/odls_private.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_quit.h"

#include "orte/orted/orted.h"

/*
 * Globals
 */
static char *get_orted_comm_cmd_str(int command);

static void _notify_release(pmix_status_t status, void *cbdata)
{
    opal_pmix_lock_t *lk = (opal_pmix_lock_t*)cbdata;

    OPAL_PMIX_WAKEUP_THREAD(lk);
}

static opal_pointer_array_t *procs_prev_ordered_to_terminate = NULL;

void orte_daemon_recv(int status, orte_process_name_t* sender,
                      opal_buffer_t *buffer, orte_rml_tag_t tag,
                      void* cbdata)
{
    orte_daemon_cmd_flag_t command;
    opal_buffer_t *relay_msg;
    int ret;
    orte_std_cntr_t n;
    int32_t signal;
    orte_jobid_t job;
    opal_buffer_t data, *answer;
    orte_job_t *jdata;
    orte_process_name_t proc;
    int32_t i, num_replies;
    opal_pointer_array_t procarray;
    orte_proc_t *proct;
    char *cmd_str = NULL;
    opal_pointer_array_t *procs_to_kill = NULL;
    orte_std_cntr_t num_procs, num_new_procs = 0, p;
    orte_proc_t *cur_proc = NULL, *prev_proc = NULL;
    bool found = false;
    orte_node_t *node;
    FILE *fp;
    char gscmd[256], path[1035], *pathptr;
    char string[256], *string_ptr = string;
    float pss;
    opal_pstats_t pstat;
    char *rtmod;
    char *coprocessors;
    orte_job_map_t *map;
    int8_t flag;
    uint8_t *cmpdata;
    size_t cmplen;
    uint32_t u32;
    void *nptr;
    opal_pmix_lock_t lk;
    pmix_proc_t pname;

    /* unpack the command */
    n = 1;
    if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &command, &n, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(ret);
        return;
    }

    cmd_str = get_orted_comm_cmd_str(command);
    OPAL_OUTPUT_VERBOSE((1, orte_debug_output,
                         "%s orted:comm:process_commands() Processing Command: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd_str));
    free(cmd_str);
    cmd_str = NULL;

    /* now process the command locally */
    switch(command) {

        /****    NULL    ****/
    case ORTE_DAEMON_NULL_CMD:
        ret = ORTE_SUCCESS;
        break;

        /****    KILL_LOCAL_PROCS   ****/
    case ORTE_DAEMON_KILL_LOCAL_PROCS:
        num_replies = 0;

        /* construct the pointer array */
        OBJ_CONSTRUCT(&procarray, opal_pointer_array_t);
        opal_pointer_array_init(&procarray, num_replies, ORTE_GLOBAL_ARRAY_MAX_SIZE, 16);

        /* unpack the proc names into the array */
        while (ORTE_SUCCESS == (ret = opal_dss.unpack(buffer, &proc, &n, ORTE_NAME))) {
            proct = OBJ_NEW(orte_proc_t);
            proct->name.jobid = proc.jobid;
            proct->name.vpid = proc.vpid;

            opal_pointer_array_add(&procarray, proct);
            num_replies++;
        }
        if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
            ORTE_ERROR_LOG(ret);
            goto KILL_PROC_CLEANUP;
        }

        if (0 == num_replies) {
            /* kill everything */
            if (ORTE_SUCCESS != (ret = orte_odls.kill_local_procs(NULL))) {
                ORTE_ERROR_LOG(ret);
            }
            break;
        } else {
            /* kill the procs */
            if (ORTE_SUCCESS != (ret = orte_odls.kill_local_procs(&procarray))) {
                ORTE_ERROR_LOG(ret);
            }
        }

        /* cleanup */
    KILL_PROC_CLEANUP:
        for (i=0; i < procarray.size; i++) {
            if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(&procarray, i))) {
                free(proct);
            }
        }
        OBJ_DESTRUCT(&procarray);
        break;

        /****    SIGNAL_LOCAL_PROCS   ****/
    case ORTE_DAEMON_SIGNAL_LOCAL_PROCS:
        /* unpack the jobid */
        n = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &job, &n, ORTE_JOBID))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* look up job data object */
        jdata = orte_get_job_data_object(job);

        /* get the signal */
        n = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &signal, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* Convert SIGTSTP to SIGSTOP so we can suspend a.out */
        if (SIGTSTP == signal) {
            if (orte_debug_daemons_flag) {
                opal_output(0, "%s orted_cmd: converted SIGTSTP to SIGSTOP before delivering",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            }
            signal = SIGSTOP;
            if (NULL != jdata) {
                jdata->state |= ORTE_JOB_STATE_SUSPENDED;
            }
        } else if (SIGCONT == signal && NULL != jdata) {
            jdata->state &= ~ORTE_JOB_STATE_SUSPENDED;
        }

        if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: received signal_local_procs, delivering signal %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        signal);
        }

        /* signal them */
        if (ORTE_SUCCESS != (ret = orte_odls.signal_local_procs(NULL, signal))) {
            ORTE_ERROR_LOG(ret);
        }
        break;

        /****    ADD_LOCAL_PROCS   ****/
    case ORTE_DAEMON_ADD_LOCAL_PROCS:
    case ORTE_DAEMON_DVM_ADD_PROCS:
        if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: received add_local_procs",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        }

        /* launch the processes */
        if (ORTE_SUCCESS != (ret = orte_odls.launch_local_procs(buffer))) {
            OPAL_OUTPUT_VERBOSE((1, orte_debug_output,
                                 "%s orted:comm:add_procs failed to launch on error %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_ERROR_NAME(ret)));
        }
        break;

    case ORTE_DAEMON_ABORT_PROCS_CALLED:
        if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: received abort_procs report",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        }

        /* Number of processes */
        n = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &num_procs, &n, ORTE_STD_CNTR)) ) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* Retrieve list of processes */
        procs_to_kill = OBJ_NEW(opal_pointer_array_t);
        opal_pointer_array_init(procs_to_kill, num_procs, INT32_MAX, 2);

        /* Keep track of previously terminated, so we don't keep ordering the
         * same processes to die.
         */
        if( NULL == procs_prev_ordered_to_terminate ) {
            procs_prev_ordered_to_terminate = OBJ_NEW(opal_pointer_array_t);
            opal_pointer_array_init(procs_prev_ordered_to_terminate, num_procs+1, INT32_MAX, 8);
        }

        num_new_procs = 0;
        for( i = 0; i < num_procs; ++i) {
            cur_proc = OBJ_NEW(orte_proc_t);

            n = 1;
            if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &(cur_proc->name), &n, ORTE_NAME)) ) {
                ORTE_ERROR_LOG(ret);
                goto CLEANUP;
            }

            /* See if duplicate */
            found = false;
            for( p = 0; p < procs_prev_ordered_to_terminate->size; ++p) {
                if( NULL == (prev_proc = (orte_proc_t*)opal_pointer_array_get_item(procs_prev_ordered_to_terminate, p))) {
                    continue;
                }
                if(OPAL_EQUAL == orte_util_compare_name_fields(ORTE_NS_CMP_ALL,
                                                               &cur_proc->name,
                                                               &prev_proc->name) ) {
                    found = true;
                    break;
                }
            }

            OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                                 "%s orted:comm:abort_procs Application %s requests term. of %s (%2d of %2d) %3s.",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(sender),
                                 ORTE_NAME_PRINT(&(cur_proc->name)), i, num_procs,
                                 (found ? "Dup" : "New") ));

            /* If not a duplicate, then add to the to_kill list */
            if( !found ) {
                opal_pointer_array_add(procs_to_kill, (void*)cur_proc);
                OBJ_RETAIN(cur_proc);
                opal_pointer_array_add(procs_prev_ordered_to_terminate, (void*)cur_proc);
                num_new_procs++;
            }
        }

        /*
         * Send the request to terminate
         */
        if( num_new_procs > 0 ) {
            OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                                 "%s orted:comm:abort_procs Terminating application requested processes (%2d / %2d).",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 num_new_procs, num_procs));
            orte_plm.terminate_procs(procs_to_kill);
        } else {
            OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                                 "%s orted:comm:abort_procs No new application processes to terminating from request (%2d / %2d).",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 num_new_procs, num_procs));
        }

        break;

        /****    EXIT COMMAND    ****/
    case ORTE_DAEMON_EXIT_CMD:
        if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: received exit cmd",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        }
        if (orte_do_not_launch) {
            ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_DAEMONS_TERMINATED);
            return;
        }
        /* kill the local procs */
        orte_odls.kill_local_procs(NULL);
        /* flag that orteds were ordered to terminate */
        orte_orteds_term_ordered = true;
        /* if all my routes and local children are gone, then terminate ourselves */
        rtmod = orte_rml.get_routed(orte_mgmt_conduit);
        if (0 == (ret = orte_routed.num_routes(rtmod))) {
            for (i=0; i < orte_local_children->size; i++) {
                if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(orte_local_children, i)) &&
                    ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    if (orte_debug_daemons_flag) {
                        opal_output(0, "%s orted_cmd: exit cmd, but proc %s is alive",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    ORTE_NAME_PRINT(&proct->name));
                    }
                    return;
                }
            }
            /* call our appropriate exit procedure */
            if (orte_debug_daemons_flag) {
                opal_output(0, "%s orted_cmd: all routes and children gone - exiting",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            }
            ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_DAEMONS_TERMINATED);
        } else if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: exit cmd, %d routes still exist",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ret);
        }
        return;
        break;

        /****    HALT VM COMMAND    ****/
    case ORTE_DAEMON_HALT_VM_CMD:
        if (orte_debug_daemons_flag) {
            opal_output(0, "%s orted_cmd: received halt_vm cmd",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        }
        if (orte_do_not_launch) {
            ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_DAEMONS_TERMINATED);
            return;
        }
        /* kill the local procs */
        orte_odls.kill_local_procs(NULL);
        /* cycle thru our known jobs to find any that are tools - these
         * may not have been killed if, for example, we didn't start
         * them */
        i = opal_hash_table_get_first_key_uint32(orte_job_data, &u32, (void **)&jdata, &nptr);
        while (OPAL_SUCCESS == i) {
            if (ORTE_FLAG_TEST(jdata, ORTE_JOB_FLAG_TOOL) &&
                0 < opal_list_get_size(&jdata->children)) {
                pmix_info_t info[3];
                bool flag;
                orte_job_t *jd;
                pmix_status_t xrc = PMIX_ERR_JOB_TERMINATED;
                /* we need to notify this job that its CHILD job terminated
                 * as that is the job it is looking for */
                jd = (orte_job_t*)opal_list_get_first(&jdata->children);
                /* must notify this tool of termination so it can
                 * cleanly exit - otherwise, it may hang waiting for
                 * some kind of notification */
                /* ensure this only goes to the job terminated event handler */
                flag = true;
                PMIX_INFO_LOAD(&info[0], PMIX_EVENT_NON_DEFAULT, &flag, PMIX_BOOL);
                /* provide the status */
                PMIX_INFO_LOAD(&info[1], PMIX_JOB_TERM_STATUS, &xrc, PMIX_STATUS);
                /* tell the requestor which job */
                OPAL_PMIX_CONVERT_JOBID(pname.nspace, jd->jobid);
                pname.rank = PMIX_RANK_WILDCARD;
                PMIX_INFO_LOAD(&info[2], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
                OPAL_PMIX_CONSTRUCT_LOCK(&lk);
                PMIx_Notify_event(PMIX_ERR_JOB_TERMINATED, &pname, PMIX_RANGE_SESSION,
                                  info, 3, _notify_release, &lk);
                OPAL_PMIX_WAIT_THREAD(&lk);
                OPAL_PMIX_DESTRUCT_LOCK(&lk);
            }
            i = opal_hash_table_get_next_key_uint32(orte_job_data, &u32, (void **)&jdata, nptr, &nptr);
        }
        /* flag that orteds were ordered to terminate */
        orte_orteds_term_ordered = true;
        if (ORTE_PROC_IS_HNP) {
            /* if all my routes and local children are gone, then terminate ourselves */
            rtmod = orte_rml.get_routed(orte_mgmt_conduit);
            if (0 == orte_routed.num_routes(rtmod)) {
                for (i=0; i < orte_local_children->size; i++) {
                    if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(orte_local_children, i)) &&
                        ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_ALIVE)) {
                        /* at least one is still alive */
                        return;
                    }
                }
                /* call our appropriate exit procedure */
                if (orte_debug_daemons_flag) {
                    opal_output(0, "%s orted_cmd: all routes and children gone - exiting",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                }
                ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        } else {
            ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_DAEMONS_TERMINATED);
        }
        return;
        break;

        /****     DVM CLEANUP JOB COMMAND    ****/
    case ORTE_DAEMON_DVM_CLEANUP_JOB_CMD:
        /* unpack the jobid */
        n = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &job, &n, ORTE_JOBID))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        /* look up job data object */
        if (NULL == (jdata = orte_get_job_data_object(job))) {
            /* we can safely ignore this request as the job
             * was already cleaned up, or it was a tool */
            goto CLEANUP;
        }
        /* convert the jobid */
        OPAL_PMIX_CONVERT_JOBID(pname.nspace, job);

        /* release all resources (even those on other nodes) that we
         * assigned to this job */
        if (NULL != jdata->map) {
            map = (orte_job_map_t*)jdata->map;
            for (n = 0; n < map->nodes->size; n++) {
                if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(map->nodes, n))) {
                    continue;
                }
                for (i = 0; i < node->procs->size; i++) {
                    if (NULL == (proct = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
                        continue;
                    }
                    if (proct->name.jobid != jdata->jobid) {
                        /* skip procs from another job */
                        continue;
                    }
                    if (!ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_TOOL)) {
                        node->slots_inuse--;
                        node->num_procs--;
                    }
                    /* deregister this proc - will be ignored if already done */
                    OPAL_PMIX_CONSTRUCT_LOCK(&lk);
                    pname.rank = proct->name.vpid;
                    PMIx_server_deregister_client(&pname, _notify_release, &lk);
                    OPAL_PMIX_WAIT_THREAD(&lk);
                    OPAL_PMIX_DESTRUCT_LOCK(&lk);
                    /* set the entry in the node array to NULL */
                    opal_pointer_array_set_item(node->procs, i, NULL);
                    /* release the proc once for the map entry */
                    OBJ_RELEASE(proct);
                }
                /* set the node location to NULL */
                opal_pointer_array_set_item(map->nodes, n, NULL);
                /* maintain accounting */
                OBJ_RELEASE(node);
                /* flag that the node is no longer in a map */
                ORTE_FLAG_UNSET(node, ORTE_NODE_FLAG_MAPPED);
            }
            OBJ_RELEASE(map);
            jdata->map = NULL;
        }
        OPAL_PMIX_CONSTRUCT_LOCK(&lk);
        PMIx_server_deregister_nspace(pname.nspace, _notify_release, &lk);
        OPAL_PMIX_WAIT_THREAD(&lk);
        OPAL_PMIX_DESTRUCT_LOCK(&lk);

        OBJ_RELEASE(jdata);
        break;


        /****     REPORT TOPOLOGY COMMAND    ****/
    case ORTE_DAEMON_REPORT_TOPOLOGY_CMD:
        OBJ_CONSTRUCT(&data, opal_buffer_t);
        /* pack the topology signature */
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&data, &orte_topo_signature, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(ret);
            OBJ_DESTRUCT(&data);
            goto CLEANUP;
        }
        /* pack the topology */
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&data, &opal_hwloc_topology, 1, OPAL_HWLOC_TOPO))) {
            ORTE_ERROR_LOG(ret);
            OBJ_DESTRUCT(&data);
            goto CLEANUP;
        }

        /* detect and add any coprocessors */
        coprocessors = opal_hwloc_base_find_coprocessors(opal_hwloc_topology);
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&data, &coprocessors, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(ret);
        }
        if (NULL != coprocessors) {
            free(coprocessors);
        }
        /* see if I am on a coprocessor */
        coprocessors = opal_hwloc_base_check_on_coprocessor();
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&data, &coprocessors, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(ret);
        }
        if (NULL!= coprocessors) {
            free(coprocessors);
        }
        answer = OBJ_NEW(opal_buffer_t);
        if (orte_util_compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                             &cmpdata, &cmplen)) {
            /* the data was compressed - mark that we compressed it */
            flag = 1;
            if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, &flag, 1, OPAL_INT8))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
                OBJ_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the compressed length */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, &cmplen, 1, OPAL_SIZE))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
                OBJ_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the uncompressed length */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, &data.bytes_used, 1, OPAL_SIZE))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
                OBJ_RELEASE(answer);
                goto CLEANUP;
            }
            /* pack the compressed info */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, cmpdata, cmplen, OPAL_UINT8))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
                OBJ_RELEASE(answer);
                goto CLEANUP;
            }
            OBJ_DESTRUCT(&data);
            free(cmpdata);
        } else {
            /* mark that it was not compressed */
            flag = 0;
            if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, &flag, 1, OPAL_INT8))) {
                ORTE_ERROR_LOG(ret);
                OBJ_DESTRUCT(&data);
                free(cmpdata);
                OBJ_RELEASE(answer);
                goto CLEANUP;
            }
            /* transfer the payload across */
            opal_dss.copy_payload(answer, &data);
            OBJ_DESTRUCT(&data);
        }
        /* send the data */
        if (0 > (ret = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                               sender, answer, ORTE_RML_TAG_TOPOLOGY_REPORT,
                                               orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(answer);
        }
        break;

    case ORTE_DAEMON_GET_STACK_TRACES:
        /* prep the response */
        answer = OBJ_NEW(opal_buffer_t);
        pathptr = path;

        // Try to find the "gstack" executable.  Failure to find the
        // executable will be handled below, because the receiver
        // expects to have the process name, hostname, and PID in the
        // buffer before finding an error message.
        char *gstack_exec;
        gstack_exec = opal_find_absolute_path("gstack");

        /* hit each local process with a gstack command */
        for (i=0; i < orte_local_children->size; i++) {
            if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(orte_local_children, i)) &&
                ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_ALIVE)) {
                relay_msg = OBJ_NEW(opal_buffer_t);
                if (OPAL_SUCCESS != opal_dss.pack(relay_msg, &proct->name, 1, ORTE_NAME) ||
                    OPAL_SUCCESS != opal_dss.pack(relay_msg, &proct->node->name, 1, OPAL_STRING) ||
                    OPAL_SUCCESS != opal_dss.pack(relay_msg, &proct->pid, 1, OPAL_PID)) {
                    OBJ_RELEASE(relay_msg);
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
                    if (OPAL_SUCCESS ==
                        opal_dss.pack(relay_msg, &string_ptr, 1, OPAL_STRING)) {
                        opal_dss.pack(answer, &relay_msg, 1, OPAL_BUFFER);
                    }
                    OBJ_RELEASE(relay_msg);
                    break;
                }
                /* Read the output a line at a time and pack it for transmission */
                memset(path, 0, sizeof(path));
                while (fgets(path, sizeof(path)-1, fp) != NULL) {
                    if (OPAL_SUCCESS != opal_dss.pack(relay_msg, &pathptr, 1, OPAL_STRING)) {
                        OBJ_RELEASE(relay_msg);
                        break;
                    }
                    memset(path, 0, sizeof(path));
                }
                /* close */
                pclose(fp);
                /* transfer this load */
                if (OPAL_SUCCESS != opal_dss.pack(answer, &relay_msg, 1, OPAL_BUFFER)) {
                    OBJ_RELEASE(relay_msg);
                    break;
                }
                OBJ_RELEASE(relay_msg);
            }
        }
        if (NULL != gstack_exec) {
            free(gstack_exec);
        }
        /* always send our response */
        if (0 > (ret = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                               ORTE_PROC_MY_HNP, answer,
                                               ORTE_RML_TAG_STACK_TRACE,
                                               orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(answer);
        }
        break;

    case ORTE_DAEMON_GET_MEMPROFILE:
        answer = OBJ_NEW(opal_buffer_t);
        /* pack our hostname so they know where it came from */
        opal_dss.pack(answer, &orte_process_info.nodename, 1, OPAL_STRING);
        /* collect my memory usage */
        OBJ_CONSTRUCT(&pstat, opal_pstats_t);
        opal_pstat.query(orte_process_info.pid, &pstat, NULL);
        opal_dss.pack(answer, &pstat.pss, 1, OPAL_FLOAT);
        OBJ_DESTRUCT(&pstat);
        /* collect the memory usage of all my children */
        pss = 0.0;
        num_replies = 0;
        for (i=0; i < orte_local_children->size; i++) {
            if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(orte_local_children, i)) &&
                ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_ALIVE)) {
                /* collect the stats on this proc */
                OBJ_CONSTRUCT(&pstat, opal_pstats_t);
                if (OPAL_SUCCESS == opal_pstat.query(proct->pid, &pstat, NULL)) {
                    pss += pstat.pss;
                    ++num_replies;
                }
                OBJ_DESTRUCT(&pstat);
            }
        }
        /* compute the average value */
        if (0 < num_replies) {
            pss /= (float)num_replies;
        }
        opal_dss.pack(answer, &pss, 1, OPAL_FLOAT);
        /* send it back */
        if (0 > (ret = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                               ORTE_PROC_MY_HNP, answer,
                                               ORTE_RML_TAG_MEMPROFILE,
                                               orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(answer);
        }
        break;

    default:
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
    }

 CLEANUP:
    return;
}

static char *get_orted_comm_cmd_str(int command)
{
    switch(command) {
    case ORTE_DAEMON_KILL_LOCAL_PROCS:
        return strdup("ORTE_DAEMON_KILL_LOCAL_PROCS");
    case ORTE_DAEMON_SIGNAL_LOCAL_PROCS:
        return strdup("ORTE_DAEMON_SIGNAL_LOCAL_PROCS");
    case ORTE_DAEMON_ADD_LOCAL_PROCS:
        return strdup("ORTE_DAEMON_ADD_LOCAL_PROCS");

    case ORTE_DAEMON_EXIT_CMD:
        return strdup("ORTE_DAEMON_EXIT_CMD");
    case ORTE_DAEMON_PROCESS_AND_RELAY_CMD:
        return strdup("ORTE_DAEMON_PROCESS_AND_RELAY_CMD");
    case ORTE_DAEMON_NULL_CMD:
        return strdup("NULL");

    case ORTE_DAEMON_HALT_VM_CMD:
        return strdup("ORTE_DAEMON_HALT_VM_CMD");

    case ORTE_DAEMON_ABORT_PROCS_CALLED:
        return strdup("ORTE_DAEMON_ABORT_PROCS_CALLED");

    case ORTE_DAEMON_DVM_NIDMAP_CMD:
        return strdup("ORTE_DAEMON_DVM_NIDMAP_CMD");
    case ORTE_DAEMON_DVM_ADD_PROCS:
        return strdup("ORTE_DAEMON_DVM_ADD_PROCS");

    case ORTE_DAEMON_GET_STACK_TRACES:
        return strdup("ORTE_DAEMON_GET_STACK_TRACES");

    case ORTE_DAEMON_GET_MEMPROFILE:
        return strdup("ORTE_DAEMON_GET_MEMPROFILE");

    case ORTE_DAEMON_DVM_CLEANUP_JOB_CMD:
        return strdup("ORTE_DAEMON_DVM_CLEANUP_JOB_CMD");

    default:
        return strdup("Unknown Command!");
    }
}
