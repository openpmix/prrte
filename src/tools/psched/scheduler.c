/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/mca/base/pmix_mca_base_var.h"

#include "src/tools/psched/psched.h"

static int sched_base_verbose = -1;
void psched_scheduler_init(void)
{
    pmix_output_stream_t lds;

    pmix_mca_base_var_register("prte", "scheduler", "base", "verbose",
                               "Verbosity for debugging scheduler operations",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &sched_base_verbose);
    if (0 <= sched_base_verbose) {
        PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
        lds.lds_want_stdout = true;
        psched_globals.scheduler_output = pmix_output_open(&lds);
        PMIX_DESTRUCT(&lds);
        pmix_output_set_verbosity(psched_globals.scheduler_output, sched_base_verbose);
    }

    pmix_output_verbose(2, psched_globals.scheduler_output,
                        "%s scheduler:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
    return;
}

void psched_scheduler_finalize(void)
{
    return;
}

void psched_request_init(int fd, short args, void *cbdata)
{
    psched_req_t *req = (psched_req_t*)cbdata;
    size_t n;
    pmix_status_t rc, rcerr = PMIX_SUCCESS;
    bool notwaiting = false;

    pmix_output_verbose(2, psched_globals.output,
                        "%s scheduler:psched: init request",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    // process the incoming directives
    for (n=0; n < req->ndata; n++) {
        if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_REQ_ID)) {
            req->user_refid = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_ID)) {
            req->alloc_refid = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_SESSION_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &req->data[n].value, req->sessionID, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                // track the first error
                if (PMIX_SUCCESS == rcerr) {
                    rcerr = rc;
                }
            }
            // continue processing as we may need some of the info
            // when reporting back the error
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NUM_NODES)) {
            PMIX_VALUE_GET_NUMBER(rc, &req->data[n].value, req->num_nodes, uint64_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                // track the first error
                if (PMIX_SUCCESS == rcerr) {
                    rcerr = rc;
                }
                // continue processing as we may need some of the info
                // when reporting back the error
            }
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NODE_LIST)) {
            req->nlist = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_EXCLUDE)) {
            req->exclude = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NUM_CPUS)) {
            PMIX_VALUE_GET_NUMBER(rc, &req->data[n].value, req->num_cpus, uint64_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                // track the first error
                if (PMIX_SUCCESS == rcerr) {
                    rcerr = rc;
                }
                // continue processing as we may need some of the info
                // when reporting back the error
            }
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NUM_CPU_LIST)) {
            req->ncpulist = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_CPU_LIST)) {
            req->cpulist = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_MEM_SIZE)) {
            PMIX_VALUE_GET_NUMBER(rc, &req->data[n].value, req->memsize, float);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                // track the first error
                if (PMIX_SUCCESS == rcerr) {
                    rcerr = rc;
                }
                // continue processing as we may need some of the info
                // when reporting back the error
            }
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_TIME)) {
            req->time = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_QUEUE)) {
            req->queue = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_PREEMPTIBLE)) {
            req->preemptible = PMIx_Value_true(&req->data[n].value);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_LEND)) {
            req->lend = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_IMAGE)) {
            req->image = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_WAIT_ALL_NODES)) {
            req->waitall = PMIx_Value_true(&req->data[n].value);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_SHARE)) {
            req->share = PMIx_Value_true(&req->data[n].value);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NOSHELL)) {
            req->noshell = PMIx_Value_true(&req->data[n].value);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_DEPENDENCY)) {
            req->dependency = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_BEGIN)) {
            req->begintime = strdup(req->data[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&req->data[n], PMIX_ALLOC_NOT_WAITING)) {
            notwaiting = true;
        }
    }
    if (notwaiting) {
        // we callback with the current status so the requestor
        // can be told if we are accepting the request
        if (NULL != req->cbfunc) {
            req->cbfunc(rcerr, NULL, 0, req->cbdata, NULL, NULL);
        }
        if (PMIX_SUCCESS == rcerr) {
            // continue to next state
            PRTE_ACTIVATE_SCHED_STATE(req, PSCHED_STATE_QUEUE);
        } else {
            PMIX_RELEASE(req);
        }
    } else if (PMIX_SUCCESS == rcerr) {
        // move to next state
        PRTE_ACTIVATE_SCHED_STATE(req, PSCHED_STATE_QUEUE);
    } else {
        // need to reply to requestor so they don't hang
        if (NULL != req->cbfunc) {
            req->cbfunc(rcerr, NULL, 0, req->cbdata, NULL, NULL);
        }
        // cannot continue processing the request
        PMIX_RELEASE(req);
    }
    return;
}


void psched_request_queue(int fd, short args, void *cbdata)
{
    psched_req_t *req = (psched_req_t*)cbdata;

    pmix_output_verbose(2, psched_globals.output,
                        "%s scheduler:psched: queue request",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    // need to reply to requestor so they don't hang
    if (NULL != req->cbfunc) {
        req->cbfunc(PMIX_ERR_NOT_SUPPORTED, NULL, 0, req->cbdata, NULL, NULL);
    }
    // cannot continue processing the request
    PMIX_RELEASE(req);

}

void psched_session_complete(int fd, short args, void *cbdata)
{
    psched_req_t *req = (psched_req_t*)cbdata;

    pmix_output_verbose(2, psched_globals.output,
                        "%s scheduler:psched: session complete",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
}
