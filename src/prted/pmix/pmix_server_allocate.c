/*
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/ras/base/base.h"

static void localrelease(void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

void pmix_server_alloc_request_resp(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tg,
                                    void *cbdata)
{

    int req_index, cnt;
    pmix_status_t ret, rc;
    prte_pmix_server_req_t *req;

    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
    }

    /* we let the above errors fall thru in the vain hope that the req number can
     * be successfully unpacked, thus allowing us to respond to the requestor */

    /* unpack our tracking room number */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &req_index, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    req = pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, req_index);
    if (NULL == req) {
        // nothing we can do
        return;
    }

    /* Report the error */
    if (ret != PMIX_SUCCESS) {
        goto ANSWER;
    }

    rc = PMIx_Data_unpack(NULL, buffer, &req->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
        goto ANSWER;
    }

    if (0 < req->ninfo) {
        PMIX_INFO_CREATE(req->info, req->ninfo);

        cnt = req->ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, req->info, &cnt, PMIX_INFO);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            ret = prte_pmix_convert_rc(rc);
            req->ninfo = 0;
            goto ANSWER;
        }
    }

ANSWER:
    if (NULL != req->infocbfunc) {
        // pass the response back to the requestor
        req->infocbfunc(ret, req->info, req->ninfo, req, localrelease, req);
    } else {
        PMIX_RELEASE(req);
    }
}

pmix_status_t prte_pmix_set_scheduler(void)
{
    pmix_status_t rc;
    pmix_info_t info[2];

    if (!prte_pmix_server_globals.scheduler_connected) {
        /* the scheduler has not attached to us - see if we
         * can attach to it, make it optional so we don't
         * hang if there is no scheduler available */
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SCHEDULER, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[1], PMIX_TOOL_CONNECT_OPTIONAL, NULL, PMIX_BOOL);
        rc = PMIx_tool_attach_to_server(NULL, &prte_pmix_server_globals.scheduler,
                                        info, 2);
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        prte_pmix_server_globals.scheduler_set_as_server = true;
    }

    /* if we have not yet set the scheduler as our server, do so */
    if (!prte_pmix_server_globals.scheduler_set_as_server) {
        rc = PMIx_tool_set_server(&prte_pmix_server_globals.scheduler, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        prte_pmix_server_globals.scheduler_set_as_server = true;
    }

    return PMIX_SUCCESS;
}

pmix_status_t prte_server_send_request(uint8_t cmd, prte_pmix_server_req_t *req)
{
    pmix_data_buffer_t *buf;
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CREATE(buf);

    /* construct a request message for the command */
    rc = PMIx_Data_pack(NULL, buf, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* pack the local reference ID */
    rc = PMIx_Data_pack(NULL, buf, &req->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* pack the requestor */
    rc = PMIx_Data_pack(NULL, buf, &req->tproc, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    if (PRTE_PMIX_ALLOC_REQ == cmd) {
        /* pack the allocation directive */
        rc = PMIx_Data_pack(NULL, buf, &req->allocdir, 1, PMIX_ALLOC_DIRECTIVE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    } else {
        /* pack the sessionID */
        rc = PMIx_Data_pack(NULL, buf, &req->sessionID, 1, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }

    /* pack the number of info */
    rc = PMIx_Data_pack(NULL, buf, &req->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    if (0 < req->ninfo) {
        /* pack the info */
        rc = PMIx_Data_pack(NULL, buf, req->info, req->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }

    /* send this request to the DVM controller */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_SCHED);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    return PMIX_SUCCESS;
}

#if 0
/* Callbacks to process an allocate request answer from the scheduler
 * and pass on any results to the requesting client
 */
static void passthru(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL != req->infocbfunc) {
        // call the requestor's callback with the returned info
        req->infocbfunc(req->status, req->info, req->ninfo, req->cbdata, req->rlcbfunc, req->rlcbdata);
    } else {
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

static void pass_request(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    pmix_status_t rc;
    size_t n = 0;
    pmix_info_t *xfer = NULL;
    PRTE_HIDE_UNUSED_PARAMS(sd, args, n, xfer);

    /* if we are the DVM master, then handle this ourselves - start
     * by ensuring the scheduler is connected to us */
    rc = prte_pmix_set_scheduler();
    if (PMIX_SUCCESS != rc) {
        goto callback;
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
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto callback;
    }
    return;

callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->infocbfunc) {
        req->infocbfunc(rc, req->info, req->ninfo, req->cbdata, localrelease, req);
        return;
    }
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}
#endif

/* this is the upcall from the PMIx server for the allocation
 * request support. Since we are going to touch global structures
 * (e.g., the session tracker pointer array), we have to threadshift
 * this request into our own internal progress thread. Note that the
 * allocation request could have come to this host from the
 * scheduler, or a tool, or even an application process. */
pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;
    pmix_status_t rc;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s allocate upcalled on behalf of proc %s:%u with %" PRIsize_t " infos",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), client->nspace, client->rank, ndata);

    /* create a request tracker for this operation */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "ALLOCATE: %s", PMIx_Alloc_directive_string(directive));
    PMIX_PROC_LOAD(&req->tproc, client->nspace, client->rank);
    req->allocdir = directive;
    req->info = (pmix_info_t *) data;
    req->ninfo = ndata;
    req->infocbfunc = cbfunc;
    req->cbdata = cbdata;

    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    if (!PRTE_PROC_IS_MASTER) {
        /* if we are not the DVM master, then we have to send
         * this request to the master for processing */
        rc = prte_server_send_request(PRTE_PMIX_ALLOC_REQ, req);
        if (PRTE_SUCCESS != rc) {
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            PMIX_RELEASE(req);
            return rc;
        }
        return rc;
    }

    // pass this to the RAS framework for handling
    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, prte_ras_base_modify, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}
