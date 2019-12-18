/* -*- C -*-
 *
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
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */

/*
 * includes
 */
#include "prrte_config.h"

#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/dss/dss.h"
#include "constants.h"
#include "types.h"
#include "src/util/proc_info.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_quit.h"

#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"

/*
 * Functions to process some FileM specific commands
 */
static void filem_base_process_get_proc_node_name_cmd(prrte_process_name_t* sender,
                                                      prrte_buffer_t* buffer);
static void filem_base_process_get_remote_path_cmd(prrte_process_name_t* sender,
                                                   prrte_buffer_t* buffer);

static bool recv_issued=false;

int prrte_filem_base_comm_start(void)
{
    /* Only active in HNP and daemons */
    if( !PRRTE_PROC_IS_MASTER && !PRRTE_PROC_IS_DAEMON ) {
        return PRRTE_SUCCESS;
    }
    if ( recv_issued ) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_filem_base_framework.framework_output,
                         "%s filem:base: Receive: Start command recv",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_FILEM_BASE,
                            PRRTE_RML_PERSISTENT,
                            prrte_filem_base_recv,
                            NULL);

    recv_issued = true;

    return PRRTE_SUCCESS;
}


int prrte_filem_base_comm_stop(void)
{
    /* Only active in HNP and daemons */
    if( !PRRTE_PROC_IS_MASTER && !PRRTE_PROC_IS_DAEMON ) {
        return PRRTE_SUCCESS;
    }
    if ( recv_issued ) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_filem_base_framework.framework_output,
                         "%s filem:base:receive stop comm",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_FILEM_BASE);
    recv_issued = false;

    return PRRTE_SUCCESS;
}


/*
 * handle message from proxies
 * NOTE: The incoming buffer "buffer" is PRRTE_RELEASED by the calling program.
 * DO NOT RELEASE THIS BUFFER IN THIS CODE
 */
void prrte_filem_base_recv(int status, prrte_process_name_t* sender,
                        prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                        void* cbdata)
{
    prrte_filem_cmd_flag_t command;
    prrte_std_cntr_t count;
    int rc;

    PRRTE_OUTPUT_VERBOSE((5, prrte_filem_base_framework.framework_output,
                         "%s filem:base: Receive a command message.",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &command, &count, PRRTE_FILEM_CMD))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    switch (command) {
        case PRRTE_FILEM_GET_PROC_NODE_NAME_CMD:
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:base: Command: Get Proc node name command",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

            filem_base_process_get_proc_node_name_cmd(sender, buffer);
            break;

        case PRRTE_FILEM_GET_REMOTE_PATH_CMD:
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:base: Command: Get remote path command",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

            filem_base_process_get_remote_path_cmd(sender, buffer);
            break;

        default:
            PRRTE_ERROR_LOG(PRRTE_ERR_VALUE_OUT_OF_BOUNDS);
    }
}

static void filem_base_process_get_proc_node_name_cmd(prrte_process_name_t* sender,
                                                      prrte_buffer_t* buffer)
{
    prrte_buffer_t *answer;
    prrte_std_cntr_t count;
    prrte_job_t *jdata = NULL;
    prrte_proc_t *proc = NULL;
    prrte_process_name_t name;
    int rc;

    /*
     * Unpack the data
     */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &name, &count, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        return;
    }

    /*
     * Process the data
     */
    /* get the job data object for this proc */
    if (NULL == (jdata = prrte_get_job_data_object(name.jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        return;
    }
    /* get the proc object for it */
    proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, name.vpid);
    if (NULL == proc || NULL == proc->node) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        return;
    }

    /*
     * Send back the answer
     */
    answer = PRRTE_NEW(prrte_buffer_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &(proc->node->name), 1, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(answer);
        return;
    }

    if (0 > (rc = prrte_rml.send_buffer_nb(sender, answer,
                                          PRRTE_RML_TAG_FILEM_BASE_RESP,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(answer);
        return;
    }
}

/*
 * This function is responsible for:
 * - Constructing the remote absolute path for the specified file/dir
 * - Verify the existence of the file/dir
 * - Determine if the specified file/dir is in fact a file or dir or unknown if not found.
 */
static void filem_base_process_get_remote_path_cmd(prrte_process_name_t* sender,
                                                   prrte_buffer_t* buffer)
{
    prrte_buffer_t *answer;
    prrte_std_cntr_t count;
    char *filename = NULL;
    char *tmp_name = NULL;
    char cwd[PRRTE_PATH_MAX];
    int file_type = PRRTE_FILEM_TYPE_UNKNOWN;
    struct stat file_status;
    int rc;

    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &filename, &count, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        goto CLEANUP;
    }

    /*
     * Determine the absolute path of the file
     */
    if (filename[0] != '/') { /* if it is not an absolute path already */
        if (NULL == getcwd(cwd, sizeof(cwd))) {
            return;
        }
        prrte_asprintf(&tmp_name, "%s/%s", cwd, filename);
    }
    else {
        tmp_name = strdup(filename);
    }

    prrte_output_verbose(10, prrte_filem_base_framework.framework_output,
                        "filem:base: process_get_remote_path_cmd: %s -> %s: Filename Requested (%s) translated to (%s)",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(sender),
                        filename, tmp_name);

    /*
     * Determine if the file/dir exists at that absolute path
     * Determine if the file/dir is a file or a directory
     */
    if (0 != (rc = stat(tmp_name, &file_status) ) ){
        file_type = PRRTE_FILEM_TYPE_UNKNOWN;
    }
    else {
        /* Is it a directory? */
        if(S_ISDIR(file_status.st_mode)) {
            file_type = PRRTE_FILEM_TYPE_DIR;
        }
        else if(S_ISREG(file_status.st_mode)) {
            file_type = PRRTE_FILEM_TYPE_FILE;
        }
    }

    /*
     * Pack up the response
     * Send back the reference type
     * - PRRTE_FILEM_TYPE_FILE    = File
     * - PRRTE_FILEM_TYPE_DIR     = Directory
     * - PRRTE_FILEM_TYPE_UNKNOWN = Could not be determined, or does not exist
     */
    answer = PRRTE_NEW(prrte_buffer_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &tmp_name, 1, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(answer);
        goto CLEANUP;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &file_type, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(answer);
        goto CLEANUP;
    }

    if (0 > (rc = prrte_rml.send_buffer_nb(sender, answer,
                                          PRRTE_RML_TAG_FILEM_BASE_RESP,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(answer);
    }

 CLEANUP:
    if( NULL != filename) {
        free(filename);
        filename = NULL;
    }
    if( NULL != tmp_name) {
        free(tmp_name);
        tmp_name = NULL;
    }
}
