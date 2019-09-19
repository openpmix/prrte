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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_CONSTANTS_H
#define PRRTE_CONSTANTS_H

#include "constants.h"
#include "prrte_config.h"

BEGIN_C_DECLS

#define PRRTE_ERR_BASE            0
#define PRRTE_ERR_SPLIT         100

enum {
    /* Error codes inherited from PRRTE.  Still enum values so that we
       get the nice debugger help. */

    PRRTE_SUCCESS                            = (PRRTE_ERR_BASE),

    PRRTE_ERROR                              = (PRRTE_ERR_BASE -  1),
    PRRTE_ERR_OUT_OF_RESOURCE                = (PRRTE_ERR_BASE -  2), /* fatal error */
    PRRTE_ERR_TEMP_OUT_OF_RESOURCE           = (PRRTE_ERR_BASE -  3), /* try again later */
    PRRTE_ERR_RESOURCE_BUSY                  = (PRRTE_ERR_BASE -  4),
    PRRTE_ERR_BAD_PARAM                      = (PRRTE_ERR_BASE -  5),  /* equivalent to MPI_ERR_ARG error code */
    PRRTE_ERR_FATAL                          = (PRRTE_ERR_BASE -  6),
    PRRTE_ERR_NOT_IMPLEMENTED                = (PRRTE_ERR_BASE -  7),
    PRRTE_ERR_NOT_SUPPORTED                  = (PRRTE_ERR_BASE -  8),
    PRRTE_ERR_INTERRUPTED                    = (PRRTE_ERR_BASE -  9),
    PRRTE_ERR_WOULD_BLOCK                    = (PRRTE_ERR_BASE - 10),
    PRRTE_ERR_IN_ERRNO                       = (PRRTE_ERR_BASE - 11),
    PRRTE_ERR_UNREACH                        = (PRRTE_ERR_BASE - 12),
    PRRTE_ERR_NOT_FOUND                      = (PRRTE_ERR_BASE - 13),
    PRRTE_EXISTS                             = (PRRTE_ERR_BASE - 14), /* indicates that the specified object already exists */
    PRRTE_ERR_TIMEOUT                        = (PRRTE_ERR_BASE - 15),
    PRRTE_ERR_NOT_AVAILABLE                  = (PRRTE_ERR_BASE - 16),
    PRRTE_ERR_PERM                           = (PRRTE_ERR_BASE - 17), /* no permission */
    PRRTE_ERR_VALUE_OUT_OF_BOUNDS            = (PRRTE_ERR_BASE - 18),
    PRRTE_ERR_FILE_READ_FAILURE              = (PRRTE_ERR_BASE - 19),
    PRRTE_ERR_FILE_WRITE_FAILURE             = (PRRTE_ERR_BASE - 20),
    PRRTE_ERR_FILE_OPEN_FAILURE              = (PRRTE_ERR_BASE - 21),
    PRRTE_ERR_PACK_MISMATCH                  = (PRRTE_ERR_BASE - 22),
    PRRTE_ERR_PACK_FAILURE                   = (PRRTE_ERR_BASE - 23),
    PRRTE_ERR_UNPACK_FAILURE                 = (PRRTE_ERR_BASE - 24),
    PRRTE_ERR_UNPACK_INADEQUATE_SPACE        = (PRRTE_ERR_BASE - 25),
    PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER = (PRRTE_ERR_BASE - 26),
    PRRTE_ERR_TYPE_MISMATCH                  = (PRRTE_ERR_BASE - 27),
    PRRTE_ERR_OPERATION_UNSUPPORTED          = (PRRTE_ERR_BASE - 28),
    PRRTE_ERR_UNKNOWN_DATA_TYPE              = (PRRTE_ERR_BASE - 29),
    PRRTE_ERR_BUFFER                         = (PRRTE_ERR_BASE - 30),
    PRRTE_ERR_DATA_TYPE_REDEF                = (PRRTE_ERR_BASE - 31),
    PRRTE_ERR_DATA_OVERWRITE_ATTEMPT         = (PRRTE_ERR_BASE - 32),
    PRRTE_ERR_MODULE_NOT_FOUND               = (PRRTE_ERR_BASE - 33),
    PRRTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED   = (PRRTE_ERR_BASE - 34),
    PRRTE_ERR_TOPO_SOCKET_NOT_SUPPORTED      = (PRRTE_ERR_BASE - 35),
    PRRTE_ERR_TOPO_CORE_NOT_SUPPORTED        = (PRRTE_ERR_BASE - 36),
    PRRTE_ERR_NOT_ENOUGH_SOCKETS             = (PRRTE_ERR_BASE - 37),
    PRRTE_ERR_NOT_ENOUGH_CORES               = (PRRTE_ERR_BASE - 38),
    PRRTE_ERR_INVALID_PHYS_CPU               = (PRRTE_ERR_BASE - 39),
    PRRTE_ERR_MULTIPLE_AFFINITIES            = (PRRTE_ERR_BASE - 40),
    PRRTE_ERR_SLOT_LIST_RANGE                = (PRRTE_ERR_BASE - 41),
    PRRTE_ERR_NETWORK_NOT_PARSEABLE          = (PRRTE_ERR_BASE - 42),
    PRRTE_ERR_SILENT                         = (PRRTE_ERR_BASE - 43),
    PRRTE_ERR_NOT_INITIALIZED                = (PRRTE_ERR_BASE - 44),
    PRRTE_ERR_NOT_BOUND                      = (PRRTE_ERR_BASE - 45),
    PRRTE_ERR_TAKE_NEXT_OPTION               = (PRRTE_ERR_BASE - 46),
    PRRTE_ERR_PROC_ENTRY_NOT_FOUND           = (PRRTE_ERR_BASE - 47),
    PRRTE_ERR_DATA_VALUE_NOT_FOUND           = (PRRTE_ERR_BASE - 48),
    PRRTE_ERR_CONNECTION_FAILED              = (PRRTE_ERR_BASE - 49),
    PRRTE_ERR_AUTHENTICATION_FAILED          = (PRRTE_ERR_BASE - 50),
    PRRTE_ERR_COMM_FAILURE                   = (PRRTE_ERR_BASE - 51),
    PRRTE_ERR_SERVER_NOT_AVAIL               = (PRRTE_ERR_BASE - 52),
    PRRTE_ERR_IN_PROCESS                     = (PRRTE_ERR_BASE - 53),
    /* PMIx equivalents for notification support */
    PRRTE_ERR_DEBUGGER_RELEASE               = (PRRTE_ERR_BASE - 54),
    PRRTE_ERR_HANDLERS_COMPLETE              = (PRRTE_ERR_BASE - 55),
    PRRTE_ERR_PARTIAL_SUCCESS                = (PRRTE_ERR_BASE - 56),
    PRRTE_ERR_PROC_ABORTED                   = (PRRTE_ERR_BASE - 57),
    PRRTE_ERR_PROC_REQUESTED_ABORT           = (PRRTE_ERR_BASE - 58),
    PRRTE_ERR_PROC_ABORTING                  = (PRRTE_ERR_BASE - 59),
    PRRTE_ERR_NODE_DOWN                      = (PRRTE_ERR_BASE - 60),
    PRRTE_ERR_NODE_OFFLINE                   = (PRRTE_ERR_BASE - 61),
    PRRTE_ERR_JOB_TERMINATED                 = (PRRTE_ERR_BASE - 62),
    PRRTE_ERR_PROC_RESTART                   = (PRRTE_ERR_BASE - 63),
    PRRTE_ERR_PROC_CHECKPOINT                = (PRRTE_ERR_BASE - 64),
    PRRTE_ERR_PROC_MIGRATE                   = (PRRTE_ERR_BASE - 65),
    PRRTE_ERR_EVENT_REGISTRATION             = (PRRTE_ERR_BASE - 66),
    PRRTE_ERR_HEARTBEAT_ALERT                = (PRRTE_ERR_BASE - 67),
    PRRTE_ERR_FILE_ALERT                     = (PRRTE_ERR_BASE - 68),
    PRRTE_ERR_MODEL_DECLARED                 = (PRRTE_ERR_BASE - 69),
    PRRTE_PMIX_LAUNCH_DIRECTIVE              = (PRRTE_ERR_BASE - 70),
    PRRTE_PMIX_LAUNCHER_READY                = (PRRTE_ERR_BASE - 71),
    PRRTE_OPERATION_SUCCEEDED                = (PRRTE_ERR_BASE - 72),

/* error codes specific to PRRTE - don't forget to update
    src/util/error_strings.c when adding new error codes!!
    Otherwise, the error reporting system will potentially crash,
    or at the least not be able to report the new error correctly.
 */

    PRRTE_ERR_RECV_LESS_THAN_POSTED          = (PRRTE_ERR_SPLIT -  1),
    PRRTE_ERR_RECV_MORE_THAN_POSTED          = (PRRTE_ERR_SPLIT -  2),
    PRRTE_ERR_NO_MATCH_YET                   = (PRRTE_ERR_SPLIT -  3),
    PRRTE_ERR_REQUEST                        = (PRRTE_ERR_SPLIT -  4),
    PRRTE_ERR_NO_CONNECTION_ALLOWED          = (PRRTE_ERR_SPLIT -  5),
    PRRTE_ERR_CONNECTION_REFUSED             = (PRRTE_ERR_SPLIT -  6),
    PRRTE_ERR_COMPARE_FAILURE                = (PRRTE_ERR_SPLIT -  9),
    PRRTE_ERR_COPY_FAILURE                   = (PRRTE_ERR_SPLIT - 10),
    PRRTE_ERR_PROC_STATE_MISSING             = (PRRTE_ERR_SPLIT - 11),
    PRRTE_ERR_PROC_EXIT_STATUS_MISSING       = (PRRTE_ERR_SPLIT - 12),
    PRRTE_ERR_INDETERMINATE_STATE_INFO       = (PRRTE_ERR_SPLIT - 13),
    PRRTE_ERR_NODE_FULLY_USED                = (PRRTE_ERR_SPLIT - 14),
    PRRTE_ERR_INVALID_NUM_PROCS              = (PRRTE_ERR_SPLIT - 15),
    PRRTE_ERR_ADDRESSEE_UNKNOWN              = (PRRTE_ERR_SPLIT - 16),
    PRRTE_ERR_SYS_LIMITS_PIPES               = (PRRTE_ERR_SPLIT - 17),
    PRRTE_ERR_PIPE_SETUP_FAILURE             = (PRRTE_ERR_SPLIT - 18),
    PRRTE_ERR_SYS_LIMITS_CHILDREN            = (PRRTE_ERR_SPLIT - 19),
    PRRTE_ERR_FAILED_GET_TERM_ATTRS          = (PRRTE_ERR_SPLIT - 20),
    PRRTE_ERR_WDIR_NOT_FOUND                 = (PRRTE_ERR_SPLIT - 21),
    PRRTE_ERR_EXE_NOT_FOUND                  = (PRRTE_ERR_SPLIT - 22),
    PRRTE_ERR_PIPE_READ_FAILURE              = (PRRTE_ERR_SPLIT - 23),
    PRRTE_ERR_EXE_NOT_ACCESSIBLE             = (PRRTE_ERR_SPLIT - 24),
    PRRTE_ERR_FAILED_TO_START                = (PRRTE_ERR_SPLIT - 25),
    PRRTE_ERR_FILE_NOT_EXECUTABLE            = (PRRTE_ERR_SPLIT - 26),
    PRRTE_ERR_HNP_COULD_NOT_START            = (PRRTE_ERR_SPLIT - 27),
    PRRTE_ERR_SYS_LIMITS_SOCKETS             = (PRRTE_ERR_SPLIT - 28),
    PRRTE_ERR_SOCKET_NOT_AVAILABLE           = (PRRTE_ERR_SPLIT - 29),
    PRRTE_ERR_SYSTEM_WILL_BOOTSTRAP          = (PRRTE_ERR_SPLIT - 30),
    PRRTE_ERR_RESTART_LIMIT_EXCEEDED         = (PRRTE_ERR_SPLIT - 31),
    PRRTE_ERR_INVALID_NODE_RANK              = (PRRTE_ERR_SPLIT - 32),
    PRRTE_ERR_INVALID_LOCAL_RANK             = (PRRTE_ERR_SPLIT - 33),
    PRRTE_ERR_UNRECOVERABLE                  = (PRRTE_ERR_SPLIT - 34),
    PRRTE_ERR_MEM_LIMIT_EXCEEDED             = (PRRTE_ERR_SPLIT - 35),
    PRRTE_ERR_HEARTBEAT_LOST                 = (PRRTE_ERR_SPLIT - 36),
    PRRTE_ERR_PROC_STALLED                   = (PRRTE_ERR_SPLIT - 37),
    PRRTE_ERR_NO_APP_SPECIFIED               = (PRRTE_ERR_SPLIT - 38),
    PRRTE_ERR_NO_EXE_SPECIFIED               = (PRRTE_ERR_SPLIT - 39),
    PRRTE_ERR_COMM_DISABLED                  = (PRRTE_ERR_SPLIT - 40),
    PRRTE_ERR_FAILED_TO_MAP                  = (PRRTE_ERR_SPLIT - 41),
    PRRTE_ERR_SENSOR_LIMIT_EXCEEDED          = (PRRTE_ERR_SPLIT - 42),
    PRRTE_ERR_ALLOCATION_PENDING             = (PRRTE_ERR_SPLIT - 43),
    PRRTE_ERR_NO_PATH_TO_TARGET              = (PRRTE_ERR_SPLIT - 44),
    PRRTE_ERR_OP_IN_PROGRESS                 = (PRRTE_ERR_SPLIT - 45),
    PRRTE_ERR_OPEN_CONDUIT_FAIL              = (PRRTE_ERR_SPLIT - 46),
    PRRTE_ERR_DUPLICATE_MSG                  = (PRRTE_ERR_SPLIT - 47),
    PRRTE_ERR_OUT_OF_ORDER_MSG               = (PRRTE_ERR_SPLIT - 48),
    PRRTE_ERR_FORCE_SELECT                   = (PRRTE_ERR_SPLIT - 49),
    PRRTE_ERR_JOB_CANCELLED                  = (PRRTE_ERR_SPLIT - 50),
    PRRTE_ERR_CONDUIT_SEND_FAIL              = (PRRTE_ERR_SPLIT - 51)
};

#define PRRTE_ERR_MAX                      (PRRTE_ERR_SPLIT - 100)

END_C_DECLS

#endif /* PRRTE_CONSTANTS_H */
