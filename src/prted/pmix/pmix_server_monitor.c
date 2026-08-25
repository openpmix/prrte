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
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/prted/pmix/pmix_server_internal.h"

/* if this request is coming up to us, then we have to pass the request to the HNP so it
 * can xcast it to all daemons in the DVM. When a daemon receives a request, it has to
 * pass the request down to its PMIx server for execution, then return the results
 * back to the HNP. Upon completion of the collective, the HNP must send the result
 * to the daemon that hosts the requestor so that daemon can relay the results back
 * down to the requestor.
 */


/* Tell the requesting daemon that we cannot serve its monitor request.
 *
 * The collective counts responses, so a daemon that goes quiet is a daemon
 * the requestor waits on for the life of the DVM - which makes a reply the
 * minimum we owe it even when we have nothing to say.  This packs the three
 * fields pmix_server_monitor_resp() reads before it looks at the status:
 * our vpid, the room number in the requestor's own tracker, and why. */
static void send_monitor_error(pmix_rank_t dvpid, int remote_index, pmix_status_t st)
{
    pmix_data_buffer_t *msg;
    pmix_status_t rc;
    int ret;

    PMIX_DATA_BUFFER_CREATE(msg);

    rc = PMIx_Data_pack(NULL, msg, &prte_process_info.myproc.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    rc = PMIx_Data_pack(NULL, msg, &remote_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    rc = PMIx_Data_pack(NULL, msg, &st, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }

    PRTE_RML_SEND(ret, dvpid, msg, PRTE_RML_TAG_MONITOR_RESP);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
    }
}

/* The first thing to go wrong is the useful one to report, so a later
 * response must not overwrite it.  (pstatus is clear of the caller's own
 * monitor code by the time any response arrives - see mfn.) */
#define RECORD_ERROR(r, st)                 \
    do {                                    \
        if (PMIX_SUCCESS == (r)->pstatus) { \
            (r)->pstatus = (st);            \
        }                                   \
    } while (0)

/* Run the completion test for a fan-out that may just have become whole.
 *
 * Whole means every daemon we were waiting on has been accounted for - by
 * answering, or by dying.  The status says which of those it was, because a
 * caller handed a sample of half the DVM has to be able to tell.  May clear
 * the request's slot and release it, so the caller must not touch it after. */
static void monitor_check_complete(prte_pmix_server_req_t *req)
{
    pmix_status_t st;

    if (req->ndaemons != req->nreported) {
        return;
    }

    if (req->nsuccess == req->ndaemons) {
        st = PMIX_SUCCESS;
    } else if (0 < req->nsuccess) {
        /* some daemons answered and some could not - the results are a
         * sample of part of the DVM, which is what this status is for */
        st = PMIX_ERR_PARTIAL_SUCCESS;
    } else {
        /* nothing came back at all - report why, not that it was partial */
        st = (PMIX_SUCCESS == req->pstatus) ? PMIX_ERROR : req->pstatus;
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s monitor request complete: %u/%u answered, status %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        req->nsuccess, req->ndaemons, PMIx_Error_string(st));

    if (NULL != req->infocbfunc) {
        req->infocbfunc(st, req->info, req->ninfo, req->cbdata,
                        prte_pmix_server_req_release, req);
    } else {
        // nothing we can do!
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                    req->local_index, NULL);
        PMIX_RELEASE(req);
    }
}

/* Account a daemon departure against every monitor collective in flight.
 *
 * A monitor request fans out with an xcast and then counts direct replies,
 * so nothing in it is keyed on the routing tree and nothing repairs it when
 * the tree changes: a daemon that dies mid-collective simply never answers,
 * and the requestor counts for the life of the DVM.  Unlike a fence, there
 * is nothing to re-drive here - a monitor reply is a sample of live state on
 * a node that no longer exists - so recovery is to stop waiting for it and
 * tell the caller its sample is short.
 *
 * The routing tree calls this twice for the same death, at LOCAL scope and
 * again at GLOBAL, and a rank may be named again later by an adoption notice
 * to a new parent.  Marking the departure in reported_dmns is what makes all
 * of that idempotent; screening against expected_dmns is what keeps a death
 * this request never waited on from counting at all. */
void prte_pmix_server_fault_handler(const prte_rml_recovery_status_t *status)
{
    prte_pmix_server_req_t *req;
    pmix_rank_t *failed;
    size_t i;
    int n, bit;
    bool lost;

    if (0 == status->failed_ranks.size || NULL == status->failed_ranks.array) {
        /* a revival, or a reshape with no deaths in it */
        return;
    }
    failed = (pmix_rank_t *) status->failed_ranks.array;

    for (n = 0; n < prte_pmix_server_globals.local_reqs.size; n++) {
        req = (prte_pmix_server_req_t *)
            pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, n);
        if (NULL == req || NULL == req->monitor || 0 == req->ndaemons) {
            /* not a monitor collective, or one that never fanned out */
            continue;
        }
        lost = false;
        for (i = 0; i < status->failed_ranks.size; i++) {
            bit = (int) failed[i];
            if (!pmix_bitmap_is_set_bit(&req->expected_dmns, bit) ||
                pmix_bitmap_is_set_bit(&req->reported_dmns, bit)) {
                continue;
            }
            pmix_bitmap_set_bit(&req->reported_dmns, bit);
            ++req->nreported;
            lost = true;
        }
        if (!lost) {
            continue;
        }
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s monitor request lost a daemon: %u/%u now in",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            req->nreported, req->ndaemons);
        if (PMIX_SUCCESS == req->pstatus) {
            req->pstatus = PMIX_ERR_UNREACH;
        }
        /* may clear slot n and release req - we do not touch it again */
        monitor_check_complete(req);
    }
}

static void mfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    pmix_data_buffer_t msg;
    pmix_status_t rc;
    pmix_rank_t r;
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* Record WHICH daemons must respond, not merely how many.  The xcast
     * below reaches every daemon the routing tree still considers live, so
     * that same predicate - a vpid inside the span that is not marked
     * failed - is exactly the set that will answer.  This has to be read
     * here, on the progress thread, because the DVM can grow or shrink.
     *
     * The identities are what let a later death be accounted for: a bare
     * count cannot say whether the daemon that just died had already
     * reported, and both readings of that are wrong. */
    req->ndaemons = 0;
    for (r = 0; r < prte_rml_base.n_dmns; r++) {
        if (r == prte_process_info.myproc.rank ||
            pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, (int) r)) {
            continue;
        }
        if (PMIX_SUCCESS != pmix_bitmap_set_bit(&req->expected_dmns, (int) r)) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            rc = PMIX_ERR_NOMEM;
            goto errorout;
        }
        ++req->ndaemons;
    }

    /* Every daemon but us answers, and the completion test lives in the
     * response handler - so on a DVM of one there is nothing to xcast to
     * and nothing that will ever run that test.  PMIx has already gathered
     * this node's own contribution before up-calling (it only asks the host
     * about participation it judges remote), so an empty success is the
     * whole of our answer and the caller gets its local results. */
    if (0 == req->ndaemons) {
        if (NULL != req->infocbfunc) {
            req->infocbfunc(PMIX_SUCCESS, NULL, 0, req->cbdata, NULL, NULL);
        }
        PMIX_RELEASE(req);
        return;
    }

    // cache the request
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    // create the request
    PMIX_DATA_BUFFER_CONSTRUCT(&msg);

    // pack my vpid
    rc = PMIx_Data_pack(NULL, &msg, &prte_process_info.myproc.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }

    // pack the room number where this is being cached
    rc = PMIx_Data_pack(NULL, &msg, &req->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }

    // pack the requestor
    rc = PMIx_Data_pack(NULL, &msg, &req->target, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }

    // pack the monitor
    rc = PMIx_Data_pack(NULL, &msg, req->monitor, 1, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }

    // pack the event update status
    rc = PMIx_Data_pack(NULL, &msg, &req->pstatus, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }
    /* that was the caller's monitor code, and it is now on the wire.  The
     * field is reused from here on to accumulate the collective's own
     * result - the first non-success a daemon reports, or the reason a
     * daemon will never report at all - so clear the caller's value out of
     * it rather than letting an ordinary monitor code read as a failure. */
    req->pstatus = PMIX_SUCCESS;

    // pack the directives, if given
    rc = PMIx_Data_pack(NULL, &msg, &req->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        goto errorout;
    }
    if (0 < req->ndirs) {
        rc = PMIx_Data_pack(NULL, &msg, req->directives, req->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&msg);
            goto errorout;
        }
    }

    // xcast this to all daemons
    ret = prte_grpcomm_xcast(PRTE_RML_TAG_MONITOR_REQUEST, &msg);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        rc = prte_pmix_convert_rc(ret);
        goto errorout;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&msg);
    return;

errorout:
    // need to alert the PMIx server so nothing hangs
    if (NULL != req->infocbfunc) {
        req->infocbfunc(rc, NULL, 0, req->cbdata, prte_pmix_server_req_release, req);
    } else {
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_RELEASE(req);
    }
}

pmix_status_t pmix_server_monitor_fn(const pmix_proc_t *requestor,
                                     const pmix_info_t *monitor, pmix_status_t error,
                                     const pmix_info_t directives[], size_t ndirs,
                                     pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;
    PRTE_HIDE_UNUSED_PARAMS(error);

    // protection
    if (NULL == requestor || NULL == monitor) {
        return PMIX_ERR_BAD_PARAM;
    }

    // create a tracking object
    req = PMIX_NEW(prte_pmix_server_req_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(&req->target, requestor, sizeof(pmix_proc_t));
    req->monitor = (pmix_info_t*)monitor;
    req->pstatus = error;
    req->directives = (pmix_info_t*)directives;
    req->ndirs = ndirs;
    req->infocbfunc = cbfunc;
    req->cbdata = cbdata;

    // need to threadshift this to our event base
    prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, mfn, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void mycbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *rq2 = (prte_pmix_server_req_t*)cbdata;
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)rq2->cbdata;
    pmix_data_buffer_t *msg;
    pmix_status_t rc;
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    rc = PMIX_SUCCESS;
    PMIX_DATA_BUFFER_CREATE(msg);

    // pack my vpid
    rc = PMIx_Data_pack(NULL, msg, &prte_process_info.myproc.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        goto errorout;
    }

    // pack the remote room number where this was cached
    rc = PMIx_Data_pack(NULL, msg, &req->remote_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        goto errorout;
    }

    // pack the returned status
    rc = PMIx_Data_pack(NULL, msg, &rq2->pstatus, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(msg);
        goto errorout;
    }

    /* if it failed, then nothing more to pack.  Gate on the same field we
     * packed above and the receiver reads: status is a PRRTE code that mycb
     * never sets, so this used to be permanently true and would ship results
     * the far end had already decided not to read. */
    if (PMIX_SUCCESS == rq2->pstatus) {
        // pack any returned info
        rc = PMIx_Data_pack(NULL, msg, &rq2->ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(msg);
            goto errorout;
        }
        if (0 < rq2->ninfo) {
            rc = PMIx_Data_pack(NULL, msg, rq2->info, rq2->ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(msg);
                goto errorout;
            }
        }
    }

    // send it to the requesting daemon
    PRTE_RML_SEND(ret, req->proxy.rank, msg, PRTE_RML_TAG_MONITOR_RESP);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        rc = prte_pmix_convert_rc(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        goto errorout;
    }

errorout:
    /* We built no reply, so the requestor is still counting and would never
     * reach its total.  Send it the bare refusal rather than going quiet. */
    if (PMIX_SUCCESS != rc) {
        send_monitor_error(req->proxy.rank, req->remote_index, rc);
    }

    // execute the release callback
    if (NULL != rq2->rlcbfunc) {
        rq2->rlcbfunc(rq2->rlcbdata);
    }

    /* local_index is OUR room in remote_reqs; remote_index is the
     * requestor's room in its own local_reqs and means nothing on this
     * array.  Clearing that one left this request's slot pointing at the
     * object we are about to free - which prte_pmix_server_clear() then
     * walks - and unlinked whichever unrelated peer request happened to
     * hold the slot it named. */
    pmix_pointer_array_set_item(&prte_pmix_server_globals.remote_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
    PMIX_RELEASE(rq2);
}

static void mycb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                 pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    prte_pmix_server_req_t *rq2;

    // need to threadshift this into our progress thread
    rq2 = PMIX_NEW(prte_pmix_server_req_t);
    rq2->pstatus = status;
    rq2->info = info;
    rq2->ninfo = ninfo;
    rq2->cbdata = cbdata;
    rq2->rlcbfunc = release_fn;
    rq2->rlcbdata = release_cbdata;

    prte_event_set(prte_event_base, &(rq2->ev), -1, PRTE_EV_WRITE, mycbfn, rq2);
    PMIX_POST_OBJECT(rq2);
    prte_event_active(&(rq2->ev), PRTE_EV_WRITE, 1);
}

void pmix_server_monitor_request(int status, pmix_proc_t *sender,
                                 pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                 void *cbdata)
{
    pmix_status_t rc;
    pmix_rank_t dvpid;
    int32_t cnt;
    int remote_index;
    pmix_status_t event;
    pmix_info_t *monitor;
    size_t ndirs;
    pmix_info_t *directives = NULL;
    prte_pmix_server_req_t *req;
    pmix_proc_t requestor;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    // unpack the requesting daemon's vpid
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &dvpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    // if it is my own request, then we can ignore it
    if (dvpid == prte_process_info.myproc.rank) {
        return;
    }

    // unpack the remote room number
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &remote_index, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    // unpack the requestor
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    // unpack the monitor
    monitor = PMIx_Info_create(1);
    if (NULL == monitor) {
        rc = PMIX_ERR_NOMEM;
        goto errorout;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, monitor, &cnt, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Info_free(monitor, 1);
        goto errorout;
    }

    // unpack the event update status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &event, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Info_free(monitor, 1);
        goto errorout;
    }

    // unpack the directives, if given
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Info_free(monitor, 1);
        goto errorout;
    }
    // we need two extra locations for our own directives
    PMIX_INFO_CREATE(directives, ndirs+2);
    if (NULL == directives) {
        PMIx_Info_free(monitor, 1);
        rc = PMIX_ERR_NOMEM;
        goto errorout;
    }
    if (0 < ndirs) {
        cnt = ndirs;
        rc = PMIx_Data_unpack(NULL, buffer, directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Info_free(monitor, 1);
            /* ndirs+2 was allocated - the two spare slots are ours */
            PMIx_Info_free(directives, ndirs + 2);
            goto errorout;
        }
    }
    // add the "local only" directive so the call to the monitoring
    // API doesn't loop back to us
    PMIX_INFO_LOAD(&directives[ndirs], PMIX_MONITOR_LOCAL_ONLY, NULL, PMIX_BOOL);
    ++ndirs;
    // add the proxy directive so the server knows who is actually requesting it
    PMIX_INFO_LOAD(&directives[ndirs], PMIX_MONITOR_PROXY, &requestor, PMIX_PROC);
    ++ndirs;

    // cache this request
    req = PMIX_NEW(prte_pmix_server_req_t);
    if (NULL == req) {
        PMIx_Info_free(monitor, 1);
        PMIx_Info_free(directives, ndirs);
        rc = PMIX_ERR_NOMEM;
        goto errorout;
    }
    PMIx_Load_procid(&req->proxy, prte_process_info.myproc.nspace, dvpid);
    req->monitor = monitor;
    req->moncopy = true;
    req->remote_index = remote_index;
    req->pstatus = event;
    req->directives = directives;
    req->ndirs = ndirs;
    req->dircopy = true;
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.remote_reqs, req);

    // pass this down - the monitoring code will return no results
    // if we are not included in the targets
    rc = PMIx_Process_monitor_nb(monitor, event, directives, ndirs, mycb, req);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.remote_reqs, req->local_index, NULL);
        PMIX_RELEASE(req);
        goto errorout;
    }
    return;

errorout:
    // cannot allow the collective to hang
    send_monitor_error(dvpid, remote_index, rc);
}

void pmix_server_monitor_resp(int status, pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc, rstatus;
    pmix_rank_t dvpid;
    int local_index;
    prte_pmix_server_req_t *req;
    pmix_info_t *info=NULL, *results;
    size_t ninfo=0, sz, m, n;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    // unpack the daemon that sent this to us
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &dvpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    // unpack our room number
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &local_index, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    // lookup the request
    req = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, local_index);
    if (NULL == req) {
        // bad index, or we no longer have this request
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    /* The index came off the wire, and the slot it names is handed out
     * again the moment its previous occupant retires - so a response that
     * crossed with a retirement can land on a live request of some other
     * kind.  Accounting a report against one of those, and merging monitor
     * results into an info array it did not allocate, corrupts a request
     * somebody else is waiting on.  monitor is set by this file and nowhere
     * else, which makes it the discriminator. */
    if (NULL == req->monitor) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s monitor response for index %d found a %s request - dropping it",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), local_index,
                            (NULL == req->operation) ? "non-monitor" : req->operation);
        return;
    }

    /* Record WHICH daemon reported.  A bare count cannot tell a second copy
     * of one daemon's response from the first, and counting one twice
     * completes the request early - before the daemon whose slot it took is
     * heard from.  Screening against the expected set also drops a response
     * from a daemon this request was never waiting on. */
    if (!pmix_bitmap_is_set_bit(&req->expected_dmns, (int) dvpid)) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s monitor response from unexpected daemon %s - dropping it",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_VPID_PRINT(dvpid));
        return;
    }
    if (pmix_bitmap_is_set_bit(&req->reported_dmns, (int) dvpid)) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s duplicate monitor response from daemon %s - dropping it",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_VPID_PRINT(dvpid));
        return;
    }
    pmix_bitmap_set_bit(&req->reported_dmns, (int) dvpid);
    ++req->nreported;

    /* Note that every failure below records the error on the request and
     * then falls through to the completion check.  Returning early would
     * leave nreported already incremented for this daemon, so a malformed
     * response from the LAST daemon to report would mean the request never
     * completes and the requestor hangs forever. */

    // unpack the returned status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &rstatus, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        RECORD_ERROR(req, rc);
        goto complete;
    }
    if (PMIX_SUCCESS == rstatus) {
        ++req->nsuccess;
    } else {
        RECORD_ERROR(req, rstatus);
    }

    // if it succeeded, then unpack the results
    if (PMIX_SUCCESS == rstatus) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            RECORD_ERROR(req, rc);
            goto complete;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            cnt = ninfo;
            rc = PMIx_Data_unpack(NULL, buffer, info, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                RECORD_ERROR(req, rc);
                goto complete;
            }
        }
        // add these to the collected results
        if (0 == req->ninfo) {
            req->info = info;
            req->ninfo = ninfo;
        } else {
            // extend the array
            sz = req->ninfo + ninfo;
            PMIX_INFO_CREATE(results, sz);
            m = 0;
            for (n=0; n < req->ninfo; n++) {
                PMIX_INFO_XFER(&results[m], &req->info[n]);
                ++m;
            }
            for (n=0; n < ninfo; n++) {
                PMIX_INFO_XFER(&results[m], &info[n]);
                ++m;
            }
            PMIX_INFO_FREE(req->info, req->ninfo);
            /* PMIX_INFO_XFER deep-copies, so the array we just unpacked
             * still owns its entries - release them */
            PMIX_INFO_FREE(info, ninfo);
            req->info = results;
            req->ninfo = sz;
        }
        req->copy = true;
    }

complete:
    // if everyone we were waiting on is accounted for, then we are complete
    monitor_check_complete(req);
}
