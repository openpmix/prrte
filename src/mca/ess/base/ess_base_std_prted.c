/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/propagate/base/base.h"
#include "src/util/proc_info.h"

#include "src/event/event-internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/arch.h"
#include "src/util/os_path.h"
#include "src/util/prte_environ.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/prtereachable/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/mca/rtc/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "src/mca/ess/base/base.h"

/* local globals */
static bool plm_in_use = false;
static bool signals_set = false;
static prte_event_t term_handler;
static prte_event_t int_handler;
static prte_event_t epipe_handler;
static char *log_path = NULL;
static void shutdown_signal(int fd, short flags, void *arg);
static void epipe_signal_callback(int fd, short flags, void *arg);
static void signal_forward_callback(int fd, short event, void *arg);
static prte_event_t *forward_signals_events = NULL;

static void setup_sighandler(int signal, prte_event_t *ev, prte_event_cbfunc_t cbfunc)
{
    prte_event_signal_set(prte_event_base, ev, signal, cbfunc, ev);
    prte_event_set_priority(ev, PRTE_ERROR_PRI);
    prte_event_signal_add(ev, NULL);
}

int prte_ess_base_prted_setup(void)
{
    int ret = PRTE_ERROR;
    int fd;
    char log_file[PATH_MAX];
    char *error = NULL;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_app_context_t *app;
    char *param;
    hwloc_obj_t obj;
    unsigned i, j;
    prte_topology_t *t;
    prte_ess_base_signal_t *sig;
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
    if (0 < (idx = prte_list_get_size(&prte_ess_base_signals))) {
        forward_signals_events = (prte_event_t *) malloc(sizeof(prte_event_t) * idx);
        if (NULL == forward_signals_events) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            error = "unable to malloc";
            goto error;
        }
        idx = 0;
        PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
        {
            setup_sighandler(sig->signal, forward_signals_events + idx, signal_forward_callback);
            ++idx;
        }
    }
    signals_set = true;

    /* get the local topology */
    if (NULL == prte_hwloc_topology) {
        if (PRTE_SUCCESS != (ret = prte_hwloc_base_get_topology())) {
            error = "topology discovery";
            goto error;
        }
    }
    /* generate the signature */
    prte_topo_signature = prte_hwloc_base_get_topo_signature(prte_hwloc_topology);
    /* remove the hostname from the topology. Unfortunately, hwloc
     * decided to add the source hostname to the "topology", thus
     * rendering it unusable as a pure topological description. So
     * we remove that information here.
     */
    obj = hwloc_get_root_obj(prte_hwloc_topology);
    for (i = 0; i < obj->infos_count; i++) {
        if (NULL == obj->infos[i].name || NULL == obj->infos[i].value) {
            continue;
        }
        if (0 == strncmp(obj->infos[i].name, "HostName", strlen("HostName"))) {
            free(obj->infos[i].name);
            free(obj->infos[i].value);
            /* left justify the array */
            for (j = i; j < obj->infos_count - 1; j++) {
                obj->infos[j] = obj->infos[j + 1];
            }
            obj->infos[obj->infos_count - 1].name = NULL;
            obj->infos[obj->infos_count - 1].value = NULL;
            obj->infos_count--;
            break;
        }
    }

    /* define the HNP name */
    PMIX_LOAD_PROCID(PRTE_PROC_MY_HNP, PRTE_PROC_MY_NAME->nspace, 0);

    /* open and setup the state machine */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_state_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_state_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_state_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_state_base_select";
        goto error;
    }
    /* open the errmgr */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_errmgr_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_errmgr_base_open";
        goto error;
    }
#if PRTE_ENABLE_FT
    /* open the propagate */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_propagate_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_propagate_base_open";
        goto error;
    }
#endif
    /* some environments allow remote launches - e.g., ssh - so
     * open and select something -only- if we are given
     * a specific module to use
     */
    (void) prte_mca_base_var_env_name("plm", &param);
    if (NULL != getenv(param)) {
        plm_in_use = true;
    }
    free(param);
    if (plm_in_use) {
        if (PRTE_SUCCESS
            != (ret = prte_mca_base_framework_open(&prte_plm_base_framework,
                                                   PRTE_MCA_BASE_OPEN_DEFAULT))) {
            PRTE_ERROR_LOG(ret);
            error = "prte_plm_base_open";
            goto error;
        }
        if (PRTE_SUCCESS != (ret = prte_plm_base_select())) {
            PRTE_ERROR_LOG(ret);
            error = "prte_plm_base_select";
            goto error;
        }
    }
    /* setup my session directory here as the OOB may need it */
    if (prte_create_session_dirs) {
        PRTE_OUTPUT_VERBOSE(
            (2, prte_ess_base_framework.framework_output,
             "%s setting up session dir with\n\ttmpdir: %s\n\thost %s",
             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
             (NULL == prte_process_info.tmpdir_base) ? "UNDEF" : prte_process_info.tmpdir_base,
             prte_process_info.nodename));

        /* take a pass thru the session directory code to fillin the
         * tmpdir names - don't create anything yet
         */
        if (PRTE_SUCCESS != (ret = prte_session_dir(false, PRTE_PROC_MY_NAME))) {
            PRTE_ERROR_LOG(ret);
            error = "prte_session_dir define";
            goto error;
        }
        /* clear the session directory just in case there are
         * stale directories laying around
         */
        prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);
        /* now actually create the directory tree */
        if (PRTE_SUCCESS != (ret = prte_session_dir(true, PRTE_PROC_MY_NAME))) {
            PRTE_ERROR_LOG(ret);
            error = "prte_session_dir";
            goto error;
        }
        /* set the prte_output env file location to be in the
         * proc-specific session directory. */
        prte_output_set_output_file_info(prte_process_info.proc_session_dir, "output-", NULL, NULL);
        /* setup stdout/stderr */
        if (prte_debug_daemons_file_flag) {
            /* if we are debugging to a file, then send stdout/stderr to
             * the prted log file
             */

            /* define a log file name in the session directory */
            snprintf(log_file, PATH_MAX, "output-prted-%s-%s.log", prte_process_info.myproc.nspace,
                     prte_process_info.nodename);
            log_path = prte_os_path(false, prte_process_info.top_session_dir, log_file, NULL);

            fd = open(log_path, O_RDWR | O_CREAT | O_TRUNC, 0640);
            if (fd < 0) {
                /* couldn't open the file for some reason, so
                 * just connect everything to /dev/null
                 */
                fd = open("/dev/null", O_RDWR | O_CREAT | O_TRUNC, 0666);
            } else {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                    close(fd);
                }
            }
        }
    }
    /* Setup the job data object for the daemons */
    /* create and store the job data object */
    jdata = PRTE_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace);
    prte_set_job_data_object(jdata);
    /* every job requires at least one app */
    app = PRTE_NEW(prte_app_context_t);
    prte_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;

    /* create and store a proc object for us */
    proc = PRTE_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_NAME->rank);
    proc->job = jdata;
    proc->rank = proc->name.rank;
    proc->pid = prte_process_info.pid;
    proc->state = PRTE_PROC_STATE_RUNNING;
    prte_pointer_array_set_item(jdata->procs, proc->name.rank, proc);
    /* record that the daemon job is running */
    jdata->num_procs = 1;
    jdata->state = PRTE_JOB_STATE_RUNNING;
    /* obviously, we have "reported" */
    jdata->num_reported = 1;

    /* setup the PMIx server - we need this here in case the
     * communications infrastructure wants to register
     * information */
    if (PRTE_SUCCESS != (ret = pmix_server_init())) {
        /* the server code already barked, so let's be quiet */
        ret = PRTE_ERR_SILENT;
        error = "pmix_server_init";
        goto error;
    }

    /* Setup the communication infrastructure */
    /* Routed system */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_routed_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_routed_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_routed_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_routed_base_select";
        goto error;
    }
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_prtereachable_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_prtereachable_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_reachable_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_prtereachable_base_select";
        goto error;
    }
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_oob_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_oob_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_oob_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_oob_base_select";
        goto error;
    }
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_rml_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rml_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_rml_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rml_base_select";
        goto error;
    }

    /* it is now safe to start the pmix server */
    pmix_server_start();

    if (NULL != prte_process_info.my_hnp_uri) {
        pmix_value_t val;

        /* extract the HNP's name so we can update the routing table */
        if (PRTE_SUCCESS
            != (ret = prte_rml_base_parse_uris(prte_process_info.my_hnp_uri, PRTE_PROC_MY_HNP,
                                               NULL))) {
            PRTE_ERROR_LOG(ret);
            error = "prte_rml_parse_HNP";
            goto error;
        }
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the HNP
         * if/when we attempt to send to it
         */
        PMIX_VALUE_LOAD(&val, prte_process_info.my_hnp_uri, PMIX_STRING);
        if (PMIX_SUCCESS != PMIx_Store_internal(PRTE_PROC_MY_PROCID, PMIX_PROC_URI, &val)) {
            PMIX_VALUE_DESTRUCT(&val);
            error = "store HNP URI";
            ret = PRTE_ERROR;
            goto error;
        }
        PMIX_VALUE_DESTRUCT(&val);
    }

    /* select the errmgr */
    if (PRTE_SUCCESS != (ret = prte_errmgr_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_errmgr_base_select";
        goto error;
    }
#if PRTE_ENABLE_FT
    /* select the propagate */
    if (PRTE_SUCCESS != (ret = prte_propagate_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_propagate_base_select";
        goto error;
    }
#endif
    /*
     * Group communications
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_grpcomm_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_grpcomm_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_grpcomm_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_grpcomm_base_select";
        goto error;
    }
    /* Open/select the odls */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_odls_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_odls_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_odls_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_odls_base_select";
        goto error;
    }
    /* Open/select the rtc */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_rtc_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rtc_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_rtc_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rtc_base_select";
        goto error;
    }
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_rmaps_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rmaps_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_rmaps_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rmaps_base_select";
        goto error;
    }

    /* if a topology file was given, then the rmaps framework open
     * will have reset our topology. Ensure we always get the right
     * one by setting our node topology afterwards
     */
    t = PRTE_NEW(prte_topology_t);
    t->topo = prte_hwloc_topology;
    /* save the signature */
    t->sig = strdup(prte_topo_signature);
    /* save the topology - note that this may have to be moved later
     * to ensure a common array position with the DVM master */
    prte_pointer_array_add(prte_node_topologies, t);
    if (15 < prte_output_get_verbosity(prte_ess_base_framework.framework_output)) {
        char *output = NULL;
        pmix_topology_t topo;
        prte_output(0, "%s Topology Info:", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        topo.source = "hwloc";
        topo.topology = prte_hwloc_topology;
        ret = PMIx_Data_print(&output, NULL, &topo, PMIX_TOPO);
        if (PMIX_SUCCESS == ret) {
            fprintf(stderr, "%s\n", output);
            free(output);
        } else {
            PMIX_ERROR_LOG(ret);
        }
    }

    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     * Do this only if a specific PLM was given to us - the
     * prted has no need of the proxy PLM at all
     */
    if (plm_in_use) {
        if (PRTE_SUCCESS != (ret = prte_plm.init())) {
            PRTE_ERROR_LOG(ret);
            error = "prte_plm_init";
            goto error;
        }
    }

    /* setup I/O forwarding system - must come after we init routes */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_iof_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_iof_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_iof_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_iof_base_select";
        goto error;
    }
    /* setup the FileM */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_filem_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_filem_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_filem_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_filem_base_select";
        goto error;
    }

    return PRTE_SUCCESS;

error:
    prte_show_help("help-prte-runtime.txt", "prte_init:startup:internal-failure", true, error,
                   PRTE_ERROR_NAME(ret), ret);
    /* remove our use of the session directory tree */
    prte_session_dir_finalize(PRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);
    return PRTE_ERR_SILENT;
}

int prte_ess_base_prted_finalize(void)
{
    prte_ess_base_signal_t *sig;
    unsigned int i;

    if (signals_set) {
        prte_event_del(&epipe_handler);
        prte_event_del(&term_handler);
        prte_event_del(&int_handler);
        /** Remove the USR signal handlers */
        i = 0;
        PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
        {
            prte_event_signal_del(forward_signals_events + i);
            ++i;
        }
        free(forward_signals_events);
        forward_signals_events = NULL;
        signals_set = false;
    }

    /* cleanup */
    if (NULL != log_path) {
        unlink(log_path);
    }

#if PRTE_ENABLE_FT
    if (NULL != prte_propagate.finalize) {
        prte_propagate.finalize();
    }
    (void) prte_mca_base_framework_close(&prte_propagate_base_framework);
#endif

    if (NULL != prte_errmgr.finalize) {
        prte_errmgr.finalize();
    }

    /* shutdown the pmix server */
    pmix_server_finalize();

    /* close frameworks */
    (void) prte_mca_base_framework_close(&prte_filem_base_framework);
    (void) prte_mca_base_framework_close(&prte_grpcomm_base_framework);
    (void) prte_mca_base_framework_close(&prte_iof_base_framework);
    /* first stage shutdown of the errmgr, deregister the handler but keep
     * the required facilities until the rml and oob are offline */
    (void) prte_mca_base_framework_close(&prte_plm_base_framework);
    /* make sure our local procs are dead */
    prte_odls.kill_local_procs(NULL);
    (void) prte_mca_base_framework_close(&prte_rtc_base_framework);
    (void) prte_mca_base_framework_close(&prte_odls_base_framework);
    (void) prte_mca_base_framework_close(&prte_routed_base_framework);
    (void) prte_mca_base_framework_close(&prte_errmgr_base_framework);
    (void) prte_mca_base_framework_close(&prte_rml_base_framework);
    (void) prte_mca_base_framework_close(&prte_oob_base_framework);
    (void) prte_mca_base_framework_close(&prte_prtereachable_base_framework);
    (void) prte_mca_base_framework_close(&prte_state_base_framework);
    /* remove our use of the session directory tree */
    prte_session_dir_finalize(PRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);

    return PRTE_SUCCESS;
}

static void shutdown_signal(int fd, short flags, void *arg)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, flags, arg);
    /* trigger the call to shutdown callback to protect
     * against race conditions - the trigger event will
     * check the one-time lock
     */
    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
}

/**
 * Deal with sigpipe errors
 */
static void epipe_signal_callback(int fd, short flags, void *arg)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, flags, arg);
    /* for now, we just ignore them */
    return;
}

/* Pass user signals to the local application processes */
static void signal_forward_callback(int fd, short event, void *arg)
{
    prte_event_t *signal = (prte_event_t *) arg;
    int32_t signum, rc;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
    PRTE_HIDE_UNUSED_PARAMS(fd, event);

    signum = PRTE_EVENT_SIGNAL(signal);
    if (!prte_execute_quiet) {
        fprintf(stderr, "PRTE: Forwarding signal %d to job\n", signum);
    }

    PMIX_DATA_BUFFER_CREATE(cmd);

    /* pack the command */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, cmd, &command, 1, PRTE_DAEMON_CMD);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(cmd);
        return;
    }

    /* pack the jobid */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, cmd, &PRTE_JOBID_WILDCARD, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(cmd);
        return;
    }

    /* pack the signal */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, cmd, &PRTE_JOBID_WILDCARD, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(cmd);
        return;
    }

    /* send it to ourselves */
    if (0
        > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_NAME, cmd, PRTE_RML_TAG_DAEMON, NULL, NULL))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(cmd);
    }
}
