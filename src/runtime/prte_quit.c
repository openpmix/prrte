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
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
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

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/plm/plm.h"
#include "src/mca/state/state.h"

#include "src/threads/pmix_threads.h"
#include "src/util/pmix_output.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/runtime.h"

/*
 * Globals
 */
static int num_aborted = 0;
static int num_killed = 0;
static int num_failed_start = 0;

void prte_quit(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    /* cleanup */
    if (NULL != caddy) {
        PMIX_RELEASE(caddy);
    }

    /* check one-time lock to protect against "bounce" */
    if (pmix_mutex_trylock(&prte_quit_lock)) { /* returns 1 if already locked */
        return;
    }

    /* flag that the event lib should no longer be looped
     * so we will exit
     */
    prte_event_base_active = false;
    PMIX_POST_OBJECT(prte_event_base_active);
    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    prte_event_base_loopexit(prte_event_base);
}

/* Render the diagnostic for a process that never got off the ground.
 *
 * Deliberately takes values rather than the job/proc/app objects: the
 * requesting tool has none of those, and the message has to be able to come
 * out in ITS voice.  Every topic here leads with prte_tool_basename, which
 * is per-process - "prterun" in a prterun, "prun" in a prun - so whoever
 * calls this signs the message with their own name.  Rendering it on the
 * HNP and shipping the prose is exactly the mistake this avoids: a prun
 * user would be told that "prte" was unable to launch their application.
 */
char *prte_render_launch_failure(int exit_code, const char *app, const char *cwd,
                                 const char *nodename, pmix_rank_t rank)
{
    char *output = NULL;

    if (NULL == nodename) {
        nodename = "UNKNOWN";
    }
    switch (exit_code) {
    case PMIX_ERR_SILENT:
    case PRTE_ERR_SILENT:
        /* say nothing - it was already reported */
        break;
    case PMIX_ERR_SYS_LIMITS_PIPES:
        output = pmix_show_help_string("help-prun.txt", "prun:sys-limit-pipe", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PMIX_ERR_PIPE_SETUP_FAILURE:
        output = pmix_show_help_string("help-prun.txt", "prun:pipe-setup-failure", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PMIX_ERR_SYS_LIMITS_CHILDREN:
        output = pmix_show_help_string("help-prun.txt", "prun:sys-limit-children", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PMIX_ERR_SYS_LIMITS_FILES:
        output = pmix_show_help_string("help-prun.txt", "prun:sys-limit-files", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PRTE_ERR_FAILED_GET_TERM_ATTRS:
        output = pmix_show_help_string("help-prun.txt", "prun:failed-term-attrs", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PMIX_ERR_JOB_WDIR_NOT_FOUND:
        output = pmix_show_help_string("help-prun.txt", "prun:wdir-not-found", true,
                                       prte_tool_basename, cwd, nodename,
                                       (unsigned long) rank);
        break;
    case PMIX_ERR_JOB_WDIR_NOT_ACCESSIBLE:
         output = pmix_show_help_string("help-prun.txt", "prun:wdir-not-accessible", true,
                                        prte_tool_basename, cwd, nodename,
                                        (unsigned long) rank);
         break;
    case PMIX_ERR_JOB_EXE_NOT_FOUND:
        output = pmix_show_help_string("help-prun.txt", "prun:exe-not-found", true,
                                       prte_tool_basename, (unsigned long) rank,
                                       prte_tool_basename, prte_tool_basename, nodename,
                                       app);
        break;
    case PMIX_ERR_EXE_NOT_ACCESSIBLE:
        output = pmix_show_help_string("help-prun.txt", "prun:exe-not-accessible", true,
                                       prte_tool_basename, app, nodename,
                                       (unsigned long) rank);
        break;
    case PRTE_ERR_PRELOAD_CONFLICT:
        output = pmix_show_help_string("help-prun.txt", "prun:preload-collision", true,
                                       prte_tool_basename, cwd, nodename,
                                       (unsigned long) rank);
        break;
    case PRTE_ERR_MULTIPLE_AFFINITIES:
        output = pmix_show_help_string("help-prun.txt", "prun:multiple-paffinity-schemes", true,
                                       NULL);
        break;
    case PRTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED:
        output = pmix_show_help_string("help-prun.txt", "prun:topo-not-supported", true,
                                       nodename,
                                       "rankfile containing a slot_list of ", NULL,
                                       app);
        break;
    case PRTE_ERR_INVALID_NODE_RANK:
        output = pmix_show_help_string("help-prun.txt", "prun:invalid-node-rank", true);
        break;
    case PRTE_ERR_INVALID_LOCAL_RANK:
        output = pmix_show_help_string("help-prun.txt", "prun:invalid-local-rank", true);
        break;
    case PRTE_ERR_NOT_ENOUGH_CORES:
        output = pmix_show_help_string("help-prun.txt", "prun:not-enough-resources", true,
                                       "sockets", nodename, "bind-to-core", app);
        break;
    case PRTE_ERR_TOPO_CORE_NOT_SUPPORTED:
        output = pmix_show_help_string("help-prun.txt", "prun:topo-not-supported", true,
                                       nodename, "bind-to-core", "", app);
        break;
    case PRTE_ERR_INVALID_PHYS_CPU:
        output = pmix_show_help_string("help-prun.txt", "prun:invalid-phys-cpu", true);
        break;
    case PRTE_ERR_NOT_ENOUGH_SOCKETS:
        output = pmix_show_help_string("help-prun.txt", "prun:not-enough-resources", true,
                                       "sockets", nodename, "bind-to-socket", app);
        break;
    case PRTE_ERR_TOPO_SOCKET_NOT_SUPPORTED:
        output = pmix_show_help_string("help-prun.txt", "prun:topo-not-supported", true,
                                       nodename, "bind-to-socket", "", app);
        break;
    case PRTE_ERR_MODULE_NOT_FOUND:
        output = pmix_show_help_string("help-prun.txt", "prun:paffinity-missing-module", true,
                                       nodename);
        break;
    case PRTE_ERR_SLOT_LIST_RANGE:
        output = pmix_show_help_string("help-prun.txt", "prun:invalid-slot-list-range", true,
                                       nodename, NULL);
        break;
    case PRTE_ERR_PIPE_READ_FAILURE:
        output = pmix_show_help_string("help-prun.txt", "prun:pipe-read-failure", true,
                                       prte_tool_basename, nodename,
                                       (unsigned long) rank);
        break;
    case PRTE_ERR_SOCKET_NOT_AVAILABLE:
        output = pmix_show_help_string("help-prun.txt", "prun:proc-socket-not-avail", true,
                                       prte_tool_basename, PRTE_ERROR_NAME(exit_code),
                                       nodename, (unsigned long) rank);
        break;

    default:
        if (0 != exit_code) {
            output = pmix_show_help_string("help-prun.txt", "prun:proc-failed-to-start", true,
                                           prte_tool_basename, exit_code,
                                           PRTE_ERROR_NAME(exit_code), nodename,
                                           (unsigned long) rank);
        } else {
            output = pmix_show_help_string("help-prun.txt",
                                           "prun:proc-failed-to-start-no-status", true,
                                           prte_tool_basename, nodename);
        }
    }
    return output;
}

static char *print_aborted_job(prte_job_t *job,
                               prte_app_context_t *approc,
                               prte_proc_t *proc,
                               prte_node_t *node)
{
    char *output = NULL;
    char *nodename;

    if (NULL == node) {
        nodename = "UNKNOWN";
    } else {
        nodename = node->name;
    }

    if (PRTE_PROC_STATE_FAILED_TO_START == proc->state ||
        PRTE_PROC_STATE_FAILED_TO_LAUNCH == proc->state) {
        return prte_render_launch_failure(proc->exit_code,
                                          (NULL == approc) ? NULL : approc->app,
                                          (NULL == approc) ? NULL : approc->cwd,
                                          nodename, proc->name.rank);
    } else if (PRTE_PROC_STATE_ABORTED == proc->state ||
               PRTE_PROC_STATE_CALLED_ABORT == proc->state) {
        output = pmix_show_help_string("help-prun.txt", "prun:proc-ordered-abort", true,
                                       prte_tool_basename, (unsigned long) proc->name.rank,
                                       (unsigned long) proc->pid, nodename, prte_tool_basename);
        return output;
        /* aborted by signal - test the PROC's state, like every other branch
         * here.  This read the JOB's state, which is a different family of
         * codes that merely happens to share a numeric base, so it only ever
         * matched when the job state had not moved on yet.  By the time
         * dvm_notify() renders the message the job is in NOTIFIED, so a
         * process killed by a signal was reported to prterun (which renders
         * from the earlier teardown path) and silently to prun, which is the
         * only thing a persistent DVM has. */
    } else if (PRTE_PROC_STATE_ABORTED_BY_SIG == proc->state) {
#ifdef HAVE_STRSIGNAL
        if (NULL != strsignal(WTERMSIG(proc->exit_code))) {
            output = pmix_show_help_string("help-prun.txt", "prun:proc-aborted-strsignal", true,
                                           prte_tool_basename, (unsigned long) proc->name.rank,
                                           (unsigned long) proc->pid, nodename,
                                           WTERMSIG(proc->exit_code),
                                           strsignal(WTERMSIG(proc->exit_code)));
        } else {
#endif
            output = pmix_show_help_string("help-prun.txt", "prun:proc-aborted", true,
                                           prte_tool_basename, (unsigned long) proc->name.rank,
                                           (unsigned long) proc->pid, nodename,
                                           WTERMSIG(proc->exit_code));
#ifdef HAVE_STRSIGNAL
        }
#endif
        return output;
    } else if (PRTE_PROC_STATE_TERM_WO_SYNC == proc->state) { /* proc exited w/o finalize */
        output = pmix_show_help_string("help-prun.txt", "prun:proc-exit-no-sync", true,
                                       prte_tool_basename, (unsigned long) proc->name.rank,
                                       (unsigned long) proc->pid, nodename, prte_tool_basename,
                                       prte_tool_basename);
        return output;
    } else if (PRTE_PROC_STATE_KILLED_BY_RELEASE == proc->state) {
        output = pmix_show_help_string("help-prun.txt", "prun:proc-killed-by-release", true,
                                       prte_tool_basename, (unsigned long) proc->name.rank,
                                       nodename, prte_tool_basename);
        return output;
    } else if (PRTE_PROC_STATE_COMM_FAILED == proc->state) {
        output = pmix_show_help_string("help-prun.txt", "prun:proc-comm-failed", true,
                                       PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                       PRTE_NAME_PRINT(&proc->name), nodename);
        return output;
    } else if (PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == proc->state) {
        switch (proc->exit_code) {
        case PRTE_ERR_MEM_LIMIT_EXCEEDED:
            output = pmix_show_help_string("help-prun.txt", "prun:proc-mem-exceeded", true,
                                           PRTE_NAME_PRINT(&proc->name), nodename);
            break;
        case PRTE_ERR_PROC_STALLED:
            output = pmix_show_help_string("help-prun.txt", "prun:proc-stalled", true);
            break;

        default:
            output = pmix_show_help_string("help-prun.txt", "prun:proc-sensor-exceeded", true);
        }
        return output;
    } else if (PRTE_PROC_STATE_HEARTBEAT_FAILED == proc->state) {
        output = pmix_show_help_string("help-prun.txt", "prun:proc-heartbeat-failed", true,
                                       prte_tool_basename, PRTE_NAME_PRINT(&proc->name),
                                       nodename);
        return output;
    } else if (PRTE_PROC_STATE_TERM_NON_ZERO == proc->state) {
        if (prte_get_attribute(&job->attributes, PRTE_JOB_ERROR_NONZERO_EXIT, NULL, PMIX_BOOL)) {
            output = pmix_show_help_string("help-prun.txt", "prun:non-zero-exit", true,
                                           prte_tool_basename, PRTE_NAME_PRINT(&proc->name),
                                           proc->exit_code);
            return output;
        }
    }

    /* nothing here */
    return NULL;
}

/*
 * On abnormal termination - dump the
 * exit status of the aborted procs.
 */

static char *dump_job(prte_job_t *job)

{
    int32_t i;
    prte_proc_t *proc, *pptr;
    prte_app_context_t *approc;
    prte_node_t *node;

    /* cycle through and count the number that were killed or aborted */
    for (i = 0; i < job->procs->size; i++) {
        if (NULL == (pptr = (prte_proc_t *) pmix_pointer_array_get_item(job->procs, i))) {
            /* array is left-justified - we are done */
            break;
        }
        if (PRTE_PROC_STATE_FAILED_TO_START == pptr->state ||
            PRTE_PROC_STATE_FAILED_TO_LAUNCH == pptr->state) {
            ++num_failed_start;
        } else if (PRTE_PROC_STATE_ABORTED == pptr->state) {
            ++num_aborted;
        } else if (PRTE_PROC_STATE_ABORTED_BY_SIG == pptr->state) {
            ++num_killed;
        } else if (PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == pptr->state) {
            ++num_killed;
        } else if (PRTE_PROC_STATE_KILLED_BY_RELEASE == pptr->state) {
            ++num_killed;
        }
    }
    /* see if there is a guilty party */
    proc = NULL;
    if (!prte_get_attribute(&job->attributes, PRTE_JOB_ABORTED_PROC, (void **) &proc, PMIX_POINTER)
        || NULL == proc) {
        return NULL;
    }

    approc = (prte_app_context_t *) pmix_pointer_array_get_item(job->apps, proc->app_idx);
    node = proc->node;
    return print_aborted_job(job, approc, proc, node);
}

char *prte_dump_aborted_procs(prte_job_t *jdata)
{
    prte_job_t *job, *launcher;
    char *output = NULL;

    /* if we already reported it, then don't do it again */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_ERR_REPORTED)) {
        return NULL;
    }
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_ERR_REPORTED);

    /* The launcher is only wanted so we can search its children for the job
     * that failed; if it is gone, fall back to the job we were handed. Its
     * absence is routine - a spawned job outlives its parent by default, and
     * the parent's job object is released as soon as the parent completes. */
    if (PMIX_NSPACE_INVALID(jdata->launcher)) {
        launcher = jdata;
    } else {
        launcher = prte_get_job_data_object(jdata->launcher);
        if (NULL == launcher) {
            PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s prte_dump_aborted_procs: launcher %s of job %s is gone - "
                                 "reporting on that job directly",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(jdata->launcher),
                                 PRTE_JOBID_PRINT(jdata->nspace)));
            launcher = jdata;
        }
    }

    /* cycle thru all the children of this launcher to find the
     * one that caused the error */
    /* if this is a non-persistent job, it won't have any child
     * jobs, so look at it directly */
    if (0 == pmix_list_get_size(&launcher->children)) {
        output = dump_job(jdata);
    } else {
        PMIX_LIST_FOREACH(job, &launcher->children, prte_job_t)
        {
            output = dump_job(job);
            if (NULL != output) {
                break;
            }
        }
    }

    return output;
}
