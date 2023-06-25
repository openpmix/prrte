/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include <ctype.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif

#include <pmix_server.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/daemon_init.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"

#include "src/util/name_fns.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/session_dir.h"

#include "src/mca/prtebacktrace/base/base.h"
#include "src/mca/prteinstalldirs/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/runtime.h"

/*
 * Globals
 */
static pmix_cli_result_t results;

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} myxfer_t;

static void infocbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myxfer_t *xfer = (myxfer_t *) cbdata;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(status);

    if (NULL != info) {
        xfer->ninfo = ninfo;
        PMIX_INFO_CREATE(xfer->info, xfer->ninfo);
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&xfer->info[n], &info[n]);
        }
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PRTE_PMIX_WAKEUP_THREAD(&xfer->lock);
}

static int wait_pipe[2];

static int wait_dvm(pid_t pid)
{
    char reply;
    int rc;
    int status;

    close(wait_pipe[1]);
    do {
        rc = read(wait_pipe[0], &reply, 1);
    } while (0 > rc && EINTR == errno);

    if (1 == rc && 'K' == reply) {
        return 0;
    } else if (0 == rc) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
    }
    return 255;
}

static bool check_exist(char *path)
{
    struct stat buf;
    /* coverity[TOCTOU] */
    if (0 == stat(path, &buf)) { /* exists */
        return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    int ret = 0;
    int i;
    pmix_data_buffer_t *buffer;
    pmix_value_t val;
    pmix_proc_t proc;
    pmix_status_t prc;
    myxfer_t xfer;
    pmix_data_buffer_t pbuf, *wbuf;
    pmix_byte_object_t pbo;
    int8_t flag;
    uint8_t naliases, ni;
    char **nonlocal = NULL, *personality;
    int n;
    pmix_value_t *vptr;
    char **pargv;
    int pargc;
    prte_schizo_base_module_t *schizo;
    pmix_cli_item_t *opt;
    char *path = NULL;

    /* initialize the globals */
    prte_tool_basename = pmix_basename(argv[0]);
    prte_tool_actual = "psched";
    pargc = argc;
    pargv = pmix_argv_copy_strip(argv);  // strip any quoted arguments

    /* carry across the toolname */
    pmix_tool_basename = prte_tool_basename;

    /* initialize install dirs code */
    ret = pmix_mca_base_framework_open(&prte_prteinstalldirs_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        fprintf(stderr,
                "prte_prteinstalldirs_base_open() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PRTE_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }

    /* initialize the MCA infrastructure */
    if (check_exist(prte_install_dirs.prtelibdir)) {
        pmix_asprintf(&path, "prte@%s", prte_install_dirs.prtelibdir);
    }
    ret = pmix_init_util(NULL, 0, path);
    if (NULL != path) {
        free(path);
    }
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "pmix_init_util", PRTE_ERROR_NAME(ret), ret);
        return prte_pmix_convert_status(ret);
    }
    ret = pmix_show_help_add_dir(prte_install_dirs.prtedatadir);
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "show_help_add_dir", PRTE_ERROR_NAME(ret), ret);
        return prte_pmix_convert_status(ret);
    }

    /* we always need the prrte and pmix params */
    ret = prte_schizo_base_parse_prte(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "parse_prte params", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    ret = prte_schizo_base_parse_pmix(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "parse_pmix params", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* initialize the memory allocator */
    prte_malloc_init();

    /* initialize the output system */
    pmix_output_init();

    /* keyval lex-based parser */
    /* Setup the parameter system */
    if (PRTE_SUCCESS != (ret = pmix_mca_base_var_init())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "var_init", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* set the nodename so anyone who needs it has it - this
     * must come AFTER we initialize the installdirs */
    prte_setup_hostname();

    /* pretty-print stack handlers */
    if (PRTE_SUCCESS != (ret = prte_util_register_stackhandlers())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "register_stackhandlers", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* pre-load any default mca param files */
    preload_default_mca_params();

    /* Register all MCA Params */
    if (PRTE_SUCCESS != (ret = prte_register_params())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "register params", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    ret = pmix_mca_base_framework_open(&prte_prtebacktrace_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "open backtrace", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* open the SCHIZO framework */
    ret = pmix_mca_base_framework_open(&prte_schizo_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    if (PRTE_SUCCESS != (ret = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    /* get our schizo module */
    personality = "psched";
    schizo = prte_schizo_base_detect_proxy(personality);
    if (NULL == schizo) {
        pmix_show_help("help-schizo-base.txt", "no-proxy", true,
                       prte_tool_basename, personality);
        return 1;
    }

    /* parse the CLI to load the MCA params */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    ret = schizo->parse_cli(pargv, &results, PMIX_CLI_SILENT);
    if (PRTE_SUCCESS != ret) {
        if (PRTE_OPERATION_SUCCEEDED == ret) {
            return PRTE_SUCCESS;
        }
        if (PRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename,
                    prte_strerror(ret));
        }
        return ret;
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning message
     */
    if (0 == geteuid()) {
        schizo->allow_run_as_root(&results); // will exit us if not allowed
    }

    /* check for debug options */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DEBUG)) {
        prte_debug_flag = true;
    }
    // leave_session_attached is used to indicate that we are not to daemonize
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_LEAVE_SESSION_ATTACHED)) {
        prte_leave_session_attached = true;
    }

    // detach from controlling terminal, if so directed
    if (!prte_leave_session_attached) {
        pipe(wait_pipe);
        prte_state_base.parent_fd = wait_pipe[1];
        prte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
#if defined(HAVE_SETSID)
        setsid();
#endif
    }
    /* ensure we silence any compression warnings */
    PMIX_SETENV_COMPAT("PMIX_MCA_compress_base_silence_warning", "1", true, &environ);

    /* run the prolog */
    if (PRTE_SUCCESS != (ret = prte_ess_base_std_prolog())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "prte_ess_base_std_prolog", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* get the local topology */
    if (NULL == prte_hwloc_topology) {
        if (PRTE_SUCCESS != (ret = prte_hwloc_base_get_topology())) {
            pmix_show_help("help-prte-runtime",
                           "prte_init:startup:internal-failure", true,
                           "topology discovery", PRTE_ERROR_NAME(ret), ret);
            return ret;
        }
    }

    /* open and setup the state machine */
    ret = pmix_mca_base_framework_open(&prte_state_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "state open", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    if (PRTE_SUCCESS != (ret = prte_state_base_select())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "state select", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* open the errmgr */
    ret = pmix_mca_base_framework_open(&prte_errmgr_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "prte_errmgr_base_open", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    /* set a name */
    ret = prte_plm_base_set_hnp_name();
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "set_hnp_name", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* take a pass thru the session directory code to fillin the
     * tmpdir names - don't create anything yet
     */
    ret = prte_session_dir(false, PRTE_PROC_MY_NAME);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "session_dir define", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    /* clear the session directory just in case there are
     * stale directories laying around
     */
    prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);

    /* now actually create the directory tree */
    ret = prte_session_dir(true, PRTE_PROC_MY_NAME);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "session_dir", PRTE_ERROR_NAME(ret), ret);
        prte_exit_status = ret;
        goto DONE;
    }

    /* setup the PMIx server - we need this here in case the
     * communications infrastructure wants to register
     * information */
    if (PRTE_SUCCESS != (ret = pmix_server_init())) {
        /* the server code already barked, so let's be quiet */
        prte_exit_status = PRTE_ERR_SILENT;
        goto DONE;
    }
    pmix_server_start();

    /* setup the error manager */
    if (PRTE_SUCCESS != (ret = prte_errmgr_base_select())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "errmgr_base_select", PRTE_ERROR_NAME(ret), ret);
        prte_exit_status = ret;
        goto DONE;
     }

    /* create my job data object */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace);
    prte_set_job_data_object(jdata);

    /* set the schizo personality to "psched" by default */
    jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy("psched");
    if (NULL == jdata->schizo) {
        pmix_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, "prte");
        error = "select personality";
        ret = PRTE_ERR_SILENT;
        goto error;
    }

    /* every job requires at least one app */
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup(argv[0]);
    app->argv = PMIX_ARGV_COPY_COMPAT(argv);
    pmix_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;
    /* create and store a node object where we are */
    node = PMIX_NEW(prte_node_t);
    node->name = strdup(prte_process_info.nodename);
    node->index = PRTE_PROC_MY_NAME->rank;
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_LOC_VERIFIED);
    pmix_pointer_array_set_item(prte_node_pool, PRTE_PROC_MY_NAME->rank, node);

    /* create and store a proc object for us */
    proc = PMIX_NEW(prte_proc_t);
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
    PMIX_RETAIN(node); /* keep accounting straight */
    proc->node = node;
    pmix_pointer_array_set_item(jdata->procs, PRTE_PROC_MY_NAME->rank, proc);

    /* setup to detect any external allocation */
    if (PRTE_SUCCESS
        != (ret = pmix_mca_base_framework_open(&prte_ras_base_framework,
                                               PMIX_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ras_base_open";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_ras_base_select())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ras_base_find_available";
        goto error;
    }

    /* add our topology to the array of known topologies */
    t = PMIX_NEW(prte_topology_t);
    t->topo = prte_hwloc_topology;
    /* generate the signature */
    prte_topo_signature = prte_hwloc_base_get_topo_signature(prte_hwloc_topology);
    t->sig = strdup(prte_topo_signature);
    t->index = pmix_pointer_array_add(prte_node_topologies, t);
    node->topology = t;
    node->available = prte_hwloc_base_filter_cpus(prte_hwloc_topology);
    if (15 < pmix_output_get_verbosity(prte_ess_base_framework.framework_output)) {
        char *output = NULL;
        pmix_output(0, "%s Topology Info:", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        prte_hwloc_print(&output, "\t", prte_hwloc_topology);
        pmix_output(0, "%s", output);
        free(output);
    }

    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (prte_debug_flag) {
        fprintf(stderr, "Scheduler %s checking in as pid %ld on host %s\n",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (long) prte_process_info.pid,
                prte_process_info.nodename);
    }

    /* loop the event lib until an exit event is detected */
    while (prte_event_base_active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }
    PMIX_ACQUIRE_OBJECT(prte_event_base_active);

DONE:
    /* update the exit status, in case it wasn't done */
    PRTE_UPDATE_EXIT_STATUS(ret);

    /* cleanup and leave */
    prte_finalize();

    prte_session_dir_cleanup(PRTE_PROC_MY_NAME->nspace);
    /* cleanup the process info */
    prte_proc_info_finalize();

    if (prte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prte_exit_status);
    }
    exit(prte_exit_status);
}
