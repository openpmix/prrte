/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2016 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#ifdef HAVE_SYS_SIGNAL_H
#include <sys/signal.h>
#else
#include <signal.h>
#endif

#include "src/mca/plm/plm_types.h"
#include "src/util/error_strings.h"
#include "src/runtime/prrte_globals.h"

const char *prrte_job_state_to_str(prrte_job_state_t state)
{
    switch(state) {
    case PRRTE_JOB_STATE_UNDEF:
        return "UNDEFINED";
    case PRRTE_JOB_STATE_INIT:
        return "PENDING INIT";
    case PRRTE_JOB_STATE_INIT_COMPLETE:
        return "INIT_COMPLETE";
    case PRRTE_JOB_STATE_ALLOCATE:
        return "PENDING ALLOCATION";
    case PRRTE_JOB_STATE_ALLOCATION_COMPLETE:
        return "ALLOCATION COMPLETE";
    case PRRTE_JOB_STATE_MAP:
        return "PENDING MAPPING";
    case PRRTE_JOB_STATE_MAP_COMPLETE:
        return "MAP COMPLETE";
    case PRRTE_JOB_STATE_SYSTEM_PREP:
        return "PENDING FINAL SYSTEM PREP";
    case PRRTE_JOB_STATE_LAUNCH_DAEMONS:
        return "PENDING DAEMON LAUNCH";
    case PRRTE_JOB_STATE_DAEMONS_LAUNCHED:
        return "DAEMONS LAUNCHED";
    case PRRTE_JOB_STATE_DAEMONS_REPORTED:
        return "ALL DAEMONS REPORTED";
    case PRRTE_JOB_STATE_VM_READY:
        return "VM READY";
    case PRRTE_JOB_STATE_LAUNCH_APPS:
        return "PENDING APP LAUNCH";
    case PRRTE_JOB_STATE_SEND_LAUNCH_MSG:
        return "SENDING LAUNCH MSG";
    case PRRTE_JOB_STATE_RUNNING:
        return "RUNNING";
    case PRRTE_JOB_STATE_SUSPENDED:
        return "SUSPENDED";
    case PRRTE_JOB_STATE_REGISTERED:
        return "SYNC REGISTERED";
    case PRRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE:
        return "LOCAL LAUNCH COMPLETE";
    case PRRTE_JOB_STATE_UNTERMINATED:
        return "UNTERMINATED";
    case PRRTE_JOB_STATE_TERMINATED:
        return "NORMALLY TERMINATED";
    case PRRTE_JOB_STATE_NOTIFY_COMPLETED:
        return "NOTIFY COMPLETED";
    case PRRTE_JOB_STATE_NOTIFIED:
        return "NOTIFIED";
    case PRRTE_JOB_STATE_ALL_JOBS_COMPLETE:
        return "ALL JOBS COMPLETE";
    case PRRTE_JOB_STATE_ERROR:
        return "ARTIFICIAL BOUNDARY - ERROR";
    case PRRTE_JOB_STATE_KILLED_BY_CMD:
        return "KILLED BY INTERNAL COMMAND";
    case PRRTE_JOB_STATE_ABORTED:
        return "ABORTED";
    case PRRTE_JOB_STATE_FAILED_TO_START:
        return "FAILED TO START";
    case PRRTE_JOB_STATE_ABORTED_BY_SIG:
        return "ABORTED BY SIGNAL";
    case PRRTE_JOB_STATE_ABORTED_WO_SYNC:
        return "TERMINATED WITHOUT SYNC";
    case PRRTE_JOB_STATE_COMM_FAILED:
        return "COMMUNICATION FAILURE";
    case PRRTE_JOB_STATE_SENSOR_BOUND_EXCEEDED:
        return "SENSOR BOUND EXCEEDED";
    case PRRTE_JOB_STATE_CALLED_ABORT:
        return "PROC CALLED ABORT";
    case PRRTE_JOB_STATE_HEARTBEAT_FAILED:
        return "HEARTBEAT FAILED";
    case PRRTE_JOB_STATE_NEVER_LAUNCHED:
        return "NEVER LAUNCHED";
    case PRRTE_JOB_STATE_ABORT_ORDERED:
        return "ABORT IN PROGRESS";
    case PRRTE_JOB_STATE_NON_ZERO_TERM:
        return "AT LEAST ONE PROCESS EXITED WITH NON-ZERO STATUS";
    case PRRTE_JOB_STATE_FAILED_TO_LAUNCH:
        return "FAILED TO LAUNCH";
    case PRRTE_JOB_STATE_FORCED_EXIT:
        return "FORCED EXIT";
    case PRRTE_JOB_STATE_DAEMONS_TERMINATED:
        return "DAEMONS TERMINATED";
    case PRRTE_JOB_STATE_SILENT_ABORT:
        return "ERROR REPORTED ELSEWHERE";
    case PRRTE_JOB_STATE_REPORT_PROGRESS:
        return "REPORT PROGRESS";
    case PRRTE_JOB_STATE_ALLOC_FAILED:
        return "ALLOCATION FAILED";
    case PRRTE_JOB_STATE_MAP_FAILED:
        return "MAP FAILED";
    case PRRTE_JOB_STATE_CANNOT_LAUNCH:
        return "CANNOT LAUNCH";
    case PRRTE_JOB_STATE_FT_CHECKPOINT:
        return "FAULT TOLERANCE CHECKPOINT";
    case PRRTE_JOB_STATE_FT_CONTINUE:
        return "FAULT TOLERANCE CONTINUE";
    case PRRTE_JOB_STATE_FT_RESTART:
        return "FAULT TOLERANCE RESTART";
    case PRRTE_JOB_STATE_ANY:
        return "ANY";
    default:
        return "UNKNOWN STATE!";
    }
}

const char *prrte_app_ctx_state_to_str(prrte_app_state_t state)
{
    switch(state) {
    case PRRTE_APP_STATE_UNDEF:
        return "UNDEFINED";
    case PRRTE_APP_STATE_INIT:
        return "PENDING INIT";
    case PRRTE_APP_STATE_ALL_MAPPED:
        return "ALL MAPPED";
    case PRRTE_APP_STATE_RUNNING:
        return "RUNNING";
    case PRRTE_APP_STATE_COMPLETED:
        return "COMPLETED";
    default:
        return "UNKNOWN STATE!";
    }
}

const char *prrte_proc_state_to_str(prrte_proc_state_t state)
{
    switch(state) {
    case PRRTE_PROC_STATE_UNDEF:
        return "UNDEFINED";
    case PRRTE_PROC_STATE_INIT:
        return "INITIALIZED";
    case PRRTE_PROC_STATE_RESTART:
        return "RESTARTING";
    case PRRTE_PROC_STATE_TERMINATE:
        return "MARKED FOR TERMINATION";
    case PRRTE_PROC_STATE_RUNNING:
        return "RUNNING";
    case PRRTE_PROC_STATE_REGISTERED:
        return "SYNC REGISTERED";
    case PRRTE_PROC_STATE_IOF_COMPLETE:
        return "IOF COMPLETE";
    case PRRTE_PROC_STATE_WAITPID_FIRED:
        return "WAITPID FIRED";
    case PRRTE_PROC_STATE_UNTERMINATED:
        return "UNTERMINATED";
    case PRRTE_PROC_STATE_TERMINATED:
        return "NORMALLY TERMINATED";
    case PRRTE_PROC_STATE_ERROR:
        return "ARTIFICIAL BOUNDARY - ERROR";
    case PRRTE_PROC_STATE_KILLED_BY_CMD:
        return "KILLED BY INTERNAL COMMAND";
    case PRRTE_PROC_STATE_ABORTED:
        return "ABORTED";
    case PRRTE_PROC_STATE_FAILED_TO_START:
        return "FAILED TO START";
    case PRRTE_PROC_STATE_ABORTED_BY_SIG:
        return "ABORTED BY SIGNAL";
    case PRRTE_PROC_STATE_TERM_WO_SYNC:
        return "TERMINATED WITHOUT SYNC";
    case PRRTE_PROC_STATE_COMM_FAILED:
        return "COMMUNICATION FAILURE";
    case PRRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED:
        return "SENSOR BOUND EXCEEDED";
    case PRRTE_PROC_STATE_CALLED_ABORT:
        return "CALLED ABORT";
    case PRRTE_PROC_STATE_HEARTBEAT_FAILED:
        return "HEARTBEAT FAILED";
    case PRRTE_PROC_STATE_MIGRATING:
        return "MIGRATING";
    case PRRTE_PROC_STATE_CANNOT_RESTART:
        return "CANNOT BE RESTARTED";
    case PRRTE_PROC_STATE_TERM_NON_ZERO:
        return "EXITED WITH NON-ZERO STATUS";
    case PRRTE_PROC_STATE_FAILED_TO_LAUNCH:
        return "FAILED TO LAUNCH";
    case PRRTE_PROC_STATE_UNABLE_TO_SEND_MSG:
        return "UNABLE TO SEND MSG";
    case PRRTE_PROC_STATE_LIFELINE_LOST:
        return "LIFELINE LOST";
    case PRRTE_PROC_STATE_NO_PATH_TO_TARGET:
        return "NO PATH TO TARGET";
    case PRRTE_PROC_STATE_FAILED_TO_CONNECT:
        return "FAILED TO CONNECT";
    case PRRTE_PROC_STATE_PEER_UNKNOWN:
        return "PEER UNKNOWN";
    case PRRTE_PROC_STATE_ANY:
        return "ANY";
    default:
        return "UNKNOWN STATE!";
    }
}

const char *prrte_node_state_to_str(prrte_node_state_t state)
{
    switch(state) {
    case PRRTE_NODE_STATE_UNDEF:
        return "UNDEF";
    case PRRTE_NODE_STATE_UNKNOWN:
        return "UNKNOWN";
    case PRRTE_NODE_STATE_DOWN:
        return "DOWN";
    case PRRTE_NODE_STATE_UP:
        return "UP";
    case PRRTE_NODE_STATE_REBOOT:
        return "REBOOT";
    case PRRTE_NODE_STATE_DO_NOT_USE:
        return "DO_NOT_USE";
    case PRRTE_NODE_STATE_NOT_INCLUDED:
        return "NOT_INCLUDED";
    case PRRTE_NODE_STATE_ADDED:
        return "ADDED";
   default:
        return "UNKNOWN STATE!";
    }
}
