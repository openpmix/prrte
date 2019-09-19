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
 * Copyright (c) 2006-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <stdio.h>
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

#include "src/mca/plm/plm.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/routed/routed.h"
#include "src/mca/state/state.h"

#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_data_server.h"

/*
 * Globals
 */
static int num_aborted = 0;
static int num_killed = 0;
static int num_failed_start = 0;

void prrte_quit(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(caddy);

    /* cleanup */
    if (NULL != caddy) {
        PRRTE_RELEASE(caddy);
    }

    /* check one-time lock to protect against "bounce" */
    if (prrte_atomic_trylock(&prrte_quit_lock)) { /* returns 1 if already locked */
        return;
    }

    /* flag that the event lib should no longer be looped
     * so we will exit
     */
    prrte_event_base_active = false;
    PRRTE_POST_OBJECT(prrte_event_base_active);
    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    prrte_event_base_loopbreak(prrte_event_base);
}

static char* print_aborted_job(prrte_job_t *job,
                               prrte_app_context_t *approc,
                               prrte_proc_t *proc,
                               prrte_node_t *node)
{
    char *output = NULL;

    if (PRRTE_PROC_STATE_FAILED_TO_START == proc->state ||
        PRRTE_PROC_STATE_FAILED_TO_LAUNCH == proc->state) {
        switch (proc->exit_code) {
        case PRRTE_ERR_SILENT:
            /* say nothing - it was already reported */
            break;
        case PRRTE_ERR_SYS_LIMITS_PIPES:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:sys-limit-pipe", true,
                                           prrte_tool_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_PIPE_SETUP_FAILURE:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:pipe-setup-failure", true,
                                           prrte_tool_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_SYS_LIMITS_CHILDREN:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:sys-limit-children", true,
                                           prrte_tool_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_FAILED_GET_TERM_ATTRS:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:failed-term-attrs", true,
                                           prrte_tool_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_WDIR_NOT_FOUND:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:wdir-not-found", true,
                                           prrte_tool_basename, approc->cwd,
                                           node->name, (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_EXE_NOT_FOUND:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:exe-not-found", true,
                                           prrte_tool_basename,
                                           (unsigned long)proc->name.vpid,
                                           prrte_tool_basename,
                                           prrte_tool_basename,
                                           node->name,
                                           approc->app);
            break;
        case PRRTE_ERR_EXE_NOT_ACCESSIBLE:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:exe-not-accessible", true,
                                           prrte_tool_basename, approc->app, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_MULTIPLE_AFFINITIES:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:multiple-paffinity-schemes", true, NULL);
            break;
        case PRRTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:topo-not-supported",
                                           true, prrte_process_info.nodename, "rankfile containing a slot_list of ",
                                           NULL, approc->app);
            break;
        case PRRTE_ERR_INVALID_NODE_RANK:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:invalid-node-rank", true);
            break;
        case PRRTE_ERR_INVALID_LOCAL_RANK:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:invalid-local-rank", true);
            break;
        case PRRTE_ERR_NOT_ENOUGH_CORES:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:not-enough-resources", true,
                                           "sockets", node->name,
                                           "bind-to-core", approc->app);
            break;
        case PRRTE_ERR_TOPO_CORE_NOT_SUPPORTED:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:topo-not-supported",
                                           true, node->name, "bind-to-core", "",
                                           approc->app);
            break;
        case PRRTE_ERR_INVALID_PHYS_CPU:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:invalid-phys-cpu", true);
            break;
        case PRRTE_ERR_NOT_ENOUGH_SOCKETS:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:not-enough-resources", true,
                                           "sockets", node->name,
                                           "bind-to-socket", approc->app);
            break;
        case PRRTE_ERR_TOPO_SOCKET_NOT_SUPPORTED:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:topo-not-supported",
                                           true, node->name, "bind-to-socket", "",
                                           approc->app);
            break;
        case PRRTE_ERR_MODULE_NOT_FOUND:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:paffinity-missing-module",
                                           true, node->name);
            break;
        case PRRTE_ERR_SLOT_LIST_RANGE:
            output = prrte_show_help_string("help-prrterun.txt",
                                           "prrterun:invalid-slot-list-range",
                                           true, node->name, NULL);
            break;
        case PRRTE_ERR_PIPE_READ_FAILURE:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:pipe-read-failure", true,
                                           prrte_tool_basename, node->name, (unsigned long)proc->name.vpid);
            break;
        case PRRTE_ERR_SOCKET_NOT_AVAILABLE:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-socket-not-avail", true,
                                           prrte_tool_basename, PRRTE_ERROR_NAME(proc->exit_code), node->name,
                                           (unsigned long)proc->name.vpid);
            break;

        default:
            if (0 != proc->exit_code) {
                output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-failed-to-start", true,
                                               prrte_tool_basename, proc->exit_code, PRRTE_ERROR_NAME(proc->exit_code),
                                               node->name, (unsigned long)proc->name.vpid);
            } else {
                output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-failed-to-start-no-status", true,
                                               prrte_tool_basename, node->name);
            }
        }
        return output;
    } else if (PRRTE_PROC_STATE_ABORTED == proc->state) {
        output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-ordered-abort", true,
                                       prrte_tool_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                       node->name, prrte_tool_basename);
        return output;
    } else if (PRRTE_PROC_STATE_ABORTED_BY_SIG == job->state) {  /* aborted by signal */
#ifdef HAVE_STRSIGNAL
        if (NULL != strsignal(WTERMSIG(proc->exit_code))) {
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-aborted-strsignal", true,
                                           prrte_tool_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                           node->name, WTERMSIG(proc->exit_code),
                                           strsignal(WTERMSIG(proc->exit_code)));
        } else {
#endif
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-aborted", true,
                                           prrte_tool_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                           node->name, WTERMSIG(proc->exit_code));
#ifdef HAVE_STRSIGNAL
        }
#endif
        return output;
    } else if (PRRTE_PROC_STATE_TERM_WO_SYNC == proc->state) { /* proc exited w/o finalize */
        output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-exit-no-sync", true,
                                       prrte_tool_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                       node->name, prrte_tool_basename, prrte_tool_basename);
        return output;
    } else if (PRRTE_PROC_STATE_COMM_FAILED == proc->state) {
        output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-comm-failed", true,
                                       PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                       PRRTE_NAME_PRINT(&proc->name), node->name);
        return output;
    } else if (PRRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == proc->state) {
        switch (proc->exit_code) {
        case PRRTE_ERR_MEM_LIMIT_EXCEEDED:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-mem-exceeded", true,
                                           PRRTE_NAME_PRINT(&proc->name), node->name);
            break;
        case PRRTE_ERR_PROC_STALLED:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-stalled", true);
            break;

        default:
            output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-sensor-exceeded", true);
        }
        return output;
    } else if (PRRTE_PROC_STATE_HEARTBEAT_FAILED == proc->state) {
        output = prrte_show_help_string("help-prrterun.txt", "prrterun:proc-heartbeat-failed", true,
                                       prrte_tool_basename, PRRTE_NAME_PRINT(&proc->name), node->name);
        return output;
    } else if (prrte_abort_non_zero_exit &&
               PRRTE_PROC_STATE_TERM_NON_ZERO == proc->state) {
        output = prrte_show_help_string("help-prrterun.txt", "prrterun:non-zero-exit", true,
                                       prrte_tool_basename, PRRTE_NAME_PRINT(&proc->name), proc->exit_code);
        return output;
    }

    /* nothing here */
    return NULL;
}

/*
 * On abnormal termination - dump the
 * exit status of the aborted procs.
 */

char* prrte_dump_aborted_procs(prrte_job_t *jdata)
{
    prrte_job_t *job, *launcher;
    prrte_std_cntr_t i;
    prrte_proc_t *proc, *pptr;
    prrte_app_context_t *approc;
    prrte_node_t *node;
    char *output = NULL;

    /* if this job is not a launcher itself, then get the launcher for this job */
    if (PRRTE_JOBID_INVALID == jdata->launcher) {
        launcher = jdata;
    } else {
        launcher = prrte_get_job_data_object(jdata->launcher);
        if (NULL == launcher) {
            output = strdup("LAUNCHER JOB OBJECT NOT FOUND");
            return output;
        }
    }

    /* cycle thru all the children of this launcher to find the
     * one that caused the error */
    PRRTE_LIST_FOREACH(job, &launcher->children, prrte_job_t) {
        /* cycle through and count the number that were killed or aborted */
        for (i=0; i < job->procs->size; i++) {
            if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(job->procs, i))) {
                /* array is left-justfied - we are done */
                break;
            }
            if (PRRTE_PROC_STATE_FAILED_TO_START == pptr->state ||
                PRRTE_PROC_STATE_FAILED_TO_LAUNCH == pptr->state) {
                ++num_failed_start;
            } else if (PRRTE_PROC_STATE_ABORTED == pptr->state) {
                ++num_aborted;
            } else if (PRRTE_PROC_STATE_ABORTED_BY_SIG == pptr->state) {
                ++num_killed;
            } else if (PRRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == pptr->state) {
                ++num_killed;
            }
        }
        /* see if there is a guilty party */
        proc = NULL;
        if (!prrte_get_attribute(&job->attributes, PRRTE_JOB_ABORTED_PROC, (void**)&proc, PRRTE_PTR) ||
            NULL == proc) {
            continue;
        }

        approc = (prrte_app_context_t*)prrte_pointer_array_get_item(job->apps, proc->app_idx);
        node = proc->node;
        if (NULL != (output = print_aborted_job(job, approc, proc, node))) {
            break;
        }
    }

    return output;
}
