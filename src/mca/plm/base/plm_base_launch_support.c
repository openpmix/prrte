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
 * Copyright (c) 2007-2017 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */
#include <ctype.h>

#include "src/include/hash_string.h"
#include "src/util/argv.h"
#include "src/util/prrte_environ.h"
#include "src/util/printf.h"
#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/compress/compress.h"

#include "src/util/dash_host/dash_host.h"
#include "src/util/nidmap.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/rtc/rtc.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_quit.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"
#include "src/util/hostfile/hostfile.h"
#include "src/mca/odls/odls_types.h"

#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/base/base.h"

void prrte_plm_base_set_slots(prrte_node_t *node)
{
    if (0 == strncmp(prrte_set_slots, "cores", strlen(prrte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_CORE, 0,
                                                             PRRTE_HWLOC_LOGICAL);
        }
    } else if (0 == strncmp(prrte_set_slots, "sockets", strlen(prrte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            if (0 == (node->slots = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                       HWLOC_OBJ_SOCKET, 0,
                                                                       PRRTE_HWLOC_LOGICAL))) {
                /* some systems don't report sockets - in this case,
                 * use numanodes */
                node->slots = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                 HWLOC_OBJ_NODE, 0,
                                                                 PRRTE_HWLOC_LOGICAL);
            }
        }
    } else if (0 == strncmp(prrte_set_slots, "numas", strlen(prrte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_NODE, 0,
                                                             PRRTE_HWLOC_LOGICAL);
        }
    } else if (0 == strncmp(prrte_set_slots, "hwthreads", strlen(prrte_set_slots))) {
        if (NULL != node->topology && NULL != node->topology->topo) {
            node->slots = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                             HWLOC_OBJ_PU, 0,
                                                             PRRTE_HWLOC_LOGICAL);
        }
    } else {
        /* must be a number */
        node->slots = strtol(prrte_set_slots, NULL, 10);
    }
    /* mark the node as having its slots "given" */
    PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
}

void prrte_plm_base_daemons_reported(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_topology_t *t;
    prrte_node_t *node;
    int i;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* if we are not launching, then we just assume that all
     * daemons share our topology */
    if (prrte_do_not_launch) {
        node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
        t = node->topology;
        for (i=1; i < prrte_node_pool->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t;
            }
        }
    }

    /* if this is an unmanaged allocation, then set the default
     * slots on each node as directed or using default
     */
    if (!prrte_managed_allocation) {
        if (NULL != prrte_set_slots &&
            0 != strncmp(prrte_set_slots, "none", strlen(prrte_set_slots))) {
            caddy->jdata->total_slots_alloc = 0;
            for (i=0; i < prrte_node_pool->size; i++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                    continue;
                }
                if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
                    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                         "%s plm:base:setting slots for node %s by %s",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node->name, prrte_set_slots));
                    prrte_plm_base_set_slots(node);
                }
                caddy->jdata->total_slots_alloc += node->slots;
            }
        }
    }

    if (prrte_display_allocation) {
        prrte_ras_base_display_alloc();
    }
    /* ensure we update the routing plan */
    prrte_routed.update_routing_plan();

    /* progress the job */
    caddy->jdata->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
    PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_VM_READY);

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_allocation_complete(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* if we don't want to launch, then we at least want
     * to map so we can see where the procs would have
     * gone - so skip to the mapping state */
    if (prrte_do_not_launch) {
        caddy->jdata->state = PRRTE_JOB_STATE_ALLOCATION_COMPLETE;
        PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_MAP);
    } else {
        /* move the state machine along */
        caddy->jdata->state = PRRTE_JOB_STATE_ALLOCATION_COMPLETE;
        PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_LAUNCH_DAEMONS);
    }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_daemons_launched(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* do NOT increment the state - we wait for the
     * daemons to report that they have actually
     * started before moving to the right state
     */
    /* cleanup */
    PRRTE_RELEASE(caddy);
}

static void files_ready(int status, void *cbdata)
{
    prrte_job_t *jdata = (prrte_job_t*)cbdata;

    if (PRRTE_SUCCESS != status) {
        PRRTE_FORCED_TERMINATE(status);
    } else {
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP);
    }
}

void prrte_plm_base_vm_ready(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* progress the job */
    caddy->jdata->state = PRRTE_JOB_STATE_VM_READY;

    /* position any required files */
    if (PRRTE_SUCCESS != prrte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_mapping_complete(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* move the state machine along */
    caddy->jdata->state = PRRTE_JOB_STATE_MAP_COMPLETE;
    PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_SYSTEM_PREP);

    /* cleanup */
    PRRTE_RELEASE(caddy);
}


void prrte_plm_base_setup_job(int fd, short args, void *cbdata)
{
    int rc;
    int i;
    prrte_app_context_t *app;
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:setup_job",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    if (PRRTE_JOB_STATE_INIT != caddy->job_state) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* start by getting a jobid */
    if (PRRTE_JOBID_INVALID == caddy->jdata->jobid) {
        if (PRRTE_SUCCESS != (rc = prrte_plm_base_create_jobid(caddy->jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }

        /* store it on the global job data pool - this is the key
         * step required before we launch the daemons. It allows
         * the prrte_rmaps_base_setup_virtual_machine routine to
         * search all apps for any hosts to be used by the vm
         */
        prrte_hash_table_set_value_uint32(prrte_job_data, caddy->jdata->jobid, caddy->jdata);
    }

    /* if job recovery is not enabled, set it to default */
    if (!PRRTE_FLAG_TEST(caddy->jdata, PRRTE_JOB_FLAG_RECOVERABLE) &&
        prrte_enable_recovery) {
        PRRTE_FLAG_SET(caddy->jdata, PRRTE_JOB_FLAG_RECOVERABLE);
    }

    /* if app recovery is not defined, set apps to defaults */
    for (i=0; i < caddy->jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(caddy->jdata->apps, i))) {
            continue;
        }
        if (!prrte_get_attribute(&app->attributes, PRRTE_APP_RECOV_DEF, NULL, PRRTE_BOOL)) {
            prrte_set_attribute(&app->attributes, PRRTE_APP_MAX_RESTARTS, PRRTE_ATTR_LOCAL, &prrte_max_restarts, PRRTE_INT32);
        }
    }

    /* set the job state to the next position */
    PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_INIT_COMPLETE);

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_setup_job_complete(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* nothing to do here but move along */
    PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_ALLOCATE);
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_complete_setup(int fd, short args, void *cbdata)
{
    prrte_job_t *jdata, *jdatorted;
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_node_t *node;
    uint32_t h;
    prrte_vpid_t *vptr;
    int i, rc;
    char *serial_number;
    prrte_process_name_t requestor, *rptr;

    PRRTE_ACQUIRE_OBJECT(caddy);

    prrte_output_verbose(5, prrte_plm_base_framework.framework_output,
                        "%s complete_setup on job %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_JOBID_PRINT(caddy->jdata->jobid));

    /* bozo check */
    if (PRRTE_JOB_STATE_SYSTEM_PREP != caddy->job_state) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* get the orted job data object */
    if (NULL == (jdatorted = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* convenience */
    jdata = caddy->jdata;

    /* If this job is being started by me, then there is nothing
     * further we need to do as any user directives (e.g., to tie
     * off IO to /dev/null) will have been included in the launch
     * message and the IOF knows how to handle any default situation.
     * However, if this is a proxy spawn request, then the spawner
     * might be a tool that wants IO forwarded to it. If that's the
     * situation, then the job object will contain an attribute
     * indicating that request */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FWDIO_TO_TOOL, NULL, PRRTE_BOOL)) {
        /* send a message to our IOF containing the requested pull */
        rptr = &requestor;
        if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_LAUNCH_PROXY, (void**)&rptr, PRRTE_NAME)) {
            PRRTE_IOF_PROXY_PULL(jdata, rptr);
        } else {
            PRRTE_IOF_PROXY_PULL(jdata, &jdata->originator);
        }
        /* the tool will PUSH its stdin, so nothing we need to do here
         * about stdin */
    }

    /* if coprocessors were detected, now is the time to
     * identify who is attached to what host - this info
     * will be shipped to the daemons in the nidmap. Someday,
     * there may be a direct way for daemons on coprocessors
     * to detect their hosts - but not today.
     */
    if (prrte_coprocessors_detected) {
        /* cycle thru the nodes looking for coprocessors */
        for (i=0; i < prrte_node_pool->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            /* if we don't have a serial number, then we are not a coprocessor */
            serial_number = NULL;
            if (!prrte_get_attribute(&node->attributes, PRRTE_NODE_SERIAL_NUMBER, (void**)&serial_number, PRRTE_STRING)) {
                continue;
            }
            if (NULL != serial_number) {
                /* if we have a serial number, then we are a coprocessor - so
                 * compute our hash and lookup our hostid
                 */
                PRRTE_HASH_STR(serial_number, h);
                free(serial_number);
                if (PRRTE_SUCCESS != (rc = prrte_hash_table_get_value_uint32(prrte_coprocessors, h,
                                                                           (void**)&vptr))) {
                    PRRTE_ERROR_LOG(rc);
                    break;
                }
                prrte_set_attribute(&node->attributes, PRRTE_NODE_HOSTID, PRRTE_ATTR_LOCAL, vptr, PRRTE_VPID);
            }
        }
    }
    /* done with the coprocessor mapping at this time */
    if (NULL != prrte_coprocessors) {
        PRRTE_RELEASE(prrte_coprocessors);
    }

    /* set the job state to the next position */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_LAUNCH_APPS);

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

/* catch timeout to allow cmds to progress */
static void timer_cb(int fd, short event, void *cbdata)
{
    prrte_job_t *jdata = (prrte_job_t*)cbdata;
    prrte_timer_t *timer=NULL;

    PRRTE_ACQUIRE_OBJECT(jdata);

    /* declare launch failed */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_FAILED_TO_START);

    /* free event */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT, (void**)&timer, PRRTE_PTR)) {
        /* timer is an prrte_timer_t object */
        PRRTE_RELEASE(timer);
        prrte_remove_attribute(&jdata->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT);
    }
}

void prrte_plm_base_launch_apps(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_daemon_cmd_flag_t command;
    int rc;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    if (PRRTE_JOB_STATE_LAUNCH_APPS != caddy->job_state) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:launch_apps for job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* pack the appropriate add_local_procs command */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FIXED_DVM, NULL, PRRTE_BOOL)) {
        command = PRRTE_DAEMON_DVM_ADD_PROCS;
    } else {
        command = PRRTE_DAEMON_ADD_LOCAL_PROCS;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&jdata->launch_msg, &command, 1, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* get the local launcher's required data */
    if (PRRTE_SUCCESS != (rc = prrte_odls.get_add_procs_data(&jdata->launch_msg, jdata->jobid))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }

    PRRTE_RELEASE(caddy);
    return;
}

void prrte_plm_base_send_launch_msg(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_timer_t *timer;
    prrte_grpcomm_signature_t *sig;
    prrte_job_t *jdata;
    int rc;

    /* convenience */
    jdata = caddy->jdata;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:send launch msg for job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* if we don't want to launch the apps, now is the time to leave */
    if (prrte_do_not_launch) {
        bool compressed;
        uint8_t *cmpdata;
        size_t cmplen;
        /* report the size of the launch message */
        compressed = prrte_compress.compress_block((uint8_t*)jdata->launch_msg.base_ptr,
                                                  jdata->launch_msg.bytes_used,
                                                  &cmpdata, &cmplen);
        if (compressed) {
            prrte_output(0, "LAUNCH MSG RAW SIZE: %d COMPRESSED SIZE: %d",
                        (int)jdata->launch_msg.bytes_used, (int)cmplen);
            free(cmpdata);
        } else {
            prrte_output(0, "LAUNCH MSG RAW SIZE: %d", (int)jdata->launch_msg.bytes_used);
        }
        prrte_never_launched = true;
        PRRTE_FORCED_TERMINATE(0);
        PRRTE_RELEASE(caddy);
        return;
    }

    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    sig->sz = 1;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, &jdata->launch_msg))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(sig);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    PRRTE_DESTRUCT(&jdata->launch_msg);
    PRRTE_CONSTRUCT(&jdata->launch_msg, prrte_buffer_t);
    /* maintain accounting */
    PRRTE_RELEASE(sig);

    /* track that we automatically are considered to have reported - used
     * only to report launch progress
     */
    caddy->jdata->num_daemons_reported++;

    /* if requested, setup a timer - if we don't launch within the
     * defined time, then we know things have failed
     */
    if (0 < prrte_startup_timeout) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:launch defining timeout for job %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_JOBID_PRINT(jdata->jobid)));
        timer = PRRTE_NEW(prrte_timer_t);
        timer->payload = jdata;
        prrte_event_evtimer_set(prrte_event_base,
                               timer->ev, timer_cb, jdata);
        prrte_event_set_priority(timer->ev, PRRTE_ERROR_PRI);
        timer->tv.tv_sec = prrte_startup_timeout;
        timer->tv.tv_usec = 0;
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT, PRRTE_ATTR_LOCAL, timer, PRRTE_PTR);
        PRRTE_POST_OBJECT(timer);
        prrte_event_evtimer_add(timer->ev, &timer->tv);
    }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

int prrte_plm_base_spawn_reponse(int32_t status, prrte_job_t *jdata)
{
    int ret;
    prrte_buffer_t *answer;
    int room, *rmptr;

    /* if the response has already been sent, don't do it again */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_SPAWN_NOTIFIED, NULL, PRRTE_BOOL)) {
        return PRRTE_SUCCESS;
    }

    /* prep the response to the spawn requestor */
    answer = PRRTE_NEW(prrte_buffer_t);
    /* pack the status */
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &status, 1, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(answer);
        return ret;
    }
    /* pack the jobid */
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &jdata->jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(answer);
        return ret;
    }
    /* pack the room number */
    rmptr = &room;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_ROOM_NUM, (void**)&rmptr, PRRTE_INT)) {
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &room, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(answer);
            return ret;
        }
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:launch sending dyn release of job %s to %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid),
                         PRRTE_NAME_PRINT(&jdata->originator)));
    if (0 > (ret = prrte_rml.send_buffer_nb(&jdata->originator, answer,
                                           PRRTE_RML_TAG_LAUNCH_RESP,
                                           prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(answer);
        return ret;
    }

    /* mark that we sent it */
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_SPAWN_NOTIFIED, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);
    return PRRTE_SUCCESS;
}

void prrte_plm_base_post_launch(int fd, short args, void *cbdata)
{
    int32_t rc;
    prrte_job_t *jdata;
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_timer_t *timer=NULL;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    /* if a timer was defined, cancel it */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT, (void**)&timer, PRRTE_PTR)) {
        prrte_event_evtimer_del(timer->ev);
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:launch deleting timeout for job %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_JOBID_PRINT(jdata->jobid)));
        PRRTE_RELEASE(timer);
        prrte_remove_attribute(&jdata->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT);
    }

    if (PRRTE_JOB_STATE_RUNNING != caddy->job_state) {
        /* error mgr handles this */
        PRRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    caddy->jdata->state = caddy->job_state;

    /* notify the spawn requestor */
    rc = prrte_plm_base_spawn_reponse(PRRTE_SUCCESS, jdata);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
    }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

void prrte_plm_base_registered(int fd, short args, void *cbdata)
{
    prrte_job_t *jdata;
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* convenience */
    jdata = caddy->jdata;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:launch %s registered",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    if (PRRTE_JOB_STATE_REGISTERED != caddy->job_state) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:launch job %s not registered - state %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_JOBID_PRINT(jdata->jobid),
                             prrte_job_state_to_str(caddy->job_state)));
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* update job state */
    jdata->state = caddy->job_state;

    PRRTE_RELEASE(caddy);
}

/* daemons callback when they start - need to listen for them */
static bool prted_failed_launch;
static prrte_job_t *jdatorted=NULL;

/* callback for topology reports */
void prrte_plm_base_daemon_topology(int status, prrte_process_name_t* sender,
                                   prrte_buffer_t *buffer,
                                   prrte_rml_tag_t tag, void *cbdata)
{
    hwloc_topology_t topo;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *sum;
    int rc, idx;
    char *sig, *coprocessors, **sns;
    prrte_proc_t *daemon=NULL;
    prrte_topology_t *t, *t2;
    int i;
    uint32_t h;
    prrte_job_t *jdata;
    uint8_t flag;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;
    prrte_buffer_t datbuf, *data;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:daemon_topology recvd for daemon %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(sender)));

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    }
    if (NULL == (daemon = (prrte_proc_t*)prrte_pointer_array_get_item(jdatorted->procs, sender->vpid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    PRRTE_CONSTRUCT(&datbuf, prrte_buffer_t);
    /* unpack the flag to see if this payload is compressed */
    idx=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &flag, &idx, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    if (flag) {
        /* unpack the data size */
        idx=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &inlen, &idx, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* unpack the unpacked data size */
        idx=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &cmplen, &idx, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* allocate the space */
        packed_data = (uint8_t*)malloc(inlen);
        /* unpack the data blob */
        idx = inlen;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, packed_data, &idx, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* decompress the data */
        if (prrte_compress.decompress_block(&cmpdata, cmplen,
                                           packed_data, inlen)) {
            /* the data has been uncompressed */
            prrte_dss.load(&datbuf, cmpdata, cmplen);
            data = &datbuf;
        } else {
            data = buffer;
        }
        free(packed_data);
    } else {
        data = buffer;
    }

    /* unpack the topology signature for this node */
    idx=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(data, &sig, &idx, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    /* find it in the array */
    t = NULL;
    for (i=0; i < prrte_node_topologies->size; i++) {
        if (NULL == (t2 = (prrte_topology_t*)prrte_pointer_array_get_item(prrte_node_topologies, i))) {
            continue;
        }
        /* just check the signature */
        if (0 == strcmp(sig, t2->sig)) {
            t = t2;
            break;
        }
    }
    if (NULL == t) {
        /* should never happen */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        prted_failed_launch = true;
        goto CLEANUP;
    }

    /* unpack the topology */
    idx=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(data, &topo, &idx, PRRTE_HWLOC_TOPO))) {
        PRRTE_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    /* record the final topology */
    t->topo = topo;
    /* setup the summary data for this topology as we will need
     * it when we go to map/bind procs to it */
    root = hwloc_get_root_obj(topo);
    root->userdata = (void*)PRRTE_NEW(prrte_hwloc_topo_data_t);
    sum = (prrte_hwloc_topo_data_t*)root->userdata;
    #if HWLOC_API_VERSION < 0x20000
        sum->available = hwloc_bitmap_alloc();
        hwloc_bitmap_and(sum->available, root->online_cpuset, root->allowed_cpuset);
    #else
        sum->available = hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));
    #endif

    /* unpack any coprocessors */
    idx=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(data, &coprocessors, &idx, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    if (NULL != coprocessors) {
        /* init the hash table, if necessary */
        if (NULL == prrte_coprocessors) {
            prrte_coprocessors = PRRTE_NEW(prrte_hash_table_t);
            prrte_hash_table_init(prrte_coprocessors, prrte_process_info.num_procs);
        }
        /* separate the serial numbers of the coprocessors
         * on this host
         */
        sns = prrte_argv_split(coprocessors, ',');
        for (idx=0; NULL != sns[idx]; idx++) {
            /* compute the hash */
            PRRTE_HASH_STR(sns[idx], h);
            /* mark that this coprocessor is hosted by this node */
            prrte_hash_table_set_value_uint32(prrte_coprocessors, h, (void*)&daemon->name.vpid);
        }
        prrte_argv_free(sns);
        free(coprocessors);
        prrte_coprocessors_detected = true;
    }
    /* see if this daemon is on a coprocessor */
    idx=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(data, &coprocessors, &idx, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        prted_failed_launch = true;
        goto CLEANUP;
    }
    if (NULL != coprocessors) {
        if (prrte_get_attribute(&daemon->node->attributes, PRRTE_NODE_SERIAL_NUMBER, NULL, PRRTE_STRING)) {
            /* this is not allowed - a coprocessor cannot be host
             * to another coprocessor at this time
             */
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_SUPPORTED);
            prted_failed_launch = true;
            free(coprocessors);
            goto CLEANUP;
        }
        prrte_set_attribute(&daemon->node->attributes, PRRTE_NODE_SERIAL_NUMBER, PRRTE_ATTR_LOCAL, coprocessors, PRRTE_STRING);
        free(coprocessors);
        prrte_coprocessors_detected = true;
    }

  CLEANUP:
    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:orted:report_topo launch %s for daemon %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         prted_failed_launch ? "failed" : "completed",
                         PRRTE_NAME_PRINT(sender)));

    if (prted_failed_launch) {
        PRRTE_ACTIVATE_JOB_STATE(jdatorted, PRRTE_JOB_STATE_FAILED_TO_START);
        return;
    } else {
        jdatorted->num_reported++;
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch recvd %d of %d reported daemons",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             jdatorted->num_reported, jdatorted->num_procs));
        if (jdatorted->num_procs == jdatorted->num_reported) {
            bool dvm = true;
            uint32_t key;
            void *nptr;
            jdatorted->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
            /* activate the daemons_reported state for all jobs
             * whose daemons were launched
             */
            rc = prrte_hash_table_get_first_key_uint32(prrte_job_data, &key, (void **)&jdata, &nptr);
            while (PRRTE_SUCCESS == rc) {
                if (PRRTE_PROC_MY_NAME->jobid != jdata->jobid) {
                    dvm = false;
                    if (PRRTE_JOB_STATE_DAEMONS_LAUNCHED == jdata->state) {
                        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
                    }
                }
                rc = prrte_hash_table_get_next_key_uint32(prrte_job_data, &key, (void **)&jdata, nptr, &nptr);
            }
            if (dvm) {
                /* must be launching a DVM - activate the state */
                PRRTE_ACTIVATE_JOB_STATE(jdatorted, PRRTE_JOB_STATE_DAEMONS_REPORTED);
            }
        }
    }
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

void prrte_plm_base_daemon_callback(int status, prrte_process_name_t* sender,
                                   prrte_buffer_t *buffer,
                                   prrte_rml_tag_t tag, void *cbdata)
{
    char *ptr;
    int rc, idx;
    prrte_proc_t *daemon=NULL;
    prrte_job_t *jdata;
    prrte_process_name_t dname;
    prrte_buffer_t *relay;
    char *sig;
    prrte_topology_t *t;
    hwloc_topology_t topo;
    int i;
    bool found;
    prrte_daemon_cmd_flag_t cmd;
    char *myendian;
    pmix_proc_t pproc;
    char *alias, **atmp=NULL;
    uint8_t naliases, ni;

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    }

    /* get my endianness */
    t = (prrte_topology_t*)prrte_pointer_array_get_item(prrte_node_topologies, 0);
    if (NULL == t) {
        /* should never happen */
        myendian = "unknown";
    } else {
        myendian = strrchr(t->sig, ':');
        ++myendian;
    }

    /* multiple daemons could be in this buffer, so unpack until we exhaust the data */
    idx = 1;
    PRRTE_PMIX_CONVERT_NAME(&pproc, PRRTE_PROC_MY_NAME);
    while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(buffer, &dname, &idx, PRRTE_NAME))) {
        char *nodename = NULL;
        pmix_status_t ret;
        pmix_info_t *info;
        size_t n, ninfo;
        prrte_byte_object_t *bptr;
        pmix_data_buffer_t pbuf;

        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch from daemon %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&dname)));

        /* update state and record for this daemon contact info */
        if (NULL == (daemon = (prrte_proc_t*)prrte_pointer_array_get_item(jdatorted->procs, dname.vpid))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        daemon->state = PRRTE_PROC_STATE_RUNNING;
        /* record that this daemon is alive */
        PRRTE_FLAG_SET(daemon, PRRTE_PROC_FLAG_ALIVE);

        /* unpack the byte object containing the info array */
        idx = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &bptr, &idx, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        /* if nothing is present, then ignore it */
        if (0 == bptr->size) {
            free(bptr);
        } else {
            /* load the bytes into a PMIx data buffer for unpacking */
            PMIX_DATA_BUFFER_LOAD(&pbuf, bptr->bytes, bptr->size);
            bptr->bytes = NULL;
            free(bptr);
            /* setup the daemon name */
            pproc.rank = dname.vpid;
            /* unpack the number of info structs */
            idx = 1;
            ret = PMIx_Data_unpack(&pproc, &pbuf, &ninfo, &idx, PMIX_SIZE);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                rc = PRRTE_ERROR;
                prted_failed_launch = true;
                goto CLEANUP;
            }
            PMIX_INFO_CREATE(info, ninfo);
            idx = ninfo;
            ret = PMIx_Data_unpack(&pproc, &pbuf, info, &idx, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_INFO_FREE(info, ninfo);
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                rc = PRRTE_ERROR;
                prted_failed_launch = true;
                goto CLEANUP;
            }
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);

            for (n=0; n < ninfo; n++) {
                /* store this in a daemon wireup buffer for later distribution */
                if (PMIX_SUCCESS != (ret = PMIx_Store_internal(&pproc, info[n].key, &info[n].value))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    rc = PRRTE_ERROR;
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
            }
            PMIX_INFO_FREE(info, ninfo);
        }

        /* unpack the node name */
        idx = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &nodename, &idx, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        if (!prrte_have_fqdn_allocation) {
            /* remove any domain info */
            if (NULL != (ptr = strchr(nodename, '.'))) {
                *ptr = '\0';
                ptr = strdup(nodename);
                free(nodename);
                nodename = ptr;
            }
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch from daemon %s on node %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&daemon->name), nodename));

        /* mark the daemon as launched */
        PRRTE_FLAG_SET(daemon->node, PRRTE_NODE_FLAG_DAEMON_LAUNCHED);

        /* first, store the nodename itself as an alias. We do
         * this in case the nodename isn't the same as what we
         * were given by the allocation. For example, a hostfile
         * might contain an IP address instead of the value returned
         * by gethostname, yet the daemon will have returned the latter
         * and apps may refer to the host by that name
         */
        prrte_argv_append_nosize(&atmp, nodename);
        /* unpack and store the provided aliases */
        idx = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &naliases, &idx, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        for (ni=0; ni < naliases; ni++) {
            idx = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &alias, &idx, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            prrte_argv_append_nosize(&atmp, alias);
            free(alias);
        }
        if (0 < naliases) {
            alias = prrte_argv_join(atmp, ',');
            prrte_set_attribute(&daemon->node->attributes, PRRTE_NODE_ALIAS, PRRTE_ATTR_LOCAL, alias, PRRTE_STRING);
            free(alias);
        }
        prrte_argv_free(atmp);

        /* unpack the topology signature for that node */
        idx=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &sig, &idx, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            prted_failed_launch = true;
            goto CLEANUP;
        }
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s RECEIVED TOPOLOGY SIG %s FROM NODE %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), sig, nodename));

        /* rank=1 always sends its topology back */
        topo = NULL;
        if (1 == dname.vpid) {
            uint8_t flag;
            size_t inlen, cmplen;
            uint8_t *packed_data, *cmpdata;
            prrte_buffer_t datbuf, *data;
            PRRTE_CONSTRUCT(&datbuf, prrte_buffer_t);
            /* unpack the flag to see if this payload is compressed */
            idx=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &flag, &idx, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(rc);
                prted_failed_launch = true;
                goto CLEANUP;
            }
            if (flag) {
                /* unpack the data size */
                idx=1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &inlen, &idx, PRRTE_SIZE))) {
                    PRRTE_ERROR_LOG(rc);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                /* unpack the unpacked data size */
                idx=1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &cmplen, &idx, PRRTE_SIZE))) {
                    PRRTE_ERROR_LOG(rc);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                /* allocate the space */
                packed_data = (uint8_t*)malloc(inlen);
                /* unpack the data blob */
                idx = inlen;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, packed_data, &idx, PRRTE_UINT8))) {
                    PRRTE_ERROR_LOG(rc);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                /* decompress the data */
                if (prrte_compress.decompress_block(&cmpdata, cmplen,
                                                   packed_data, inlen)) {
                    /* the data has been uncompressed */
                    prrte_dss.load(&datbuf, cmpdata, cmplen);
                    data = &datbuf;
                } else {
                    data = buffer;
                }
                free(packed_data);
            } else {
                data = buffer;
            }
            /* unpack the available topology information */
            idx=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(data, &topo, &idx, PRRTE_HWLOC_TOPO))) {
                PRRTE_ERROR_LOG(rc);
                prted_failed_launch = true;
                goto CLEANUP;
            }
        }

        /* see if they provided their inventory */
        idx = 1;
        if (PRRTE_SUCCESS == prrte_dss.unpack(buffer, &bptr, &idx, PRRTE_BYTE_OBJECT)) {
            /* if nothing is present, then ignore it */
            if (0 == bptr->size) {
                free(bptr);
            } else {
                prrte_pmix_lock_t lock;
                /* load the bytes into a PMIx data buffer for unpacking */
                PMIX_DATA_BUFFER_LOAD(&pbuf, bptr->bytes, bptr->size);
                free(bptr);
                idx = 1;
                ret = PMIx_Data_unpack(NULL, &pbuf, &ninfo, &idx, PMIX_SIZE);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    rc = PRRTE_ERROR;
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PMIX_INFO_CREATE(info, ninfo);
                idx = ninfo;
                ret = PMIx_Data_unpack(NULL, &pbuf, info, &idx, PMIX_INFO);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    rc = PRRTE_ERROR;
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
                ret = PMIx_server_deliver_inventory(info, ninfo, NULL, 0, opcbfunc, &lock);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_INFO_FREE(info, ninfo);
                    rc = PRRTE_ERROR;
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                PRRTE_PMIX_WAIT_THREAD(&lock);
                PRRTE_PMIX_DESTRUCT_LOCK(&lock);
            }
        }

        /* do we already have this topology from some other node? */
        found = false;
        for (i=0; i < prrte_node_topologies->size; i++) {
            if (NULL == (t = (prrte_topology_t*)prrte_pointer_array_get_item(prrte_node_topologies, i))) {
                continue;
            }
            /* just check the signature */
            if (0 == strcmp(sig, t->sig)) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s TOPOLOGY ALREADY RECORDED",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                found = true;
                daemon->node->topology = t;
                if (NULL != topo) {
                    hwloc_topology_destroy(topo);
                }
                free(sig);
                break;
            }
#if !PRRTE_ENABLE_HETEROGENEOUS_SUPPORT
              else {
                /* check if the difference is due to the endianness */
                ptr = strrchr(sig, ':');
                ++ptr;
                if (0 != strcmp(ptr, myendian)) {
                    /* we don't currently handle multi-endian operations in the
                     * MPI support */
                    prrte_show_help("help-plm-base", "multi-endian", true,
                                   nodename, ptr, myendian);
                    prted_failed_launch = true;
                    if (NULL != topo) {
                        hwloc_topology_destroy(topo);
                    }
                    goto CLEANUP;
                }
            }
#endif
        }

        if (!found) {
            /* nope - save the signature and request the complete topology from that node */
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s NEW TOPOLOGY - ADDING",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            t = PRRTE_NEW(prrte_topology_t);
            t->sig = sig;
            t->index = prrte_pointer_array_add(prrte_node_topologies, t);
            daemon->node->topology = t;
            if (NULL != topo) {
                t->topo = topo;
            } else {
                /* nope - save the signature and request the complete topology from that node */
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s REQUESTING TOPOLOGY FROM %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&dname)));
                /* construct the request */
                relay = PRRTE_NEW(prrte_buffer_t);
                cmd = PRRTE_DAEMON_REPORT_TOPOLOGY_CMD;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(relay, &cmd, 1, PRRTE_DAEMON_CMD))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(relay);
                    prted_failed_launch = true;
                    goto CLEANUP;
                }
                /* send it */
                prrte_rml.send_buffer_nb(&dname, relay,
                                        PRRTE_RML_TAG_DAEMON,
                                        prrte_rml_send_callback, NULL);
                /* we will count this node as completed
                 * when we get the full topology back */
                if (NULL != nodename) {
                    free(nodename);
                    nodename = NULL;
                }
                idx = 1;
                continue;
            }
        }

      CLEANUP:
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:orted_report_launch %s for daemon %s at contact %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             prted_failed_launch ? "failed" : "completed",
                             PRRTE_NAME_PRINT(&dname),
                             (NULL == daemon) ? "UNKNOWN" : daemon->rml_uri));

        if (NULL != nodename) {
            free(nodename);
            nodename = NULL;
        }

        if (prted_failed_launch) {
            PRRTE_ACTIVATE_JOB_STATE(jdatorted, PRRTE_JOB_STATE_FAILED_TO_START);
            return;
        } else {
            jdatorted->num_reported++;
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s plm:base:orted_report_launch job %s recvd %d of %d reported daemons",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(jdatorted->jobid),
                                 jdatorted->num_reported, jdatorted->num_procs));
            if (jdatorted->num_procs == jdatorted->num_reported) {
                bool dvm = true;
                uint32_t key;
                void *nptr;
                jdatorted->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
                /* activate the daemons_reported state for all jobs
                 * whose daemons were launched
                 */
                rc = prrte_hash_table_get_first_key_uint32(prrte_job_data, &key, (void **)&jdata, &nptr);
                while (PRRTE_SUCCESS == rc) {
                    if (PRRTE_PROC_MY_NAME->jobid == jdata->jobid) {
                        goto next;
                    }
                    dvm = false;
                    if (PRRTE_JOB_STATE_DAEMONS_LAUNCHED == jdata->state) {
                        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
                    }
                  next:
                    rc = prrte_hash_table_get_next_key_uint32(prrte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                if (dvm) {
                    /* must be launching a DVM - activate the state */
                    PRRTE_ACTIVATE_JOB_STATE(jdatorted, PRRTE_JOB_STATE_DAEMONS_REPORTED);
                }
            }
        }
        idx = 1;
    }
    if (PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_ACTIVATE_JOB_STATE(jdatorted, PRRTE_JOB_STATE_FAILED_TO_START);
    }
}

void prrte_plm_base_daemon_failed(int st, prrte_process_name_t* sender,
                                 prrte_buffer_t *buffer,
                                 prrte_rml_tag_t tag, void *cbdata)
{
    int status, rc;
    int32_t n;
    prrte_vpid_t vpid;
    prrte_proc_t *daemon=NULL;

    /* get the daemon job, if necessary */
    if (NULL == jdatorted) {
        jdatorted = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    }

    /* unpack the daemon that failed */
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &vpid, &n, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        goto finish;
    }

    /* unpack the exit status */
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &status, &n, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        status = PRRTE_ERROR_DEFAULT_EXIT_CODE;
        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    } else {
        PRRTE_UPDATE_EXIT_STATUS(WEXITSTATUS(status));
    }

    /* find the daemon and update its state/status */
    if (NULL == (daemon = (prrte_proc_t*)prrte_pointer_array_get_item(jdatorted->procs, vpid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        goto finish;
    }
    daemon->state = PRRTE_PROC_STATE_FAILED_TO_START;
    daemon->exit_code = status;

  finish:
    if (NULL == daemon) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        return;
    }
    PRRTE_ACTIVATE_PROC_STATE(&daemon->name, PRRTE_PROC_STATE_FAILED_TO_START);
}

int prrte_plm_base_setup_prted_cmd(int *argc, char ***argv)
{
    int i, loc;
    char **tmpv;

    /* set default location to be 0, indicating that
     * only a single word is in the cmd
     */
    loc = 0;
    /* split the command apart in case it is multi-word */
    tmpv = prrte_argv_split(prrte_launch_agent, ' ');
    for (i = 0; NULL != tmpv && NULL != tmpv[i]; ++i) {
        if (0 == strcmp(tmpv[i], "prted")) {
            loc = i;
        }
        prrte_argv_append(argc, argv, tmpv[i]);
    }
    prrte_argv_free(tmpv);

    return loc;
}


/* pass all options as MCA params so anything we pickup
 * from the environment can be checked for duplicates
 */
int prrte_plm_base_prted_append_basic_args(int *argc, char ***argv,
                                          char *ess,
                                          int *proc_vpid_index)
{
    char *param = NULL;
    int i, j, cnt, rc;
    prrte_job_t *jdata;
    unsigned long num_procs;
    bool ignore;

    /* check for debug flags */
    if (prrte_debug_flag) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_debug");
        prrte_argv_append(argc, argv, "1");
    }
    if (prrte_debug_daemons_flag) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_debug_daemons");
        prrte_argv_append(argc, argv, "1");
    }
    if (prrte_debug_daemons_file_flag) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_debug_daemons_file");
        prrte_argv_append(argc, argv, "1");
    }
    if (prrte_leave_session_attached) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_leave_session_attached");
        prrte_argv_append(argc, argv, "1");
    }

    if (prrte_hwloc_report_bindings) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_report_bindings");
        prrte_argv_append(argc, argv, "1");
    }

    if (prrte_map_stddiag_to_stderr) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_map_stddiag_to_stderr");
        prrte_argv_append(argc, argv, "1");
    }
    else if (prrte_map_stddiag_to_stdout) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_map_stddiag_to_stdout");
        prrte_argv_append(argc, argv, "1");
    }

    /* the following is not an mca param */
    if (NULL != getenv("PRRTE_TEST_PRRTED_SUICIDE")) {
        prrte_argv_append(argc, argv, "--test-suicide");
    }

    /* tell the orted what ESS component to use */
    if (NULL != ess) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "ess");
        prrte_argv_append(argc, argv, ess);
    }

    /* pass the daemon jobid */
    prrte_argv_append(argc, argv, "--prtemca");
    prrte_argv_append(argc, argv, "ess_base_jobid");
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_jobid_to_string(&param, PRRTE_PROC_MY_NAME->jobid))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    prrte_argv_append(argc, argv, param);
    free(param);

    /* setup to pass the vpid */
    if (NULL != proc_vpid_index) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "ess_base_vpid");
        *proc_vpid_index = *argc;
        prrte_argv_append(argc, argv, "<template>");
    }

    /* pass the total number of daemons that will be in the system */
    if (PRRTE_PROC_IS_MASTER) {
        jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
        num_procs = jdata->num_procs;
    } else {
        num_procs = prrte_process_info.num_procs;
    }
    prrte_argv_append(argc, argv, "--prtemca");
    prrte_argv_append(argc, argv, "ess_base_num_procs");
    prrte_asprintf(&param, "%lu", num_procs);
    prrte_argv_append(argc, argv, param);
    free(param);

    /* pass the HNP uri */
    prrte_argv_append(argc, argv, "--prtemca");
    prrte_argv_append(argc, argv, "prrte_hnp_uri");
    prrte_argv_append(argc, argv, prrte_process_info.my_hnp_uri);

    /* if --xterm was specified, pass that along */
    if (NULL != prrte_xterm) {
        prrte_argv_append(argc, argv, "--prtemca");
        prrte_argv_append(argc, argv, "prrte_xterm");
        prrte_argv_append(argc, argv, prrte_xterm);
    }

    /* pass along any cmd line MCA params provided to mpirun,
     * being sure to "purge" any that would cause problems
     * on backend nodes and ignoring all duplicates
     */
    if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_DAEMON) {
        cnt = prrte_argv_count(prted_cmd_line);
        for (i=0; i < cnt; i+=3) {
            /* if the specified option is more than one word, we don't
             * have a generic way of passing it as some environments ignore
             * any quotes we add, while others don't - so we ignore any
             * such options. In most cases, this won't be a problem as
             * they typically only apply to things of interest to the HNP.
             * Individual environments can add these back into the cmd line
             * as they know if it can be supported
             */
            if (NULL != strchr(prted_cmd_line[i+2], ' ')) {
                continue;
            }
            /* The daemon will attempt to open the PLM on the remote
             * end. Only a few environments allow this, so the daemon
             * only opens the PLM -if- it is specifically told to do
             * so by giving it a specific PLM module. To ensure we avoid
             * confusion, do not include any directives here
             */
            if (0 == strcmp(prted_cmd_line[i+1], "plm")) {
                continue;
            }
            /* check for duplicate */
            ignore = false;
            for (j=0; j < *argc; j++) {
                if (0 == strcmp((*argv)[j], prted_cmd_line[i+1])) {
                    ignore = true;
                    break;
                }
            }
            if (!ignore) {
                /* pass it along */
                prrte_argv_append(argc, argv, prted_cmd_line[i]);
                prrte_argv_append(argc, argv, prted_cmd_line[i+1]);
                prrte_argv_append(argc, argv, prted_cmd_line[i+2]);
            }
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_plm_base_setup_virtual_machine(prrte_job_t *jdata)
{
    prrte_node_t *node, *nptr;
    prrte_proc_t *proc, *pptr;
    prrte_job_map_t *map=NULL;
    int rc, i;
    prrte_job_t *daemons;
    prrte_list_t nodes, tnodes;
    prrte_list_item_t *item, *next;
    prrte_app_context_t *app;
    bool one_filter = false;
    int num_nodes;
    bool default_hostfile_used;
    char *hosts = NULL;
    bool singleton=false;
    bool multi_sim = false;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:setup_vm",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    if (NULL == (daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (NULL == daemons->map) {
        daemons->map = PRRTE_NEW(prrte_job_map_t);
    }
    map = daemons->map;

    /* if this job is being launched against a fixed DVM, then there is
     * nothing for us to do - the DVM will stand as is */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FIXED_DVM, NULL, PRRTE_BOOL)) {
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
        map->num_new_daemons = 0;
        return PRRTE_SUCCESS;
    }

    /* if this is a dynamic spawn, then we don't make any changes to
     * the virtual machine unless specifically requested to do so
     */
    if (PRRTE_JOBID_INVALID != jdata->originator.jobid) {
        if (0 == map->num_nodes) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm creating map",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            /* this is the first time thru, so the vm is just getting
             * defined - create a map for it and put us in as we
             * are obviously already here! The ess will already
             * have assigned our node to us.
             */
            node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
            prrte_pointer_array_add(map->nodes, (void*)node);
            ++(map->num_nodes);
            /* maintain accounting */
            PRRTE_RETAIN(node);
            /* mark that this is from a singleton */
            singleton = true;
        }
        PRRTE_CONSTRUCT(&nodes, prrte_list_t);
        for (i=1; i < prrte_node_pool->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            /* only add in nodes marked as "added" */
            if (!singleton && PRRTE_NODE_STATE_ADDED != node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "%s plm_base:setup_vm NODE %s WAS NOT ADDED",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node->name));
                continue;
            }
            PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                 "%s plm_base:setup_vm ADDING NODE %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node->name));
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRRTE_RETAIN(node);
            prrte_list_append(&nodes, &node->super);
            /* reset the state so it can be used for mapping */
            node->state = PRRTE_NODE_STATE_UP;
        }
        map->num_new_daemons = 0;
        /* if we didn't get anything, then there is nothing else to
         * do as no other daemons are to be launched
         */
        if (0 == prrte_list_get_size(&nodes)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm no new daemons required",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            PRRTE_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
            PRRTE_FLAG_UNSET(daemons, PRRTE_JOB_FLAG_UPDATED);
            return PRRTE_SUCCESS;
        }
        /* if we got some new nodes to launch, we need to handle it */
        goto process;
    }

    /* if we are not working with a virtual machine, then we
     * look across all jobs and ensure that the "VM" contains
     * all nodes with application procs on them
     */
    multi_sim = prrte_get_attribute(&jdata->attributes, PRRTE_JOB_MULTI_DAEMON_SIM, NULL, PRRTE_BOOL);
    if (prrte_get_attribute(&daemons->attributes, PRRTE_JOB_NO_VM, NULL, PRRTE_BOOL) || multi_sim) {
        PRRTE_CONSTRUCT(&nodes, prrte_list_t);
        /* loop across all nodes and include those that have
         * num_procs > 0 && no daemon already on them
         */
        for (i=1; i < prrte_node_pool->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRRTE_NODE_STATE_UP;
                continue;
            }
            if (PRRTE_NODE_STATE_DOWN == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            if (0 < node->num_procs || multi_sim) {
                /* retain a copy for our use in case the item gets
                 * destructed along the way
                 */
                PRRTE_RETAIN(node);
                prrte_list_append(&nodes, &node->super);
            }
        }
        if (multi_sim) {
            goto process;
        }
        /* see if anybody had procs */
        if (0 == prrte_list_get_size(&nodes)) {
            /* if the HNP has some procs, then we are still good */
            node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
            if (0 < node->num_procs) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s plm:base:setup_vm only HNP in use",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                PRRTE_DESTRUCT(&nodes);
                map->num_nodes = 1;
                /* mark that the daemons have reported so we can proceed */
                daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
                return PRRTE_SUCCESS;
            }
            /* well, if the HNP doesn't have any procs, and neither did
             * anyone else...then we have a big problem
             */
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRRTE_ERR_FATAL;
        }
        goto process;
    }

    if (0 == map->num_nodes) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm creating map",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        /* this is the first time thru, so the vm is just getting
         * defined - put us in as we
         * are obviously already here! The ess will already
         * have assigned our node to us.
         */
        node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
        prrte_pointer_array_add(map->nodes, (void*)node);
        ++(map->num_nodes);
        /* maintain accounting */
        PRRTE_RETAIN(node);
    }

    /* zero-out the number of new daemons as we will compute this
     * each time we are called
     */
    map->num_new_daemons = 0;

    /* setup the list of nodes */
    PRRTE_CONSTRUCT(&nodes, prrte_list_t);

    /* if this is an unmanaged allocation, then we use
     * the nodes that were specified for the union of
     * all apps - there is no need to collect all
     * available nodes and "filter" them
     */
    if (!prrte_managed_allocation) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s setup:vm: working unmanaged allocation",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        default_hostfile_used = false;
        PRRTE_CONSTRUCT(&tnodes, prrte_list_t);
        for (i=0; i < jdata->apps->size; i++) {
            if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
                continue;
            }
            /* if the app provided a dash-host, and we are not treating
             * them as requested or "soft" locations, then use those nodes
             */
            hosts = NULL;
            if (!prrte_soft_locations &&
                prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s using dash_host",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                if (PRRTE_SUCCESS != (rc = prrte_util_add_dash_host_nodes(&tnodes, hosts, false))) {
                    PRRTE_ERROR_LOG(rc);
                    free(hosts);
                    return rc;
                }
                free(hosts);
            } else if (prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
                /* otherwise, if the app provided a hostfile, then use that */
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s using hostfile %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts));
                if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&tnodes, hosts))) {
                    PRRTE_ERROR_LOG(rc);
                    free(hosts);
                    return rc;
                }
                free(hosts);
            } else if (NULL != prrte_rankfile) {
                /* use the rankfile, if provided */
                PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                     "%s using rankfile %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     prrte_rankfile));
                if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&tnodes,
                                                                       prrte_rankfile))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            } else if (NULL != prrte_default_hostfile) {
                if (!default_hostfile_used) {
                    /* fall back to the default hostfile, if provided */
                    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                         "%s using default hostfile %s",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         prrte_default_hostfile));
                    if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&tnodes,
                                                                           prrte_default_hostfile))) {
                        PRRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    /* only include it once */
                    default_hostfile_used = true;
                }
            }
        }
        /* cycle thru the resulting list, finding the nodes on
         * the node pool array while removing ourselves
         * and all nodes that are down or otherwise unusable
         */
        while (NULL != (item = prrte_list_remove_first(&tnodes))) {
            nptr = (prrte_node_t*)item;
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s checking node %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 nptr->name));
            for (i=0; i < prrte_node_pool->size; i++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                    continue;
                }
                if (0 != strcmp(node->name, nptr->name)) {
                    continue;
                }
                /* have a match - now see if we want this node */
                /* ignore nodes that are marked as do-not-use for this mapping */
                if (PRRTE_NODE_STATE_DO_NOT_USE == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_USE", node->name));
                    /* reset the state so it can be used another time */
                    node->state = PRRTE_NODE_STATE_UP;
                    break;
                }
                if (PRRTE_NODE_STATE_DOWN == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED DOWN", node->name));
                    break;
                }
                if (PRRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_INCLUDE", node->name));
                    break;
                }
                /* if this node is us, ignore it */
                if (0 == node->index) {
                    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                         "%s ignoring myself",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                    break;
                }
                /* we want it - add it to list */
                PRRTE_RETAIN(node);
                prrte_list_append(&nodes, &node->super);
            }
            PRRTE_RELEASE(nptr);
        }
        PRRTE_LIST_DESTRUCT(&tnodes);
        /* if we didn't get anything, then we are the only node in the
         * allocation - so there is nothing else to do as no other
         * daemons are to be launched
         */
        if (0 == prrte_list_get_size(&nodes)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                                 "%s plm:base:setup_vm only HNP in allocation",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            PRRTE_DESTRUCT(&nodes);
            /* mark that the daemons have reported so we can proceed */
            daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
            PRRTE_FLAG_UNSET(daemons, PRRTE_JOB_FLAG_UPDATED);
            return PRRTE_SUCCESS;
        }
        /* continue processing */
        goto process;
    }

    /* construct a list of available nodes */
    for (i=1; i < prrte_node_pool->size; i++) {
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRRTE_NODE_STATE_UP;
                continue;
            }
            if (PRRTE_NODE_STATE_DOWN == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRRTE_RETAIN(node);
            prrte_list_append(&nodes, &node->super);
            /* by default, mark these as not to be included
             * so the filtering logic works correctly
             */
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * system - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == prrte_list_get_size(&nodes)) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP in allocation",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        /* cleanup */
        PRRTE_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
        PRRTE_FLAG_UNSET(daemons, PRRTE_JOB_FLAG_UPDATED);
        return PRRTE_SUCCESS;
    }

    /* filter across the union of all app_context specs - if the HNP
     * was allocated, then we have to include
     * ourselves in case someone has specified a -host or hostfile
     * that includes the head node. We will remove ourselves later
     * as we clearly already exist
     */
    if (prrte_hnp_is_allocated) {
        node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
        PRRTE_RETAIN(node);
        prrte_list_prepend(&nodes, &node->super);
    }
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_filter_nodes(app, &nodes, false)) &&
            rc != PRRTE_ERR_TAKE_NEXT_OPTION) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRRTE_SUCCESS == rc) {
            /* we filtered something */
            one_filter = true;
        }
    }

    if (one_filter) {
        /* at least one filtering option was executed, so
         * remove all nodes that were not mapped
         */
        item = prrte_list_get_first(&nodes);
        while (item != prrte_list_get_end(&nodes)) {
            next = prrte_list_get_next(item);
            node = (prrte_node_t*)item;
            if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_MAPPED)) {
                prrte_list_remove_item(&nodes, item);
                PRRTE_RELEASE(item);
            } else {
                /* The filtering logic sets this flag only for nodes which
                 * are kept after filtering. This flag will be subsequently
                 * used in rmaps components and must be reset here */
                PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
            }
            item = next;
        }
    }

    /* ensure we are not on the list */
    if (0 < prrte_list_get_size(&nodes)) {
        item = prrte_list_get_first(&nodes);
        node = (prrte_node_t*)item;
        if (0 == node->index) {
            prrte_list_remove_item(&nodes, item);
            PRRTE_RELEASE(item);
        }
    }

    /* if we didn't get anything, then we are the only node in the
     * allocation - so there is nothing else to do as no other
     * daemons are to be launched
     */
    if (0 == prrte_list_get_size(&nodes)) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm only HNP left",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_DESTRUCT(&nodes);
        /* mark that the daemons have reported so we can proceed */
        daemons->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;
        PRRTE_FLAG_UNSET(daemons, PRRTE_JOB_FLAG_UPDATED);
        return PRRTE_SUCCESS;
    }

 process:
    /* cycle thru all available nodes and find those that do not already
     * have a daemon on them - no need to include our own as we are
     * obviously already here! If a max vm size was given, then limit
     * the overall number of active nodes to the given number. Only
     * count the HNP's node if it was included in the allocation
     */
    if (prrte_hnp_is_allocated) {
        num_nodes = 1;
    } else {
        num_nodes = 0;
    }
    while (NULL != (item = prrte_list_remove_first(&nodes))) {
        /* if a max size was given and we are there, then exit the loop */
        if (0 < prrte_max_vm_size && num_nodes == prrte_max_vm_size) {
            /* maintain accounting */
            PRRTE_RELEASE(item);
            break;
        }
        node = (prrte_node_t*)item;
        /* if this node is already in the map, skip it */
        if (NULL != node->daemon) {
            num_nodes++;
            /* maintain accounting */
            PRRTE_RELEASE(item);
            continue;
        }
        /* add the node to the map - we retained it
         * when adding it to the list, so we don't need
         * to retain it again
         */
        prrte_pointer_array_add(map->nodes, (void*)node);
        ++(map->num_nodes);
        num_nodes++;
        /* create a new daemon object for this node */
        proc = PRRTE_NEW(prrte_proc_t);
        if (NULL == proc) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        proc->name.jobid = PRRTE_PROC_MY_NAME->jobid;
        if (PRRTE_VPID_MAX-1 <= daemons->num_procs) {
            /* no more daemons available */
            prrte_show_help("help-prrte-rmaps-base.txt", "out-of-vpids", true);
            PRRTE_RELEASE(proc);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        proc->name.vpid = daemons->num_procs;  /* take the next available vpid */
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm add new daemon %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name)));
        /* add the daemon to the daemon job object */
        if (0 > (rc = prrte_pointer_array_set_item(daemons->procs, proc->name.vpid, (void*)proc))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        ++daemons->num_procs;
        PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                             "%s plm:base:setup_vm assigning new daemon %s to node %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name),
                             node->name));
        /* point the node to the daemon */
        node->daemon = proc;
        PRRTE_RETAIN(proc);  /* maintain accounting */
        /* point the proc to the node and maintain accounting */
        proc->node = node;
        PRRTE_RETAIN(node);
        if (prrte_plm_globals.daemon_nodes_assigned_at_launch) {
            PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_LOC_VERIFIED);
        } else {
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_LOC_VERIFIED);
        }
        /* track number of daemons to be launched */
        ++map->num_new_daemons;
        /* and their starting vpid */
        if (PRRTE_VPID_INVALID == map->daemon_vpid_start) {
            map->daemon_vpid_start = proc->name.vpid;
        }
        /* loop across all app procs on this node and update their parent */
        for (i=0; i < node->procs->size; i++) {
            if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                pptr->parent = proc->name.vpid;
            }
        }
    }

    if (prrte_process_info.num_procs != daemons->num_procs) {
        /* more daemons are being launched - update the routing tree to
         * ensure that the HNP knows how to route messages via
         * the daemon routing tree - this needs to be done
         * here to avoid potential race conditions where the HNP
         * hasn't unpacked its launch message prior to being
         * asked to communicate.
         */
        prrte_process_info.num_procs = daemons->num_procs;

        if (prrte_process_info.max_procs < prrte_process_info.num_procs) {
            prrte_process_info.max_procs = prrte_process_info.num_procs;
        }

        /* ensure all routing plans are up-to-date - we need this
         * so we know how to tree-spawn and/or xcast info */
        prrte_routed.update_routing_plan();
    }

    /* mark that the daemon job changed */
    PRRTE_FLAG_SET(daemons, PRRTE_JOB_FLAG_UPDATED);

    /* if new daemons are being launched, mark that this job
     * caused it to happen */
    if (0 < map->num_new_daemons) {
        if (PRRTE_SUCCESS != (rc = prrte_set_attribute(&jdata->attributes, PRRTE_JOB_LAUNCHED_DAEMONS,
                                                     true, NULL, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRRTE_SUCCESS;
}
