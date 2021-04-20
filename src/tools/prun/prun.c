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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "src/util/fd.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"
#include "src/util/prte_getcwd.h"
#include "src/util/show_help.h"

#include "src/class/prte_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "prun.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"
#include "src/prted/prted.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static pmix_nspace_t spawnednspace;

static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool forcibly_die = false;
static prte_event_t term_handler;
static int term_pipe[2];
static prte_mutex_t prun_abort_inprogress_lock = PRTE_MUTEX_STATIC_INIT;
static bool verbose = false;
static prte_cmd_line_t *prte_cmd_line = NULL;
static prte_list_t forwarded_signals;

static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void signal_forward_callback(int signal);
static void epipe_signal_callback(int signal);

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    evid = ref;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void defhandler(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    size_t n;
    pmix_status_t rc;

    if (verbose) {
        prte_output(0, "PRUN: DEFHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    if (PMIX_ERR_IOF_FAILURE == status) {
        pmix_proc_t target;
        pmix_info_t directive;

        /* tell PRTE to terminate our job */
        PMIX_LOAD_PROCID(&target, prte_process_info.myproc.nspace, PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
        rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        goto progress;
    }

    if (PMIX_ERR_UNREACH == status || PMIX_ERR_LOST_CONNECTION == status) {
        /* we should always have info returned to us - if not, there is
         * nothing we can do */
        if (NULL != info) {
            for (n = 0; n < ninfo; n++) {
                if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                    lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
                }
            }
        }

        if (NULL == lock) {
            exit(1);
        }
        /* save the status */
        lock->status = status;
        /* release the lock */
        PRTE_PMIX_WAKEUP_THREAD(lock);
    }
progress:
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void evhandler(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    int jobstatus = 0;
    pmix_nspace_t jobid = {0};
    size_t n;
    char *msg = NULL;

    if (verbose) {
        prte_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = prte_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PMIX_LOAD_NSPACE(jobid, info[n].value.data.proc->nspace);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
            }
        }
        if (verbose && PMIX_CHECK_NSPACE(jobid, spawnednspace)) {
            prte_output(0, "JOB %s COMPLETED WITH STATUS %d", PRTE_JOBID_PRINT(jobid), jobstatus);
        }
    }
    if (NULL != lock) {
        /* save the status */
        lock->status = jobstatus;
        if (NULL != msg) {
            lock->msg = strdup(msg);
        }
        /* release the lock */
        PRTE_PMIX_WAKEUP_THREAD(lock);
    }

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void debug_cbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t *results, size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
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

    /* release the caller */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }

    PRTE_PMIX_WAKEUP_THREAD(&mylock->lock);
}

static prte_cmd_line_init_t prte_tool_options[] = {
    /* look first for a system server */
    {'\0', "system-server-first", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "First look for a system server and connect to it if found", PRTE_CMD_LINE_OTYPE_DVM},
    /* connect only to a system server */
    {'\0', "system-server-only", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Connect only to a system-level server", PRTE_CMD_LINE_OTYPE_DVM},
    /* do not connect */
    {'\0', "do-not-connect", 0, PRTE_CMD_LINE_TYPE_BOOL, "Do not connect to a server",
     PRTE_CMD_LINE_OTYPE_DVM},
    /* wait to connect */
    {'\0', "wait-to-connect", 0, PRTE_CMD_LINE_TYPE_INT,
     "Delay specified number of seconds before trying to connect", PRTE_CMD_LINE_OTYPE_DVM},
    /* number of times to try to connect */
    {'\0', "num-connect-retries", 0, PRTE_CMD_LINE_TYPE_INT,
     "Max number of times to try to connect", PRTE_CMD_LINE_OTYPE_DVM},
    /* provide a connection PID */
    {'\0', "pid", 1, PRTE_CMD_LINE_TYPE_STRING,
     "PID of the daemon to which we should connect (int => PID or file:<file> for file containing "
     "the PID",
     PRTE_CMD_LINE_OTYPE_DVM},
    /* provide a connection namespace */
    {'\0', "namespace", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Namespace of the daemon to which we should connect", PRTE_CMD_LINE_OTYPE_DVM},
    /* uri of the dvm, or at least where to get it */
    {'\0', "dvm-uri", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Specify the URI of the DVM master, or the name of the file (specified as file:filename) that "
     "contains that info",
     PRTE_CMD_LINE_OTYPE_DVM},
    /* override personality */
    {'\0', "personality", 1, PRTE_CMD_LINE_TYPE_STRING, "Specify the personality to be used",
     PRTE_CMD_LINE_OTYPE_DVM},

    /* End of list */
    {'\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL}};

int prun(int argc, char *argv[])
{
    int rc = 1, i;
    char *param, *ptr;
    prte_pmix_lock_t lock, rellock;
    prte_list_t apps;
    prte_pmix_app_t *app;
    void *tinfo, *jinfo;
    pmix_info_t info, *iptr;
    pmix_proc_t pname;
    pmix_status_t ret;
    bool flag;
    size_t n, ninfo;
    pmix_app_t *papps;
    size_t napps;
    mylock_t mylock;
    prte_value_t *pval;
    uint32_t ui32;
    pid_t pid;
    char **pargv, **targv;
    int pargc;
    char *fullpath;
    prte_ess_base_signal_t *sig;
    prte_event_list_item_t *evitm;
    pmix_value_t *val;
    pmix_data_array_t darray;
    prte_schizo_base_module_t *schizo;
    char hostname[PRTE_PATH_MAX];
    pmix_rank_t rank;
    pmix_status_t code;

    /* init the globals */
    PRTE_CONSTRUCT(&apps, prte_list_t);
    PRTE_CONSTRUCT(&forwarded_signals, prte_list_t);

    /* init the tiny part of PRTE we use */
    prte_init_util(PRTE_PROC_TOOL); // just so we pickup any PRTE params from sys/user files

    fullpath = prte_find_absolute_path(argv[0]);
    prte_tool_basename = prte_basename(argv[0]);
    pargc = argc;
    pargv = prte_argv_copy(argv);
    gethostname(hostname, sizeof(hostname));

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
        fprintf(stderr, "Failed to create pipe\n");
        exit(1);
    }
    /* setup an event to attempt normal termination on signal */
    prte_event_base = prte_progress_thread_init(NULL);
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

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* setup callback for SIGPIPE */
    signal(SIGPIPE, epipe_signal_callback);

    /* because we have to use the schizo framework prior to parsing the
     * incoming argv for cmd line options, do a hacky search to support
     * passing of options (e.g., verbosity) for schizo */
    for (i = 1; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--prtemca") || 0 == strcmp(argv[i], "--mca")) {
            if (0 == strncmp(argv[i + 1], "schizo", 6)) {
                prte_asprintf(&param, "PRTE_MCA_%s", argv[i + 1]);
                prte_setenv(param, argv[i + 2], true, &environ);
                free(param);
                i += 2;
            }
        }
    }

    /* open the SCHIZO framework */
    if (PRTE_SUCCESS
        != (rc = prte_mca_base_framework_open(&prte_schizo_base_framework,
                                              PRTE_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* look for any personality specification */
    ptr = NULL;
    for (i = 0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            ptr = argv[i + 1];
            break;
        }
    }
    if (NULL == ptr) {
        ptr = fullpath;
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo.detect_proxy(ptr);
    if (NULL == schizo) {
        prte_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, fullpath);
        return 1;
    }

    /* setup the cmd line - this is specific to the proxy */
    prte_cmd_line = PRTE_NEW(prte_cmd_line_t);
    if (PRTE_SUCCESS != (rc = schizo->define_cli(prte_cmd_line))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* add the tool-specific options */
    if (PRTE_SUCCESS != (rc = prte_cmd_line_add(prte_cmd_line, prte_tool_options))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* handle deprecated options */
    if (PRTE_SUCCESS != (rc = schizo->parse_deprecated_cli(prte_cmd_line, &pargc, &pargv))) {
        if (PRTE_OPERATION_SUCCEEDED == rc) {
            /* the cmd line was restructured - show them the end result */
            param = prte_argv_join(pargv, ' ');
            fprintf(stderr, "\n******* Corrected cmd line: %s\n\n\n", param);
            free(param);
        } else {
            return rc;
        }
    }

    /* parse the result to get values - this will not include MCA params */
    if (PRTE_SUCCESS != (rc = prte_cmd_line_parse(prte_cmd_line, true, false, pargc, pargv))) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
        }
        return rc;
    }

    /* let the schizo components take a pass at it to get the MCA params - this
     * will include whatever default/user-level param files each schizo component
     * supports */
    if (PRTE_SUCCESS != (rc = schizo->parse_cli(pargc, 0, pargv, NULL))) {
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
        schizo->allow_run_as_root(prte_cmd_line); // will exit us if not allowed
    }

    /* check command line sanity - ensure there aren't multiple instances of
     * options where there should be only one */
    rc = schizo->check_sanity(prte_cmd_line);
    if (PRTE_SUCCESS != rc) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
        }
        param = prte_argv_join(pargv, ' ');
        fprintf(stderr, "\n******* Cmd line: %s\n\n\n", param);
        free(param);
        return rc;
    }

    if (prte_cmd_line_is_taken(prte_cmd_line, "verbose")) {
        verbose = true;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
    if (prte_cmd_line_is_taken(prte_cmd_line, "version")) {
        fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n", prte_tool_basename,
                "PMIx Reference RunTime Environment", PRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* Check for help request */
    if (prte_cmd_line_is_taken(prte_cmd_line, "help")) {
        char *str, *args = NULL;
        args = prte_cmd_line_get_usage_msg(prte_cmd_line, false);
        str = prte_show_help_string("help-prun.txt", "prun:usage", false, prte_tool_basename,
                                    "PRTE", PRTE_VERSION, prte_tool_basename, args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /** setup callbacks for signals we should forward */
    PRTE_CONSTRUCT(&prte_ess_base_signals, prte_list_t);
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "forward-signals", 0, 0))) {
        param = pval->value.data.string;
    } else {
        param = NULL;
    }
    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        return rc;
    }
    PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
    {
        signal(sig->signal, signal_forward_callback);
    }

    /* setup the job data global table */
    prte_job_data = PRTE_NEW(prte_pointer_array_t);
    if (PRTE_SUCCESS
        != (ret = prte_pointer_array_init(prte_job_data, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                          PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                          PRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRTE_ERROR_LOG(ret);
        return rc;
    }

    /* setup options */
    PMIX_INFO_LIST_START(tinfo);

    /* tell PMIx what our name should be */
    if (NULL != (param = getenv("PMIX_NAMESPACE"))) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
    } else {
        prte_asprintf(&param, "%s.%s.%lu", prte_tool_basename, hostname, getpid());
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
        free(param);
    }
    if (NULL != (param = getenv("PMIX_RANK"))) {
        rank = strtoul(param, NULL, 10);
    } else {
        rank = 0;
    }
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);

    if (prte_cmd_line_is_taken(prte_cmd_line, "do-not-connect")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-first")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-only")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    }

    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "wait-to-connect", 0, 0))
        && 0 < pval->value.data.integer) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
    }

    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "num-connect-retries", 0, 0))
        && 0 < pval->value.data.integer) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
    }

    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "pid", 0, 0))) {
        /* see if it is an integer value */
        char *leftover;
        leftover = NULL;
        pid = strtol(pval->value.data.string, &leftover, 10);
        if (NULL == leftover || 0 == strlen(leftover)) {
            /* it is an integer */
            PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else if (0 == strncasecmp(pval->value.data.string, "file", 4)) {
            FILE *fp;
            /* step over the file: prefix */
            param = strchr(pval->value.data.string, ':');
            if (NULL == param) {
                /* malformed input */
                prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                               "--pid", pval->value.data.string, "file:path");
                return PRTE_ERR_BAD_PARAM;
            }
            ++param;
            fp = fopen(param, "r");
            if (NULL == fp) {
                prte_show_help("help-prun.txt", "file-open-error", true, prte_tool_basename,
                               "--pid", pval->value.data.string, param);
                return PRTE_ERR_BAD_PARAM;
            }
            rc = fscanf(fp, "%lu", (unsigned long *) &pid);
            if (1 != rc) {
                /* if we were unable to obtain the single conversion we
                 * require, then error out */
                prte_show_help("help-prun.txt", "bad-file", true, prte_tool_basename,
                               "--pid", pval->value.data.string, param);
                return PRTE_ERR_BAD_PARAM;
            }
            fclose(fp);
            PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else { /* a string that's neither an integer nor starts with 'file:' */
                prte_show_help("help-prun.txt", "bad-option-input", true,
                               prte_tool_basename, "--pid",
                               pval->value.data.string, "file:path");
                return PRTE_ERR_BAD_PARAM;
        }
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "namespace", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_NSPACE, pval->value.data.string, PMIX_STRING);
    }

    /* set our session directory to something hopefully unique so
     * our rendezvous files don't conflict with other prun/prte
     * instances */
    prte_asprintf(&ptr, "%s/%s.session.%s.%lu.%lu", prte_tmp_directory(), prte_tool_basename,
                  prte_process_info.nodename, (unsigned long) geteuid(), (unsigned long) getpid());
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TMPDIR, ptr, PMIX_STRING);
    free(ptr);

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_LAUNCHER, NULL, PMIX_BOOL);

    /* we always support tool rendezvous */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);

    /* if they specified the URI, then pass it along */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "dvm-uri", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_URI, pval->value.data.string, PMIX_STRING);
    }

    /* convert to array of info */
    PMIX_INFO_LIST_CONVERT(ret, tinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(tinfo);

    /* now initialize PMIx */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n",
                prte_tool_basename);
        exit(1);
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* register a default event handler and pass it our release lock
     * so we can cleanly exit if the server goes away */
    PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "DEFAULT", PMIX_STRING);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(NULL, 0, iptr, 2, defhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /***** CONSTRUCT THE APP'S JOB-INFO ****/
    PMIX_INFO_LIST_START(jinfo);

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

    /* we want to be notified upon job completion */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);

    /* pass the personality */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PERSONALITY, schizo->name, PMIX_STRING);

    /* get display options */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "display", 0, 0))) {
        targv = prte_argv_split(pval->value.data.string, ',');

        for (int idx = 0; idx < prte_argv_count(targv); idx++) {
            if (0 == strcmp(targv[idx], "allocation")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYALLOC", PMIX_STRING);
            }
            if (0 == strcmp(targv[idx], "map")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAY", PMIX_STRING);
            }
            if (0 == strcmp(targv[idx], "bind")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, ":REPORT", PMIX_STRING);
            }
            if (0 == strcmp(targv[idx], "map-devel")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYDEVEL", PMIX_STRING);
            }
            if (0 == strcmp(targv[idx], "topo")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYTOPO", PMIX_STRING);
            }
        }
        prte_argv_free(targv);
    }

    /* cannot have both files and directory set for output */
    ptr = NULL;
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "output", 0, 0))) {
        targv = prte_argv_split(pval->value.data.string, ',');

        for (int idx = 0; idx < prte_argv_count(targv); idx++) {
            if (0 == strcmp(targv[idx], "tag")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TAG_OUTPUT, &flag, PMIX_BOOL);
            }
            if (0 == strcmp(targv[idx], "timestamp")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
            }
            if (0 == strcmp(targv[idx], "xml")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":XMLOUTPUT", PMIX_STRING);
            }
            if (0 == strcmp(targv[idx], "merge-stderr-to-stdout")) {
                PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
            }
            if (NULL != (ptr = strchr(targv[idx], ':'))) {
                ++ptr;
                ptr = strdup(ptr);
            }
        }
        prte_argv_free(targv);
    }

    param = NULL;
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "output-filename", 0, 0))) {
        param = pval->value.data.string;
    }

    if (NULL != param && NULL != ptr) {
        prte_show_help("help-prted.txt", "both-file-and-dir-set", true, param, ptr);
        return PRTE_ERR_FATAL;
    } else if (NULL != param) {
        /* if the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!prte_path_is_absolute(param)) {
            char cwd[PRTE_PATH_MAX];
            if (NULL == getcwd(cwd, sizeof(cwd))) {
                return PRTE_ERR_FATAL;
            }
            ptr = prte_os_path(false, cwd, param, NULL);
        } else {
            ptr = strdup(param);
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_OUTPUT_TO_FILE, ptr, PMIX_STRING);
        free(ptr);
    } else if (NULL != ptr) {
        /* If the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!prte_path_is_absolute(ptr)) {
            char cwd[PRTE_PATH_MAX];
            if (NULL == getcwd(cwd, sizeof(cwd))) {
                return PRTE_ERR_FATAL;
            }
            param = prte_os_path(false, cwd, ptr, NULL);
        } else {
            param = strdup(ptr);
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_OUTPUT_TO_DIRECTORY, param, PMIX_STRING);
        free(param);
    }

    /* check what user wants us to do with stdin */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "stdin", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_STDIN_TGT, pval->value.data.string, PMIX_STRING);
    }

    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "map-by", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, pval->value.data.string, PMIX_STRING);
        if (NULL != strcasestr(pval->value.data.string, "DONOTLAUNCH")) {
            PMIX_INFO_LIST_ADD(ret, jinfo, "PRTE_JOB_DO_NOT_LAUNCH", NULL, PMIX_BOOL);
        }
    }

    /* if the user specified a ranking policy, then set it */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "rank-by", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RANKBY, pval->value.data.string, PMIX_STRING);
    }

    /* if the user specified a binding policy, then set it */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "bind-to", 0, 0))) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, pval->value.data.string, PMIX_STRING);
    }

    /* mark if recovery was enabled on the cmd line */
    if (prte_cmd_line_is_taken(prte_cmd_line, "enable-recovery")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
    }
    /* record the max restarts */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "max-restarts", 0, 0))
        && 0 < pval->value.data.integer) {
        ui32 = pval->value.data.integer;
        PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t)
        {
            PMIX_INFO_LIST_ADD(ret, app->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
        }
    }
    /* if continuous operation was specified */
    if (prte_cmd_line_is_taken(prte_cmd_line, "continuous")) {
        /* mark this job as continuously operating */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
    }

    /* if stop-on-exec was specified */
    if (prte_cmd_line_is_taken(prte_cmd_line, "stop-on-exec")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DEBUG_STOP_ON_EXEC, &flag, PMIX_BOOL);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    param = NULL;
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "timeout", 0, 0))
        || NULL != (param = getenv("MPIEXEC_TIMEOUT"))) {
        if (NULL != param) {
            i = strtol(param, NULL, 10);
            /* both cannot be present, or they must agree */
            if (NULL != pval && i != pval->value.data.integer) {
                prte_show_help("help-prun.txt", "prun:timeoutconflict", false, prte_tool_basename,
                               pval->value.data.integer, param);
                exit(1);
            }
        } else {
            i = pval->value.data.integer;
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT, &i, PMIX_INT);
    }

    if (prte_cmd_line_is_taken(prte_cmd_line, "get-stack-traces")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_STACKTRACES, &flag, PMIX_BOOL);
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "report-state-on-timeout")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_REPORT_STATE, &flag, PMIX_BOOL);
    }

    /* give the schizo components a chance to add to the job info */
    prte_schizo.job_info(prte_cmd_line, jinfo);

    /* pickup any relevant envars */
    ninfo = 4;
    PMIX_INFO_CREATE(iptr, ninfo);
    flag = true;
    PMIX_INFO_LOAD(&iptr[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    ui32 = geteuid();
    PMIX_INFO_LOAD(&iptr[1], PMIX_USERID, &ui32, PMIX_UINT32);
    ui32 = getegid();
    PMIX_INFO_LOAD(&iptr[2], PMIX_GRPID, &ui32, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_PERSONALITY, schizo->name, PMIX_STRING);

    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prte_process_info.myproc.nspace, iptr, ninfo, setupcbfunc,
                                        &mylock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        PRTE_UPDATE_EXIT_STATUS(ret);
        goto DONE;
    }
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_INFO_FREE(iptr, ninfo);
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

    /* they want to run an application, so let's parse
     * the cmd line to get it */

    if (PRTE_SUCCESS != (rc = prte_parse_locals(prte_cmd_line, &apps, pargc, pargv, NULL, NULL))) {
        PRTE_ERROR_LOG(rc);
        PRTE_LIST_DESTRUCT(&apps);
        goto DONE;
    }

    /* bozo check */
    if (0 == prte_list_get_size(&apps)) {
        prte_output(0, "No application specified!");
        goto DONE;
    }

    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
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
        papps[n].info = (pmix_info_t *) darray.array;
        papps[n].ninfo = darray.size;
        /* pickup any relevant envars */
        rc = prte_schizo.parse_env(prte_cmd_line, environ, &papps[n].env, false);
        if (PRTE_SUCCESS != rc) {
            goto DONE;
        }
        ++n;
    }
    PRTE_LIST_DESTRUCT(&apps);

    if (verbose) {
        prte_output(0, "Calling PMIx_Spawn");
    }

    ret = PMIx_Spawn(iptr, ninfo, papps, napps, spawnednspace);
    if (PRTE_SUCCESS != ret) {
        prte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        goto DONE;
    }

    /* register to receive the ready-for-debug event - the internal
     * event library can relay it to any tool connected to us */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    code = PMIX_READY_FOR_DEBUG;
    n = 0;
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[n], PMIX_EVENT_HDLR_NAME, "READY-FOR-DEBUG", PMIX_STRING);
    ++n;
    PMIX_LOAD_PROCID(&pname, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&iptr[n], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, iptr, 2, debug_cbfunc, regcbfunc,
                                (void *) &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /* push our stdin to the apps */
    PMIX_LOAD_PROCID(&pname, spawnednspace, 0); // forward stdin to rank=0
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

    /* register to be notified when
     * our job completes */
    ret = PMIX_ERR_JOB_TERMINATED;
    /* setup the info */
    ninfo = 3;
    PMIX_INFO_CREATE(iptr, ninfo);
    /* give the handler a name */
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "JOB_TERMINATION_EVENT", PMIX_STRING);
    /* specify we only want to be notified when our
     * job terminates */
    PMIX_LOAD_PROCID(&pname, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    /* request that they return our lock object */
    PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    /* do the registration */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&ret, 1, iptr, ninfo, evhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (verbose) {
        prte_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(spawnednspace));
    }
    PRTE_PMIX_WAIT_THREAD(&rellock);
    /* save the status */
    rc = rellock.status;
    /* output any message */
    if (NULL != rellock.msg) {
        fprintf(stderr, "%s\n", rellock.msg);
    }

    /* if we lost connection to the server, then we are done */
    if (PMIX_ERR_LOST_CONNECTION == rc || PMIX_ERR_UNREACH == rc) {
        goto DONE;
    }

    /* deregister our event handler */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&rellock);

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
    PRTE_LIST_FOREACH(evitm, &forwarded_signals, prte_event_list_item_t)
    {
        prte_event_signal_del(&evitm->ev);
    }
    PRTE_LIST_DESTRUCT(&forwarded_signals);

    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PMIX_SUCCESS != ret) {
        // Since the user job has probably exited by
        // now, let's preserve its return code and print
        // a warning here, if prte logging is on.
        prte_output(0, "PMIx_tool_finalize() failed. Status = %d", ret);
    }
    return rc;
}

static void clean_abort(int fd, short flags, void *arg)
{
    pmix_proc_t target;
    pmix_info_t directive;
    pmix_status_t rc;

    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prte_mutex_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr,
                "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        return;
    }

    /* tell PRTE to terminate our job */
    PMIX_LOAD_PROCID(&target, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
    rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIx_tool_finalize();
        /* exit with a non-zero status */
        exit(1);
    }
}

static struct timeval current, last = {0, 0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg
        = "Abort is in progress...hit ctrl-c again within 5 seconds to forcibly terminate\n\n";

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
        if (-1 == write(1, (void *) msg, strlen(msg))) {
            exit(1);
        }
    }
    /* save the time */
    last.tv_sec = current.tv_sec;
    /* tell the event lib to attempt to abnormally terminate */
    if (-1 == write(term_pipe[1], &foo, 1)) {
        exit(1);
    }
}

static void signal_forward_callback(int signum)
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
static void epipe_signal_callback(int signal)
{
    sigpipe_error_count++;

    if (10 < sigpipe_error_count) {
        /* time to abort */
        prte_output(0, "%s: SIGPIPE detected - aborting", prte_tool_basename);
        clean_abort(0, 0, NULL);
    }

    return;
}
