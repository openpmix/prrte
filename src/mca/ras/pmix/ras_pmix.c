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

/* Caddy carrying a scheduler's answer from the PMIx progress thread over to
 * the PRRTE progress thread.
 *
 * PMIx invokes our completion callback on ITS progress thread, and the
 * top-level AGENTS.md golden rule is that such a callback must do nothing
 * beyond capturing its arguments and posting an event: a prte_pmix_server_req_t
 * is a PRRTE object, so recording the answer directly on it - let alone
 * freeing the array it was carrying - is a cross-thread write to state the
 * PRRTE progress thread owns. This caddy is the capture buffer that lets the
 * callback stay trivial; every mutation of the request happens in the
 * thread-shifted handler below.
 *
 * The caddy holds a reference on the request so the request cannot be
 * reclaimed out from under the shift. The answer's info array remains owned by
 * PMIx and stays valid until we invoke rel(relcbdata), which is why that pair
 * is carried across too. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_pmix_server_req_t *req;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
    pmix_release_cbfunc_t rel;
    void *relcbdata;
} prte_ras_pmix_caddy_t;

static void rpcon(prte_ras_pmix_caddy_t *p)
{
    p->req = NULL;
    p->status = PMIX_SUCCESS;
    p->info = NULL;
    p->ninfo = 0;
    p->rel = NULL;
    p->relcbdata = NULL;
}
static void rpdes(prte_ras_pmix_caddy_t *p)
{
    if (NULL != p->req) {
        PMIX_RELEASE(p->req);
    }
}
static PMIX_CLASS_INSTANCE(prte_ras_pmix_caddy_t, pmix_object_t, rpcon, rpdes);

/* Runs on the PRRTE progress thread: record the scheduler's answer on the
 * request, apply it, and relay it to whoever asked. */
static void passthru(int sd, short args, void *cbdata)
{
    prte_ras_pmix_caddy_t *cd = (prte_ras_pmix_caddy_t*)cbdata;
    prte_pmix_server_req_t *req = cd->req;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* The scheduler's verdict is the request's outcome. Record it in pstatus,
     * not just status: pstatus is what drives the completion below and what
     * the requester is told. (Recording only status left pstatus holding the
     * PMIX_ERR_NOT_SUPPORTED default that prte_ras_base_modify seeds, so a
     * granted allocation was never applied and the requester was told the
     * operation was unsupported.) */
    req->status = cd->status;
    req->pstatus = cd->status;

    /* The answer replaces whatever the request was carrying, so drop our own
     * copy first if we made one in modify() below.
     *
     * Take a COPY of the scheduler's array rather than pointing at it. The
     * array belongs to PMIx until cd->rel() is invoked, and there is no good
     * moment to invoke it if we keep it: the requester below is handed
     * prte_pmix_server_req_release (so it never calls cd->rel), and
     * prte_ras_base_complete_request may repoint req->info at a response of
     * its own (so we cannot simply forward it either). Copying makes the
     * request the sole owner of everything downstream - its destructor frees
     * whatever req->info ends up being - and lets us hand PMIx's array back
     * immediately, right here, where the lifetime is obvious. */
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->info = NULL;
    req->ninfo = 0;
    req->copy = false;
    if (0 < cd->ninfo && NULL != cd->info) {
        PMIX_INFO_CREATE(req->info, cd->ninfo);
        if (NULL != req->info) {
            for (n = 0; n < cd->ninfo; n++) {
                PMIX_INFO_XFER(&req->info[n], &cd->info[n]);
            }
            req->ninfo = cd->ninfo;
            req->copy = true;
        }
    }
    if (NULL != cd->rel) {
        cd->rel(cd->relcbdata);
        cd->rel = NULL;
    }
    /* nothing of PMIx's is left for anyone to release */
    req->rlcbfunc = NULL;
    req->rlcbdata = NULL;

    // if we met the request, then we need to process it
    if (PMIX_SUCCESS == req->pstatus) {
        prte_ras_base_complete_request(req);
    }

    if (NULL != req->infocbfunc) {
        /* Call the requestor's callback with the returned info, handing it
         * prte_pmix_server_req_release as the release function and returning
         * without releasing the request ourselves - this mirrors
         * prte_ras_base_modify's tail. The info the requester is looking at is
         * owned by the request, so dropping the request here would pull it out
         * from under a callback that has not finished with it. */
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata,
                        prte_pmix_server_req_release, req);
        PMIX_RELEASE(cd);
        return;
    }

    // nobody to relay to - cleanup our request
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
    PMIX_RELEASE(cd);
}

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_ras_pmix_caddy_t *cd;

    /* GOLDEN RULE: we are on the PMIx progress thread. Capture the answer and
     * post it - touch no PRRTE state here. */
    cd = PMIX_NEW(prte_ras_pmix_caddy_t);
    if (NULL == cd) {
        /* nothing we can do but hand the answer back so PMIx can reclaim it */
        if (NULL != rel) {
            rel(relcbdata);
        }
        return;
    }
    PMIX_RETAIN(req);
    cd->req = req;
    cd->status = status;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->rel = rel;
    cd->relcbdata = relcbdata;

    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, passthru);
}

static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    pmix_status_t rc;
    pmix_info_t *xfer;
    size_t n;

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
    /* Repoint the request at our augmented copy. If it merely borrowed the
     * caller's array we leave that alone, but if it already OWNED one we have
     * to release it here or repointing strands it - which is exactly what
     * happens to an allocation request relayed from a remote peer, since
     * pmix_server.c builds those with copy = true. */
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->copy = true;
    req->info = xfer;
    req->ninfo++;

    /* pass the request to the scheduler */
    rc = PMIx_Allocation_request_nb(req->allocdir, req->info, req->ninfo,
                                    infocbfunc, req);

    return rc;
}
