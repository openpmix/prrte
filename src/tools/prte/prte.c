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
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/base/base.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/cmd_line.h"
#include "src/util/daemon_init.h"
#include "src/util/fd.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/prrte_environ.h"
#include "src/util/prrte_getcwd.h"
#include "src/util/show_help.h"
#include "src/sys/atomic.h"

#include "src/runtime/prrte_progress_threads.h"
#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"

#include "src/prted/prted.h"
#include "src/prted/pmix/pmix_server_internal.h"

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
static pmix_nspace_t spawnednspace;

static int create_app(int argc, char* argv[],
                      prrte_list_t *jdata,
                      prrte_pmix_app_t **app,
                      bool *made_app, char ***app_env);
static int parse_locals(prrte_list_t *jdata, int argc, char* argv[]);
static void set_classpath_jar_file(prrte_pmix_app_t *app, int index, char *jarfile);
static bool verbose = false;
static prrte_cmd_line_t *prrte_cmd_line = NULL;
static bool want_prefix_by_default = (bool) PRRTE_WANT_PRRTE_PREFIX_BY_DEFAULT;

/* prun-specific options */
static prrte_cmd_line_init_t cmd_line_init[] = {

    /* DVM options */
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
    /* forward signals */
    { '\0', "forward-signals", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of additional signals (names or integers) to forward to "
      "application processes [\"none\" => forward nothing]. Signals provided by "
      "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
      PRRTE_CMD_LINE_OTYPE_DVM},


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
    { '\0', "debug", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Invoke the indicated user-level debugger (provide a comma-delimited list of debuggers to search for)",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "output-proctable", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Print the complete proctable to stdout [-], stderr [+], or a file [anything else] after launch",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "stop-on-exec", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "If supported, stop each process at start of execution",
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
    { 'x', NULL, 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the environment variable foo and takes its value from the current environment; \"-x foo=bar\" exports the environment variable name foo and sets its value to \"bar\" in the started processes; \"-x foo*\" exports all current environmental variables starting with \"foo\")",
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


static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_ACQUIRE_OBJECT(lock);
    PRRTE_PMIX_WAKEUP_THREAD(lock);
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

static void spcbfunc(pmix_status_t status,
                     char nspace[], void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(lock);
    lock->status = status;
    if (PMIX_SUCCESS == status) {
        lock->msg = strdup(nspace);
    }
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}


static int wait_pipe[2];

static int wait_dvm(pid_t pid) {
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

int main(int argc, char *argv[])
{
    int rc=1, i, j;
    char *param, *ptr, *tpath;
    prrte_pmix_lock_t lock;
    prrte_list_t apps;
    prrte_pmix_app_t *app;
    pmix_info_t info, *iptr;
    pmix_proc_t pname, controller;
    pmix_status_t ret;
    bool flag;
    prrte_ds_info_t *ds;
    size_t m, n, ninfo, param_len;
    pmix_app_t *papps;
    size_t napps;
    mylock_t mylock;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    bool notify_launch = false;
#endif
    prrte_value_t *pval;
    uint32_t ui32;
    char *rfile = NULL;
    char *mytmpdir;
    char **pargv;
    int pargc;
    char **tmp;
    prrte_job_t *jdata;
    prrte_app_context_t *dapp;

    /* init the globals */
    PRRTE_CONSTRUCT(&job_info, prrte_list_t);
    PRRTE_CONSTRUCT(&apps, prrte_list_t);

    /* init the tiny part of PRRTE we use */
    prrte_init_util();

    prrte_tool_basename = prrte_basename(argv[0]);
    pargc = argc;
    pargv = prrte_argv_copy(argv);

    /* because we have to use the schizo framework prior to parsing the
     * incoming argv for cmd line options, do a hacky search to support
     * passing of options (e.g., verbosity) for schizo */
    for (i=1; NULL != argv[i]; i++) {
        if (0 == strncmp(argv[i], "schizo", 6)) {
            prrte_asprintf(&param, "PRRTE_MCA_%s", argv[i]);
            prrte_setenv(param, argv[i+1], true, &environ);
            free(param);
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
    /* setup our common personalities */
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "prrte");
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "pmix");
    /* add anything they specified */
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            tmp = prrte_argv_split(argv[i+1], ',');
            for (m=0; NULL != tmp[m]; m++) {
                prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, tmp[m]);
            }
            prrte_argv_free(tmp);
        }
    }

    /* detect if we are running as a proxy and setup the rendezvous
     * and session directory files */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.detect_proxy(pargv, &rfile))) {
        if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* get our session directory */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.define_session_dir(&mytmpdir))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* setup the rest of the cmd line only once */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.define_cli(prrte_cmd_line))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* handle deprecated options */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.parse_deprecated_cli(prrte_cmd_line, &pargc, &pargv))) {
        if (PRRTE_OPERATION_SUCCEEDED == rc) {
            /* the cmd line was restructured - show them the end result */
            param = prrte_argv_join(pargv, ' ');
            fprintf(stderr, "\n******* Corrected cmd line: %s\n\n\n", param);
            free(param);
        } else {
            return rc;
        }
    }

    /* parse the result to get values - this will not include MCA params */
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_parse(prrte_cmd_line,
                                                    true, false, pargc, pargv)) ) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n",
                    prrte_tool_basename,
                    prrte_strerror(rc));
        }
       return rc;
    }

    /* let the schizo components take a pass at it to get the MCA params - this
     * will include whatever default/user-level param files each schizo component
     * supports */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.parse_cli(pargc, 0, pargv, NULL, NULL))) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n",
                    prrte_tool_basename,
                    prrte_strerror(rc));
        }
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

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid()) {
        prrte_schizo.allow_run_as_root(prrte_cmd_line);  // will exit us if not allowed
    }

    /* Check for help request */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "help")) {
        char *str, *args = NULL;
        args = prrte_cmd_line_get_usage_msg(prrte_cmd_line, false);
        str = prrte_show_help_string("help-prun.txt", "prun:usage", false,
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

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    prrte_debug_flag = prrte_cmd_line_is_taken(prrte_cmd_line, "debug");
    prrte_debug_daemons_flag = prrte_cmd_line_is_taken(prrte_cmd_line, "debug-daemons");
    if (!prrte_debug_flag &&
        !prrte_debug_daemons_flag &&
        prrte_cmd_line_is_taken(prrte_cmd_line, "daemonize")) {
        pipe(wait_pipe);
        prrte_state_base_parent_fd = wait_pipe[1];
        prrte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    } else {
#if defined(HAVE_SETSID)
        /* see if we were directed to separate from current session */
        if (prrte_cmd_line_is_taken(prrte_cmd_line, "set-sid")) {
            setsid();
        }
#endif
    }

    if (prrte_cmd_line_is_taken(prrte_cmd_line, "no-ready-msg")) {
        prrte_state_base_ready_msg = false;
    }

    if (prrte_cmd_line_is_taken(prrte_cmd_line, "system-server")) {
        /* we should act as system-level PMIx server */
        prrte_setenv("PRRTE_MCA_pmix_system_server", "1", true, &environ);
    }
    /* always act as session-level PMIx server */
    prrte_setenv("PRRTE_MCA_pmix_session_server", "1", true, &environ);
    /* if we were asked to report a uri, set the MCA param to do so */
     if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "report-uri", 0, 0))) {
        prrte_setenv("PMIX_MCA_ptl_tcp_report_uri", pval->data.string, true, &environ);
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "remote-tools")) {
        prrte_setenv("PMIX_MCA_ptl_tcp_remote_connections", "1", true, &environ);
    }
    /* don't aggregate help messages as that will apply job-to-job */
    prrte_setenv("PRRTE_MCA_prrte_base_help_aggregate", "0", true, &environ);

    /* Setup MCA params */
    prrte_register_params();

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars were set in the enviro prior to calling
     * prun
     */
    prrte_launch_environ = prrte_argv_copy(environ);

    /* setup PRRTE infrastructure */
    if (PRRTE_SUCCESS != (ret = prrte_init(&pargc, &pargv, PRRTE_PROC_MASTER))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    /* if we were launched by a tool wanting to direct our
     * operation, then we need to pause here and give it
     * a chance to tell us what we need to do - this needs to be
     * done prior to starting the DVM as it may include instructions
     * on the daemon executable, the fork/exec agent to be used by
     * the daemons, or other directives impacting the DVM itself */
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
        PMIX_INFO_CREATE(iptr, 1);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);
        PMIx_Notify_event(PMIX_LAUNCHER_READY, &pname, PMIX_RANGE_CUSTOM,
                          iptr, 1, NULL, NULL);
        /* now wait for the launch directives to arrive */
        while (prrte_event_base_active && myinfo.lock.active) {
            prrte_event_loop(prrte_event_base, PRRTE_EVLOOP_ONCE);
        }
        PMIX_INFO_FREE(iptr, 1);

        /* process the returned directives */
        if (NULL != myinfo.info) {
            for (n=0; n < myinfo.ninfo; n++) {

                if (PMIX_CHECK_KEY(&myinfo.info[n], PMIX_DEBUG_JOB_DIRECTIVES)) {
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

    /* start the DVM */

     /* get the daemon job object - was created by ess/hnp component */
    if (NULL == (jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        prrte_show_help("help-prun.txt", "bad-job-object", true,
                       prrte_tool_basename);
        exit(0);
    }
    /* also should have created a daemon "app" */
    if (NULL == (dapp = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0))) {
        prrte_show_help("help-prun.txt", "bad-app-object", true,
                       prrte_tool_basename);
        exit(0);
    }

    /* Did the user specify a prefix, or want prefix by default? */
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "prefix", 0, 0)) || want_prefix_by_default) {
        if (NULL != pval) {
            param = strdup(pval->data.string);
        } else {
            /* --enable-prun-prefix-default was given to prun */
            param = strdup(prrte_install_dirs.prefix);
        }
        /* "Parse" the param, aka remove superfluous path_sep. */
        param_len = strlen(param);
        while (0 == strcmp (PRRTE_PATH_SEP, &(param[param_len-1]))) {
            param[param_len-1] = '\0';
            param_len--;
            if (0 == param_len) {
                prrte_show_help("help-prun.txt", "prun:empty-prefix",
                               true, prrte_tool_basename, prrte_tool_basename);
                return PRRTE_ERR_FATAL;
            }
        }
        prrte_set_attribute(&dapp->attributes, PRRTE_APP_PREFIX_DIR, PRRTE_ATTR_GLOBAL, param, PRRTE_STRING);
        free(param);
    } else {
        /* Check if called with fully-qualified path to prte.
           (Note: Put this second so can override with --prefix (above). */
        tpath = NULL;
        if ('/' == argv[0][0] ) {
            char *tmp_basename = NULL;
            tpath = prrte_dirname(argv[0]);

            if( NULL != tpath ) {
                /* Quick sanity check to ensure we got
                   something/bin/<exec_name> and that the installation
                   tree is at least more or less what we expect it to
                   be */
                tmp_basename = prrte_basename(tpath);
                if (0 == strcmp("bin", tmp_basename)) {
                    char* tmp = tpath;
                    tpath = prrte_dirname(tmp);
                    free(tmp);
                } else {
                    free(tpath);
                    tpath = NULL;
                }
                free(tmp_basename);
            }
            prrte_set_attribute(&dapp->attributes, PRRTE_APP_PREFIX_DIR, PRRTE_ATTR_GLOBAL, tpath, PRRTE_STRING);
        }
    }

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "hostfile"))) {
        if(1 < j) {
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pval = prrte_cmd_line_get_param(prrte_cmd_line, "hostfile", 0, 0);
            prrte_set_attribute(&dapp->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, pval->data.string, PRRTE_STRING);
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "machinefile"))) {
        if(1 < j || prrte_get_attribute(&dapp->attributes, PRRTE_APP_HOSTFILE, NULL, PRRTE_STRING)) {
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pval = prrte_cmd_line_get_param(prrte_cmd_line, "machinefile", 0, 0);
            prrte_set_attribute(&dapp->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, pval->data.string, PRRTE_STRING);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prrte_cmd_line_get_ninsts(prrte_cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            pval = prrte_cmd_line_get_param(prrte_cmd_line, "host", i, 0);
            prrte_argv_append_nosize(&targ, pval->data.string);
        }
        tval = prrte_argv_join(targ, ',');
        prrte_set_attribute(&dapp->attributes, PRRTE_APP_DASH_HOST, PRRTE_ATTR_LOCAL, tval, PRRTE_STRING);
        prrte_argv_free(targ);
        free(tval);
    }

    /* setup to listen for commands sent specifically to me, even though I would probably
     * be the one sending them! Unfortunately, since I am a participating daemon,
     * there are times I need to send a command to "all daemons", and that means *I* have
     * to receive it too
     */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DAEMON,
                            PRRTE_RML_PERSISTENT, prrte_daemon_recv, NULL);

    /* spawn the DVM - we skip the initial steps as this
     * isn't a user-level application */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOCATE);

    if (prrte_cmd_line_is_taken(prrte_cmd_line, "daemonize")) {
        /* cannot be any apps */
        goto proceed;
    }

    /* see if they want to run an application - let's parse
     * the cmd line to get it */
    if (PRRTE_SUCCESS != (rc = parse_locals(&apps, pargc, pargv))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_LIST_DESTRUCT(&apps);
        goto DONE;
    }

    /* did they provide an app? */
    if (0 == prrte_list_get_size(&apps)) {
        /* nope - just need to wait for instructions */
        goto proceed;
    }
    /* mark that we are not a persistent DVM */
    prrte_persistent = false;

    /* pass the personality */
    for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
        tmp = NULL;
        if (0 != strcmp(prrte_schizo_base.personalities[i], "prrte") &&
            0 != strcmp(prrte_schizo_base.personalities[i], "pmix")) {
            prrte_argv_append_nosize(&tmp, prrte_schizo_base.personalities[i]);
        }
        if (NULL != tmp) {
            param = prrte_argv_join(tmp, ',');
            prrte_argv_free(tmp);
            ds = PRRTE_NEW(prrte_ds_info_t);
            PMIX_INFO_CREATE(ds->info, 1);
            PMIX_INFO_LOAD(ds->info, PMIX_PERSONALITY, param, PMIX_STRING);
            free(param);
            prrte_list_append(&job_info, &ds->super);
        }
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
            if (NULL == getcwd(cwd, sizeof(cwd))) {
                return PRRTE_ERR_FATAL;
            }
            ptr = prrte_os_path(false, cwd, param, NULL);
        } else {
            ptr = strdup(param);
        }
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_FILE, ptr, PMIX_STRING);
        free(ptr);
        prrte_list_append(&job_info, &ds->super);
    } else if (NULL != ptr) {
#if PMIX_NUMERIC_VERSION >= 0x00040000
        /* if we were asked to output to a directory, pass it along. */
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        /* If the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!prrte_path_is_absolute(ptr)) {
            char cwd[PRRTE_PATH_MAX];
            if (NULL == getcwd(cwd, sizeof(cwd))) {
                return PRRTE_ERR_FATAL;
            }
            param = prrte_os_path(false, cwd, ptr, NULL);
        } else {
            param = strdup(ptr);
        }
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_DIRECTORY, param, PMIX_STRING);
        free(param);
#endif
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

    /* if stop-on-exec was specified */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "stop-on-exec")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_DEBUG_STOP_ON_EXEC, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    param = NULL;
    if (NULL != (pval = prrte_cmd_line_get_param(prrte_cmd_line, "timeout", 0, 0)) ||
        NULL != (param = getenv("MPIEXEC_TIMEOUT"))) {
        if (NULL != param) {
            i = strtol(param, NULL, 10);
            /* both cannot be present, or they must agree */
            if (NULL != pval && i != pval->data.integer) {
                prrte_show_help("help-prun.txt", "prun:timeoutconflict", false,
                               prrte_tool_basename, pval->data.integer, param);
                exit(1);
            }
        } else {
            i = pval->data.integer;
        }
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT, &i, PMIX_INT);
        prrte_list_append(&job_info, &ds->super);
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "get-stack-traces")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT_STACKTRACES, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "report-state-on-timeout")) {
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMEOUT_REPORT_STATE, &flag, PMIX_BOOL);
        prrte_list_append(&job_info, &ds->super);
    }

    /* give the schizo components a chance to add to the job info */
    prrte_schizo.job_info(prrte_cmd_line, &job_info);

    /* pickup any relevant envars */
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);

    PRRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prrte_process_info.myproc.nspace, &info, 1, setupcbfunc, &mylock);
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
        /* pickup any relevant envars */
        prrte_schizo.parse_env(prrte_cmd_line, environ, &papps[n].env, false);
        ++n;
    }

    if (verbose) {
        prrte_output(0, "Spawning job");
    }

    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    pmix_server_spawn_fn(&prrte_process_info.myproc, iptr, ninfo, papps, napps, spcbfunc, &lock);
    if (PRRTE_SUCCESS != ret) {
        prrte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        goto DONE;
    }
    /* we have to cycle the event library here so we can process
     * the spawn request */
    while (prrte_event_base_active && lock.active) {
        prrte_event_loop(prrte_event_base, PRRTE_EVLOOP_ONCE);
    }
    PRRTE_ACQUIRE_OBJECT(&lock.lock);
    if (PMIX_SUCCESS != lock.status) {
        goto DONE;
    }
    PMIX_LOAD_NSPACE(spawnednspace, lock.msg);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    PRRTE_PMIX_CONVERT_NSPACE(rc, &myjobid, spawnednspace);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (notify_launch) {
        /* direct an event back to our controller telling them
         * the namespace of the spawned job */
        PMIX_INFO_CREATE(iptr, 2);
        /* target this notification solely to that one tool */
        PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_CUSTOM_RANGE, &controller, PMIX_PROC);
        /* pass the nspace of the spawned job */
        PMIX_INFO_LOAD(&iptr[1], PMIX_NSPACE, spawnednspace, PMIX_STRING);
        PMIx_Notify_event(PMIX_LAUNCH_COMPLETE, &prrte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          iptr, 2, NULL, NULL);
        PMIX_INFO_FREE(iptr, 2);
    }
#endif


    if (verbose) {
        prrte_output(0, "JOB %s EXECUTING", PRRTE_JOBID_PRINT(myjobid));
    }

  proceed:
    /* loop the event lib until an exit event is detected */
    while (prrte_event_base_active) {
        prrte_event_loop(prrte_event_base, PRRTE_EVLOOP_ONCE);
    }
    PRRTE_ACQUIRE_OBJECT(prrte_event_base_active);

  DONE:
    /* cleanup and leave */
    prrte_finalize();

    if (prrte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prrte_exit_status);
    }
    exit(prrte_exit_status);
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
                       printed; */
                    return rc;
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
            return rc;
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
        prrte_show_help("help-prun.txt", "prun:executable-not-specified",
                       true, "prun", "prun");
        rc = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
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
                prrte_show_help("help-prun.txt", "prun:init-failure",
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
            prrte_show_help("help-prun.txt", "prun:init-failure",
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
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
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
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
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
            prrte_show_help("help-prun.txt", "prun:negative-nprocs",
                           true, "prun", app->app.argv[0],
                           pvalue->data.integer, NULL);
            return PRRTE_ERR_FATAL;
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
       the node where prun is executing.  So just strdup() argv[0]
       into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        prrte_show_help("help-prun.txt", "prun:call-failed",
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
