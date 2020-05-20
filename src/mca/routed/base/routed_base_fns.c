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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/dss/dss.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"

#include "src/mca/routed/base/base.h"

void prte_routed_base_xcast_routing(prte_list_t *coll, prte_list_t *my_children)
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
            daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid);
            for (i=1; i < daemons->procs->size; i++) {
                if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(daemons->procs, i))) {
                    continue;
                }
                /* exclude anyone known not alive */
                if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_ALIVE)) {
                    nm = PRTE_NEW(prte_namelist_t);
                    nm->name.jobid = PRTE_PROC_MY_NAME->jobid;
                    nm->name.vpid = proc->name.vpid;
                    prte_list_append(coll, &nm->super);
                }
            }
            /* if nobody is known alive, then we need to die */
            if (0 == prte_list_get_size(coll)) {
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            }
        } else {
            /* the xcast always goes to our children */
            PRTE_LIST_FOREACH(child, my_children, prte_routed_tree_t) {
                nm = PRTE_NEW(prte_namelist_t);
                nm->name.jobid = PRTE_PROC_MY_NAME->jobid;
                nm->name.vpid = child->vpid;
                prte_list_append(coll, &nm->super);
            }
        }
    } else {
        /* I am a daemon - route to my children */
        PRTE_LIST_FOREACH(child, my_children, prte_routed_tree_t) {
            nm = PRTE_NEW(prte_namelist_t);
            nm->name.jobid = PRTE_PROC_MY_NAME->jobid;
            nm->name.vpid = child->vpid;
            prte_list_append(coll, &nm->super);
        }
    }
}

int prte_routed_base_process_callback(prte_jobid_t job, prte_buffer_t *buffer)
{
    prte_proc_t *proc;
    prte_job_t *jdata;
    int32_t cnt;
    char *rml_uri;
    prte_vpid_t vpid;
    int rc;

    /* lookup the job object for this process */
    if (NULL == (jdata = prte_get_job_data_object(job))) {
        /* came from a different job family - this is an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* unpack the data for each entry */
    cnt = 1;
    while (PRTE_SUCCESS == (rc = prte_dss.unpack(buffer, &vpid, &cnt, PRTE_VPID))) {

        if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &rml_uri, &cnt, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            continue;
        }

        PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                             "%s routed_base:callback got uri %s for job %s rank %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (NULL == rml_uri) ? "NULL" : rml_uri,
                             PRTE_JOBID_PRINT(job), PRTE_VPID_PRINT(vpid)));

        if (NULL == rml_uri) {
            /* should not happen */
            PRTE_ERROR_LOG(PRTE_ERR_FATAL);
            return PRTE_ERR_FATAL;
        }

        if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, vpid))) {
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
