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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
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
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/base/base.h"
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
#include "src/sys/atomic.h"

#include "src/runtime/prte_progress_threads.h"
#include "src/class/prte_pointer_array.h"
#include "src/dss/dss.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"

#include "prun.h"

typedef struct {
    prte_object_t super;
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} myinfo_t;
static void mcon(myinfo_t *p)
{
    PRTE_PMIX_CONSTRUCT_LOCK(&p->lock);
    p->info = NULL;
    p->ninfo = 0;
}
static void mdes(myinfo_t *p)
{
    PRTE_PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
static PRTE_CLASS_INSTANCE(myinfo_t, prte_object_t,
                            mcon, mdes);

typedef struct {
    prte_list_item_t super;
    pmix_app_t app;
    prte_list_t info;
} prte_pmix_app_t;
static void acon(prte_pmix_app_t *p)
{
    PMIX_APP_CONSTRUCT(&p->app);
    PRTE_CONSTRUCT(&p->info, prte_list_t);
}
static void ades(prte_pmix_app_t *p)
{
    PMIX_APP_DESTRUCT(&p->app);
    PRTE_LIST_DESTRUCT(&p->info);
}
static PRTE_CLASS_INSTANCE(prte_pmix_app_t,
                          prte_list_item_t,
                          acon, ades);

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static prte_list_t job_info;
static prte_jobid_t myjobid = PRTE_JOBID_INVALID;
static myinfo_t myinfo;
static pmix_nspace_t spawnednspace;

static int create_app(int argc, char* argv[],
                      prte_list_t *jdata,
                      prte_pmix_app_t **app,
                      bool *made_app, char ***app_env);
static int parse_locals(prte_list_t *jdata, int argc, char* argv[]);
static void set_classpath_jar_file(prte_pmix_app_t *app, int index, char *jarfile);
static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool forcibly_die=false;
static prte_event_t term_handler;
static int term_pipe[2];
static prte_atomic_lock_t prun_abort_inprogress_lock = PRTE_ATOMIC_LOCK_INIT;
static bool verbose = false;
static prte_cmd_line_t *prte_cmd_line = NULL;
static prte_list_t forwarded_signals;

/* prun-specific options */
static prte_cmd_line_init_t cmd_line_init[] = {

    /* DVM options */
    /* tell the dvm to terminate */
    { '\0', "terminate", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Terminate the DVM", PRTE_CMD_LINE_OTYPE_DVM },
    /* look first for a system server */
    { '\0', "system-server-first", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "First look for a system server and connect to it if found",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* connect only to a system server */
    { '\0', "system-server-only", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Connect only to a system-level server",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* do not connect */
    { '\0', "do-not-connect", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Do not connect to a server",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* wait to connect */
    { '\0', "wait-to-connect", 0, PRTE_CMD_LINE_TYPE_INT,
      "Delay specified number of seconds before trying to connect",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* number of times to try to connect */
    { '\0', "num-connect-retries", 0, PRTE_CMD_LINE_TYPE_INT,
      "Max number of times to try to connect",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* provide a connection PID */
    { '\0', "pid", 1, PRTE_CMD_LINE_TYPE_INT,
      "PID of the daemon to which we should connect",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* provide a connection namespace */
    { '\0', "namespace", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Namespace of the daemon to which we should connect",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* uri of the dvm, or at least where to get it */
    { '\0', "dvm-uri", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the DVM master, or the name of the file (specified as file:filename) that contains that info",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* forward signals */
    { '\0', "forward-signals", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of additional signals (names or integers) to forward to "
      "application processes [\"none\" => forward nothing]. Signals provided by "
      "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
      PRTE_CMD_LINE_OTYPE_DVM},


    /* testing options */
    { '\0', "timeout", 1, PRTE_CMD_LINE_TYPE_INT,
      "Timeout the job after the specified number of seconds",
      PRTE_CMD_LINE_OTYPE_DEBUG },
#if PMIX_NUMERIC_VERSION >= 0x00040000
    { '\0', "report-state-on-timeout", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Report all job and process states upon timeout",
      PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "get-stack-traces", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Get stack traces of all application procs on timeout",
      PRTE_CMD_LINE_OTYPE_DEBUG },
#endif

    /* Conventional options - for historical compatibility, support
     * both single and multi dash versions */
    /* Number of processes; -c, -n, --n, -np, and --np are all
       synonyms */
    { 'c', "np", 1, PRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run",
      PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'n', "n", 1, PRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run",
      PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'N', NULL, 1, PRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run per node",
      PRTE_CMD_LINE_OTYPE_GENERAL },
    /* Use an appfile */
    { '\0',  "app", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Provide an appfile; ignore all other command line options",
      PRTE_CMD_LINE_OTYPE_GENERAL },


      /* Output options */
    /* exit status reporting */
    { '\0', "report-child-jobs-separately", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Return the exit status of the primary job only",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    /* select XML output */
    { '\0', "xml", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Provide all output in XML format",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "xml-file", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Provide all output in XML format to the specified file",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    /* tag output */
    { '\0', "tag-output", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Tag all output with [job,rank]",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "timestamp-output", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Timestamp all application process output",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-directory", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename/job/rank/std[out,err,diag]. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOJOBID (do not include a job-id directory level) and NOCOPY (do not copy the output to the stdout/err streams)",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-filename", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename.rank. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOCOPY (do not copy the output to the stdout/err streams)",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "merge-stderr-to-stdout", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Merge stderr to stdout for each process",
      PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "xterm", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Create a new xterm window and display output from the specified ranks there",
      PRTE_CMD_LINE_OTYPE_OUTPUT },

    /* select stdin option */
    { '\0', "stdin", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Specify procs to receive stdin [rank, all, none] (default: 0, indicating rank 0)",
      PRTE_CMD_LINE_OTYPE_INPUT },


    /* User-level debugger arguments */
    { '\0', "output-proctable", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Print the complete proctable to stdout [-], stderr [+], or a file [anything else] after launch",
      PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "stop-on-exec", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "If supported, stop each process at start of execution",
      PRTE_CMD_LINE_OTYPE_DEBUG },


    /* Launch options */
    /* request that argv[0] be indexed */
    { '\0', "index-argv-by-rank", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Uniquely index argv[0] for each process using its rank",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload the binary on the remote machine */
    { 's', "preload-binary", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Preload the binary on the remote machine before starting the remote process.",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload files on the remote machine */
    { '\0', "preload-files", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Preload the comma separated list of files to the remote machines current working directory before starting the remote process.",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Export environment variables; potentially used multiple times,
       so it does not make sense to set into a variable */
    { 'x', NULL, 1, PRTE_CMD_LINE_TYPE_STRING,
      "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the environment variable foo and takes its value from the current environment; \"-x foo=bar\" exports the environment variable name foo and sets its value to \"bar\" in the started processes; \"-x foo*\" exports all current environmental variables starting with \"foo\")",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wdir", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Set the working directory of the started processes",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wd", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Synonym for --wdir",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "set-cwd-to-session-dir", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Set the working directory of the started processes to their session directory",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "path", 1, PRTE_CMD_LINE_TYPE_STRING,
      "PATH to be used to look for executables to start processes",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "show-progress", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Output a brief periodic report on launch progress",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "pset", 1, PRTE_CMD_LINE_TYPE_STRING,
      "User-specified name assigned to the processes in their given application",
      PRTE_CMD_LINE_OTYPE_LAUNCH },



    /* Developer options */
    { '\0', "do-not-resolve", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Do not attempt to resolve interfaces - usually used to determine proposed process placement/binding prior to obtaining an allocation",
      PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "do-not-launch", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Perform all necessary operations to prepare to launch the application, but do not actually launch it (usually used to test mapping patterns)",
      PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
       "Display a detailed process map (mostly intended for developers) just before launch",
       PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-topo", 0, PRTE_CMD_LINE_TYPE_BOOL,
       "Display the topology as part of the process map (mostly intended for developers) just before launch",
       PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-diffable-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
       "Display a diffable process map (mostly intended for developers) just before launch",
       PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "report-bindings", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Whether to report process bindings to stderr",
      PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Display a detailed list (mostly intended for developers) of the allocation being used by this job",
      PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Display the process map just before launch",
      PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "display-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Display the allocation being used by this job",
      PRTE_CMD_LINE_OTYPE_DEBUG },


    /* Mapping options */
    { '\0', "map-by", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Mapping Policy for job [slot | hwthread | core (default:np<=2) | l1cache | "
      "l2cache | l3cache | socket (default:np>2) | node | seq | dist | ppr],"
      " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
      "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
      "DEVICE(for dist policy), INHERIT, NOINHERIT, PE-LIST=a,b (comma-delimited "
      "ranges of cpus to use for this job)",
      PRTE_CMD_LINE_OTYPE_MAPPING },


      /* Ranking options */
    { '\0', "rank-by", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Ranking Policy for job [slot (default:np<=2) | hwthread | core | l1cache "
      "| l2cache | l3cache | socket (default:np>2) | node], with modifier :SPAN or :FILL",
      PRTE_CMD_LINE_OTYPE_RANKING },


      /* Binding options */
    { '\0', "bind-to", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Binding policy for job. Allowed values: none, hwthread, core, l1cache, l2cache, "
      "l3cache, socket, (\"none\" is the default when oversubscribed, \"core\" is "
      "the default when np<=2, and \"socket\" is the default when np>2). Allowed colon-delimited qualifiers: "
      "overload-allowed, if-supported",
      PRTE_CMD_LINE_OTYPE_BINDING },


    /* Fault Tolerance options */
    { '\0', "enable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Enable recovery from process failure [Default = disabled]",
      PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "max-restarts", 1, PRTE_CMD_LINE_TYPE_INT,
      "Max number of times to restart a failed process",
      PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "disable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Disable recovery (resets all recovery options to off)",
      PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "continuous", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Job is to run until explicitly terminated",
      PRTE_CMD_LINE_OTYPE_FT },


    /* End of list */
    { '\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL }
};


static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void signal_forward_callback(int signal);
static void epipe_signal_callback(int signal);

static void infocb(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
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
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    evid = ref;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
    PRTE_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void defhandler(size_t evhdlr_registration_id,
                       pmix_status_t status,
                       const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    size_t n;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    pmix_status_t rc;
#endif

    if (verbose) {
        prte_output(0, "PRUN: DEFHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
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
#endif

    if (PMIX_ERR_UNREACH == status ||
        PMIX_ERR_LOST_CONNECTION_TO_SERVER == status) {
        /* we should always have info returned to us - if not, there is
         * nothing we can do */
        if (NULL != info) {
            for (n=0; n < ninfo; n++) {
                if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                    lock = (prte_pmix_lock_t*)info[n].value.data.ptr;
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
#if PMIX_NUMERIC_VERSION >= 0x00040000
  progress:
#endif
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}


static void evhandler(size_t evhdlr_registration_id,
                      pmix_status_t status,
                      const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc,
                      void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    int jobstatus=0, rc;
    prte_jobid_t jobid = PRTE_JOBID_INVALID;
    size_t n;
    char *msg = NULL;

    if (verbose) {
        prte_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = prte_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PRTE_PMIX_CONVERT_NSPACE(rc, &jobid, info[n].value.data.proc->nspace);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                }
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (prte_pmix_lock_t*)info[n].value.data.ptr;
        #ifdef PMIX_EVENT_TEXT_MESSAGE
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
        #endif
            }
        }
        if (verbose && (myjobid != PRTE_JOBID_INVALID && jobid == myjobid)) {
            prte_output(0, "JOB %s COMPLETED WITH STATUS %d",
                        PRTE_JOBID_PRINT(jobid), jobstatus);
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

static void setupcbfunc(pmix_status_t status,
                        pmix_info_t info[], size_t ninfo,
                        void *provided_cbdata,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    mylock_t *mylock = (mylock_t*)provided_cbdata;
    size_t n;

    if (NULL != info) {
        mylock->ninfo = ninfo;
        PMIX_INFO_CREATE(mylock->info, mylock->ninfo);
        /* cycle across the provided info */
        for (n=0; n < ninfo; n++) {
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

static void launchhandler(size_t evhdlr_registration_id,
                          pmix_status_t status,
                          const pmix_proc_t *source,
                          pmix_info_t info[], size_t ninfo,
                          pmix_info_t *results, size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc,
                          void *cbdata)
{
    size_t n;

    /* the info list will include the launch directives, so
     * transfer those to the myinfo_t for return to the main thread */
    if (NULL != info) {
        myinfo.ninfo = ninfo;
        PMIX_INFO_CREATE(myinfo.info, myinfo.ninfo);
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&myinfo.info[n], &info[n]);
        }
    }

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }

    /* now release the thread */
    PRTE_PMIX_WAKEUP_THREAD(&myinfo.lock);
}

int prun(int argc, char *argv[])
{
    int rc=1, i;
    char *param, *ptr;
    prte_pmix_lock_t lock, rellock;
    prte_list_t apps;
    prte_pmix_app_t *app;
    prte_list_t tinfo;
    pmix_info_t info, *iptr;
    pmix_proc_t pname, controller;
    pmix_status_t ret;
    bool flag;
    prte_ds_info_t *ds;
    size_t m, n, ninfo;
    pmix_app_t *papps;
    size_t napps;
    mylock_t mylock;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    bool notify_launch = false;
#endif
    prte_value_t *pval;
    uint32_t ui32;
    pid_t pid;
    char *mytmpdir;
    char **pargv;
    int pargc;
    char **tmp;
    prte_ess_base_signal_t *sig;
    prte_event_list_item_t *evitm;
    char *personality = NULL;

    /* init the globals */
    PRTE_CONSTRUCT(&job_info, prte_list_t);
    PRTE_CONSTRUCT(&apps, prte_list_t);
    PRTE_CONSTRUCT(&forwarded_signals, prte_list_t);

    prte_atomic_lock_init(&prun_abort_inprogress_lock, PRTE_ATOMIC_LOCK_UNLOCKED);
    /* init the tiny part of PRTE we use */
    prte_init_util(PRTE_PROC_MASTER);  // just so we pickup any PRTE params from sys/user files

    prte_tool_basename = prte_basename(argv[0]);
    pargc = argc;
    pargv = prte_argv_copy(argv);

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
    prte_event_base = prte_progress_thread_init(NULL);
    prte_event_set(prte_event_base, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
    prte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
       children inherit it */
    if (prte_fd_set_cloexec(term_pipe[0]) != PRTE_SUCCESS ||
        prte_fd_set_cloexec(term_pipe[1]) != PRTE_SUCCESS) {
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
    for (i=1; NULL != argv[i]; i++) {
        if (0 == strncmp(argv[i], "schizo", 6)) {
            prte_asprintf(&param, "PRTE_MCA_%s", argv[i]);
            prte_setenv(param, argv[i+1], true, &environ);
            free(param);
        }
    }

    /* setup our cmd line */
    prte_cmd_line = PRTE_NEW(prte_cmd_line_t);
    if (PRTE_SUCCESS != (rc = prte_cmd_line_add(prte_cmd_line, cmd_line_init))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* open the SCHIZO framework */
    if (PRTE_SUCCESS != (rc = prte_mca_base_framework_open(&prte_schizo_base_framework, 0))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* setup our common personalities */
    prte_argv_append_unique_nosize(&prte_schizo_base.personalities, "prte");
    prte_argv_append_unique_nosize(&prte_schizo_base.personalities, "pmix");
    /* add anything they specified */
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            tmp = prte_argv_split(argv[i+1], ',');
            for (m=0; NULL != tmp[m]; m++) {
                prte_argv_append_unique_nosize(&prte_schizo_base.personalities, tmp[m]);
            }
            prte_argv_free(tmp);
        }
    }

    /* get our session directory */
    if (PRTE_SUCCESS != (rc = prte_schizo.define_session_dir(&mytmpdir))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* setup the rest of the cmd line only once */
    if (PRTE_SUCCESS != (rc = prte_schizo.define_cli(prte_cmd_line))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* handle deprecated options */
    if (PRTE_SUCCESS != (rc = prte_schizo.parse_deprecated_cli(prte_cmd_line, &pargc, &pargv))) {
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
    if (PRTE_SUCCESS != (rc = prte_cmd_line_parse(prte_cmd_line,
                                                    true, false, pargc, pargv)) ) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n",
                    prte_tool_basename,
                    prte_strerror(rc));
        }
       return rc;
    }

    /* let the schizo components take a pass at it to get the MCA params - this
     * will include whatever default/user-level param files each schizo component
     * supports */
    if (PRTE_SUCCESS != (rc = prte_schizo.parse_cli(pargc, 0, pargv, NULL, NULL))) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n",
                    prte_tool_basename,
                    prte_strerror(rc));
        }
        return rc;
    }

    if (prte_cmd_line_is_taken(prte_cmd_line, "verbose")) {
        verbose = true;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
     if (prte_cmd_line_is_taken(prte_cmd_line, "version")) {
        fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                prte_tool_basename, "PMIx Reference RunTime Environment",
                PRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* Check for help request */
    if (prte_cmd_line_is_taken(prte_cmd_line, "help")) {
        char *str, *args = NULL;
        args = prte_cmd_line_get_usage_msg(prte_cmd_line, false);
        str = prte_show_help_string("help-prun.txt", "prun:usage", false,
                                    prte_tool_basename, "PRTE", PRTE_VERSION,
                                    prte_tool_basename, args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid()) {
        prte_schizo.allow_run_as_root(prte_cmd_line);  // will exit us if not allowed
    }

    /** setup callbacks for signals we should forward */
    PRTE_CONSTRUCT(&prte_ess_base_signals, prte_list_t);
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "forward-signals", 0, 0))) {
        param = pval->data.string;
    } else {
        param = NULL;
    }
    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        return rc;
    }
    PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t) {
        signal(sig->signal, signal_forward_callback);
    }

    /* setup the job data global table */
    prte_job_data = PRTE_NEW(prte_hash_table_t);
    if (PRTE_SUCCESS != (ret = prte_hash_table_init(prte_job_data, 128))) {
        PRTE_ERROR_LOG(ret);
        return rc;
    }

    /* setup options */
    PRTE_CONSTRUCT(&tinfo, prte_list_t);
    if (prte_cmd_line_is_taken(prte_cmd_line, "do-not-connect")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-first")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "system-server-only")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "wait-to-connect", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        ui32 = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
        prte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "num-connect-retries", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        ui32 = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
        prte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "pid", 0, 0)) &&
        0 < pval->data.integer) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        pid = pval->data.integer;
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        prte_list_append(&tinfo, &ds->super);
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "namespace", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_NSPACE, pval->data.string, PMIX_STRING);
        prte_list_append(&tinfo, &ds->super);
    }
    /* ensure we don't try to use the usock PTL component */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_USOCK_DISABLE, NULL, PMIX_BOOL);
    prte_list_append(&tinfo, &ds->super);

    /* set our session directory to something hopefully unique so
     * our rendezvous files don't conflict with other prun/prte
     * instances */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SERVER_TMPDIR, mytmpdir, PMIX_STRING);
    prte_list_append(&tinfo, &ds->super);

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_LAUNCHER, NULL, PMIX_BOOL);
    prte_list_append(&tinfo, &ds->super);
    /* we always support session-level rendezvous */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    prte_list_append(&tinfo, &ds->super);
    /* use only one listener */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SINGLE_LISTENER, NULL, PMIX_BOOL);
    prte_list_append(&tinfo, &ds->super);

    /* setup any output format requests */
    if (prte_cmd_line_is_taken(prte_cmd_line, "tag-output")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_TAG_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "timestamp-output")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "xml")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_XML_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    }

    /* if they specified the URI, then pass it along */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "dvm-uri", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_URI, pval->data.string, PMIX_STRING);
        prte_list_append(&tinfo, &ds->super);
    }

    /* if we were launched by a debugger, then we need to have
     * notification of our termination sent */
    if (NULL != getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = false;
        PMIX_INFO_LOAD(ds->info, PMIX_EVENT_SILENT_TERMINATION, &flag, PMIX_BOOL);
        prte_list_append(&tinfo, &ds->super);
    }

    /* convert to array of info */
    ninfo = prte_list_get_size(&tinfo);
    PMIX_INFO_CREATE(iptr, ninfo);
    n = 0;
    PRTE_LIST_FOREACH(ds, &tinfo, prte_ds_info_t) {
        PMIX_INFO_XFER(&iptr[n], ds->info);
        ++n;
    }
    PRTE_LIST_DESTRUCT(&tinfo);

    /* now initialize PMIx - we have to indicate we are a launcher so that we
     * will provide rendezvous points for tools to connect to us */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n", prte_tool_basename);
        exit(1);
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* if the user just wants us to terminate a DVM, then do so */
    if (prte_cmd_line_is_taken(prte_cmd_line, "terminate")) {
        /* advise the user to utilize "pterm" in the future */
        prte_show_help("help-prun.txt", "use-pterm", true, prte_tool_basename);
        /* setup a lock to track the connection */
        PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
        /* register to trap connection loss */
        pmix_status_t code[2] = {PMIX_ERR_UNREACH, PMIX_ERR_LOST_CONNECTION_TO_SERVER};
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
        PMIx_Register_event_handler(code, 2, &info, 1,
                                    evhandler, regcbfunc, &lock);
        PRTE_PMIX_WAIT_THREAD(&lock);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        flag = true;
        PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_TERMINATE, &flag, PMIX_BOOL);
        fprintf(stderr, "TERMINATING DVM...");
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        rc = PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            goto DONE;
        } else if (PMIX_SUCCESS == rc) {
    #if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
            /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
             * being called when the server terminates. The callback might be eventually
             * called though then the connection to the server closes with
             * status PMIX_ERR_COMM_FAILURE */
            poll(NULL, 0, 1000);
            infocb(PMIX_SUCCESS, NULL, 0, (void *)&lock, NULL, NULL);
    #endif
            PRTE_PMIX_WAIT_THREAD(&lock);
            PRTE_PMIX_DESTRUCT_LOCK(&lock);
            /* wait for connection to depart */
            PRTE_PMIX_WAIT_THREAD(&rellock);
            PRTE_PMIX_DESTRUCT_LOCK(&rellock);
            /* wait for the connection to go away */
        }
        fprintf(stderr, "DONE\n");
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        return rc;
#else
        goto DONE;
#endif
    }


    /* register a default event handler and pass it our release lock
     * so we can cleanly exit if the server goes away */
    PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(NULL, 0, &info, 1, defhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    /* we want to be notified upon job completion */
    ds = PRTE_NEW(prte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    flag = true;
    PMIX_INFO_LOAD(ds->info, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);
    prte_list_append(&job_info, &ds->super);

    /* pass the personality */
    for (i=0; NULL != prte_schizo_base.personalities[i]; i++) {
        tmp = NULL;
        if (0 != strcmp(prte_schizo_base.personalities[i], "prte") &&
            0 != strcmp(prte_schizo_base.personalities[i], "pmix")) {
            prte_argv_append_nosize(&tmp, prte_schizo_base.personalities[i]);
        }
        if (NULL != tmp) {
            personality = prte_argv_join(tmp, ',');
            prte_argv_free(tmp);
            ds = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(ds->info, 1);
            PMIX_INFO_LOAD(ds->info, PMIX_PERSONALITY, personality, PMIX_STRING);
            /* don't free personality as we need it again later */
            prte_list_append(&job_info, &ds->super);
        }
    }

    /* check for stdout/err directives */
    /* if we were asked to tag output, mark it so */
    if (prte_cmd_line_is_taken(prte_cmd_line, "tag-output")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TAG_OUTPUT, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }
    /* if we were asked to timestamp output, mark it so */
    if (prte_cmd_line_is_taken(prte_cmd_line, "timestamp-output")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }
    /* cannot have both files and directory set for output */
    param = NULL;
    ptr = NULL;
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "output-filename", 0, 0))) {
        param = pval->data.string;
    }
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "output-directory", 0, 0))) {
        ptr = pval->data.string;
    }
    if (NULL != param && NULL != ptr) {
        prte_show_help("help-prted.txt", "both-file-and-dir-set", true,
                        param, ptr);
        return PRTE_ERR_FATAL;
    } else if (NULL != param) {
        /* if we were asked to output to files, pass it along. */
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
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
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_FILE, ptr, PMIX_STRING);
        free(ptr);
        prte_list_append(&job_info, &ds->super);
    } else if (NULL != ptr) {
#if PMIX_NUMERIC_VERSION >= 0x00040000
        /* if we were asked to output to a directory, pass it along. */
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
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
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_DIRECTORY, param, PMIX_STRING);
        free(param);
#endif
    }
    /* if we were asked to merge stderr to stdout, mark it so */
    if (prte_cmd_line_is_taken(prte_cmd_line, "merge-stderr-to-stdout")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }

    /* check what user wants us to do with stdin */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "stdin", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_STDIN_TGT, pval->data.string, PMIX_STRING);
        prte_list_append(&job_info, &ds->super);
    }

    /* if we want the argv's indexed, indicate that */
    if (prte_cmd_line_is_taken(prte_cmd_line, "index-argv-by-rank")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_INDEX_ARGV, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }

    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "map-by", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_MAPBY, pval->data.string, PMIX_STRING);
        prte_list_append(&job_info, &ds->super);
    }

    /* if the user specified a ranking policy, then set it */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "rank-by", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_RANKBY, pval->data.string, PMIX_STRING);
        prte_list_append(&job_info, &ds->super);
    }

    /* if the user specified a binding policy, then set it */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "bind-to", 0, 0))) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_BINDTO, pval->data.string, PMIX_STRING);
        prte_list_append(&job_info, &ds->super);
    }

    if (prte_cmd_line_is_taken(prte_cmd_line, "report-bindings")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_REPORT_BINDINGS, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }

    /* mark if recovery was enabled on the cmd line */
    if (prte_cmd_line_is_taken(prte_cmd_line, "enable-recovery")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }
    /* record the max restarts */
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "max-restarts", 0, 0)) &&
        0 < pval->data.integer) {
        ui32 = pval->data.integer;
        PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t) {
            ds = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(ds->info, 1);
            PMIX_INFO_LOAD(ds->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
            prte_list_append(&app->info, &ds->super);
        }
    }
    /* if continuous operation was specified */
    if (prte_cmd_line_is_taken(prte_cmd_line, "continuous")) {
        /* mark this job as continuously operating */
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }

    /* if stop-on-exec was specified */
    if (prte_cmd_line_is_taken(prte_cmd_line, "stop-on-exec")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_DEBUG_STOP_ON_EXEC, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    param = NULL;
    if (NULL != (pval = prte_cmd_line_get_param(prte_cmd_line, "timeout", 0, 0)) ||
        NULL != (param = getenv("MPIEXEC_TIMEOUT"))) {
        if (NULL != param) {
            i = strtol(param, NULL, 10);
            /* both cannot be present, or they must agree */
            if (NULL != pval && i != pval->data.integer) {
                prte_show_help("help-prun.txt", "prun:timeoutconflict", false,
                               prte_tool_basename, pval->data.integer, param);
                exit(1);
            }
        } else {
            i = pval->data.integer;
        }
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT, &i, PMIX_INT);
        prte_list_append(&job_info, &ds->super);
    }
#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (prte_cmd_line_is_taken(prte_cmd_line, "get-stack-traces")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT_STACKTRACES, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "report-state-on-timeout")) {
        ds = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT_REPORT_STATE, &flag, PMIX_BOOL);
        prte_list_append(&job_info, &ds->super);
    }
#endif

    /* give the schizo components a chance to add to the job info */
    prte_schizo.job_info(prte_cmd_line, &job_info);

    /* pickup any relevant envars */
    ninfo = 3;
    if (NULL != personality) {
        ++ninfo;
    }
    PMIX_INFO_CREATE(iptr, ninfo);
    flag = true;
    PMIX_INFO_LOAD(&iptr[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    ui32 = geteuid();
    PMIX_INFO_LOAD(&iptr[1], PMIX_USERID, &ui32, PMIX_UINT32);
    ui32 = getegid();
    PMIX_INFO_LOAD(&iptr[2], PMIX_GRPID, &ui32, PMIX_UINT32);
    if (NULL != personality) {
        PMIX_INFO_LOAD(&iptr[3], PMIX_PERSONALITY, personality, PMIX_STRING);
        free(personality);  // done with this now
    }

    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prte_process_info.myproc.nspace, iptr, ninfo, setupcbfunc, &mylock);
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
        for (n=0; n < mylock.ninfo; n++) {
            if (0 == strncmp(mylock.info[n].key, PMIX_SET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_ADD_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_UNSET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_PREPEND_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_APPEND_ENVAR, PMIX_MAX_KEYLEN)) {
                ds = PRTE_NEW(prte_ds_info_t);
                PMIX_INFO_CREATE(ds->info, 1);
                PMIX_INFO_XFER(&ds->info[0], &mylock.info[n]);
                prte_list_append(&job_info, &ds->super);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }

    /* they want to run an application, so let's parse
     * the cmd line to get it */

    if (PRTE_SUCCESS != (rc = parse_locals(&apps, pargc, pargv))) {
        PRTE_ERROR_LOG(rc);
        PRTE_LIST_DESTRUCT(&apps);
        goto DONE;
    }

    /* bozo check */
    if (0 == prte_list_get_size(&apps)) {
        prte_output(0, "No application specified!");
        goto DONE;
    }

    /* if we were launched by a tool wanting to direct our
     * operation, then we need to pause here and give it
     * a chance to tell us what we need to do */
    if (NULL != (param = getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL"))) {
        ptr = strdup(param);
        param = strchr(ptr, ':');
        if (NULL == param) {
            prte_show_help("help-prun.txt", "bad-pause-for-tool", true,
                           prte_tool_basename, ptr, prte_tool_basename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            goto DONE;
        }
        *param = '\0';
        ++param;
        /* register for the PMIX_LAUNCH_DIRECTIVE event */
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIX_LAUNCH_DIRECTIVE;
        /* setup the myinfo object to capture the returned
         * values - must do so prior to registering in case
         * the event has already arrived */
        PRTE_CONSTRUCT(&myinfo, myinfo_t);

        /* go ahead and register */
        PMIx_Register_event_handler(&ret, 1, NULL, 0, launchhandler, regcbfunc, &lock);
        PRTE_PMIX_WAIT_THREAD(&lock);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        /* notify the tool that we are ready */
        (void)strncpy(controller.nspace, ptr, PMIX_MAX_NSLEN);
        controller.rank = strtoul(param, NULL, 10);
        PMIX_INFO_CREATE(iptr, 2);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);
        /* not to be delivered to a default event handler */
        PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        PMIx_Notify_event(PMIX_LAUNCHER_READY, &prte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          iptr, 2, NULL, NULL);
        /* now wait for the launch directives to arrive */
        PRTE_PMIX_WAIT_THREAD(&myinfo.lock);
        PMIX_INFO_FREE(iptr, 1);

        /* process the returned directives */
        if (NULL != myinfo.info) {
            for (n=0; n < myinfo.ninfo; n++) {
                if (0 == strncmp(myinfo.info[n].key, PMIX_DEBUG_JOB_DIRECTIVES, PMIX_MAX_KEYLEN)) {
                    /* there will be a pmix_data_array containing the directives */
                    iptr = (pmix_info_t*)myinfo.info[n].value.data.darray->array;
                    ninfo = myinfo.info[n].value.data.darray->size;
                    for (m=0; m < ninfo; m++) {
#if PMIX_NUMERIC_VERSION >= 0x00040000
                        if (PMIX_CHECK_KEY(&iptr[m], PMIX_NOTIFY_LAUNCH)) {
                            /* we don't pass this along - it is aimed at us */
                            notify_launch = true;
                            continue;
                        }
#endif
                        ds = PRTE_NEW(prte_ds_info_t);
                        ds->info = &iptr[m];
                        prte_list_append(&job_info, &ds->super);
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                } else if (0 == strncmp(myinfo.info[n].key, PMIX_DEBUG_APP_DIRECTIVES, PMIX_MAX_KEYLEN)) {
                    /* there will be a pmix_data_array containing the directives */
                    iptr = (pmix_info_t*)myinfo.info[n].value.data.darray->array;
                    ninfo = myinfo.info[n].value.data.darray->size;
                    for (m=0; m < ninfo; m++) {
                        PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t) {
                            /* the value can only be on one list at a time, so replicate it */
                            ds = PRTE_NEW(prte_ds_info_t);
                            ds->info = &iptr[n];
                            prte_list_append(&app->info, &ds->super);
                        }
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                }
            }
        }
    }

    /* convert the job info into an array */
    ninfo = prte_list_get_size(&job_info);
    iptr = NULL;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(iptr, ninfo);
        n=0;
        PRTE_LIST_FOREACH(ds, &job_info, prte_ds_info_t) {
            PMIX_INFO_XFER(&iptr[n], ds->info);
            ++n;
        }
    }

    /* convert the apps to an array */
    napps = prte_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    n = 0;
    PRTE_LIST_FOREACH(app, &apps, prte_pmix_app_t) {
        size_t fbar;

        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = prte_argv_copy(app->app.argv);
        papps[n].env = prte_argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        fbar = prte_list_get_size(&app->info);
        if (0 < fbar) {
            papps[n].ninfo = fbar;
            PMIX_INFO_CREATE(papps[n].info, fbar);
            m = 0;
            PRTE_LIST_FOREACH(ds, &app->info, prte_ds_info_t) {
                PMIX_INFO_XFER(&papps[n].info[m], ds->info);
                ++m;
            }
        }
        /* pickup any relevant envars */
        rc = prte_schizo.parse_env(prte_cmd_line, environ, &papps[n].env, false);
        if (PRTE_SUCCESS != rc) {
            goto DONE;
        }
        ++n;
    }

    if (verbose) {
        prte_output(0, "Calling PMIx_Spawn");
    }

    ret = PMIx_Spawn(iptr, ninfo, papps, napps, spawnednspace);
    if (PRTE_SUCCESS != ret) {
        prte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        goto DONE;
    }

    PRTE_PMIX_CONVERT_NSPACE(rc, &myjobid, spawnednspace);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (notify_launch) {
        /* direct an event back to our controller telling them
         * the namespace of the spawned job */
        PMIX_INFO_CREATE(iptr, 3);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[1], PMIX_NSPACE, spawnednspace, PMIX_STRING);
        /* not to be delivered to a default event handler */
        PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        PMIx_Notify_event(PMIX_LAUNCH_COMPLETE, &controller, PMIX_RANGE_CUSTOM,
                          iptr, 3, NULL, NULL);
        PMIX_INFO_FREE(iptr, 3);
    }
    /* push our stdin to the apps */
    PMIX_LOAD_PROCID(&pname, spawnednspace, 0);  // forward stdin to rank=0
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
#endif

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
        prte_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(myjobid));
    }
    PRTE_PMIX_WAIT_THREAD(&rellock);
    /* save the status */
    rc = rellock.status;
    /* output any message */
    if (NULL != rellock.msg) {
        fprintf(stderr, "%s\n", rellock.msg);
    }

    /* if we lost connection to the server, then we are done */
    if (PMIX_ERR_LOST_CONNECTION_TO_SERVER == rc ||
        PMIX_ERR_UNREACH == rc) {
        goto DONE;
    }

    /* deregister our event handler */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&rellock);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* close the push of our stdin */
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, iptr, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prte_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 1);
#endif

  DONE:
    PRTE_LIST_FOREACH(evitm, &forwarded_signals, prte_event_list_item_t) {
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

static int parse_locals(prte_list_t *jdata, int argc, char* argv[])
{
    int i, rc;
    int temp_argc;
    char **temp_argv, **env;
    prte_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argc = 0;
    temp_argv = NULL;
    prte_argv_append(&temp_argc, &temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
       create_app(), below.  See comment immediately before the
       create_app() function for an explanation. */

    env = NULL;
    for (i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (prte_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    prte_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
                if (PRTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                       printed */
                    return rc;
                }
                if (made_app) {
                    prte_list_append(jdata, &app->super);
                }

                /* Reset the temps */

                temp_argc = 0;
                temp_argv = NULL;
                prte_argv_append(&temp_argc, &temp_argv, argv[0]);
            }
        } else {
            prte_argv_append(&temp_argc, &temp_argv, argv[i]);
        }
    }

    if (prte_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
        if (PRTE_SUCCESS != rc) {
            /* Assume that the error message has already been printed;
               no need to cleanup -- we can just exit */
            exit(1);
        }
        if (made_app) {
            prte_list_append(jdata, &app->super);
        }
    }
    if (NULL != env) {
        prte_argv_free(env);
    }
    prte_argv_free(temp_argv);

    /* All done */

    return PRTE_SUCCESS;
}


/*
 * This function takes a "char ***app_env" parameter to handle the
 * specific case:
 *
 *   prun --mca foo bar -app appfile
 *
 * That is, we'll need to keep foo=bar, but the presence of the app
 * file will cause an invocation of parse_appfile(), which will cause
 * one or more recursive calls back to create_app().  Since the
 * foo=bar value applies globally to all apps in the appfile, we need
 * to pass in the "base" environment (that contains the foo=bar value)
 * when we parse each line in the appfile.
 *
 * This is really just a special case -- when we have a simple case like:
 *
 *   prun --mca foo bar -np 4 hostname
 *
 * Then the upper-level function (parse_locals()) calls create_app()
 * with a NULL value for app_env, meaning that there is no "base"
 * environment that the app needs to be created from.
 */
static int create_app(int argc, char* argv[],
                      prte_list_t *jdata,
                      prte_pmix_app_t **app_ptr,
                      bool *made_app, char ***app_env)
{
    char cwd[PRTE_PATH_MAX];
    int i, j, count, rc;
    char *param, *value;
    prte_pmix_app_t *app = NULL;
    bool found = false;
    char *appname = NULL;
    prte_ds_info_t *val;
    prte_value_t *pvalue;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    if (PRTE_SUCCESS != (rc = prte_cmd_line_parse(prte_cmd_line, true, false,
                                                    argc, argv)) ) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prte_strerror(rc));
        }
        return rc;
    }

    /* Setup application context */
    app = PRTE_NEW(prte_pmix_app_t);
    prte_cmd_line_get_tail(prte_cmd_line, &count, &app->app.argv);

    /* See if we have anything left */
    if (0 == count) {
        prte_show_help("help-prun.txt", "prun:executable-not-specified",
                       true, "prun", "prun");
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* Did the user request a specific wdir? */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "wdir", 0, 0))) {
        param = pvalue->data.string;
        /* if this is a relative path, convert it to an absolute path */
        if (prte_path_is_absolute(param)) {
            app->app.cwd = strdup(param);
        } else {
            /* get the cwd */
            if (PRTE_SUCCESS != (rc = prte_getcwd(cwd, sizeof(cwd)))) {
                prte_show_help("help-prun.txt", "prun:init-failure",
                               true, "get the cwd", rc);
                goto cleanup;
            }
            /* construct the absolute path */
            app->app.cwd = prte_os_path(false, cwd, param, NULL);
        }
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "set-cwd-to-session-dir")) {
        val = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
        prte_list_append(&app->info, &val->super);
    } else {
        if (PRTE_SUCCESS != (rc = prte_getcwd(cwd, sizeof(cwd)))) {
            prte_show_help("help-prun.txt", "prun:init-failure",
                           true, "get the cwd", rc);
            goto cleanup;
        }
        app->app.cwd = strdup(cwd);
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* if they specified a process set name, then pass it along */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "pset", 0, 0))) {
        val = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_PSET_NAME, pvalue->data.string, PMIX_STRING);
        prte_list_append(&app->info, &val->super);
    }
#endif

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    found = false;
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "hostfile"))) {
        if (1 < j) {
            prte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, "prun", NULL);
            return PRTE_ERR_FATAL;
        } else {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "hostfile", 0, 0);
            val = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, pvalue->data.string, PMIX_STRING);
            prte_list_append(&app->info, &val->super);
            found = true;
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "machinefile"))) {
        if (1 < j || found) {
            prte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, "prun", NULL);
            return PRTE_ERR_FATAL;
        } else {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "machinefile", 0, 0);
            val = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, pvalue->data.string, PMIX_STRING);
            prte_list_append(&app->info, &val->super);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "host", i, 0);
            prte_argv_append_nosize(&targ, pvalue->data.string);
        }
        tval = prte_argv_join(targ, ',');
        val = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_HOST, tval, PMIX_STRING);
        prte_list_append(&app->info, &val->super);
        free(tval);
    }

    /* check for bozo error */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "np", 0, 0)) ||
        NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "n", 0, 0))) {
        if (0 > pvalue->data.integer) {
            prte_show_help("help-prun.txt", "prun:negative-nprocs",
                           true, "prun", app->app.argv[0],
                           pvalue->data.integer, NULL);
            return PRTE_ERR_FATAL;
        }
    }
    if (NULL != pvalue) {
        /* we don't require that the user provide --np or -n because
         * the cmd line might stipulate a mapping policy that computes
         * the number of procs - e.g., a map-by ppr option */
        app->app.maxprocs = pvalue->data.integer;
    }

    /* see if we need to preload the binary to
     * find the app - don't do this for java apps, however, as we
     * can't easily find the class on the cmd line. Java apps have to
     * preload their binary via the preload_files option
     */
    if (NULL == strstr(app->app.argv[0], "java")) {
        if (prte_cmd_line_is_taken(prte_cmd_line, "preload-binaries")) {
            val = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
            prte_list_append(&app->info, &val->super);
            val = PRTE_NEW(prte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
            prte_list_append(&app->info, &val->super);
        }
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "preload-files")) {
        val = PRTE_NEW(prte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_FILES, NULL, PMIX_BOOL);
        prte_list_append(&app->info, &val->super);
    }

    /* Do not try to find argv[0] here -- the starter is responsible
       for that because it may not be relevant to try to find it on
       the node where prun is executing.  So just strdup() argv[0]
       into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        prte_show_help("help-prun.txt", "prun:call-failed",
                       true, "prun", "library", "strdup returned NULL", errno);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* if this is a Java application, we have a bit more work to do. Such
     * applications actually need to be run under the Java virtual machine
     * and the "java" command will start the "executable". So we need to ensure
     * that all the proper java-specific paths are provided
     */
    appname = prte_basename(app->app.cmd);
    if (0 == strcmp(appname, "java")) {
        /* see if we were given a library path */
        found = false;
        for (i=1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "java.library.path")) {
                char *dptr;
                /* find the '=' that delineates the option from the path */
                if (NULL == (dptr = strchr(app->app.argv[i], '='))) {
                    /* that's just wrong */
                    rc = PRTE_ERR_BAD_PARAM;
                    goto cleanup;
                }
                /* step over the '=' */
                ++dptr;
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                if (NULL == strstr(app->app.argv[i], prte_install_dirs.libdir)) {
                    /* doesn't appear to - add it to be safe */
                    if (':' == app->app.argv[i][strlen(app->app.argv[i]-1)]) {
                        prte_asprintf(&value, "-Djava.library.path=%s%s", dptr, prte_install_dirs.libdir);
                    } else {
                        prte_asprintf(&value, "-Djava.library.path=%s:%s", dptr, prte_install_dirs.libdir);
                    }
                    free(app->app.argv[i]);
                    app->app.argv[i] = value;
                }
                break;
            }
        }
        if (!found) {
            /* need to add it right after the java command */
            prte_asprintf(&value, "-Djava.library.path=%s", prte_install_dirs.libdir);
            prte_argv_insert_element(&app->app.argv, 1, value);
            free(value);
        }

        /* see if we were given a class path */
        found = false;
        for (i=1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "cp") ||
                NULL != strstr(app->app.argv[i], "classpath")) {
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                /* check if mpi.jar exists - if so, add it */
                value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "mpi.jar");
                }
                free(value);
                /* check for oshmem support */
                value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "shmem.jar");
                }
                free(value);
                /* always add the local directory */
                prte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[i+1]);
                free(app->app.argv[i+1]);
                app->app.argv[i+1] = value;
                break;
            }
        }
        if (!found) {
            /* check to see if CLASSPATH is in the environment */
            found = false;  // just to be pedantic
            for (i=0; NULL != environ[i]; i++) {
                if (0 == strncmp(environ[i], "CLASSPATH", strlen("CLASSPATH"))) {
                    value = strchr(environ[i], '=');
                    ++value; /* step over the = */
                    prte_argv_insert_element(&app->app.argv, 1, value);
                    /* check for mpi.jar */
                    value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "mpi.jar");
                    }
                    free(value);
                    /* check for shmem.jar */
                    value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "shmem.jar");
                    }
                    free(value);
                    /* always add the local directory */
                    prte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[1]);
                    free(app->app.argv[1]);
                    app->app.argv[1] = value;
                    prte_argv_insert_element(&app->app.argv, 1, "-cp");
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* need to add it right after the java command - have
                 * to include the working directory and trust that
                 * the user set cwd if necessary
                 */
                char *str, *str2;
                /* always start with the working directory */
                str = strdup(app->app.cwd);
                /* check for mpi.jar */
                value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    prte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                /* check for shmem.jar */
                value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    prte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                prte_argv_insert_element(&app->app.argv, 1, str);
                free(str);
                prte_argv_insert_element(&app->app.argv, 1, "-cp");
            }
        }
    }

    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

  cleanup:
    if (NULL != app) {
        PRTE_RELEASE(app);
    }
    if (NULL != appname) {
        free(appname);
    }
    return rc;
}

static void set_classpath_jar_file(prte_pmix_app_t *app, int index, char *jarfile)
{
    if (NULL == strstr(app->app.argv[index], jarfile)) {
        /* nope - need to add it */
        char *fmt = ':' == app->app.argv[index][strlen(app->app.argv[index]-1)]
                    ? "%s%s/%s" : "%s:%s/%s";
        char *str;
        prte_asprintf(&str, fmt, app->app.argv[index], prte_install_dirs.libdir, jarfile);
        free(app->app.argv[index]);
        app->app.argv[index] = str;
    }
}

static void clean_abort(int fd, short flags, void *arg)
{
    pmix_proc_t target;
    pmix_info_t directive;
    pmix_status_t rc;

    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prte_atomic_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr, "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        return;
    }

    /* tell PRTE to terminate our job */
    PMIX_LOAD_PROCID(&target, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
    rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
    prte_output(0, "JOB CTRL %s", PMIx_Error_string(rc));
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIx_tool_finalize();
        /* exit with a non-zero status */
        exit(1);
    }
}

static struct timeval current, last={0,0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been abprted.
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

static void signal_forward_callback(int signum)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;

    if (verbose){
        fprintf(stderr, "%s: Forwarding signal %d to job\n",
                prte_tool_basename, signum);
    }

    /* send the signal out to the processes */
    PMIX_LOAD_PROCID(&proc, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_SIGNAL, &signum, PMIX_INT);
#if PMIX_NUMERIC_VERSION >= 0x00040000
    rc = PMIx_Job_control(&proc, 1, &info, 1, NULL, NULL);
#else
    rc = PMIx_Job_control(&proc, 1, &info, 1);
#endif
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Signal %d could not be sent to job %s (returned %s)",
                signum, spawnednspace, PMIx_Error_string(rc));
    }
}

/**
 * Deal with sigpipe errors
 */
static int sigpipe_error_count=0;
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
