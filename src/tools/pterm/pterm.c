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
 * Copyright (c) 2006-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/include/constants.h"
#include "src/include/version.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif  /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif  /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/base/base.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/basename.h"
#include "src/util/cmd_line.h"
#include "src/util/prrte_environ.h"
#include "src/util/prrte_getcwd.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/util/fd.h"
#include "src/sys/atomic.h"

#include "src/runtime/prrte_progress_threads.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"

typedef struct {
    prrte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static prrte_list_t job_info;
static prrte_jobid_t myjobid = PRRTE_JOBID_INVALID;

static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool forcibly_die=false;
static prrte_event_t term_handler;
static int term_pipe[2];
static prrte_atomic_lock_t prun_abort_inprogress_lock = PRRTE_ATOMIC_LOCK_INIT;
static prrte_event_base_t *myevbase = NULL;
static bool proxyrun = false;
static bool verbose = false;
static prrte_cmd_line_t *prrte_cmd_line = NULL;

/* prun-specific options */
static prrte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" generalized options */
    { 'h', "help", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "This help message", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'V', "version", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Print version and exit", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'v', "verbose", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Be verbose", PRRTE_CMD_LINE_OTYPE_GENERAL },

    /* look first for a system server */
    { '\0', "system-server-first", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "First look for a system server and connect to it if found",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* connect only to a system server */
    { '\0', "system-server-only", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Connect only to a system-level server",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* wait to connect */
    { '\0', "wait-to-connect", 0, PRRTE_CMD_LINE_TYPE_INT,
      "Delay specified number of seconds before trying to connect",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* number of times to try to connect */
    { '\0', "num-connect-retries", 0, PRRTE_CMD_LINE_TYPE_INT,
      "Max number of times to try to connect",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* provide a connection PID */
    { '\0', "pid", 1, PRRTE_CMD_LINE_TYPE_INT,
      "PID of the session-level daemon to which we should connect",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* uri of the dvm, or at least where to get it */
    { '\0', "dvm-uri", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the DVM master, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },

   /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};


static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);

static void infocb(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    /* The callback should likely not have been called
     * see the comment below */
    if (PMIX_ERR_COMM_FAILURE == status) {
        return;
    }
#endif
    PRRTE_ACQUIRE_OBJECT(lock);

    if (verbose) {
        prrte_output(0, "PRUN: INFOCB");
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_ACQUIRE_OBJECT(lock);
    evid = ref;
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

static void evhandler(size_t evhdlr_registration_id,
                      pmix_status_t status,
                      const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc,
                      void *cbdata)
{
    prrte_pmix_lock_t *lock = NULL;
    int jobstatus=0, rc;
    prrte_jobid_t jobid = PRRTE_JOBID_INVALID;
    size_t n;
    char *msg = NULL;

    if (verbose) {
        prrte_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = prrte_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PRRTE_PMIX_CONVERT_NSPACE(rc, &jobid, info[n].value.data.proc->nspace);
                if (PRRTE_SUCCESS != rc) {
                    PRRTE_ERROR_LOG(rc);
                }
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (prrte_pmix_lock_t*)info[n].value.data.ptr;
        #ifdef PMIX_EVENT_TEXT_MESSAGE
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
        #endif
            }
        }
        if (verbose && (myjobid != PRRTE_JOBID_INVALID && jobid == myjobid)) {
            prrte_output(0, "JOB %s COMPLETED WITH STATUS %d",
                        PRRTE_JOBID_PRINT(jobid), jobstatus);
        }
    }
    /* save the status */
    lock->status = jobstatus;
    if (NULL != msg) {
        lock->msg = strdup(msg);
    }
    /* release the lock */
    PRRTE_PMIX_WAKEUP_THREAD(lock);

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

int main(int argc, char *argv[])
{
    int rc=PRRTE_ERR_FATAL;
    prrte_pmix_lock_t lock, rellock;
    prrte_list_t tinfo;
    pmix_info_t info, *iptr;
    pmix_status_t ret;
    bool flag;
    prrte_ds_info_t *ds;
    size_t n, ninfo;
    prrte_value_t *pval;
    uint32_t ui32;
    pid_t pid;

    /* init the globals */
    PRRTE_CONSTRUCT(&job_info, prrte_list_t);

    prrte_atomic_lock_init(&prun_abort_inprogress_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);
    /* init the tiny part of PRRTE we use */
    prrte_init_util();

    prrte_tool_basename = prrte_basename(argv[0]);

    /* setup our cmd line */
    prrte_cmd_line = PRRTE_NEW(prrte_cmd_line_t);
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_add(prrte_cmd_line, cmd_line_init))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* parse the result to get values - this will not include MCA params */
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_parse(prrte_cmd_line,
                                                    true, false, argc, argv)) ) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(rc));
        }
         PRRTE_ERROR_LOG(rc);
       return rc;
    }

    if (prrte_cmd_line_is_taken(prrte_cmd_line, "verbose")) {
        verbose = true;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
     if (prrte_cmd_line_is_taken(prrte_cmd_line, "version")) {
        fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                prrte_tool_basename, "PMIx Reference RunTime Environment",
                PRRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* Check for help request */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "help")) {
        char *str, *args = NULL;
        args = prrte_cmd_line_get_usage_msg(prrte_cmd_line, false);
        str = prrte_show_help_string("help-pterm.txt", "usage", false,
                                    prrte_tool_basename, "PRRTE", PRRTE_VERSION,
                                    prrte_tool_basename, args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* setup options */
    PRRTE_CONSTRUCT(&tinfo, prrte_list_t);
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "system-server-first")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    } else if (prrte_cmd_line_is_taken(prrte_cmd_line, "system-server-only")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "wait-to-connect", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        ui32 = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
        prrte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "num-connect-retries", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        ui32 = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
        prrte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "pid", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        pid = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        prrte_list_append(&tinfo, &ds->super);
    }
    /* ensure we don't try to use the usock PTL component */
    ds = PRRTE_NEW(prrte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_USOCK_DISABLE, NULL, PMIX_BOOL);
    prrte_list_append(&tinfo, &ds->super);

    /* if they specified the URI, then pass it along */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "dvm-uri", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_URI, pval->data.string, PMIX_STRING);
        prrte_list_append(&tinfo, &ds->super);
    }

    /* convert to array of info */
    ninfo = prrte_list_get_size(&tinfo);
    PMIX_INFO_CREATE(iptr, ninfo);
    n = 0;
    PRRTE_LIST_FOREACH(ds, &tinfo, prrte_ds_info_t) {
        PMIX_INFO_XFER(&iptr[n], ds->info);
        ++n;
    }
    PRRTE_LIST_DESTRUCT(&tinfo);

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
    myevbase = prrte_progress_thread_init(NULL);
    prrte_event_set(myevbase, &term_handler, term_pipe[0], PRRTE_EV_READ, clean_abort, NULL);
    prrte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
       children inherit it */
    if (prrte_fd_set_cloexec(term_pipe[0]) != PRRTE_SUCCESS ||
        prrte_fd_set_cloexec(term_pipe[1]) != PRRTE_SUCCESS) {
        fprintf(stderr, "unable to set the pipe to CLOEXEC\n");
        prrte_progress_thread_finalize(NULL);
        exit(1);
    }

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* now initialize PMIx - we have to indicate we are a launcher so that we
     * will provide rendezvous points for tools to connect to us */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n", prrte_tool_basename);
        exit(1);
    }
    PMIX_INFO_FREE(iptr, ninfo);


     /* setup a lock to track the connection */
    PRRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    /* register to trap connection loss */
    pmix_status_t code[2] = {PMIX_ERR_UNREACH, PMIX_ERR_LOST_CONNECTION_TO_SERVER};
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIx_Register_event_handler(code, 2, &info, 1,
                                evhandler, regcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_TERMINATE, &flag, PMIX_BOOL);
    if (!proxyrun) {
        fprintf(stderr, "TERMINATING DVM...");
    }
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    rc = PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
    if (PMIX_SUCCESS == rc) {
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
         * being called when the server terminates. The callback might be eventually
         * called though then the connection to the server closes with
         * status PMIX_ERR_COMM_FAILURE */
        poll(NULL, 0, 1000);
        infocb(PMIX_SUCCESS, NULL, 0, (void *)&lock, NULL, NULL);
#endif
        PRRTE_PMIX_WAIT_THREAD(&lock);
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
        /* wait for connection to depart */
        PRRTE_PMIX_WAIT_THREAD(&rellock);
        PRRTE_PMIX_DESTRUCT_LOCK(&rellock);
    } else {
        PRRTE_PMIX_WAIT_THREAD(&lock);
        PRRTE_PMIX_DESTRUCT_LOCK(&rellock);
    }
    /* wait for the connection to go away */
    fprintf(stderr, "DONE\n");
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    return rc;
#endif

    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PRRTE_SUCCESS == rc && PMIX_SUCCESS != ret) {
        rc = ret;
    }
    return rc;
}

static void clean_abort(int fd, short flags, void *arg)
{
    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prrte_atomic_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr, "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prrte_event_add(&term_handler, NULL);
        PMIx_tool_finalize();
        return;
    }
}

static struct timeval current, last={0,0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been abprrted.
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
        if (-1 == write(1, (void*)msg, strlen(msg))) {
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
