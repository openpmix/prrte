/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_fd.h"
#include "src/util/nidmap.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/filem/filem.h"
#include "src/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/rml/rml.h"
#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"

#include "src/mca/state/base/base.h"
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
    .init = init,
    .finalize = finalize,
    .activate_job_state = prte_state_base_activate_job_state,
    .add_job_state = prte_state_base_add_job_state,
    .set_job_state_callback = prte_state_base_set_job_state_callback,
    .remove_job_state = prte_state_base_remove_job_state,
    .activate_proc_state = prte_state_base_activate_proc_state,
    .add_proc_state = prte_state_base_add_proc_state,
    .set_proc_state_callback = prte_state_base_set_proc_state_callback,
    .remove_proc_state = prte_state_base_remove_proc_state
};

static void dvm_notify(int sd, short args, void *cbdata);
static void dvm_dereg_complete(pmix_status_t status, void *cbdata);
static void check_complete_resume(int fd, short args, void *cbdata);

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
    PRTE_PROC_STATE_READY_FOR_DEBUG,
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
    prte_state_base_track_procs,
    prte_state_base_track_procs
};

static void force_quit(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;

    /* give us a chance to stop the orteds */
    prte_plm.terminate_orteds();
    PMIX_RELEASE(caddy);
}

/************************
 * Local variables
 ************************/
static bool terminate_dvm = false;
static bool dvm_terminated = false;


/************************
 * API Definitions
 ************************/
static int init(void)
{
    int i, rc;
    int num_states;

    /* setup the state machines */
    PMIX_CONSTRUCT(&prte_job_states, pmix_list_t);
    PMIX_CONSTRUCT(&prte_proc_states, pmix_list_t);

    /* setup the job state machine */
    num_states = sizeof(launch_states) / sizeof(prte_job_state_t);
    for (i = 0; i < num_states; i++) {
        if (PRTE_SUCCESS
            != (rc = prte_state.add_job_state(launch_states[i], launch_callbacks[i]))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    /* add the termination response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_DAEMONS_TERMINATED, prte_quit);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add a default error response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_FORCED_EXIT, force_quit);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add callback to report progress, if requested */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_REPORT_PROGRESS,
                                  prte_state_base_report_progress);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    if (5 < pmix_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_job_state_machine();
    }

    /* populate the proc state machine to allow us to
     * track proc lifecycle changes
     */
    num_states = sizeof(proc_states) / sizeof(prte_proc_state_t);
    for (i = 0; i < num_states; i++) {
        rc = prte_state.add_proc_state(proc_states[i], proc_callbacks[i]);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
        }
    }
    if (5 < pmix_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_proc_state_machine();
    }

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    /* cleanup the state machines */
    PMIX_LIST_DESTRUCT(&prte_proc_states);
    PMIX_LIST_DESTRUCT(&prte_job_states);

    return PRTE_SUCCESS;
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

static void init_complete(int sd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(sd, args);
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(caddy);

    /* need to go thru allocate step in case someone wants to
     * expand the DVM */
    PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_ALLOCATE);
    PMIX_RELEASE(caddy);
}

/* Whether any DVM-ready broadcast has gone out yet.  The first one describes
 * a DVM starting up; every later one follows a grow, and the daemons reading
 * it for the first time are the ones the grow added. */
static bool first_vm_ready = true;

static void vm_ready(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    int rc;
    pmix_data_buffer_t buf;
    prte_job_t *jptr;
    prte_proc_t *dmn;
    int32_t v;
    uint32_t epoch;
    bool grown;
    pmix_value_t *val, *sval;
    pmix_status_t ret;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    /* if this is my job, then we are done */
    if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_LAUNCHED_DAEMONS, NULL, PMIX_BOOL)) {
        /* if there is more than one daemon in the job, then there
         * is just a little bit to do */
        if (!prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)
            && 1 < prte_process_info.num_daemons) {
            /* send the daemon map to every daemon in this DVM - we
             * do this here so we don't have to do it for every
             * job we are going to launch */
            PMIX_DATA_BUFFER_CONSTRUCT(&buf);
            rc = prte_util_nidmap_create(prte_node_pool, &buf);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
                /* the whole DVM is being torn down; held jobs will be
                 * failed as part of that teardown, so leave the fence
                 * untouched here */
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_RELEASE(caddy);
                return;
            }
            /* Tell the daemons what jobs are already running in this DVM.
             * A daemon that just joined has never heard of them, and needs
             * to resolve their procs' namespaces the moment one of them
             * talks to a proc of the job about to launch.  This used to ride
             * in that job's launch message, which made every such launch
             * message grow with the number of jobs resident in the DVM; it
             * belongs here, where the DVM's membership is what is being
             * described.  The job being launched is excluded - it is not
             * running yet, and it travels in its own launch message. */
            rc = prte_util_pack_job_catchup(&buf, caddy->jdata);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_RELEASE(caddy);
                return;
            }

            /* ...and the collective recovery epoch this DVM has reached.  A
             * daemon that just joined starts at zero, and every fence or group
             * contribution it makes is then dropped as stale by daemons that
             * have recovered from a failure - or from an elastic shrink, which
             * departs its daemon through the same machinery.  It cannot learn
             * the epoch from the failure notices that moved it: those were
             * broadcast while this daemon either did not exist or had not yet
             * reported in, and a broadcast to a daemon with no contact info is
             * dropped by design (prte_oob_base_send_nb).  This message is
             * built only once every expected daemon HAS reported, so it is the
             * first thing that can carry the value to one of them.  Daemons
             * already at this epoch take it as a no-op.
             *
             * The epoch we have applied, rather than the last one issued: a
             * notice still in flight will also reach the new daemon, which is
             * routable by now, and the epoch is adopted by highest value seen
             * so the two orders agree. */
            /* ...and whether the daemons reading this for the FIRST time
             * joined a DVM that was already running collectives.
             *
             * A fence's round number is per-signature and is bootstrapped
             * locally: a daemon with no record for a signature takes it as
             * round 0.  That is right for a daemon present from the start and
             * wrong for one a grow added, which would stamp 0 while every
             * other participant is at some k and have its contribution
             * dropped as ancient - hanging the fence.
             *
             * What travels is a flag rather than a count, and deliberately: a
             * count would be stale on arrival, because this master goes on
             * answering fences over other signatures while the grow completes,
             * and there is no moment at which a number handed over here is
             * still true.  The flag is always true when it is true.  It says
             * only "you do not know your round numbers, so ask rather than
             * assume", and a joiner stops needing it the moment it sees its
             * first release for a signature.
             *
             * Broadcast to everyone, like the epoch, and harmless there: the
             * receiver keeps the first answer it is given, so a daemon that
             * has been through a wireup already is unaffected by this one. */
            grown = !first_vm_ready;
            first_vm_ready = false;
            rc = PMIx_Data_pack(NULL, &buf, &grown, 1, PMIX_BOOL);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_RELEASE(caddy);
                return;
            }

            epoch = prte_grpcomm_current_epoch();
            rc = PMIx_Data_pack(NULL, &buf, &epoch, 1, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_RELEASE(caddy);
                return;
            }

            /* get wireup info for daemons */
            jptr = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            for (v = 0; v < jptr->procs->size; v++) {
                if (NULL == (dmn = (prte_proc_t *) pmix_pointer_array_get_item(jptr->procs, v))) {
                    continue;
                }
                val = NULL;
                if (PMIX_SUCCESS != (ret = PMIx_Get(&dmn->name, PMIX_PROC_URI, NULL, 0, &val)) ||
                    NULL == val) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&buf);
                    /* the whole DVM is being torn down; held jobs will be
                     * failed as part of that teardown, so leave the fence
                     * untouched here */
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_RELEASE(caddy);
                    return;
                }
                rc = PMIx_Data_pack(NULL, &buf, &dmn->name, 1, PMIX_PROC);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_DESTRUCT(&buf);
                    /* the whole DVM is being torn down; held jobs will be
                     * failed as part of that teardown, so leave the fence
                     * untouched here */
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_RELEASE(caddy);
                    return;
                }
                rc = PMIx_Data_pack(NULL, &buf, &val->data.string, 1, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_DESTRUCT(&buf);
                    PMIX_VALUE_RELEASE(val);
                    /* the whole DVM is being torn down; held jobs will be
                     * failed as part of that teardown, so leave the fence
                     * untouched here */
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_RELEASE(caddy);
                    return;
                }
                PMIX_VALUE_RELEASE(val);

                /* ...and that node's PMIx SERVER rendezvous URI. This is not
                 * wireup in the RML sense - no daemon ever opens a PMIx
                 * connection to another daemon, and the URI just above is
                 * what they route with. It rides along here so that every
                 * daemon can answer a TOOL asking "where is the PMIx server
                 * on node X?" (a hostname/nodeid-qualified PMIX_SERVER_URI
                 * query - see src/pmix/AGENTS.md). Collecting it only at the
                 * master would mean the answer depended on which daemon the
                 * tool happened to be connected to.
                 *
                 * Each daemon reported this in its PRTED_CALLBACK rollup and
                 * we stored it against its name, so this is a local lookup.
                 * A daemon that could not report one leaves nothing to find:
                 * pack a NULL rather than failing the DVM over auxiliary
                 * information. */
                sval = NULL;
                ret = PMIx_Get(&dmn->name, PMIX_SERVER_URI, NULL, 0, &sval);
                if (PMIX_SUCCESS == ret && NULL != sval && PMIX_STRING == sval->type) {
                    rc = PMIx_Data_pack(NULL, &buf, &sval->data.string, 1, PMIX_STRING);
                } else {
                    char *nulluri = NULL;
                    rc = PMIx_Data_pack(NULL, &buf, &nulluri, 1, PMIX_STRING);
                }
                if (NULL != sval) {
                    PMIX_VALUE_RELEASE(sval);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_DESTRUCT(&buf);
                    /* the whole DVM is being torn down; held jobs will be
                     * failed as part of that teardown, so leave the fence
                     * untouched here */
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_RELEASE(caddy);
                    return;
                }
            }

            /* goes to all daemons */
            if (PRTE_SUCCESS != (rc = prte_grpcomm_xcast(PRTE_RML_TAG_WIREUP, &buf))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
                /* the whole DVM is being torn down; held jobs will be
                 * failed as part of that teardown, so leave the fence
                 * untouched here */
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_RELEASE(caddy);
                return;
            }
            PMIX_DATA_BUFFER_DESTRUCT(&buf);
        }
        /* success path (and DO_NOT_LAUNCH / single-daemon path): the new
         * daemons (if any) are now wired up.  This callback only fires once
         * every expected daemon has reported (num_reported == num_procs), so
         * any in-progress grow campaigns have fully succeeded.  Drain them,
         * dropping their fence contribution, and release any held jobs.
         * Doing the release here — after the WIREUP xcast above — guarantees
         * held jobs are only admitted once the new daemons are wired up.
         * Nothing to drain outside elastic mode (no campaign was ever made). */
        if (prte_elastic_mode) {
            prte_plm_base_grow_drain(true);
        }
    }
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, caddy->jdata->nspace)) {
        bool first_ready = !prte_dvm_started;

        prte_dvm_ready = true;
        /* and latch that we have started at least once - unlike the flag
         * above, this one is never cleared.  prte_dvm_ready goes false again
         * on every grow, session instantiate and teardown, so it says "is a
         * size change in flight", not "have we started" */
        prte_dvm_started = true;
        /* notify that the vm is ready - once.  This state is re-entered at
         * the end of every grow, and a persistent DVM announcing itself ready
         * again each time a node is added is noise on a terminal the user is
         * no longer watching for startup */
        if (0 > prte_state_base.parent_fd) {
            if (first_ready && prte_state_base.ready_msg && prte_persistent) {
                fprintf(stdout, "DVM ready\n");
                fflush(stdout);
            }
        } else {
            char ok = 'K';
            pmix_fd_write(prte_state_base.parent_fd, 1, &ok);
            close(prte_state_base.parent_fd);
            prte_state_base.parent_fd = -1;
        }
        prte_plm_base_release_cached_jobs();
        /* progress the job */
        caddy->jdata->state = PRTE_JOB_STATE_VM_READY;
        PMIX_RELEASE(caddy);
        return;
    }

    /* if a daemon launch campaign is active, park this app job (only possible
     * in elastic mode, where the fence is raised; the explicit guard keeps the
     * non-elastic path identical even if the fence were ever left nonzero) */
    if (prte_elastic_mode && 0 < prte_dvm_launch_fence) {
        caddy->jdata->state = PRTE_JOB_STATE_WAITING_FOR_DAEMONS;
        PMIX_RETAIN(caddy->jdata);
        pmix_pointer_array_add(prte_held_jobs, caddy->jdata);
        PMIX_RELEASE(caddy);
        return;
    }

    /* position any required files */
    if (PRTE_SUCCESS != prte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
    }
    PMIX_RELEASE(caddy);
}

static void job_started(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata = caddy->jdata;
    pmix_info_t *iptr;
    time_t timestamp;
    pmix_proc_t *nptr;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* if there is an originator for this job, notify them
     * that the first process of the job has been started */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, NULL, PMIX_BOOL)) {
        /* dvm job => launch was requested by a TOOL, so we notify the launch proxy
         * and NOT the originator (as that would be us) */
        nptr = NULL;
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC)
            || NULL == nptr) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PMIX_RELEASE(caddy);
            return;
        }
        timestamp = time(NULL);
        PMIX_INFO_CREATE(iptr, 5);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, nptr, PMIX_PROC);
        PMIX_PROC_RELEASE(nptr);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[1], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
        /* not to be delivered to a default event handler */
        PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        /* provide the timestamp */
        PMIX_INFO_LOAD(&iptr[3], PMIX_EVENT_TIMESTAMP, &timestamp, PMIX_TIME);
        PMIX_INFO_LOAD(&iptr[4], "prte.notify.donotloop", NULL, PMIX_BOOL);
        PMIx_Notify_event(PMIX_EVENT_JOB_START, &prte_process_info.myproc, PMIX_RANGE_CUSTOM, iptr,
                          5, NULL, NULL);
        PMIX_INFO_FREE(iptr, 5);
    }

    PMIX_RELEASE(caddy);
}

static void ready_for_debug(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata = caddy->jdata;
    pmix_proc_t *nptr;
    time_t timestamp;
    pmix_info_t *iptr;
    size_t ninfo;
    pmix_data_array_t darray;
    void *tinfo;
    pmix_status_t rc;
    int n;
    char *name, *bkpt;
    prte_app_context_t *app;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* launch was requested by a TOOL, so we notify the launch proxy
     * and NOT the originator (as that would be us) */
    nptr = NULL;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC)
        || NULL == nptr) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto DONE;
    }
    timestamp = time(NULL);
    PMIX_INFO_LIST_START(tinfo);
    /* target this notification solely to that one tool */
    PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_EVENT_CUSTOM_RANGE, nptr, PMIX_PROC);
    PMIX_PROC_RELEASE(nptr);
    /* pass the nspace of the job */
    PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_NSPACE, jdata->nspace, PMIX_STRING);
    /* a READY_FOR_DEBUG event is supposed to say WHERE the processes are
     * waiting.  If the user named the breakpoint, that is the answer - the
     * procs cannot have reported ready anywhere else.  We have nothing to
     * say when they did not: a process that stops at a place of its own
     * choosing reports the name to its local daemon, and that name does not
     * travel with the daemon's aggregated report to us. */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT,
                           (void **) &bkpt, PMIX_STRING)) {
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_BREAKPOINT, bkpt, PMIX_STRING);
        free(bkpt);
    }
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
    PMIX_INFO_LIST_ADD(rc, tinfo, "prte.notify.donotloop", NULL, PMIX_BOOL);
    PMIX_INFO_LIST_CONVERT(rc, tinfo, &darray);
    if (PMIX_ERR_EMPTY == rc) {
        iptr = NULL;
        ninfo = 0;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_UPDATE_EXIT_STATUS(rc);
        PMIX_INFO_LIST_RELEASE(tinfo);
        /* nptr was released as soon as it was added to the list above -
         * releasing it again here freed it twice */
        goto DONE;
    } else {
        iptr = (pmix_info_t *) darray.array;
        ninfo = darray.size;
    }
    PMIX_INFO_LIST_RELEASE(tinfo);

    PMIx_Notify_event(PMIX_READY_FOR_DEBUG, PRTE_PROC_MY_NAME, PMIX_RANGE_CUSTOM, iptr,
                      ninfo, NULL, NULL);
    PMIX_INFO_FREE(iptr, ninfo);

DONE:
    PMIX_RELEASE(caddy);
}

/* Is the exit status of child jobs reported separately from the primary
 * job's?  The policy belongs to the run rather than to any one job - it is
 * consulted as EACH job reaches teardown - so it is read from the
 * prte_report_child_jobs_separately MCA param or from the copy that the
 * "report-child-jobs-separately" runtime option leaves on the daemon job.
 * See prte_state_base_set_runtime_options() for why it is recorded there. */
static bool report_child_jobs_separately(void)
{
    prte_job_t *djob;

    if (prte_report_child_jobs_separately) {
        return true;
    }
    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == djob) {
        return false;
    }
    return prte_get_attribute(&djob->attributes, PRTE_JOB_REPORT_CHILD_SEP, NULL, PMIX_BOOL);
}

static void check_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_proc_t *proc;
    int i, rc;
    pmix_proc_t pname;
    prte_timer_t *timer;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    pmix_output_verbose(2, prte_state_base_framework.framework_output,
                        "%s state:dvm:check_job_complete on job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == jdata) ? "NULL" : PRTE_JOBID_PRINT(jdata->nspace));

    if (NULL != jdata &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT, (void **) &timer, PMIX_POINTER)) {
        /* timer is an prte_timer_t object */
        prte_event_evtimer_del(timer->ev);
        PMIX_RELEASE(timer);
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT_EVENT);
    }

    if (NULL == jdata || PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        /* just check to see if the daemons are complete */
        PMIX_OUTPUT_VERBOSE(
            (2, prte_state_base_framework.framework_output,
             "%s state:dvm:check_job_complete - received NULL job, checking daemons",
             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* safety net: if grow campaigns were still pending when the daemon
         * job reached this point, drain them so held jobs are not parked
         * indefinitely */
        if (!pmix_list_is_empty(&prte_grow_campaigns)) {
            prte_plm_base_grow_drain(false);
        }
        if (0 == prte_rml_base.n_children) {
            /* orteds are done! */
            PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s prteds complete - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            if (NULL == jdata) {
                jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            }
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            PMIX_RELEASE(caddy);
            prte_dvm_ready = false;
            return;
        }
        prte_plm.terminate_orteds();
        PMIX_RELEASE(caddy);
        return;
    }

    /* mark the job as terminated, but don't override any
     * abnormal termination flags
     */
    if (jdata->state < PRTE_JOB_STATE_UNTERMINATED) {
        jdata->state = PRTE_JOB_STATE_TERMINATED;
    }

    /* apply any reservation inheritance dispositions triggered by the
     * termination of this namespace */
    prte_ras_base_check_reservations_on_term(jdata);

    /* see if there was any problem */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, NULL, PMIX_POINTER)) {
        rc = prte_pmix_convert_rc(jdata->exit_code);
        /* or whether we got cancelled by the user */
    } else if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CANCELLED, NULL, PMIX_BOOL)) {
        rc = prte_pmix_convert_rc(PRTE_ERR_JOB_CANCELLED);
    } else {
        rc = prte_pmix_convert_rc(jdata->exit_code);
    }

    /* if would be rare, but a very fast terminating job could conceivably
     * reach here prior to the spawn requestor being notified of spawn */
    rc = prte_plm_base_spawn_response(rc, jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* cleanup any pending server ops */
    PMIX_LOAD_PROCID(&pname, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pmix_server_clear(&pname);

    /* cleanup the local procs as these are gone */
    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL == (proc = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        /* if this child is part of the job... */
        if (PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
            /* clear the entry in the local children */
            pmix_pointer_array_set_item(prte_local_children, i, NULL);
            PMIX_RELEASE(proc); // maintain accounting
        }
    }

    /* tell the IOF that the job is complete */
    if (NULL != prte_iof.complete) {
        prte_iof.complete(jdata);
    }

    /* Tell the PMIx subsystem the job is complete and resume the teardown
     * when it says so.  This must NOT block: prte_event_base has no progress
     * thread of its own (event.c: "PRTE tools block in their own loop over
     * the event base"), so waiting here parks the ONE thread driving it and
     * the HNP goes deaf - no RML, no IOF, no other job's state transitions -
     * for as long as PMIx takes to tear the nspace down.  That is unbounded:
     * the deregistration runs each peer's filesystem epilog.  On a persistent
     * DVM it stalls every other job in flight. */
    PMIx_server_deregister_nspace(pname.nspace, dvm_dereg_complete, caddy);
    /* the continuation owns the caddy now */
    return;
}

/* Completion of the nspace deregistration.  Runs on the PMIx progress
 * thread, so it does nothing but hand the caddy back to ours - everything
 * the continuation touches is a PRRTE object. */
static void dvm_dereg_complete(pmix_status_t status, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, check_complete_resume);
}

/* The remainder of check_complete, resumed once PMIx has finished
 * deregistering the nspace.  Runs on the PRRTE progress thread. */
static void check_complete_resume(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_session_t *session;
    prte_job_t *jdata, *jptr;
    prte_proc_t *proc;
    int i, rc, nprocs;
    prte_node_t *node;
    prte_job_map_t *map;
    int32_t index;
    pmix_proc_t pname;
    pmix_pointer_array_t procs;
    prte_app_context_t *app;
    hwloc_obj_t obj;
    hwloc_obj_type_t type;
    hwloc_cpuset_t boundcpus, tgt;
    bool takeall, sep, *sepptr = &sep;
    prte_pmix_server_pset_t *pst, *pst2;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;
    PMIX_LOAD_PROCID(&pname, jdata->nspace, PMIX_RANK_WILDCARD);


    if (!prte_persistent) {
        /* Update our exit status.
         *
         * With child jobs reported separately, only the PRIMARY job's status
         * is returned - a child's non-zero status is reported to the user but
         * does not become the DVM's exit code.  Otherwise every job feeds the
         * same status and PRTE_UPDATE_EXIT_STATUS keeps the first non-zero
         * one, which is the documented default.  The primary job is the one
         * the launcher created, local jobid 1; anything above that was
         * spawned by it. */
        if (report_child_jobs_separately() && 1 != PRTE_LOCAL_JOBID(jdata->nspace)) {
            if (0 != jdata->exit_code) {
                prte_show_help("help-state-base.txt", "child-job-status", true,
                               PRTE_LOCAL_JOBID_PRINT(jdata->nspace), jdata->exit_code);
            }
        } else {
            PRTE_UPDATE_EXIT_STATUS(jdata->exit_code);
        }
        /* if this is an abnormal termination, report it */
        if (jdata->state > PRTE_JOB_STATE_ERROR) {
            char *msg;
            msg = prte_dump_aborted_procs(jdata);
            if (NULL != msg) {
                pmix_byte_object_t bo;
                PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
                bo.bytes = (char *) msg;
                bo.size = strlen(msg);
                /* This one stays synchronous, but PMIx does the blocking:
                 * with a NULL callback PMIx_server_IOF_deliver waits on its
                 * OWN lock, so no PRRTE object is touched from the PMIx
                 * thread.  Waiting is required rather than merely tidy - the
                 * API borrows the source proc and the byte object BY POINTER
                 * and both live on this stack.  The cost is acceptable here
                 * where it is not for the deregistration above: this runs
                 * only for a non-persistent (prterun) DVM already tearing
                 * down after an abnormal exit, and the delivery is a bounded,
                 * purely PMIx-internal write with no host upcall. */
                rc = PMIx_server_IOF_deliver(&prte_process_info.myproc,
                                             PMIX_FWD_STDDIAG_CHANNEL,
                                             &bo, NULL, 0, NULL, NULL);
                if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
                    PMIX_ERROR_LOG(rc);
                }
                free(msg);
            }
        }
        /* if all of the jobs we are running are done, then shut us down */
        for (i = 0; i < prte_job_data->size; i++) {
            jptr = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
            if (NULL == jptr) {
                continue;
            }
            /* skip the daemon job */
            if (PMIX_CHECK_NSPACE(jptr->nspace, PRTE_PROC_MY_NAME->nspace)) {
                continue;
            }
            if (jptr->state < PRTE_JOB_STATE_TERMINATED) {
                /* still alive - finish processing this job's termination */
                goto release;
            }
        }

        /* Let the tools know that a job terminated before we shutdown */
        if (jdata->state != PRTE_JOB_STATE_NOTIFIED) {
            PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:dvm:check_job_completed state is terminated - activating notify",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            terminate_dvm = true;  // flag that the DVM is to terminate
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFY_COMPLETED);
            PMIX_RELEASE(caddy);
            return;
        }

        /* if we fell thru to this point, then nobody is still
         * alive except the daemons, so just shut us down */
        prte_plm.terminate_orteds();
        PMIX_RELEASE(caddy);
        return;
    }

    /* Tell the data server the namespace is over, so it can drop what this
     * job published that was not to outlive it.  This used to be an
     * open-coded copy of the same call, sent only when an external data
     * server was configured; the built-in one - the usual case - was
     * therefore never told a job had ended, and nothing was ever reclaimed
     * from it short of the DVM shutting down. */
    prte_state_base_purge_nspace(jdata->nspace);

release:
    /* Release the resources used by this job. Since some errmgrs may want
     * to continue using resources allocated to the job as part of their
     * fault recovery procedure, we only do this once the job is "complete".
     * Note that an aborted/killed job -is- flagged as complete and will
     * therefore have its resources released. We need to do this after
     * we call the errmgr so that any attempt to restart the job will
     * avoid doing so in the exact same place as the current job
     */
    session = jdata->session;
    if(NULL != session){
        for(i = 0; i < session->jobs->size; i++){
            if(NULL != (jptr = pmix_pointer_array_get_item(session->jobs, i))){
                if(PMIX_CHECK_NSPACE(jdata->nspace, jptr->nspace)){
                    pmix_pointer_array_set_item(session->jobs, i, NULL);
                    break;
                }
            }
        }
        /* Tell the session-control layer the job is gone. It records the
         * termination status for the completion report the scheduler is owed,
         * and reclaims a session that exists only to run the jobs it was
         * instantiated with. Must follow the removal above, since that is what
         * it tests to decide the session has drained. */
        prte_pmix_server_session_job_terminated(session, jdata);
    }
    if (NULL != jdata->map) {
        map = jdata->map;
        takeall = false;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
            type = HWLOC_OBJ_PU;
        } else {
            type = HWLOC_OBJ_CORE;
        }
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16) ||
            PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(map->mapping) ||
            PRTE_MAPPING_SEQ == PRTE_GET_MAPPING_POLICY(map->mapping)) {
            takeall = true;
        }
        boundcpus = hwloc_bitmap_alloc();
        for (index = 0; index < map->nodes->size; index++) {
            node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, index);
            if (NULL == node) {
                continue;
            }
            PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:dvm releasing procs from node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
            for (i = 0; i < node->procs->size; i++) {
                proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, i);
                if (NULL == proc) {
                    continue;
                }
                if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                    /* skip procs from another job */
                    continue;
                }
                app = (prte_app_context_t*) pmix_pointer_array_get_item(jdata->apps, proc->app_idx);
                if (!PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL) &&
                    !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                    node->slots_inuse--;
                    node->num_procs--;
                }
                /* release the resources held by the proc - only the first
                 * cpu in the proc's cpuset was used to mark usage.  The
                 * do/while(0) lets a failure to decode the cpuset abandon just
                 * the cpu restore: a "continue" here would also skip dropping
                 * the proc from the node below, leaking it and leaving a
                 * dangling entry behind in the node's proc array. */
                if (NULL != proc->cpuset) {
                    do {
                        if (0 != (rc = hwloc_bitmap_list_sscanf(boundcpus, proc->cpuset))) {
                            pmix_output(0, "hwloc_bitmap_sscanf returned %s for the string %s",
                                        prte_strerror(rc), proc->cpuset);
                            break;
                        }
                        if (takeall) {
                            tgt = boundcpus;
                        } else {
                            /* we only want to restore the first CPU of whatever region
                             * the proc was bound to, so we have to first narrow the
                             * bitmap down to only that region */
                            hwloc_bitmap_andnot(prte_rmaps_base.available, boundcpus,
                                                node->available);
                            /* the set bits in the result are the bound cpus that are still
                             * marked as in-use */
                            obj = hwloc_get_obj_inside_cpuset_by_type(node->topology->topo,
                                                                      prte_rmaps_base.available,
                                                                      type, 0);
                            if (NULL == obj) {
                                pmix_output(0, "COULD NOT GET BOUND CPU FOR RESOURCE RELEASE");
                                break;
                            }
                            tgt = obj->cpuset;
                        }
                        hwloc_bitmap_or(node->available, node->available, tgt);
                    } while (0);
                }

                PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                     "%s state:dvm releasing proc %s from node %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name), node->name));
                /* set the entry in the node array to NULL */
                pmix_pointer_array_set_item(node->procs, i, NULL);
                /* release the proc once for the map entry */
                PMIX_RELEASE(proc);
            }
            /* set the node location to NULL */
            pmix_pointer_array_set_item(map->nodes, index, NULL);
            /* flag that the node is no longer in a map.  This has to precede
             * the release: the map holds a reference, and dropping it may be
             * the last one */
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
            /* maintain accounting */
            PMIX_RELEASE(node);
        }
        hwloc_bitmap_free(boundcpus);
        PMIX_RELEASE(map);
        jdata->map = NULL;
    }
    // if this job has apps that named a pset, then remove them
    PMIX_LIST_FOREACH_SAFE(pst, pst2, &prte_pmix_server_globals.psets, prte_pmix_server_pset_t) {
        if (pst->jdata == jdata) {
            pmix_list_remove_item(&prte_pmix_server_globals.psets, &pst->super);
            PMIX_RELEASE(pst);
        }
    }

    /* if requested, check fd status for leaks */
    if (prte_state_base.run_fdcheck) {
        prte_state_base_check_fds(jdata);
    }

    /* if this job started child jobs, then we need to abort all of its
     * child jobs that might still be running unless designated to
     * run independently of their parent */
    if (0 < pmix_list_get_size(&jdata->children)) {
        PMIX_CONSTRUCT(&procs, pmix_pointer_array_t);
        pmix_pointer_array_init(&procs, 1, INT_MAX, 1);
        nprocs = 0;
        PMIX_LIST_FOREACH(jptr, &jdata->children, prte_job_t)
        {
            if (prte_get_attribute(&jptr->attributes, PRTE_JOB_CHILD_SEP, (void**)&sepptr, PMIX_BOOL) && !sep) {
                proc = PMIX_NEW(prte_proc_t);
                PMIX_LOAD_PROCID(&proc->name, jptr->nspace, PMIX_RANK_WILDCARD);
                pmix_pointer_array_add(&procs, proc);
                ++nprocs;
                if (1 == nprocs) {
                    // output a warning message that at least one child is being terminated
                    prte_show_help("help-state-base.txt", "child-term", true,
                                   jdata->nspace, jptr->nspace);
                }
            }
        }
        if (0 < nprocs) {
            prte_plm.terminate_procs(&procs);
            for (i = 0; i < procs.size; i++) {
                if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(&procs, i))) {
                    PMIX_RELEASE(proc);
                }
            }
        }
        PMIX_DESTRUCT(&procs);
    }

    if (jdata->state != PRTE_JOB_STATE_NOTIFIED) {
        PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:check_job_completed state is terminated - activating notify",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFY_COMPLETED);
        /* mark the job as notified */
        jdata->state = PRTE_JOB_STATE_NOTIFIED;
    }

    PMIX_POST_OBJECT(jdata);
    PMIX_RELEASE(caddy);
}

static void cleanup_job(int sd, short args, void *cbdata)
{
    pmix_proc_t *nptr;
    prte_job_t *parent;
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    if (terminate_dvm && !dvm_terminated) {
        dvm_terminated = true;
        prte_plm.terminate_orteds();
    }
    if (NULL != caddy->jdata) {
        /* if the job had a spawn parent remove it from the parents child list */
        if (prte_get_attribute(&caddy->jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC)) {
            if(NULL != (parent = prte_get_job_data_object(nptr->nspace)) &&
            !PMIX_CHECK_NSPACE(parent->nspace, PRTE_PROC_MY_NAME->nspace)){
                pmix_list_remove_item(&parent->children, &caddy->jdata->super);
                /* We retained the jdata before adding it to the list - maintain ref count */
                PMIX_RELEASE(caddy->jdata);
            }
            PMIX_PROC_RELEASE(nptr);
        }
        /* From here on, the answer to a question about this job is "not
         * found" rather than "wait" - and the difference is a hang.  A
         * lookup that fails cannot tell a job we have not heard of YET from
         * one that has been and gone, so it assumes the first and parks the
         * request; nothing drains that on a timer.  The daemons record their
         * own departures where they release their copy of the job
         * (PRTE_DAEMON_CONT_CLEANUP_JOB); this is the master's copy, and its
         * lifecycle is here. */
        prte_pmix_server_job_departed(caddy->jdata->nspace);
        PMIX_RELEASE(caddy->jdata);
    }
    PMIX_RELEASE(caddy);
}

#ifdef PMIX_SPAWN_TREE_ROOT
/* Do these two namespaces name the same thing?
 *
 * NOT PMIX_CHECK_NSPACE, which answers "true" the moment either side is
 * empty - wildcard semantics that are right for a match against a request
 * and wrong here.  Most jobs in a DVM carry an empty launcher, and reading
 * every one of them as a member of whatever tree we are asking about would
 * put a stranger's job in a tool's wait set. */
static bool same_nspace(const char *a, const char *b)
{
    if (PMIX_NSPACE_INVALID(a) || PMIX_NSPACE_INVALID(b)) {
        return false;
    }
    return (0 == strncmp(a, b, PMIX_MAX_NSLEN));
}

/* The root of the spawn tree JDATA belongs to.  prte_job_t::launcher already
 * holds it, recorded when the job was created and copied transitively from
 * the parent, so a grandchild names the same root as its parent does.  It is
 * empty only for a job nobody spawned - the primary job of a prterun, or of a
 * prun whose tool namespace never got a job object - and such a job is the
 * root of its own tree. */
static const char *spawn_tree_root(prte_job_t *jdata)
{
    if (PMIX_NSPACE_INVALID(jdata->launcher)) {
        return jdata->nspace;
    }
    return jdata->launcher;
}

/* How many jobs in ROOT's spawn tree have yet to terminate, not counting
 * JDATA, whose termination is being reported.
 *
 * A job is in the tree if it names ROOT as its launcher, or if it IS the root
 * - the latter matters when the root is a job rather than a tool, so that a
 * child ending while its parent is still alive does not report an empty tree.
 * Tool job objects are skipped: a tool is a namespace the DVM tracks, not a
 * job that ever reaches a terminal state, so counting one would leave the
 * tree permanently non-empty.
 *
 * "Not yet terminated" is the same test check_complete() applies when a
 * non-persistent DVM decides whether it can shut down, and deliberately so:
 * the whole point is that a tool watching a persistent DVM can now wait for
 * exactly what prterun waits for. */
static uint32_t spawn_tree_active(prte_job_t *jdata, const char *root)
{
    prte_job_t *jptr;
    uint32_t count = 0;
    int i;

    for (i = 0; i < prte_job_data->size; i++) {
        jptr = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
        if (NULL == jptr || jptr == jdata) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(jptr->nspace, PRTE_PROC_MY_NAME->nspace) ||
            PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_TOOL)) {
            continue;
        }
        if (!same_nspace(jptr->launcher, root) &&
            !same_nspace(jptr->nspace, root)) {
            continue;
        }
        if (jptr->state < PRTE_JOB_STATE_TERMINATED) {
            ++count;
        }
    }
    return count;
}
#endif

static void dvm_notify(int sd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata = caddy->jdata;
    prte_proc_t *pptr = NULL;
    int rc;
    int xcode;
    pmix_status_t jstatus;
    pmix_data_buffer_t *reply;
    prte_daemon_cmd_flag_t command;
    bool notify = true, flag;
    pmix_proc_t *proc, pnotify;
    pmix_info_t *info;
    size_t ninfo, n;
    pmix_proc_t pname;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    char *errmsg = NULL;
#ifdef PMIX_SPAWN_TREE_ROOT
    const char *treeroot;
    uint32_t treeactive;
#endif
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                         "%s state:dvm:dvm_notify called",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* See if there was any problem.  Two different things come out of this,
     * and they used to be conflated into one:
     *
     *   jstatus - a pmix_status_t saying WHY the job ended.  This is what
     *             goes on the wire as PMIX_JOB_TERM_STATUS, whose type is
     *             pmix_status_t, and which any PMIx tool is entitled to read
     *             as one.  What used to be put there was jdata->exit_code -
     *             an application exit status, or on some paths a PRRTE error
     *             constant - so a job whose rank exited 7 announced its
     *             termination status as "7", which is not a PMIx status at
     *             all.  prun then ran it through prte_pmix_convert_status(),
     *             which recognized nothing and answered PRTE_ERROR, and that
     *             is why every failed job made prun exit 71.
     *
     *   xcode   - the application's exit status, reported separately as
     *             PMIX_EXIT_CODE so a launcher can pass it on the way
     *             prterun does.  Only sent when it looks like one: on paths
     *             where no process ever ran, jdata->exit_code carries a
     *             (negative) PRRTE error constant instead, and handing that
     *             to a tool as an exit status would be the same category
     *             error one level down.
     */
    xcode = jdata->exit_code;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, (void **) &pptr, PMIX_POINTER)
        && NULL != pptr) {
        rc = jdata->exit_code;
        jstatus = prte_pmix_convert_job_state_to_error(jdata->state);
        /* or whether we got cancelled by the user */
    } else if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CANCELLED, NULL, PMIX_BOOL)) {
        rc = PRTE_ERR_JOB_CANCELLED;
        jstatus = PMIX_ERR_JOB_CANCELED;
    } else {
        rc = jdata->exit_code;
        jstatus = (0 == rc) ? PMIX_SUCCESS
                            : prte_pmix_convert_job_state_to_error(jdata->state);
    }
    /* a plausible process exit status, or nothing */
    if (0 >= xcode || 255 < xcode) {
        xcode = -1;
    }

    if (0 == rc &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_SILENT_TERMINATION, NULL, PMIX_BOOL)) {
        notify = false;
    }
    /* if the jobid matches that of the requestor, then don't notify */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &proc, PMIX_PROC)) {
        if (PMIX_CHECK_NSPACE(proc->nspace, jdata->nspace)) {
            notify = false;
        }
        PMIX_PROC_RELEASE(proc);
    }

    if (notify) {
        PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:dvm_notify notification requested",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        /* if it was an abnormal termination, then construct an appropriate
         * error message */
        if (PRTE_SUCCESS != rc) {
            errmsg = prte_dump_aborted_procs(jdata);
        }
        /* construct the info to be provided */
        ninfo = 3;
        if (NULL != errmsg) {
            ++ninfo;
        }
        if (0 < xcode) {
            ++ninfo;
        }
#ifdef PMIX_SPAWN_TREE_ROOT
        /* Which spawn tree this job belonged to, and what is left of it.
         * A tool that launched the root of the tree cannot work either out
         * for itself: it is told when a job ends, never when one starts, so
         * at the moment its own job ends it has no way to know whether
         * anything it started is still running - nor, when some later job
         * ends, whether that job descended from it or belongs to another
         * user of the same persistent DVM. */
        treeroot = spawn_tree_root(jdata);
        treeactive = spawn_tree_active(jdata, treeroot);
        ninfo += 2;
#endif
        PMIX_INFO_CREATE(info, ninfo);
        n = 0;
        /* ensure this only goes to the job terminated event handler */
        flag = true;
        PMIX_INFO_LOAD(&info[n++], PMIX_EVENT_NON_DEFAULT, &flag, PMIX_BOOL);
        /* provide the status */
        PMIX_INFO_LOAD(&info[n++], PMIX_JOB_TERM_STATUS, &jstatus, PMIX_STATUS);
        /* tell the requestor which job or proc  */
        PMIX_LOAD_NSPACE(pname.nspace, jdata->nspace);
        if (NULL != pptr) {
            pname.rank = pptr->name.rank;
        } else {
            pname.rank = PMIX_RANK_WILDCARD;
        }
        PMIX_INFO_LOAD(&info[n++], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
#ifdef PMIX_SPAWN_TREE_ROOT
        PMIX_INFO_LOAD(&info[n++], PMIX_SPAWN_TREE_ROOT, treeroot, PMIX_STRING);
        PMIX_INFO_LOAD(&info[n++], PMIX_SPAWN_TREE_ACTIVE, &treeactive, PMIX_UINT32);
#endif
        /* and what the application exited with, when that is a thing */
        if (0 < xcode) {
            PMIX_INFO_LOAD(&info[n++], PMIX_EXIT_CODE, &xcode, PMIX_INT);
        }
        if (NULL != errmsg) {
            PMIX_INFO_LOAD(&info[n++], PMIX_EVENT_TEXT_MESSAGE, errmsg, PMIX_STRING);
            free(errmsg);
        }

        /* pack the info for sending */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

        /* pack the status code */
        code = PMIX_EVENT_JOB_END;
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_RELEASE(caddy);
            return;
        }
        /* pack the source - it cannot be me as that will cause
         * the pmix server to upcall the event back to me */
        PMIX_LOAD_PROCID(&pnotify, jdata->nspace, 0);
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &pnotify, 1, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_RELEASE(caddy);
            return;
        }
        /* pack the range */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_RELEASE(caddy);
            return;
        }
        /* pack the number of infos */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_RELEASE(caddy);
            return;
        }
        /* pack the infos themselves */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_RELEASE(caddy);
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
            PMIX_RELEASE(caddy);
            return;
        }
        rc = PMIx_Data_copy_payload(reply, &pbkt);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_RELEASE(caddy);
            return;
        }

        /* we have to send the notification to all daemons so that
         * anyone watching for it can receive it */
        if (PRTE_SUCCESS != (rc = prte_grpcomm_xcast(PRTE_RML_TAG_NOTIFICATION, reply))) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_RELEASE(caddy);
            return;
        }
        PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:dvm:dvm_notify notification sent",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_DATA_BUFFER_RELEASE(reply);
    }

    if (prte_persistent) {
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
            PMIX_RELEASE(caddy);
            return;
        }
        rc = PMIx_Data_pack(NULL, reply, &jdata->nspace, 1, PMIX_PROC_NSPACE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_RELEASE(caddy);
            return;
        }
        prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, reply);
        PMIX_DATA_BUFFER_RELEASE(reply);
    }

    // We are done with our use of job data and have notified the other daemons
    if (notify) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFIED);
    }

    PMIX_RELEASE(caddy);
}
