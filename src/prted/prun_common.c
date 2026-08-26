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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/mca/base/pmix_base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/pmix_mutex.h"
#include "src/util/daemon_init.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/prted/prted.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/runtime.h"

#include "src/prted/prted.h"

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static pmix_nspace_t spawnednspace;

/* The application exit status the DVM reported for our job, or -1 if it
 * reported none.  This is what we exit with when we have it, so that
 * "prun ... false" answers 1 the way "prterun ... false" does.  The
 * job-termination status cannot serve: it is a pmix_status_t naming the
 * *reason* the job ended, and passing that to exit() is how every failed
 * job used to come back as 71 - the low byte of PRTE_ERROR. */
static int job_exit_code = -1;

/* Our own job's termination is not the end of the work we started: a job it
 * spawned outlives it by default, and prterun - which runs its own DVM and
 * shuts it down only once every job in it has terminated - waits for the
 * whole tree and returns what the tree came back with.  We are the same
 * launcher talking to a DVM somebody else owns, so we have to wait for the
 * same thing by watching for it.  The DVM stamps each PMIX_EVENT_JOB_END
 * with the root of the spawn tree the job belonged to and the number of jobs
 * in that tree still running; what follows is what we make of that.
 *
 * Whether a child job's status is reported on its own instead of becoming
 * ours.  Read from our command line before the spawn, on the main thread. */
static bool report_child_sep = false;

/* The first non-zero termination status seen anywhere in the tree, and the
 * first message that came with one - what we hand the waiting main thread
 * once the tree has drained.
 *
 * These three, and the list below, are written by the event handler on the
 * PMIx progress thread and read by the main thread only after the wait that
 * handler wakes has completed and the handler has been deregistered.  None
 * of them is a PRRTE object, so nothing here is touched from the wrong
 * thread. */
static int status_from_tree = 0;
static char *tree_msg = NULL;

/* a child job's exit status, held for reporting once we are back on the main
 * thread - show_help is not ours to call from the PMIx progress thread */
typedef struct child_status_t {
    struct child_status_t *next;
    pmix_nspace_t nspace;
    int code;
} child_status_t;
static child_status_t *child_statuses = NULL;

static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool verbose = false;
static pmix_list_t forwarded_signals;

static void signal_forward_callback(int signal);

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PMIX_ACQUIRE_OBJECT(lock);
    evid = ref;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PMIX_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

/* the release lock the main thread parks on while the job runs -
 * the default handler must be able to wake it even if an event
 * fails to carry the registered return object */
static prte_pmix_lock_t *release_lock = NULL;

static void defhandler(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    size_t n;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, source, results, nresults);

    if (verbose) {
        pmix_output(0, "PRUN: DEFHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* find the lock we are to release - if the event did not carry
     * it, fall back to the release lock we registered */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
            }
        }
    }
    if (NULL == lock) {
        lock = release_lock;
    }

    if (PMIX_ERR_IOF_FAILURE == status) {
        pmix_proc_t target;
        pmix_info_t directive;

        /* tell PRTE to terminate our job */
        PMIX_LOAD_PROCID(&target, prte_process_info.myproc.nspace, PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
        rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            /* we cannot terminate the job. This handler executes on
             * the PMIx progress thread, so we must not finalize the
             * library or exit from here - record the error and wake
             * the main thread, which owns the shutdown path */
            if (NULL != lock) {
                lock->status = rc;
                if (NULL == lock->msg) {
                    lock->msg = strdup("failed to terminate job after IOF failure");
                }
                PRTE_PMIX_WAKEUP_THREAD(lock);
            }
        }
        goto progress;
    }

    if (PMIX_ERR_UNREACH == status || PMIX_ERR_LOST_CONNECTION == status) {
        if (NULL != lock) {
            /* save the status */
            lock->status = status;
            /* release the lock */
            PRTE_PMIX_WAKEUP_THREAD(lock);
        }
    }
progress:
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* Is JOBID, whose end the DVM just reported with spawn-tree root ROOT, part
 * of the tree we launched?
 *
 * Our own job is, by name.  Beyond that a descendant is recognized by its
 * root, which for anything we started is either the namespace we hold as a
 * tool - the DVM builds a job object for a connected tool and roots the tree
 * there - or, where it did not, our job's own namespace.  Everything else
 * belongs to another user of the same persistent DVM and is none of our
 * business: an unfiltered handler is how we get to see our descendants at
 * all, and this is what keeps it from making us wait on strangers. */
static bool in_our_tree(const char *jobid, const char *root)
{
    if (0 == strncmp(jobid, spawnednspace, PMIX_MAX_NSLEN)) {
        return true;
    }
    if (NULL == root || '\0' == root[0]) {
        return false;
    }
    return (0 == strncmp(root, myproc.nspace, PMIX_MAX_NSLEN) ||
            0 == strncmp(root, spawnednspace, PMIX_MAX_NSLEN));
}

static void evhandler(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    int jobstatus = 0, xcode = -1;
    pmix_nspace_t jobid = {0};
    char *root = NULL;
    uint32_t active = 0;
    bool primary;
    child_status_t *cs;
    size_t n;
    char *msg = NULL;
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, source, results, nresults);

    if (verbose) {
        pmix_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = prte_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
                /* the application's own status - preferred over the
                 * termination reason when we come to exit */
                xcode = info[n].value.data.integer;
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN) &&
                       NULL != info[n].value.data.proc) {
                PMIX_LOAD_NSPACE(jobid, info[n].value.data.proc->nspace);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
#ifdef PMIX_SPAWN_TREE_ROOT
            } else if (0 == strncmp(info[n].key, PMIX_SPAWN_TREE_ROOT, PMIX_MAX_KEYLEN)) {
                root = info[n].value.data.string;
            } else if (0 == strncmp(info[n].key, PMIX_SPAWN_TREE_ACTIVE, PMIX_MAX_KEYLEN)) {
                active = info[n].value.data.uint32;
#endif
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
            }
        }
    }

    if (!in_our_tree(jobid, root)) {
        /* somebody else's job on a DVM we share */
        goto progress;
    }
    primary = (0 == strncmp(jobid, spawnednspace, PMIX_MAX_NSLEN));

    if (verbose) {
        pmix_output(0, "JOB %s COMPLETED WITH STATUS %d, %u LEFT IN TREE",
                    PRTE_JOBID_PRINT(jobid), jobstatus, active);
    }

    /* Fold this job's result into what we will exit with.  The rule is
     * prterun's: the FIRST non-zero status wins, so a job that failed is not
     * overwritten by one that later succeeded - unless the user asked for
     * child jobs to be reported separately, in which case only the primary
     * job decides our status and a child's failure is reported on its own. */
    if (primary || !report_child_sep) {
        if (0 >= job_exit_code && 0 < xcode) {
            job_exit_code = xcode;
        }
        if (0 == status_from_tree && 0 != jobstatus) {
            status_from_tree = jobstatus;
        }
    } else if (0 < xcode) {
        /* 0 < , not 0 != : xcode starts at -1 meaning "the DVM reported
         * none", which is what a job that exited cleanly leaves it at */
        cs = (child_status_t *) malloc(sizeof(child_status_t));
        if (NULL != cs) {
            PMIX_LOAD_NSPACE(cs->nspace, jobid);
            cs->code = xcode;
            cs->next = child_statuses;
            child_statuses = cs;
        }
    }
    if (NULL != msg && NULL == tree_msg) {
        tree_msg = strdup(msg);
    }

    /* Wait until the tree is empty.  A job can only be spawned by a process
     * that is still running, so a tree with nothing left in it cannot
     * acquire anything more, and this is the last event we will get. */
    if (0 < active) {
        goto progress;
    }

    if (NULL == lock) {
        /* the event did not carry the object we registered - fall back to
         * the lock the main thread is actually parked on */
        lock = release_lock;
    }
    if (NULL != lock) {
        /* save the status */
        lock->status = status_from_tree;
        if (NULL != tree_msg) {
            /* first message wins, here as everywhere else - the default
             * handler may already have put one here, and overwriting it
             * would drop both the message and its allocation */
            if (NULL == lock->msg) {
                lock->msg = tree_msg;
            } else {
                free(tree_msg);
            }
            tree_msg = NULL;
        }
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

/* A launch that fails before we have an nspace is reported here and nowhere
 * else.  The spawn call is about to return an error and we will leave without
 * ever registering the job-termination handler that carries this news for a
 * job that at least started, so the DVM sends it ahead of the spawn response
 * precisely so that we are still here to receive it.
 *
 * The DVM sends the facts, not the sentence, and we compose the sentence -
 * because the message names the tool that could not launch the application,
 * and that is us.  Rendered on the HNP it would tell a prun user that "prte"
 * had failed them. */
static void launch_failed_cbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                                 const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                 pmix_info_t *results, size_t nresults,
                                 pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    const char *app = NULL, *wdir = NULL, *nodename = NULL;
    pmix_rank_t rank = PMIX_RANK_WILDCARD;
    int code = 0;
    char *msg;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            if (NULL != info[n].value.data.proc) {
                rank = info[n].value.data.proc->rank;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
            nodename = info[n].value.data.string;
        } else if (PMIX_CHECK_KEY(&info[n], "prte.launch.failed.app")) {
            app = info[n].value.data.string;
        } else if (PMIX_CHECK_KEY(&info[n], "prte.launch.failed.wdir")) {
            wdir = info[n].value.data.string;
        } else if (PMIX_CHECK_KEY(&info[n], "prte.launch.failed.code")) {
            code = info[n].value.data.int32;
        }
    }

    msg = prte_render_launch_failure(code, app, wdir, nodename, rank);
    if (NULL != msg) {
        fprintf(stderr, "%s\n", msg);
        free(msg);
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
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results, nresults);

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
    PRTE_HIDE_UNUSED_PARAMS(status);

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

/* Resolve "--stdin": the two words, or a rank.
 *
 * A value that is none of those is REFUSED rather than read as strtoul's
 * zero, which would quietly send our stdin to rank 0.  This has to be
 * asked before the spawn - once the job is running, refusing the command
 * line leaves it running with nobody to report it - so the validation is
 * done in prte_prun_parse_common_cli() and the answer is computed again
 * here, from the same string, once we have a namespace to aim it at.
 * prte's own path has an equivalent check of its own; prun and
 * "prterun --dvm" reach this one. */
static int stdin_target_rank(pmix_cli_result_t *results, pmix_rank_t *rank)
{
    pmix_cli_item_t *opt;
    unsigned long ulval;

    *rank = 0;
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_STDIN);
    if (NULL == opt) {
        return PRTE_SUCCESS;
    }
    if (0 == strcmp(opt->values[0], "all")) {
        *rank = PMIX_RANK_WILDCARD;
        return PRTE_SUCCESS;
    }
    if (0 == strcmp(opt->values[0], "none")) {
        *rank = PMIX_RANK_INVALID;
        return PRTE_SUCCESS;
    }
    if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], PMIX_RANK_VALID - 1, &ulval)) {
        return PRTE_ERR_BAD_PARAM;
    }
    *rank = (pmix_rank_t) ulval;
    return PRTE_SUCCESS;
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

int prun_common(pmix_cli_result_t *results,
                prte_schizo_base_module_t *schizo,
                int pargc, char **pargv)
{
    int rc = 1;
    char *param, *ptr;
    prte_pmix_lock_t lock, rellock;
    pmix_list_t apps, jobdata;
    prte_info_item_t *iprteinfo;
    prte_pmix_app_t *app;
    void *tinfo, *jinfo = NULL;
    pmix_info_t info, *iptr;
    pmix_proc_t pname;
    pmix_status_t ret;
    bool flag;
    size_t n, ninfo;
    pmix_app_t *papps = NULL;
    size_t napps = 0;
    mylock_t mylock;
    uint32_t ui32;
    pid_t pid;
    prte_ess_base_signal_t *sig;
    prte_event_list_item_t *evitm;
    pmix_value_t *val;
    pmix_data_array_t darray;
    char hostname[PRTE_PATH_MAX];
    pmix_rank_t rank;
    pmix_status_t code;
    pmix_proc_t parent;
    pmix_cli_item_t *opt;
    unsigned long ulval;
    PRTE_HIDE_UNUSED_PARAMS(pargc);

    /* init the globals */
    /* "-v" was accepted and never read here: "verbose" is a file-scope bool
     * hardwired to false, so every "if (verbose)" below it was dead and the
     * option did nothing at all.  prte.c had the same defect and reads its
     * own copy now; this is the one prun and "prterun --dvm" run. */
    verbose = pmix_cmd_line_is_taken(results, PRTE_CLI_VERBOSE);
    PMIX_CONSTRUCT(&apps, pmix_list_t);
    PMIX_CONSTRUCT(&forwarded_signals, pmix_list_t);
    /* this only names the tool to PMIx, so a host that will not tell us
     * its name is not fatal - but the buffer has to be readable either
     * way, and gethostname leaves it untouched when it fails */
    hostname[0] = '\0';
    if (0 != gethostname(hostname, sizeof(hostname))) {
        hostname[0] = '\0';
    }
    hostname[sizeof(hostname) - 1] = '\0';

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_DAEMONIZE)) {
        if (0 > pipe(wait_pipe)) {
            return PRTE_ERROR;
        }
        prte_state_base.parent_fd = wait_pipe[1];
        prte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    } else {
#if defined(HAVE_SETSID)
        /* see if we were directed to separate from current session */
        if (pmix_cmd_line_is_taken(results, PRTE_CLI_SET_SID)) {
            setsid();
        }
#endif
    }

    /** setup callbacks for signals we should forward */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_FWD_SIGNALS);
    if (NULL != opt) {
        param = opt->values[0];
    } else {
        param = NULL;
    }
    if (PMIX_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        return rc;
    }
    PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
    {
        signal(sig->signal, signal_forward_callback);
    }

    /* setup the job data global table */
    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    ret = pmix_pointer_array_init(prte_job_data, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                  PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        /* NOT rc: it holds the SUCCESS the signal setup just returned, so
         * returning it told our caller the tool had done its job */
        return ret;
    }

    /* setup options */
    PMIX_INFO_LIST_START(tinfo);

    /* tell PMIx what our name should be */
    if (NULL != (param = getenv("PMIX_NAMESPACE"))) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
    } else {
        pmix_asprintf(&param, "%s.%s.%lu", prte_tool_basename, hostname, (unsigned long)getpid());
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
        free(param);
    }
    rank = 0;
    if (NULL != (param = getenv("PMIX_RANK"))) {
        /* whatever set this in our environment is a PMIx launcher, so it is
         * well formed - but strtoul reads anything else as rank 0, which is
         * a real rank, so take it only when it is one */
        if (PRTE_SUCCESS == prte_parse_uint_option(param, PMIX_RANK_VALID - 1, &ulval)) {
            rank = (pmix_rank_t) ulval;
        }
    }
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);

    if (pmix_cmd_line_is_taken(results, PRTE_CLI_DO_NOT_CONNECT)) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);

    } else if (pmix_cmd_line_is_taken(results, PRTE_CLI_SYS_SERVER_FIRST)) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);

    } else if (pmix_cmd_line_is_taken(results, PRTE_CLI_SYS_SERVER_ONLY)) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_WAIT_TO_CONNECT);
    if (NULL != opt) {
        if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], UINT32_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_WAIT_TO_CONNECT, opt->values[0], "a number of seconds");
            PMIX_INFO_LIST_RELEASE(tinfo);
            return PRTE_ERR_BAD_PARAM;
        }
        ui32 = (uint32_t) ulval;
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_NUM_CONNECT_RETRIES);
    if (NULL != opt) {
        if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], UINT32_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_NUM_CONNECT_RETRIES, opt->values[0], "a number of retries");
            PMIX_INFO_LIST_RELEASE(tinfo);
            return PRTE_ERR_BAD_PARAM;
        }
        ui32 = (uint32_t) ulval;
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_PID);
    if (NULL != opt) {
        rc = prte_parse_pid_option(opt->values[0], &pid, (const char **) &param);
        switch (rc) {
        case PRTE_SUCCESS:
            PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
            break;
        case PRTE_ERR_FILE_OPEN_FAILURE:
            prte_show_help("help-prun.txt", "file-open-error", true, prte_tool_basename,
                           "--" PRTE_CLI_PID, opt->values[0], param);
            PMIX_INFO_LIST_RELEASE(tinfo);
            return PRTE_ERR_BAD_PARAM;
        case PRTE_ERR_FILE_READ_FAILURE:
            /* we could not obtain the single conversion we require */
            prte_show_help("help-prun.txt", "bad-file", true, prte_tool_basename,
                           "--" PRTE_CLI_PID, opt->values[0], param);
            PMIX_INFO_LIST_RELEASE(tinfo);
            return PRTE_ERR_BAD_PARAM;
        default: /* neither an integer nor a usable 'file:' spec */
            prte_show_help("help-prun.txt", "bad-option-input", true,
                           prte_tool_basename, "--" PRTE_CLI_PID,
                           opt->values[0], "file:path");
            PMIX_INFO_LIST_RELEASE(tinfo);
            return PRTE_ERR_BAD_PARAM;
        }
    }
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_NAMESPACE);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_NSPACE, opt->values[0], PMIX_STRING);
    }

    /* set our session directory to something hopefully unique so
     * our rendezvous files don't conflict with other prun/prte
     * instances */
    pmix_asprintf(&ptr, "%s/%s.session.%s.%lu.%lu", pmix_tmp_directory(), prte_tool_basename,
                  prte_process_info.nodename, (unsigned long) geteuid(), (unsigned long) getpid());
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TMPDIR, ptr, PMIX_STRING);
    free(ptr);

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_LAUNCHER, NULL, PMIX_BOOL);

    /* we always support tool rendezvous */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);

    /* if they specified the URI, then pass it along */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_DVM_URI);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_URI, opt->values[0], PMIX_STRING);
    }

    /* output all IOF */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_IOF_LOCAL_OUTPUT, NULL, PMIX_BOOL);

    /* convert to array of info */
    PMIX_INFO_LIST_CONVERT(ret, tinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(tinfo);

    /* now initialize PMIx */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n",
                prte_tool_basename);
        PMIX_INFO_FREE(iptr, ninfo);
        /* return rather than exit(): our caller has teardown of its own to
         * do - it is holding the pid file it was asked to write, and it
         * expects the ess framework we close here to have been closed by
         * whoever reached this function.  There is nothing to finalize:
         * PMIx never came up. */
        (void) pmix_mca_base_framework_close(&prte_ess_base_framework);
        /* 1, not a PRTE code: our return IS the tool's exit status, and
         * this is the commonest failure a script driving us will see */
        return 1;
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* register a default event handler and pass it our release lock
     * so we can cleanly exit if the server goes away */
    PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    release_lock = &rellock;
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "DEFAULT", PMIX_STRING);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    /* Only a PMIX_SUCCESS return means the callback we are about to park
     * on will be made: the entry point refuses an uninitialized or
     * shutting-down library, and a failed allocation, in front of the
     * thread-shift that would eventually call regcbfunc.  Waiting anyway
     * is a permanent hang in place of an error. */
    ret = PMIx_Register_event_handler(NULL, 0, iptr, 2, defhandler, regcbfunc, &lock);
    if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /* Register for the launch-failure event BEFORE we spawn - the whole point
     * of it is to describe a job that never got an nspace, so there is no
     * later moment at which we could ask for it.  The DVM aims it at us
     * alone, so no affected-proc filter is needed.
     *
     * Name the concrete code rather than leaning on the default handler just
     * above.  A PMIx server records a default registration only by appending
     * it to a default entry it already holds, and creates no entry when it
     * holds none - so the first tool to attach to a server is dropped from
     * its dispatch list and silently receives no default-routed event again.
     * That is fixed upstream, but PRRTE builds against any PMIx from 6.1.0
     * on, and this has to work on all of them. */
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "LAUNCH-FAILED", PMIX_STRING);
    code = PMIX_ERR_JOB_FAILED_TO_LAUNCH;
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_Register_event_handler(&code, 1, iptr, 1, launch_failed_cbfunc, regcbfunc, &lock);
    if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 1);

    /***** CONSTRUCT THE APP'S JOB-INFO ****/
    PMIX_INFO_LIST_START(jinfo);
    PMIX_LOAD_PROCID(&parent, prte_process_info.myproc.nspace, prte_process_info.myproc.rank);

    /***** CHECK FOR LAUNCH DIRECTIVES - ADD THEM TO JOB INFO IF FOUND ****/
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    ret = PMIx_Get(&pname, PMIX_LAUNCH_DIRECTIVES, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        /* Whoever launched us supplied this, so its type is not ours to
         * assume: reading a value of any other type as a data array walks
         * whatever else the value union happens to hold. */
        if (PMIX_DATA_ARRAY != val->type || NULL == val->data.darray ||
            PMIX_INFO != val->data.darray->type) {
            pmix_output(0, "%s: ignoring launch directives given as %s - "
                        "they must be an array of info",
                        prte_tool_basename, PMIx_Data_type_string(val->type));
        } else {
            iptr = (pmix_info_t *) val->data.darray->array;
            ninfo = val->data.darray->size;
            for (n = 0; n < ninfo; n++) {
                PMIX_INFO_LIST_XFER(ret, jinfo, &iptr[n]);
            }
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* we want to be notified upon job completion */
    flag = true;
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);

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
        PMIX_INFO_FREE(iptr, ninfo);
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        PRTE_UPDATE_EXIT_STATUS(ret);
        rc = ret;
        goto DONE;
    }
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_INFO_FREE(iptr, ninfo);
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n = 0; n < mylock.ninfo; n++) {
            if (PMIX_CHECK_KEY(&mylock.info[n], PMIX_SET_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_ADD_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_UNSET_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_PREPEND_ENVAR) ||
                PMIX_CHECK_KEY(&mylock.info[n], PMIX_APPEND_ENVAR)) {
                PMIX_INFO_LIST_XFER(ret, jinfo, &mylock.info[n]);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }
    /* mark that we harvested envars so prte knows not to do it again */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_ENVARS_HARVESTED, NULL, PMIX_BOOL);


    /* they want to run an application, so let's parse
     * the cmd line to get it */

    PMIX_CONSTRUCT(&jobdata, pmix_list_t);
    rc = prte_parse_locals(schizo, &apps, pargv, NULL, NULL, &jobdata, results);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_LIST_DESTRUCT(&jobdata);
        PMIX_LIST_DESTRUCT(&apps);
        goto DONE;
    }
    /* anything the parser determined to be job-level goes into the job spec */
    PMIX_LIST_FOREACH(iprteinfo, &jobdata, prte_info_item_t) {
        PMIX_INFO_LIST_XFER(ret, jinfo, &iprteinfo->info);
    }
    PMIX_LIST_DESTRUCT(&jobdata);

    /* bozo check */
    if (0 == pmix_list_get_size(&apps)) {
        pmix_output(0, "No application specified!");
        PMIX_LIST_DESTRUCT(&apps);
        rc = PRTE_ERR_BAD_PARAM;
        goto DONE;
    }

    ret = prte_prun_parse_common_cli(jinfo, results, schizo, &apps);
    if (PRTE_SUCCESS != ret) {
        /* rc still holds the SUCCESS of the parse above, and it is what we
         * return - so a command line we have just REFUSED left the tool
         * exiting 0 without having launched anything */
        PMIX_LIST_DESTRUCT(&apps);
        rc = ret;
        goto DONE;
    }

    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
    if (PMIX_SUCCESS != ret) {
        /* the list is never empty here - NOTIFY_COMPLETION alone sees to
         * that - so this is a real failure, and going on with the empty
         * array the convert leaves behind would launch the job with none
         * of the directives the user asked for */
        PMIX_ERROR_LOG(ret);
        PMIX_LIST_DESTRUCT(&apps);
        rc = ret;
        goto DONE;
    }
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(jinfo);
    jinfo = NULL;

    /* convert the apps to an array */
    napps = pmix_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    if (NULL == papps) {
        PMIX_LIST_DESTRUCT(&apps);
        PMIX_INFO_FREE(iptr, ninfo);
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto DONE;
    }
    n = 0;
    PMIX_LIST_FOREACH(app, &apps, prte_pmix_app_t)
    {
        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = PMIx_Argv_copy(app->app.argv);
        papps[n].env = PMIx_Argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        /* an app that carries no directives of its own is ordinary, and
         * the convert reports it as EMPTY after zeroing the array - but
         * anything else has lost directives that were given */
        PMIX_INFO_LIST_CONVERT(ret, app->info, &darray);
        if (PMIX_SUCCESS != ret && PMIX_ERR_EMPTY != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_LIST_DESTRUCT(&apps);
            PMIX_INFO_FREE(iptr, ninfo);
            rc = ret;
            goto DONE;
        }
        papps[n].info = (pmix_info_t *) darray.array;
        papps[n].ninfo = darray.size;
        ++n;
    }
    PMIX_LIST_DESTRUCT(&apps);

    if (verbose) {
        pmix_output(0, "Calling PMIx_Spawn");
    }

    ret = PMIx_Spawn(iptr, ninfo, papps, napps, spawnednspace);
    /* the blocking form has packed everything it needed by the time it
     * returns, and iptr is about to be reused for the next registration */
    PMIX_INFO_FREE(iptr, ninfo);
    iptr = NULL;
    ninfo = 0;
    if (PMIX_SUCCESS != ret) {
        /* SILENT is the DVM saying it has already explained itself - it is
         * how every refusal that comes with a show_help message is reported.
         * Restating it as a bare error code contradicts the very thing the
         * code means, and buries the explanation the user was given. */
        if (PMIX_ERR_SILENT != ret) {
            pmix_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        }
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
    ret = PMIx_Register_event_handler(&code, 1, iptr, 2, debug_cbfunc, regcbfunc,
                                      (void *) &lock);
    if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /* check what user wants us to do with stdin - the value was refused
     * before the spawn if it was not one we can act on */
    PMIX_LOAD_NSPACE(pname.nspace, spawnednspace);
    (void) stdin_target_rank(results, &pname.rank);
    if (PMIX_RANK_INVALID != pname.rank) {
        PMIX_INFO_CREATE(iptr, 1);
        PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIx_IOF_push(&pname, 1, NULL, iptr, 1, opcbfunc, &lock);
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            pmix_output(0, "IOF push of stdin failed: %s", PMIx_Error_string(ret));
        } else if (PMIX_SUCCESS == ret) {
            PRTE_PMIX_WAIT_THREAD(&lock);
        }
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        PMIX_INFO_FREE(iptr, 1);
    }

    /* Register to be notified when the work we started completes.
     *
     * NOT filtered to our own namespace with PMIX_EVENT_AFFECTED_PROC, which
     * is what this used to do and is the whole reason a job dynamically
     * spawned by ours was abandoned the moment ours ended: the spawned job's
     * termination names the spawned job, so the filter hid exactly the
     * events we most needed.  We take every job end the session reports and
     * sort out which are ours in the handler, where the spawn-tree root the
     * DVM stamps on each one makes that a string compare. */
    /* setup the info */
    ninfo = 2;
    PMIX_INFO_CREATE(iptr, ninfo);
    /* give the handler a name */
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "JOB_TERMINATION_EVENT", PMIX_STRING);
    /* request that they return our lock object */
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    /* do the registration */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    code = PMIX_EVENT_JOB_END;
    ret = PMIx_Register_event_handler(&code, 1, iptr, ninfo, evhandler, regcbfunc, &lock);
    if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    /* the registration copied these */
    PMIX_INFO_FREE(iptr, ninfo);
    if (PMIX_SUCCESS != ret) {
        /* nothing will tell us the job ended, so there is nothing to wait
         * for - say so rather than blocking on an event that cannot come */
        pmix_output(0, "%s: failed to register for job termination: %s",
                    prte_tool_basename, PMIx_Error_string(ret));
        rc = ret;
        goto DONE;
    }

    if (verbose) {
        pmix_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(spawnednspace));
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
    ret = PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    /* Report any child job that ended non-zero while "report child jobs
     * separately" was in effect.  Held until here rather than said as it
     * happened: the handler that collected these runs on the PMIx progress
     * thread, and show_help is not ours to call from there.  The handler is
     * deregistered by now, so nothing is still writing this list. */
    while (NULL != child_statuses) {
        child_status_t *cs = child_statuses;
        child_statuses = cs->next;
        prte_show_help("help-state-base.txt", "child-job-status", true,
                       PRTE_LOCAL_JOBID_PRINT(cs->nspace), cs->code);
        free(cs);
    }

    /* close the push of our stdin */
    PMIX_INFO_LOAD(&info, PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, &info, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        pmix_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_DESTRUCT(&info);

DONE:
    /* the job spec, on a path that never got as far as converting it */
    if (NULL != jinfo) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        jinfo = NULL;
    }
    /* an abort message we never got to hand to a waiter - the DVM went away
     * before the tree drained */
    if (NULL != tree_msg) {
        free(tree_msg);
        tree_msg = NULL;
    }
    while (NULL != child_statuses) {
        child_status_t *cstmp = child_statuses;
        child_statuses = cstmp->next;
        free(cstmp);
    }
    PMIX_LIST_FOREACH(evitm, &forwarded_signals, prte_event_list_item_t)
    {
        prte_event_signal_del(&evitm->ev);
    }
    PMIX_LIST_DESTRUCT(&forwarded_signals);
    if (NULL != papps) {
        PMIX_APP_FREE(papps, napps);
    }
    /* Close the ess framework our caller opened for us - and close it HERE,
     * while PMIx is still up.  PRRTE has no component repository of its own:
     * its components are loaded, and unloaded, by PMIx's.  PMIx_tool_finalize
     * below reaches pmix_mca_base_close(), which finalizes that repository and
     * dlcloses every component DSO it opened, ours included.  Closing a PRRTE
     * framework after that walks its component list into memory that is no
     * longer mapped, and the tool segfaults in teardown having already done
     * its work correctly - so the job succeeds and prun exits 139.
     *
     * None of that is visible in the default build, where the components are
     * linked into libprrte and their structs are mapped for the life of the
     * process.  It is why the callers no longer close this themselves.
     */
    (void) pmix_mca_base_framework_close(&prte_ess_base_framework);
    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PMIX_SUCCESS != ret) {
        // Since the user job has probably exited by
        // now, let's preserve its return code and print
        // a warning here, if prte logging is on.
        pmix_output(0, "PMIx_tool_finalize() failed. Status = %d", ret);
    }

    /* Only NOW is the release lock finished with.  It is on our stack, and
     * the default event handler holds a pointer to it that is never
     * deregistered - so destroying it while PMIx could still deliver an
     * event left that handler locking a destroyed mutex.  The window is
     * narrow (a lost connection between the job's end and our teardown),
     * which is exactly the interval in which losing the DVM is likeliest.
     * The finalize above is the point after which no handler can run.
     * Destructing here also frees any message the lock was still carrying,
     * which the lost-connection exit above used to walk away from. */
    release_lock = NULL;
    PRTE_PMIX_DESTRUCT_LOCK(&rellock);

    /* Our caller makes this our exit status.  If the DVM told us what the
     * application exited with, that is the answer - a launcher reports the
     * status of what it launched, which is what prterun does and what any
     * script driving prun expects.  Otherwise all we have is why the job
     * ended, which is a status code, not an exit status: keep returning it
     * (a caller only ever gets its low byte, but non-zero is non-zero) so
     * that failures with no application status behind them still fail. */
    if (0 < job_exit_code) {
        return job_exit_code;
    }
    return rc;
}

/* the mutually-exclusive ways of naming the allocation a job is to be
 * mapped onto - see the resolution order in prte_plm_base_recv() */
static const char *alloc_target_opts[] = {
    PRTE_CLI_SESSION_ID,
    PRTE_CLI_TARGET_ALLOC,
    PRTE_CLI_ALLOC_REFID,
    NULL
};

int prte_prun_parse_common_cli(void *jinfo, pmix_cli_result_t *results,
                               prte_schizo_base_module_t *schizo,
                               pmix_list_t *apps)
{
    pmix_cli_item_t *opt, opt2;
    int ret, i, ntargets;
    uint32_t ui32;
    unsigned long ulval;
    bool flag;
    prte_pmix_app_t *app;
    char *param;

    /* pass the personality */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PERSONALITY, schizo->name, PMIX_STRING);

    /* Display directives are job-level only: there is no way to scope a map,
     * binding, or allocation display to an individual app context.  A second
     * instance therefore almost always means the user attached one to an app
     * in an MPMD line, which we cannot honor - reject it rather than silently
     * applying just one. */
    if (1 < pmix_cmd_line_get_ninsts(results, PRTE_CLI_DISPLAY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_DISPLAY);
        return PRTE_ERR_BAD_PARAM;
    }

    /* get display options */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        ret = prte_schizo_base_parse_display(opt, jinfo);
    } else if (NULL != prte_schizo_base.default_display_options) {
        PMIX_CONSTRUCT(&opt2, pmix_cli_item_t);
        opt2.key = strdup(PRTE_CLI_DISPLAY);
        PMIx_Argv_append_nosize(&opt2.values, prte_schizo_base.default_display_options);
        ret = prte_schizo_base_parse_display(&opt2, jinfo);
        PMIX_DESTRUCT(&opt2);
    }
    if (PRTE_SUCCESS != ret) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        return ret;
    }

    /* check for output options */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        ret = prte_schizo_base_parse_output(opt, jinfo);
    } else if (NULL != prte_schizo_base.default_output_options) {
        PMIX_CONSTRUCT(&opt2, pmix_cli_item_t);
        opt2.key = strdup(PRTE_CLI_OUTPUT);
        PMIx_Argv_append_nosize(&opt2.values, prte_schizo_base.default_output_options);
        ret = prte_schizo_base_parse_output(&opt2, jinfo);
        PMIX_DESTRUCT(&opt2);
    }
    if (PRTE_SUCCESS != ret) {
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        return ret;
    }

    /* check for runtime options */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_RTOS);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RUNTIME_OPTIONS, opt->values[0], PMIX_STRING);
        /* one of these is ours to act on as well: it decides whether a child
         * job's exit status becomes OUR exit status, and we are the process
         * that has one.  The DVM reads the same directive for its own answer
         * when it is the launcher; here nobody else can.  The MCA param has
         * the same standing it has there - set, it wins outright. */
        report_child_sep = prte_report_child_jobs_separately ||
                           prte_state_base_report_child_sep(opt->values[0]);
    } else if (NULL != prte_schizo_base.default_runtime_options) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RUNTIME_OPTIONS,
                           prte_schizo_base.default_runtime_options, PMIX_STRING);
        report_child_sep = prte_report_child_jobs_separately ||
                           prte_state_base_report_child_sep(prte_schizo_base.default_runtime_options);
    } else {
        report_child_sep = prte_report_child_jobs_separately;
    }

    /* check what user wants us to do with stdin */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        pmix_rank_t stdintgt;

        if (PRTE_SUCCESS != stdin_target_rank(results, &stdintgt)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_STDIN, opt->values[0], "all, none, or a rank");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_STDIN_TGT, opt->values[0], PMIX_STRING);
    }

    /* The mapping, ranking and binding directives are NOT taken from the
     * global parse: it stops at the first app, so it cannot see a directive
     * the user attached to a later one, and it cannot tell how many were
     * given. prte_parse_locals() sees every segment and decides - a lone
     * directive is the job's, several are the apps' - handing the job's back
     * on the jobdata list, which is transferred into jinfo above. */

    /* check for an exec agent */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_EXEC_AGENT);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_EXEC_AGENT, opt->values[0], PMIX_STRING);
    }

    /* mark if recovery was enabled on the cmd line */
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_ENABLE_RECOVERY)) {
        flag = true;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
    }
    /* record the max restarts */
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_MAX_RESTARTS);
    if (NULL != opt) {
        if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], UINT32_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_MAX_RESTARTS, opt->values[0], "a number of restarts");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        ui32 = (uint32_t) ulval;
        PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t)
        {
            PMIX_INFO_LIST_ADD(ret, app->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
        }
    }
    /* if continuous operation was specified */
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_CONTINUOUS)) {
        /* mark this job as continuously operating */
        flag = true;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
    }

    /* if stop-on-exec was specified */
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_STOP_ON_EXEC)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    i = 0;
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_TIMEOUT);
    if (NULL != opt) {
        if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], INT_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_TIMEOUT, opt->values[0], "a number of seconds");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        i = (int) ulval;
    } else if (NULL != (param = getenv("MPIEXEC_TIMEOUT"))) {
        if (PRTE_SUCCESS != prte_parse_uint_option(param, INT_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "MPIEXEC_TIMEOUT", param, "a number of seconds");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        i = (int) ulval;
    }
    if (0 != i) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_TIMEOUT, &i, PMIX_INT);
    }

    /* these two are the bare presence of an option: the value they carry is
     * the assertion the user made by writing it.  They used to be handed
     * "flag" as it stood, which is set only by the recovery and continuous
     * blocks above - so on any command line that gave neither, the DVM was
     * told to take stack traces "false" and --get-stack-traces did nothing */
    flag = true;
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_STACK_TRACES)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_STACKTRACES, &flag, PMIX_BOOL);
    }
    if (pmix_cmd_line_is_taken(results, PRTE_CLI_REPORT_STATE)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_REPORT_STATE, &flag, PMIX_BOOL);
    }
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_SPAWN_TIMEOUT);
    if (NULL != opt) {
        if (PRTE_SUCCESS != prte_parse_uint_option(opt->values[0], INT_MAX, &ulval)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_SPAWN_TIMEOUT, opt->values[0], "a number of seconds");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        i = (int) ulval;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_SPAWN_TIMEOUT, &i, PMIX_INT);
    }
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_DO_NOT_AGG_HELP);
    if (NULL != opt) {
        flag = false;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_LOG_AGG, &flag, PMIX_BOOL);
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_MEM_ALLOC_KIND);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MEM_ALLOC_KIND, opt->values[0], PMIX_STRING);
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_GPU_SUPPORT);
    if (NULL != opt) {
        /* they could be enabling or disabling it - but the truth test
         * underneath reports anything it does not recognize as FALSE, so
         * "--gpu-support maybe" quietly meant "no GPU support".  Refuse a
         * value that is neither, the way every other boolean option here
         * does */
        if (PRTE_SUCCESS != prte_cli_bool_value(opt->values[0], &flag)) {
            prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                           "--" PRTE_CLI_GPU_SUPPORT, opt->values[0], "true or false");
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_GPU_SUPPORT, &flag, PMIX_BOOL);
    }

    /* Determine the allocation upon which this job is to be mapped. The
     * three options are alternative spellings of the same thing - the
     * numeric ID of the session that holds the allocation, the identifier
     * the host environment assigned to the allocation, or the reference ID
     * the user attached to it when the allocation was requested. The DVM
     * master resolves them in a fixed order, so naming more than one is
     * ambiguous: refuse it here rather than silently honoring just one.
     * Each is a job-level directive - there is no way to map different
     * app contexts of a single job onto different allocations - so a
     * second instance means the user attached one to an app in an MPMD
     * line, which we likewise cannot honor. */
    ntargets = 0;
    for (i = 0; NULL != alloc_target_opts[i]; i++) {
        if (1 < pmix_cmd_line_get_ninsts(results, alloc_target_opts[i])) {
            prte_show_help("help-schizo-base.txt", "multi-instances", true,
                           alloc_target_opts[i]);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        if (pmix_cmd_line_is_taken(results, alloc_target_opts[i])) {
            ++ntargets;
        }
    }
    if (1 < ntargets) {
        prte_show_help("help-schizo-base.txt", "alloc-target-conflict", true,
                       PRTE_CLI_SESSION_ID, PRTE_CLI_TARGET_ALLOC, PRTE_CLI_ALLOC_REFID);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
        return PRTE_ERR_BAD_PARAM;
    }

    opt = pmix_cmd_line_get_param(results, PRTE_CLI_SESSION_ID);
    if (NULL != opt) {
        char *endptr = NULL;
        unsigned long sid;

        /* require a bare run of digits: strtoul would otherwise accept a
         * leading sign or whitespace, and silently wrap "-1" into a valid
         * (and quite possibly meaningful) session ID */
        errno = 0;
        sid = strtoul(opt->values[0], &endptr, 10);
        if (!isdigit((unsigned char) opt->values[0][0]) || '\0' != *endptr ||
            0 != errno || sid != (unsigned long) (uint32_t) sid) {
            prte_show_help("help-schizo-base.txt", "bad-session-id", true,
                           PRTE_CLI_SESSION_ID, opt->values[0]);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
            return PRTE_ERR_BAD_PARAM;
        }
        ui32 = (uint32_t) sid;
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_SESSION_ID, &ui32, PMIX_UINT32);
    }
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_TARGET_ALLOC);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_ALLOC_ID, opt->values[0], PMIX_STRING);
    }
    opt = pmix_cmd_line_get_param(results, PRTE_CLI_ALLOC_REFID);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_ALLOC_REQ_ID, opt->values[0], PMIX_STRING);
    }

    /* give the schizo components a chance to add to the job info */
    schizo->job_info(results, jinfo);

    return PRTE_SUCCESS;
}

static void signal_forward_callback(int signum)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;

    /* We are installed before the tool has connected to anything, let alone
     * spawned, and every forwardable signal is forwarded by default - so a
     * SIGTSTP or a "kill -USR1" that lands while we are still connecting or
     * still inside PMIx_Spawn finds spawnednspace empty.  An empty namespace
     * is not a safe way to say "no job": the job-control path packs it
     * verbatim and prted_comm.c reads it as EVERY job, so on a shared
     * persistent DVM we would deliver the signal to other people's work. */
    if ('\0' == spawnednspace[0]) {
        if (verbose) {
            fprintf(stderr, "%s: signal %d received before the job was spawned - not forwarded\n",
                    prte_tool_basename, signum);
        }
        return;
    }

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
