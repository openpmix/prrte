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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/util/pmix_argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/odls/odls_types.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"

#include "src/mca/routed/base/base.h"

void prte_routed_base_xcast_routing(pmix_list_t *coll, pmix_list_t *my_children)
{
    prte_routed_tree_t *child;
    prte_namelist_t *nm;
    int i;
    prte_proc_t *proc;
    prte_job_t *daemons;

    /* if we are the HNP and an abnormal termination is underway,
     * then send it directly to everyone
     */
    if (PRTE_PROC_IS_MASTER) {
        if (prte_abnormal_term_ordered || !prte_routing_is_enabled) {
            daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            for (i = 1; i < daemons->procs->size; i++) {
                if (NULL
                    == (proc = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, i))) {
                    continue;
                }
                /* exclude anyone known not alive */
                if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_ALIVE)) {
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, proc->name.rank);
                    pmix_list_append(coll, &nm->super);
                }
            }
            /* if nobody is known alive, then we need to die */
            if (0 == pmix_list_get_size(coll)) {
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        } else {
            /* the xcast always goes to our children */
            PMIX_LIST_FOREACH(child, my_children, prte_routed_tree_t)
            {
                nm = PMIX_NEW(prte_namelist_t);
                PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, child->rank);
                pmix_list_append(coll, &nm->super);
            }
        }
    } else {
        /* I am a daemon - route to my children */
        PMIX_LIST_FOREACH(child, my_children, prte_routed_tree_t)
        {
            nm = PMIX_NEW(prte_namelist_t);
            PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, child->rank);
            pmix_list_append(coll, &nm->super);
        }
    }
}

int prte_routed_base_process_callback(pmix_nspace_t job, pmix_data_buffer_t *buffer)
{
    prte_proc_t *proc;
    prte_job_t *jdata;
    int32_t cnt;
    char *rml_uri;
    pmix_rank_t vpid;
    int rc;

    /* lookup the job object for this process */
    if (NULL == (jdata = prte_get_job_data_object(job))) {
        /* came from a different job family - this is an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* unpack the data for each entry */
    cnt = 1;
    while (PRTE_SUCCESS == (rc = PMIx_Data_unpack(NULL, buffer, &vpid, &cnt, PMIX_PROC_RANK))) {

        rc = PMIx_Data_unpack(NULL, buffer, &rml_uri, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS == rc) {
            PMIX_ERROR_LOG(rc);
            continue;
        }

        PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                             "%s routed_base:callback got uri %s for job %s rank %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (NULL == rml_uri) ? "NULL" : rml_uri, PRTE_JOBID_PRINT(job),
                             PRTE_VPID_PRINT(vpid)));

        if (NULL == rml_uri) {
            /* should not happen */
            PRTE_ERROR_LOG(PRTE_ERR_FATAL);
            return PRTE_ERR_FATAL;
        }

        if (NULL == (proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, vpid))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            continue;
        }

        /* update the record */
        proc->rml_uri = strdup(rml_uri);
        free(rml_uri);

        cnt = 1;
    }
    if (PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}
