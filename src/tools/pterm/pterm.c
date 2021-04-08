/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
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
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"
#include "src/util/prte_getcwd.h"
#include "src/util/show_help.h"

#include "src/class/prte_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/util/os_path.h"
#include "src/util/path.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static prte_list_t job_info;
static pmix_nspace_t myjobid = {0};

static pmix_proc_t myproc;
static bool forcibly_die = false;
static prte_event_t term_handler;
static int term_pipe[2];
static prte_mutex_t prun_abort_inprogress_lock = PRTE_MUTEX_STATIC_INIT;
static prte_event_base_t *myevbase = NULL;
static bool proxyrun = false;
static bool verbose = false;
static prte_cmd_line_t *prte_cmd_line = NULL;

/* prun-specific options */
static prte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" generalized options */
    {'h', "help", 0, PRTE_CMD_LINE_TYPE_BOOL, "This help message", PRTE_CMD_LINE_OTYPE_GENERAL},
    {'V', "version", 0, PRTE_CMD_LINE_TYPE_BOOL, "Print version and exit",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'v', "verbose", 0, PRTE_CMD_LINE_TYPE_BOOL, "Be verbose", PRTE_CMD_LINE_OTYPE_GENERAL},

    /* look first for a system server */
    {'\0', "system-server-first", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "First look for a system server and connect to it if found", PRTE_CMD_LINE_OTYPE_DVM},
    /* connect only to a system server */
    {'\0', "system-server-only", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Connect only to a system-level server", PRTE_CMD_LINE_OTYPE_DVM},
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
    /* uri of the dvm, or at least where to get it */
    {'\0', "dvm-uri", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Specify the URI of the DVM master, or the name of the file (specified as file:filename) that "
     "contains that info",
     PRTE_CMD_LINE_OTYPE_DVM},

    /* End of list */
    {'\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL}};

static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);

static void infocb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                   pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    /* The callback should likely not have been called
     * see the comment below */
    if (PMIX_ERR_COMM_FAILURE == status) {
        return;
    }
#endif
    PRTE_ACQUIRE_OBJECT(lock);

    if (verbose) {
        prte_output(0, "PRUN: INFOCB");
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
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
        if (verbose && PMIX_CHECK_NSPACE(jobid, myjobid)) {
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

int main(int argc, char *argv[])
{
    int rc = PRTE_ERR_FATAL;
    prte_pmix_lock_t lock, rellock;
    pmix_info_t info, *iptr;
    pmix_status_t ret;
    bool flag;
    size_t ninfo;
    prte_value_t *pval;
    uint32_t ui32;
    char *param;
    pid_t pid;
    void *tinfo;
    pmix_data_array_t darray;
    char hostname[PRTE_PATH_MAX];
    pmix_rank_t rank;

    /* init the globals */
    PRTE_CONSTRUCT(&job_info, prte_list_t);

    /* init the tiny part of PRTE we use */
    prte_init_util(PRTE_PROC_MASTER);

    prte_tool_basename = prte_basename(argv[0]);
    gethostname(hostname, sizeof(hostname));

    /* setup our cmd line */
    prte_cmd_line = PRTE_NEW(prte_cmd_line_t);
    if (PRTE_SUCCESS != (rc = prte_cmd_line_add(prte_cmd_line, cmd_line_init))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* parse the result to get values - this will not include MCA params */
    if (PRTE_SUCCESS != (rc = prte_cmd_line_parse(prte_cmd_line, true, false, argc, argv))) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], prte_strerror(rc));
        }
        PRTE_ERROR_LOG(rc);
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
        str = prte_show_help_string("help-pterm.txt", "usage", false, prte_tool_basename, "PRTE",
                                    PRTE_VERSION, prte_tool_basename, args, PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* setup options */
    PMIX_INFO_LIST_START(tinfo);

    /* tell PMIx what our name should be */
    prte_asprintf(&param, "%s.%s.%lu", prte_tool_basename, hostname, getpid());
    PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
    free(param);
    rank = 0;
    PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);

    if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-first")) {
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-only")) {
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "wait-to-connect", 0, 0))
        && 0 < pval->value.data.integer) {
        ui32 = pval->value.data.integer;
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "num-connect-retries", 0, 0))
        && 0 < pval->value.data.integer) {
        ui32 = pval->value.data.integer;
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "pid", 0, 0))) {
        /* see if it is an integer value */
        char *leftover;
        leftover = NULL;
        pid = strtol(pval->value.data.string, &leftover, 10);
        if (NULL == leftover || 0 == strlen(leftover)) {
            /* it is an integer */
            PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
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
            PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        }
    }

    /* if they specified the URI, then pass it along */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "dvm-uri", 0, 0))) {
        PMIX_INFO_LIST_ADD(rc, tinfo, PMIX_SERVER_URI, pval->value.data.string, PMIX_STRING);
    }

    /* convert to array of info */
    PMIX_INFO_LIST_CONVERT(rc, tinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(tinfo);

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
    myevbase = prte_progress_thread_init(NULL);
    prte_event_set(myevbase, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
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

    /* now initialize PMIx - we have to indicate we are a launcher so that we
     * will provide rendezvous points for tools to connect to us */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n",
                prte_tool_basename);
        exit(1);
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* setup a lock to track the connection */
    PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    /* register to trap connection loss */
    pmix_status_t code[2] = {PMIX_ERR_UNREACH, PMIX_ERR_LOST_CONNECTION};
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIx_Register_event_handler(code, 2, &info, 1, evhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_TERMINATE, &flag, PMIX_BOOL);
    if (!proxyrun) {
        fprintf(stderr, "TERMINATING DVM...");
    }
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    rc = PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void *) &lock);
    if (PMIX_SUCCESS == rc) {
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
         * being called when the server terminates. The callback might be eventually
         * called though then the connection to the server closes with
         * status PMIX_ERR_COMM_FAILURE */
        poll(NULL, 0, 1000);
        infocb(PMIX_SUCCESS, NULL, 0, (void *) &lock, NULL, NULL);
#endif
        PRTE_PMIX_WAIT_THREAD(&lock);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        /* wait for connection to depart */
        PRTE_PMIX_WAIT_THREAD(&rellock);
        PRTE_PMIX_DESTRUCT_LOCK(&rellock);
    } else {
        PRTE_PMIX_WAIT_THREAD(&lock);
        PRTE_PMIX_DESTRUCT_LOCK(&rellock);
    }
    /* wait for the connection to go away */
    fprintf(stderr, "DONE\n");
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    return rc;
#endif

    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PRTE_SUCCESS == rc && PMIX_SUCCESS != ret) {
        rc = ret;
    }
    return rc;
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
                "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        PMIx_tool_finalize();
        return;
    }
}

static struct timeval current, last = {0, 0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been abprted.
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
