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
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/dss/dss.h"
#include "src/event/event-internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/pstat/base/base.h"
#include "src/util/arch.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_path.h"

#include "src/mca/rtc/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/filem/base/base.h"
#include "src/util/session_dir.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_quit.h"
#include "src/prted/pmix/pmix_server.h"

#include "src/mca/ess/base/base.h"

/* local globals */
static bool plm_in_use=false;
static bool signals_set=false;
static prrte_event_t term_handler;
static prrte_event_t int_handler;
static prrte_event_t epipe_handler;
static char *log_path = NULL;
static void shutdown_signal(int fd, short flags, void *arg);
static void epipe_signal_callback(int fd, short flags, void *arg);
static void signal_forward_callback(int fd, short event, void *arg);
static prrte_event_t *forward_signals_events = NULL;

static void setup_sighandler(int signal, prrte_event_t *ev,
                             prrte_event_cbfunc_t cbfunc)
{
    prrte_event_signal_set(prrte_event_base, ev, signal, cbfunc, ev);
    prrte_event_set_priority(ev, PRRTE_ERROR_PRI);
    prrte_event_signal_add(ev, NULL);
}


int prrte_ess_base_prted_setup(void)
{
    int ret = PRRTE_ERROR;
    int fd;
    char log_file[PATH_MAX];
    char *jobidstring;
    char *error = NULL;
    prrte_job_t *jdata;
    prrte_proc_t *proc;
    prrte_app_context_t *app;
    char *param;
    hwloc_obj_t obj;
    unsigned i, j;
    prrte_topology_t *t;
    prrte_ess_base_signal_t *sig;
    int idx;

    plm_in_use = false;

    /* setup callback for SIGPIPE */
    setup_sighandler(SIGPIPE, &epipe_handler, epipe_signal_callback);
    /* Set signal handlers to catch kill signals so we can properly clean up
     * after ourselves.
     */
    setup_sighandler(SIGTERM, &term_handler, shutdown_signal);
    setup_sighandler(SIGINT, &int_handler, shutdown_signal);
    /** setup callbacks for signals we should forward */
    if (0 < (idx = prrte_list_get_size(&prrte_ess_base_signals))) {
        forward_signals_events = (prrte_event_t*)malloc(sizeof(prrte_event_t) * idx);
        if (NULL == forward_signals_events) {
            ret = PRRTE_ERR_OUT_OF_RESOURCE;
            error = "unable to malloc";
            goto error;
        }
        idx = 0;
        PRRTE_LIST_FOREACH(sig, &prrte_ess_base_signals, prrte_ess_base_signal_t) {
            setup_sighandler(sig->signal, forward_signals_events + idx, signal_forward_callback);
            ++idx;
        }
    }
    signals_set = true;


    /* get the local topology */
    if (NULL == prrte_hwloc_topology) {
        if (PRRTE_SUCCESS != (ret = prrte_hwloc_base_get_topology())) {
            error = "topology discovery";
            goto error;
        }
    }
    /* generate the signature */
    prrte_topo_signature = prrte_hwloc_base_get_topo_signature(prrte_hwloc_topology);
    /* remove the hostname from the topology. Unfortunately, hwloc
     * decided to add the source hostname to the "topology", thus
     * rendering it unusable as a pure topological description. So
     * we remove that information here.
     */
    obj = hwloc_get_root_obj(prrte_hwloc_topology);
    for (i=0; i < obj->infos_count; i++) {
        if (NULL == obj->infos[i].name ||
            NULL == obj->infos[i].value) {
            continue;
        }
        if (0 == strncmp(obj->infos[i].name, "HostName", strlen("HostName"))) {
            free(obj->infos[i].name);
            free(obj->infos[i].value);
            /* left justify the array */
            for (j=i; j < obj->infos_count-1; j++) {
                obj->infos[j] = obj->infos[j+1];
            }
            obj->infos[obj->infos_count-1].name = NULL;
            obj->infos[obj->infos_count-1].value = NULL;
            obj->infos_count--;
            break;
        }
    }
    if (15 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
        prrte_output(0, "%s Topology Info:", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        prrte_dss.dump(0, prrte_hwloc_topology, PRRTE_HWLOC_TOPO);
    }

    /* open and setup the prrte_pstat framework so we can provide
     * process stats if requested
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_pstat_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_pstat_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_pstat_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_pstat_base_select";
        goto error;
    }

    /* define the HNP name */
    PRRTE_PROC_MY_HNP->jobid = PRRTE_PROC_MY_NAME->jobid;
    PRRTE_PROC_MY_HNP->vpid = 0;

    /* open and setup the state machine */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_state_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_state_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_state_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_state_base_select";
        goto error;
    }
    /* open the errmgr */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_errmgr_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_errmgr_base_open";
        goto error;
    }
    /* some environments allow remote launches - e.g., ssh - so
     * open and select something -only- if we are given
     * a specific module to use
     */
    (void) prrte_mca_base_var_env_name("plm", &param);
    plm_in_use = !!(getenv(param));
    free (param);
    if (plm_in_use)  {
        if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_plm_base_framework, 0))) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_plm_base_open";
            goto error;
        }
        if (PRRTE_SUCCESS != (ret = prrte_plm_base_select())) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_plm_base_select";
            goto error;
        }
    }
    /* setup my session directory here as the OOB may need it */
    if (prrte_create_session_dirs) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_ess_base_framework.framework_output,
                             "%s setting up session dir with\n\ttmpdir: %s\n\thost %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             (NULL == prrte_process_info.tmpdir_base) ? "UNDEF" : prrte_process_info.tmpdir_base,
                             prrte_process_info.nodename));

        /* take a pass thru the session directory code to fillin the
         * tmpdir names - don't create anything yet
         */
        if (PRRTE_SUCCESS != (ret = prrte_session_dir(false, PRRTE_PROC_MY_NAME))) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_session_dir define";
            goto error;
        }
        /* clear the session directory just in case there are
         * stale directories laying around
         */
        prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
        /* now actually create the directory tree */
        if (PRRTE_SUCCESS != (ret = prrte_session_dir(true, PRRTE_PROC_MY_NAME))) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_session_dir";
            goto error;
        }
        /* set the prrte_output env file location to be in the
         * proc-specific session directory. */
        prrte_output_set_output_file_info(prrte_process_info.proc_session_dir,
                                         "output-", NULL, NULL);
        /* setup stdout/stderr */
        if (prrte_debug_daemons_file_flag) {
            /* if we are debugging to a file, then send stdout/stderr to
             * the prted log file
             */
            /* get my jobid */
            if (PRRTE_SUCCESS != (ret = prrte_util_convert_jobid_to_string(&jobidstring,
                                                                         PRRTE_PROC_MY_NAME->jobid))) {
                PRRTE_ERROR_LOG(ret);
                error = "convert_jobid";
                goto error;
            }
            /* define a log file name in the session directory */
            snprintf(log_file, PATH_MAX, "output-prted-%s-%s.log",
                     jobidstring, prrte_process_info.nodename);
            log_path = prrte_os_path(false, prrte_process_info.top_session_dir,
                                    log_file, NULL);

            fd = open(log_path, O_RDWR|O_CREAT|O_TRUNC, 0640);
            if (fd < 0) {
                /* couldn't open the file for some reason, so
                 * just connect everything to /dev/null
                 */
                fd = open("/dev/null", O_RDWR|O_CREAT|O_TRUNC, 0666);
            } else {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if(fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                    close(fd);
                }
            }
        }
    }
    /* setup the global job and node arrays */
    prrte_job_data = PRRTE_NEW(prrte_hash_table_t);
    if (PRRTE_SUCCESS != (ret = prrte_hash_table_init(prrte_job_data, 128))) {
        PRRTE_ERROR_LOG(ret);
        error = "setup job array";
        goto error;
    }
    prrte_node_pool = PRRTE_NEW(prrte_pointer_array_t);
    if (PRRTE_SUCCESS != (ret = prrte_pointer_array_init(prrte_node_pool,
                               PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                               PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                               PRRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRRTE_ERROR_LOG(ret);
        error = "setup node array";
        goto error;
    }
    prrte_node_topologies = PRRTE_NEW(prrte_pointer_array_t);
    if (PRRTE_SUCCESS != (ret = prrte_pointer_array_init(prrte_node_topologies,
                               PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                               PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                               PRRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRRTE_ERROR_LOG(ret);
        error = "setup node topologies array";
        goto error;
    }
    /* Setup the job data object for the daemons */
    /* create and store the job data object */
    jdata = PRRTE_NEW(prrte_job_t);
    jdata->jobid = PRRTE_PROC_MY_NAME->jobid;
    prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, jdata);
    /* every job requires at least one app */
    app = PRRTE_NEW(prrte_app_context_t);
    prrte_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;

    /* create and store a proc object for us */
    proc = PRRTE_NEW(prrte_proc_t);
    proc->name.jobid = PRRTE_PROC_MY_NAME->jobid;
    proc->name.vpid = PRRTE_PROC_MY_NAME->vpid;
    proc->pid = prrte_process_info.pid;
    proc->state = PRRTE_PROC_STATE_RUNNING;
    prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc);
    /* record that the daemon job is running */
    jdata->num_procs = 1;
    jdata->state = PRRTE_JOB_STATE_RUNNING;
    /* obviously, we have "reported" */
    jdata->num_reported = 1;

    /* setup the PMIx server - we need this here in case the
     * communications infrastructure wants to register
     * information */
    if (PRRTE_SUCCESS != (ret = pmix_server_init())) {
        /* the server code already barked, so let's be quiet */
        ret = PRRTE_ERR_SILENT;
        error = "pmix_server_init";
        goto error;
    }

    /* Setup the communication infrastructure */
    /* Routed system */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_routed_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_routed_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_routed_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_routed_base_select";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_oob_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_oob_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_oob_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_oob_base_select";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_rml_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rml_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_rml_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rml_base_select";
        goto error;
    }

    /* it is now safe to start the pmix server */
    pmix_server_start();

    if (NULL != prrte_process_info.my_hnp_uri) {
        pmix_value_t val;
        pmix_proc_t proc;

        /* extract the HNP's name so we can update the routing table */
        if (PRRTE_SUCCESS != (ret = prrte_rml_base_parse_uris(prrte_process_info.my_hnp_uri,
                                                            PRRTE_PROC_MY_HNP, NULL))) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_rml_parse_HNP";
            goto error;
        }
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the HNP
         * if/when we attempt to send to it
         */
        PMIX_VALUE_LOAD(&val, prrte_process_info.my_hnp_uri, PRRTE_STRING);
        PRRTE_PMIX_CONVERT_NAME(&proc, PRRTE_PROC_MY_HNP);
        if (PMIX_SUCCESS != PMIx_Store_internal(&proc, PMIX_PROC_URI, &val)) {
            PMIX_VALUE_DESTRUCT(&val);
            error = "store HNP URI";
            ret = PRRTE_ERROR;
            goto error;
        }
        PMIX_VALUE_DESTRUCT(&val);
    }

    /* select the errmgr */
    if (PRRTE_SUCCESS != (ret = prrte_errmgr_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_errmgr_base_select";
        goto error;
    }

    /*
     * Group communications
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_grpcomm_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_grpcomm_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_grpcomm_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_grpcomm_base_select";
        goto error;
    }
    /* Open/select the odls */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_odls_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_odls_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_odls_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_odls_base_select";
        goto error;
    }
    /* Open/select the rtc */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_rtc_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rtc_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_rtc_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rtc_base_select";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_rmaps_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rmaps_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_rmaps_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rmaps_base_select";
        goto error;
    }

    /* if a topology file was given, then the rmaps framework open
     * will have reset our topology. Ensure we always get the right
     * one by setting our node topology afterwards
     */
    t = PRRTE_NEW(prrte_topology_t);
    t->topo = prrte_hwloc_topology;
    /* generate the signature */
    prrte_topo_signature = prrte_hwloc_base_get_topo_signature(prrte_hwloc_topology);
    t->sig = strdup(prrte_topo_signature);
    prrte_pointer_array_add(prrte_node_topologies, t);
    if (15 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
        prrte_output(0, "%s Topology Info:", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        prrte_dss.dump(0, prrte_hwloc_topology, PRRTE_HWLOC_TOPO);
    }

    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     * Do this only if a specific PLM was given to us - the
     * prted has no need of the proxy PLM at all
     */
    if (plm_in_use) {
        if (PRRTE_SUCCESS != (ret = prrte_plm.init())) {
            PRRTE_ERROR_LOG(ret);
            error = "prrte_plm_init";
            goto error;
        }
    }

    /* setup I/O forwarding system - must come after we init routes */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_iof_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_iof_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_iof_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_iof_base_select";
        goto error;
    }
    /* setup the FileM */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_filem_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_filem_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_filem_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_filem_base_select";
        goto error;
    }

    return PRRTE_SUCCESS;

  error:
    prrte_show_help("help-prrte-runtime.txt",
                   "prrte_init:startup:internal-failure",
                   true, error, PRRTE_ERROR_NAME(ret), ret);
    /* remove our use of the session directory tree */
    prrte_session_dir_finalize(PRRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    return PRRTE_ERR_SILENT;
}

int prrte_ess_base_prted_finalize(void)
{
    prrte_ess_base_signal_t *sig;
    unsigned int i;

    if (signals_set) {
        prrte_event_del(&epipe_handler);
        prrte_event_del(&term_handler);
        prrte_event_del(&int_handler);
        /** Remove the USR signal handlers */
        i = 0;
        PRRTE_LIST_FOREACH(sig, &prrte_ess_base_signals, prrte_ess_base_signal_t) {
            prrte_event_signal_del(forward_signals_events + i);
            ++i;
        }
        free (forward_signals_events);
        forward_signals_events = NULL;
        signals_set = false;
    }

    /* cleanup */
    if (NULL != log_path) {
        unlink(log_path);
    }
    /* shutdown the pmix server */
    pmix_server_finalize();

    /* close frameworks */
    (void) prrte_mca_base_framework_close(&prrte_filem_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_grpcomm_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_iof_base_framework);
    /* first stage shutdown of the errmgr, deregister the handler but keep
     * the required facilities until the rml and oob are offline */
    prrte_errmgr.finalize();
    (void) prrte_mca_base_framework_close(&prrte_plm_base_framework);
    /* make sure our local procs are dead */
    prrte_odls.kill_local_procs(NULL);
    (void) prrte_mca_base_framework_close(&prrte_rtc_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_odls_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_routed_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_rml_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_oob_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_errmgr_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_state_base_framework);
    /* remove our use of the session directory tree */
    prrte_session_dir_finalize(PRRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    /* release the job hash table */
    PRRTE_RELEASE(prrte_job_data);
    return PRRTE_SUCCESS;
}

static void shutdown_signal(int fd, short flags, void *arg)
{
    /* trigger the call to shutdown callback to protect
     * against race conditions - the trigger event will
     * check the one-time lock
     */
    PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_FORCED_EXIT);
}

/**
 * Deal with sigpipe errors
 */
static void epipe_signal_callback(int fd, short flags, void *arg)
{
    /* for now, we just ignore them */
    return;
}

/* Pass user signals to the local application processes */
static void signal_forward_callback(int fd, short event, void *arg)
{
    prrte_event_t *signal = (prrte_event_t*)arg;
    int32_t signum, rc;
    prrte_buffer_t *cmd;
    prrte_daemon_cmd_flag_t command=PRRTE_DAEMON_SIGNAL_LOCAL_PROCS;
    prrte_jobid_t job = PRRTE_JOBID_WILDCARD;

    signum = PRRTE_EVENT_SIGNAL(signal);
    if (!prrte_execute_quiet){
        fprintf(stderr, "PRRTE: Forwarding signal %d to job\n", signum);
    }

    cmd = PRRTE_NEW(prrte_buffer_t);

    /* pack the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &command, 1, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
        return;
    }

    /* pack the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &job, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
        return;
    }

    /* pack the signal */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &signum, 1, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
        return;
    }

    /* send it to ourselves */
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_NAME, cmd,
                                          PRRTE_RML_TAG_DAEMON,
                                          NULL, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
    }

}
