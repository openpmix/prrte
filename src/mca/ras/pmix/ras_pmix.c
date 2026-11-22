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

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/prte_show_help.h"
#include "ras_pmix.h"

/*
 * Local functions
 */
static int init(void);
/* Hand the scheduler's whereabouts to the code that will go looking for it.
 *
 * Every ras_pmix_* connection parameter is the name of a PMIx attach
 * attribute - ras_pmix_uri is PMIX_SERVER_URI, ras_pmix_server_pid is
 * PMIX_SERVER_PIDINFO, and so on down to the retry pair, which PMIx
 * implements itself.  None of them was ever handed to anything: the attach
 * in prte_pmix_set_scheduler() passed only PMIX_CONNECT_TO_SCHEDULER and
 * PMIX_TOOL_CONNECT_OPTIONAL, so PRRTE scanned for a rendezvous and took
 * whatever it found.  A site naming the scheduler it means - one of two on
 * the machine, or one whose rendezvous is not where PMIx looks by default -
 * got its parameter accepted, reported by prte_info, used to decide that this
 * component is the allocator (so no hostfile is read), and then ignored at
 * the one moment it meant anything.
 *
 * This runs at selection, which is the only place with both the parameters
 * and a guarantee of being ahead of every consumer: allocation, session
 * control and the tool-connection path all reach prte_pmix_set_scheduler(),
 * it looks exactly once, and whichever of them gets there first would
 * otherwise burn that one look with nothing to aim it.
 *
 * A parameter left at its default contributes nothing, so a DVM that has not
 * been told anything gets the same bare scan it always did. */
static int init(void)
{
    pmix_info_t *dirs;
    size_t ndirs = 0, n = 0;
    prte_ras_pmix_component_t *c = &prte_mca_ras_pmix_component;

    if (NULL != c->uri) {
        ++ndirs;
    }
    if (0 < strlen(c->server.nspace)) {
        ++ndirs;
    }
    if (0 < c->server_pid) {
        ++ndirs;
    }
    if (NULL != c->server_host) {
        ++ndirs;
    }
    if (NULL != c->connection_order) {
        ++ndirs;
    }
    /* the retry pair always goes: the parameters document defaults (5 and 1)
     * and a user who reads that is entitled to get them */
    ndirs += 2;

    PMIX_INFO_CREATE(dirs, ndirs);
    if (NULL == dirs) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (NULL != c->uri) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SERVER_URI, c->uri, PMIX_STRING);
    }
    if (0 < strlen(c->server.nspace)) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SERVER_NSPACE, c->server.nspace, PMIX_STRING);
    }
    if (0 < c->server_pid) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SERVER_PIDINFO, &c->server_pid, PMIX_PID);
    }
    if (NULL != c->server_host) {
        /* PMIX_SERVER_HOSTNAME is the attribute defined for exactly this -
         * the node the target server is on - and no PTL reads it yet, so it
         * is forwarded to the server rather than used to choose one.  Send it
         * anyway: it is the right attribute for what the parameter says, and
         * a PMIx that starts honoring it needs nothing here.
         *
         * ras_pmix_rank is NOT sent, and there is nothing to send it as.
         * PMIX_SERVER_RANK is a PMIx_server_init attribute - the rank a
         * server takes for ITSELF - not a way to name the server a tool wants,
         * and PMIx identifies an attach target by nspace and rendezvous.  Sent
         * here it would not select anything and would be forwarded to the
         * server as an assertion we have no business making.  See
         * docs/todo.rst. */
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SERVER_HOSTNAME, c->server_host, PMIX_STRING);
    }
    if (NULL != c->connection_order) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_CONNECTION_ORDER, c->connection_order, PMIX_STRING);
    }
    PMIX_INFO_LOAD(&dirs[n++], PMIX_CONNECT_MAX_RETRIES, &c->max_retries, PMIX_UINT32);
    PMIX_INFO_LOAD(&dirs[n++], PMIX_CONNECT_RETRY_DELAY, &c->retry_delay, PMIX_UINT32);

    prte_pmix_set_scheduler_directives(dirs, ndirs);
    return PRTE_SUCCESS;
}

static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static pmix_status_t modify(prte_pmix_server_req_t *req);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_pmix_module = {
    .scheduler_owned = true,
    .init = init,
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
    /* the copy of the request that went to the scheduler - see modify() */
    pmix_info_t *sent;
    size_t nsent;
} prte_ras_pmix_caddy_t;

static void rpcon(prte_ras_pmix_caddy_t *p)
{
    p->req = NULL;
    p->status = PMIX_SUCCESS;
    p->info = NULL;
    p->ninfo = 0;
    p->rel = NULL;
    p->relcbdata = NULL;
    p->sent = NULL;
    p->nsent = 0;
}
static void rpdes(prte_ras_pmix_caddy_t *p)
{
    if (NULL != p->req) {
        PMIX_RELEASE(p->req);
    }
    if (NULL != p->sent) {
        PMIX_INFO_FREE(p->sent, p->nsent);
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
 * answer does not mention is kept.  PMIX_REQUESTOR is dropped: it is what
 * modify() tells the scheduler, and nobody's answer.
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

/* Read PMIX_ALLOC_UID/GID from an info array. A value that is not a number
 * counts as absent: it says nothing we could compare. */
static void get_owner(const pmix_info_t *info, size_t ninfo,
                      uint32_t *uid, bool *have_uid, uint32_t *gid, bool *have_gid)
{
    size_t n;

    *have_uid = false;
    *have_gid = false;
    for (n = 0; NULL != info && n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_ALLOC_UID)) {
            *have_uid = (PMIX_SUCCESS == PMIx_Value_get_number(&info[n].value, uid, PMIX_UINT32));
        } else if (PMIx_Check_key(info[n].key, PMIX_ALLOC_GID)) {
            *have_gid = (PMIX_SUCCESS == PMIx_Value_get_number(&info[n].value, gid, PMIX_UINT32));
        }
    }
}

/* Returning a grant the answer's owner does not let us keep. The request
 * is ours, and so is its info array, until the scheduler answers. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_info_t *info;
    size_t ninfo;
    pmix_release_cbfunc_t rel;
    void *relcbdata;
} prte_ras_pmix_giveback_t;

static void gbcon(prte_ras_pmix_giveback_t *p)
{
    p->info = NULL;
    p->ninfo = 0;
    p->rel = NULL;
    p->relcbdata = NULL;
}
static void gbdes(prte_ras_pmix_giveback_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
static PMIX_CLASS_INSTANCE(prte_ras_pmix_giveback_t, pmix_object_t, gbcon, gbdes);

static void giveback_done(int sd, short args, void *cbdata)
{
    prte_ras_pmix_giveback_t *gb = (prte_ras_pmix_giveback_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(gb);
    if (NULL != gb->rel) {
        gb->rel(gb->relcbdata);
    }
    PMIX_RELEASE(gb);
}

static void giveback_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                        void *cbdata, pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_ras_pmix_giveback_t *gb = (prte_ras_pmix_giveback_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status, info, ninfo);

    /* GOLDEN RULE: on the PMIx progress thread - capture and post */
    gb->rel = rel;
    gb->relcbdata = relcbdata;
    PRTE_PMIX_THREADSHIFT(gb, prte_event_base, giveback_done);
}

/* Ask the scheduler to take back what an answer granted: the allocation it
 * named for a PMIX_ALLOC_NEW, and the nodes it added for a
 * PMIX_ALLOC_EXTEND - releasing the whole allocation there would take back
 * what was held before the request. */
static void giveback(pmix_alloc_directive_t dir, const pmix_info_t *ans, size_t nans)
{
    prte_ras_pmix_giveback_t *gb;
    const pmix_info_t *id = NULL, *nodes = NULL;
    size_t n, k = 0;
    pmix_status_t rc;

    for (n = 0; NULL != ans && n < nans; n++) {
        if (PMIx_Check_key(ans[n].key, PMIX_ALLOC_ID)) {
            id = &ans[n];
        } else if (PMIx_Check_key(ans[n].key, PMIX_ALLOC_NODE_LIST)) {
            nodes = &ans[n];
        }
    }
    if (NULL == id || (PMIX_ALLOC_EXTEND == dir && NULL == nodes)) {
        pmix_output(0, "%s ras:pmix: the scheduler's answer does not name what it "
                       "granted, so it could not be returned",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        return;
    }
    gb = PMIX_NEW(prte_ras_pmix_giveback_t);
    if (NULL == gb) {
        return;
    }
    gb->ninfo = (PMIX_ALLOC_EXTEND == dir) ? 2 : 1;
    PMIX_INFO_CREATE(gb->info, gb->ninfo);
    if (NULL == gb->info) {
        gb->ninfo = 0;
        PMIX_RELEASE(gb);
        return;
    }
    PMIX_INFO_XFER(&gb->info[k++], id);
    if (PMIX_ALLOC_EXTEND == dir) {
        PMIX_INFO_XFER(&gb->info[k++], nodes);
    }
    rc = PMIx_Allocation_request_nb(PMIX_ALLOC_RELEASE, gb->info, gb->ninfo,
                                    giveback_cb, gb);
    if (PMIX_SUCCESS != rc) {
        /* answered without a callback - nothing will consume it */
        pmix_output(0, "%s ras:pmix: returning a granted allocation failed: %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(rc));
        PMIX_RELEASE(gb);
    }
}

/* The scheduler was told whom the allocation is for (PMIX_ALLOC_UID/GID,
 * settled by prte_ras_base_alloc_owner and sent in req->info), and its
 * answer must say the same. Anything else is the scheduler's error: the
 * request fails and the grant is returned. Checked against the answer
 * alone - merge_answer would fill the keys in from the request. */
static bool owner_answered(prte_pmix_server_req_t *req, const pmix_info_t *ans, size_t nans)
{
    uint32_t suid = 0, sgid = 0, auid = 0, agid = 0;
    bool shave_uid, shave_gid, ahave_uid, ahave_gid;
    char answered[64];

    if (PMIX_ALLOC_NEW != req->allocdir && PMIX_ALLOC_EXTEND != req->allocdir) {
        return true;
    }
    get_owner(req->info, req->ninfo, &suid, &shave_uid, &sgid, &shave_gid);
    if (!shave_uid || !shave_gid) {
        /* we asked for no one - a --add-host request, which allocates
         * nothing (prte_ras_base_alloc_owner) */
        return true;
    }
    get_owner(ans, nans, &auid, &ahave_uid, &agid, &ahave_gid);
    if (ahave_uid && ahave_gid && auid == suid && agid == sgid) {
        return true;
    }
    if (ahave_uid && ahave_gid) {
        snprintf(answered, sizeof(answered), "uid %u, gid %u", auid, agid);
    } else {
        snprintf(answered, sizeof(answered), "no user and group");
    }
    prte_show_help_proc(&req->tproc, "help-ras-base.txt",
                        "ras-base:alloc-owner-mismatch", true,
                        PMIx_Alloc_directive_string(req->allocdir), suid, sgid, answered);
    return false;
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

    if (PMIX_SUCCESS == req->pstatus && !owner_answered(req, cd->info, cd->ninfo)) {
        giveback(req->allocdir, cd->info, cd->ninfo);
        req->status = PMIX_ERROR;
        req->pstatus = PMIX_ERROR;
    }

    merge_answer(req, cd->info, cd->ninfo);

    if (NULL != cd->rel) {
        cd->rel(cd->relcbdata);
        cd->rel = NULL;
        cd->relcbdata = NULL;
    }
    /* the answer is PMIx's again - leave nothing here that looks readable */
    cd->info = NULL;
    cd->ninfo = 0;
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
    PMIX_INFO_CREATE(xfer, req->ninfo + 1 + PRTE_PMIX_ORIGIN_MAX);
    if (NULL == xfer) {
        return PMIX_ERR_NOMEM;
    }
    /* the request is the requester's, not ours - say whom we act for */
    n = prte_pmix_server_relay_copy(xfer, req->info, req->ninfo);
    PMIX_INFO_LOAD(&xfer[n], PMIX_REQUESTOR, &req->tproc, PMIX_PROC);

    /* The copy is what the scheduler is sent, and nothing else. The request
     * keeps its own info: once the scheduler answers, the request is
     * completed here (prte_ras_base_complete_request), and that is decided
     * on the request as PMIx handed it to us. The copy renames the
     * requester's PMIX_USERID and PMIX_GRPID to their PMIX_ORIGIN_ forms,
     * since to the scheduler we are the requester - so a request pointed
     * at it carries no PMIX_USERID, and a release by allocation id, which
     * checks that the requester may operate on the reservation, was
     * refused PMIX_ERR_NO_PERMISSIONS after the scheduler had already
     * released it. The caddy owns the copy, and frees it with itself. */

    /* Build the capture buffer for the answer BEFORE the request goes out.
     * The completion callback runs on the PMIx progress thread, where it may
     * do nothing but capture and post - and where it has no one to tell if it
     * cannot.  Allocated here, a failure is still one we can answer. */
    cd = PMIX_NEW(prte_ras_pmix_caddy_t);
    if (NULL == cd) {
        PMIX_INFO_FREE(xfer, n + 1);
        return PMIX_ERR_NOMEM;
    }
    /* the caddy holds a reference on the request so the request cannot be
     * reclaimed out from under the shift */
    PMIX_RETAIN(req);
    cd->req = req;
    cd->sent = xfer;
    cd->nsent = n + 1;

    /* pass the request to the scheduler */
    rc = PMIx_Allocation_request_nb(req->allocdir, cd->sent, cd->nsent,
                                    infocbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* PMIx answers anything other than PMIX_SUCCESS without ever calling
         * back, so nothing will come to consume the caddy */
        PMIX_RELEASE(cd);
    }

    return rc;
}
