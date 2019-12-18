/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <sys/syscall.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/prrte_environ.h"

#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/odls/alps/odls_alps.h"

int prrte_odls_alps_get_rdma_creds(void)
{
    int alps_status = 0, num_creds, i, len;
    uint64_t apid;
    size_t alps_count;
    int ret = PRRTE_SUCCESS;
    alpsAppLLIGni_t *rdmacred_rsp=NULL;
    alpsAppGni_t *rdmacred_buf;
    char *ptr;
    char env_buffer[1024];
    static int already_got_creds = 0;

    /*
     * If we already put the GNI RDMA credentials into prrte_launch_environ,
     * no need to do anything.
     * TODO: kind of ugly, need to implement an prrte_getenv
     */

    if (1 == already_got_creds) {
        return PRRTE_SUCCESS;
    }

    /*
     * get the Cray HSN RDMA credentials here and stuff them in to the
     * PMI env variable format expected by uGNI consumers like the uGNI
     * BTL, etc. Stuff into the prrte_launch_environ to make sure the
     * application processes can actually use the HSN API (uGNI).
     */

    ret = alps_app_lli_lock();

    /*
     * First get our apid
     */

    ret = alps_app_lli_put_request(ALPS_APP_LLI_ALPS_REQ_APID, NULL, 0);
    if (ALPS_APP_LLI_ALPS_STAT_OK != ret) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                              "%s odls:alps: alps_app_lli_put_request returned %d",
                              PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
         ret = PRRTE_ERR_FILE_WRITE_FAILURE;
         goto fn_exit;
    }

    ret = alps_app_lli_get_response (&alps_status, &alps_count);
    if (ALPS_APP_LLI_ALPS_STAT_OK != alps_status) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: alps_app_lli_get_response returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), alps_status));
        ret = PRRTE_ERR_FILE_READ_FAILURE;
        goto fn_exit;
    }

    ret = alps_app_lli_get_response_bytes (&apid, sizeof(apid));
    if (ALPS_APP_LLI_ALPS_STAT_OK != ret) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: alps_app_lli_get_response_bytes returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        ret = PRRTE_ERR_FILE_READ_FAILURE;
        goto fn_exit;
    }

    /*
     * now get the GNI rdma credentials info
     */

    ret = alps_app_lli_put_request(ALPS_APP_LLI_ALPS_REQ_GNI, NULL, 0);
    if (ALPS_APP_LLI_ALPS_STAT_OK != ret) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: alps_app_lli_put_request returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        ret = PRRTE_ERR_FILE_WRITE_FAILURE;
        goto fn_exit;
    }

    ret = alps_app_lli_get_response(&alps_status, &alps_count);
    if (ALPS_APP_LLI_ALPS_STAT_OK != alps_status) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: alps_app_lli_get_response returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), alps_status));
        ret = PRRTE_ERR_FILE_READ_FAILURE;
        goto fn_exit;
    }

    rdmacred_rsp = (alpsAppLLIGni_t *)malloc(alps_count);
    if (NULL == rdmacred_rsp) {
        ret = PRRTE_ERR_OUT_OF_RESOURCE;
        goto fn_exit;
    }

    memset(rdmacred_rsp,0,alps_count);

    ret = alps_app_lli_get_response_bytes(rdmacred_rsp, alps_count);
    if (ALPS_APP_LLI_ALPS_STAT_OK != ret) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: alps_app_lli_get_response_bytes returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        free(rdmacred_rsp);
        ret = PRRTE_ERR_FILE_READ_FAILURE;
        goto fn_exit;
    }

    ret = alps_app_lli_unlock();

    rdmacred_buf = (alpsAppGni_t *)(rdmacred_rsp->u.buf);

    /*
     * now set up the env. variables -
     * The cray pmi sets up 4 environment variables:
     * PMI_GNI_DEV_ID - format (id0:id1....idX)
     * PMI_GNI_LOC_ADDR - format (locaddr0:locaddr1:....locaddrX)
     * PMI_GNI_COOKIE - format (cookie0:cookie1:...cookieX)
     * PMI_GNI_PTAG - format (ptag0:ptag1:....ptagX)
     *
     * where X == num_creds - 1
     *
     * TODO: need in theory to check for possible overrun of env_buffer
     */

    num_creds = rdmacred_rsp->count;

    /*
     * first build ptag env
     */

    memset(env_buffer,0,sizeof(env_buffer));
    ptr = env_buffer;
    for (i=0; i<num_creds-1; i++) {
        len = sprintf(ptr,"%d:",rdmacred_buf[i].ptag);
        ptr += len;
    }
    sprintf(ptr,"%d",rdmacred_buf[num_creds-1].ptag);
    ret = prrte_setenv("PMI_GNI_PTAG", env_buffer, false, &prrte_launch_environ);
    if (ret != PRRTE_SUCCESS) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: prrte_setenv for PMI_GNI_TAG failed - returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        goto fn_exit;

    } else {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: PMI_GNI_TAG = %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), env_buffer));
    }

    /*
     * the cookie env
     */

    memset(env_buffer,0,sizeof(env_buffer));
    ptr = env_buffer;
    for (i=0; i<num_creds-1; i++) {
        len = sprintf(ptr,"%d:",rdmacred_buf[i].cookie);
        ptr += len;
    }
    sprintf(ptr,"%d",rdmacred_buf[num_creds-1].cookie);
    ret = prrte_setenv("PMI_GNI_COOKIE", env_buffer, false, &prrte_launch_environ);
    if (ret != PRRTE_SUCCESS) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: prrte_setenv for PMI_GNI_COOKIE returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        goto fn_exit;

    } else {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: PMI_GNI_COOKIE = %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), env_buffer));
    }

    /*
     * nic loc addrs
     */

    memset(env_buffer,0,sizeof(env_buffer));
    ptr = env_buffer;
    for (i=0; i<num_creds-1; i++) {
        len = sprintf(ptr,"%d:",rdmacred_buf[i].local_addr);
        ptr += len;
    }
    sprintf(ptr,"%d",rdmacred_buf[num_creds-1].local_addr);
    ret = prrte_setenv("PMI_GNI_LOC_ADDR", env_buffer, false, &prrte_launch_environ);
    if (ret != PRRTE_SUCCESS) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: prrte_setenv for PMI_GNI_LOC_ADDR returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        goto fn_exit;

    } else {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: PMI_GNI_LOC_ADDR = %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), env_buffer));
    }

    /*
     * finally device ids
     */

    memset(env_buffer,0,sizeof(env_buffer));
    ptr = env_buffer;
    for (i=0; i<num_creds-1; i++) {
        len = sprintf(ptr,"%d:",rdmacred_buf[i].device_id);
        ptr += len;
    }
    sprintf(ptr,"%d",rdmacred_buf[num_creds-1].device_id);
    ret = prrte_setenv("PMI_GNI_DEV_ID", env_buffer, false, &prrte_launch_environ);
    if (ret != PRRTE_SUCCESS) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: prrte_setenv for PMI_GNI_DEV_ID returned %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ret));
        goto fn_exit;

    } else {
        PRRTE_OUTPUT_VERBOSE((20, prrte_odls_base_framework.framework_output,
                             "%s odls:alps: PMI_GNI_DEV_ID = %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), env_buffer));
    }

   fn_exit:
    if (PRRTE_SUCCESS == ret) already_got_creds = 1;
    return ret;
}


