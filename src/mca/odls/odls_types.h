/* Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2016 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_ODLS_TYPES_H
#define PRRTE_MCA_ODLS_TYPES_H

#include "prrte_config.h"
#include "types.h"

#include "src/dss/dss_types.h"

BEGIN_C_DECLS

/* define the orted command flag type */
typedef uint8_t prrte_daemon_cmd_flag_t;
#define PRRTE_DAEMON_CMD_T   PRRTE_UINT8


/*
 * Definitions needed for communication
 */
#define PRRTE_DAEMON_CONTACT_QUERY_CMD       (prrte_daemon_cmd_flag_t) 1
#define PRRTE_DAEMON_KILL_LOCAL_PROCS        (prrte_daemon_cmd_flag_t) 2
#define PRRTE_DAEMON_SIGNAL_LOCAL_PROCS      (prrte_daemon_cmd_flag_t) 3
#define PRRTE_DAEMON_ADD_LOCAL_PROCS         (prrte_daemon_cmd_flag_t) 4
#define PRRTE_DAEMON_HEARTBEAT_CMD           (prrte_daemon_cmd_flag_t) 6
#define PRRTE_DAEMON_EXIT_CMD                (prrte_daemon_cmd_flag_t) 7
#define PRRTE_DAEMON_PROCESS_AND_RELAY_CMD   (prrte_daemon_cmd_flag_t) 9
#define PRRTE_DAEMON_NULL_CMD                (prrte_daemon_cmd_flag_t) 11

/* commands for use by tools */
#define PRRTE_DAEMON_REPORT_JOB_INFO_CMD     (prrte_daemon_cmd_flag_t) 14
#define PRRTE_DAEMON_REPORT_NODE_INFO_CMD    (prrte_daemon_cmd_flag_t) 15
#define PRRTE_DAEMON_REPORT_PROC_INFO_CMD    (prrte_daemon_cmd_flag_t) 16
#define PRRTE_DAEMON_SPAWN_JOB_CMD           (prrte_daemon_cmd_flag_t) 17
#define PRRTE_DAEMON_TERMINATE_JOB_CMD       (prrte_daemon_cmd_flag_t) 18
#define PRRTE_DAEMON_HALT_VM_CMD             (prrte_daemon_cmd_flag_t) 19
#define PRRTE_DAEMON_HALT_DVM_CMD            (prrte_daemon_cmd_flag_t) 20
#define PRRTE_DAEMON_REPORT_JOB_COMPLETE     (prrte_daemon_cmd_flag_t) 21


/* request proc resource usage */
#define PRRTE_DAEMON_TOP_CMD                 (prrte_daemon_cmd_flag_t) 22

/* bootstrap */
#define PRRTE_DAEMON_NAME_REQ_CMD            (prrte_daemon_cmd_flag_t) 23
#define PRRTE_DAEMON_CHECKIN_CMD             (prrte_daemon_cmd_flag_t) 24
#define PRRTE_TOOL_CHECKIN_CMD               (prrte_daemon_cmd_flag_t) 25

/* process msg command */
#define PRRTE_DAEMON_PROCESS_CMD             (prrte_daemon_cmd_flag_t) 26

/* process called "errmgr.abort_procs" */
#define PRRTE_DAEMON_ABORT_PROCS_CALLED      (prrte_daemon_cmd_flag_t) 28

/* nidmap for the DVM */
#define PRRTE_DAEMON_DVM_NIDMAP_CMD          (prrte_daemon_cmd_flag_t) 29
/* add procs for the DVM */
#define PRRTE_DAEMON_DVM_ADD_PROCS           (prrte_daemon_cmd_flag_t) 30

/* for debug purposes, get stack traces from all application procs */
#define PRRTE_DAEMON_GET_STACK_TRACES        (prrte_daemon_cmd_flag_t) 31

/* for memory profiling */
#define PRRTE_DAEMON_GET_MEMPROFILE          (prrte_daemon_cmd_flag_t) 32

/* request full topology string */
#define PRRTE_DAEMON_REPORT_TOPOLOGY_CMD     (prrte_daemon_cmd_flag_t) 33

/* tell DVM daemons to cleanup resources from job */
#define PRRTE_DAEMON_DVM_CLEANUP_JOB_CMD     (prrte_daemon_cmd_flag_t) 34

/* pass node info */
#define PRRTE_DAEMON_PASS_NODE_INFO_CMD      (prrte_daemon_cmd_flag_t) 35

/*
 * Struct written up the pipe from the child to the parent.
 */
typedef struct {
    /* True if the child has died; false if this is just a warning to
       be printed. */
    bool fatal;
    /* Relevant only if fatal==true */
    int exit_status;

    /* Length of the strings that are written up the pipe after this
       struct */
    int file_str_len;
    int topic_str_len;
    int msg_str_len;
} prrte_odls_pipe_err_msg_t;

/*
 * Max length of strings from the prrte_odls_pipe_err_msg_t
 */
#define PRRTE_ODLS_MAX_FILE_LEN 511
#define PRRTE_ODLS_MAX_TOPIC_LEN PRRTE_ODLS_MAX_FILE_LEN


END_C_DECLS

#endif
