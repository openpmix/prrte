/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_list.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "ras_pmix.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static pmix_status_t modify(prte_pmix_server_req_t *req);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_pmix_module = {
    .init = NULL,
    .allocate = allocate,
    .modify = modify,
    .finalize = finalize
};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata, nodes);


    return PRTE_ERR_TAKE_NEXT_OPTION;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}

/* Callbacks to process an allocate request answer from the scheduler
 * and pass on any results to the requesting client
 */
static void passthru(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    // if we met the request, then we need to process it
    if (PMIX_SUCCESS == req->pstatus) {
        prte_ras_base_complete_request(req);
    }

    if (NULL != req->infocbfunc) {
        // call the requestor's callback with the returned info
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata, req->rlcbfunc, req->rlcbdata);
    } else if (NULL != req->rlcbfunc) {
        // let them cleanup
        req->rlcbfunc(req->rlcbdata);
    }
    // cleanup our request
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);


    PMIX_RELEASE(req);
}

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;

    // need to pass this into our progress thread for processing
    // since we touch the global request array
    req->status = status;
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
        req->copy = false;
    }
    req->info = info;
    req->ninfo = ninfo;
    req->rlcbfunc = rel;
    req->rlcbdata = relcbdata;

    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, passthru, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
}

static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    pmix_status_t rc;
    pmix_info_t *xfer;
    size_t n;

    if (prte_mca_ras_pmix_component.simulate) {
        // pretend we are attached to a scheduler
        xfer = PMIx_Info_create(1);
        PMIX_INFO_LOAD(xfer, PMIX_ALLOC_NODE_LIST, prte_mca_ras_pmix_component.simulate_nodelist, PMIX_STRING);
        req->pstatus = PMIX_SUCCESS;
        req->info = xfer;
        req->ninfo = 1;
        prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, passthru, req);
        PMIX_POST_OBJECT(req);
        prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
        return PMIX_SUCCESS;
    }

    // check if scheduler is attached and try to
    // attach if not
    rc = prte_pmix_set_scheduler();
    if (PMIX_SUCCESS != rc) {
        /* No scheduler is reachable, so we cannot forward this request.  Defer
         * to the next RAS module rather than failing the whole request: in a
         * schedulerless DVM the ras/hosts component handles node-list grow and
         * shrink locally.  Returning a hard error here (e.g. PMIX_ERR_UNREACH)
         * would instead abort the modify loop before hosts is consulted. */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    // we need to pass the request on to the scheduler
    // need to add the requestor's ID to the info array
    PMIX_INFO_CREATE(xfer, req->ninfo + 1);
    for (n=0; n < req->ninfo; n++) {
        PMIX_INFO_XFER(&xfer[n], &req->info[n]);
    }
    PMIX_INFO_LOAD(&xfer[req->ninfo], PMIX_REQUESTOR, &req->tproc, PMIX_PROC);
    // the current req object points to the caller's info array, so leave it alone
    req->copy = true;
    req->info = xfer;
    req->ninfo++;

    /* pass the request to the scheduler */
    rc = PMIx_Allocation_request_nb(req->allocdir, req->info, req->ninfo,
                                    infocbfunc, req);

    return rc;
}
