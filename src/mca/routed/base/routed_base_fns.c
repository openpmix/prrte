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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/dss/dss.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"

#include "src/mca/routed/base/base.h"

void prrte_routed_base_xcast_routing(prrte_list_t *coll, prrte_list_t *my_children)
{
    prrte_routed_tree_t *child;
    prrte_namelist_t *nm;
    int i;
    prrte_proc_t *proc;
    prrte_job_t *daemons;

    /* if we are the HNP and an abnormal termination is underway,
     * then send it directly to everyone
     */
    if (PRRTE_PROC_IS_MASTER) {
        if (prrte_abnormal_term_ordered || !prrte_routing_is_enabled) {
            daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
            for (i=1; i < daemons->procs->size; i++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(daemons->procs, i))) {
                    continue;
                }
                /* exclude anyone known not alive */
                if (PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_ALIVE)) {
                    nm = PRRTE_NEW(prrte_namelist_t);
                    nm->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                    nm->name.vpid = proc->name.vpid;
                    prrte_list_append(coll, &nm->super);
                }
            }
            /* if nobody is known alive, then we need to die */
            if (0 == prrte_list_get_size(coll)) {
                PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        } else {
            /* the xcast always goes to our children */
            PRRTE_LIST_FOREACH(child, my_children, prrte_routed_tree_t) {
                nm = PRRTE_NEW(prrte_namelist_t);
                nm->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                nm->name.vpid = child->vpid;
                prrte_list_append(coll, &nm->super);
            }
        }
    } else {
        /* I am a daemon - route to my children */
        PRRTE_LIST_FOREACH(child, my_children, prrte_routed_tree_t) {
            nm = PRRTE_NEW(prrte_namelist_t);
            nm->name.jobid = PRRTE_PROC_MY_NAME->jobid;
            nm->name.vpid = child->vpid;
            prrte_list_append(coll, &nm->super);
        }
    }
}

int prrte_routed_base_process_callback(prrte_jobid_t job, prrte_buffer_t *buffer)
{
    prrte_proc_t *proc;
    prrte_job_t *jdata;
    prrte_std_cntr_t cnt;
    char *rml_uri;
    prrte_vpid_t vpid;
    int rc;

    /* lookup the job object for this process */
    if (NULL == (jdata = prrte_get_job_data_object(job))) {
        /* came from a different job family - this is an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* unpack the data for each entry */
    cnt = 1;
    while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(buffer, &vpid, &cnt, PRRTE_VPID))) {

        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &rml_uri, &cnt, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            continue;
        }

        PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                             "%s routed_base:callback got uri %s for job %s rank %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             (NULL == rml_uri) ? "NULL" : rml_uri,
                             PRRTE_JOBID_PRINT(job), PRRTE_VPID_PRINT(vpid)));

        if (NULL == rml_uri) {
            /* should not happen */
            PRRTE_ERROR_LOG(PRRTE_ERR_FATAL);
            return PRRTE_ERR_FATAL;
        }

        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, vpid))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            continue;
        }

        /* update the record */
        proc->rml_uri = strdup(rml_uri);
        free(rml_uri);

        cnt = 1;
    }
    if (PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}
