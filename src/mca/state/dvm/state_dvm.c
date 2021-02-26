/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#include "src/util/nidmap.h"
#include "src/util/output.h"
#include "src/util/os_dirpath.h"
#include "src/util/proc_info.h"
#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/filem/filem.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/routed.h"
#include "src/threads/threads.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/prte_data_server.h"

#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"
#include "state_dvm.h"

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

/* local functions */
static void init_complete(int fd, short args, void *cbdata);
static void vm_ready(int fd, short args, void *cbata);
static void check_complete(int fd, short args, void *cbdata);
static void cleanup_job(int fd, short args, void *cbdata);
static void job_started(int fd, short args, void *cbata);
static void ready_for_debug(int fd, short args, void *cbata);

/******************
 * DVM module - used when mpirun is persistent
 ******************/
prte_state_base_module_t prte_state_dvm_module = {
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

static void dvm_notify(int sd, short args, void *cbdata);

/* defined default state machine sequence - individual
 * plm's must add a state for launching daemons
 */
static prte_job_state_t launch_states[] = {
    PRTE_JOB_STATE_INIT,
    PRTE_JOB_STATE_INIT_COMPLETE,
    PRTE_JOB_STATE_ALLOCATE,
    PRTE_JOB_STATE_ALLOCATION_COMPLETE,
    PRTE_JOB_STATE_DAEMONS_LAUNCHED,
    PRTE_JOB_STATE_DAEMONS_REPORTED,
    PRTE_JOB_STATE_VM_READY,
    PRTE_JOB_STATE_MAP,
    PRTE_JOB_STATE_MAP_COMPLETE,
    PRTE_JOB_STATE_SYSTEM_PREP,
    PRTE_JOB_STATE_LAUNCH_APPS,
    PRTE_JOB_STATE_SEND_LAUNCH_MSG,
    PRTE_JOB_STATE_STARTED,
    PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE,
    PRTE_JOB_STATE_READY_FOR_DEBUG,
    PRTE_JOB_STATE_RUNNING,
    PRTE_JOB_STATE_REGISTERED,
    /* termination states */
    PRTE_JOB_STATE_TERMINATED,
    PRTE_JOB_STATE_NOTIFY_COMPLETED,
    PRTE_JOB_STATE_NOTIFIED,
    PRTE_JOB_STATE_ALL_JOBS_COMPLETE
};
static prte_state_cbfunc_t launch_callbacks[] = {
    prte_plm_base_setup_job,
    init_complete,
    prte_ras_base_allocate,
    prte_plm_base_allocation_complete,
    prte_plm_base_daemons_launched,
    prte_plm_base_daemons_reported,
    vm_ready,
    prte_rmaps_base_map_job,
    prte_plm_base_mapping_complete,
    prte_plm_base_complete_setup,
    prte_plm_base_launch_apps,
    prte_plm_base_send_launch_msg,
    job_started,
    prte_state_base_local_launch_complete,
    ready_for_debug,
    prte_plm_base_post_launch,
    prte_plm_base_registered,
    check_complete,
    dvm_notify,
    cleanup_job,
    prte_quit
};

static prte_proc_state_t proc_states[] = {
    PRTE_PROC_STATE_RUNNING,
    PRTE_PROC_STATE_REGISTERED,
    PRTE_PROC_STATE_IOF_COMPLETE,
    PRTE_PROC_STATE_WAITPID_FIRED,
    PRTE_PROC_STATE_TERMINATED
};
static prte_state_cbfunc_t proc_callbacks[] = {
    prte_state_base_track_procs,
    prte_state_base_track_procs,
    prte_state_base_track_procs,
    prte_state_base_track_procs,
    prte_state_base_track_procs
};

static void force_quit(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    /* give us a chance to stop the orteds */
    prte_plm.terminate_orteds();
    PRTE_RELEASE(caddy);
}

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int i, rc;
    int num_states;

    /* setup the state machines */
    PRTE_CONSTRUCT(&prte_job_states, prte_list_t);
    PRTE_CONSTRUCT(&prte_proc_states, prte_list_t);

    /* setup the job state machine */
    num_states = sizeof(launch_states) / sizeof(prte_job_state_t);
    for (i=0; i < num_states; i++) {
        if (PRTE_SUCCESS != (rc = prte_state.add_job_state(launch_states[i],
                                                           launch_callbacks[i],
                                                           PRTE_SYS_PRI))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    /* add the termination response */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_DAEMONS_TERMINATED,
                                                       prte_quit, PRTE_SYS_PRI))) {
        PRTE_ERROR_LOG(rc);
    }
    /* add a default error response */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_FORCED_EXIT,
                                                       force_quit, PRTE_ERROR_PRI))) {
        PRTE_ERROR_LOG(rc);
    }
    /* add callback to report progress, if requested */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_REPORT_PROGRESS,
                                                       prte_state_base_report_progress, PRTE_ERROR_PRI))) {
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

    /* cleanup the proc state machine */
    while (NULL != (item = prte_list_remove_first(&prte_proc_states))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_proc_states);

    return PRTE_SUCCESS;
}

static void files_ready(int status, void *cbdata)
{
    prte_job_t *jdata = (prte_job_t*)cbdata;

    if (PRTE_SUCCESS != status) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    } else {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    }
}

static void init_complete(int sd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* nothing to do here but move along - if it is the
     * daemon job, then next step is allocate */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, caddy->jdata->nspace)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_ALLOCATE);
    } else {
        /* we already did an allocation when we launched the DVM,
         * so skip that step */
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_ALLOCATION_COMPLETE);
    }
    PRTE_RELEASE(caddy);
}

static void vm_ready(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    int rc, i;
    pmix_data_buffer_t *buf;
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_PASS_NODE_INFO_CMD;
    prte_grpcomm_signature_t sig;
    pmix_data_buffer_t *wireup;
    prte_job_t *jptr;
    prte_proc_t *dmn;
    pmix_byte_object_t bo;
    int32_t v;
    pmix_value_t *val;
    pmix_info_t info;
    size_t ninfo;
    pmix_data_buffer_t pbuf;
    pmix_status_t ret;
    pmix_byte_object_t pbo;

    PRTE_ACQUIRE_OBJECT(caddy);

    /* if this is my job, then we are done */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, caddy->jdata->nspace)) {
        prte_dvm_ready = true;
        /* if there is only one daemon in the job, then there
         * is just a little bit to do */
        if (!prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) &&
            1 < prte_process_info.num_daemons) {
            /* send the daemon map to every daemon in this DVM - we
             * do this here so we don't have to do it for every
             * job we are going to launch */
            PMIX_DATA_BUFFER_CREATE(buf);
            rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                return;
            }
            rc = prte_util_nidmap_create(prte_node_pool, buf);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                return;
            }
            /* provide the info on the capabilities of each node */
            if (PRTE_SUCCESS != (rc = prte_util_pass_node_info(buf))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                return;
            }
            /* get wireup info for daemons */
            jptr = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            PMIX_DATA_BUFFER_CREATE(wireup);
            for (v=0; v < jptr->procs->size; v++) {
                if (NULL == (dmn = (prte_proc_t*)prte_pointer_array_get_item(jptr->procs, v))) {
                    continue;
                }
                val = NULL;
                if (PMIX_SUCCESS != (ret = PMIx_Get(&dmn->name, PMIX_PROC_URI, NULL, 0, &val)) || NULL == val) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    return;
                }
                /* use the PMIx data support to pack it */
                PMIX_INFO_LOAD(&info, PMIX_PROC_URI, val->data.string, PMIX_STRING);
                PMIX_VALUE_RELEASE(val);
                ninfo = 1;
                PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&dmn->name, &pbuf, &ninfo, 1, PMIX_SIZE))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_INFO_DESTRUCT(&info);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    return;
                }
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&dmn->name, &pbuf, &info, ninfo, PMIX_INFO))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_INFO_DESTRUCT(&info);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                    return;
                }
                PMIX_INFO_DESTRUCT(&info);
                rc = PMIx_Data_unload(&pbuf, &pbo);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_INFO_DESTRUCT(&info);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                }
                rc = PMIx_Data_pack(NULL, wireup, &dmn->name, 1, PMIX_PROC);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_INFO_DESTRUCT(&info);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                }
                rc = PMIx_Data_pack(NULL, wireup, &pbo, 1, PMIX_BYTE_OBJECT);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PMIX_DATA_BUFFER_RELEASE(wireup);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_INFO_DESTRUCT(&info);
                    PMIX_DATA_BUFFER_RELEASE(buf);
                }
            }
            /* put it in a byte object for xmission */
            rc = PMIx_Data_unload(wireup, &bo);
            PMIX_DATA_BUFFER_RELEASE(wireup);
            /* pack the byte object - zero-byte objects are fine */
            rc = PMIx_Data_pack(NULL, buf, &bo, 1, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                return;
            }
            /* release the data since it has now been copied into our buffer */
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);

            /* goes to all daemons */
            PMIX_PROC_CREATE(sig.signature, 1);
            PMIX_LOAD_PROCID(&sig.signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
            sig.sz = 1;
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(&sig, PRTE_RML_TAG_DAEMON, buf))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                PMIX_PROC_FREE(sig.signature, 1);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                return;
            }
            PMIX_DATA_BUFFER_RELEASE(buf);
            PMIX_PROC_FREE(sig.signature, 1);
        }
        /* notify that the vm is ready */
        if (0 > prte_state_base_parent_fd) {
            if (prte_state_base_ready_msg && prte_persistent) {
                fprintf(stdout, "DVM ready\n"); fflush(stdout);
            }
        } else {
            char ok = 'K';
            write(prte_state_base_parent_fd, &ok, 1);
            close(prte_state_base_parent_fd);
            prte_state_base_parent_fd = -1;
        }

        for (i=0; i < prte_cache->size; i++) {
            jptr = (prte_job_t*)prte_pointer_array_get_item(prte_cache, i);
            if (NULL != jptr) {
                prte_pointer_array_set_item(prte_cache, i, NULL);
                prte_plm.spawn(jptr);
            }
        }

        /* progress the job */
        caddy->jdata->state = PRTE_JOB_STATE_VM_READY;
        PRTE_RELEASE(caddy);
        return;
    }

    /* position any required files */
    if (PRTE_SUCCESS != prte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    }
    PRTE_RELEASE(caddy);
}

static void job_started(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata = caddy->jdata;
    pmix_info_t *iptr;
    time_t timestamp;

    /* if there is an originator for this job, notify them
     * that the first process of the job has been started */

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, NULL, PMIX_BOOL)) {
        timestamp = time(NULL);
        PMIX_INFO_CREATE(iptr, 4);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, &jdata->originator, PMIX_PROC);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[1], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
        /* not to be delivered to a default event handler */
        PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        /* provide the timestamp */
        PMIX_INFO_LOAD(&iptr[3], PMIX_EVENT_TIMESTAMP, &timestamp, PMIX_TIME);
        PMIx_Notify_event(PMIX_EVENT_JOB_START, &prte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          iptr, 4, NULL, NULL);
        PMIX_INFO_FREE(iptr, 4);
    }

    PRTE_RELEASE(caddy);
}

static void ready_for_debug(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata = caddy->jdata;

    /* track number of procs at this point */
    jdata->num_ready_for_debug++;

    /* check if all have reported */
    if (jdata->num_procs == jdata->num_ready_for_debug) {
        /* generate the event notifying any connected tool that
         * the specified job is ready for debug */
    }
    PRTE_RELEASE(caddy);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t*)cbdata;

    PRTE_POST_OBJECT(lk);
    lk->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lk);
}
static void check_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata, *jptr;
    prte_proc_t *proc;
    int i, rc;
    prte_node_t *node;
    prte_job_map_t *map;
    int32_t index;
    pmix_proc_t pname;
    prte_pmix_lock_t lock;
    uint8_t command = PRTE_PMIX_PURGE_PROC_CMD;
    pmix_data_buffer_t *buf;
    prte_pointer_array_t procs;
    char *tmp;
    prte_timer_t *timer;
    int num_tools_attached = 0;

    PRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    prte_output_verbose(2, prte_state_base_framework.framework_output,
                        "%s state:dvm:check_job_complete on job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == jdata) ? "NULL" : PRTE_JOBID_PRINT(jdata->nspace));

    if (NULL != jdata && prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, (void**)&timer, PMIX_POINTER)) {
        /* timer is an prte_timer_t object */
        PRTE_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT);
    }

    if (NULL == jdata || PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        /* just check to see if the daemons are complete */
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:check_job_complete - received NULL job, checking daemons",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        if (0 == prte_routed.num_routes()) {
            /* orteds are done! */
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s orteds complete - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            if (NULL == jdata) {
                jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            }
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            PRTE_RELEASE(caddy);
            prte_dvm_ready = false;
            return;
        }
        PRTE_RELEASE(caddy);
        return;
    }

    /* mark the job as terminated, but don't override any
     * abnormal termination flags
     */
    if (jdata->state < PRTE_JOB_STATE_UNTERMINATED) {
        jdata->state = PRTE_JOB_STATE_TERMINATED;
    }

    /* cleanup any pending server ops */
    PMIX_LOAD_PROCID(&pname, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pmix_server_clear(&pname);

    /* cleanup the procs as these are gone */
    for (i=0; i < prte_local_children->size; i++) {
        if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
            /* clear the entry in the local children */
            prte_pointer_array_set_item(prte_local_children, i, NULL);
            PRTE_RELEASE(proc);  // maintain accounting
        }
    }

    /* tell the IOF that the job is complete */
    if (NULL != prte_iof.complete) {
        prte_iof.complete(jdata);
    }

    /* tell the PMIx subsystem the job is complete */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_server_deregister_nspace(pname.nspace, opcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (!prte_persistent) {
        /* update our exit status */
        PRTE_UPDATE_EXIT_STATUS(jdata->exit_code);
        /* if this is an abnormal termination, report it */
        if (jdata->state > PRTE_JOB_STATE_ERROR) {
            char *msg;
            msg = prte_dump_aborted_procs(jdata);
            if (NULL != msg) {
                prte_output(prte_clean_output, "%s", msg);
                free(msg);
            }
        }
        /* if all of the jobs we are running are done, then shut us down */
        for (i=0; i < prte_job_data->size; i++) {
            jptr = (prte_job_t*)prte_pointer_array_get_item(prte_job_data, i);
            if (NULL == jptr) {
                continue;
            }
            /* skip the daemon job */
            if (PMIX_CHECK_NSPACE(jptr->nspace, PRTE_PROC_MY_NAME->nspace)) {
                continue;
            }
            /* if this is a tool it might be interested in the terminated event */
            if (PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_TOOL)) {
                ++num_tools_attached;
            }
            /* if the job is flagged to not be monitored, skip it */
            if (PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_DO_NOT_MONITOR)) {
                continue;
            }
            if (jptr->state < PRTE_JOB_STATE_TERMINATED) {
                /* still alive - finish processing this job's termination */
                goto release;
            }
        }

        /* Let the tools know that a job terminated before we shutdown */
        if (num_tools_attached > 0 && jdata->state != PRTE_JOB_STATE_NOTIFIED) {
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:dvm:check_job_completed state is terminated - activating notify",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFY_COMPLETED);
            /* mark the job as notified */
            jdata->state = PRTE_JOB_STATE_NOTIFIED;
        }

        /* if we fell thru to this point, then nobody is still
         * alive except the daemons, so just shut us down */
        prte_plm.terminate_orteds();
        PRTE_RELEASE(caddy);
        return;
    }

    if (NULL != prte_data_server_uri) {
        /* tell the data server to purge any data from this nspace */
        PMIX_DATA_BUFFER_CREATE(buf);
        /* room number is ignored, but has to be included for pack sequencing */
        i=0;
        rc = PMIx_Data_pack(NULL, buf, &i, 1, PMIX_INT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto release;
        }
        rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto release;
        }
        /* pack the nspace to be purged */
        pname.rank = PMIX_RANK_WILDCARD;
        rc = PMIx_Data_pack(NULL, buf, &pname, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto release;
        }
        /* send it to the data server */
        rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_NAME, buf,
                                     PRTE_RML_TAG_DATA_SERVER,
                                     prte_rml_send_callback, NULL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PRTE_RELEASE(buf);
        }
    }

  release:
    /* Release the resources used by this job. Since some errmgrs may want
     * to continue using resources allocated to the job as part of their
     * fault recovery procedure, we only do this once the job is "complete".
     * Note that an aborted/killed job -is- flagged as complete and will
     * therefore have its resources released. We need to do this after
     * we call the errmgr so that any attempt to restart the job will
     * avoid doing so in the exact same place as the current job
     */
    if (NULL != jdata->map) {
        map = jdata->map;
        for (index = 0; index < map->nodes->size; index++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, index))) {
                continue;
            }
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:dvm releasing procs from node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 node->name));
            for (i = 0; i < node->procs->size; i++) {
                if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                    continue;
                }
                if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                    /* skip procs from another job */
                    continue;
                }
                if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
                    !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                    node->slots_inuse--;
                    node->num_procs--;
                    node->next_node_rank--;
                }

                PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                     "%s state:dvm releasing proc %s from node %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name), node->name));
                /* set the entry in the node array to NULL */
                prte_pointer_array_set_item(node->procs, i, NULL);
                /* release the proc once for the map entry */
                PRTE_RELEASE(proc);
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

    /* if this job was a launcher, then we need to abort all of its
     * child jobs that might still be running */
    if (0 < prte_list_get_size(&jdata->children)) {
        PRTE_CONSTRUCT(&procs, prte_pointer_array_t);
        prte_pointer_array_init(&procs, 1, INT_MAX, 1);
        PRTE_LIST_FOREACH(jptr, &jdata->children, prte_job_t) {
            proc = PRTE_NEW(prte_proc_t);
            PMIX_LOAD_PROCID(&proc->name, jptr->nspace, PMIX_RANK_WILDCARD);
            prte_pointer_array_add(&procs, proc);

        }
        prte_plm.terminate_procs(&procs);
        for (i=0; i < procs.size; i++) {
            if (NULL != (proc = (prte_proc_t*)prte_pointer_array_get_item(&procs, i))) {
                PRTE_RELEASE(proc);
            }
        }
        PRTE_DESTRUCT(&procs);
    }

    /* remove the session directory tree */
    if (0 > prte_asprintf(&tmp, "%s/%u", prte_process_info.jobfam_session_dir, PRTE_LOCAL_JOBID(jdata->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
    } else {
        prte_os_dirpath_destroy(tmp, true, NULL);
        free(tmp);
    }

    if (jdata->state != PRTE_JOB_STATE_NOTIFIED) {
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:check_job_completed state is terminated - activating notify",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFY_COMPLETED);
        /* mark the job as notified */
        jdata->state = PRTE_JOB_STATE_NOTIFIED;
    }

    PRTE_RELEASE(caddy);
}

static void cleanup_job(int sd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(caddy);

    PRTE_RELEASE(caddy);
}

static void dvm_notify(int sd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata = caddy->jdata;
    prte_proc_t *pptr=NULL;
    int rc;
    pmix_data_buffer_t *reply;
    prte_daemon_cmd_flag_t command;
    prte_grpcomm_signature_t sig;
    bool notify = true, flag;
    pmix_proc_t *proc, pnotify;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t pname;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    char *errmsg = NULL;

    PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                         "%s state:dvm:dvm_notify called",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* see if there was any problem */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, (void**)&pptr, PMIX_POINTER) && NULL != pptr) {
        rc = jdata->exit_code;
    /* or whether we got cancelled by the user */
    } else if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CANCELLED, NULL, PMIX_BOOL)) {
        rc = PRTE_ERR_JOB_CANCELLED;
    } else {
        rc = jdata->exit_code;
    }

    if (0 == rc && prte_get_attribute(&jdata->attributes, PRTE_JOB_SILENT_TERMINATION, NULL, PMIX_BOOL)) {
        notify = false;
    }
    /* if the jobid matches that of the requestor, then don't notify */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void**)&proc, PMIX_PROC)) {
        if (PMIX_CHECK_NSPACE(proc->nspace, jdata->nspace)) {
            notify = false;
        }
    }

    if (notify) {
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:dvm_notify notification requested",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    #ifdef PMIX_EVENT_TEXT_MESSAGE
        /* if it was an abnormal termination, then construct an appropriate
         * error message */
        if (PRTE_SUCCESS != rc) {
            errmsg = prte_dump_aborted_procs(jdata);
        }
    #else
        errmsg = NULL;
    #endif
        /* construct the info to be provided */
        if (NULL == errmsg) {
            ninfo = 3;
        } else {
            ninfo = 4;
        }
        PMIX_INFO_CREATE(info, ninfo);
        /* ensure this only goes to the job terminated event handler */
        flag = true;
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_NON_DEFAULT, &flag, PMIX_BOOL);
        /* provide the status */
        PMIX_INFO_LOAD(&info[1], PMIX_JOB_TERM_STATUS, &rc, PMIX_STATUS);
        /* tell the requestor which job or proc  */
        PMIX_LOAD_NSPACE(pname.nspace, jdata->nspace);
        if (NULL != pptr) {
            pname.rank = pptr->name.rank;
        } else {
            pname.rank = PMIX_RANK_WILDCARD;
        }
        PMIX_INFO_LOAD(&info[2], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    #ifdef PMIX_EVENT_TEXT_MESSAGE
        if (NULL != errmsg) {
            PMIX_INFO_LOAD(&info[3], PMIX_EVENT_TEXT_MESSAGE, errmsg, PMIX_STRING);
            free(errmsg);
        }
    #endif

        /* pack the info for sending */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

        /* pack the status code */
        code = PMIX_ERR_JOB_TERMINATED;
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* pack the source - it cannot be me as that will cause
         * the pmix server to upcall the event back to me */
        PMIX_LOAD_PROCID(&pnotify, jdata->nspace, 0);
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &pnotify, 1, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* pack the range */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* pack the number of infos */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* pack the infos themselves */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        PMIX_INFO_FREE(info, ninfo);

        /* insert into pmix_data_buffer_t */
        PMIX_DATA_BUFFER_CREATE(reply);
        /* we need to add a flag indicating this came from an invalid proc so that we will
         * inject it into our own PMIx server library */
        rc = PMIx_Data_pack(NULL, reply, &PRTE_NAME_INVALID->rank, 1, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        rc = PMIx_Data_copy_payload(reply, &pbkt);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }

        /* we have to send the notification to all daemons so that
         * anyone watching for it can receive it */
        PMIX_PROC_CREATE(sig.signature, 1);
        PMIX_LOAD_PROCID(&sig.signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
        sig.sz = 1;
        if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(&sig, PRTE_RML_TAG_NOTIFICATION, reply))) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_PROC_FREE(sig.signature, 1);
            return;
        }
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:dvm_notify notification sent",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_DATA_BUFFER_RELEASE(reply);
        /* maintain accounting */
        PMIX_PROC_FREE(sig.signature, 1);
    }

    /* now ensure that _all_ daemons know that this job has terminated so even
     * those that did not participate in it will know to cleanup the resources
     * they assigned to the job. This is necessary now that the mapping function
     * has been moved to the backend daemons - otherwise, non-participating daemons
     * retain the slot assignments on the participating daemons, and then incorrectly
     * map subsequent jobs thinking those nodes are still "busy" */
    PMIX_DATA_BUFFER_CREATE(reply);
    command = PRTE_DAEMON_DVM_CLEANUP_JOB_CMD;
    rc = PMIx_Data_pack(NULL, reply, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
#if PMIX_NUMERIC_VERSION < 0x00040100
    char *tmp = NULL;
    if (0 < strlen(jdata->nspace)) {
        tmp = strdup(jdata->nspace);
    }
    rc = PMIx_Data_pack(NULL, reply, (void*)&tmp, 1, PMIX_STRING);
    if (NULL != tmp) {
        free(tmp);
    }
#else
    rc = PMIx_Data_pack(NULL, reply, &jdata->nspace, 1, PMIX_PROC_NSPACE);
#endif
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    PMIX_PROC_CREATE(sig.signature, 1);
    PMIX_LOAD_PROCID(&sig.signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    sig.sz = 1;
    prte_grpcomm.xcast(&sig, PRTE_RML_TAG_DAEMON, reply);
    PMIX_DATA_BUFFER_RELEASE(reply);
    PMIX_PROC_FREE(sig.signature, 1);
    PRTE_RELEASE(caddy);

    // We are done with our use of job data and have notified the other daemons
    if (notify) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFIED);
    }
}
