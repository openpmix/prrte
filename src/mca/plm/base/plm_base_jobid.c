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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>

#include "src/include/hash_string.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/proc_info.h"
#include "src/util/printf.h"
#include "src/util/name_fns.h"
#include "src/runtime/prte_globals.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/plm/base/plm_private.h"

/*
 * attempt to create a globally unique name - do a hash
 * of the hostname plus pid
 */
int prte_plm_base_set_hnp_name(void)
{
    char *evar;
    int rc;

    /* we may have been passed a PMIx nspace to use */
    if (NULL != (evar = getenv("PMIX_SERVER_NSPACE"))) {
        PMIX_LOAD_PROCID(&prte_process_info.myproc, evar, 0);
        /* setup the corresponding numerical jobid and add the
         * job to the hash table */
        PRTE_PMIX_REGISTER_DAEMON_NSPACE(&PRTE_PROC_MY_NAME->jobid, evar);

        if (NULL != (evar = getenv("PMIX_SERVER_RANK"))) {
            PRTE_PROC_MY_NAME->vpid = strtoul(evar, NULL, 10);
        } else {
            PRTE_PROC_MY_NAME->vpid = 0;
        }
        /* copy it to the HNP field */
        PRTE_PROC_MY_HNP->jobid = PRTE_PROC_MY_NAME->jobid;
        PRTE_PROC_MY_HNP->vpid = PRTE_PROC_MY_NAME->vpid;
        return PRTE_SUCCESS;
    }

    /* for our nspace, we will use the nodename+pid
     * Note that we do not add the .0 suffix to the namespace as we do with
     * the children namespaces. The daemon jobfamily is always "0".
     * The PRTE_PMIX_CONVERT_NSPACE routine will create the prte_job_t structre
     * and add it to the prte_job_data hash table at position "0".
     */
    prte_asprintf(&evar, "%s-%s%u", prte_tool_basename, prte_process_info.nodename, (uint32_t)prte_process_info.pid);

    PRTE_PMIX_CONVERT_NSPACE(rc, &PRTE_PROC_MY_NAME->jobid, evar);
    if (PRTE_SUCCESS != rc) {
        free(evar);
        return rc;
    }
    PRTE_PROC_MY_NAME->vpid = 0;

    /* copy it to the HNP field */
    PRTE_PROC_MY_HNP->jobid = PRTE_PROC_MY_NAME->jobid;
    PRTE_PROC_MY_HNP->vpid = PRTE_PROC_MY_NAME->vpid;

    /* set the nspace */
    PMIX_LOAD_NSPACE(prte_process_info.myproc.nspace, evar);
    prte_process_info.myproc.rank = 0;

    /* done */
    free(evar);
    return PRTE_SUCCESS;
}

/*
 * Create a jobid
 */
static bool reuse = false;

int prte_plm_base_create_jobid(prte_job_t *jdata)
{
    int16_t i;
    prte_jobid_t pjid;
    prte_job_t *ptr;
    bool found;
    char *tmp;
    int rc;
    prte_job_t *old_jdata;

    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        /* this job is being restarted - do not assign it
         * a new jobid
         */
        return PRTE_SUCCESS;
    }

    if (reuse) {
        /* find the first unused jobid */
        found = false;
        for (i=1; i < INT16_MAX; i++) {
            ptr = NULL;
            pjid = PRTE_CONSTRUCT_LOCAL_JOBID(PRTE_PROC_MY_NAME->jobid, prte_plm_globals.next_jobid);
            prte_hash_table_get_value_uint32(prte_job_data, pjid, (void**)&ptr);
            if (NULL == ptr) {
                found = true;
                break;
            }

            if (INT16_MAX == prte_plm_globals.next_jobid) {
                prte_plm_globals.next_jobid = 1;
            } else {
                prte_plm_globals.next_jobid++;
            }
        }
        if (!found) {
            /* we have run out of jobids! */
            prte_output(0, "Whoa! What are you doing starting that many jobs concurrently? We are out of jobids!");
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    /* the new nspace is our nspace with a ".N" extension */
    prte_asprintf(&tmp, "%s.%u", prte_process_info.myproc.nspace, (unsigned)prte_plm_globals.next_jobid);
    PMIX_LOAD_NSPACE(jdata->nspace, tmp);
    free(tmp);
    // The following routine will create a new prte_job_t structure and add it
    // to the prte_job_data hash table at the ".N" position. Note that the 'new'
    // object is not the 'jdata' referenced here. After this call we have
    // two objects in the system with the same jobid/nspace.
    PRTE_PMIX_CONVERT_NSPACE(rc, &jdata->jobid, jdata->nspace);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    // Replace the 'new' (temporary) object with the one passed to this function
    old_jdata = prte_set_job_data_object(jdata->jobid, jdata);
    // Release the temporary object, but mark the jobid as invalid so the
    // destructor does not remove the object we just put on the hash.
    old_jdata->jobid = PRTE_JOBID_INVALID;
    if (NULL != old_jdata) {
        PRTE_RELEASE(old_jdata);
    }

    prte_plm_globals.next_jobid++;
    if (INT16_MAX == prte_plm_globals.next_jobid) {
        reuse = true;
        prte_plm_globals.next_jobid = 1;
    }

    return PRTE_SUCCESS;
}
