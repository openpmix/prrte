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
 * Copyright (c) 2014-2018 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

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

#include "orte/mca/plm/plm.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/state/state.h"

#include "orte/util/session_dir.h"
#include "orte/util/show_help.h"
#include "orte/util/threads.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_quit.h"
#include "orte/runtime/orte_locks.h"
#include "orte/runtime/orte_data_server.h"

/*
 * Globals
 */
static int num_aborted = 0;
static int num_killed = 0;
static int num_failed_start = 0;

void orte_quit(int fd, short args, void *cbdata)
{
    orte_state_caddy_t *caddy = (orte_state_caddy_t*)cbdata;

    ORTE_ACQUIRE_OBJECT(caddy);

    /* cleanup */
    if (NULL != caddy) {
        OBJ_RELEASE(caddy);
    }

    /* check one-time lock to protect against "bounce" */
    if (opal_atomic_trylock(&orte_quit_lock)) { /* returns 1 if already locked */
        return;
    }

    /* flag that the event lib should no longer be looped
     * so we will exit
     */
    orte_event_base_active = false;
    ORTE_POST_OBJECT(orte_event_base_active);
    /* break out of the event loop */
    opal_event_base_loopbreak(orte_event_base);
}

static char* print_aborted_job(orte_job_t *job,
                               orte_app_context_t *approc,
                               orte_proc_t *proc,
                               orte_node_t *node)
{
    char *output = NULL;

    if (ORTE_PROC_STATE_FAILED_TO_START == proc->state ||
        ORTE_PROC_STATE_FAILED_TO_LAUNCH == proc->state) {
        switch (proc->exit_code) {
        case ORTE_ERR_SILENT:
            /* say nothing - it was already reported */
            break;
        case ORTE_ERR_SYS_LIMITS_PIPES:
            output = orte_show_help_string("help-orterun.txt", "orterun:sys-limit-pipe", true,
                                           orte_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_PIPE_SETUP_FAILURE:
            output = orte_show_help_string("help-orterun.txt", "orterun:pipe-setup-failure", true,
                                           orte_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_SYS_LIMITS_CHILDREN:
            output = orte_show_help_string("help-orterun.txt", "orterun:sys-limit-children", true,
                                           orte_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_FAILED_GET_TERM_ATTRS:
            output = orte_show_help_string("help-orterun.txt", "orterun:failed-term-attrs", true,
                                           orte_basename, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_WDIR_NOT_FOUND:
            output = orte_show_help_string("help-orterun.txt", "orterun:wdir-not-found", true,
                                           orte_basename, approc->cwd,
                                           node->name, (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_EXE_NOT_FOUND:
            output = orte_show_help_string("help-orterun.txt", "orterun:exe-not-found", true,
                                           orte_basename,
                                           (unsigned long)proc->name.vpid,
                                           orte_basename,
                                           orte_basename,
                                           node->name,
                                           approc->app);
            break;
        case ORTE_ERR_EXE_NOT_ACCESSIBLE:
            output = orte_show_help_string("help-orterun.txt", "orterun:exe-not-accessible", true,
                                           orte_basename, approc->app, node->name,
                                           (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_MULTIPLE_AFFINITIES:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:multiple-paffinity-schemes", true, NULL);
            break;
        case ORTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:topo-not-supported",
                                           true, orte_process_info.nodename, "rankfile containing a slot_list of ",
                                           NULL, approc->app);
            break;
        case ORTE_ERR_INVALID_NODE_RANK:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:invalid-node-rank", true);
            break;
        case ORTE_ERR_INVALID_LOCAL_RANK:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:invalid-local-rank", true);
            break;
        case ORTE_ERR_NOT_ENOUGH_CORES:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:not-enough-resources", true,
                                           "sockets", node->name,
                                           "bind-to-core", approc->app);
            break;
        case ORTE_ERR_TOPO_CORE_NOT_SUPPORTED:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:topo-not-supported",
                                           true, node->name, "bind-to-core", "",
                                           approc->app);
            break;
        case ORTE_ERR_INVALID_PHYS_CPU:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:invalid-phys-cpu", true);
            break;
        case ORTE_ERR_NOT_ENOUGH_SOCKETS:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:not-enough-resources", true,
                                           "sockets", node->name,
                                           "bind-to-socket", approc->app);
            break;
        case ORTE_ERR_TOPO_SOCKET_NOT_SUPPORTED:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:topo-not-supported",
                                           true, node->name, "bind-to-socket", "",
                                           approc->app);
            break;
        case ORTE_ERR_MODULE_NOT_FOUND:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:paffinity-missing-module",
                                           true, node->name);
            break;
        case ORTE_ERR_SLOT_LIST_RANGE:
            output = orte_show_help_string("help-orterun.txt",
                                           "orterun:invalid-slot-list-range",
                                           true, node->name, NULL);
            break;
        case ORTE_ERR_PIPE_READ_FAILURE:
            output = orte_show_help_string("help-orterun.txt", "orterun:pipe-read-failure", true,
                                           orte_basename, node->name, (unsigned long)proc->name.vpid);
            break;
        case ORTE_ERR_SOCKET_NOT_AVAILABLE:
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-socket-not-avail", true,
                                           orte_basename, ORTE_ERROR_NAME(proc->exit_code), node->name,
                                           (unsigned long)proc->name.vpid);
            break;

        default:
            if (0 != proc->exit_code) {
                output = orte_show_help_string("help-orterun.txt", "orterun:proc-failed-to-start", true,
                                               orte_basename, proc->exit_code, ORTE_ERROR_NAME(proc->exit_code),
                                               node->name, (unsigned long)proc->name.vpid);
            } else {
                output = orte_show_help_string("help-orterun.txt", "orterun:proc-failed-to-start-no-status", true,
                                               orte_basename, node->name);
            }
        }
        return output;
    } else if (ORTE_PROC_STATE_ABORTED == proc->state) {
        output = orte_show_help_string("help-orterun.txt", "orterun:proc-ordered-abort", true,
                                       orte_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                       node->name, orte_basename);
        return output;
    } else if (ORTE_PROC_STATE_ABORTED_BY_SIG == job->state) {  /* aborted by signal */
#ifdef HAVE_STRSIGNAL
        if (NULL != strsignal(WTERMSIG(proc->exit_code))) {
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-aborted-strsignal", true,
                                           orte_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                           node->name, WTERMSIG(proc->exit_code),
                                           strsignal(WTERMSIG(proc->exit_code)));
        } else {
#endif
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-aborted", true,
                                           orte_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                           node->name, WTERMSIG(proc->exit_code));
#ifdef HAVE_STRSIGNAL
        }
#endif
        return output;
    } else if (ORTE_PROC_STATE_TERM_WO_SYNC == proc->state) { /* proc exited w/o finalize */
        output = orte_show_help_string("help-orterun.txt", "orterun:proc-exit-no-sync", true,
                                       orte_basename, (unsigned long)proc->name.vpid, (unsigned long)proc->pid,
                                       node->name, orte_basename, orte_basename);
        return output;
    } else if (ORTE_PROC_STATE_COMM_FAILED == proc->state) {
        output = orte_show_help_string("help-orterun.txt", "orterun:proc-comm-failed", true,
                                       ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                       ORTE_NAME_PRINT(&proc->name), node->name);
        return output;
    } else if (ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == proc->state) {
        switch (proc->exit_code) {
        case ORTE_ERR_MEM_LIMIT_EXCEEDED:
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-mem-exceeded", true,
                                           ORTE_NAME_PRINT(&proc->name), node->name);
            break;
        case ORTE_ERR_PROC_STALLED:
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-stalled", true);
            break;

        default:
            output = orte_show_help_string("help-orterun.txt", "orterun:proc-sensor-exceeded", true);
        }
        return output;
    } else if (ORTE_PROC_STATE_HEARTBEAT_FAILED == proc->state) {
        output = orte_show_help_string("help-orterun.txt", "orterun:proc-heartbeat-failed", true,
                                       orte_basename, ORTE_NAME_PRINT(&proc->name), node->name);
        return output;
    } else if (orte_abort_non_zero_exit &&
               ORTE_PROC_STATE_TERM_NON_ZERO == proc->state) {
        output = orte_show_help_string("help-orterun.txt", "orterun:non-zero-exit", true,
                                       orte_basename, ORTE_NAME_PRINT(&proc->name), proc->exit_code);
        return output;
    }

    /* nothing here */
    return NULL;
}

/*
 * On abnormal termination - dump the
 * exit status of the aborted procs.
 */

char* orte_dump_aborted_procs(orte_job_t *jdata)
{
    orte_job_t *job, *launcher;
    orte_std_cntr_t i;
    orte_proc_t *proc, *pptr;
    orte_app_context_t *approc;
    orte_node_t *node;
    char *output = NULL;

    /* if this job is not a launcher itself, then get the launcher for this job */
    if (ORTE_JOBID_INVALID == jdata->launcher) {
        launcher = jdata;
    } else {
        launcher = orte_get_job_data_object(jdata->launcher);
        if (NULL == launcher) {
            output = strdup("LAUNCHER JOB OBJECT NOT FOUND");
            return output;
        }
    }

    /* cycle thru all the children of this launcher to find the
     * one that caused the error */
    OPAL_LIST_FOREACH(job, &launcher->children, orte_job_t) {
        /* cycle through and count the number that were killed or aborted */
        for (i=0; i < job->procs->size; i++) {
            if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(job->procs, i))) {
                /* array is left-justfied - we are done */
                break;
            }
            if (ORTE_PROC_STATE_FAILED_TO_START == pptr->state ||
                ORTE_PROC_STATE_FAILED_TO_LAUNCH == pptr->state) {
                ++num_failed_start;
            } else if (ORTE_PROC_STATE_ABORTED == pptr->state) {
                ++num_aborted;
            } else if (ORTE_PROC_STATE_ABORTED_BY_SIG == pptr->state) {
                ++num_killed;
            } else if (ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == pptr->state) {
                ++num_killed;
            }
        }
        /* see if there is a guilty party */
        proc = NULL;
        if (!orte_get_attribute(&job->attributes, ORTE_JOB_ABORTED_PROC, (void**)&proc, OPAL_PTR) ||
            NULL == proc) {
            continue;
        }

        approc = (orte_app_context_t*)opal_pointer_array_get_item(job->apps, proc->app_idx);
        node = proc->node;
        if (NULL != (output = print_aborted_job(job, approc, proc, node))) {
            break;
        }
    }

    return output;
}
