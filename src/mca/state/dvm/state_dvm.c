/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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
#include "src/runtime/prrte_quit.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_data_server.h"

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

/******************
 * DVM module - used when mpirun is persistent
 ******************/
prrte_state_base_module_t prrte_state_dvm_module = {
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

static void dvm_notify(int sd, short args, void *cbdata);

/* defined default state machine sequence - individual
 * plm's must add a state for launching daemons
 */
static prrte_job_state_t launch_states[] = {
    PRRTE_JOB_STATE_INIT,
    PRRTE_JOB_STATE_INIT_COMPLETE,
    PRRTE_JOB_STATE_ALLOCATE,
    PRRTE_JOB_STATE_ALLOCATION_COMPLETE,
    PRRTE_JOB_STATE_DAEMONS_LAUNCHED,
    PRRTE_JOB_STATE_DAEMONS_REPORTED,
    PRRTE_JOB_STATE_VM_READY,
    PRRTE_JOB_STATE_MAP,
    PRRTE_JOB_STATE_MAP_COMPLETE,
    PRRTE_JOB_STATE_SYSTEM_PREP,
    PRRTE_JOB_STATE_LAUNCH_APPS,
    PRRTE_JOB_STATE_SEND_LAUNCH_MSG,
    PRRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE,
    PRRTE_JOB_STATE_RUNNING,
    PRRTE_JOB_STATE_REGISTERED,
    /* termination states */
    PRRTE_JOB_STATE_TERMINATED,
    PRRTE_JOB_STATE_NOTIFY_COMPLETED,
    PRRTE_JOB_STATE_NOTIFIED,
    PRRTE_JOB_STATE_ALL_JOBS_COMPLETE
};
static prrte_state_cbfunc_t launch_callbacks[] = {
    prrte_plm_base_setup_job,
    init_complete,
    prrte_ras_base_allocate,
    prrte_plm_base_allocation_complete,
    prrte_plm_base_daemons_launched,
    prrte_plm_base_daemons_reported,
    vm_ready,
    prrte_rmaps_base_map_job,
    prrte_plm_base_mapping_complete,
    prrte_plm_base_complete_setup,
    prrte_plm_base_launch_apps,
    prrte_plm_base_send_launch_msg,
    prrte_state_base_local_launch_complete,
    prrte_plm_base_post_launch,
    prrte_plm_base_registered,
    check_complete,
    dvm_notify,
    cleanup_job,
    prrte_quit
};

static prrte_proc_state_t proc_states[] = {
    PRRTE_PROC_STATE_RUNNING,
    PRRTE_PROC_STATE_REGISTERED,
    PRRTE_PROC_STATE_IOF_COMPLETE,
    PRRTE_PROC_STATE_WAITPID_FIRED,
    PRRTE_PROC_STATE_TERMINATED
};
static prrte_state_cbfunc_t proc_callbacks[] = {
    prrte_state_base_track_procs,
    prrte_state_base_track_procs,
    prrte_state_base_track_procs,
    prrte_state_base_track_procs,
    prrte_state_base_track_procs
};

static void force_quit(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    /* give us a chance to stop the orteds */
    prrte_plm.terminate_orteds();
    PRRTE_RELEASE(caddy);
}

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int i, rc;
    int num_states;

    /* setup the state machines */
    PRRTE_CONSTRUCT(&prrte_job_states, prrte_list_t);
    PRRTE_CONSTRUCT(&prrte_proc_states, prrte_list_t);

    /* setup the job state machine */
    num_states = sizeof(launch_states) / sizeof(prrte_job_state_t);
    for (i=0; i < num_states; i++) {
        if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(launch_states[i],
                                                           launch_callbacks[i],
                                                           PRRTE_SYS_PRI))) {
            PRRTE_ERROR_LOG(rc);
        }
    }
    /* add the termination response */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_DAEMONS_TERMINATED,
                                                       prrte_quit, PRRTE_SYS_PRI))) {
        PRRTE_ERROR_LOG(rc);
    }
    /* add a default error response */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_FORCED_EXIT,
                                                       force_quit, PRRTE_ERROR_PRI))) {
        PRRTE_ERROR_LOG(rc);
    }
    /* add callback to report progress, if requested */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_REPORT_PROGRESS,
                                                       prrte_state_base_report_progress, PRRTE_ERROR_PRI))) {
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

    /* cleanup the proc state machine */
    while (NULL != (item = prrte_list_remove_first(&prrte_proc_states))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_proc_states);

    return PRRTE_SUCCESS;
}

static void files_ready(int status, void *cbdata)
{
    prrte_job_t *jdata = (prrte_job_t*)cbdata;

    if (PRRTE_SUCCESS != status) {
        PRRTE_FORCED_TERMINATE(status);
        return;
    } else {
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP);
    }
}

static void init_complete(int sd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* nothing to do here but move along - if it is the
     * daemon job, then next step is allocate */
    PRRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRRTE_JOB_STATE_ALLOCATE);
    PRRTE_RELEASE(caddy);
}

static void vm_ready(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    int rc;
    prrte_buffer_t *buf;
    prrte_daemon_cmd_flag_t command = PRRTE_DAEMON_PASS_NODE_INFO_CMD;
    prrte_grpcomm_signature_t *sig;
    prrte_buffer_t *wireup;
    prrte_job_t *jptr;
    prrte_proc_t *dmn;
    prrte_byte_object_t bo, *boptr;
    int32_t numbytes, v;
    pmix_value_t *val;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t pproc;
    pmix_data_buffer_t pbuf;
    pmix_status_t ret;
    pmix_byte_object_t pbo;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* if this is my job, then we are done */
    if (PRRTE_PROC_MY_NAME->jobid == caddy->jdata->jobid) {
        /* if there is only one daemon in the job, then there
         * is just a little bit to do */
        if (1 < prrte_process_info.num_procs) {
            /* send the daemon map to every daemon in this DVM - we
             * do this here so we don't have to do it for every
             * job we are going to launch */
            buf = PRRTE_NEW(prrte_buffer_t);
            prrte_dss.pack(buf, &command, 1, PRRTE_DAEMON_CMD);
            if (PRRTE_SUCCESS != (rc = prrte_util_nidmap_create(prrte_node_pool, buf))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buf);
                return;
            }
            /* provide the info on the capabilities of each node */
            if (PRRTE_SUCCESS != (rc = prrte_util_pass_node_info(buf))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buf);
                return;
            }
            /* get wireup info for daemons */
            jptr = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
            wireup = PRRTE_NEW(prrte_buffer_t);
            PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, PRRTE_PROC_MY_NAME->jobid);
            for (v=0; v < jptr->procs->size; v++) {
                if (NULL == (dmn = (prrte_proc_t*)prrte_pointer_array_get_item(jptr->procs, v))) {
                    continue;
                }
                val = NULL;
                PRRTE_PMIX_CONVERT_VPID(pproc.rank, dmn->name.vpid);
                if (PMIX_SUCCESS != (ret = PMIx_Get(&pproc, NULL, NULL, 0, &val)) || NULL == val) {
                    PMIX_ERROR_LOG(ret);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                /* the data is returned as a pmix_data_array_t */
                if (PMIX_DATA_ARRAY != val->type || NULL == val->data.darray ||
                    PMIX_INFO != val->data.darray->type || NULL == val->data.darray->array) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                /* use the PMIx data support to pack it */
                info = (pmix_info_t*)val->data.darray->array;
                ninfo = val->data.darray->size;
                PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pproc, &pbuf, &ninfo, 1, PMIX_SIZE))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pproc, &pbuf, info, ninfo, PMIX_INFO))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(wireup, &dmn->name, 1, PRRTE_NAME))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                boptr = &bo;
                bo.bytes = (uint8_t*)pbo.bytes;
                bo.size = pbo.size;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(wireup, &boptr, 1, PRRTE_BYTE_OBJECT))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(wireup);
                    return;
                }
                PMIX_VALUE_RELEASE(val);
            }
            /* put it in a byte object for xmission */
            prrte_dss.unload(wireup, (void**)&bo.bytes, &numbytes);
            /* pack the byte object - zero-byte objects are fine */
            bo.size = numbytes;
            boptr = &bo;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(wireup);
                PRRTE_RELEASE(buf);
                return;
            }
            /* release the data since it has now been copied into our buffer */
            if (NULL != bo.bytes) {
                free(bo.bytes);
            }
            PRRTE_RELEASE(wireup);

            /* goes to all daemons */
            sig = PRRTE_NEW(prrte_grpcomm_signature_t);
            sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
            sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
            sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
            if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, buf))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buf);
                PRRTE_RELEASE(sig);
                PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                return;
            }
            PRRTE_RELEASE(buf);
        }
        /* notify that the vm is ready */
        if (0 > prrte_state_base_parent_fd) {
            fprintf(stdout, "DVM ready\n"); fflush(stdout);
        } else {
            char ok = 'K';
            write(prrte_state_base_parent_fd, &ok, 1);
            close(prrte_state_base_parent_fd);
            prrte_state_base_parent_fd = -1;
        }

        /* progress the job */
        caddy->jdata->state = PRRTE_JOB_STATE_VM_READY;
        PRRTE_RELEASE(caddy);
        return;
    }

    /* position any required files */
    if (PRRTE_SUCCESS != prrte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_POST_OBJECT(lk);
    lk->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lk);
}
static void check_complete(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata, *jptr;
    prrte_proc_t *proc;
    int i, rc;
    prrte_node_t *node;
    prrte_job_map_t *map;
    prrte_std_cntr_t index;
    pmix_proc_t pname;
    prrte_pmix_lock_t lock;
    uint8_t command = PRRTE_PMIX_PURGE_PROC_CMD;
    prrte_buffer_t *buf;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    prrte_byte_object_t bo, *boptr;
    pmix_status_t ret;
    prrte_pointer_array_t procs;
    char *tmp;

    PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    prrte_output_verbose(2, prrte_state_base_framework.framework_output,
                        "%s state:dvm:check_job_complete on job %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == jdata) ? "NULL" : PRRTE_JOBID_PRINT(jdata->jobid));

    if (NULL == jdata || jdata->jobid == PRRTE_PROC_MY_NAME->jobid) {
        /* just check to see if the daemons are complete */
        PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                             "%s state:dvm:check_job_complete - received NULL job, checking daemons",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        if (0 == prrte_routed.num_routes()) {
            /* orteds are done! */
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s orteds complete - exiting",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            if (NULL == jdata) {
                jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
            }
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            PRRTE_RELEASE(caddy);
            return;
        }
        PRRTE_RELEASE(caddy);
        return;
    }

    /* mark the job as terminated, but don't override any
     * abnormal termination flags
     */
    if (jdata->state < PRRTE_JOB_STATE_UNTERMINATED) {
        jdata->state = PRRTE_JOB_STATE_TERMINATED;
    }

    /* cleanup any pending server ops */
    PRRTE_PMIX_CONVERT_JOBID(pname.nspace, jdata->jobid);
    pname.rank = PMIX_RANK_WILDCARD;
    prrte_pmix_server_clear(&pname);

    /* cleanup the procs as these are gone */
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (proc->name.jobid == jdata->jobid) {
            /* clear the entry in the local children */
            prrte_pointer_array_set_item(prrte_local_children, i, NULL);
            PRRTE_RELEASE(proc);  // maintain accounting
        }
    }

    /* tell the IOF that the job is complete */
    if (NULL != prrte_iof.complete) {
        prrte_iof.complete(jdata);
    }

    /* tell the PMIx subsystem the job is complete */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_server_deregister_nspace(pname.nspace, opcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);

    /* tell the data server to purge any data from this nspace */
    buf = PRRTE_NEW(prrte_buffer_t);
    /* room number is ignored, but has to be included for pack sequencing */
    i=0;
    prrte_dss.pack(buf, &i, 1, PRRTE_INT);
    prrte_dss.pack(buf, &command, 1, PRRTE_UINT8);
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    /* pack the nspace to be purged */
    pname.rank = PMIX_RANK_WILDCARD;
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &pname, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        goto release;
    }
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;
    /* pack it into our command */
    boptr = &bo;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        free(bo.bytes);
        PRRTE_RELEASE(buf);
        goto release;
    }
    free(bo.bytes);
    /* send it to the data server */
    rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_NAME, buf,
                                 PRRTE_RML_TAG_DATA_SERVER,
                                 prrte_rml_send_callback, NULL);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
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
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, index))) {
                continue;
            }
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s state:dvm releasing procs from node %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 node->name));
            for (i = 0; i < node->procs->size; i++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                    continue;
                }
                if (proc->name.jobid != jdata->jobid) {
                    /* skip procs from another job */
                    continue;
                }
                if (!PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_TOOL)) {
                    node->slots_inuse--;
                    node->num_procs--;
                }

                PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                     "%s state:dvm releasing proc %s from node %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proc->name), node->name));
                /* set the entry in the node array to NULL */
                prrte_pointer_array_set_item(node->procs, i, NULL);
                /* release the proc once for the map entry */
                PRRTE_RELEASE(proc);
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

    /* if this job was a launcher, then we need to abort all of its
     * child jobs that might still be running */
    if (0 < prrte_list_get_size(&jdata->children)) {
        PRRTE_CONSTRUCT(&procs, prrte_pointer_array_t);
        prrte_pointer_array_init(&procs, 1, INT_MAX, 1);
        PRRTE_LIST_FOREACH(jptr, &jdata->children, prrte_job_t) {
            proc = PRRTE_NEW(prrte_proc_t);
            proc->name.jobid = jptr->jobid;
            proc->name.vpid = PRRTE_VPID_WILDCARD;
            prrte_pointer_array_add(&procs, proc);

        }
        prrte_plm.terminate_procs(&procs);
        for (i=0; i < procs.size; i++) {
            if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(&procs, i))) {
                PRRTE_RELEASE(proc);
            }
        }
        PRRTE_DESTRUCT(&procs);
    }

    /* remove the session directory tree */
    if (0 > prrte_asprintf(&tmp, "%s/%d", prrte_process_info.jobfam_session_dir, PRRTE_LOCAL_JOBID(jdata->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
    } else {
        prrte_os_dirpath_destroy(tmp, true, NULL);
        free(tmp);
    }

    if (jdata->state != PRRTE_JOB_STATE_NOTIFIED) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                             "%s state:dvm:check_job_completed state is terminated - activating notify",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_NOTIFY_COMPLETED);
        /* mark the job as notified */
        jdata->state = PRRTE_JOB_STATE_NOTIFIED;
    }

    PRRTE_RELEASE(caddy);
}

static void cleanup_job(int sd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;

    PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    /* remove this object from the job array */
    prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, NULL);

    PRRTE_RELEASE(caddy);
}

static void dvm_notify(int sd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata = caddy->jdata;
    prrte_proc_t *pptr=NULL;
    int rc;
    prrte_buffer_t *reply;
    prrte_daemon_cmd_flag_t command;
    prrte_grpcomm_signature_t *sig;
    bool notify = true, flag;
    prrte_process_name_t *proc, pnotify;
    prrte_byte_object_t bo, *boptr;
    pmix_byte_object_t pbo;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t pname, psource;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    char *errmsg = NULL;

    /* see if there was any problem */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_ABORTED_PROC, (void**)&pptr, PRRTE_PTR) && NULL != pptr) {
        rc = jdata->exit_code;
    /* or whether we got cancelled by the user */
    } else if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CANCELLED, NULL, PRRTE_BOOL)) {
        rc = PRRTE_ERR_JOB_CANCELLED;
    } else {
        rc = jdata->exit_code;
    }
    ret = prrte_pmix_convert_rc(rc);

    if (0 == ret && prrte_get_attribute(&jdata->attributes, PRRTE_JOB_SILENT_TERMINATION, NULL, PRRTE_BOOL)) {
        notify = false;
    }
    /* if the jobid matches that of the requestor, then don't notify */
    proc = &pnotify;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_LAUNCH_PROXY, (void**)&proc, PRRTE_NAME)) {
        if (pnotify.jobid == jdata->jobid) {
            notify = false;
        }
    }

    if (notify) {
    #ifdef PMIX_EVENT_TEXT_MESSAGE
        /* if it was an abnormal termination, then construct an appropriate
         * error message */
        if (PRRTE_SUCCESS != rc) {
            errmsg = prrte_dump_aborted_procs(jdata);
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
        PMIX_INFO_LOAD(&info[1], PMIX_JOB_TERM_STATUS, &ret, PMIX_STATUS);
        /* tell the requestor which job or proc  */
        PRRTE_PMIX_CONVERT_JOBID(pname.nspace, jdata->jobid);
        if (NULL != pptr) {
            PRRTE_PMIX_CONVERT_VPID(pname.rank, pptr->name.vpid);
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
        PRRTE_PMIX_CONVERT_NAME(&pname, PRRTE_PROC_MY_NAME);

        /* pack the status code */
        code = PMIX_ERR_JOB_TERMINATED;
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pname, &pbkt, &code, 1, PMIX_STATUS))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
        /* pack the source - it cannot be me as that will cause
         * the pmix server to upcall the event back to me */
        pnotify.jobid = jdata->jobid;
        pnotify.vpid = 0;
        PRRTE_PMIX_CONVERT_NAME(&psource, &pnotify);
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pname, &pbkt, &psource, 1, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
        /* pack the range */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pname, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
        /* pack the number of infos */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pname, &pbkt, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
        /* pack the infos themselves */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&pname, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
        PMIX_INFO_FREE(info, ninfo);

        /* unload the data buffer */
        PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;

        /* insert into prrte_buffer_t */
        reply = PRRTE_NEW(prrte_buffer_t);
        boptr = &bo;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &boptr, 1, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(reply);
            free(bo.bytes);
            return;
        }
        free(bo.bytes);

        /* we have to send the notification to all daemons so that
         * anyone watching for it can receive it */
        sig = PRRTE_NEW(prrte_grpcomm_signature_t);
        if (NULL == sig) {
            PRRTE_RELEASE(reply);
            return;
        }
        sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
        if (NULL == sig->signature) {
            PRRTE_RELEASE(reply);
            PRRTE_RELEASE(sig);
            return;
        }
        sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
        sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
        sig->sz = 1;
        if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_NOTIFICATION, reply))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(reply);
            PRRTE_RELEASE(sig);
            return;
        }
        PRRTE_RELEASE(reply);
        /* maintain accounting */
        PRRTE_RELEASE(sig);
    }

    /* now ensure that _all_ daemons know that this job has terminated so even
     * those that did not participate in it will know to cleanup the resources
     * they assigned to the job. This is necessary now that the mapping function
     * has been moved to the backend daemons - otherwise, non-participating daemons
     * retain the slot assignments on the participating daemons, and then incorrectly
     * map subsequent jobs thinking those nodes are still "busy" */
    reply = PRRTE_NEW(prrte_buffer_t);
    command = PRRTE_DAEMON_DVM_CLEANUP_JOB_CMD;
    prrte_dss.pack(reply, &command, 1, PRRTE_DAEMON_CMD);
    prrte_dss.pack(reply, &jdata->jobid, 1, PRRTE_JOBID);
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, reply);
    PRRTE_RELEASE(reply);
    PRRTE_RELEASE(sig);
    PRRTE_RELEASE(caddy);
}
