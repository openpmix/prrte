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

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

#include "src/grpcomm/grpcomm.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/plm.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/prted/pmix/pmix_server_internal.h"

/* process a job control request - runs on the PRRTE progress
 * thread, since it accesses proc objects and drives the PLM
 * and daemon-command xcasts */
static pmix_status_t process_job_ctrl(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[], size_t ndirs)
{
    pmix_status_t rc;
    int prc, j;
    int32_t signum, ntgts;
    size_t m, n;
    prte_proc_t *proc;
    pmix_nspace_t jobid;
    pmix_pointer_array_t parray, *ptrarray;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    pmix_proc_t *proct;
    PRTE_HIDE_UNUSED_PARAMS(requestor);

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
                        if (NULL == proc) {
                            continue;
                        }
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
            prc = prte_plm.terminate_procs(ptrarray);
            if (PRTE_SUCCESS != prc) {
                PRTE_ERROR_LOG(prc);
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
            if (PRTE_SUCCESS != prc) {
                return prte_pmix_convert_rc(prc);
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
                prc = prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, cmd);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                if (PRTE_SUCCESS != prc) {
                    PRTE_ERROR_LOG(prc);
                    return prte_pmix_convert_rc(prc);
                }
                return PMIX_OPERATION_SUCCEEDED;
            }
            /* Terminating a named set of procs is PMIX_JOB_CTRL_KILL's job
             * here; this directive only ever means the whole DVM, and with
             * targets there is nothing for it to do.  Say so rather than
             * falling through, which left the refusal conditional on what
             * else happened to be in the array - a request carrying both
             * this and a SIGNAL was answered by signalling the job and
             * reporting success. */
            return PMIX_ERR_NOT_SUPPORTED;
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
            rc = PMIx_Value_get_number(&directives[m].value, &signum, PMIX_INT32);
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
            prc = prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, cmd);
            PMIX_DATA_BUFFER_RELEASE(cmd);
            if (PRTE_SUCCESS != prc) {
                PRTE_ERROR_LOG(prc);
                return prte_pmix_convert_rc(prc);
            }
            return PMIX_OPERATION_SUCCEEDED;
        }

        if (PMIX_CHECK_KEY(&directives[m], PMIX_JOB_CTRL_DEFINE_PSET)) {
            /* The directive is the client's, straight off the wire, and
             * nothing between it and here checks what it holds. The name
             * is read out of the value union below, so a value of any
             * other type hands the packer whatever those eight bytes are
             * as a char* and it faults in strlen - one PMIx_Job_control
             * away, from any process attached to this daemon. A member
             * list is equally required: the receiving daemon sizes an
             * allocation from the count and PMIx refuses a set with no
             * members or no name, so either would leave every daemon in
             * the DVM logging an error while the requestor was told the
             * operation succeeded. */
            if (PMIX_STRING != directives[m].value.type ||
                NULL == directives[m].value.data.string ||
                NULL == targets || 0 == ntargets) {
                return PMIX_ERR_BAD_PARAM;
            }
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
            /* pack the #targets - the receiver unpacks an int32, so
             * narrow it here rather than handing PMIx the address of a
             * size_t and letting it read the wrong half on a big-endian
             * machine */
            ntgts = (int32_t) ntargets;
            rc = PMIx_Data_pack(NULL, cmd, &ntgts, 1, PMIX_INT32);
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
            prc = prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, cmd);
            PMIX_DATA_BUFFER_RELEASE(cmd);
            if (PRTE_SUCCESS != prc) {
                PRTE_ERROR_LOG(prc);
                return prte_pmix_convert_rc(prc);
            }
            return PMIX_OPERATION_SUCCEEDED;
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

/* release the one-element context-id result once it has been packed */
static void ctxid_relfn(void *cbdata)
{
    pmix_info_t *results = (pmix_info_t *) cbdata;

    PMIX_INFO_FREE(results, 1);
}

/* Answer a PMIX_GROUP_ASSIGN_CONTEXT_ID directive.
 *
 * This arrives as a job-control request because the group it is for is being
 * formed by PMIx_Group_invite, which runs no server collective - so unlike
 * PMIx_Group_construct there is no group up-call to carry the request, and
 * the PMIx library asks this way instead.
 *
 * Only the DVM master holds the id pool, and the leader of an invited group
 * is an application process that may sit anywhere, so this usually has to be
 * relayed. Nothing about the id depends on the directives, so the relay
 * carries none of them.
 *
 * On PMIX_SUCCESS this has taken over the caddy and answered (or will
 * answer) the request; the caller must do neither. Any other return means it
 * did neither and the caller still owns both. */
static pmix_status_t assign_group_ctxid(prte_pmix_server_op_caddy_t *cd)
{
    prte_pmix_server_req_t *req;
    pmix_info_t *results;
    size_t ctxid;
    int rc;

    if (PRTE_PROC_IS_MASTER) {
        rc = prte_grpcomm_assign_context_id(&ctxid);
        if (PRTE_SUCCESS != rc) {
            return prte_pmix_convert_rc(rc);
        }
        PMIX_INFO_CREATE(results, 1);
        if (NULL == results) {
            return PMIX_ERR_NOMEM;
        }
        PMIX_INFO_LOAD(&results[0], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
        /* the reply is packed after a thread shift, so hand the array over
         * with a release function rather than freeing it here */
        cd->infocbfunc(PMIX_SUCCESS, results, 1, cd->cbdata, ctxid_relfn, results);
        PMIX_RELEASE(cd);
        return PMIX_SUCCESS;
    }

    req = PMIX_NEW(prte_pmix_server_req_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    pmix_asprintf(&req->operation, "GROUPCTXID");
    PMIX_PROC_LOAD(&req->tproc, cd->proc.nspace, cd->proc.rank);
    req->infocbfunc = cd->infocbfunc;
    req->cbdata = cd->cbdata;
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
    rc = prte_server_send_request(PRTE_PMIX_GROUP_CTXID, req);
    if (PRTE_SUCCESS != rc) {
        /* nothing will answer it, so take it back off the tracker and let the
         * caller report the failure */
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                    req->local_index, NULL);
        PMIX_RELEASE(req);
        return prte_pmix_convert_rc(rc);
    }
    PMIX_RELEASE(cd);
    return PMIX_SUCCESS;
}

static void _job_ctrl(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_status_t rc;
    size_t n;
    bool ctxid_req = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* A context-id request is the one directive here that produces a result
     * rather than a status, and it may have to be relayed to the master, so
     * it is handled ahead of process_job_ctrl() - which can express neither.
     * It takes over the caddy when it succeeds. */
    for (n = 0; n < cd->ndirs; n++) {
        if (PMIX_CHECK_KEY(&cd->directives[n], PMIX_GROUP_ASSIGN_CONTEXT_ID) &&
            PMIX_INFO_TRUE(&cd->directives[n])) {
            ctxid_req = true;
            break;
        }
    }
    if (ctxid_req) {
        if (NULL == cd->infocbfunc) {
            /* there would be nowhere to put the answer */
            rc = PMIX_ERR_NOT_SUPPORTED;
        } else {
            rc = assign_group_ctxid(cd);
            if (PMIX_SUCCESS == rc) {
                return;
            }
        }
        if (NULL != cd->infocbfunc) {
            cd->infocbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
        }
        PMIX_RELEASE(cd);
        return;
    }

    rc = process_job_ctrl(&cd->proc, cd->procs, cd->nprocs, cd->directives, cd->ndirs);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    /* this upcall arrives on the PMIx progress thread - the request
     * requires access to proc objects, the PLM, and the daemon
     * command channel, so shift it to our progress thread. The
     * targets and directives arrays remain valid until we invoke
     * the callback, which happens at the end of the shifted
     * handler */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proc, requestor, sizeof(pmix_proc_t));
    cd->procs = (pmix_proc_t *) targets;
    cd->nprocs = ntargets;
    cd->directives = (pmix_info_t *) directives;
    cd->ndirs = ndirs;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _job_ctrl, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}
