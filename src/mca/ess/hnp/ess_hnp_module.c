/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
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

#include "src/include/hash_string.h"
#include "src/class/prrte_hash_table.h"
#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"

#include "src/util/arch.h"
#include "src/util/argv.h"
#include "src/util/if.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/util/malloc.h"
#include "src/util/basename.h"
#include "src/util/fd.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/pstat/base/base.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/oob/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/mca/rtc/base/base.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"

#include "src/prted/prted_submit.h"
#include "src/prted/pmix/pmix_server.h"

#include "src/util/show_help.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/name_fns.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/prrte_locks.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/hnp/ess_hnp.h"

static int rte_init(int argc, char **argv);
static int rte_finalize(void);
static void rte_abort(int status, bool report) __prrte_attribute_noreturn__;

prrte_ess_base_module_t prrte_ess_hnp_module = {
    rte_init,
    rte_finalize,
    rte_abort,
    NULL /* ft_event */
};

/* local globals */
static bool signals_set=false;
static bool forcibly_die=false;
static prrte_event_t term_handler;
static prrte_event_t epipe_handler;
static int term_pipe[2];
static prrte_event_t *forward_signals_events = NULL;

static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void epipe_signal_callback(int fd, short flags, void *arg);
static void signal_forward_callback(int fd, short event, void *arg);

static void setup_sighandler(int signal, prrte_event_t *ev,
                             prrte_event_cbfunc_t cbfunc)
{
    prrte_event_signal_set(prrte_event_base, ev, signal, cbfunc, ev);
    prrte_event_set_priority(ev, PRRTE_ERROR_PRI);
    prrte_event_signal_add(ev, NULL);
}

static int rte_init(int argc, char **argv)
{
    int ret;
    char *error = NULL;
    char *contact_path;
    prrte_job_t *jdata;
    prrte_node_t *node;
    prrte_proc_t *proc;
    prrte_app_context_t *app;
    char **aliases, *aptr;
    char *coprocessors, **sns;
    uint32_t h;
    int idx;
    prrte_topology_t *t;
    prrte_ess_base_signal_t *sig;
    pmix_proc_t pname;
    pmix_value_t pval;
    pmix_status_t pret;

    /* run the prolog */
    if (PRRTE_SUCCESS != (ret = prrte_ess_base_std_prolog())) {
        error = "prrte_ess_base_std_prolog";
        goto error;
    }

    /* setup callback for SIGPIPE */
    setup_sighandler(SIGPIPE, &epipe_handler, epipe_signal_callback);
    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup. However, we cannot directly use libevent
     * to trap these signals as otherwise we cannot respond
     * to them if we are stuck in an event! So instead use
     * the basic POSIX trap functions to handle the signal,
     * and then let that signal handler do some magic to
     * avoid the hang
     *
     * NOTE: posix traps don't allow us to do anything major
     * in them, so use a pipe tied to a libevent event to
     * reach a "safe" place where the termination event can
     * be created
     */
    pipe(term_pipe);
    /* setup an event to attempt normal termination on signal */
    prrte_event_set(prrte_event_base, &term_handler, term_pipe[0], PRRTE_EV_READ, clean_abort, NULL);
    prrte_event_set_priority(&term_handler, PRRTE_ERROR_PRI);
    prrte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
       children inherit it */
    if (prrte_fd_set_cloexec(term_pipe[0]) != PRRTE_SUCCESS ||
        prrte_fd_set_cloexec(term_pipe[1]) != PRRTE_SUCCESS) {
        error = "unable to set the pipe to CLOEXEC";
        goto error;
    }

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

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

    /* if we are using xml for output, put an mpirun start tag */
    if (prrte_xml_output) {
        fprintf(prrte_xml_fp, "<mpirun>\n");
        fflush(prrte_xml_fp);
    }

    /* open and setup the prrte_pstat framework so we can provide
     * process stats if requested
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_pstat_base_framework, 0))) {
        error = "prrte_pstat_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_pstat_base_select())) {
        error = "prrte_pstat_base_select";
        goto error;
    }

    /* open and setup the state machine */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_state_base_framework, 0))) {
        error = "prrte_state_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_state_base_select())) {
        error = "prrte_state_base_select";
        goto error;
    }

    /* open the errmgr */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_errmgr_base_framework, 0))) {
        error = "prrte_errmgr_base_open";
        goto error;
    }

    /* Since we are the HNP, then responsibility for
     * defining the name falls to the PLM component for our
     * respective environment - hence, we have to open the PLM
     * first and select that component.
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_plm_base_framework, 0))) {
        error = "prrte_plm_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_plm_base_select())) {
        error = "prrte_plm_base_select";
        if (PRRTE_ERR_FATAL == ret) {
            /* we already output a show_help - so keep down the verbage */
            ret = PRRTE_ERR_SILENT;
        }
        goto error;
    }
    /* if we were spawned by a singleton, our jobid was given to us */
    if (NULL != prrte_ess_base_jobid) {
        if (PRRTE_SUCCESS != (ret = prrte_util_convert_string_to_jobid(&PRRTE_PROC_MY_NAME->jobid, prrte_ess_base_jobid))) {
            error = "convert_string_to_jobid";
            goto error;
        }
        PRRTE_PROC_MY_NAME->vpid = 0;
    } else {
        if (PRRTE_SUCCESS != (ret = prrte_plm.set_hnp_name())) {
            error = "prrte_plm_set_hnp_name";
            goto error;
        }
    }

    /* setup my session directory here as the OOB may need it */
    if (prrte_create_session_dirs) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_debug_output,
                             "%s setting up session dir with\n\ttmpdir: %s\n\thost %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             (NULL == prrte_process_info.tmpdir_base) ? "UNDEF" : prrte_process_info.tmpdir_base,
                             prrte_process_info.nodename));
        /* take a pass thru the session directory code to fillin the
         * tmpdir names - don't create anything yet
         */
        if (PRRTE_SUCCESS != (ret = prrte_session_dir(false, PRRTE_PROC_MY_NAME))) {
            error = "prrte_session_dir define";
            goto error;
        }
        /* clear the session directory just in case there are
         * stale directories laying around
         */
        prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);

        /* now actually create the directory tree */
        if (PRRTE_SUCCESS != (ret = prrte_session_dir(true, PRRTE_PROC_MY_NAME))) {
            error = "prrte_session_dir";
            goto error;
        }
    }

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
    /*
     * Routed system
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_routed_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rml_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_routed_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_routed_base_select";
        goto error;
    }
    /*
     * OOB Layer
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_oob_base_framework, 0))) {
        error = "prrte_oob_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_oob_base_select())) {
        error = "prrte_oob_base_select";
        goto error;
    }

    /*
     * Runtime Messaging Layer
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_rml_base_framework, 0))) {
        error = "prrte_rml_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_rml_base_select())) {
        error = "prrte_rml_base_select";
        goto error;
    }

    /* it is now safe to start the pmix server */
    pmix_server_start();

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


    /* setup the error manager */
    if (PRRTE_SUCCESS != (ret = prrte_errmgr_base_select())) {
        error = "prrte_errmgr_base_select";
        goto error;
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
    /* mark that the daemons have reported as we are the
     * only ones in the system right now, and we definitely
     * are running!
     */
    jdata->state = PRRTE_JOB_STATE_DAEMONS_REPORTED;

    /* every job requires at least one app */
    app = PRRTE_NEW(prrte_app_context_t);
    app->app = strdup(argv[0]);
    app->argv = prrte_argv_copy(argv);
    prrte_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;
    /* create and store a node object where we are */
    node = PRRTE_NEW(prrte_node_t);
    node->name = strdup(prrte_process_info.nodename);
    node->index = PRRTE_PROC_MY_NAME->vpid;
    prrte_pointer_array_set_item(prrte_node_pool, 0, node);

    /* create and store a proc object for us */
    proc = PRRTE_NEW(prrte_proc_t);
    proc->name.jobid = PRRTE_PROC_MY_NAME->jobid;
    proc->name.vpid = PRRTE_PROC_MY_NAME->vpid;
    proc->pid = prrte_process_info.pid;
    prrte_oob_base_get_addr(&proc->rml_uri);
    prrte_process_info.my_hnp_uri = strdup(proc->rml_uri);
    /* store it in the local PMIx repo for later retrieval */
    PMIX_VALUE_LOAD(&pval, proc->rml_uri, PMIX_STRING);
    PRRTE_PMIX_CONVERT_NAME(&pname, PRRTE_PROC_MY_NAME);
    if (PMIX_SUCCESS != (pret = PMIx_Store_internal(&pname, PMIX_PROC_URI, &pval))) {
        PMIX_ERROR_LOG(pret);
        ret = PRRTE_ERROR;
        PMIX_VALUE_DESTRUCT(&pval);
        error = "store uri";
        goto error;
    }
    PMIX_VALUE_DESTRUCT(&pval);
    /* we are also officially a daemon, so better update that field too */
    prrte_process_info.my_daemon_uri = strdup(proc->rml_uri);
    proc->state = PRRTE_PROC_STATE_RUNNING;
    PRRTE_RETAIN(node);  /* keep accounting straight */
    proc->node = node;
    prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc);
    /* record that the daemon (i.e., us) is on this node
     * NOTE: we do not add the proc object to the node's
     * proc array because we are not an application proc.
     * Instead, we record it in the daemon field of the
     * node object
     */
    PRRTE_RETAIN(proc);   /* keep accounting straight */
    node->daemon = proc;
    PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_DAEMON_LAUNCHED);
    node->state = PRRTE_NODE_STATE_UP;
    /* if we are to retain aliases, get ours */
    if (prrte_retain_aliases) {
        aliases = NULL;
        prrte_ifgetaliases(&aliases);
        if (0 < prrte_argv_count(aliases)) {
            /* add our own local name to it */
            prrte_argv_append_nosize(&aliases, prrte_process_info.nodename);
            aptr = prrte_argv_join(aliases, ',');
            prrte_set_attribute(&node->attributes, PRRTE_NODE_ALIAS, PRRTE_ATTR_LOCAL, aptr, PRRTE_STRING);
            free(aptr);
        }
        prrte_argv_free(aliases);
    }
    /* record that the daemon job is running */
    jdata->num_procs = 1;
    jdata->state = PRRTE_JOB_STATE_RUNNING;
    /* obviously, we have "reported" */
    jdata->num_reported = 1;

    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     */
    if (PRRTE_SUCCESS != (ret = prrte_plm.init())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_plm_init";
        goto error;
    }
    /*
     * Setup the remaining resource
     * management and errmgr frameworks - application procs
     * and daemons do not open these frameworks as they only use
     * the hnp proxy support in the PLM framework.
     */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_ras_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_ras_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_ras_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_ras_base_find_available";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_rmaps_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rmaps_base_open";
        goto error;
    }
    if (PRRTE_SUCCESS != (ret = prrte_rmaps_base_select())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_rmaps_base_find_available";
        goto error;
    }

    /* if a topology file was given, then the rmaps framework open
     * will have reset our topology. Ensure we always get the right
     * one by setting our node topology afterwards
     */
    /* add it to the array of known topologies */
    t = PRRTE_NEW(prrte_topology_t);
    t->topo = prrte_hwloc_topology;
    /* generate the signature */
    prrte_topo_signature = prrte_hwloc_base_get_topo_signature(prrte_hwloc_topology);
    t->sig = strdup(prrte_topo_signature);
    prrte_pointer_array_add(prrte_node_topologies, t);
    node->topology = t;
    if (15 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
        prrte_output(0, "%s Topology Info:", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        prrte_dss.dump(0, prrte_hwloc_topology, PRRTE_HWLOC_TOPO);
    }


    /* init the hash table, if necessary */
    if (NULL == prrte_coprocessors) {
        prrte_coprocessors = PRRTE_NEW(prrte_hash_table_t);
        prrte_hash_table_init(prrte_coprocessors, prrte_process_info.num_procs);
    }
    /* detect and add any coprocessors */
    coprocessors = prrte_hwloc_base_find_coprocessors(prrte_hwloc_topology);
    if (NULL != coprocessors) {
        /* separate the serial numbers of the coprocessors
         * on this host
         */
        sns = prrte_argv_split(coprocessors, ',');
        for (idx=0; NULL != sns[idx]; idx++) {
            /* compute the hash */
            PRRTE_HASH_STR(sns[idx], h);
            /* mark that this coprocessor is hosted by this node */
            prrte_hash_table_set_value_uint32(prrte_coprocessors, h, (void*)&(PRRTE_PROC_MY_NAME->vpid));
        }
        prrte_argv_free(sns);
        free(coprocessors);
        prrte_coprocessors_detected = true;
    }
    /* see if I am on a coprocessor */
    coprocessors = prrte_hwloc_base_check_on_coprocessor();
    if (NULL != coprocessors) {
        /* compute the hash */
        PRRTE_HASH_STR(coprocessors, h);
        /* mark that I am on this coprocessor */
        prrte_hash_table_set_value_uint32(prrte_coprocessors, h, (void*)&(PRRTE_PROC_MY_NAME->vpid));
        prrte_set_attribute(&node->attributes, PRRTE_NODE_SERIAL_NUMBER, PRRTE_ATTR_LOCAL, coprocessors, PRRTE_STRING);
        free(coprocessors);
        prrte_coprocessors_detected = true;
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

    if (prrte_create_session_dirs) {
        /* set the prrte_output hnp file location to be in the
         * proc-specific session directory. */
        prrte_output_set_output_file_info(prrte_process_info.proc_session_dir,
                                         "output-", NULL, NULL);
        /* save my contact info in a file for others to find */
        if( NULL == prrte_process_info.jobfam_session_dir ){
            /* has to be set here! */
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
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
    if (PRRTE_ERR_SILENT != ret && !prrte_report_silent_errors) {
        prrte_show_help("help-prrte-runtime.txt",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }
    /* remove my contact info file, if we have session directories */
    if (NULL != prrte_process_info.jobfam_session_dir) {
        contact_path = prrte_os_path(false, prrte_process_info.jobfam_session_dir,
                                    "contact.txt", NULL);
        unlink(contact_path);
        free(contact_path);
    }
    /* remove our use of the session directory tree */
    prrte_session_dir_finalize(PRRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    return PRRTE_ERR_SILENT;
}

static int rte_finalize(void)
{
    char *contact_path;
    prrte_job_t *jdata;
    uint32_t key;
    prrte_ess_base_signal_t *sig;
    unsigned int i;

    if (signals_set) {
        /* Remove the epipe handler */
        prrte_event_signal_del(&epipe_handler);
        /* remove the term handler */
        prrte_event_del(&term_handler);
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

    /* shutdown the pmix server */
    pmix_server_finalize();
    /* output any lingering stdout/err data */
    fflush(stdout);
    fflush(stderr);

    /* first stage shutdown of the errmgr, deregister the handler but keep
     * the required facilities until the rml and oob are offline */
    prrte_errmgr.finalize();

    /* remove my contact info file, if we have session directories */
    if (NULL != prrte_process_info.jobfam_session_dir) {
        contact_path = prrte_os_path(false, prrte_process_info.jobfam_session_dir,
                                    "contact.txt", NULL);
        unlink(contact_path);
        free(contact_path);
    }

    /* shutdown the pmix server */
    pmix_server_finalize();

    /* close frameworks */
    (void) prrte_mca_base_framework_close(&prrte_filem_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_grpcomm_base_framework);
    (void) prrte_mca_base_framework_close(&prrte_iof_base_framework);
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

    /* close the xml output file, if open */
    if (prrte_xml_output) {
        fprintf(prrte_xml_fp, "</mpirun>\n");
        fflush(prrte_xml_fp);
        if (stdout != prrte_xml_fp) {
            fclose(prrte_xml_fp);
        }
    }

    /* release the job hash table */
    PRRTE_HASH_TABLE_FOREACH(key, uint32, jdata, prrte_job_data) {
        if (NULL != jdata) {
            PRRTE_RELEASE(jdata);
        }
    }
    PRRTE_RELEASE(prrte_job_data);

    if (prrte_do_not_launch) {
        exit(0);
    }

{
    prrte_pointer_array_t * array = prrte_node_topologies;
    int i;
    if( array->number_free != array->size ) {
        prrte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                prrte_topology_t * topo = (prrte_topology_t *)array->addr[i];
                topo->topo = NULL;
                PRRTE_RELEASE(topo);
            }
            array->addr[i] = NULL;
        }
        prrte_mutex_unlock(&array->lock);
    }
}
    PRRTE_RELEASE(prrte_node_topologies);

{
    prrte_pointer_array_t * array = prrte_node_pool;
    int i;
    prrte_node_t* node = (prrte_node_t *)prrte_pointer_array_get_item(prrte_node_pool, 0);
    assert(NULL != node);
    PRRTE_RELEASE(node->daemon);
    node->daemon = NULL;
    if( array->number_free != array->size ) {
        prrte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                node= (prrte_node_t*)array->addr[i];
                PRRTE_RELEASE(node);
            }
            array->addr[i] = NULL;
        }
        prrte_mutex_unlock(&array->lock);
    }
}
    PRRTE_RELEASE(prrte_node_pool);

    free(prrte_topo_signature);

    return PRRTE_SUCCESS;
}

static void rte_abort(int status, bool report)
{
    /* do NOT do a normal finalize as this will very likely
     * hang the process. We are aborting due to an abnormal condition
     * that precludes normal cleanup
     *
     * We do need to do the following bits to make sure we leave a
     * clean environment. Taken from prrte_finalize():
     * - Assume errmgr cleans up child processes before we exit.
     */

    /* ensure we scrub the session directory tree */
    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    /* - Clean out the global structures
     * (not really necessary, but good practice)
     */
    prrte_proc_info_finalize();
    /* just exit */
    exit(status);
}

static void clean_abort(int fd, short flags, void *arg)
{
    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prrte_atomic_trylock(&prrte_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            /* kill any local procs */
            prrte_odls.kill_local_procs(NULL);
            /* whack any lingering session directory files from our jobs */
            prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
            /* cleanup our pmix server */
            PMIx_server_finalize();
            /* exit with a non-zero status */
            exit(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        }
        fprintf(stderr, "%s: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n", prrte_tool_basename);
        forcibly_die = true;
        /* reset the event */
        prrte_event_add(&term_handler, NULL);
        return;
    }
    /* ensure we exit with a non-zero status */
    PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);

    /* ensure that the forwarding of stdin stops */
    prrte_job_term_ordered = true;
    /* tell us to be quiet - hey, the user killed us with a ctrl-c,
     * so need to tell them that!
     */
    prrte_execute_quiet = true;
    /* We are in an event handler; the job completed procedure
       will delete the signal handler that is currently running
       (which is a Bad Thing), so we can't call it directly.
       Instead, we have to exit this handler and setup to call
       job_completed() after this. */
    prrte_plm.terminate_orteds();;
}

static struct timeval current, last={0,0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg = "Abort is in progress...hit ctrl-c again within 5 seconds to forcibly terminate\n\n";

    /* if this is the first time thru, just get
     * the current time
     */
    if (first) {
        first = false;
        gettimeofday(&current, NULL);
    } else {
        /* get the current time */
        gettimeofday(&current, NULL);
        /* if this is within 5 seconds of the
         * last time we were called, then just
         * exit - we are probably stuck
         */
        if ((current.tv_sec - last.tv_sec) < 5) {
            exit(1);
        }
        write(1, (void*)msg, strlen(msg));
    }
    /* save the time */
    last.tv_sec = current.tv_sec;
    /* tell the event lib to attempt to abnormally terminate */
    write(term_pipe[1], &foo, 1);
}

/**
 * Deal with sigpipe errors
 */
static int sigpipe_error_count=0;
static void epipe_signal_callback(int fd, short flags, void *arg)
{
    sigpipe_error_count++;

    if (10 < sigpipe_error_count) {
        /* time to abort */
        prrte_output(0, "%s: SIGPIPE detected on fd %d - aborting", prrte_tool_basename, fd);
        clean_abort(0, 0, NULL);
    }

    return;
}

/**
 * Pass user signals to the remote application processes
 */
static void  signal_forward_callback(int fd, short event, void *arg)
{
    prrte_event_t *signal = (prrte_event_t*)arg;
    int signum, ret;

    signum = PRRTE_EVENT_SIGNAL(signal);
    if (!prrte_execute_quiet){
        fprintf(stderr, "%s: Forwarding signal %d to job\n",
                prrte_tool_basename, signum);
    }

    /** send the signal out to the processes, including any descendants */
    if (PRRTE_SUCCESS != (ret = prrte_plm.signal_job(PRRTE_JOBID_WILDCARD, signum))) {
        fprintf(stderr, "Signal %d could not be sent to the job (returned %d)",
                signum, ret);
    }
}
