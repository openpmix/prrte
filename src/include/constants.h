/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_CONSTANTS_H
#define PRTE_CONSTANTS_H

#include "prte_config.h"
#include "constants.h"
#include "pmix_common.h"

BEGIN_C_DECLS

#define PRTE_SUCCESS                               PMIX_SUCCESS
#define PRTE_ERROR                                 PMIX_ERROR           // general error
/* fault tolerance */
#define PRTE_ERR_PROC_RESTART                      PMIX_ERR_PROC_RESTART
#define PRTE_ERR_PROC_CHECKPOINT                   PMIX_ERR_PROC_CHECKPOINT
#define PRTE_ERR_PROC_MIGRATE                      PMIX_ERR_PROC_MIGRATE
#define PRTE_ERR_EXISTS                            PMIX_ERR_EXISTS
/* communication failures */
#define PRTE_ERR_INVALID_CRED                      PMIX_ERR_INVALID_CRED                       
#define PRTE_ERR_WOULD_BLOCK                       PMIX_ERR_WOULD_BLOCK
#define PRTE_ERR_UNKNOWN_DATA_TYPE                 PMIX_ERR_UNKNOWN_DATA_TYPE
#define PRTE_ERR_TYPE_MISMATCH                     PMIX_ERR_TYPE_MISMATCH
#define PRTE_ERR_UNPACK_INADEQUATE_SPACE           PMIX_ERR_UNPACK_INADEQUATE_SPACE
#define PRTE_ERR_UNPACK_FAILURE                    PMIX_ERR_UNPACK_FAILURE
#define PRTE_ERR_PACK_FAILURE                      PMIX_ERR_PACK_FAILURE
#define PRTE_ERR_NO_PERMISSIONS                    PMIX_ERR_NO_PERMISSIONS
#define PRTE_ERR_TIMEOUT                           PMIX_ERR_TIMEOUT
#define PRTE_ERR_UNREACH                           PMIX_ERR_UNREACH
#define PRTE_ERR_BAD_PARAM                         PMIX_ERR_BAD_PARAM
#define PRTE_ERR_RESOURCE_BUSY                     PMIX_ERR_RESOURCE_BUSY
#define PRTE_ERR_OUT_OF_RESOURCE                   PMIX_ERR_OUT_OF_RESOURCE
#define PRTE_ERR_INIT                              PMIX_ERR_INIT
#define PRTE_ERR_NOMEM                             PMIX_ERR_NOMEM
#define PRTE_ERR_NOT_FOUND                         PMIX_ERR_NOT_FOUND
#define PRTE_ERR_NOT_SUPPORTED                     PMIX_ERR_NOT_SUPPORTED
#define PRTE_ERR_PARAM_VALUE_NOT_SUPPORTED         PMIX_ERR_PARAM_VALUE_NOT_SUPPORTED
#define PRTE_ERR_COMM_FAILURE                      PMIX_ERR_COMM_FAILURE
#define PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER    PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER

#define PRTE_ERR_CONFLICTING_CLEANUP_DIRECTIVES    PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES
#define PRTE_ERR_PARTIAL_SUCCESS                   PMIX_ERR_PARTIAL_SUCCESS
#define PRTE_ERR_DUPLICATE_KEY                     PMIX_ERR_DUPLICATE_KEY
#define PRTE_ERR_EMPTY                             PMIX_ERR_EMPTY
#define PRTE_ERR_LOST_CONNECTION                   PMIX_ERR_LOST_CONNECTION
#define PRTE_ERR_EXISTS_OUTSIDE_SCOPE              PMIX_ERR_EXISTS_OUTSIDE_SCOPE

/* Process set */
#define PRTE_PROCESS_SET_DEFINE                    PMIX_PROCESS_SET_DEFINE
#define PRTE_PROCESS_SET_DELETE                    PMIX_PROCESS_SET_DELETE

/* Debugger ops */
#define PRTE_DEBUGGER_RELEASE                      PMIX_DEBUGGER_RELEASE       // replaced deprecated PRTE_ERR_DEBUGGER_RELEASE
#define PRTE_READY_FOR_DEBUG                       PMIX_READY_FOR_DEBUG    // accompanied by PRTE_BREAKPOINT indicating where proc is waiting

/* query errors */
#define PRTE_QUERY_PARTIAL_SUCCESS                 PMIX_QUERY_PARTIAL_SUCCESS

/* job control */
#define PRTE_JCTRL_CHECKPOINT                      PMIX_JCTRL_CHECKPOINT     // monitored by client to trigger checkpoint operation
#define PRTE_JCTRL_CHECKPOINT_COMPLETE             PMIX_JCTRL_CHECKPOINT_COMPLETE     // sent by client and monitored by server to notify that requested
                                                            //     checkpoint operation has completed
#define PRTE_JCTRL_PREEMPT_ALERT                   PMIX_JCTRL_PREEMPT_ALERT      // monitored by client to detect RM intends to preempt

/* monitoring */
#define PRTE_MONITOR_HEARTBEAT_ALERT                PMIX_MONITOR_HEARTBEAT_ALERT  
#define PRTE_MONITOR_FILE_ALERT                     PMIX_MONITOR_FILE_ALERT
#define PRTE_PROC_TERMINATED                        PMIX_PROC_TERMINATED
#define PRTE_ERR_EVENT_REGISTRATION                 PMIX_ERR_EVENT_REGISTRATION               
#define PRTE_MODEL_DECLARED                         PMIX_MODEL_DECLARED
#define PRTE_MODEL_RESOURCES                        PMIX_MODEL_RESOURCES      // model resource usage has changed
#define PRTE_OPENMP_PARALLEL_ENTERED                PMIX_OPENMP_PARALLEL_ENTERED       // an OpenMP parallel region has been entered
#define PRTE_OPENMP_PARALLEL_EXITED                 PMIX_OPENMP_PARALLEL_EXITED      // an OpenMP parallel region has completed
#define PRTE_LAUNCHER_READY                         PMIX_LAUNCHER_READY
#define PRTE_OPERATION_IN_PROGRESS                  PMIX_OPERATION_IN_PROGRESS
#define PRTE_OPERATION_SUCCEEDED                    PMIX_OPERATION_SUCCEEDED
#define PRTE_ERR_INVALID_OPERATION                  PMIX_ERR_INVALID_OPERATION
#define PRTE_GROUP_INVITED                          PMIX_GROUP_INVITED
#define PRTE_GROUP_LEFT                             PMIX_GROUP_LEFT
#define PRTE_GROUP_INVITE_ACCEPTED                  PMIX_GROUP_INVITE_ACCEPTED
#define PRTE_GROUP_INVITE_DECLINED                  PMIX_GROUP_INVITE_DECLINED
#define PRTE_GROUP_INVITE_FAILED                    PMIX_GROUP_INVITE_FAILED
#define PRTE_GROUP_MEMBERSHIP_UPDATE                PMIX_GROUP_MEMBERSHIP_UPDATE
#define PRTE_GROUP_CONSTRUCT_ABORT                  PMIX_GROUP_CONSTRUCT_ABORT
#define PRTE_GROUP_CONSTRUCT_COMPLETE               PMIX_GROUP_CONSTRUCT_COMPLETE
#define PRTE_GROUP_LEADER_SELECTED                  PMIX_GROUP_LEADER_SELECTED
#define PRTE_GROUP_LEADER_FAILED                    PMIX_GROUP_LEADER_FAILED
#define PRTE_GROUP_CONTEXT_ID_ASSIGNED              PMIX_GROUP_CONTEXT_ID_ASSIGNED
#define PRTE_GROUP_MEMBER_FAILED                    PMIX_GROUP_MEMBER_FAILED
#define PRTE_ERR_REPEAT_ATTR_REGISTRATION           PMIX_ERR_REPEAT_ATTR_REGISTRATION
#define PRTE_ERR_IOF_FAILURE                        PMIX_ERR_IOF_FAILURE
#define PRTE_ERR_IOF_COMPLETE                       PMIX_ERR_IOF_COMPLETE
#define PRTE_LAUNCH_COMPLETE                        PMIX_LAUNCH_COMPLETE     // include nspace of the launched job with notification
#define PRTE_FABRIC_UPDATED                         PMIX_FABRIC_UPDATED
#define PRTE_FABRIC_UPDATE_PENDING                  PMIX_FABRIC_UPDATE_PENDING
#define PRTE_FABRIC_UPDATE_ENDPOINTS                PMIX_FABRIC_UPDATE_ENDPOINTS
#define PRTE_ERR_JOB_APP_NOT_EXECUTABLE             PMIX_ERR_JOB_APP_NOT_EXECUTABLE
#define PRTE_ERR_JOB_NO_EXE_SPECIFIED               PMIX_ERR_JOB_NO_EXE_SPECIFIED
#define PRTE_ERR_JOB_FAILED_TO_MAP                  PMIX_ERR_JOB_FAILED_TO_MAP
#define PRTE_ERR_JOB_CANCELED                       PMIX_ERR_JOB_CANCELED
#define PRTE_ERR_JOB_FAILED_TO_LAUNCH               PMIX_ERR_JOB_FAILED_TO_LAUNCH
#define PRTE_ERR_JOB_ABORTED                        PMIX_ERR_JOB_ABORTED
#define PRTE_ERR_JOB_KILLED_BY_CMD                  PMIX_ERR_JOB_KILLED_BY_CMD
#define PRTE_ERR_JOB_ABORTED_BY_SIG                 PMIX_ERR_JOB_ABORTED_BY_SIG
#define PRTE_ERR_JOB_TERM_WO_SYNC                   PMIX_ERR_JOB_TERM_WO_SYNC
#define PRTE_ERR_JOB_SENSOR_BOUND_EXCEEDED          PMIX_ERR_JOB_SENSOR_BOUND_EXCEEDED
#define PRTE_ERR_JOB_NON_ZERO_TERM                  PMIX_ERR_JOB_NON_ZERO_TERM
#define PRTE_ERR_JOB_ALLOC_FAILED                   PMIX_ERR_JOB_ALLOC_FAILED
#define PRTE_ERR_JOB_ABORTED_BY_SYS_EVENT           PMIX_ERR_JOB_ABORTED_BY_SYS_EVENT
#define PRTE_ERR_JOB_EXE_NOT_FOUND                  PMIX_ERR_JOB_EXE_NOT_FOUND
#define PRTE_ERR_JOB_WDIR_NOT_FOUND                 PMIX_ERR_JOB_WDIR_NOT_FOUND
#define PRTE_ERR_JOB_INSUFFICIENT_RESOURCES         PMIX_ERR_JOB_INSUFFICIENT_RESOURCES
#define PRTE_ERR_JOB_SYS_OP_FAILED                  PMIX_ERR_JOB_SYS_OP_FAILED

/* jobrelated nonerror events */
#define PRTE_EVENT_JOB_START                        PMIX_EVENT_JOB_START
#define PRTE_EVENT_JOB_END                          PMIX_EVENT_JOB_END
#define PRTE_EVENT_SESSION_START                    PMIX_EVENT_SESSION_START
#define PRTE_EVENT_SESSION_END                      PMIX_EVENT_SESSION_END

/* processrelated events */
#define PRTE_ERR_PROC_TERM_WO_SYNC                  PMIX_ERR_PROC_TERM_WO_SYNC
#define PRTE_EVENT_PROC_TERMINATED                  PMIX_EVENT_PROC_TERMINATED

/* system failures */
#define PRTE_EVENT_SYS_BASE                         PMIX_EVENT_SYS_BASE
#define PRTE_EVENT_NODE_DOWN                        PMIX_EVENT_NODE_DOWN
#define PRTE_EVENT_NODE_OFFLINE                     PMIX_EVENT_NODE_OFFLINE

/* define a macro for identifying system event values */
#define PRTE_SYSTEM_EVENT(a)   \
    ((a) <= PRTE_EVENT_SYS_BASE && PRTE_EVENT_SYS_OTHER <= (a))

/* used by event handlers */
#define PRTE_EVENT_NO_ACTION_TAKEN                  PMIX_EVENT_NO_ACTION_TAKEN
#define PRTE_EVENT_PARTIAL_ACTION_TAKEN             PMIX_EVENT_PARTIAL_ACTION_TAKEN
#define PRTE_EVENT_ACTION_DEFERRED                  PMIX_EVENT_ACTION_DEFERRED
#define PRTE_EVENT_ACTION_COMPLETE                  PMIX_EVENT_ACTION_COMPLETE

/* Deprecated from PMIX */
#define PRTE_ERR_SILENT                             PMIX_ERR_SILENT
#define PRTE_ERR_DEBUGGER_RELEASE                   PMIX_ERR_DEBUGGER_RELEASE
#define PRTE_ERR_PROC_ABORTED                       PMIX_ERR_PROC_ABORTED
#define PRTE_ERR_PROC_REQUESTED_ABORT               PMIX_ERR_PROC_REQUESTED_ABORT
#define PRTE_ERR_PROC_ABORTING                      PMIX_ERR_PROC_ABORTING
#define PRTE_ERR_SERVER_FAILED_REQUEST              PMIX_ERR_SERVER_FAILED_REQUEST
#define PRTE_EXISTS                                 PMIX_EXISTS
#define PRTE_ERR_HANDSHAKE_FAILED                   PMIX_ERR_HANDSHAKE_FAILED
#define PRTE_ERR_READY_FOR_HANDSHAKE                PMIX_ERR_READY_FOR_HANDSHAKE
#define PRTE_ERR_PROC_ENTRY_NOT_FOUND               PMIX_ERR_PROC_ENTRY_NOT_FOUND
#define PRTE_ERR_PACK_MISMATCH                      PMIX_ERR_PACK_MISMATCH
#define PRTE_ERR_IN_ERRNO                           PMIX_ERR_IN_ERRNO
#define PRTE_ERR_DATA_VALUE_NOT_FOUND               PMIX_ERR_DATA_VALUE_NOT_FOUND
#define PRTE_ERR_INVALID_ARG                        PMIX_ERR_INVALID_ARG
#define PRTE_ERR_INVALID_KEY                        PMIX_ERR_INVALID_KEY
#define PRTE_ERR_INVALID_KEY_LENGTH                 PMIX_ERR_INVALID_KEY_LENGTH
#define PRTE_ERR_INVALID_VAL                        PMIX_ERR_INVALID_VAL
#define PRTE_ERR_INVALID_VAL_LENGTH                 PMIX_ERR_INVALID_VAL_LENGTH
#define PRTE_ERR_INVALID_LENGTH                     PMIX_ERR_INVALID_LENGTH
#define PRTE_ERR_INVALID_NUM_ARGS                   PMIX_ERR_INVALID_NUM_ARGS
#define PRTE_ERR_INVALID_ARGS                       PMIX_ERR_INVALID_ARGS
#define PRTE_ERR_NODE_OFFLINE                       PMIX_ERR_NODE_OFFLINE
#define PRTE_ERR_NODE_DOWN                          PMIX_ERR_NODE_DOWN

#define PRTE_ERR_INVALID_NUM_PARSED                 PMIX_ERR_INVALID_NUM_PARSED
#define PRTE_ERR_INVALID_KEYVALP                    PMIX_ERR_INVALID_KEYVALP
#define PRTE_ERR_INVALID_SIZE                       PMIX_ERR_INVALID_SIZE
#define PRTE_ERR_INVALID_NAMESPACE                  PMIX_ERR_INVALID_NAMESPACE
#define PRTE_ERR_SERVER_NOT_AVAIL                   PMIX_ERR_SERVER_NOT_AVAIL
#define PRTE_ERR_NOT_IMPLEMENTED                    PMIX_ERR_NOT_IMPLEMENTED
#define PRTE_DEBUG_WAITING_FOR_NOTIFY               PMIX_DEBUG_WAITING_FOR_NOTIFY
#define PRTE_ERR_LOST_CONNECTION_TO_SERVER          PMIX_ERR_LOST_CONNECTION_TO_SERVER
#define PRTE_ERR_LOST_PEER_CONNECTION               PMIX_ERR_LOST_PEER_CONNECTION
#define PRTE_ERR_LOST_CONNECTION_TO_CLIENT          PMIX_ERR_LOST_CONNECTION_TO_CLIENT
#define PRTE_NOTIFY_ALLOC_COMPLETE                  PMIX_NOTIFY_ALLOC_COMPLETE
#define PRTE_ERR_INVALID_TERMINATION                PMIX_ERR_INVALID_TERMINATION
#define PRTE_ERR_JOB_TERMINATED                     PMIX_ERR_JOB_TERMINATED                 // DEPRECATED NAME  nonerror termination
#define PRTE_ERR_UPDATE_ENDPOINTS                   PMIX_ERR_UPDATE_ENDPOINTS
#define PRTE_GDS_ACTION_COMPLETE                    PMIX_GDS_ACTION_COMPLETE
#define PRTE_PROC_HAS_CONNECTED                     PMIX_PROC_HAS_CONNECTED
#define PRTE_CONNECT_REQUESTED                      PMIX_CONNECT_REQUESTED
#define PRTE_ERR_NODE_DOWN                          PMIX_ERR_NODE_DOWN
#define PRTE_ERR_NODE_OFFLINE                       PMIX_ERR_NODE_OFFLINE
#define PRTE_ERR_SYS_BASE                           PMIX_ERR_SYS_BASE
#define PRTE_ERR_SYS_OTHER                          PMIX_ERR_SYS_OTHER

#define PRTE_JOB_STATE_PREPPED                      PMIX_JOB_STATE_PREPPED

/* define a starting point for prte-level defined error
 * constants - negative values larger than this are guaranteed
 * not to conflict with PMIx values. Definitions should always
 * be based on the PRTE_INTERNAL_ERR_BASE constant and -not- a
 * specific value as the value of the constant may change */
#define PRTE_INTERNAL_ERR_BASE                     PMIX_EXTERNAL_ERR_BASE

#define PRTE_ERR_CONNECTION_REFUSED                PRTE_INTERNAL_ERR_BASE - 1
#define PRTE_ERR_ADDRESSEE_UNKNOWN                 PRTE_INTERNAL_ERR_BASE - 2
#define PRTE_ERR_SYS_LIMITS_PIPES                  PRTE_INTERNAL_ERR_BASE - 3
#define PRTE_ERR_PIPE_SETUP_FAILURE                PRTE_INTERNAL_ERR_BASE - 4
#define PRTE_ERR_SYS_LIMITS_CHILDREN               PRTE_INTERNAL_ERR_BASE - 5
#define PRTE_ERR_SYS_LIMITS_SOCKETS                PRTE_INTERNAL_ERR_BASE - 6
#define PRTE_ERR_SOCKET_NOT_AVAILABLE              PRTE_INTERNAL_ERR_BASE - 7
#define PRTE_ERR_ALLOCATION_PENDING                PRTE_INTERNAL_ERR_BASE - 8
#define PRTE_ERR_NO_PATH_TO_TARGET                 PRTE_INTERNAL_ERR_BASE - 9
#define PRTE_ERR_TAKE_NEXT_OPTION                  PRTE_INTERNAL_ERR_BASE - 10
#define PRTE_ERR_NOT_INITIALIZED                   PRTE_INTERNAL_ERR_BASE - 11
#define PRTE_ERR_FILE_OPEN_FAILURE                 PRTE_INTERNAL_ERR_BASE - 12
#define PRTE_ERR_FILE_WRITE_FAILURE                PRTE_INTERNAL_ERR_BASE - 13
#define PRTE_ERR_FILE_READ_FAILURE                 PRTE_INTERNAL_ERR_BASE - 14
#define PRTE_ERR_VALUE_OUT_OF_BOUNDS               PRTE_INTERNAL_ERR_BASE - 15
#define PRTE_ERR_NOT_AVAILABLE                     PRTE_INTERNAL_ERR_BASE - 16
#define PRTE_ERR_FATAL                             PRTE_INTERNAL_ERR_BASE - 17

END_C_DECLS

#endif /* PRTE_CONSTANTS_H */
