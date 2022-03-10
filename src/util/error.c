/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      FUJITSU LIMITED.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/util/error.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_string_copy.h"

const char *prte_strerror(int errnum)
{
    const char *retval;

    switch (errnum) {
    case PRTE_SUCCESS:
        retval = "Success";
        break;
    case PRTE_ERROR:
        retval = "Error";
        break;
    case PRTE_ERR_OUT_OF_RESOURCE:
        retval = "Out of resource";
        break;
    case PRTE_ERR_RESOURCE_BUSY:
        retval = "Resource busy";
        break;
    case PRTE_ERR_BAD_PARAM:
        retval = "Bad parameter";
        break;
    case PRTE_ERR_FATAL:
        retval = "Fatal";
        break;
    case PRTE_ERR_NOT_IMPLEMENTED:
        retval = "Not implemented";
        break;
    case PRTE_ERR_NOT_SUPPORTED:
        retval = "Not supported";
        break;
    case PRTE_ERR_WOULD_BLOCK:
        retval = "Would block";
        break;
    case PRTE_ERR_IN_ERRNO:
        retval = "In errno";
        break;
    case PRTE_ERR_UNREACH:
        retval = "Unreachable";
        break;
    case PRTE_ERR_NOT_FOUND:
        retval = "Not found";
        break;
    case PRTE_EXISTS:
        retval = "Exists";
        break;
    case PRTE_ERR_TIMEOUT:
        retval = "Timeout";
        break;
    case PRTE_ERR_NOT_AVAILABLE:
        retval = "Not available";
        break;
    case PRTE_ERR_NO_PERMISSIONS:
        retval = "No permission";
        break;
    case PRTE_ERR_VALUE_OUT_OF_BOUNDS:
        retval = "Value out of bounds";
        break;
    case PRTE_ERR_FILE_READ_FAILURE:
        retval = "File read failure";
        break;
    case PRTE_ERR_FILE_WRITE_FAILURE:
        retval = "File write failure";
        break;
    case PRTE_ERR_FILE_OPEN_FAILURE:
        retval = "File open failure";
        break;
    case PRTE_ERR_PACK_FAILURE:
        retval = "Data pack failed";
        break;
    case PRTE_ERR_UNPACK_FAILURE:
        retval = "Data unpack failed";
        break;
    case PRTE_ERR_COMM_FAILURE:
        retval = "Comm failure";
        break;
    case PRTE_ERR_PARTIAL_SUCCESS:
        retval = "Partial success";
        break;
    case PRTE_ERR_PROC_ABORTED:
        retval = "Process abnormally terminated";
        break;
    case PRTE_ERR_PROC_REQUESTED_ABORT:
        retval = "Process requested abort";
        break;
    case PRTE_ERR_PROC_ABORTING:
        retval = "Process is aborting";
        break;
    case PRTE_ERR_NODE_DOWN:
        retval = "Node has gone down";
        break;
    case PRTE_ERR_NODE_OFFLINE:
        retval = "Node has gone offline";
        break;
    case PRTE_ERR_JOB_TERMINATED:
        retval = "Job terminated";
        break;
    case PRTE_MONITOR_HEARTBEAT_ALERT:
        retval = "Heartbeat not received";
        break;
    case PRTE_ERR_CONNECTION_REFUSED:
        retval = "Connection refused";
        break;
    case PRTE_ERR_TYPE_MISMATCH:
        retval = "Type mismatch";
        break;
    case PRTE_ERR_SILENT:
        if (prte_report_silent_errors) {
            retval = "Silent error";
        } else {
            retval = "";
        }
        break;
    case PRTE_ERR_ADDRESSEE_UNKNOWN:
        retval = "A message is attempting to be sent to a process whose contact information is "
                 "unknown";
        break;
    case PRTE_ERR_SYS_LIMITS_PIPES:
        retval = "The system limit on number of pipes a process can open was reached";
        break;
    case PRTE_ERR_PIPE_SETUP_FAILURE:
        retval = "A pipe could not be setup between a daemon and one of its local processes";
        break;
    case PRTE_ERR_SYS_LIMITS_CHILDREN:
        retval = "The system limit on number of children a process can have was reached";
        break;
    case PRTE_ERR_JOB_WDIR_NOT_FOUND:
        retval = "The specified working directory could not be found";
        break;
    case PRTE_ERR_JOB_FAILED_TO_LAUNCH:
        retval = "The specified application failed to launch";
        break;
    case PRTE_ERR_SYS_LIMITS_SOCKETS:
        retval = "The system limit on number of network connections a process can open was reached";
        break;
    case PRTE_ERR_SOCKET_NOT_AVAILABLE:
        retval = "Unable to open a TCP socket for out-of-band communications";
        break;
    case PRTE_ERR_JOB_FAILED_TO_MAP:
        retval = "Unable to map job";
        break;
    case PRTE_ERR_TAKE_NEXT_OPTION:
        if (prte_report_silent_errors) {
            retval = "Next option";
        } else {
            retval = "";
        }
        break;
    case PRTE_ERR_ALLOCATION_PENDING:
        retval = "Allocation pending";
        break;
    case PRTE_ERR_NO_PATH_TO_TARGET:
        retval = "No OOB path to target";
        break;
    case PRTE_ERR_JOB_CANCELED:
        retval = "Job canceled";
        break;
    default:
        // Check PMIx just in case. Unless
        // this is a PRRTE specific code that
        // is missing here (and should be added),
        // it'll get returned here with the PRRTE -> PMIx
        // code mapping.
        retval = PMIx_Error_string(errnum);
    }
    return retval;
}
