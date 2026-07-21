/*
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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Advanced Micro Devices, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#include <ctype.h>

#include "src/class/pmix_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/include/hash_string.h"
#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/rml/rml_contact.h"
#include "src/rml/rml.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/runtime.h"
#include "src/threads/pmix_threads.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/pmix_argv.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/nidmap.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_environ.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"

void prte_plm_base_set_slots(prte_node_t *node)
{
    if (0 == strncmp(prte_set_slots, "cores", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                   HWLOC_OBJ_CORE);
        }
    } else if (0 == strncmp(prte_set_slots, "sockets", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                   HWLOC_OBJ_SOCKET);
            if (0 == node->slots) {
                /* some systems don't report sockets - in this case,
                 * use numanodes */
                node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                       HWLOC_OBJ_NUMANODE);
            }
        }
    } else if (0 == strncmp(prte_set_slots, "numas", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                   HWLOC_OBJ_NUMANODE);
        }
    } else if (0 == strncmp(prte_set_slots, "hwthreads", strlen(prte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                   HWLOC_OBJ_PU);
        }
    } else {
        /* must be a number */
        node->slots = strtol(prte_set_slots, NULL, 10);
    }
    /* mark the node as having its slots "given" */
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
}

void prte_plm_base_daemons_reported(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_topology_t *t;
    prte_node_t *node;
    int i;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* if we are not launching, then we just assume that all
     * daemons share our topology */
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node || NULL == node->topology) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            caddy->jdata->state = PRTE_JOB_STATE_FAILED_TO_START;
            PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FAILED_TO_START);

            /* cleanup */
            PMIX_RELEASE(caddy);
            return;
        }
        t = node->topology;
        for (i = 1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t;
                node->available = prte_hwloc_base_filter_cpus(node->topology->topo);
            }
            node->state = PRTE_NODE_STATE_UP;
        }
    }

    /* if this is an unmanaged allocation, then set the default
     * slots on each node as directed or using default
     */
    if (!prte_managed_allocation || prte_set_slots_override) {
        caddy->jdata->total_slots_alloc = 0;
        for (i = 0; i < caddy->jdata->session->nodes->size; i++) {
            node = (prte_node_t *) pmix_pointer_array_get_item(caddy->jdata->session->nodes, i);
            if (NULL == node) {
                continue;
            }
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                     "%s plm:base:setting slots for node %s by %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name,
                                     prte_set_slots));
                prte_plm_base_set_slots(node);
            }
            caddy->jdata->total_slots_alloc += node->slots;
        }
    } else {
        /* for managed allocations, the total slots allocated is fixed at time of allocation */
        caddy->jdata->total_slots_alloc = prte_ras_base.total_slots_alloc;
    }

    /* progress the job */
    caddy->jdata->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_VM_READY);

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_plm_base_allocation_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_node_t *node;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* In a bootstrapped DVM the daemons were started independently on every
     * node and are already phoning home - there is nothing for us to launch.
     * We therefore establish the virtual machine directly from the allocated
     * node pool (assigning vpids, building the routing tree, and setting the
     * expected daemon count) and then simply wait: as each running daemon
     * reports in, prte_plm_base_daemon_callback advances the state machine to
     * DAEMONS_REPORTED and on to VM_READY.
     *
     * This special handling applies ONLY to the daemon job (the one-time DVM
     * formation). Application jobs launched later into the running DVM must
     * follow the normal path: their LAUNCH_DAEMONS step calls setup_vm, finds
     * every daemon already present ("no new daemons required"), and advances
     * itself to DAEMONS_REPORTED. Taking the bootstrap branch for an app job
     * would strand it in DAEMONS_LAUNCHED with nothing left to report in. */
    if (prte_bootstrap_setup &&
        caddy->jdata == prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace)) {
        caddy->jdata->state = PRTE_JOB_STATE_ALLOCATION_COMPLETE;
        rc = prte_plm_base_setup_virtual_machine(caddy->jdata);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FAILED_TO_START);
            PMIX_RELEASE(caddy);
            return;
        }
        caddy->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PMIX_RELEASE(caddy);
        return;
    }

    /* if we don't want to launch, then we at least want
     * to map so we can see where the procs would have
     * gone - so skip to the mapping state */
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node) {
            // should never happen
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FAILED_TO_START);
            PMIX_RELEASE(caddy);
            return;
        }
        prte_rmaps_base.require_hwtcpus = !prte_hwloc_base_core_cpus(node->topology->topo);
        prte_rmaps_base.have_cores = prte_hwloc_base_has_cores(node->topology->topo);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
    } else {
        /* move the state machine along */
        caddy->jdata->state = PRTE_JOB_STATE_ALLOCATION_COMPLETE;
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_LAUNCH_DAEMONS);
    }

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_plm_base_daemons_launched(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* do NOT increment the state - we wait for the
     * daemons to report that they have actually
     * started before moving to the right state
     */
    /* cleanup */
    PMIX_RELEASE(caddy);
}

static void files_ready(int status, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t *) cbdata;

    if (PRTE_SUCCESS != status) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    } else {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    }
}

void prte_plm_base_vm_ready(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_node_t *node;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* progress the job */
    caddy->jdata->state = PRTE_JOB_STATE_VM_READY;

    /* check the first daemon's node for topology
     * limitations - or the HNP's node if we didn't
     * launch any daemons */
    node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, 1);
    if (NULL == node) {
        node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, 0);
    }
    if (NULL != node && NULL != node->topology &&
        NULL != node->topology->topo) {
        prte_rmaps_base.require_hwtcpus = !prte_hwloc_base_core_cpus(node->topology->topo);
        prte_rmaps_base.have_cores = prte_hwloc_base_has_cores(node->topology->topo);
    }

    /* position any required files */
    if (PRTE_SUCCESS != prte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    }

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_plm_base_mapping_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* move the state machine along */
    caddy->jdata->state = PRTE_JOB_STATE_MAP_COMPLETE;
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_SYSTEM_PREP);

    /* cleanup */
    PMIX_RELEASE(caddy);
}

/* catch spawn timeout */
static void spawn_timeout_cb(int fd, short event, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t *) cbdata;
    prte_timer_t *timer = NULL;
    pmix_proc_t proc;
    int timeout, *tp;
    char *st;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(fd, event);

    PMIX_ACQUIRE_OBJECT(jdata);

    /* Display a useful message to the user */
    tp = &timeout;
    if (!prte_get_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT, (void **) &tp, PMIX_INT)) {
        /* This shouldn't happen, but at least don't segv / display
         *something* if it does */
        timeout = -1;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, (void **) &timer, PMIX_POINTER)) {
        prte_event_evtimer_del(timer->ev);
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch deleting timeout for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));
        PMIX_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT);
    }
    pmix_asprintf(&st, "--------------------------------------------------------------------------\n"
                       "The user-provided time limit for job launch has been reached:\n\n"
                       "  Timeout: %d seconds\n\n"
                       "The job will now be aborted.  Please check your environment to\n"
                       "identify the source of the delay and try again.\n"
                       "--------------------------------------------------------------------------\n",
                  timeout);
    bo.bytes = st;
    bo.size = strlen(st);
    PMIX_LOAD_PROCID(&proc, jdata->nspace, PMIX_RANK_WILDCARD);
    PMIx_server_IOF_deliver(&proc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
    free(st);

    /* abort the job */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START);
    jdata->exit_code = PRTE_ERR_TIMEOUT;

    if (!prte_persistent) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_TIMEOUT);
    }
}

void prte_plm_base_stack_trace_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata)
{
    pmix_byte_object_t pbo;
    pmix_data_buffer_t blob;
    char *st, *st2;
    int32_t cnt;
    pmix_proc_t name;
    char *hostname, *nspace;
    pid_t pid;
    prte_job_t *jdata = NULL;
    prte_timer_t *timer;
    prte_proc_t proc;
    pmix_pointer_array_t parray;
    int rc;
    pmix_byte_object_t bo;

    PMIX_DATA_BUFFER_CONSTRUCT(&blob);
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                        "%s: stacktrace recvd from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the stack_trace blob */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&blob);
        return;
    }
    jdata = prte_get_job_data_object(nspace);
    if (NULL == jdata) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        free(nspace);
        return;
    }
    free(nspace);

    while (PMIX_SUCCESS == (rc = PMIx_Data_unpack(NULL, buffer, &pbo, &cnt, PMIX_BYTE_OBJECT))) {
        rc = PMIx_Data_load(&blob, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&blob);
            goto DONE;
        }
        /* first piece is the name of the process */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &blob, &name, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&blob);
            goto DONE;
        }
        rc = PMIx_Data_unpack(NULL, &blob, &hostname, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&blob);
            goto DONE;
        }
        rc = PMIx_Data_unpack(NULL, &blob, &pid, &cnt, PMIX_PID);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&blob);
            goto DONE;
        }
        pmix_asprintf(&st, "STACK TRACE FOR PROC %s (%s, PID %lu)\n",
                      PRTE_NAME_PRINT(&name), hostname,
                      (unsigned long) pid);
        PMIx_Argv_append_nosize(&jdata->traces, st);
        free(hostname);
        free(st);
        /* unpack the stack_trace until complete */
        cnt = 1;
        while (PRTE_SUCCESS == (rc = PMIx_Data_unpack(NULL, &blob, &st, &cnt, PMIX_STRING))) {
            pmix_asprintf(&st2, "\t%s", st); // has its own newline
            PMIx_Argv_append_nosize(&jdata->traces, st2);
            free(st);
            free(st2);
            cnt = 1;
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&blob);
        cnt = 1;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }

DONE:
    jdata->ntraces++;
    if (prte_process_info.num_daemons == jdata->ntraces) {
        timer = NULL;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT,
                               (void **) &timer, PMIX_POINTER) &&
            NULL != timer) {
            prte_event_evtimer_del(timer->ev);
            /* timer is an prte_timer_t object */
            PMIX_RELEASE(timer);
            prte_remove_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT);
        }
        /* output the results - note that the output might need to go to a
         * tool instead of just to stderr, so we use the PMIx IOF deliver
         * function to ensure it gets where it needs to go */
        PMIX_LOAD_PROCID(&name, jdata->nspace, PMIX_RANK_WILDCARD);
        for (cnt=0; NULL != jdata->traces[cnt]; cnt++) {
            bo.bytes = jdata->traces[cnt];
            bo.size = strlen(jdata->traces[cnt]);
            PMIx_server_IOF_deliver(&name, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
        }
        /* abort the job */
        PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
        /* create an object */
        PMIX_LOAD_PROCID(&proc.name, jdata->nspace, PMIX_RANK_WILDCARD);
        cnt = pmix_pointer_array_add(&parray, &proc);
        if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
            PRTE_ERROR_LOG(rc);
        }
        PMIX_DESTRUCT(&parray);
    }
}

static void stack_trace_timeout(int sd, short args, void *cbdata)
{
    prte_timer_t *timer;
    prte_job_t *jdata = (prte_job_t *) cbdata;
    prte_proc_t proc;
    pmix_pointer_array_t parray;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* clear the timer */
    timer = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, (void **) &timer, PMIX_POINTER)
        && NULL != timer) {
        prte_event_evtimer_del(timer->ev);
        /* timer is an prte_timer_t object */
        PMIX_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT);
    }

    /* abort the job */
    PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
    /* create an object */
    PMIX_LOAD_PROCID(&proc.name, jdata->nspace, PMIX_RANK_WILDCARD);
    pmix_pointer_array_add(&parray, &proc);
    if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
        PRTE_ERROR_LOG(rc);
    }
    PMIX_DESTRUCT(&parray);
}

static void dump_job(prte_job_t *jdata)
{
    pmix_proc_t pc;
    prte_proc_t *proc;
    pmix_byte_object_t bo;
    char *st;
    int i;

    PMIX_LOAD_PROCID(&pc, jdata->nspace, PMIX_RANK_WILDCARD);
    pmix_asprintf(&st, "DATA FOR JOB: %s\n", PRTE_JOBID_PRINT(jdata->nspace));
    bo.bytes = st;
    bo.size = strlen(st);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
    free(st);
    pmix_asprintf(&st, "\tNum apps: %d\tNum procs: %d\tJobState: %s\tAbort: %s\n",
                  (int) jdata->num_apps, (int) jdata->num_procs, prte_job_state_to_str(jdata->state),
                  (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ABORTED)) ? "True" : "False");
    bo.bytes = st;
    bo.size = strlen(st);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
    free(st);
    pmix_asprintf(&st, "\tNum launched: %ld\tNum reported: %ld\tNum terminated: %ld\n\n\tProcs:\n",
                  (long) jdata->num_launched, (long) jdata->num_reported,
                  (long) jdata->num_terminated);
    bo.bytes = st;
    bo.size = strlen(st);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
    free(st);
    for (i = 0; i < jdata->procs->size; i++) {
        if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, i))) {
            pmix_asprintf(&st, "\t\tRank: %s\tNode: %s\tPID: %u\tState: %s\tExitCode %d\n",
                          PRTE_VPID_PRINT(proc->name.rank),
                          (NULL == proc->node) ? "UNKNOWN" : proc->node->name,
                          (unsigned int) proc->pid, prte_proc_state_to_str(proc->state),
                          proc->exit_code);
            bo.bytes = st;
            bo.size = strlen(st);
            PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
            free(st);
        }
    }
    st = "\n";
    bo.bytes = st;
    bo.size = strlen(st);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
}

static int get_traces(prte_job_t *jdata)
{
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_GET_STACK_TRACES;
    pmix_data_buffer_t buffer;
    pmix_byte_object_t bo;
    pmix_proc_t pc;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&pc, jdata->nspace, PMIX_RANK_WILDCARD);
    bo.bytes = "Waiting for stack traces (this may take a few moments)...\n";
    bo.size = strlen(bo.bytes);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);


    /* setup the buffer */
    PMIX_DATA_BUFFER_CONSTRUCT(&buffer);
    /* pack the command */
    rc = PMIx_Data_pack(NULL, &buffer, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&buffer);
        return PRTE_ERROR;
    }
    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, &buffer, &jdata->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&buffer);
        return PRTE_ERROR;
    }
    /* goes to all daemons */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &buffer))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&buffer);
        return PRTE_ERROR;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buffer);
    return PRTE_SUCCESS;
}

static void job_timeout_cb(int fd, short event, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t *) cbdata;
    prte_timer_t *timer = NULL;
    prte_proc_t *prc;
    prte_job_t *child;
    pmix_proc_t pc;
    int rc, timeout, *tp, i;
    pmix_pointer_array_t parray;
    pmix_byte_object_t bo;
    char *st;
    PRTE_HIDE_UNUSED_PARAMS(fd, event);

    PMIX_ACQUIRE_OBJECT(jdata);

    /* Display a useful message to the user */
    tp = &timeout;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT, (void **) &tp, PMIX_INT)) {
        /* This shouldn't happen, but at least don't segv / display
         *something* if it does */
        timeout = -1;
    }
    pmix_asprintf(&st, "--------------------------------------------------------------------------\n"
                       "The user-provided time limit for job execution has been reached:\n\n"
                       "  Timeout: %d seconds\n\n"
                       "The job will now be aborted.  Please check your code and/or\n"
                       "adjust/remove the job execution time limit (as specified by --timeout\n"
                       "command line option or MPIEXEC_TIMEOUT environment variable).\n"
                       "--------------------------------------------------------------------------\n",
                  timeout);
    bo.bytes = st;
    bo.size = strlen(st);
    PMIX_LOAD_PROCID(&pc, jdata->nspace, PMIX_RANK_WILDCARD);
    PMIx_server_IOF_deliver(&pc, PMIX_FWD_STDERR_CHANNEL, &bo, NULL, 0, NULL, NULL);
    free(st);
    PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_TIMEOUT);

    /* see if they want proc states reported */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_STATE, NULL, PMIX_BOOL)) {
        /* output the results - note that the output might need to go to a
         * tool instead of just to stderr, so we use the PMIx IOF deliver
         * function to ensure it gets where it needs to go. */
        dump_job(jdata);
    }

    /* Do this for all its child jobs, if any */
    PMIX_LIST_FOREACH(child, &jdata->children, prte_job_t) {
        dump_job(child);
    }

    /* see if they want stacktraces */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_STACKTRACES, NULL, PMIX_BOOL)) {
        /* if they asked for stack_traces, attempt to get them, but timeout
         * if we cannot do so */
        rc = get_traces(jdata);
        if (PRTE_SUCCESS != rc) {
            goto giveup;
        }
        // get traces for child jobs too
        PMIX_LIST_FOREACH(child, &jdata->children, prte_job_t) {
            rc = get_traces(child);
            if (PRTE_SUCCESS != rc) {
                goto giveup;
            }
        }

        /* we will terminate after we get the stack_traces, but set a timeout
         * just in case we never hear back from everyone */
        if (prte_stack_trace_wait_timeout > 0) {
            timer = PMIX_NEW(prte_timer_t);
            prte_event_evtimer_set(prte_event_base, timer->ev, stack_trace_timeout, jdata);
            timer->tv.tv_sec = prte_stack_trace_wait_timeout;
            timer->tv.tv_usec = 0;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TRACE_TIMEOUT_EVENT,
                               PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
            PMIX_POST_OBJECT(timer);
            prte_event_evtimer_add(timer->ev, &timer->tv);
        }
        return;
    }

giveup:
    /* abort the job */
    PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
    pmix_pointer_array_init(&parray,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);

    prc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&prc->name, jdata->nspace, PMIX_RANK_WILDCARD);
    pmix_pointer_array_add(&parray, prc);
    PMIX_LIST_FOREACH(child, &jdata->children, prte_job_t) {
        prc = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&prc->name, child->nspace, PMIX_RANK_WILDCARD);
        pmix_pointer_array_add(&parray, prc);
    }
    if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(&parray))) {
        PRTE_ERROR_LOG(rc);
    }
    for (i=0; i < parray.size; i++) {
        prc = (prte_proc_t *) pmix_pointer_array_get_item(&parray, i);
        if (NULL == prc) {
            continue;
        }
        pmix_pointer_array_set_item(&parray, i, NULL);
        PMIX_RELEASE(prc);
    }
    PMIX_DESTRUCT(&parray);
}


void prte_plm_base_setup_job(int fd, short args, void *cbdata)
{
    int rc;
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_timer_t *timer = NULL;
    int time, *tp;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:setup_job",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (PRTE_JOB_STATE_INIT != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* start by getting a jobid */
    if (PMIX_NSPACE_INVALID(caddy->jdata->nspace)) {
        if (PRTE_SUCCESS != (rc = prte_plm_base_create_jobid(caddy->jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
            PMIX_RELEASE(caddy);
            return;
        }
    }

    /* if the spawn operation has a timeout assigned to it, setup the timer for it */
    tp = &time;
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_SPAWN_TIMEOUT, (void **) &tp, PMIX_INT)) {
        /* setup a timer to monitor execution time */
        timer = PMIX_NEW(prte_timer_t);
        timer->payload = caddy->jdata;
        prte_event_evtimer_set(prte_event_base, timer->ev, spawn_timeout_cb, caddy->jdata);
        timer->tv.tv_sec = time;
        timer->tv.tv_usec = 0;
        prte_set_attribute(&caddy->jdata->attributes, PRTE_SPAWN_TIMEOUT_EVENT, PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
        PMIX_POST_OBJECT(timer);
        prte_event_evtimer_add(timer->ev, &timer->tv);
    }

    /* if the overall job has a timeout assigned to it, setup the timer for it */
    tp = &time;
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_TIMEOUT, (void **) &tp, PMIX_INT)) {
        /* setup a timer to monitor execution time */
        timer = PMIX_NEW(prte_timer_t);
        timer->payload = caddy->jdata;
        prte_event_evtimer_set(prte_event_base, timer->ev, job_timeout_cb, caddy->jdata);
        timer->tv.tv_sec = time;
        timer->tv.tv_usec = 0;
        prte_set_attribute(&caddy->jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, PRTE_ATTR_LOCAL, timer, PMIX_POINTER);
        PMIX_POST_OBJECT(timer);
        prte_event_evtimer_add(timer->ev, &timer->tv);
    }

    // if we are not going to launch this job, then ensure we output something - otherwise,
    // we will simply silently exit
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) &&
        !prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) &&
        !prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL) &&
        !prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_REPORT_BINDINGS, NULL, PMIX_BOOL)) {
        // default to the devel map
        prte_set_attribute(&caddy->jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, PRTE_ATTR_GLOBAL,
                           NULL, PMIX_BOOL);
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_INIT_COMPLETE);

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_plm_base_setup_job_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    /* nothing to do here but move along */
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_ALLOCATE);
    PMIX_RELEASE(caddy);
}

void prte_plm_base_complete_setup(int fd, short args, void *cbdata)
{
    prte_job_t *jdata;
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    pmix_output_verbose(5, prte_plm_base_framework.framework_output, "%s complete_setup on job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(caddy->jdata->nspace));

    /* bozo check */
    if (PRTE_JOB_STATE_SYSTEM_PREP != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* convenience */
    jdata = caddy->jdata;

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_LAUNCH_APPS);

    /* cleanup */
    PMIX_RELEASE(caddy);
}


void prte_plm_base_launch_apps(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_daemon_cmd_flag_t command;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    if (PRTE_JOB_STATE_LAUNCH_APPS != caddy->job_state) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* if a shrink campaign is active, hold this job until all targeted
     * daemons have departed the DVM to avoid sending launch data to a dying
     * daemon (shrink campaigns are only ever created in elastic mode, so the
     * explicit guard keeps the non-elastic launch path identical) */
    if (prte_elastic_mode && !pmix_list_is_empty(&prte_shrink_campaigns)) {
        jdata->state = PRTE_JOB_STATE_WAITING_FOR_DAEMONS;
        PMIX_RETAIN(jdata);
        pmix_pointer_array_add(prte_prelaunch_held_jobs, jdata);
        PMIX_RELEASE(caddy);
        return;
    }

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch_apps for job %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* pack the appropriate add_local_procs command */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FIXED_DVM, NULL, PMIX_BOOL)) {
        command = PRTE_DAEMON_DVM_ADD_PROCS;
    } else {
        command = PRTE_DAEMON_ADD_LOCAL_PROCS;
    }
    rc = PMIx_Data_pack(NULL, &jdata->launch_msg, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(caddy);
        return;
    }

    /* get the local launcher's required data */
    if (PRTE_SUCCESS != (rc = prte_odls.get_add_procs_data(&jdata->launch_msg, jdata->nspace))) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
    }

    PMIX_RELEASE(caddy);
    return;
}

/* completion of the nspace registration for a do-not-launch job -
 * executes on the PRRTE progress thread */
static void donotlaunch_reg_complete(pmix_status_t status, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t *) cbdata;

    if (PMIX_SUCCESS != status) {
        PRTE_ERROR_LOG(prte_pmix_convert_status(status));
    }
    /* if we are persistent, then we remain alive - otherwise, declare
     * all jobs complete and terminate */
    if (prte_persistent) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
    } else {
        prte_never_launched = true;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALL_JOBS_COMPLETE);
    }
}

void prte_plm_base_send_launch_msg(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* convenience */
    jdata = caddy->jdata;

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:send launch msg for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));

    /* if we don't want to launch the apps, now is the time to leave */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* go ahead and register the job - the completion callback
         * advances the job state once the registration is done */
        rc = prte_pmix_server_register_nspace(jdata, donotlaunch_reg_complete, jdata);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            /* the callback will never fire - advance the state
             * ourselves */
            donotlaunch_reg_complete(PMIX_SUCCESS, jdata);
        }
        PMIX_RELEASE(caddy);
        return;
    }

    /* goes to all daemons */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &jdata->launch_msg))) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(caddy);
        return;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&jdata->launch_msg);
    PMIX_DATA_BUFFER_CONSTRUCT(&jdata->launch_msg);

    /* track that we automatically are considered to have reported - used
     * only to report launch progress
     */
    caddy->jdata->num_daemons_reported++;

    /* cleanup */
    PMIX_RELEASE(caddy);
}

int prte_plm_base_spawn_response(int32_t status, prte_job_t *jdata)
{
    int rc;
    pmix_data_buffer_t *answer;
    int room, *rmptr;
    pmix_info_t *iptr;
    size_t ninfo;
    time_t timestamp;
    pmix_proc_t *nptr;
    void *tinfo;
    int n;
    char *name;
    pmix_data_array_t darray;
    prte_app_context_t *app;

    /* if the requestor simply told us to terminate, they won't
     * be waiting for a response */
    if (PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        return PRTE_SUCCESS;
    }

    /* if the response has already been sent, don't do it again */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED, NULL, PMIX_BOOL)) {
        return PRTE_SUCCESS;
    }

    /* if the requestor was a tool, use PMIx to notify them of
     * launch complete as they won't be listening on PRRTE oob */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, NULL, PMIX_BOOL)) {

        /* dvm job => launch was requested by a TOOL, so we notify the launch proxy
         * and NOT the originator (as that would be us) */
        nptr = NULL;
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC) ||
            NULL == nptr) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }

        /* direct an event back to our controller */
        timestamp = time(NULL);
        PMIX_INFO_LIST_START(tinfo);
        /* target this notification solely to that one tool */
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_EVENT_CUSTOM_RANGE, nptr, PMIX_PROC);
        PMIX_PROC_RELEASE(nptr);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_NSPACE, jdata->nspace, PMIX_STRING);
        for (n=0; n < jdata->apps->size; n++) {
            app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
            if (NULL == app) {
                continue;
            }
            /* if pset name was assigned, pass it */
            if (prte_get_attribute(&app->attributes, PRTE_APP_PSET_NAME, (void**) &name, PMIX_STRING)) {
                PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_PSET_NAME, name, PMIX_STRING);
                free(name);
            }
            /* pass the argv from each app */
            name = PMIx_Argv_join(app->argv, ' ');
            PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_APP_ARGV, name, PMIX_STRING);
            free(name);
        }

        /* not to be delivered to a default event handler */
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        /* provide the timestamp */
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_EVENT_TIMESTAMP, &timestamp, PMIX_TIME);
        /* protect against loops */
        PMIX_INFO_LIST_ADD(rc, tinfo, "prte.notify.donotloop", NULL, PMIX_BOOL);
        PMIX_INFO_LIST_CONVERT(rc, tinfo, &darray);
        if (PMIX_ERR_EMPTY == rc) {
            iptr = NULL;
            ninfo = 0;
        } else if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PRTE_UPDATE_EXIT_STATUS(rc);
            PMIX_INFO_LIST_RELEASE(tinfo);
            PMIX_PROC_RELEASE(nptr);
            return rc;
        } else {
            iptr = (pmix_info_t *) darray.array;
            ninfo = darray.size;
        }
        PMIX_INFO_LIST_RELEASE(tinfo);
        PMIx_Notify_event(PMIX_LAUNCH_COMPLETE, &prte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          iptr, ninfo, NULL, NULL);
        PMIX_INFO_FREE(iptr, ninfo);
    }

    rmptr = &room;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_ROOM_NUM, (void **) &rmptr, PMIX_INT)) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* if the originator is me, then just do the notification */
    if (PMIX_CHECK_PROCID(&jdata->originator, PRTE_PROC_MY_NAME)) {
        pmix_server_notify_spawn(jdata->nspace, room, status);
        return PRTE_SUCCESS;
    }

    /* prep the response to the spawn requestor */
    PMIX_DATA_BUFFER_CREATE(answer);

    /* pack the status */
    rc = PMIx_Data_pack(NULL, answer, &status, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return prte_pmix_convert_status(rc);
    }
    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, answer, &jdata->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return prte_pmix_convert_status(rc);
    }
    /* pack the room number */
    rc = PMIx_Data_pack(NULL, answer, &room, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return prte_pmix_convert_status(rc);
    }

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch sending dyn release of job %s to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                         PRTE_NAME_PRINT(&jdata->originator)));
    PRTE_RML_SEND(rc, jdata->originator.rank, answer, PRTE_RML_TAG_LAUNCH_RESP);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return rc;
    }

    return PRTE_SUCCESS;
}

void prte_plm_base_post_launch(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    int32_t rc, n;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_app_context_t *app;
    prte_timer_t *timer;
    char *file = NULL;
    FILE *fp;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    /* if a timer was defined, cancel it */
    if (prte_get_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT_EVENT, (void **) &timer, PMIX_POINTER)) {
        prte_event_evtimer_del(timer->ev);
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch deleting spawn timeout for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));
        PMIX_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT_EVENT);
    }

    if (PRTE_JOB_STATE_RUNNING != caddy->job_state) {
        /* error mgr handles this */
        PMIX_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* complete wiring up the iof */
    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch wiring up iof for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));

    /* if requested, output the proctable */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_PROCTABLE, (void**)&file, PMIX_STRING)) {
        /* if file="-", then output to stdout */
        if (0 == strcmp(file, "-")) {
            fp = stdout;
        } else if (0 == strcmp(file, "+")) {
            fp = stderr;
        } else {
            /* attempt to open the specified file */
            fp = fopen(file, "w");
            if (NULL == fp) {
                pmix_output(0, "Unable to open file %s for output of proctable", file);
                goto next;
            }
        }
        for (n=0; n < jdata->procs->size; n++) {
            proc = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, n);
            if (NULL == proc) {
                continue;
            }
            app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, proc->app_idx);
            if (NULL == app) {
                // should never happen
                continue;
            }
            fprintf(fp, "(rank, host, exe, pid) = (%u, %s, %s, %d)\n",
                    proc->name.rank, proc->node->name, app->app, proc->pid);
        }
        if (stdout != fp && stderr != fp) {
            fclose(fp);
        }
    }

next:
    /* notify the spawn requestor */
    rc = prte_plm_base_spawn_response(PRTE_SUCCESS, jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_plm_base_registered(int fd, short args, void *cbdata)
{
    prte_job_t *jdata;
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:launch %s registered", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    if (PRTE_JOB_STATE_REGISTERED != caddy->job_state) {
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:launch job %s not registered - state %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                             prte_job_state_to_str(caddy->job_state)));
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_RELEASE(caddy);
        return;
    }
    /* update job state */
    jdata->state = caddy->job_state;

    PMIX_RELEASE(caddy);
}

/* daemons callback when they start - need to listen for them */
static void progress_daemons(prte_job_t *daemons,
                             bool show_progress)
{
    int i;
    prte_job_t *jdata;
    prte_proc_t *daemon;
    prte_topology_t *t;
    pmix_rank_t j;

    if (show_progress &&
        (0 == daemons->num_reported % 100 ||
         daemons->num_reported == prte_process_info.num_daemons)) {
        PRTE_ACTIVATE_JOB_STATE(daemons, PRTE_JOB_STATE_REPORT_PROGRESS);
    }
    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:progress_daemons recvd %d of %d reported daemons",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), daemons->num_reported,
                         daemons->num_procs));

    if (daemons->num_procs == daemons->num_reported) {
        bool oneactivated = false;
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        /* activate the daemons_reported state for all jobs
         * whose daemons were launched
         */
        for (i = 1; i < prte_job_data->size; i++) {
            jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
            if (NULL == jdata) {
                continue;
            }
            if (PRTE_JOB_STATE_DAEMONS_LAUNCHED == jdata->state) {
                oneactivated = true;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
            }
        }
        if (!oneactivated) {
            /* must be launching a DVM - activate the state */
            PRTE_ACTIVATE_JOB_STATE(daemons, PRTE_JOB_STATE_DAEMONS_REPORTED);
        }
        if (prte_homo_nodes && 1 < daemons->num_procs) {
            // ensure that all topologies point to the one
            // returned by daemon rank=1 as it might be different
            // from where prterun is executing (e.g., login node)
            daemon = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, 1);
            if (NULL == daemon->node->topology) {
                // should never happen
            }
            t = daemon->node->topology;
            for (j=2; j < daemons->num_procs; j++) {
                daemon = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, j);
                if (NULL == daemon) {
                    continue;
                }
                daemon->node->topology = t;
                /* update the node's available processors */
                if (NULL != daemon->node->available) {
                    hwloc_bitmap_free(daemon->node->available);
                }
                daemon->node->available = prte_hwloc_base_filter_cpus(t->topo);
            }
        }
    }
}

void prte_plm_base_daemon_callback(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tag, void *cbdata)
{
    char *ptr;
    int idx;
    pmix_status_t ret;
    prte_proc_t *daemon = NULL;
    pmix_proc_t dname;
    prte_topology_t *t;
    int i;
    bool found, show_progress;
    char *alias;
    char *nodename = NULL;
    pmix_byte_object_t pbo, bo;
    bool compressed;
    pmix_data_buffer_t datbuf, *data;
    pmix_topology_t ptopo;
    pmix_value_t cnctinfo;
    hwloc_topology_diff_t diff;
    bool prted_failed_launch = false;
    prte_job_t *jdatorted = NULL;

    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    /* get the daemon job */
    jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    show_progress = prte_get_attribute(&jdatorted->attributes, PRTE_JOB_SHOW_PROGRESS, NULL, PMIX_BOOL);

    /* multiple daemons could be in this buffer, so unpack until we exhaust the data */
    idx = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &dname, &idx, PMIX_PROC);
    while (PMIX_SUCCESS == ret) {

        pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                            "%s plm:base:prted_report_launch from daemon %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&dname));

        /* update state and record for this daemon contact info */
        daemon = (prte_proc_t *) pmix_pointer_array_get_item(jdatorted->procs, dname.rank);
        if (NULL == daemon) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* A returning daemon (the bootstrap unheal path): its node rebooted and
         * it is reporting in again after we recorded it COMM_FAILED and
         * decremented num_daemons on its death. Restore the count so the nidmap
         * span [0, num_daemons) and every daemon-count-derived computation are
         * correct again -- otherwise the launch nidmap encodes a shorter span
         * than the live daemon set and re-poisons the returned daemon (it decodes
         * its own rank, or a peer beyond the truncated span, as a dead hole).
         * Only a daemon that had actually failed reaches this: a first launch or
         * a grow target is not COMM_FAILED here, so formation and grow are
         * unaffected, and once set RUNNING below a duplicate report cannot
         * double-count. Gated on bootstrap, the only mode a daemon can return in. */
        if (prte_bootstrap_setup && PRTE_PROC_STATE_COMM_FAILED == daemon->state) {
            ++prte_process_info.num_daemons;
        }
        daemon->state = PRTE_PROC_STATE_RUNNING;
        /* record that this daemon is alive */
        PRTE_FLAG_SET(daemon, PRTE_PROC_FLAG_ALIVE);
        /* unload its contact info */
        PMIX_VALUE_CONSTRUCT(&cnctinfo);
        cnctinfo.type = PMIX_STRING;
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &cnctinfo.data.string, &idx, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* store this for later distribution */
        ret = PMIx_Store_internal(&dname, PMIX_PROC_URI, &cnctinfo);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_VALUE_DESTRUCT(&cnctinfo);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        daemon->rml_uri = strdup(cnctinfo.data.string);
        PMIX_VALUE_DESTRUCT(&cnctinfo);

        /* unpack the node name */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &nodename, &idx, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }

        if (!pmix_net_isaddr(nodename) &&
            NULL != (ptr = strchr(nodename, '.'))) {
            /* retain the non-fqdn name as an alias */
            *ptr = '\0';
            PMIx_Argv_append_unique_nosize(&daemon->node->aliases, nodename);
            *ptr = '.';
        }

        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:prted_report_launch from daemon %s on node %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&daemon->name),
                             nodename));

        /* mark the daemon as launched */
        PRTE_FLAG_SET(daemon->node, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
        daemon->node->state = PRTE_NODE_STATE_UP;

        /* first, store the nodename itself. in case the nodename isn't
         * the same as what we were given by the allocation, we replace
         * the node's name with the returned value and store the allocation
         * value as an alias. For example, a hostfile
         * might contain an IP address instead of the value returned
         * by gethostname, yet the daemon will have returned the latter
         * and apps may refer to the host by that name
         */
        if (0 != strcmp(nodename, daemon->node->name)) {
            PMIx_Argv_append_unique_nosize(&daemon->node->aliases, daemon->node->name);
            free(daemon->node->name);
            daemon->node->name = strdup(nodename);
        }

        /* unpack and store the provided aliases */
        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &alias, &idx, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        if (NULL != alias) {
            daemon->node->aliases = PMIx_Argv_split(alias, ',');
            free(alias);
        }

        if (0 < pmix_output_get_verbosity(prte_plm_base_framework.framework_output)) {
            int ni;
            pmix_output(0, "ALIASES FOR NODE %s (%s)", daemon->node->name, nodename);
            if (NULL != daemon->node->aliases) {
                for (ni=0; NULL != daemon->node->aliases[ni]; ni++) {
                    pmix_output(0, "\tALIAS: %s", daemon->node->aliases[ni]);
                }
            }
        }

        if (!prte_homo_nodes || 1 == daemon->name.rank) {
            /* unpack the topology for that node */
            PMIX_DATA_BUFFER_CONSTRUCT(&datbuf);
            /* unpack the flag to see if this payload is compressed */
            idx = 1;
            ret = PMIx_Data_unpack(NULL, buffer, &compressed, &idx, PMIX_BOOL);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* unpack the data */
            idx = 1;
            ret = PMIx_Data_unpack(NULL, buffer, &pbo, &idx, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            if (compressed) {
                /* decompress the data */
                if (PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size,
                                         (uint8_t **) &bo.bytes, &bo.size)) {
                    /* the data has been uncompressed */
                    ret = PMIx_Data_load(&datbuf, &bo);
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        prted_failed_launch = true;
                        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                        goto CLEANUP;
                    }
                } else {
                    pmix_show_help("help-prte-runtime.txt", "failed-to-uncompress",
                                   true, prte_process_info.nodename);
                    prted_failed_launch = true;
                    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                    goto CLEANUP;
                }
            } else {
                ret = PMIx_Data_load(&datbuf, &pbo);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    prted_failed_launch = true;
                    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                    goto CLEANUP;
                }
            }
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            data = &datbuf;

            /* unpack the topology information */
            idx = 1;
            ret = PMIx_Data_unpack(NULL, data, &ptopo, &idx, PMIX_TOPO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            /* cleanup */
            PMIX_DATA_BUFFER_DESTRUCT(data);

            pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                                "%s RECEIVED TOPOLOGY FROM NODE %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nodename);

            /* check to see if we already have this topology from some other node,
             * and if we do, record the diff for this node  */
            found = false;
            for (i = 0; i < prte_node_topologies->size; i++) {
                t = (prte_topology_t *) pmix_pointer_array_get_item(prte_node_topologies, i);
                if (NULL == t) {
                    continue;
                }
                /* compute the diff */
                ret = hwloc_topology_diff_build(t->topo, ptopo.topology, 0, &diff);
                if (0 == ret) {
                    pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                                        "%s TOPOLOGY ALREADY RECORDED IN POSN %d - %s DIFFS FOUND",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), i,
                                        (NULL == diff) ? "NO" : "SOME");
                    daemon->node->topology = t;
                    found = true;
                    /* update the node's available processors */
                    if (NULL != daemon->node->available) {
                        hwloc_bitmap_free(daemon->node->available);
                    }
                    daemon->node->available = prte_hwloc_base_filter_cpus(t->topo);
                    daemon->node->topodiff = diff;
                    // release the unpacked topology
                    PMIX_TOPOLOGY_DESTRUCT(&ptopo);
                    break;
                }
            }
            if (!found) {
                // this is a new topology
                t = PMIX_NEW(prte_topology_t);
                t->topo = ptopo.topology;
                // we don't use the source field
                if (NULL != ptopo.source) {
                    free(ptopo.source);
                }
                t->index = pmix_pointer_array_add(prte_node_topologies, t);
                pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                                    "%s ADDING NEW TOPOLOGY AT POSN %d",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), t->index);
                daemon->node->topology = t;
                daemon->node->available = prte_hwloc_base_filter_cpus(t->topo);
                prte_hwloc_base_setup_summary(t->topo);
            }
        }

        // mark as completed
        jdatorted->num_reported++;
        jdatorted->num_daemons_reported++;


    CLEANUP:
        pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                            "%s plm:base:prted_daemon_cback %s for daemon %s at contact %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            prted_failed_launch ? "failed" : "completed", PRTE_NAME_PRINT(&dname),
                            (NULL == daemon) ? "UNKNOWN" : daemon->rml_uri);

        if (NULL != nodename) {
            free(nodename);
            nodename = NULL;
        }

        idx = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &dname, &idx, PMIX_PROC);
        if (PMIX_SUCCESS != ret) {
            break;
        }
    }

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_FAILED_TO_START);
        return;
    }
    if (prted_failed_launch) {
        PRTE_ACTIVATE_JOB_STATE(jdatorted, PRTE_JOB_STATE_FAILED_TO_START);
        return;
    }
    progress_daemons(jdatorted, show_progress);
}

void prte_plm_base_daemon_failed(int st, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                 prte_rml_tag_t tag, void *cbdata)
{
    int status, rc;
    int32_t n;
    pmix_rank_t vpid;
    prte_proc_t *daemon = NULL;
    prte_job_t *jdatorted;
    PRTE_HIDE_UNUSED_PARAMS(st, sender, tag, cbdata);

    /* get the daemon job */
    jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* unpack the daemon that failed */
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &vpid, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
        goto finish;
    }

    /* unpack the exit status */
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &status, &n, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = PRTE_ERROR_DEFAULT_EXIT_CODE;
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
    } else {
        PRTE_UPDATE_EXIT_STATUS(WEXITSTATUS(status));
    }

    /* find the daemon and update its state/status */
    if (NULL == (daemon = (prte_proc_t *) pmix_pointer_array_get_item(jdatorted->procs, vpid))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto finish;
    }
    daemon->state = PRTE_PROC_STATE_FAILED_TO_START;
    daemon->exit_code = status;

finish:
    if (NULL == daemon) {
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_PROC_STATE_FAILED_TO_START);
        return;
    } else {
        PRTE_ACTIVATE_PROC_STATE(&daemon->name, PRTE_PROC_STATE_FAILED_TO_START);
    }
}

int prte_plm_base_setup_prted_cmd(int *argc, char ***argv)
{
    int i, loc;
    char **tmpv;

    /* set default location to be 0, indicating that
     * only a single word is in the cmd
     */
    loc = 0;
    /* split the command apart in case it is multi-word */
    tmpv = PMIx_Argv_split(prte_launch_agent, ' ');
    for (i = 0; NULL != tmpv && NULL != tmpv[i]; ++i) {
        if (0 == strcmp(tmpv[i], "prted")) {
            loc = i;
        }
        pmix_argv_append(argc, argv, tmpv[i]);
    }
    PMIx_Argv_free(tmpv);

    return loc;
}

/* pass all options as MCA params so anything we pickup
 * from the environment can be checked for duplicates
 */
int prte_plm_base_prted_append_basic_args(int *argc, char ***argv, char *ess, int *proc_vpid_index)
{
    char *param = NULL, **tmpv;
    int i, j, cnt, offset;
    prte_job_t *jdata;
    unsigned long num_procs;
    bool ignore;
    char *skips[] = {
        "rmaps",
        "ras",
        NULL
    };

    /* check for debug flags */
    if (prte_debug_flag) {
        pmix_argv_append(argc, argv, "--debug");
    }
    if (prte_debug_daemons_flag) {
        pmix_argv_append(argc, argv, "--debug-daemons");
    }
    if (prte_debug_daemons_file_flag) {
        pmix_argv_append(argc, argv, "--debug-daemons-file");
    }
    if (prte_leave_session_attached) {
        pmix_argv_append(argc, argv, "--leave-session-attached");
    }
    if (prte_allow_run_as_root) {
        pmix_argv_append(argc, argv, "--allow-run-as-root");
    }
    if (prte_homo_nodes) {
        pmix_argv_append(argc, argv, "--uniform-nodes");
    }

    /* the following is not an mca param */
    if (NULL != getenv("PRTE_TEST_PRTED_SUICIDE")) {
        pmix_argv_append(argc, argv, "--test-suicide");
    }

    /* tell the orted what ESS component to use */
    if (NULL != ess) {
        pmix_argv_append(argc, argv, "--prtemca");
        pmix_argv_append(argc, argv, "ess");
        pmix_argv_append(argc, argv, ess);
    }

    /* pass the daemon nspace */
    pmix_argv_append(argc, argv, "--prtemca");
    pmix_argv_append(argc, argv, "ess_base_nspace");
    pmix_argv_append(argc, argv, prte_process_info.myproc.nspace);
    free(param);

    /* setup to pass the vpid */
    if (NULL != proc_vpid_index) {
        pmix_argv_append(argc, argv, "--prtemca");
        pmix_argv_append(argc, argv, "ess_base_vpid");
        *proc_vpid_index = *argc;
        pmix_argv_append(argc, argv, "<template>");
    }

    /* pass the total number of daemons that will be in the system */
    if (PRTE_PROC_IS_MASTER) {
        jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        num_procs = jdata->num_procs;
    } else {
        num_procs = prte_process_info.num_daemons;
    }
    pmix_argv_append(argc, argv, "--prtemca");
    pmix_argv_append(argc, argv, "ess_base_num_procs");
    pmix_asprintf(&param, "%lu", num_procs);
    pmix_argv_append(argc, argv, param);
    free(param);

    /* pass the HNP uri */
    pmix_argv_append(argc, argv, "--prtemca");
    pmix_argv_append(argc, argv, "prte_hnp_uri");
    pmix_argv_append(argc, argv, prte_process_info.my_hnp_uri);

    /* if --xterm was specified, pass that along */
    if (NULL != prte_xterm) {
        pmix_argv_append(argc, argv, "--prtemca");
        pmix_argv_append(argc, argv, "prte_xterm");
        pmix_argv_append(argc, argv, prte_xterm);
    }

    /* look for any envars that relate to us and pass
     * them along on the cmd line */
    offset = strlen("PRTE_MCA_");
    for (i=0; NULL != environ[i]; i++) {
        if (0 == strncmp(environ[i], "PMIX_MCA_", offset) ||
            0 == strncmp(environ[i], "PRTE_MCA_", offset)) {
            tmpv = PMIx_Argv_split(environ[i], '=');
            ignore = false;
            for (j=0; NULL != skips[j]; j++) {
                if (0 == strncmp(&tmpv[0][offset], skips[j], strlen(skips[j])) ||
                    0 == strcmp(&tmpv[0][offset], "plm")) {
                    ignore = true;;
                    break;
                }
            }
            if (ignore) {
                continue;
            }

            /* check for duplicate */
            ignore = false;
            for (j = 0; j < *argc; j++) {
                if (0 == strcmp((*argv)[j], &tmpv[0][offset])) {
                    ignore = true;
                    break;
                }
            }
            if (!ignore) {
                /* pass it along */
                if (0 == strncmp(tmpv[0], "PRTE_MCA_", offset)) {
                    pmix_argv_append(argc, argv, "--prtemca");
                } else {
                    pmix_argv_append(argc, argv, "--pmixmca");
                }
                pmix_argv_append(argc, argv, &tmpv[0][offset]);
                pmix_argv_append(argc, argv, tmpv[1]);
            }
            PMIx_Argv_free(tmpv);
        }
    }

    /* pass along any cmd line MCA params provided to mpirun,
     * being sure to "purge" any that would cause problems
     * on backend nodes and ignoring all duplicates
     */
    cnt = PMIx_Argv_count(prted_cmd_line);
    for (i = 0; i < cnt; i += 3) {
        /* if the specified option is more than one word, we don't
         * have a generic way of passing it as some environments ignore
         * any quotes we add, while others don't - so we ignore any
         * such options. In most cases, this won't be a problem as
         * they typically only apply to things of interest to the HNP.
         * Individual environments can add these back into the cmd line
         * as they know if it can be supported
         */
        if (NULL != strchr(prted_cmd_line[i + 2], ' ')) {
            continue;
        }
        /* The daemon will attempt to open the PLM on the remote
         * end. Only a few environments allow this, so the daemon
         * only opens the PLM -if- it is specifically told to do
         * so by giving it a specific PLM module. To ensure we avoid
         * confusion, do not include any directives here, and ignore
         * any frameworks the daemons do not use
         */
        ignore = false;
        for (j=0; NULL != skips[j]; j++) {
            if (0 == strncmp(prted_cmd_line[i + 1], skips[j], strlen(skips[j])) ||
                0 == strcmp(prted_cmd_line[i + 1], "plm")) {
                ignore = true;;
                break;
            }
        }
        if (ignore) {
            continue;
        }

        /* check for duplicate */
        ignore = false;
        for (j = 0; j < *argc; j++) {
            if (0 == strcmp((*argv)[j], prted_cmd_line[i + 1])) {
                ignore = true;
                break;
            }
        }
        if (!ignore) {
            /* pass it along */
            pmix_argv_append(argc, argv, prted_cmd_line[i]);
            pmix_argv_append(argc, argv, prted_cmd_line[i + 1]);
            pmix_argv_append(argc, argv, prted_cmd_line[i + 2]);
        }
    }

    return PRTE_SUCCESS;
}

void prte_plm_base_wrap_args(char **args)
{
    int i;
    char *tstr;

    for (i = 0; NULL != args && NULL != args[i]; i++) {
        /* if the arg ends in "mca", then we wrap its arguments */
        if (strlen(args[i]) > 3 && 0 == strcmp(args[i] + strlen(args[i]) - 3, "mca")) {
            /* it was at the end */
            if (NULL == args[i + 1] || NULL == args[i + 2]) {
                /* this should be impossible as the error would
                 * have been detected well before here, but just
                 * be safe */
                return;
            }
            i += 2;
            /* if the argument already has quotes, then leave it alone */
            if ('\"' == args[i][0]) {
                continue;
            }
            pmix_asprintf(&tstr, "\"%s\"", args[i]);
            free(args[i]);
            args[i] = tstr;
        }
    }
}

int prte_plm_base_setup_virtual_machine(prte_job_t *jdata)
{
    prte_node_t *node, *nptr;
    prte_proc_t *proc, *pptr;
    prte_job_map_t *map = NULL;
    int rc, i;
    prte_job_t *daemons;
    pmix_list_t nodes, tnodes;
    pmix_list_item_t *item, *next;
    prte_app_context_t *app;
    bool one_filter = false;
    int num_nodes;
    bool default_hostfile_used;
    char *hosts = NULL;
    bool singleton = false;
    bool multi_sim = false;

    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:base:setup_vm",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (NULL == (daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    if (NULL == daemons->map) {
        daemons->map = PMIX_NEW(prte_job_map_t);
    }
    map = daemons->map;

    /* if this job is being launched against a fixed DVM, then there is
     * nothing for us to do - the DVM will stand as is */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FIXED_DVM, NULL, PMIX_BOOL)) {
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        map->num_new_daemons = 0;
        return PRTE_SUCCESS;
    }

    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_EXTEND_DVM, NULL, PMIX_BOOL)) {
        // nodes have been added, so extend the DVM
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_EXTEND_DVM);
        /* Reset the per-launch daemon accounting. The initial-VM path zeroes
         * these further below, but the grow path jumps straight to construct:
         * and would otherwise accumulate num_new_daemons across successive
         * grows and reuse a stale daemon_vpid_start - corrupting the grow
         * campaign's target list and suppressing its completion event (#2491). */
        map->num_new_daemons = 0;
        map->daemon_vpid_start = PMIX_RANK_INVALID;
        goto construct;
    }

    /* if this is a dynamic spawn, then we don't make any changes to
     * the virtual machine unless specifically requested to do so
     */
    if (!PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        if (0 == map->num_nodes) {
            PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm creating map",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* this is the first time thru, so the vm is just getting
             * defined - create a map for it and put us in as we
             * are obviously already here! The ess will already
             * have assigned our node to us.
             */
            node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
            if (NULL == node) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return PRTE_ERR_NOT_FOUND;
            }
            pmix_pointer_array_add(map->nodes, (void *) node);
            ++(map->num_nodes);
            /* maintain accounting */
            PMIX_RETAIN(node);
            /* mark that this is from a singleton */
            singleton = true;
        }
        for (i = 1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            /* only add in nodes marked as "added" */
            if (!singleton && PRTE_NODE_STATE_ADDED != node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "%s plm_base:setup_vm NODE %s WAS NOT ADDED",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
                continue;
            }
            PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                 "%s plm_base:setup_vm ADDING NODE %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PMIX_RETAIN(node);
            pmix_list_append(&nodes, &node->super);
            /* reset the state so it can be used for mapping */
            node->state = PRTE_NODE_STATE_UP;
        }
        map->num_new_daemons = 0;
        /* if we didn't get anything, then there is nothing else to
         * do as no other daemons are to be launched
         */
        if (0 == pmix_list_get_size(&nodes)) {
            PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm no new daemons required",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PMIX_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
            PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
            return PRTE_SUCCESS;
        }
        /* if we got some new nodes to launch, we need to handle it */
        goto process;
    }

    /* if we are not working with a virtual machine, then we
     * look across all jobs and ensure that the "VM" contains
     * all nodes with application procs on them
     */
    multi_sim = prte_get_attribute(&jdata->attributes, PRTE_JOB_MULTI_DAEMON_SIM, NULL, PMIX_BOOL);
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_NO_VM, NULL, PMIX_BOOL) || multi_sim) {
        /* loop across all nodes and include those that have
         * num_procs > 0 && no daemon already on them
         */
        for (i = 1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRTE_NODE_STATE_UP;
                continue;
            }
            if (PRTE_NODE_STATE_DOWN == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            if (0 < node->num_procs || multi_sim) {
                /* retain a copy for our use in case the item gets
                 * destructed along the way
                 */
                PMIX_RETAIN(node);
                pmix_list_append(&nodes, &node->super);
            }
        }
        if (multi_sim) {
            goto process;
        }
        /* see if anybody had procs */
        if (0 == pmix_list_get_size(&nodes)) {
            /* if the HNP has some procs, then we are still good */
            node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
            if (NULL == node) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return PRTE_ERR_NOT_FOUND;
            }
            if (0 < node->num_procs) {
                PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                     "%s plm:base:setup_vm only HNP in use",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PMIX_DESTRUCT(&nodes);
                map->num_nodes = 1;
                /* mark that the daemons have reported so we can proceed */
                daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
                return PRTE_SUCCESS;
            }
            /* well, if the HNP doesn't have any procs, and neither did
             * anyone else...then we have a big problem
             */
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            return PRTE_ERR_FATAL;
        }
        goto process;
    }

    if (0 == map->num_nodes) {
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm creating map",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* this is the first time thru, so the vm is just getting
         * defined - put us in as we
         * are obviously already here! The ess will already
         * have assigned our node to us.
         */
        node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }
        pmix_pointer_array_add(map->nodes, (void *) node);
        ++(map->num_nodes);
        /* maintain accounting */
        PMIX_RETAIN(node);
    }

    /* zero-out the number of new daemons as we will compute this
     * each time we are called
     */
    map->num_new_daemons = 0;

    /* if this is an unmanaged allocation, then we use
     * the nodes that were specified for the union of
     * all apps - there is no need to collect all
     * available nodes and "filter" them
     */
    if (!prte_managed_allocation) {
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s setup:vm: working unmanaged allocation",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        default_hostfile_used = false;
        PMIX_CONSTRUCT(&tnodes, pmix_list_t);
        hosts = NULL;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING)) {
            /* use the file, if provided */
            PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s using rank/seqfile %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 hosts));
            if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&tnodes, hosts))) {
                PRTE_ERROR_LOG(rc);
                free(hosts);
                return rc;
            }
            free(hosts);
        } else {
            for (i = 0; i < jdata->apps->size; i++) {
                if (NULL
                    == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                /* if the app provided a dash-host, then use those nodes */
                hosts = NULL;
                if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                                       PMIX_STRING)) {
                    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s using dash_host", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    if (PRTE_SUCCESS
                        != (rc = prte_util_add_dash_host_nodes(&tnodes, hosts, false))) {
                        PRTE_ERROR_LOG(rc);
                        free(hosts);
                        return rc;
                    }
                    free(hosts);
                } else if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts,
                                              PMIX_STRING)) {
                    /* otherwise, if the app provided a hostfile, then use that */
                    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s using hostfile %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         hosts));
                    if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&tnodes, hosts))) {
                        PRTE_ERROR_LOG(rc);
                        free(hosts);
                        return rc;
                    }
                    free(hosts);
                } else if (NULL != prte_default_hostfile) {
                    if (!default_hostfile_used) {
                        /* fall back to the default hostfile, if provided */
                        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                             "%s using default hostfile %s",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                             prte_default_hostfile));
                        if (PRTE_SUCCESS
                            != (rc = prte_util_add_hostfile_nodes(&tnodes,
                                                                  prte_default_hostfile))) {
                            PRTE_ERROR_LOG(rc);
                            return rc;
                        }
                        /* only include it once */
                        default_hostfile_used = true;
                    }
                }
            }
        }

        /* cycle thru the resulting list, finding the nodes on
         * the node pool array while removing ourselves
         * and all nodes that are down or otherwise unusable
         */
        while (NULL != (item = pmix_list_remove_first(&tnodes))) {
            nptr = (prte_node_t *) item;
            PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output, "%s checking node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nptr->name));
            for (i = 0; i < prte_node_pool->size; i++) {
                node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
                if (NULL == node) {
                    continue;
                }
                if (!prte_nptr_match(node, nptr)) {
                    continue;
                }
                /* have a match - now see if we want this node */
                /* ignore nodes that are marked as do-not-use for this mapping */
                if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                    PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_USE", node->name));
                    /* reset the state so it can be used another time */
                    node->state = PRTE_NODE_STATE_UP;
                    break;
                }
                if (PRTE_NODE_STATE_DOWN == node->state) {
                    PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED DOWN", node->name));
                    break;
                }
                if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                    PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_INCLUDE", node->name));
                    break;
                }
                /* if this node is us, ignore it */
                if (0 == node->index) {
                    PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                         "%s ignoring myself", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    break;
                }
                /* we want it - add it to list */
                PMIX_RETAIN(node);
                pmix_list_append(&nodes, &node->super);
            }
            PMIX_RELEASE(nptr);
        }
        PMIX_LIST_DESTRUCT(&tnodes);
        /* if we didn't get anything, then we are the only node in the
         * allocation - so there is nothing else to do as no other
         * daemons are to be launched
         */
        if (0 == pmix_list_get_size(&nodes)) {
            PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm only HNP in allocation",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PMIX_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
            PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
            return PRTE_SUCCESS;
        }
        /* continue processing */
        goto process;
    }

construct:
    /* construct a list of available nodes */
    for (i = 1; i < prte_node_pool->size; i++) {
        if (NULL != (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRTE_NODE_STATE_UP;
                continue;
            }
            if (PRTE_NODE_STATE_DOWN == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PMIX_RETAIN(node);
            pmix_list_append(&nodes, &node->super);
            /* by default, mark these as not to be included
             * so the filtering logic works correctly
             */
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * system - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == pmix_list_get_size(&nodes)) {
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP in allocation",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* cleanup */
        PMIX_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
        return PRTE_SUCCESS;
    }

    /* filter across the union of all app_context specs - if the HNP
     * was allocated, then we have to include
     * ourselves in case someone has specified a -host or hostfile
     * that includes the head node. We will remove ourselves later
     * as we clearly already exist
     */
    if (prte_hnp_is_allocated) {
        node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }
        PMIX_RETAIN(node);
        pmix_list_prepend(&nodes, &node->super);
    }
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_filter_nodes(app, &nodes, false))
            && rc != PRTE_ERR_TAKE_NEXT_OPTION) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRTE_SUCCESS == rc) {
            /* we filtered something */
            one_filter = true;
        }
    }

    if (one_filter) {
        /* at least one filtering option was executed, so
         * remove all nodes that were not mapped
         */
        item = pmix_list_get_first(&nodes);
        while (item != pmix_list_get_end(&nodes)) {
            next = pmix_list_get_next(item);
            node = (prte_node_t *) item;
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                pmix_list_remove_item(&nodes, item);
                PMIX_RELEASE(item);
            } else {
                /* The filtering logic sets this flag only for nodes which
                 * are kept after filtering. This flag will be subsequently
                 * used in rmaps components and must be reset here */
                PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
            }
            item = next;
        }
    }

    /* ensure we are not on the list */
    if (0 < pmix_list_get_size(&nodes)) {
        item = pmix_list_get_first(&nodes);
        node = (prte_node_t *) item;
        if (0 == node->index) {
            pmix_list_remove_item(&nodes, item);
            PMIX_RELEASE(item);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * allocation - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == pmix_list_get_size(&nodes)) {
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP left",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRTE_JOB_STATE_DAEMONS_REPORTED;
        PRTE_FLAG_UNSET(daemons, PRTE_JOB_FLAG_UPDATED);
        return PRTE_SUCCESS;
    }

process:
    /* cycle thru all available nodes and find those that do not already
     * have a daemon on them - no need to include our own as we are
     * obviously already here! If a max vm size was given, then limit
     * the overall number of active nodes to the given number. Only
     * count the HNP's node if it was included in the allocation
     */
    if (prte_hnp_is_allocated) {
        num_nodes = 1;
    } else {
        num_nodes = 0;
    }
    while (NULL != (item = pmix_list_remove_first(&nodes))) {
        /* if a max size was given and we are there, then exit the loop */
        if (0 < prte_max_vm_size && num_nodes == prte_max_vm_size) {
            /* maintain accounting */
            PMIX_RELEASE(item);
            break;
        }
        node = (prte_node_t *) item;
        /* if this node is already in the map, skip it */
        if (NULL != node->daemon) {
            num_nodes++;
            /* maintain accounting */
            PMIX_RELEASE(item);
            continue;
        }
        /* add the node to the map - we retained it
         * when adding it to the list, so we don't need
         * to retain it again
         */
        pmix_pointer_array_add(map->nodes, (void *) node);
        ++(map->num_nodes);
        num_nodes++;
        /* create a new daemon object for this node */
        proc = PMIX_NEW(prte_proc_t);
        if (NULL == proc) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        PMIX_LOAD_NSPACE(proc->name.nspace, PRTE_PROC_MY_NAME->nspace);
        if (PMIX_RANK_VALID - 1 <= daemons->num_procs) {
            /* no more daemons available */
            pmix_show_help("help-prte-rmaps-base.txt", "out-of-vpids", true);
            PMIX_RELEASE(proc);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        proc->name.rank = daemons->num_procs; /* take the next available vpid */
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm add new daemon %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
        /* add the daemon to the daemon job object */
        if (0
            > (rc = pmix_pointer_array_set_item(daemons->procs, proc->name.rank, (void *) proc))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        ++daemons->num_procs;
        PMIX_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm assigning new daemon %s to node %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             node->name));
        /* point the node to the daemon */
        node->daemon = proc;
        PMIX_RETAIN(proc); /* maintain accounting */
        /* point the proc to the node and maintain accounting */
        proc->node = node;
        PMIX_RETAIN(node);
        if (prte_plm_globals.daemon_nodes_assigned_at_launch) {
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
        } else {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
        }
        /* track number of daemons to be launched */
        ++map->num_new_daemons;
        /* and their starting vpid */
        if (PMIX_RANK_INVALID == map->daemon_vpid_start) {
            map->daemon_vpid_start = proc->name.rank;
        }
        /* loop across all app procs on this node and update their parent */
        for (i = 0; i < node->procs->size; i++) {
            if (NULL != (pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, i))) {
                pptr->parent = proc->name.rank;
            }
        }
    }

    if (prte_process_info.num_daemons != daemons->num_procs) {
        /* more daemons are being launched - update the routing tree to
         * ensure that the HNP knows how to route messages via
         * the daemon routing tree - this needs to be done
         * here to avoid potential race conditions where the HNP
         * hasn't unpacked its launch message prior to being
         * asked to communicate.
         */
        prte_process_info.num_daemons = daemons->num_procs;

        /* ensure all routing plans are up-to-date - we need this
         * so we know how to tree-spawn and/or xcast info */
        prte_rml_compute_routing_tree();
    }

    /* mark that the daemon job changed */
    PRTE_FLAG_SET(daemons, PRTE_JOB_FLAG_UPDATED);

    /* if new daemons are being launched, mark that this job
     * caused it to happen */
    if (0 < map->num_new_daemons) {
        rc = prte_set_attribute(&jdata->attributes, PRTE_JOB_LAUNCHED_DAEMONS, true,
                                NULL, PMIX_BOOL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* The launch fence only operates when the DVM is permitted to grow/shrink.
     * Outside elastic mode the DVM is fixed-size, so no campaign is recorded
     * and the fence is never raised — leaving the normal launch path, and the
     * normal daemon-failure handling, completely unchanged. */
    if (prte_elastic_mode && 0 < map->num_new_daemons) {
        prte_grow_campaign_t *gcamp;
        pmix_rank_t gr;
        int gk;

        /* Record this launch campaign so the launch fence can be resolved on
         * a per-daemon basis: each new daemon either reports home (success)
         * or its launch fails (comm-failure / failed-to-start), and only
         * those specific ranks affect this campaign.  This avoids an
         * unrelated daemon loss consuming the fence, and lets concurrent
         * campaigns be tracked independently.  The new daemons were assigned
         * consecutive vpids starting at map->daemon_vpid_start (see the
         * daemon-creation loop above). */
        gcamp = PMIX_NEW(prte_grow_campaign_t);
        gcamp->ntargets = map->num_new_daemons;
        gcamp->targets = (pmix_rank_t *) malloc(gcamp->ntargets * sizeof(pmix_rank_t));
        for (gk = 0, gr = map->daemon_vpid_start; gk < gcamp->ntargets; gk++, gr++) {
            gcamp->targets[gk] = gr;
        }
        /* Record the requester for the spec's phase-two completion event.  The
         * RAS reservation machinery sets each reserved node's ->session
         * backpointer (add_nodes_to_session), and that session carries the
         * requestor and the allocation ids; take them from the first new
         * daemon's node.  The initial DVM bring-up and a scheduler-driven push
         * have no such requestor (the default session, or an invalid requestor
         * rank), so have_requester stays false and grow_drain() emits no event
         * for them. */
        {
            prte_proc_t *dproc = (prte_proc_t *)
                pmix_pointer_array_get_item(daemons->procs, map->daemon_vpid_start);
            prte_session_t *sess =
                (NULL != dproc && NULL != dproc->node) ? dproc->node->session : NULL;
            if (NULL != sess && PMIX_RANK_INVALID != sess->requestor.rank) {
                PMIX_XFER_PROCID(&gcamp->requester, &sess->requestor);
                gcamp->alloc_id = (NULL != sess->alloc_refid) ? strdup(sess->alloc_refid) : NULL;
                gcamp->req_id = (NULL != sess->user_refid) ? strdup(sess->user_refid) : NULL;
                gcamp->have_requester = true;
            }
        }
        pmix_list_append(&prte_grow_campaigns, &gcamp->super);
        prte_dvm_launch_fence += map->num_new_daemons;
    }

    return PRTE_SUCCESS;
}

void prte_plm_base_dvm_mod_notify(const pmix_proc_t *requester,
                                  const char *alloc_id,
                                  const char *req_id,
                                  bool success,
                                  pmix_status_t cause)
{
#if PRTE_HAVE_DVM_MOD_EVENTS
    pmix_status_t code = success ? PMIX_DVM_IS_READY : PMIX_ERR_DVM_MOD;
    pmix_info_t *rinfo;
    pmix_data_array_t parray;
    pmix_proc_t ptarg;
    size_t nrinfo, idx;
    pmix_status_t rc;

    /* Assemble a directed (custom-range) notification to the requester only,
     * mirroring the PMIX_ALLOC_TIMEOUT_WARNING delivery: custom range plus the
     * allocation id, the requester's own request id when one was supplied, and
     * — on failure — the underlying cause so the requester can distinguish
     * what went wrong rather than only that something did. */
    nrinfo = 2;
    if (NULL != req_id) {
        nrinfo++;
    }
    if (!success) {
        nrinfo++;
    }
    PMIX_INFO_CREATE(rinfo, nrinfo);
    idx = 0;
    PMIX_LOAD_PROCID(&ptarg, requester->nspace, requester->rank);
    parray.type = PMIX_PROC;
    parray.size = 1;
    parray.array = &ptarg;
    /* PMIX_INFO_LOAD deep-copies the data, so the stack copies are fine */
    PMIX_INFO_LOAD(&rinfo[idx++], PMIX_EVENT_CUSTOM_RANGE, &parray, PMIX_DATA_ARRAY);
    PMIX_INFO_LOAD(&rinfo[idx++], PMIX_ALLOC_ID, (void *) alloc_id, PMIX_STRING);
    if (NULL != req_id) {
        PMIX_INFO_LOAD(&rinfo[idx++], PMIX_ALLOC_REQ_ID, (void *) req_id, PMIX_STRING);
    }
    if (!success) {
        /* carry the underlying failure status; PMIX_JOB_TERM_STATUS is the
         * standard pmix_status_t-typed info key (reconcile the carrier with
         * PMIx PR openpmix#3917 should it define a dedicated DVM-mod cause
         * key) */
        PMIX_INFO_LOAD(&rinfo[idx++], PMIX_JOB_TERM_STATUS, &cause, PMIX_STATUS);
    }
    rc = PMIx_Notify_event(code, PRTE_PROC_MY_NAME, PMIX_RANGE_CUSTOM,
                           rinfo, nrinfo, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_INFO_FREE(rinfo, nrinfo);
#else
    /* The installed PMIx defines neither DVM modification event code, so the
     * completion notification is compiled out per the spec's backward-
     * compatibility clause.  The DVM still grows/shrinks; only the event is
     * omitted. */
    PRTE_HIDE_UNUSED_PARAMS(requester, alloc_id, req_id, success, cause);
#endif
}

void prte_plm_base_fence_release(void)
{
    int hi;
    prte_job_t *held;
    prte_shrink_campaign_t *scamp, *snext;
    prte_grow_campaign_t *gcamp, *gnext;

    /* SUCCESS release — reached only when the global fence has dropped to
     * zero, i.e. every grow and shrink campaign has completed successfully.
     * Both classes of held job are admitted.  The only path that *fails* a
     * held job is prte_plm_base_abort_premap_held() on a grow failure. */
    for (hi = 0; hi < prte_held_jobs->size; hi++) {
        held = (prte_job_t *) pmix_pointer_array_get_item(prte_held_jobs, hi);
        if (NULL == held) {
            continue;
        }
        pmix_pointer_array_set_item(prte_held_jobs, hi, NULL);
        PRTE_ACTIVATE_JOB_STATE(held, PRTE_JOB_STATE_VM_READY);
        PMIX_RELEASE(held);
    }

    for (hi = 0; hi < prte_prelaunch_held_jobs->size; hi++) {
        held = (prte_job_t *) pmix_pointer_array_get_item(prte_prelaunch_held_jobs, hi);
        if (NULL == held) {
            continue;
        }
        pmix_pointer_array_set_item(prte_prelaunch_held_jobs, hi, NULL);
        if (prte_plm_base_job_needs_remap(held)) {
            prte_plm_base_reset_proc_map(held);
            PRTE_ACTIVATE_JOB_STATE(held, PRTE_JOB_STATE_MAP);
        } else {
            PRTE_ACTIVATE_JOB_STATE(held, PRTE_JOB_STATE_LAUNCH_APPS);
        }
        PMIX_RELEASE(held);
    }

    /* Campaigns are removed individually as they drain, so both lists should
     * be empty here.  Sweep each defensively anyway — and sweep both kinds,
     * not just shrink — so a future change that can leave a residual campaign
     * behind cannot wedge the fence. */
    PMIX_LIST_FOREACH_SAFE(scamp, snext, &prte_shrink_campaigns, prte_shrink_campaign_t) {
        pmix_list_remove_item(&prte_shrink_campaigns, &scamp->super);
        PMIX_RELEASE(scamp);
    }
    PMIX_LIST_FOREACH_SAFE(gcamp, gnext, &prte_grow_campaigns, prte_grow_campaign_t) {
        pmix_list_remove_item(&prte_grow_campaigns, &gcamp->super);
        PMIX_RELEASE(gcamp);
    }
}

void prte_plm_base_abort_premap_held(void)
{
    int hi;
    prte_job_t *held;

    /* GROW-FAILURE abort — fail every job parked at the VM_READY -> MAP
     * boundary, immediately and independent of the fence value, so a grow
     * failure aborts its pre-map waiters even while a concurrent shrink keeps
     * the fence nonzero.  The pre-launch held jobs are deliberately left
     * untouched: they wait only on a shrink, never on the grow (spec
     * conformance #4). */
    for (hi = 0; hi < prte_held_jobs->size; hi++) {
        held = (prte_job_t *) pmix_pointer_array_get_item(prte_held_jobs, hi);
        if (NULL == held) {
            continue;
        }
        pmix_pointer_array_set_item(prte_held_jobs, hi, NULL);
        PRTE_ACTIVATE_JOB_STATE(held, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(held);
    }
}

void prte_plm_base_grow_drain(bool success)
{
    prte_grow_campaign_t *camp;

    /* Resolve all in-progress grow campaigns at once and drop their entire
     * contribution from the launch fence.  This is called from exactly the
     * two safe points: from vm_ready on the all-success path (after the
     * WIREUP xcast, so held jobs are only admitted once the new daemons are
     * wired up), and from the failure path when one of the launched daemons
     * dies.  Each drained campaign emits its phase-two completion event to its
     * requester — PMIX_DVM_IS_READY on success, PMIX_ERR_DVM_MOD on failure. */
    while (NULL != (camp = (prte_grow_campaign_t *)
                               pmix_list_remove_first(&prte_grow_campaigns))) {
        prte_dvm_launch_fence -= camp->ntargets;
        if (camp->have_requester) {
            /* the specific daemon-failure status is not threaded down to this
             * layer yet, so report a generic cause on failure (reconcile by
             * passing the dying daemon's status as a follow-up) */
            prte_plm_base_dvm_mod_notify(&camp->requester, camp->alloc_id,
                                         camp->req_id, success,
                                         success ? PMIX_SUCCESS : PMIX_ERROR);
        }
        PMIX_RELEASE(camp);
    }
    if (success) {
        /* admit the held jobs only once the *global* fence is clear — a
         * concurrent shrink may still hold it nonzero */
        if (0 == prte_dvm_launch_fence) {
            prte_plm_base_fence_release();
        }
    } else {
        /* a grow failure fails the whole pre-map held-job set immediately,
         * regardless of the fence value, so a concurrent shrink cannot later
         * admit a job whose grow dependency has failed; the shrink-only
         * pre-launch held jobs are left parked */
        prte_plm_base_abort_premap_held();
    }
}

/* Return a node that has been torn out of the DVM (by a completed shrink or a
 * rolled-back grow) to a pristine, never-launched state so a later grow can
 * relaunch a daemon on it.  Clearing the daemon backpointer alone is not enough:
 * the launch machinery keys off per-node flags and the persistent daemon-job
 * map, and stale values there make a re-grow silently misbehave.  Specifically,
 * PRTE_NODE_FLAG_DAEMON_LAUNCHED left set makes every plm launcher skip the node
 * ("daemon already exists"), so the new prted is never spawned; and leaving the
 * node in the daemon map lets setup_vm add it a second time, duplicating it in
 * the VM.  Reset both here. */
void prte_plm_base_reset_dvm_node(prte_node_t *node)
{
    prte_job_t *daemons;
    prte_node_t *nptr;
    int i;

    if (NULL == node) {
        return;
    }

    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_LOC_VERIFIED);

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons || NULL == daemons->map) {
        return;
    }
    for (i = 0; i < daemons->map->nodes->size; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(daemons->map->nodes, i);
        if (nptr != node) {
            continue;
        }
        pmix_pointer_array_set_item(daemons->map->nodes, i, NULL);
        if (0 < daemons->map->num_nodes) {
            --daemons->map->num_nodes;
        }
        PMIX_RELEASE(node); /* the map held a retain */
        break;
    }
}

/* Roll a failed grow campaign back out of the DVM so a failed grow leaves the
 * DVM at its exact pre-grow membership rather than half-extended (spec
 * conformance #5).  `trigger` is the rank whose death triggered the failure;
 * the errmgr has already marked it not-alive and decremented num_daemons, and
 * its routing is repaired here.  Every *other* target is handled by whether a
 * daemon actually came up:
 *
 *   - a target that started (PRTE_PROC_FLAG_ALIVE) is terminated using the same
 *     PRTE_DAEMON_SHRINK_CMD machinery the DVM shrink path uses; its departure
 *     is then reconciled on the normal daemon-loss path as it exits;
 *   - a target that never started has no daemon to signal, so its launch-time
 *     daemon-count bump is reverted here (no comm-failure event will arrive for
 *     it).
 *
 * In all cases the node is reset out of the DVM's usable set (see
 * prte_plm_base_reset_dvm_node).  The new nodes carry no application procs — the
 * jobs that would have used them were held at the fence, never launched.  The
 * teardown is strictly campaign-scoped: it touches only the ranks in this
 * campaign's ``targets`` array. */
static void grow_rollback(prte_grow_campaign_t *camp, pmix_rank_t trigger)
{
    prte_job_t *daemons;
    prte_proc_t *dproc;
    prte_node_t *node;
    pmix_rank_t *kill;
    int32_t nkill = 0;
    int t;

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        return;
    }
    kill = (pmix_rank_t *) malloc(camp->ntargets * sizeof(pmix_rank_t));
    if (NULL == kill) {
        return;
    }

    /* repair routing for the daemon whose loss triggered the failure — the
     * errmgr's own route_lost call is skipped because grow_target_failed()
     * reports the death as handled (so the errmgr does not abort the DVM) */
    prte_rml_route_lost(trigger);

    for (t = 0; t < camp->ntargets; t++) {
        pmix_rank_t r = camp->targets[t];
        if (PMIX_RANK_INVALID == r) {
            continue;
        }
        dproc = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, r);
        if (NULL == dproc) {
            continue;
        }
        node = dproc->node;
        if (r != trigger) {
            if (PRTE_FLAG_TEST(dproc, PRTE_PROC_FLAG_ALIVE)) {
                /* a started daemon — terminate it via the shrink command */
                kill[nkill++] = r;
            } else {
                /* never started: no comm-failure event will arrive, so revert
                 * its launch-time daemon-count bump here */
                if (0 < prte_process_info.num_daemons) {
                    --prte_process_info.num_daemons;
                }
            }
        }
        /* detach the node from the DVM's usable set */
        if (NULL != node) {
            if (NULL != node->session && node->session != prte_default_session) {
                node->session = NULL;
            }
            if (node->daemon == dproc) {
                node->daemon = NULL;
                PMIX_RELEASE(dproc);
            }
            /* return the node to a pristine, never-launched state so a later
             * grow can relaunch a daemon on it (clears the launch flags and
             * drops it from the daemon-job map) */
            prte_plm_base_reset_dvm_node(node);
        }
    }

    if (0 < nkill) {
        pmix_data_buffer_t msg;
        prte_daemon_cmd_flag_t cmd = PRTE_DAEMON_SHRINK_CMD;
        pmix_status_t rc;

        PMIX_DATA_BUFFER_CONSTRUCT(&msg);
        rc = PMIx_Data_pack(NULL, &msg, &cmd, 1, PMIX_UINT8);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_pack(NULL, &msg, &nkill, 1, PMIX_INT32);
        }
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_pack(NULL, &msg, kill, nkill, PMIX_PROC_RANK);
        }
        if (PMIX_SUCCESS == rc) {
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
                PRTE_ERROR_LOG(rc);
            }
        } else {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
    }
    free(kill);
}

bool prte_plm_base_grow_target_failed(pmix_rank_t rank)
{
    prte_grow_campaign_t *camp;
    int t;

    /* Outside elastic mode there are no grow campaigns and the daemon loss is
     * the errmgr's to handle exactly as it always has — never report it as
     * handled here, so the normal DVM-failure path is preserved. */
    if (!prte_elastic_mode) {
        return false;
    }

    /* A daemon has died.  Only act if it was actually the target of an
     * in-progress grow campaign — an unrelated daemon loss must not consume
     * the launch fence (and must be left to the errmgr's normal handling).
     * If it was a grow target, that campaign is compromised: roll it back out
     * of the DVM, drop its fence contribution, notify its requester of the
     * failure, and abort the pre-map held jobs.  The teardown is scoped to the
     * matched campaign, so any concurrent grow keeps its own daemons and
     * completes normally (spec conformance #5). */
    PMIX_LIST_FOREACH(camp, &prte_grow_campaigns, prte_grow_campaign_t) {
        for (t = 0; t < camp->ntargets; t++) {
            if (camp->targets[t] != rank) {
                continue;
            }
            pmix_list_remove_item(&prte_grow_campaigns, &camp->super);
            prte_dvm_launch_fence -= camp->ntargets;
            grow_rollback(camp, rank);
            if (camp->have_requester) {
                /* the specific daemon-failure status is not threaded down to
                 * this layer yet, so report a generic cause */
                prte_plm_base_dvm_mod_notify(&camp->requester, camp->alloc_id,
                                             camp->req_id, false, PMIX_ERROR);
            }
            PMIX_RELEASE(camp);
            /* any grow failure fails the whole pre-map held-job set */
            prte_plm_base_abort_premap_held();
            return true;
        }
    }
    return false;
}

bool prte_plm_base_job_needs_remap(prte_job_t *jdata)
{
    prte_shrink_campaign_t *camp;
    prte_proc_t *proc;
    int p, t;

    PMIX_LIST_FOREACH(camp, &prte_shrink_campaigns, prte_shrink_campaign_t) {
        for (p = 0; p < jdata->procs->size; p++) {
            proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, p);
            if (NULL == proc || NULL == proc->node || NULL == proc->node->daemon) continue;
            for (t = 0; t < camp->ntargets; t++) {
                if (camp->targets[t] == proc->node->daemon->name.rank) {
                    return true;
                }
            }
        }
    }
    return false;
}

void prte_plm_base_reset_proc_map(prte_job_t *jdata)
{
    int p, np;
    prte_proc_t *proc;
    prte_node_t *node;
    prte_app_context_t *app;

    for (p = 0; p < jdata->procs->size; p++) {
        proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, p);
        if (NULL == proc) continue;
        node = proc->node;
        if (NULL != node) {
            for (np = 0; np < node->procs->size; np++) {
                if (pmix_pointer_array_get_item(node->procs, np) == proc) {
                    pmix_pointer_array_set_item(node->procs, np, NULL);
                    node->num_procs--;
                    app = (prte_app_context_t *)
                        pmix_pointer_array_get_item(jdata->apps, proc->app_idx);
                    if (NULL == app || !PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                        node->slots_inuse--;
                    }
                    break;
                }
            }
        }
        pmix_pointer_array_set_item(jdata->procs, p, NULL);
        PMIX_RELEASE(proc);
    }
    jdata->num_procs = 0;
    jdata->num_launched = 0;
}
