/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/include/constants.h"
#include "src/include/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#    include <poll.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/mutex.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/cmd_line.h"
#include "src/util/daemon_init.h"
#include "src/util/fd.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"
#include "src/util/prte_getcwd.h"
#include "src/util/show_help.h"

#include "src/class/prte_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/odls/odls.h"
#include "src/mca/plm/plm.h"
#include "src/mca/prteif/prteif.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

#include "include/prte.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/prted.h"

typedef struct {
    prte_pmix_lock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static pmix_nspace_t spawnednspace;
static pmix_proc_t myproc;
static bool signals_set = false;
static bool forcibly_die = false;
static prte_event_t term_handler;
static prte_event_t epipe_handler;
static int term_pipe[2];
static prte_mutex_t prun_abort_inprogress_lock = PRTE_MUTEX_STATIC_INIT;
static prte_event_t *forward_signals_events = NULL;
static char *mypidfile = NULL;
static bool verbose = false;
static bool want_prefix_by_default = (bool) PRTE_WANT_PRTE_PREFIX_BY_DEFAULT;
static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void signal_forward_callback(int fd, short args, void *cbdata);
static void epipe_signal_callback(int fd, short args, void *cbdata);
static int prep_singleton(const char *name);

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void setupcbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                        void *provided_cbdata, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    mylock_t *mylock = (mylock_t *) provided_cbdata;
    size_t n;

    if (NULL != info) {
        mylock->ninfo = ninfo;
        PMIX_INFO_CREATE(mylock->info, mylock->ninfo);
        /* cycle across the provided info */
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mylock->info[n], &info[n]);
        }
    } else {
        mylock->info = NULL;
        mylock->ninfo = 0;
    }
    mylock->status = status;

    /* release the caller */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }

    PRTE_PMIX_WAKEUP_THREAD(&mylock->lock);
}

static void spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(lock);
    lock->status = status;
    if (PMIX_SUCCESS == status) {
        lock->msg = strdup(nspace);
    }
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void parent_died_fn(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    clean_abort(0, 0, NULL);
    cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
}

static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(&lock->lock);
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

static void setup_sighandler(int signal, prte_event_t *ev, prte_event_cbfunc_t cbfunc)
{
    prte_event_signal_set(prte_event_base, ev, signal, cbfunc, ev);
    prte_event_signal_add(ev, NULL);
}

int main(int argc, char *argv[])
{
    int rc = 1, i, j;
    char *param, *timeoutenv, *ptr, *tpath, *cptr;
    prte_pmix_lock_t lock;
    prte_list_t apps;
    prte_pmix_app_t *app;
    pmix_info_t *iptr, info;
    pmix_status_t ret;
    bool flag;
    size_t n, ninfo, param_len;
    pmix_app_t *papps;
    size_t napps;
    mylock_t mylock;
    uint32_t ui32;
    char **pargv;
    int pargc;
    prte_job_t *jdata;
    prte_app_context_t *dapp;
    bool proxyrun = false;
    void *jinfo;
    pmix_proc_t pname;
    pmix_value_t *val;
    pmix_data_array_t darray;
    char **hostfiles = NULL;
    char **hosts = NULL;
    prte_schizo_base_module_t *schizo;
    prte_ess_base_signal_t *sig;
    char **targv, **options;
    char *outdir = NULL;
    char *outfile = NULL;
    pmix_status_t code;
    char *personality;
    prte_cli_result_t results;
    prte_cli_item_t *opt;

    /* init the globals */
    PRTE_CONSTRUCT(&apps, prte_list_t);
    if (NULL == (param = getenv("PRTE_BASENAME"))) {
        prte_tool_basename = prte_basename(argv[0]);
    } else {
        prte_tool_basename = strdup(param);
    }
    if (0 == strcmp(prte_tool_basename, "prterun")) {
        prte_tool_actual = "prterun";
    } else {
        prte_tool_actual = "prte";
    }
    pargc = argc;
    pargv = prte_argv_copy_strip(argv); // strip any incoming quoted arguments

    /* save a pristine copy of the environment for launch purposes.
     * This MUST be done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs will get a bunch of
     * params only relevant to PRRTE. Skip all PMIx and PRRTE params
     * as those are only targeting us
     */
    prte_launch_environ = NULL;
    for (i=0; NULL != environ[i]; i++) {
        if (0 != strncmp(environ[i], "PMIX_", 5) &&
            0 != strncmp(environ[i], "PRTE_", 5)) {
            prte_argv_append_nosize(&prte_launch_environ, environ[i]);
        }
    }

    /* because we have to use the schizo framework and init our hostname
     * prior to parsing the incoming argv for cmd line options, do a hacky
     * search to support passing of impacted options (e.g., verbosity for schizo) */
    rc = prte_schizo_base_parse_prte(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prte_schizo_base_parse_pmix(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* init the tiny part of PRTE we initially need */
    prte_init_util(PRTE_PROC_MASTER);

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
    if (0 != (rc = pipe(term_pipe))) {
        exit(1);
    }
    /* setup an event to attempt normal termination on signal */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "Unable to initialize event library\n");
        exit(1);
    }
    prte_event_set(prte_event_base, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
    prte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
     children inherit it */
    if (prte_fd_set_cloexec(term_pipe[0]) != PRTE_SUCCESS
        || prte_fd_set_cloexec(term_pipe[1]) != PRTE_SUCCESS) {
        fprintf(stderr, "unable to set the pipe to CLOEXEC\n");
        prte_progress_thread_finalize(NULL);
        exit(1);
    }

    /* setup callback for SIGPIPE */
    setup_sighandler(SIGPIPE, &epipe_handler, epipe_signal_callback);

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* open the SCHIZO framework */
    rc = prte_mca_base_framework_open(&prte_schizo_base_framework,
                                      PRTE_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* look for any personality specification */
    personality = NULL;
    for (i = 0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            personality = argv[i + 1];
            break;
        }
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo_base_detect_proxy(personality);
    if (NULL == schizo) {
        prte_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, personality);
        return 1;
    }
    if (0 != strcmp(schizo->name, "prte")) {
        proxyrun = true;
    } else {
        /* if we are using the "prte" personality, but we
         * are not actually running as "prte" or are actively
         * testing the proxy capability , then we are acting
         * as a proxy */
        if (0 != strcmp(prte_tool_basename, "prte") || prte_schizo_base.test_proxy_launch) {
            proxyrun = true;
        }
    }
    if (NULL == personality) {
        personality = schizo->name;
    }

    /* parse the input argv to get values, including everyone's MCA params */
    PRTE_CONSTRUCT(&results, prte_cli_result_t);
    rc = schizo->parse_cli(pargv, &results, PRTE_CLI_WARN);
    if (PRTE_SUCCESS != rc) {
        PRTE_DESTRUCT(&results);
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
        }
        return rc;
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning message
     */
    if (0 == geteuid()) {
        schizo->allow_run_as_root(&results); // will exit us if not allowed
    }

    /* if we were given a keepalive pipe, set up to monitor it now */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_KEEPALIVE);
    if (NULL != opt) {
        prte_setenv("PMIX_KEEPALIVE_PIPE", opt->values[0], true, &environ);
    }

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if (!prte_debug_flag && prte_cmd_line_is_taken(&results, PRTE_CLI_DAEMONIZE)) {
        pipe(wait_pipe);
        prte_state_base_parent_fd = wait_pipe[1];
        prte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    } else {
#if defined(HAVE_SETSID)
        /* see if we were directed to separate from current session */
        if (prte_cmd_line_is_taken(&results, PRTE_CLI_SET_SID)) {
            setsid();
        }
#endif
    }

    if (prte_cmd_line_is_taken(&results, PRTE_CLI_NO_READY_MSG)) {
        prte_state_base_ready_msg = false;
    }

    if (prte_cmd_line_is_taken(&results, PRTE_CLI_SYSTEM_SERVER)) {
        /* we should act as system-level PMIx server */
        prte_setenv("PRTE_MCA_pmix_system_server", "1", true, &environ);
    }
    /* always act as session-level PMIx server */
    prte_setenv("PRTE_MCA_pmix_session_server", "1", true, &environ);
    /* if we were asked to report a uri, set the MCA param to do so */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_REPORT_URI);
    if (NULL != opt) {
        prte_pmix_server_globals.report_uri = strdup(opt->values[0]);
    }
    /* don't aggregate help messages as that will apply job-to-job */
    prte_setenv("PRTE_MCA_prte_base_help_aggregate", "0", true, &environ);

    /* if we are supporting a singleton, push its ID into the environ
     * so it can get picked up and registered by server init */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_SINGLETON);
    if (NULL != opt) {
        prte_pmix_server_globals.singleton = strdup(opt->values[0]);
    }

    /* Setup MCA params */
    prte_register_params();

    /* default to a persistent DVM */
    prte_persistent = true;

    /* if we are told to daemonize, then we cannot have apps */
    if (!prte_cmd_line_is_taken(&results, PRTE_CLI_DAEMONIZE)) {
        /* see if they want to run an application - let's parse
         * the cmd line to get it */
        rc = prte_parse_locals(schizo, &apps, pargv, &hostfiles, &hosts);
        // not-found => no app given
        if (PRTE_SUCCESS != rc && PRTE_ERR_NOT_FOUND != rc) {
            PRTE_UPDATE_EXIT_STATUS(rc);
            goto DONE;
        }
        /* did they provide an app? */
        if (PMIX_SUCCESS != rc || 0 == prte_list_get_size(&apps)) {
            if (proxyrun) {
                prte_show_help("help-prun.txt", "prun:executable-not-specified", true,
                               prte_tool_basename, prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
            /* nope - just need to wait for instructions */
        } else {
            /* they did provide an app - this is only allowed
             * when running as a proxy! */
            if (!proxyrun) {
                prte_show_help("help-prun.txt", "prun:executable-incorrectly-given", true,
                               prte_tool_basename, prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
            /* mark that we are not a persistent DVM */
            prte_persistent = false;
        }
    }

    /* setup PRTE infrastructure */
    if (PRTE_SUCCESS != (ret = prte_init(&pargc, &pargv, PRTE_PROC_MASTER))) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }
    /* get my proc ID */
    ret = PMIx_Get(NULL, PMIX_PROCID, NULL, 0, &val);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    memcpy(&myproc, val->data.proc, sizeof(pmix_proc_t));
    PMIX_VALUE_RELEASE(val);

    /* setup callbacks for signals we should forward */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_FWD_SIGNALS);
    if (NULL != opt) {
        param = opt->values[0];
    } else {
        param = NULL;
    }
    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    if (0 < (i = prte_list_get_size(&prte_ess_base_signals))) {
        forward_signals_events = (prte_event_t *) malloc(sizeof(prte_event_t) * i);
        if (NULL == forward_signals_events) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
        i = 0;
        PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
        {
            setup_sighandler(sig->signal, forward_signals_events + i, signal_forward_callback);
            ++i;
        }
    }
    signals_set = true;

    /* if we are supporting a singleton, add it to our jobs */
    if (NULL != prte_pmix_server_globals.singleton) {
        rc = prep_singleton(prte_pmix_server_globals.singleton);
        if (PRTE_SUCCESS != ret) {
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
    }

    /* setup the keepalive event registration */
    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    code = PMIX_ERR_JOB_TERMINATED;
    PMIX_LOAD_PROCID(&pname, "PMIX_KEEPALIVE_PIPE", PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, &info, 1, parent_died_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);

    /* check for launch directives in case we were launched by a
     * tool wanting to direct our operation - this needs to be
     * done prior to starting the DVM as it may include instructions
     * on the daemon executable, the fork/exec agent to be used by
     * the daemons, or other directives impacting the DVM itself. */
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    /*  Have to cycle over directives we support*/
    ret = PMIx_Get(&pname, PMIX_FORKEXEC_AGENT, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        /* set our fork/exec agent */
        PMIX_VALUE_RELEASE(val);
    }

    /* start the DVM */

    /* get the daemon job object - was created by ess/hnp component */
    if (NULL == (jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace))) {
        prte_show_help("help-prun.txt", "bad-job-object", true, prte_tool_basename);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }
    /* ess/hnp also should have created a daemon "app" */
    if (NULL == (dapp = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, 0))) {
        prte_show_help("help-prun.txt", "bad-app-object", true, prte_tool_basename);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        goto DONE;
    }

    opt = prte_cmd_line_get_param(&results, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        if (NULL != strcasestr(opt->values[0], PRTE_CLI_NOLAUNCH)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
        }
    }

    /* Did the user specify a prefix, or want prefix by default? */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_PREFIX);
    if (NULL != opt || want_prefix_by_default) {
        if (NULL != opt) {
            param = strdup(opt->values[0]);
        } else {
            /* --enable-prun-prefix-default was given to prun */
            param = strdup(prte_install_dirs.prefix);
        }
        /* "Parse" the param, aka remove superfluous path_sep. */
        param_len = strlen(param);
        while (0 == strcmp(PRTE_PATH_SEP, &(param[param_len - 1]))) {
            param[param_len - 1] = '\0';
            param_len--;
            if (0 == param_len) {
                prte_show_help("help-prun.txt", "prun:empty-prefix", true, prte_tool_basename,
                               prte_tool_basename);
                PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                goto DONE;
            }
        }
        prte_set_attribute(&dapp->attributes, PRTE_APP_PREFIX_DIR, PRTE_ATTR_GLOBAL, param,
                           PMIX_STRING);
        free(param);
    } else {
        /* Check if called with fully-qualified path to prte.
           (Note: Put this second so can override with --prefix (above). */
        tpath = NULL;
        if ('/' == argv[0][0]) {
            char *tmp_basename = NULL;
            tpath = prte_dirname(argv[0]);

            if (NULL != tpath) {
                /* Quick sanity check to ensure we got
                   something/bin/<exec_name> and that the installation
                   tree is at least more or less what we expect it to
                   be */
                tmp_basename = prte_basename(tpath);
                if (0 == strcmp("bin", tmp_basename)) {
                    char *tmp = tpath;
                    tpath = prte_dirname(tmp);
                    free(tmp);
                } else {
                    free(tpath);
                    tpath = NULL;
                }
                free(tmp_basename);
            }
            prte_set_attribute(&dapp->attributes, PRTE_APP_PREFIX_DIR, PRTE_ATTR_GLOBAL,
                               tpath, PMIX_STRING);
        }
    }

    /* setup to listen for commands sent specifically to me, even though I would probably
     * be the one sending them! Unfortunately, since I am a participating daemon,
     * there are times I need to send a command to "all daemons", and that means *I* have
     * to receive it too
     */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON, PRTE_RML_PERSISTENT,
                            prte_daemon_recv, NULL);

    /* setup to capture job-level info */
    PMIX_INFO_LIST_START(jinfo);

    /* see if we ourselves were spawned by someone */
    ret = PMIx_Get(&prte_process_info.myproc, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS == ret) {
        PMIX_LOAD_PROCID(&prte_process_info.my_parent, val->data.proc->nspace, val->data.proc->rank);
        PMIX_VALUE_RELEASE(val);
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_REQUESTOR_IS_TOOL, NULL, PMIX_BOOL);
        /* indicate that we are launching on behalf of a parent */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PARENT_ID, &prte_process_info.my_parent, PMIX_PROC);
    } else {
        PMIX_LOAD_PROCID(&prte_process_info.my_parent, prte_process_info.myproc.nspace, prte_process_info.myproc.rank);
    }

    /* add any hostfile directives to the daemon job */
    if (prte_persistent) {
        opt = prte_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
        if (NULL != opt) {
            tpath = prte_argv_join(opt->values, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_HOSTFILE,
                               PRTE_ATTR_GLOBAL, tpath, PMIX_STRING);
            free(tpath);
        }

        /* Did the user specify any hosts? */
        opt = prte_cmd_line_get_param(&results, PRTE_CLI_HOST);
        if (NULL != opt) {
            char *tval;
            tval = prte_argv_join(opt->values, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_DASH_HOST,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
        }
    } else {
        /* the directives might be in the app(s) */
        if (NULL != hostfiles) {
            char *tval;
            tval = prte_argv_join(hostfiles, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_HOSTFILE,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
            prte_argv_free(hostfiles);
        }
        if (NULL != hosts) {
            char *tval;
            tval = prte_argv_join(hosts, ',');
            prte_set_attribute(&dapp->attributes, PRTE_APP_DASH_HOST,
                               PRTE_ATTR_GLOBAL, tval, PMIX_STRING);
            free(tval);
            prte_argv_free(hosts);
        }
    }

    /* spawn the DVM - we skip the initial steps as this
     * isn't a user-level application */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATE);

    /* we need to loop the event library until the DVM is alive */
    while (prte_event_base_active && !prte_dvm_ready) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_REPORT_PID);
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
                prte_asprintf(&leftover, "%lu", (unsigned long) getpid());
                /* output to the pipe */
                rc = prte_fd_write(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
            } else {
                /* must be a file */
                FILE *fp;
                fp = fopen(opt->values[0], "w");
                if (NULL == fp) {
                    prte_output(0, "Impossible to open the file %s in write mode\n", opt->values[0]);
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

    if (prte_persistent) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        goto proceed;
    }

    /***** CHECK FOR LAUNCH DIRECTIVES - ADD THEM TO JOB INFO IF FOUND ****/
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    ret = PMIx_Get(&pname, PMIX_LAUNCH_DIRECTIVES, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        iptr = (pmix_info_t *) val->data.darray->array;
        ninfo = val->data.darray->size;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_LIST_XFER(ret, jinfo, &iptr[n]);
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* pass the personality */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PERSONALITY, personality, PMIX_STRING);

    /* get display options */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            targv = prte_argv_split(opt->values[n], ',');

            for (int idx = 0; idx < prte_argv_count(targv); idx++) {
                if (0 == strncasecmp(targv[idx], PRTE_CLI_ALLOC, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":"PRTE_CLI_DISPALLOC, PMIX_STRING);
                }
                if (0 == strcasecmp(targv[idx], PRTE_CLI_MAP)) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":"PRTE_CLI_DISPLAY, PMIX_STRING);
                }
                if (0 == strncasecmp(targv[idx], PRTE_CLI_BIND, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, ":"PRTE_CLI_REPORT, PMIX_STRING);
                }
                if (0 == strcasecmp(targv[idx], PRTE_CLI_MAPDEV)) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":"PRTE_CLI_DISPDEV, PMIX_STRING);
                }
                if (0 == strncasecmp(targv[idx], PRTE_CLI_TOPO, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":"PRTE_CLI_DISPTOPO, PMIX_STRING);
                }
            }
            prte_argv_free(targv);
        }
    }

    /* cannot have both files and directory set for output */
    outdir = NULL;
    outfile = NULL;
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            targv = prte_argv_split(opt->values[0], ',');
            for (int idx = 0; NULL != targv[idx]; idx++) {
                /* check for qualifiers */
                cptr = strchr(targv[idx], ':');
                if (NULL != cptr) {
                    *cptr = '\0';
                    ++cptr;
                    /* could be multiple qualifiers, so separate them */
                    options = prte_argv_split(cptr, ',');
                    for (int m=0; NULL != options[m]; m++) {
                        if (0 == strcasecmp(options[m], PRTE_CLI_NOCOPY)) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_ONLY, NULL, PMIX_BOOL);
                        } else if (0 == strcasecmp(options[m], PRTE_CLI_PATTERN)) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_PATTERN, NULL, PMIX_BOOL);
#ifdef PMIX_IOF_OUTPUT_RAW
                        } else if (0 == strcasecmp(options[m], PRTE_CLI_RAW)) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_RAW, NULL, PMIX_BOOL);
#endif
                        }
                    }
                    prte_argv_free(options);
                }
                if (0 == strlen(targv[idx])) {
                    // only qualifiers were given
                    continue;
                }
                /* remove any '=' sign in the directive */
                if (NULL != (ptr = strchr(targv[idx], '='))) {
                    *ptr = '\0';
                    ++ptr; // step over '=' sign
                }
                if (0 == strncasecmp(targv[idx], PRTE_CLI_TAG, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_TAG_OUTPUT, NULL, PMIX_BOOL);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_RANK, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_RANK_OUTPUT, NULL, PMIX_BOOL);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_TIMESTAMP, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_XML, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_XML_OUTPUT, NULL, PMIX_BOOL);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_MERGE_ERROUT, strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_DIR, strlen(targv[idx]))) {
                    if (NULL != outfile) {
                        prte_show_help("help-prted.txt", "both-file-and-dir-set", true, outfile, outdir);
                        return PRTE_ERR_FATAL;
                    }
                    if (NULL == ptr) {
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "missing-qualifier", true,
                                       "output", "directory", "directory");
                        return PRTE_ERR_FATAL;
                    }
                    /* If the given filename isn't an absolute path, then
                     * convert it to one so the name will be relative to
                     * the directory where prun was given as that is what
                     * the user will have seen */
                    if (!prte_path_is_absolute(ptr)) {
                        char cwd[PRTE_PATH_MAX];
                        if (NULL == getcwd(cwd, sizeof(cwd))) {
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                            goto DONE;
                        }
                        outdir = prte_os_path(false, cwd, ptr, NULL);
                    } else {
                        outdir = strdup(ptr);
                    }
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_DIRECTORY, outdir, PMIX_STRING);
                } else if (0 == strncasecmp(targv[idx], PRTE_CLI_FILE, strlen(targv[idx]))) {
                    if (NULL != outdir) {
                        prte_show_help("help-prted.txt", "both-file-and-dir-set", true, outfile, outdir);
                        return PRTE_ERR_FATAL;
                    }
                    if (NULL == ptr) {
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "missing-qualifier", true,
                                       "output", "filename", "filename");
                        return PRTE_ERR_FATAL;
                    }
                    /* If the given filename isn't an absolute path, then
                     * convert it to one so the name will be relative to
                     * the directory where prun was given as that is what
                     * the user will have seen */
                    if (!prte_path_is_absolute(ptr)) {
                        char cwd[PRTE_PATH_MAX];
                        if (NULL == getcwd(cwd, sizeof(cwd))) {
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                            goto DONE;
                        }
                        outfile = prte_os_path(false, cwd, ptr, NULL);
                    } else {
                        outfile = strdup(ptr);
                    }
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_FILE, outfile, PMIX_STRING);
                }
            }
            prte_argv_free(targv);
        }
    }
    if (NULL != outdir) {
        free(outdir);
    }
    if (NULL != outfile) {
        free(outfile);
    }

    /* check what user wants us to do with stdin */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_STDIN_TGT, opt->values[0], PMIX_STRING);
    }

    opt = prte_cmd_line_get_param(&results, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, opt->values[0], PMIX_STRING);
    }

    /* if the user specified a ranking policy, then set it */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RANKBY, opt->values[0], PMIX_STRING);
    }

    /* if the user specified a binding policy, then set it */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, opt->values[0], PMIX_STRING);
    }

    /* check for an exec agent */
   opt = prte_cmd_line_get_param(&results, PRTE_CLI_EXEC_AGENT);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_EXEC_AGENT, opt->values[0], PMIX_STRING);
    }

    /* mark if recovery was enabled on the cmd line */
    if (prte_cmd_line_is_taken(&results, PRTE_CLI_ENABLE_RECOVERY)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_RECOVERABLE, NULL, PMIX_BOOL);
    }
    /* record the max restarts */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_MAX_RESTARTS);
    if (NULL != opt) {
        ui32 = strtol(opt->values[0], NULL, 10);
        PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t)
        {
            PMIX_INFO_LIST_ADD(ret, app->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
        }
    }
    /* if continuous operation was specified */
    if (prte_cmd_line_is_taken(&results, PRTE_CLI_CONTINUOUS)) {
        /* mark this job as continuously operating */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_CONTINUOUS, NULL, PMIX_BOOL);
    }

    /* if stop-on-exec was specified */
    if (prte_cmd_line_is_taken(&results, PRTE_CLI_STOP_ON_EXEC)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    timeoutenv = NULL;
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_TIMEOUT);
    if (NULL != opt || NULL != (timeoutenv = getenv("MPIEXEC_TIMEOUT"))) {
        if (NULL != timeoutenv) {
            i = strtol(timeoutenv, NULL, 10);
            /* both cannot be present, or they must agree */
            if (NULL != opt) {
                n = strtol(opt->values[0], NULL, 10);
                if (i != n) {
                    prte_show_help("help-prun.txt", "prun:timeoutconflict", false,
                                   prte_tool_basename, n, timeoutenv);
                    PRTE_UPDATE_EXIT_STATUS(1);
                    goto DONE;
                }
            }
        } else {
            i = strtol(opt->values[0], NULL, 10);
        }
#ifdef PMIX_JOB_TIMEOUT
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_TIMEOUT, &i, PMIX_INT);
#else
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT, &i, PMIX_INT);
#endif
    }
    if (prte_cmd_line_is_taken(&results, PRTE_CLI_STACK_TRACES)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_STACKTRACES, NULL, PMIX_BOOL);
    }
    if (prte_cmd_line_is_taken(&results, PRTE_CLI_REPORT_STATE)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_REPORT_STATE, NULL, PMIX_BOOL);
    }
#ifdef PMIX_SPAWN_TIMEOUT
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_SPAWN_TIMEOUT);
    if (NULL != opt) {
        i = strtol(opt->values[0], NULL, 10);
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_SPAWN_TIMEOUT, &i, PMIX_INT);
    }
#endif

    /* give the schizo components a chance to add to the job info */
    schizo->job_info(&results, jinfo);

    /* pickup any relevant envars */
    ninfo = 4;
    PMIX_INFO_CREATE(iptr, ninfo);
    flag = true;
    PMIX_INFO_LOAD(&iptr[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    ui32 = geteuid();
    PMIX_INFO_LOAD(&iptr[1], PMIX_USERID, &ui32, PMIX_UINT32);
    ui32 = getegid();
    PMIX_INFO_LOAD(&iptr[2], PMIX_GRPID, &ui32, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_PERSONALITY, personality, PMIX_STRING);

    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prte_process_info.myproc.nspace, iptr, ninfo, setupcbfunc,
                                        &mylock);
    if (PMIX_SUCCESS != ret) {
        prte_output(0, "Error setting up application: %s", PMIx_Error_string(ret));
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        PRTE_UPDATE_EXIT_STATUS(ret);
        goto DONE;
    }
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_INFO_FREE(iptr, ninfo);
    if (PMIX_SUCCESS != mylock.status) {
        prte_output(0, "Error setting up application: %s", PMIx_Error_string(mylock.status));
        PRTE_UPDATE_EXIT_STATUS(mylock.status);
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        goto DONE;
    }
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n = 0; n < mylock.ninfo; n++) {
            if (0 == strncmp(mylock.info[n].key, PMIX_SET_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_ADD_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_UNSET_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_PREPEND_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_APPEND_ENVAR, PMIX_MAX_KEYLEN)) {
                PMIX_INFO_LIST_XFER(ret, jinfo, &mylock.info[n]);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }

    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
    if (PMIX_ERR_EMPTY == ret) {
        iptr = NULL;
        ninfo = 0;
    } else if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_UPDATE_EXIT_STATUS(rc);
        goto DONE;
    } else {
        iptr = (pmix_info_t *) darray.array;
        ninfo = darray.size;
    }
    PMIX_INFO_LIST_RELEASE(jinfo);

    /* convert the apps to an array */
    napps = prte_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    n = 0;
    PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t)
    {
        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = prte_argv_copy(app->app.argv);
        papps[n].env = prte_argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        PMIX_INFO_LIST_CONVERT(ret, app->info, &darray);
        if (PMIX_SUCCESS != ret) {
            if (PMIX_ERR_EMPTY == ret) {
                papps[n].info = NULL;
                papps[n].ninfo = 0;
            } else {
                PMIX_ERROR_LOG(ret);
                PRTE_UPDATE_EXIT_STATUS(rc);
                goto DONE;
            }
        } else {
            papps[n].info = (pmix_info_t *) darray.array;
            papps[n].ninfo = darray.size;
        }
        ++n;
    }

    if (verbose) {
        prte_output(0, "Spawning job");
    }

    /* let the PMIx server handle it for us so that all the job infos
     * get properly recorded - e.g., forwarding IOF */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_Spawn_nb(iptr, ninfo, papps, napps, spcbfunc, &lock);
    if (PRTE_SUCCESS != ret) {
        prte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        PRTE_UPDATE_EXIT_STATUS(rc);
        goto DONE;
    }
    /* we have to cycle the event library here so we can process
     * the spawn request */
    while (prte_event_base_active && lock.active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }
    PRTE_ACQUIRE_OBJECT(&lock.lock);
    if (PMIX_SUCCESS != lock.status) {
        PRTE_UPDATE_EXIT_STATUS(lock.status);
        goto DONE;
    }
    PMIX_LOAD_NSPACE(spawnednspace, lock.msg);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (verbose) {
        prte_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(spawnednspace));
    }

    /* check what user wants us to do with stdin */
    PMIX_LOAD_NSPACE(pname.nspace, spawnednspace);
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        if (0 == strcmp(opt->values[0], "all")) {
            pname.rank = PMIX_RANK_WILDCARD;
        } else if (0 == strcmp(opt->values[0], "none")) {
            pname.rank = PMIX_RANK_INVALID;
        } else {
            pname.rank = 0;
        }
    } else {
        pname.rank = 0;
    }
    if (PMIX_RANK_INVALID != pname.rank) {
        PMIX_INFO_CREATE(iptr, 1);
        PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIx_IOF_push(&pname, 1, NULL, iptr, 1, opcbfunc, &lock);
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            prte_output(0, "IOF push of stdin failed: %s", PMIx_Error_string(ret));
        } else if (PMIX_SUCCESS == ret) {
            PRTE_PMIX_WAIT_THREAD(&lock);
        }
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        PMIX_INFO_FREE(iptr, 1);
    }

proceed:
    /* loop the event lib until an exit event is detected */
    while (prte_event_base_active) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    }

    PRTE_ACQUIRE_OBJECT(prte_event_base_active);

    /* close the push of our stdin */
    PMIX_INFO_LOAD(&info, PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, &info, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prte_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_DESTRUCT(&info);

DONE:
    /* cleanup and leave */
    prte_finalize();

    if (NULL != mypidfile) {
        unlink(mypidfile);
    }

    if (prte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prte_exit_status);
    }
    exit(prte_exit_status);
}

static void clean_abort(int fd, short flags, void *arg)
{
    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prte_mutex_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
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
    /* ensure that the forwarding of stdin stops */
    prte_job_term_ordered = true;
    /* tell us to be quiet - hey, the user killed us with a ctrl-c,
     * so need to tell them that!
     */
    prte_execute_quiet = true;
    /* We are in an event handler; the job completed procedure
     will delete the signal handler that is currently running
     (which is a Bad Thing), so we can't call it directly.
     Instead, we have to exit this handler and setup to call
     job_completed() after this. */
    prte_plm.terminate_orteds();
}

static bool first = true;
static bool second = true;

static void surekill(void)
{
    prte_proc_t *child;
    int n;
    pid_t pid;

    for (n=0; n < prte_local_children->size; n++) {
        child = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, n);
        if (NULL != child && 0 < child->pid) {
            pid = child->pid;
#if HAVE_SETPGID
            {
                pid_t pgrp;
                pgrp = getpgid(pid);
                if (-1 != pgrp) {
                    /* target the lead process of the process
                     * group so we ensure that the signal is
                     * seen by all members of that group. This
                     * ensures that the signal is seen by any
                     * child processes our child may have
                     * started
                     */
                    pid = -pgrp;
                }
            }
#endif
            kill(pid, SIGKILL);
        }
    }
}

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg = "Abort is in progress...hit ctrl-c again to forcibly terminate\n\n";

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
        surekill();  // ensure we attempt to kill everything
        prte_os_dirpath_destroy(prte_process_info.jobfam_session_dir, true, NULL);
        exit(1);
    }
}

static int prep_singleton(const char *name)
{
    char *ptr, *p1;
    prte_job_t *jdata;
    prte_node_t *node;
    prte_proc_t *proc;
    int rc;
    pmix_rank_t rank;
    prte_app_context_t *app;
    char cwd[PRTE_PATH_MAX];

    ptr = strdup(name);
    p1 = strrchr(ptr, '.');
    *p1 = '\0';
    ++p1;
    rank = strtoul(p1, NULL, 10);
    jdata = PRTE_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, ptr);
    free(ptr);
    rc = prte_set_job_data_object(jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        PRTE_RELEASE(jdata);
        return PRTE_ERR_FATAL;
    }
    /* must have an app */
    app = PRTE_NEW(prte_app_context_t);
    app->app = strdup(jdata->nspace);
    app->num_procs = 1;
    prte_argv_append_nosize(&app->argv, app->app);
    getcwd(cwd, sizeof(cwd));
    app->cwd = strdup(cwd);
    prte_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps = 1;

    /* add a map */
    jdata->map = PRTE_NEW(prte_job_map_t);
    /* add our node to the map since the singleton must
     * be here */
    node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, PRTE_PROC_MY_NAME->rank);
    PRTE_RETAIN(node);
    prte_pointer_array_add(jdata->map->nodes, node);
    ++(jdata->map->num_nodes);

    /* create a proc for the singleton */
    proc = PRTE_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, jdata->nspace, rank);
    proc->rank = proc->name.rank;
    proc->parent = PRTE_PROC_MY_NAME->rank;
    proc->app_idx = 0;
    proc->app_rank = rank;
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->state = PRTE_PROC_STATE_RUNNING;
    /* link it to the job */
    PRTE_RETAIN(jdata);
    proc->job = jdata;
    /* link it to the app */
    PRTE_RETAIN(proc);
    prte_pointer_array_set_item(&app->procs, rank, proc);
    app->first_rank = rank;
    /* link it to the node */
    PRTE_RETAIN(node);
    proc->node = node;
    /* add it to the job */
    prte_pointer_array_set_item(jdata->procs, rank, proc);
    jdata->num_procs = 1;
    jdata->num_local_procs = 1;
    /* add it to the node */
    PRTE_RETAIN(proc);
    prte_pointer_array_add(node->procs, proc);
    node->num_procs = 1;
    node->slots_inuse = 1;

    return PRTE_SUCCESS;
}

static void signal_forward_callback(int signum, short args, void *cbdata)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;

    if (verbose) {
        fprintf(stderr, "%s: Forwarding signal %d to job\n", prte_tool_basename, signum);
    }

    /* send the signal out to the processes */
    PMIX_LOAD_PROCID(&proc, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_SIGNAL, &signum, PMIX_INT);
    rc = PMIx_Job_control(&proc, 1, &info, 1, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Signal %d could not be sent to job %s (returned %s)", signum,
                spawnednspace, PMIx_Error_string(rc));
    }
}

/**
 * Deal with sigpipe errors
 */
static int sigpipe_error_count = 0;
static void epipe_signal_callback(int fd, short args, void *cbdata)
{
    sigpipe_error_count++;

    if (10 < sigpipe_error_count) {
        /* time to abort */
        prte_output(0, "%s: SIGPIPE detected - aborting", prte_tool_basename);
        clean_abort(0, 0, NULL);
    }

    return;
}
