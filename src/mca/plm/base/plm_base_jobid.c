/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>

#include "src/include/hash_string.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/plm/base/plm_private.h"

/*
 * attempt to create a globally unique name - do a hash
 * of the hostname plus pid
 */
int prrte_plm_base_set_hnp_name(void)
{
    uint16_t jobfam;
    uint32_t hash32;
    uint32_t bias;

    /* hash the nodename */
    PRRTE_HASH_STR(prrte_process_info.nodename, hash32);

    bias = (uint32_t)prrte_process_info.pid;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "plm:base:set_hnp_name: initial bias %ld nodename hash %lu",
                         (long)bias, (unsigned long)hash32));

    /* fold in the bias */
    hash32 = hash32 ^ bias;

    /* now compress to 16-bits */
    jobfam = (uint16_t)(((0x0000ffff & (0xffff0000 & hash32) >> 16)) ^ (0x0000ffff & hash32));

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "plm:base:set_hnp_name: final jobfam %lu",
                         (unsigned long)jobfam));

    /* set the name */
    PRRTE_PROC_MY_NAME->jobid = 0xffff0000 & ((uint32_t)jobfam << 16);
    PRRTE_PROC_MY_NAME->vpid = 0;

    /* copy it to the HNP field */
    PRRTE_PROC_MY_HNP->jobid = PRRTE_PROC_MY_NAME->jobid;
    PRRTE_PROC_MY_HNP->vpid = PRRTE_PROC_MY_NAME->vpid;

    /* done */
    return PRRTE_SUCCESS;
}

/*
 * Create a jobid
 */
int prrte_plm_base_create_jobid(prrte_job_t *jdata)
{
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        /* this job is being restarted - do not assign it
         * a new jobid
         */
        return PRRTE_SUCCESS;
    }

    if (UINT16_MAX == prrte_plm_globals.next_jobid) {
        /* if we get here, then no local jobids are available */
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        jdata->jobid = PRRTE_JOBID_INVALID;
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* take the next jobid */
    jdata->jobid =  PRRTE_CONSTRUCT_LOCAL_JOBID(PRRTE_PROC_MY_NAME->jobid, prrte_plm_globals.next_jobid);
    prrte_plm_globals.next_jobid++;
    return PRRTE_SUCCESS;
}
