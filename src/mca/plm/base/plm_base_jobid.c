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
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
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
#include "src/util/printf.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/pmix/pmix-internal.h"
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

    /* set the nspace */
    PRRTE_PMIX_CREATE_NSPACE(prrte_process_info.myproc.nspace, PRRTE_PROC_MY_NAME->jobid);

    /* done */
    return PRRTE_SUCCESS;
}

/*
 * Create a jobid
 */
static bool reuse = false;

int prrte_plm_base_create_jobid(prrte_job_t *jdata)
{
    int16_t i;
    prrte_jobid_t pjid;
    prrte_job_t *ptr;
    bool found;

    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        /* this job is being restarted - do not assign it
         * a new jobid
         */
        return PRRTE_SUCCESS;
    }

    if (reuse) {
        /* find the first unused jobid */
        found = false;
        for (i=1; i < INT16_MAX; i++) {
            ptr = NULL;
            pjid = PRRTE_CONSTRUCT_LOCAL_JOBID(PRRTE_PROC_MY_NAME->jobid, prrte_plm_globals.next_jobid);
            prrte_hash_table_get_value_uint32(prrte_job_data, pjid, (void**)&ptr);
            if (NULL == ptr) {
                found = true;
                break;
            }

            if (INT16_MAX == prrte_plm_globals.next_jobid) {
                prrte_plm_globals.next_jobid = 1;
            } else {
                prrte_plm_globals.next_jobid++;
            }
        }
        if (!found) {
            /* we have run out of jobids! */
            prrte_output(0, "Whoa! What are you doing starting that many jobs concurrently? We are out of jobids!");
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    /* take the next jobid */
    jdata->jobid =  PRRTE_CONSTRUCT_LOCAL_JOBID(PRRTE_PROC_MY_NAME->jobid, prrte_plm_globals.next_jobid);
    PRRTE_PMIX_CREATE_NSPACE(jdata->nspace, jdata->jobid);
    prrte_plm_globals.next_jobid++;
    if (INT16_MAX == prrte_plm_globals.next_jobid) {
        reuse = true;
        prrte_plm_globals.next_jobid = 1;
    }

    return PRRTE_SUCCESS;
}
