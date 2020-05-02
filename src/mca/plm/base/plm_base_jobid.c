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
    char *evar;
    int rc;

    /* we may have been passed a PMIx nspace to use */
    if (NULL != (evar = getenv("PMIX_SERVER_NSPACE"))) {
        PMIX_LOAD_PROCID(&prrte_process_info.myproc, evar, 0);
        /* setup the corresponding numerical jobid and add the
         * job to the hash table */
        PRRTE_PMIX_REGISTER_DAEMON_NSPACE(&PRRTE_PROC_MY_NAME->jobid, evar);

        if (NULL != (evar = getenv("PMIX_SERVER_RANK"))) {
            PRRTE_PROC_MY_NAME->vpid = strtoul(evar, NULL, 10);
        } else {
            PRRTE_PROC_MY_NAME->vpid = 0;
        }
        /* copy it to the HNP field */
        PRRTE_PROC_MY_HNP->jobid = PRRTE_PROC_MY_NAME->jobid;
        PRRTE_PROC_MY_HNP->vpid = PRRTE_PROC_MY_NAME->vpid;
        return PRRTE_SUCCESS;
    }

    /* for our nspace, we will use the nodename+pid */
    prrte_asprintf(&evar, "%s-%s%u", prrte_tool_basename, prrte_process_info.nodename, (uint32_t)prrte_process_info.pid);

    PRRTE_PMIX_CONVERT_NSPACE(rc, &PRRTE_PROC_MY_NAME->jobid, evar);
    if (PRRTE_SUCCESS != rc) {
        free(evar);
        return rc;
    }
    PRRTE_PROC_MY_NAME->vpid = 0;

    /* copy it to the HNP field */
    PRRTE_PROC_MY_HNP->jobid = PRRTE_PROC_MY_NAME->jobid;
    PRRTE_PROC_MY_HNP->vpid = PRRTE_PROC_MY_NAME->vpid;

    /* set the nspace */
    PMIX_LOAD_NSPACE(prrte_process_info.myproc.nspace, evar);
    prrte_process_info.myproc.rank = 0;


    /* done */
    free(evar);
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
    char *tmp;
    int rc;

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

    /* the new nspace is our nspace with a ".N" extension */
    prrte_asprintf(&tmp, "%s.%u", prrte_process_info.myproc.nspace, (unsigned)prrte_plm_globals.next_jobid);
    PMIX_LOAD_NSPACE(jdata->nspace, tmp);
    free(tmp);
    PRRTE_PMIX_CONVERT_NSPACE(rc, &jdata->jobid, jdata->nspace);
    if (PRRTE_SUCCESS != rc) {
        return rc;
    }
    prrte_plm_globals.next_jobid++;
    if (INT16_MAX == prrte_plm_globals.next_jobid) {
        reuse = true;
        prrte_plm_globals.next_jobid = 1;
    }

    return PRRTE_SUCCESS;
}
