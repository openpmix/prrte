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
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
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
#include "src/util/pmix_fd.h"
#include "src/util/daemon_init.h"
#include "src/util/malloc.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"

#include "src/util/name_fns.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/session_dir.h"

#include "src/mca/ess/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/prtebacktrace/base/base.h"
#include "src/mca/prteinstalldirs/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/runtime.h"
#include "src/runtime/runtime_internals.h"

#include "psched.h"

/*
 * Globals
 */
static pmix_cli_result_t results;

typedef struct {
    prte_pmix_lock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} myxfer_t;

static void infocbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myxfer_t *xfer = (myxfer_t *) cbdata;
    size_t n;

    xfer->status = status;
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

static bool forcibly_die = false;
static int wait_pipe[2];
static prte_event_t term_handler;
static prte_event_t epipe_handler;
static int term_pipe[2];
static pmix_mutex_t abort_inprogress_lock = PMIX_MUTEX_STATIC_INIT;
static void clean_abort(int fd, short flags, void *arg);
static void abort_signal_callback(int signal);

static void parent_died_fn(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results, nresults);
    clean_abort(0, 0, NULL);
    cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
}

static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    myxfer_t *lock = (myxfer_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(evhandler_ref);

    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(&lock->lock);
}

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
    int i, code;
    pmix_data_buffer_t *buffer;
    pmix_value_t val;
    pmix_proc_t proc, pname;
    pmix_status_t prc;
    myxfer_t xfer;
    pmix_data_buffer_t pbuf, *wbuf;
    pmix_byte_object_t pbo;
    int8_t flag;
    uint8_t naliases, ni;
    char **nonlocal = NULL, *personality, *mypidfile;
    int n;
    pmix_info_t info;
    pmix_value_t *vptr;
    char **pargv;
    int pargc;
    prte_schizo_base_module_t *schizo;
    pmix_cli_item_t *opt;
    char *path = NULL;
    prte_job_t *jdata;
    prte_app_context_t *app;
    prte_node_t *node;
    prte_proc_t *pptr;
    prte_topology_t *t;

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

    /* ensure we know the type of proc for when we finalize */
    prte_process_info.proc_type = PRTE_PROC_MASTER;

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
    /* add network aliases to our list of alias hostnames */
    pmix_ifgetaliases(&prte_process_info.aliases);

    /* pretty-print stack handlers */
    if (PRTE_SUCCESS != (ret = prte_util_register_stackhandlers())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "register_stackhandlers", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* pre-load any default mca param files */
    prte_preload_default_mca_params();
    psched_register_params();

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
    if (0 != pipe(term_pipe)) {
        exit(1);
    }

    /*
     * Initialize the event library
     */
    if (PRTE_SUCCESS != (ret = prte_event_base_open())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "event base open", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* setup an event to attempt normal termination on signal */
    prte_event_set(prte_event_base, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
    prte_event_add(&term_handler, NULL);
    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* setup the locks */
    if (PRTE_SUCCESS != (ret = prte_locks_init())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "locks init", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* Ensure the rest of the process info structure is initialized */
    if (PRTE_SUCCESS != (ret = prte_proc_info())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "proc info", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    if (PRTE_SUCCESS != (ret = prte_hwloc_base_register())) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "register hwloc", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* open hwloc */
    prte_hwloc_base_open();
    /* get the local topology */
    ret = prte_hwloc_base_get_topology();
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "get topology", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* setup the global job and node arrays */
    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    ret = pmix_pointer_array_init(prte_job_data,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                  PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "setup job array", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
    ret = pmix_pointer_array_init(prte_node_pool, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                  PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "setup node array", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    prte_node_topologies = PMIX_NEW(pmix_pointer_array_t);
    ret = pmix_pointer_array_init(prte_node_topologies, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                  PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "setup node topologies array", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }
    /* initialize the cache */
    prte_cache = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_cache, 1, INT_MAX, 1);

    /* setup the SCHIZO module */
    psched_schizo_init();
    schizo = &psched_schizo_module;

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

    /* if we were given a keepalive pipe, set up to monitor it now */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_KEEPALIVE);
    if (NULL != opt) {
        PMIX_SETENV_COMPAT("PMIX_KEEPALIVE_PIPE", opt->values[0], true, &environ);
    }

    /* check for debug options */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DEBUG)) {
        prte_debug_flag = true;
        if (psched_globals.verbosity <= 0) {
            // verbosity not previously set, so do so now
            psched_globals.verbosity = 10;
            psched_globals.output = pmix_output_open(NULL);
            pmix_output_set_verbosity(psched_globals.output,
                                      psched_globals.verbosity);
            prte_pmix_server_globals.output = pmix_output_open(NULL);
            pmix_output_set_verbosity(prte_pmix_server_globals.output,
                                      psched_globals.verbosity);
        }
    }

    // detach from controlling terminal, if so directed
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_DAEMONIZE)) {
        pipe(wait_pipe);
        prte_state_base.parent_fd = wait_pipe[1];
        prte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
#if defined(HAVE_SETSID)
        /* see if we were directed to separate from current session */
        if (pmix_cmd_line_is_taken(&results, PRTE_CLI_SET_SID)) {
            setsid();
        }
#endif
    }

    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_NO_READY_MSG)) {
        prte_state_base.ready_msg = false;
    } else {
        prte_state_base.ready_msg = true;
    }

    /* if we were asked to report a uri, set the MCA param to do so */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_URI);
    if (NULL != opt) {
        prte_pmix_server_globals.report_uri = strdup(opt->values[0]);
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_PID);
    if (NULL != opt) {
        /* if the string is a "-", then output to stdout */
        if (0 == strcmp(opt->values[0], "-")) {
            fprintf(stdout, "%lu\n", (unsigned long) getpid());
        } else if (0 == strcmp(opt->values[0], "+")) {
            /* output to stderr */
            fprintf(stderr, "%lu\n", (unsigned long) getpid());
        } else {
            char *leftover;
            int outpipe;
            /* see if it is an integer pipe */
            leftover = NULL;
            outpipe = strtol(opt->values[0], &leftover, 10);
            if (NULL == leftover || 0 == strlen(leftover)) {
                /* stitch together the var names and URI */
                pmix_asprintf(&leftover, "%lu", (unsigned long) getpid());
                /* output to the pipe */
                prc = pmix_fd_write(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
            } else {
                /* must be a file */
                FILE *fp;
                fp = fopen(opt->values[0], "w");
                if (NULL == fp) {
                    pmix_output(0, "Impossible to open the file %s in write mode\n", opt->values[0]);
                    PRTE_UPDATE_EXIT_STATUS(1);
                    goto DONE;
                }
                /* output my PID */
                fprintf(fp, "%lu\n", (unsigned long) getpid());
                fclose(fp);
                mypidfile = strdup(opt->values[0]);
            }
        }
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

    /* setup the state machine */
    psched_state_init();

    /* setup the errmgr */
    psched_errmgr_init();

    /* set a name */
    ret = prte_plm_base_set_hnp_name();
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "set_hnp_name", PRTE_ERROR_NAME(ret), ret);
        return ret;
    }

    /* create my job data object */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace);
    prte_set_job_data_object(jdata);

    /* set the schizo personality to "psched" by default */
    jdata->schizo = (struct prte_schizo_base_module_t *)schizo;

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
    pptr = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&pptr->name, PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_NAME->rank);
    pptr->pid = prte_process_info.pid;
    pptr->state = PRTE_PROC_STATE_RUNNING;
    PMIX_RETAIN(node); /* keep accounting straight */
    pptr->node = node;
    pmix_pointer_array_set_item(jdata->procs, PRTE_PROC_MY_NAME->rank, pptr);

    /* create the directory tree */
    ret = prte_session_dir(PRTE_PROC_MY_NAME);
    if (PRTE_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime",
                       "prte_init:startup:internal-failure", true,
                       "session_dir", PRTE_ERROR_NAME(ret), ret);
        prte_exit_status = ret;
        goto DONE;
    }

    /* setup the PMIx server library */
    if (PRTE_SUCCESS != (ret = psched_server_init(&results))) {
        /* the server code already barked, so let's be quiet */
        prte_exit_status = PRTE_ERR_SILENT;
        goto DONE;
    }

    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_KEEPALIVE)) {
        /* setup the keepalive event registration */
        memset(&xfer, 0, sizeof(myxfer_t));
        PRTE_PMIX_CONSTRUCT_LOCK(&xfer.lock);
        code = PMIX_ERR_JOB_TERMINATED;
        PMIX_LOAD_PROCID(&pname, "PMIX_KEEPALIVE_PIPE", PMIX_RANK_UNDEF);
        PMIX_INFO_LOAD(&info, PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
        PMIx_Register_event_handler(&code, 1, &info, 1, parent_died_fn, evhandler_reg_callbk,
                                    (void *) &xfer);
        PRTE_PMIX_WAIT_THREAD(&xfer.lock);
        PMIX_INFO_DESTRUCT(&info);
        PRTE_PMIX_DESTRUCT_LOCK(&xfer.lock);
    }

    // pass along any hostfile option
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
    if (NULL != opt) {
        prte_set_attribute(&app->attributes, PRTE_APP_HOSTFILE, PRTE_ATTR_GLOBAL,
                           opt->values[0], PMIX_STRING);
    }

    // pass along any dash-host option
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOST);
    if (NULL != opt) {
        prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST, PRTE_ATTR_GLOBAL,
                           opt->values[0], PMIX_STRING);
    }

    /* setup to detect any external allocation */
    ret = pmix_mca_base_framework_open(&prte_ras_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        prte_exit_status = ret;
        goto DONE;
    }
    if (PRTE_SUCCESS != (ret = prte_ras_base_select())) {
        PRTE_ERROR_LOG(ret);
        prte_exit_status = ret;
        goto DONE;
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

    // check for default hostfile CLI option
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_DEFAULT_HOSTFILE);
    if (NULL != opt) {
        if (NULL != prte_default_hostfile) {
            // command line overrides environ
            free(prte_default_hostfile);
        }
        prte_default_hostfile = strdup(opt->values[0]);
        prte_default_hostfile_given = true;
    }

    // setup the scheduler itself
    psched_scheduler_init();

    /* output a message indicating we are alive, our name, and our pid */
    if (prte_state_base.ready_msg) {
        fprintf(stderr, "Scheduler %s checking in as pid %ld on host %s\n",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (long) prte_process_info.pid,
                prte_process_info.nodename);
    }

    // trigger the state event to read the allocation
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATE);

    /* loop the event lib until an exit event is detected */
    while (prte_event_base_active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }
    PMIX_ACQUIRE_OBJECT(prte_event_base_active);

DONE:
    /* update the exit status, in case it wasn't done */
    PRTE_UPDATE_EXIT_STATUS(ret);

    /* cleanup and leave */
    psched_server_finalize();

    /* release our internal job object - this
     * also purges the session dir tree */
    PMIX_RELEASE(jdata);

    /* cleanup the process info */
    prte_proc_info_finalize();

    if (prte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prte_exit_status);
    }
    exit(prte_exit_status);
}

static void clean_abort(int fd, short flags, void *arg)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, flags, arg);

    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (pmix_mutex_trylock(&abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr,
                "%s: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n",
                prte_tool_basename);
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        return;
    }

    fflush(stderr);
    /* ensure we exit with a non-zero status */
    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
    // stop the event loop
    prte_event_base_active = false;
}

static bool first = true;
static bool second = true;

/*
 * Attempt to terminate and wait for callback
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg = "Abort is in progress...hit ctrl-c again to forcibly terminate\n\n";
    PRTE_HIDE_UNUSED_PARAMS(fd);

    /* if this is the first time thru, just get
     * the current time
     */
    if (first) {
        first = false;
        /* tell the event lib to attempt to abnormally terminate */
        if (-1 == write(term_pipe[1], &foo, 1)) {
            exit(1);
        }
    } else if (second) {
        if (-1 == write(2, (void *) msg, strlen(msg))) {
            exit(1);
        }
        fflush(stderr);
        second = false;
    } else {
        pmix_os_dirpath_destroy(prte_process_info.top_session_dir, true, NULL);
        exit(1);
    }
}
