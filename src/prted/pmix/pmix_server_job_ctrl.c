/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"


pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    int32_t signum;
    size_t m, n;
    prte_proc_t *proc;
    pmix_nspace_t jobid;
    pmix_pointer_array_t parray, *ptrarray;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    pmix_proc_t *proct;
    PRTE_HIDE_UNUSED_PARAMS(cbfunc, cbdata);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m = 0; m < ndirs; m++) {
        if (PMIX_CHECK_KEY(&directives[m], PMIX_JOB_CTRL_KILL)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
                for (n = 0; n < ntargets; n++) {
                    if (PMIX_RANK_WILDCARD == targets[n].rank) {
                        /* create an object */
                        proc = PMIX_NEW(prte_proc_t);
                        PMIX_LOAD_PROCID(&proc->name, targets[n].nspace, PMIX_RANK_WILDCARD);
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prte_get_proc_object(&targets[n]))) {
                            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                            continue;
                        }
                        PMIX_RETAIN(proc);
                    }
                    pmix_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(ptrarray))) {
                PRTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j = 0; j < parray.size; j++) {
                    if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(&parray, j))) {
                        PMIX_RELEASE(proc);
                    }
                }
                PMIX_DESTRUCT(&parray);
            }
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            return PMIX_OPERATION_SUCCEEDED;
        }

        if (PMIX_CHECK_KEY(&directives[m], PMIX_JOB_CTRL_TERMINATE)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                PMIX_DATA_BUFFER_CREATE(cmd);
                /* pack the command */
                cmmnd = PRTE_DAEMON_HALT_VM_CMD;
                rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(cmd);
                    return rc;
                }
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PMIX_DATA_BUFFER_RELEASE(cmd);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
                return PMIX_OPERATION_SUCCEEDED;
            }
        }

        if (PMIX_CHECK_KEY(&directives[m], PMIX_JOB_CTRL_SIGNAL)) {
            PMIX_DATA_BUFFER_CREATE(cmd);
            cmmnd = PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
            /* pack the command */
            rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the target jobid */
            if (NULL == targets) {
                PMIX_LOAD_NSPACE(jobid, NULL);
            } else {
                proct = (pmix_proc_t *) &targets[0];
                PMIX_LOAD_NSPACE(jobid, proct->nspace);
            }
            rc = PMIx_Data_pack(NULL, cmd, &jobid, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the signal */
            PMIX_VALUE_GET_NUMBER(rc, &directives[m].value, signum, int32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            rc = PMIx_Data_pack(NULL, cmd, &signum, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, cmd))) {
                PRTE_ERROR_LOG(rc);
            }
            PMIX_DATA_BUFFER_RELEASE(cmd);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            return PMIX_OPERATION_SUCCEEDED;
        }

        if (PMIX_CHECK_KEY(&directives[m], PMIX_JOB_CTRL_DEFINE_PSET)) {
            // goes to all daemons
            PMIX_DATA_BUFFER_CREATE(cmd);
            cmmnd = PRTE_DAEMON_DEFINE_PSET;
            /* pack the command */
            rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            // pack the pset name
            rc = PMIx_Data_pack(NULL, cmd, (void*)&directives[m].value.data.string, 1, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            // pack the #targets
            rc = PMIx_Data_pack(NULL, cmd, &ntargets, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            // pack the targets
            rc = PMIx_Data_pack(NULL, cmd, (void*)targets, ntargets, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, cmd))) {
                PRTE_ERROR_LOG(rc);
            }
            PMIX_DATA_BUFFER_RELEASE(cmd);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            return PMIX_OPERATION_SUCCEEDED;
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}
