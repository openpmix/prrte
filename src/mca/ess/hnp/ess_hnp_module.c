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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
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

#include "src/class/prte_hash_table.h"
#include "src/class/prte_list.h"
#include "src/event/event-internal.h"
#include "src/include/hash_string.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/arch.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/fd.h"
#include "src/util/if.h"
#include "src/util/malloc.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/prte_environ.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/prtereachable/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/mca/rtc/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"

#include "src/prted/pmix/pmix_server.h"

#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/runtime.h"

#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ess/hnp/ess_hnp.h"

static int rte_init(int argc, char **argv);
static int rte_finalize(void);
static void rte_abort(int status, bool report) __prte_attribute_noreturn__;

prte_ess_base_module_t prte_ess_hnp_module = {.init = rte_init,
                                              .finalize = rte_finalize,
                                              .abort = rte_abort};

static int rte_init(int argc, char **argv)
{
    int ret;
    char *error = NULL;
    char *contact_path;
    prte_job_t *jdata;
    prte_node_t *node;
    prte_proc_t *proc;
    prte_app_context_t *app;
    char *aptr;
    char *coprocessors, **sns;
    uint32_t h;
    int idx;
    prte_topology_t *t;
    pmix_value_t pval;
    pmix_status_t pret;

    /* run the prolog */
    if (PRTE_SUCCESS != (ret = prte_ess_base_std_prolog())) {
        error = "prte_ess_base_std_prolog";
        goto error;
    }

    /* get the local topology */
    if (NULL == prte_hwloc_topology) {
        if (PRTE_SUCCESS != (ret = prte_hwloc_base_get_topology())) {
            error = "topology discovery";
            goto error;
        }
    }

    /* open and setup the state machine */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_state_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_state_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_state_base_select())) {
        error = "prte_state_base_select";
        goto error;
    }

    /* open the errmgr */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_errmgr_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_errmgr_base_open";
        goto error;
    }

#if PRTE_ENABLE_FT
    /* open the propagator */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_propagate_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_propagate_base_open";
        goto error;
    }
#endif

    /* Since we are the HNP, then responsibility for
     * defining the name falls to the PLM component for our
     * respective environment - hence, we have to open the PLM
     * first and select that component.
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_plm_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_plm_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_plm_base_select())) {
        error = "prte_plm_base_select";
        if (PRTE_ERR_FATAL == ret) {
            /* we already output a show_help - so keep down the verbage */
            ret = PRTE_ERR_SILENT;
        }
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_plm.set_hnp_name())) {
        error = "prte_plm_set_hnp_name";
        goto error;
    }

    /* setup my session directory here as the OOB may need it */
    if (prte_create_session_dirs) {
        PRTE_OUTPUT_VERBOSE(
            (2, prte_debug_output, "%s setting up session dir with\n\ttmpdir: %s\n\thost %s",
             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
             (NULL == prte_process_info.tmpdir_base) ? "UNDEF" : prte_process_info.tmpdir_base,
             prte_process_info.nodename));
        /* take a pass thru the session directory code to fillin the
         * tmpdir names - don't create anything yet
         */
        if (PRTE_SUCCESS != (ret = prte_session_dir(false, PRTE_PROC_MY_NAME))) {
            error = "prte_session_dir define";
            goto error;
        }
        /* clear the session directory just in case there are
         * stale directories laying around
         */
        prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);

        /* now actually create the directory tree */
        if (PRTE_SUCCESS != (ret = prte_session_dir(true, PRTE_PROC_MY_NAME))) {
            error = "prte_session_dir";
            goto error;
        }
    }

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
    /*
     * Routed system
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_routed_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_rml_base_open";
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
    /*
     * OOB Layer
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_oob_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_oob_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_oob_base_select())) {
        error = "prte_oob_base_select";
        goto error;
    }

    /*
     * Runtime Messaging Layer
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_rml_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_rml_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_rml_base_select())) {
        error = "prte_rml_base_select";
        goto error;
    }

    /* it is now safe to start the pmix server */
    pmix_server_start();

    /* and register our show_help recv */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_SHOW_HELP, PRTE_RML_PERSISTENT,
                            prte_show_help_recv, NULL);
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

    /* setup the error manager */
    if (PRTE_SUCCESS != (ret = prte_errmgr_base_select())) {
        error = "prte_errmgr_base_select";
        goto error;
    }
#if PRTE_ENABLE_FT
    /* setup the propagate */
    if (PRTE_SUCCESS != (ret = prte_propagate_base_select())) {
        error = "prte_propagate_base_select";
        goto error;
    }
#endif
    /* get the job data object for the daemons */
    jdata = PRTE_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace);
    prte_set_job_data_object(jdata);

    /* mark that the daemons have reported as we are the
     * only ones in the system right now, and we definitely
     * are running!
     */
    jdata->state = PRTE_JOB_STATE_DAEMONS_REPORTED;

    /* every job requires at least one app */
    app = PRTE_NEW(prte_app_context_t);
    app->app = strdup(argv[0]);
    app->argv = prte_argv_copy(argv);
    prte_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;
    /* create and store a node object where we are */
    node = PRTE_NEW(prte_node_t);
    node->name = strdup(prte_process_info.nodename);
    node->index = PRTE_PROC_MY_NAME->rank;
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
    prte_pointer_array_set_item(prte_node_pool, PRTE_PROC_MY_NAME->rank, node);

    /* create and store a proc object for us */
    proc = PRTE_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_NAME->rank);
    proc->job = jdata;
    proc->rank = proc->name.rank;
    proc->pid = prte_process_info.pid;
    prte_oob_base_get_addr(&proc->rml_uri);
    prte_process_info.my_hnp_uri = strdup(proc->rml_uri);
    /* store it in the local PMIx repo for later retrieval */
    PMIX_VALUE_LOAD(&pval, proc->rml_uri, PMIX_STRING);
    if (PMIX_SUCCESS != (pret = PMIx_Store_internal(PRTE_PROC_MY_NAME, PMIX_PROC_URI, &pval))) {
        PMIX_ERROR_LOG(pret);
        ret = PRTE_ERROR;
        PMIX_VALUE_DESTRUCT(&pval);
        error = "store uri";
        goto error;
    }
    PMIX_VALUE_DESTRUCT(&pval);
    proc->state = PRTE_PROC_STATE_RUNNING;
    PRTE_RETAIN(node); /* keep accounting straight */
    proc->node = node;
    prte_pointer_array_set_item(jdata->procs, PRTE_PROC_MY_NAME->rank, proc);

    /* record that the daemon (i.e., us) is on this node
     * NOTE: we do not add the proc object to the node's
     * proc array because we are not an application proc.
     * Instead, we record it in the daemon field of the
     * node object
     */
    PRTE_RETAIN(proc); /* keep accounting straight */
    node->daemon = proc;
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
    node->state = PRTE_NODE_STATE_UP;
    /* get our aliases - will include all the interface aliases captured in prte_init */
    aptr = prte_argv_join(prte_process_info.aliases, ',');
    prte_set_attribute(&node->attributes, PRTE_NODE_ALIAS, PRTE_ATTR_LOCAL, aptr, PMIX_STRING);
    free(aptr);
    /* record that the daemon job is running */
    jdata->num_procs = 1;
    jdata->state = PRTE_JOB_STATE_RUNNING;
    /* obviously, we have "reported" */
    jdata->num_reported = 1;

    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     */
    if (PRTE_SUCCESS != (ret = prte_plm.init())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_plm_init";
        goto error;
    }
    /*
     * Setup the remaining resource
     * management and errmgr frameworks - application procs
     * and daemons do not open these frameworks as they only use
     * the hnp proxy support in the PLM framework.
     */
    if (PRTE_SUCCESS
        != (ret = prte_mca_base_framework_open(&prte_ras_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ras_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_ras_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ras_base_find_available";
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
        error = "prte_rmaps_base_find_available";
        goto error;
    }

    /* if a topology file was given, then the rmaps framework open
     * will have reset our topology. Ensure we always get the right
     * one by setting our node topology afterwards
     */
    /* add it to the array of known topologies */
    t = PRTE_NEW(prte_topology_t);
    t->topo = prte_hwloc_topology;
    /* generate the signature */
    prte_topo_signature = prte_hwloc_base_get_topo_signature(prte_hwloc_topology);
    t->sig = strdup(prte_topo_signature);
    prte_pointer_array_add(prte_node_topologies, t);
    node->topology = t;
    if (15 < prte_output_get_verbosity(prte_ess_base_framework.framework_output)) {
        char *output = NULL;
        prte_output(0, "%s Topology Info:", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        prte_hwloc_print(&output, "\t", prte_hwloc_topology);
        prte_output(0, "%s", output);
        free(output);
    }

    /* init the hash table, if necessary */
    if (NULL == prte_coprocessors) {
        prte_coprocessors = PRTE_NEW(prte_hash_table_t);
        prte_hash_table_init(prte_coprocessors, prte_process_info.num_daemons);
    }
    /* detect and add any coprocessors */
    coprocessors = prte_hwloc_base_find_coprocessors(prte_hwloc_topology);
    if (NULL != coprocessors) {
        /* separate the serial numbers of the coprocessors
         * on this host
         */
        sns = prte_argv_split(coprocessors, ',');
        for (idx = 0; NULL != sns[idx]; idx++) {
            /* compute the hash */
            PRTE_HASH_STR(sns[idx], h);
            /* mark that this coprocessor is hosted by this node */
            prte_hash_table_set_value_uint32(prte_coprocessors, h,
                                             (void *) &(PRTE_PROC_MY_NAME->rank));
        }
        prte_argv_free(sns);
        free(coprocessors);
        prte_coprocessors_detected = true;
    }
    /* see if I am on a coprocessor */
    coprocessors = prte_hwloc_base_check_on_coprocessor();
    if (NULL != coprocessors) {
        /* compute the hash */
        PRTE_HASH_STR(coprocessors, h);
        /* mark that I am on this coprocessor */
        prte_hash_table_set_value_uint32(prte_coprocessors, h, (void *) &(PRTE_PROC_MY_NAME->rank));
        prte_set_attribute(&node->attributes, PRTE_NODE_SERIAL_NUMBER, PRTE_ATTR_LOCAL,
                           coprocessors, PMIX_STRING);
        free(coprocessors);
        prte_coprocessors_detected = true;
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

    if (prte_create_session_dirs) {
        /* set the prte_output hnp file location to be in the
         * proc-specific session directory. */
        prte_output_set_output_file_info(prte_process_info.proc_session_dir, "output-", NULL, NULL);
        /* save my contact info in a file for others to find */
        if (NULL == prte_process_info.jobfam_session_dir) {
            /* has to be set here! */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
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
    if (PRTE_ERR_SILENT != ret && !prte_report_silent_errors) {
        prte_show_help("help-prte-runtime.txt", "prte_init:startup:internal-failure", true, error,
                       PRTE_ERROR_NAME(ret), ret);
    }
    /* remove my contact info file, if we have session directories */
    if (NULL != prte_process_info.jobfam_session_dir) {
        contact_path = prte_os_path(false, prte_process_info.jobfam_session_dir, "contact.txt",
                                    NULL);
        unlink(contact_path);
        free(contact_path);
    }
    /* remove our use of the session directory tree */
    prte_session_dir_finalize(PRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);
    return PRTE_ERR_SILENT;
}

static int rte_finalize(void)
{
    char *contact_path;

    /* shutdown the pmix server */
    pmix_server_finalize();
    /* output any lingering stdout/err data */
    fflush(stdout);
    fflush(stderr);

    /* first stage shutdown of the errmgr, deregister the handler but keep
     * the required facilities until the rml and oob are offline */
    prte_errmgr.finalize();
#if PRTE_ENABLE_FT
    (void) prte_mca_base_framework_close(&prte_propagate_base_framework);
#endif

    /* remove my contact info file, if we have session directories */
    if (NULL != prte_process_info.jobfam_session_dir) {
        contact_path = prte_os_path(false, prte_process_info.jobfam_session_dir, "contact.txt",
                                    NULL);
        unlink(contact_path);
        free(contact_path);
    }

    /* close frameworks */
    (void) prte_mca_base_framework_close(&prte_filem_base_framework);
    (void) prte_mca_base_framework_close(&prte_grpcomm_base_framework);
    (void) prte_mca_base_framework_close(&prte_iof_base_framework);
    (void) prte_mca_base_framework_close(&prte_plm_base_framework);
    /* make sure our local procs are dead */
    prte_odls.kill_local_procs(NULL);
    (void) prte_mca_base_framework_close(&prte_rtc_base_framework);
    (void) prte_mca_base_framework_close(&prte_odls_base_framework);
    (void) prte_mca_base_framework_close(&prte_routed_base_framework);
    (void) prte_mca_base_framework_close(&prte_rml_base_framework);
    (void) prte_mca_base_framework_close(&prte_oob_base_framework);
    (void) prte_mca_base_framework_close(&prte_prtereachable_base_framework);
    (void) prte_mca_base_framework_close(&prte_errmgr_base_framework);
    (void) prte_mca_base_framework_close(&prte_state_base_framework);

    /* remove our use of the session directory tree */
    prte_session_dir_finalize(PRTE_PROC_MY_NAME);
    /* ensure we scrub the session directory tree */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);

    free(prte_topo_signature);

    return PRTE_SUCCESS;
}

static void rte_abort(int status, bool report)
{
    /* do NOT do a normal finalize as this will very likely
     * hang the process. We are aborting due to an abnormal condition
     * that precludes normal cleanup
     *
     * We do need to do the following bits to make sure we leave a
     * clean environment. Taken from prte_finalize():
     * - Assume errmgr cleans up child processes before we exit.
     */

    /* ensure we scrub the session directory tree */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);
    /* - Clean out the global structures
     * (not really necessary, but good practice)
     */
    prte_proc_info_finalize();
    /* just exit */
    exit(status);
}
