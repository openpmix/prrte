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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"

/*
 * attempt to create a globally unique name
 */
int prte_plm_base_set_hnp_name(void)
{
    char *evar;

    /* we may have been passed a PMIx nspace to use */
    if (NULL != (evar = getenv("PMIX_SERVER_NSPACE"))) {
        PMIX_LOAD_PROCID(&prte_process_info.myproc, evar, 0);
        prte_plm_globals.base_nspace = strdup(evar);

        if (NULL != (evar = getenv("PMIX_SERVER_RANK"))) {
            PRTE_PROC_MY_NAME->rank = strtoul(evar, NULL, 10);
        }
        /* copy it to the HNP field */
        memcpy(PRTE_PROC_MY_HNP, PRTE_PROC_MY_NAME, sizeof(pmix_proc_t));
        return PRTE_SUCCESS;
    }

    if (NULL == prte_plm_globals.base_nspace) {
        /* use pmix_basename.hostname-pid as our base nspace */
        pmix_asprintf(&prte_plm_globals.base_nspace, "%s-%s-%u", prte_tool_basename,
                      prte_process_info.nodename, (uint32_t) prte_process_info.pid);
    }

    /* create the DVM nspace */
    pmix_asprintf(&evar, "%s@0", prte_plm_globals.base_nspace);
    PMIX_LOAD_PROCID(PRTE_PROC_MY_NAME, evar, 0);
    /* copy it to the HNP field */
    memcpy(PRTE_PROC_MY_HNP, PRTE_PROC_MY_NAME, sizeof(pmix_proc_t));

    /* done */
    free(evar);
    return PRTE_SUCCESS;
}

/*
 * Name a job
 *
 * Naming is deliberately separate from entering the job in the global
 * registry, and it happens as early as the HNP can possibly do it: the
 * moment a job object exists here.  An unnamed job carries the EMPTY
 * namespace, and PMIx reads an empty namespace as a WILDCARD -
 * PMIx_Check_nspace() answers true against anything - so every identity
 * test an unnamed job passes through silently succeeds.  That has bitten
 * this code base repeatedly: it disabled a reservation's ownership gate,
 * it let one application's early failure be taken for the daemon job and
 * terminate the DVM, and it let a completing job clear another
 * job's slot in session->jobs.  Each was repaired where it was found; a
 * job that has a name from the outset cannot raise the next one.
 *
 * Registration in prte_job_data waits for INIT (prte_plm_base_setup_job).
 * That array is what a daemon joining the DVM is caught up from
 * (prte_util_pack_job_catchup) and what the job queries enumerate, and a
 * job named at the door may still be waiting on an allocation, or parked
 * in prte_cache until the DVM is ready, with no map to describe yet.
 */
int prte_plm_base_next_jobid(pmix_nspace_t nspace)
{
    uint32_t i, jid;
    pmix_nspace_t pjid;

    /* Find an unused jobid at or after next_jobid, wrapping past the end.
     * Every name is checked, not only once the counter has wrapped: a
     * tool may name itself, and one that took "base@N" ahead of us would
     * otherwise be handed the next job, or the next tool, of that name -
     * the job's processes joining the tool's namespace.  The scan starts
     * where we left off rather than at 1 because a name is handed out here
     * but not recorded in prte_job_data until the job reaches INIT: a scan
     * that always restarted at 1 would hand the same hole to every job
     * named while the first one is still waiting for its allocation. */
    jid = prte_plm_globals.next_jobid;
    for (i = 0; i < UINT32_MAX; i++) {
        if (0 == jid || UINT32_MAX == jid) {
            /* "@0" is the DVM itself */
            jid = 1;
        }
        (void) snprintf(pjid, PMIX_MAX_NSLEN - 1, "%s@%u", prte_plm_globals.base_nspace, jid);
        if (NULL == prte_get_job_data_object(pjid)) {
            PMIX_LOAD_NSPACE(nspace, pjid);
            prte_plm_globals.next_jobid = jid + 1;
            return PRTE_SUCCESS;
        }
        ++jid;
    }
    /* we have run out of jobids! */
    pmix_output(0, "Whoa! What are you doing starting that many jobs concurrently? We are "
                   "out of jobids!");
    return PRTE_ERR_OUT_OF_RESOURCE;
}

int prte_plm_base_create_jobid(prte_job_t *jdata)
{
    if (!PMIX_NSPACE_INVALID(jdata->nspace)) {
        /* already named - a restarted job keeps the name it had, and a job
         * named at the DVM's door reaches INIT already carrying one */
        return PRTE_SUCCESS;
    }
    return prte_plm_base_next_jobid(jdata->nspace);
}
