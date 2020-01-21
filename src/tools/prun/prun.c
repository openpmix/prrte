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

#include "prun.h"

typedef struct {
    prrte_object_t super;
    prrte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} myinfo_t;
static void mcon(myinfo_t *p)
{
    PRRTE_PMIX_CONSTRUCT_LOCK(&p->lock);
    p->info = NULL;
    p->ninfo = 0;
}
static void mdes(myinfo_t *p)
{
    PRRTE_PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
static PRRTE_CLASS_INSTANCE(myinfo_t, prrte_object_t,
                            mcon, mdes);

typedef struct {
    prrte_list_item_t super;
    pmix_app_t app;
    prrte_list_t info;
} prrte_pmix_app_t;
static void acon(prrte_pmix_app_t *p)
{
    PMIX_APP_CONSTRUCT(&p->app);
    PRRTE_CONSTRUCT(&p->info, prrte_list_t);
}
static void ades(prrte_pmix_app_t *p)
{
    PMIX_APP_DESTRUCT(&p->app);
    PRRTE_LIST_DESTRUCT(&p->info);
}
static PRRTE_CLASS_INSTANCE(prrte_pmix_app_t,
                          prrte_list_item_t,
                          acon, ades);

typedef struct {
    prrte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static prrte_list_t job_info;
static prrte_jobid_t myjobid = PRRTE_JOBID_INVALID;
static myinfo_t myinfo;

static int create_app(int argc, char* argv[],
                      prrte_list_t *jdata,
                      prrte_pmix_app_t **app,
                      bool *made_app, char ***app_env);
static int parse_locals(prrte_list_t *jdata, int argc, char* argv[]);
static void set_classpath_jar_file(prrte_pmix_app_t *app, int index, char *jarfile);
static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool forcibly_die=false;
static prrte_event_t term_handler;
static int term_pipe[2];
static prrte_atomic_lock_t prun_abort_inprogress_lock = {0};
static prrte_event_base_t *myevbase = NULL;
static bool proxyrun = false;
static bool verbose = false;
static prrte_cmd_line_t *prrte_cmd_line = NULL;

/* prun-specific options */
static prrte_cmd_line_init_t cmd_line_init[] = {

    /* tell the dvm to terminate */
    { '\0', "terminate", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Terminate the DVM", PRRTE_CMD_LINE_OTYPE_DVM },
    /* look first for a system server */
    { '\0', "system-server-first", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "First look for a system server and connect to it if found",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* connect only to a system server */
    { '\0', "system-server-only", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Connect only to a system-level server",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* do not connect */
    { '\0', "do-not-connect", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not connect to a server",
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


    /* testing options */
    { '\0', "timeout", 1, PRRTE_CMD_LINE_TYPE_INT,
      "Timeout the job after the specified number of seconds",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "report-state-on-timeout", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Report all job and process states upon timeout",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "get-stack-traces", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Get stack traces of all application procs on timeout",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* Conventional options - for historical compatibility, support
     * both single and multi dash versions */
    /* Number of processes; -c, -n, --n, -np, and --np are all
       synonyms */
    { 'c', "np", 1, PRRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run",
      PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'n', "n", 1, PRRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run",
      PRRTE_CMD_LINE_OTYPE_GENERAL },
    /* Use an appfile */
    { '\0',  "app", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide an appfile; ignore all other command line options",
      PRRTE_CMD_LINE_OTYPE_GENERAL },


      /* Output options */
    /* exit status reporting */
    { '\0', "report-child-jobs-separately", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Return the exit status of the primary job only",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    /* select XML output */
    { '\0', "xml", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Provide all output in XML format",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "xml-file", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide all output in XML format to the specified file",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    /* tag output */
    { '\0', "tag-output", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Tag all output with [job,rank]",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "timestamp-output", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Timestamp all application process output",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-directory", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename/job/rank/std[out,err,diag]. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOJOBID (do not include a job-id directory level) and NOCOPY (do not copy the output to the stdout/err streams)",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-filename", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename.rank. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOCOPY (do not copy the output to the stdout/err streams)",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "merge-stderr-to-stdout", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Merge stderr to stdout for each process",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "xterm", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Create a new xterm window and display output from the specified ranks there",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },

    /* select stdin option */
    { '\0', "stdin", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify procs to receive stdin [rank, all, none] (default: 0, indicating rank 0)",
      PRRTE_CMD_LINE_OTYPE_INPUT },


    /* User-level debugger arguments */
    { '\0', "debug", 1, PRRTE_CMD_LINE_TYPE_BOOL,
      "Invoke the indicated user-level debugger (provide a comma-delimited list of debuggers to search for)",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "output-proctable", 1, PRRTE_CMD_LINE_TYPE_BOOL,
      "Print the complete proctable to stdout [-], stderr [+], or a file [anything else] after launch",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* Launch options */
    /* request that argv[0] be indexed */
    { '\0', "index-argv-by-rank", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Uniquely index argv[0] for each process using its rank",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload the binary on the remote machine */
    { 's', "preload-binary", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Preload the binary on the remote machine before starting the remote process.",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload files on the remote machine */
    { '\0', "preload-files", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Preload the comma separated list of files to the remote machines current working directory before starting the remote process.",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Export environment variables; potentially used multiple times,
       so it does not make sense to set into a variable */
    { 'x', NULL, 1, PRRTE_CMD_LINE_TYPE_NULL,
      "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the environment variable foo and takes its value from the current environment; \"-x foo=bar\" exports the environment variable name foo and sets its value to \"bar\" in the started processes)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wdir", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Set the working directory of the started processes",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wd", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Synonym for --wdir",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "set-cwd-to-session-dir", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Set the working directory of the started processes to their session directory",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "path", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "PATH to be used to look for executables to start processes",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "show-progress", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Output a brief periodic report on launch progress",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "pset", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "User-specified name assigned to the processes in their given application",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },



    /* Developer options */
    { '\0', "do-not-resolve", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not attempt to resolve interfaces - usually used to determine proposed process placement/binding prior to obtaining an allocation",
      PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "do-not-launch", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Perform all necessary operations to prepare to launch the application, but do not actually launch it (usually used to test mapping patterns)",
      PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-map", 0, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display a detailed process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-topo", 0, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display the topology as part of the process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-diffable-map", 0, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display a diffable process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "report-bindings", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Whether to report process bindings to stderr",
      PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-allocation", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display a detailed list (mostly intended for developers) of the allocation being used by this job",
      PRRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-map", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display the process map just before launch",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "display-allocation", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display the allocation being used by this job",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* Mapping options */
    { '\0', "map-by", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Mapping Policy [slot | hwthread | core (default:np<=2) | l1cache | l2cache | l3cache | socket | numa (default:np>2) | board | node | seq | dist | ppr(:N:RESOURCE) | rankfile(:FILENAME)], with allowed modifiers :PE=y:PE-LIST=a,b:SPAN:OVERSUBSCRIBE:NOOVERSUBSCRIBE:NOLOCAL",
      PRRTE_CMD_LINE_OTYPE_MAPPING },


      /* Ranking options */
    { '\0', "rank-by", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Ranking Policy [slot (default) | hwthread | core | socket | numa | board | node], with allowed modifiers :SPAN",
      PRRTE_CMD_LINE_OTYPE_RANKING },


      /* Binding options */
    { '\0', "bind-to", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Policy for binding processes [none (default:oversubscribed), hwthread, core (default:np<=2), l1cache, l2cache, l3cache, socket, numa] Allowed qualifiers: OVERLOAD:IF-SUPPORTED",
      PRRTE_CMD_LINE_OTYPE_BINDING },


    /* Fault Tolerance options */
    { '\0', "enable-recovery", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable recovery from process failure [Default = disabled]",
      PRRTE_CMD_LINE_OTYPE_FT },
    { '\0', "max-restarts", 1, PRRTE_CMD_LINE_TYPE_INT,
      "Max number of times to restart a failed process",
      PRRTE_CMD_LINE_OTYPE_FT },
    { '\0', "disable-recovery", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Disable recovery (resets all recovery options to off)",
      PRRTE_CMD_LINE_OTYPE_FT },
    { '\0', "continuous", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Job is to run until explicitly terminated",
      PRRTE_CMD_LINE_OTYPE_FT },



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

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_ACQUIRE_OBJECT(lock);
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

static void defhandler(size_t evhdlr_registration_id,
                       pmix_status_t status,
                       const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    prrte_pmix_lock_t *lock = NULL;
    size_t n;
    pmix_proc_t target;
    pmix_info_t directive;

    if (verbose) {
        prrte_output(0, "PRUN: DEFHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (PMIX_ERR_IOF_FAILURE == status) {
        /* tell PRRTE to terminate our job */
        PRRTE_PMIX_CONVERT_JOBID(target.nspace, myjobid);
        target.rank = PMIX_RANK_WILDCARD;
        PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL)) {
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
                    lock = (prrte_pmix_lock_t*)info[n].value.data.ptr;
                }
            }
        }

        if (NULL == lock) {
            exit(1);
        }
        /* save the status */
        lock->status = status;
        /* release the lock */
        PRRTE_PMIX_WAKEUP_THREAD(lock);
    }

  progress:
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

    PRRTE_PMIX_WAKEUP_THREAD(&mylock->lock);
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
    PRRTE_PMIX_WAKEUP_THREAD(&myinfo.lock);
}

int prun(int argc, char *argv[])
{
    int rc=1, i;
    char *param, *ptr;
    prrte_pmix_lock_t lock, rellock;
    prrte_list_t apps;
    prrte_pmix_app_t *app;
    prrte_list_t tinfo;
    pmix_info_t info, *iptr;
    pmix_proc_t pname, controller;
    pmix_status_t ret;
    bool flag;
    prrte_ds_info_t *ds;
    size_t m, n, ninfo;
    pmix_app_t *papps;
    size_t napps;
    char nspace[PMIX_MAX_NSLEN+1];
    mylock_t mylock;
    bool notify_launch = false;
    char **prteargs = NULL;
    FILE *fp;
    char buf[2048];
    prrte_value_t *pval;
    uint32_t ui32;
    pid_t pid;

    /* init the globals */
    PRRTE_CONSTRUCT(&job_info, prrte_list_t);
    PRRTE_CONSTRUCT(&apps, prrte_list_t);

    prrte_atomic_lock_init(&prun_abort_inprogress_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);
    /* init the tiny part of PRRTE we use */
    prrte_init_util();

    prrte_tool_basename = prrte_basename(argv[0]);
    if (0 != strcmp(prrte_tool_basename, "prun")) {
        proxyrun = true;
        if (NULL != strchr(argv[0], '/')) {
            /* see if we were given a path to the proxy */
            ptr = prrte_dirname(argv[0]);
            if (NULL == ptr) {
                fprintf(stderr, "Could not parse the given cmd line\n");
                exit(1);
            }
            /* they gave us a path, so prefix the "prte" cmd with it */
            if ('/' == ptr[strlen(ptr)-1]) {
                prrte_asprintf(&param, "%sprte", ptr);
            } else {
                prrte_asprintf(&param, "%s/prte", ptr);
            }
            prrte_argv_append_nosize(&prteargs, param);
            free(ptr);
            free(param);
        } else {
            prrte_argv_append_nosize(&prteargs, "prte");
        }
    }

    /* setup our cmd line */
    prrte_cmd_line = PRRTE_NEW(prrte_cmd_line_t);
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_add(prrte_cmd_line, cmd_line_init))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* open the SCHIZO framework */
    if (PRRTE_SUCCESS != (rc = prrte_mca_base_framework_open(&prrte_schizo_base_framework, 0))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRRTE_SUCCESS != (rc = prrte_schizo_base_select())) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* scan for personalities */
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, argv[i+1], false);
        }
    }
    if (proxyrun) {
        prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "ompi", false);
    }

    /* setup the rest of the cmd line only once */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.define_cli(prrte_cmd_line))) {
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

    /* now let the schizo components take a pass at it to get the MCA params */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.parse_cli(argc, 0, argv, NULL, NULL))) {
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
        str = prrte_show_help_string("help-prrterun.txt", "prrterun:usage", false,
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

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid()) {
        prrte_schizo.allow_run_as_root(prrte_cmd_line);  // will exit us if not allowed
    }

    if (!prrte_cmd_line_is_taken(prrte_cmd_line, "terminate")) {
        /* they want to run an application, so let's parse
         * the cmd line to get it */

        if (PRRTE_SUCCESS != (rc = parse_locals(&apps, argc, argv))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_LIST_DESTRUCT(&apps);
            goto DONE;
        }

        /* bozo check */
        if (0 == prrte_list_get_size(&apps)) {
            prrte_output(0, "No application specified!");
            goto DONE;
        }

        if (proxyrun) {
            prrte_schizo.parse_proxy_cli(prrte_cmd_line, &prteargs);
            prrte_argv_append_nosize(&prteargs, "&");
            prrte_schizo.wrap_args(prteargs);
            param = prrte_argv_join(prteargs, ' ');
            fp = popen(param, "r");
            if (NULL == fp) {
                fprintf(stderr, "Error executing prte\n");
                exit(1);
            }
            i = 0;
            while (fgets(buf, 2048, fp) != NULL) {
                if (NULL != strstr(buf, "ready")) {
                    break;
                }
                ++i;
                if (i > 6500000) {
                    fprintf(stderr, "prte failed to start\n");
                    exit(1);
                }
            }
        }
    }

    /* setup options */
    PRRTE_CONSTRUCT(&tinfo, prrte_list_t);
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "do-not-connect")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    } else if (prrte_cmd_line_is_taken(prrte_cmd_line, "system-server-first")) {
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

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    ds = PRRTE_NEW(prrte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_LAUNCHER, NULL, PMIX_BOOL);
    prrte_list_append(&tinfo, &ds->super);
    /* we always support session-level rendezvous */
    ds = PRRTE_NEW(prrte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    prrte_list_append(&tinfo, &ds->super);
    /* use only one listener */
    ds = PRRTE_NEW(prrte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SINGLE_LISTENER, NULL, PMIX_BOOL);
    prrte_list_append(&tinfo, &ds->super);

    /* setup any output format requests */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "tag-output")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_TAG_OUTPUT, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "timestamp-output")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "xml")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_IOF_XML_OUTPUT, NULL, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    }

    /* if they specified the URI, then pass it along */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "dvm-uri", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_URI, pval->data.string, PMIX_STRING);
        prrte_list_append(&tinfo, &ds->super);
    }

    /* if we were launched by a debugger, then we need to have
     * notification of our termination sent */
    if (NULL != getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = false;
        PMIX_INFO_LOAD(ds->info, PMIX_EVENT_SILENT_TERMINATION, &flag, PMIX_BOOL);
        prrte_list_append(&tinfo, &ds->super);
    }

#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    /* check for request to drop a rendezvous file */
    if (NULL != (param = getenv("PMIX_LAUNCHER_RENDEZVOUS_FILE"))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_LAUNCHER_RENDEZVOUS_FILE, param, PMIX_STRING);
        prrte_list_append(&tinfo, &ds->super);
    }
#endif

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
    pipe(term_pipe);
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
        PRRTE_ERROR_LOG(ret);
        prrte_progress_thread_finalize(NULL);
        return ret;
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* if the user just wants us to terminate a DVM, then do so */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "terminate")) {
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
        PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
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
        /* wait for the connection to go away */
        if (!proxyrun) {
            fprintf(stderr, "DONE\n");
        }
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        return rc;
#else
        goto DONE;
#endif
    }

    /* register a default event handler and pass it our release lock
     * so we can cleanly exit if the server goes away */
    PRRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(NULL, 0, &info, 1, defhandler, regcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);

    /* we want to be notified upon job completion */
    ds = PRRTE_NEW(prrte_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    flag = true;
    PMIX_INFO_LOAD(ds->info, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);
    prrte_list_append(&job_info, &ds->super);

    /* see if they specified the personality */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "personality", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_PERSONALITY, pval->data.string, PMIX_STRING);
        prrte_list_append(&job_info, &ds->super);
    }

    /* check for stdout/err directives */
    /* if we were asked to tag output, mark it so */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "tag-output")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TAG_OUTPUT, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }
    /* if we were asked to timestamp output, mark it so */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "timestamp-output")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }
   /* cannot have both files and directory set for output */
    param = NULL;
    ptr = NULL;
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "output-filename", 0, 0))) {
        param = pval->data.string;
    }
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "output-directory", 0, 0))) {
        ptr = pval->data.string;
    }
    if (NULL != param && NULL != ptr) {
        prrte_show_help("help-prted.txt", "both-file-and-dir-set", true,
                        param, ptr);
        return PRRTE_ERR_FATAL;
    } else if (NULL != param) {
        /* if we were asked to output to files, pass it along. */
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        /* if the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!prrte_path_is_absolute(param)) {
            char cwd[PRRTE_PATH_MAX];
            getcwd(cwd, sizeof(cwd));
            ptr = prrte_os_path(false, cwd, param, NULL);
        } else {
            ptr = strdup(param);
        }
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_FILE, ptr, PMIX_STRING);
        free(ptr);
        prrte_list_append(&job_info, &ds->super);
    } else if (NULL != ptr) {
        /* if we were asked to output to a directory, pass it along. */
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        /* If the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!prrte_path_is_absolute(ptr)) {
            char cwd[PRRTE_PATH_MAX];
            getcwd(cwd, sizeof(cwd));
            param = prrte_os_path(false, cwd, ptr, NULL);
        } else {
            param = strdup(ptr);
        }
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_DIRECTORY, param, PMIX_STRING);
        free(param);
    }
    /* if we were asked to merge stderr to stdout, mark it so */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "merge-stderr-to-stdout")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    /* check what user wants us to do with stdin */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "stdin", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_STDIN_TGT, pval->data.string, PMIX_STRING);
        prrte_list_append(&job_info, &ds->super);
    }

    /* if we want the argv's indexed, indicate that */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "index-argv-by-rank")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_INDEX_ARGV, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "map-by", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_MAPBY, pval->data.string, PMIX_STRING);
        prrte_list_append(&job_info, &ds->super);
    }

    /* if the user specified a ranking policy, then set it */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "rank-by", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_RANKBY, pval->data.string, PMIX_STRING);
        prrte_list_append(&job_info, &ds->super);
    }

    /* if the user specified a binding policy, then set it */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "bind-to", 0, 0))) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_BINDTO, pval->data.string, PMIX_STRING);
        prrte_list_append(&job_info, &ds->super);
    }

    if (prrte_cmd_line_is_taken(prrte_cmd_line, "report-bindings")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_REPORT_BINDINGS, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    /* mark if recovery was enabled on the cmd line */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "enable-recovery")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }
    /* record the max restarts */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "max-restarts", 0, 0)) &&
        0 < pval->data.integer) {
        ui32 = pval->data.integer;
        PRRTE_LIST_FOREACH(app, &apps, prrte_pmix_app_t) {
            ds = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(ds->info, 1);
            PMIX_INFO_LOAD(ds->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
            prrte_list_append(&app->info, &ds->super);
        }
    }
    /* if continuous operation was specified */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "continuous")) {
        /* mark this job as continuously operating */
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    /* pickup any relevant envars */
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    PRRTE_PMIX_CONVERT_JOBID(pname.nspace, PRRTE_PROC_MY_NAME->jobid);

    PRRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(pname.nspace, &info, 1, setupcbfunc, &mylock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        goto DONE;
    }
    PRRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n=0; n < mylock.ninfo; n++) {
            if (0 == strncmp(mylock.info[n].key, PMIX_SET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_ADD_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_UNSET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_PREPEND_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_APPEND_ENVAR, PMIX_MAX_KEYLEN)) {
                ds = PRRTE_NEW(prrte_ds_info_t);
                PMIX_INFO_CREATE(ds->info, 1);
                PMIX_INFO_XFER(&ds->info[0], &mylock.info[n]);
                prrte_list_append(&job_info, &ds->super);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }

    /* if we were launched by a tool wanting to direct our
     * operation, then we need to pause here and give it
     * a chance to tell us what we need to do */
    if (NULL != (param = getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL"))) {
        /* register for the PMIX_LAUNCH_DIRECTIVE event */
        PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIX_LAUNCH_DIRECTIVE;
        /* setup the myinfo object to capture the returned
         * values - must do so prior to registering in case
         * the event has already arrived */
        PRRTE_CONSTRUCT(&myinfo, myinfo_t);

        /* go ahead and register */
        PMIx_Register_event_handler(&ret, 1, NULL, 0, launchhandler, regcbfunc, &lock);
        PRRTE_PMIX_WAIT_THREAD(&lock);
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
        /* notify the tool that we are ready */
        ptr = strdup(param);
        param = strchr(ptr, ':');
        *param = '\0';
        ++param;
        (void)strncpy(controller.nspace, ptr, PMIX_MAX_NSLEN);
        controller.rank = strtoul(param, NULL, 10);
        /* do not cache this event - the tool is waiting for us */
        PMIX_INFO_CREATE(iptr, 2);
        flag = true;
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_DO_NOT_CACHE, &flag, PMIX_BOOL);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);

        PMIx_Notify_event(PMIX_LAUNCHER_READY, &pname, PMIX_RANGE_CUSTOM,
                          iptr, 2, NULL, NULL);
        /* now wait for the launch directives to arrive */
        PRRTE_PMIX_WAIT_THREAD(&myinfo.lock);
        PMIX_INFO_FREE(iptr, 2);

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
                        ds = PRRTE_NEW(prrte_ds_info_t);
                        ds->info = &iptr[m];
                        prrte_list_append(&job_info, &ds->super);
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                } else if (0 == strncmp(myinfo.info[n].key, PMIX_DEBUG_APP_DIRECTIVES, PMIX_MAX_KEYLEN)) {
                    /* there will be a pmix_data_array containing the directives */
                    iptr = (pmix_info_t*)myinfo.info[n].value.data.darray->array;
                    ninfo = myinfo.info[n].value.data.darray->size;
                    for (m=0; m < ninfo; m++) {
                        PRRTE_LIST_FOREACH(app, &apps, prrte_pmix_app_t) {
                            /* the value can only be on one list at a time, so replicate it */
                            ds = PRRTE_NEW(prrte_ds_info_t);
                            ds->info = &iptr[n];
                            prrte_list_append(&app->info, &ds->super);
                        }
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                }
            }
        }
    }

    /* convert the job info into an array */
    ninfo = prrte_list_get_size(&job_info);
    iptr = NULL;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(iptr, ninfo);
        n=0;
        PRRTE_LIST_FOREACH(ds, &job_info, prrte_ds_info_t) {
            PMIX_INFO_XFER(&iptr[n], ds->info);
            ++n;
        }
    }

    /* convert the apps to an array */
    napps = prrte_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    n = 0;
    PRRTE_LIST_FOREACH(app, &apps, prrte_pmix_app_t) {
        size_t fbar;

        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = prrte_argv_copy(app->app.argv);
        papps[n].env = prrte_argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        fbar = prrte_list_get_size(&app->info);
        if (0 < fbar) {
            papps[n].ninfo = fbar;
            PMIX_INFO_CREATE(papps[n].info, fbar);
            m = 0;
            PRRTE_LIST_FOREACH(ds, &app->info, prrte_ds_info_t) {
                PMIX_INFO_XFER(&papps[n].info[m], ds->info);
                ++m;
            }
        }
        ++n;
    }

    ret = PMIx_Spawn(iptr, ninfo, papps, napps, nspace);
    PRRTE_PMIX_CONVERT_NSPACE(rc, &myjobid, nspace);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (notify_launch) {
        /* direct an event back to our controller telling them
         * the namespace of the spawned job */
        PMIX_INFO_CREATE(iptr, 3);
        /* do not cache this event - the tool is waiting for us */
        flag = true;
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_DO_NOT_CACHE, &flag, PMIX_BOOL);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[2], PMIX_NSPACE, nspace, PMIX_STRING);
        PMIx_Notify_event(PMIX_LAUNCH_COMPLETE, &controller, PMIX_RANGE_CUSTOM,
                          iptr, 3, NULL, NULL);
    }
    /* push our stdin to the apps */
    PMIX_LOAD_PROCID(&pname, nspace, 0);  // forward stdin to rank=0
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(&pname, 1, NULL, iptr, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prrte_output(0, "IOF push of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
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
    (void)strncpy(pname.nspace, nspace, PMIX_MAX_NSLEN);
    pname.rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    /* request that they return our lock object */
    PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    /* do the registration */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&ret, 1, iptr, ninfo, evhandler, regcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (verbose) {
        prrte_output(0, "JOB %s EXECUTING", PRRTE_JOBID_PRINT(myjobid));
    }
    PRRTE_PMIX_WAIT_THREAD(&rellock);
    /* save the status */
    rc = rellock.status;
    /* output any message */
    if (NULL != rellock.msg) {
        fprintf(stderr, "%s\n", rellock.msg);
    }
    PRRTE_PMIX_DESTRUCT_LOCK(&rellock);

    /* if we lost connection to the server, then we are done */
    if (PMIX_ERR_LOST_CONNECTION_TO_SERVER == rc ||
        PMIX_ERR_UNREACH == rc) {
        goto DONE;
    }

    /* deregister our event handler */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* close the push of our stdin */
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, iptr, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prrte_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 1);
#endif

  DONE:
    if (proxyrun) {
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
        PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
        PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
         * being called when the server successes. The callback might be eventually
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
    }

    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PRRTE_SUCCESS == rc && PMIX_SUCCESS != ret) {
        rc = ret;
    }
    return rc;
}

static int parse_locals(prrte_list_t *jdata, int argc, char* argv[])
{
    int i, rc;
    int temp_argc;
    char **temp_argv, **env;
    prrte_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argc = 0;
    temp_argv = NULL;
    prrte_argv_append(&temp_argc, &temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
       create_app(), below.  See comment immediately before the
       create_app() function for an explanation. */

    env = NULL;
    for (i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (prrte_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    prrte_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
                if (PRRTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                       printed; no need to cleanup -- we can just
                       exit */
                    exit(1);
                }
                if (made_app) {
                    prrte_list_append(jdata, &app->super);
                }

                /* Reset the temps */

                temp_argc = 0;
                temp_argv = NULL;
                prrte_argv_append(&temp_argc, &temp_argv, argv[0]);
            }
        } else {
            prrte_argv_append(&temp_argc, &temp_argv, argv[i]);
        }
    }

    if (prrte_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
        if (PRRTE_SUCCESS != rc) {
            /* Assume that the error message has already been printed;
               no need to cleanup -- we can just exit */
            exit(1);
        }
        if (made_app) {
            prrte_list_append(jdata, &app->super);
        }
    }
    if (NULL != env) {
        prrte_argv_free(env);
    }
    prrte_argv_free(temp_argv);

    /* All done */

    return PRRTE_SUCCESS;
}


/*
 * This function takes a "char ***app_env" parameter to handle the
 * specific case:
 *
 *   prrterun --mca foo bar -app appfile
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
 *   prrterun --mca foo bar -np 4 hostname
 *
 * Then the upper-level function (parse_locals()) calls create_app()
 * with a NULL value for app_env, meaning that there is no "base"
 * environment that the app needs to be created from.
 */
static int create_app(int argc, char* argv[],
                      prrte_list_t *jdata,
                      prrte_pmix_app_t **app_ptr,
                      bool *made_app, char ***app_env)
{
    char cwd[PRRTE_PATH_MAX];
    int i, j, count, rc;
    char *param, *value;
    prrte_pmix_app_t *app = NULL;
    bool found = false;
    char *appname = NULL;
    prrte_ds_info_t *val;
    prrte_value_t *pvalue;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_parse(prrte_cmd_line, true, false,
                                                    argc, argv)) ) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(rc));
        }
        return rc;
    }

    /* Setup application context */
    app = PRRTE_NEW(prrte_pmix_app_t);
    prrte_cmd_line_get_tail(prrte_cmd_line, &count, &app->app.argv);

    /* See if we have anything left */
    if (0 == count) {
        prrte_show_help("help-prrterun.txt", "prrterun:executable-not-specified",
                       true, "prun", "prun");
        rc = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* set necessary env variables for external usage from tune conf file */
    int set_from_file = 0;
    char **vars = NULL;
    if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list_from_file(&vars) &&
            NULL != vars) {
        for (i=0; NULL != vars[i]; i++) {
            value = strchr(vars[i], '=');
            /* terminate the name of the param */
            *value = '\0';
            /* step over the equals */
            value++;
            /* overwrite any prior entry */
            prrte_setenv(vars[i], value, true, &app->app.env);
            /* save it for any comm_spawn'd apps */
            prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
        }
        set_from_file = 1;
        prrte_argv_free(vars);
    }
    /* Did the user request to export any environment variables on the cmd line? */
    char *env_set_flag;
    env_set_flag = getenv("PRRTE_MCA_mca_base_env_list");
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "x")) {
        if (NULL != env_set_flag) {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
        j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "x");
        for (i = 0; i < j; ++i) {
            pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "x", i, 0);
            param = pvalue->data.string;

            if (NULL != (value = strchr(param, '='))) {
                /* terminate the name of the param */
                *value = '\0';
                /* step over the equals */
                value++;
                /* overwrite any prior entry */
                prrte_setenv(param, value, true, &app->app.env);
                /* save it for any comm_spawn'd apps */
                prrte_setenv(param, value, true, &prrte_forwarded_envars);
            } else {
                value = getenv(param);
                if (NULL != value) {
                    /* overwrite any prior entry */
                    prrte_setenv(param, value, true, &app->app.env);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(param, value, true, &prrte_forwarded_envars);
                } else {
                    prrte_output(0, "Warning: could not find environment variable \"%s\"\n", param);
                }
            }
        }
    } else if (NULL != env_set_flag) {
        /* if mca_base_env_list was set, check if some of env vars were set via -x from a conf file.
         * If this is the case, error out.
         */
        if (!set_from_file) {
            /* set necessary env variables for external usage */
            vars = NULL;
            if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list(env_set_flag, &vars) &&
                    NULL != vars) {
                for (i=0; NULL != vars[i]; i++) {
                    value = strchr(vars[i], '=');
                    /* terminate the name of the param */
                    *value = '\0';
                    /* step over the equals */
                    value++;
                    /* overwrite any prior entry */
                    prrte_setenv(vars[i], value, true, &app->app.env);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
                }
                prrte_argv_free(vars);
            }
        } else {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
    }

    /* Did the user request a specific wdir? */

    if (NULL != (pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "wdir", 0, 0))) {
        param = pvalue->data.string;
        /* if this is a relative path, convert it to an absolute path */
        if (prrte_path_is_absolute(param)) {
            app->app.cwd = strdup(param);
        } else {
            /* get the cwd */
            if (PRRTE_SUCCESS != (rc = prrte_getcwd(cwd, sizeof(cwd)))) {
                prrte_show_help("help-prrterun.txt", "prrterun:init-failure",
                               true, "get the cwd", rc);
                goto cleanup;
            }
            /* construct the absolute path */
            app->app.cwd = prrte_os_path(false, cwd, param, NULL);
        }
    } else if (prrte_cmd_line_is_taken(prrte_cmd_line, "set-cwd-to-session-dir")) {
        val = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
        prrte_list_append(&app->info, &val->super);
    } else {
        if (PRRTE_SUCCESS != (rc = prrte_getcwd(cwd, sizeof(cwd)))) {
            prrte_show_help("help-prrterun.txt", "prrterun:init-failure",
                           true, "get the cwd", rc);
            goto cleanup;
        }
        app->app.cwd = strdup(cwd);
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* if they specified a process set name, then pass it along */
    if (NULL != (pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "pset", 0, 0))) {
        val = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_PSET_NAME, pvalue->data.string, PMIX_STRING);
        prrte_list_append(&app->info, &val->super);
    }
#endif

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    found = false;
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "hostfile"))) {
        if (1 < j) {
            prrte_show_help("help-prrterun.txt", "prrterun:multiple-hostfiles",
                           true, "prun", NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "hostfile", 0, 0);
            val = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, pvalue->data.string, PMIX_STRING);
            prrte_list_append(&app->info, &val->super);
            found = true;
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "machinefile"))) {
        if (1 < j || found) {
            prrte_show_help("help-prrterun.txt", "prrterun:multiple-hostfiles",
                           true, "prun", NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "machinefile", 0, 0);
            val = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, pvalue->data.string, PMIX_STRING);
            prrte_list_append(&app->info, &val->super);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "host", i, 0);
            prrte_argv_append_nosize(&targ, pvalue->data.string);
        }
        tval = prrte_argv_join(targ, ',');
        val = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_HOST, tval, PMIX_STRING);
        prrte_list_append(&app->info, &val->super);
        free(tval);
    }

    /* check for bozo error */
    if (NULL != (pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "np", 0, 0)) ||
        NULL != (pvalue = prrte_cmd_line_get_param(prrte_cmd_line, "n", 0, 0))) {
        if (0 > pvalue->data.integer) {
            prrte_show_help("help-prrterun.txt", "prrterun:negative-nprocs",
                           true, "prun", app->app.argv[0],
                           pvalue->data.integer, NULL);
            return PRRTE_ERR_FATAL;
        }
    }
    if (NULL == pvalue) {
        prrte_output(0, "NO NP");
        return PRRTE_ERR_FATAL;
    }

    app->app.maxprocs = pvalue->data.integer;

    /* see if we need to preload the binary to
     * find the app - don't do this for java apps, however, as we
     * can't easily find the class on the cmd line. Java apps have to
     * preload their binary via the preload_files option
     */
    if (NULL == strstr(app->app.argv[0], "java")) {
        if (prrte_cmd_line_is_taken(prrte_cmd_line, "preload-binaries")) {
            val = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
            prrte_list_append(&app->info, &val->super);
            val = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
            prrte_list_append(&app->info, &val->super);
        }
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "preload-files")) {
        val = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_FILES, NULL, PMIX_BOOL);
        prrte_list_append(&app->info, &val->super);
    }

    /* Do not try to find argv[0] here -- the starter is responsible
       for that because it may not be relevant to try to find it on
       the node where prrterun is executing.  So just strdup() argv[0]
       into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        prrte_show_help("help-prrterun.txt", "prrterun:call-failed",
                       true, "prun", "library", "strdup returned NULL", errno);
        rc = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* if this is a Java application, we have a bit more work to do. Such
     * applications actually need to be run under the Java virtual machine
     * and the "java" command will start the "executable". So we need to ensure
     * that all the proper java-specific paths are provided
     */
    appname = prrte_basename(app->app.cmd);
    if (0 == strcmp(appname, "java")) {
        /* see if we were given a library path */
        found = false;
        for (i=1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "java.library.path")) {
                char *dptr;
                /* find the '=' that delineates the option from the path */
                if (NULL == (dptr = strchr(app->app.argv[i], '='))) {
                    /* that's just wrong */
                    rc = PRRTE_ERR_BAD_PARAM;
                    goto cleanup;
                }
                /* step over the '=' */
                ++dptr;
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                if (NULL == strstr(app->app.argv[i], prrte_install_dirs.libdir)) {
                    /* doesn't appear to - add it to be safe */
                    if (':' == app->app.argv[i][strlen(app->app.argv[i]-1)]) {
                        prrte_asprintf(&value, "-Djava.library.path=%s%s", dptr, prrte_install_dirs.libdir);
                    } else {
                        prrte_asprintf(&value, "-Djava.library.path=%s:%s", dptr, prrte_install_dirs.libdir);
                    }
                    free(app->app.argv[i]);
                    app->app.argv[i] = value;
                }
                break;
            }
        }
        if (!found) {
            /* need to add it right after the java command */
            prrte_asprintf(&value, "-Djava.library.path=%s", prrte_install_dirs.libdir);
            prrte_argv_insert_element(&app->app.argv, 1, value);
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
                value = prrte_os_path(false, prrte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "mpi.jar");
                }
                free(value);
                /* check for oshmem support */
                value = prrte_os_path(false, prrte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "shmem.jar");
                }
                free(value);
                /* always add the local directory */
                prrte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[i+1]);
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
                    prrte_argv_insert_element(&app->app.argv, 1, value);
                    /* check for mpi.jar */
                    value = prrte_os_path(false, prrte_install_dirs.libdir, "mpi.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "mpi.jar");
                    }
                    free(value);
                    /* check for shmem.jar */
                    value = prrte_os_path(false, prrte_install_dirs.libdir, "shmem.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "shmem.jar");
                    }
                    free(value);
                    /* always add the local directory */
                    prrte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[1]);
                    free(app->app.argv[1]);
                    app->app.argv[1] = value;
                    prrte_argv_insert_element(&app->app.argv, 1, "-cp");
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
                value = prrte_os_path(false, prrte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    prrte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                /* check for shmem.jar */
                value = prrte_os_path(false, prrte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    prrte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                prrte_argv_insert_element(&app->app.argv, 1, str);
                free(str);
                prrte_argv_insert_element(&app->app.argv, 1, "-cp");
            }
        }
    }

    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

  cleanup:
    if (NULL != app) {
        PRRTE_RELEASE(app);
    }
    if (NULL != appname) {
        free(appname);
    }
    return rc;
}

static void set_classpath_jar_file(prrte_pmix_app_t *app, int index, char *jarfile)
{
    if (NULL == strstr(app->app.argv[index], jarfile)) {
        /* nope - need to add it */
        char *fmt = ':' == app->app.argv[index][strlen(app->app.argv[index]-1)]
                    ? "%s%s/%s" : "%s:%s/%s";
        char *str;
        prrte_asprintf(&str, fmt, app->app.argv[index], prrte_install_dirs.libdir, jarfile);
        free(app->app.argv[index]);
        app->app.argv[index] = str;
    }
}

static void clean_abort(int fd, short flags, void *arg)
{
    pmix_proc_t target;
    pmix_info_t directive;

    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (prrte_atomic_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr, "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prrte_event_add(&term_handler, NULL);
        return;
    }

    /* tell PRRTE to terminate our job */
    PRRTE_PMIX_CONVERT_JOBID(target.nspace, myjobid);
    target.rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL)) {
        PMIx_tool_finalize();
        /* exit with a non-zero status */
        exit(1);
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
        write(1, (void*)msg, strlen(msg));
    }
    /* save the time */
    last.tv_sec = current.tv_sec;
    /* tell the event lib to attempt to abnormally terminate */
    write(term_pipe[1], &foo, 1);
}
