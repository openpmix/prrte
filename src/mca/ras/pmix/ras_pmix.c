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
    .scheduler_owned = true,
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
 * The caddy is built by modify() before the request is issued - see the note
 * there - and holds a reference on the request so the request cannot be
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

/* Fold the scheduler's answer into the request's info array.
 *
 * The answer is the operation's result, but it is not the whole of what the
 * completion needs.  prte_ras_base_complete_request routes the grow or the
 * teardown using directives that belong to the ORIGINAL request:
 * PMIX_ALLOC_ID and PMIX_ALLOC_REQ_ID name which reservation an EXTEND or a
 * RELEASE is for, and PMIX_ALLOC_TARGET / PMIX_ALLOC_SHARE /
 * PMIX_ALLOC_INHERITANCE decide which session a PMIX_ALLOC_NEW's nodes join
 * and what becomes of them afterwards.  A scheduler has no reason to echo any
 * of those - they are PRRTE-local routing, not anything it was asked to
 * decide - so replacing the array outright threw them away and then failed
 * the request locally after the scheduler had already committed the change:
 * an EXTEND with nothing left to name its target is refused
 * PMIX_ERR_BAD_PARAM, and a RELEASE by allocation id falls through to a node
 * list that is no longer there and is refused PMIX_ERR_NOT_FOUND.
 *
 * So merge.  Every key the answer carries wins - the granted node list
 * supersedes the requested one - and every key of the request's that the
 * answer does not mention is kept.  PMIX_REQUESTOR is dropped: modify() added
 * it for the scheduler's benefit and it is nobody's answer.
 *
 * The copy is deliberate.  The answer belongs to PMIx until cd->rel() is
 * invoked, and there is no good moment to invoke it if we point at it
 * instead: the requester is handed prte_pmix_server_req_release (so it never
 * calls cd->rel), and prte_ras_base_complete_request may repoint req->info at
 * a response of its own.  Copying makes the request the sole owner of
 * everything downstream - its destructor frees whatever req->info ends up
 * being - and lets us hand PMIx's array back immediately, where the lifetime
 * is obvious. */
static void merge_answer(prte_pmix_server_req_t *req,
                         pmix_info_t *ans, size_t nans)
{
    pmix_info_t *merged;
    bool *keep = NULL;
    size_t nkeep = 0, nmerged, idx, n, m;

    if (0 < req->ninfo && NULL != req->info) {
        keep = (bool *) malloc(req->ninfo * sizeof(bool));
        if (NULL == keep) {
            /* leave the request holding what it has - the original directives
             * are the ones the completion below cannot do without */
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return;
        }
        for (n = 0; n < req->ninfo; n++) {
            keep[n] = !PMIx_Check_key(req->info[n].key, PMIX_REQUESTOR);
            for (m = 0; keep[n] && m < nans; m++) {
                if (PMIx_Check_key(req->info[n].key, ans[m].key)) {
                    keep[n] = false;
                }
            }
            if (keep[n]) {
                ++nkeep;
            }
        }
    }

    nmerged = nkeep + ((NULL == ans) ? 0 : nans);
    if (0 == nmerged) {
        if (NULL != keep) {
            free(keep);
        }
        if (req->copy && NULL != req->info) {
            PMIX_INFO_FREE(req->info, req->ninfo);
        }
        req->info = NULL;
        req->ninfo = 0;
        req->copy = false;
        return;
    }

    PMIX_INFO_CREATE(merged, nmerged);
    if (NULL == merged) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        if (NULL != keep) {
            free(keep);
        }
        return;
    }
    idx = 0;
    for (n = 0; NULL != ans && n < nans; n++) {
        PMIX_INFO_XFER(&merged[idx++], &ans[n]);
    }
    for (n = 0; NULL != keep && n < req->ninfo; n++) {
        if (keep[n]) {
            PMIX_INFO_XFER(&merged[idx++], &req->info[n]);
        }
    }
    if (NULL != keep) {
        free(keep);
    }

    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->info = merged;
    req->ninfo = nmerged;
    req->copy = true;
}

/* Runs on the PRRTE progress thread: record the scheduler's answer on the
 * request, apply it, and relay it to whoever asked. */
static void passthru(int sd, short args, void *cbdata)
{
    prte_ras_pmix_caddy_t *cd = (prte_ras_pmix_caddy_t*)cbdata;
    prte_pmix_server_req_t *req = cd->req;
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

    merge_answer(req, cd->info, cd->ninfo);

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
    prte_ras_pmix_caddy_t *cd = (prte_ras_pmix_caddy_t*)cbdata;

    /* GOLDEN RULE: we are on the PMIx progress thread.  Capture the answer and
     * post it - touch no PRRTE state here, and allocate nothing.  The caddy
     * was built by modify() before the request went out precisely so that this
     * cannot fail: there is no way to report a failure from here, so an
     * allocation that came up short left the request sitting in the tracker
     * array and the client waiting on a callback that would never arrive. */
    cd->status = status;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->rel = rel;
    cd->relcbdata = relcbdata;

    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, passthru);
}

static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    prte_ras_pmix_caddy_t *cd;
    pmix_status_t rc;
    pmix_info_t *xfer;
    size_t n;

    // check if scheduler is attached and try to
    // attach if not
    rc = prte_pmix_set_scheduler();
    if (PMIX_SUCCESS != rc) {
        /* No scheduler is reachable, so we cannot forward this request.  Say
         * so, and say it accurately.
         *
         * This used to answer PMIX_ERR_TAKE_NEXT_OPTION so that ras/hosts,
         * further down the module list, could serve a grow or shrink locally
         * in a DVM with no scheduler.  There is no module list any more:
         * prte_ras_base_select() keeps exactly one module, and this component
         * is selected only when it has actually been pointed at a scheduler
         * (see ras_pmix_component_query).  So a schedulerless DVM never
         * reaches this function at all - ras/hosts owns the allocation and is
         * asked directly - and where we ARE the selected module there is
         * nothing to defer to.  All the old return bought was leaving
         * req->pstatus holding the PMIX_ERR_NOT_SUPPORTED that
         * prte_ras_base_modify seeds, telling the requester the operation is
         * unsupported when in truth it is supported and the scheduler is out
         * of touch - the difference between "give up" and "try again". */
        return PMIX_ERR_UNREACH;
    }

    // we need to pass the request on to the scheduler
    // need to add the requestor's ID to the info array
    PMIX_INFO_CREATE(xfer, req->ninfo + 1);
    if (NULL == xfer) {
        return PMIX_ERR_NOMEM;
    }
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

    /* Build the capture buffer for the answer BEFORE the request goes out.
     * The completion callback runs on the PMIx progress thread, where it may
     * do nothing but capture and post - and where it has no one to tell if it
     * cannot.  Allocated here, a failure is still one we can answer. */
    cd = PMIX_NEW(prte_ras_pmix_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    /* the caddy holds a reference on the request so the request cannot be
     * reclaimed out from under the shift */
    PMIX_RETAIN(req);
    cd->req = req;

    /* pass the request to the scheduler */
    rc = PMIx_Allocation_request_nb(req->allocdir, req->info, req->ninfo,
                                    infocbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* PMIx answers anything other than PMIX_SUCCESS without ever calling
         * back, so nothing will come to consume the caddy */
        PMIX_RELEASE(cd);
    }

    return rc;
}
