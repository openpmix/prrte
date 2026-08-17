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
#include "src/util/name_fns.h"

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

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s sched response received for local req %d: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req_index,
                        PMIx_Error_string(ret));

    req = pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, req_index);
    if (NULL == req) {
        /* The index arrived on the wire, so it is untrusted - but it is also
         * OUR index, echoed back, so a miss means the request was retired
         * early and whoever is waiting on it will wait forever. There is
         * nothing to complete, so say so rather than returning in silence. */
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s sched response names local req %d, which is gone",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req_index);
        return;
    }

    /* Report the error */
    if (ret != PMIX_SUCCESS) {
        goto ANSWER;
    }

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &req->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
        goto ANSWER;
    }

    if (0 < req->ninfo) {
        PMIX_INFO_CREATE(req->info, req->ninfo);
        /* the request now owns this array - without this its destructor
         * leaves the whole unpacked result behind */
        req->copy = true;

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
        /* Pass the response back to the requestor.  The callback's cbdata is
         * the one the ORIGINAL caller gave us (req->cbdata) - PMIx uses it to
         * find the request it is completing.  Handing it the tracker instead
         * meant every allocation and every session-control request issued
         * from a non-master daemon was answered into the void: the client sat
         * in its PMIx call until it timed out, or the library faulted on the
         * unrecognized pointer. */
        req->infocbfunc(ret, req->info, req->ninfo, req->cbdata,
                        prte_pmix_server_req_release, req);
    } else {
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                    req->local_index, NULL);
        PMIX_RELEASE(req);
    }
}

pmix_status_t prte_pmix_set_scheduler(void)
{
    pmix_status_t rc;
    pmix_info_t info[2];

    if (!prte_pmix_server_globals.scheduler_connected) {
        /* Look for a scheduler to attach to - ONCE.
         *
         * PMIx_tool_attach_to_server is a BLOCKING call, and this runs on the
         * PRRTE progress thread, which is the only thread driving the DVM. It
         * hunts for a rendezvous and tries to connect to what it finds, so
         * repeating it per request puts a filesystem scan and a connect
         * attempt in front of every allocation and every session-control
         * request. Against a DVM with no scheduler - the ordinary case, where
         * it can only ever fail - that is pure cost on the one thread that
         * must not be spent.
         *
         * Nothing is lost by looking only once. A scheduler that appears
         * later announces itself by attaching to US, and the tool-connect
         * upcall (pmix_server_gen.c) sets scheduler_connected from there. */
        if (prte_pmix_server_globals.scheduler_lookup_done) {
            return PMIX_ERR_UNREACH;
        }
        prte_pmix_server_globals.scheduler_lookup_done = true;
        /* make it optional so we don't hang if there is no scheduler */
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SCHEDULER, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[1], PMIX_TOOL_CONNECT_OPTIONAL, NULL, PMIX_BOOL);
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s looking for a scheduler to attach to",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        rc = PMIx_tool_attach_to_server(NULL, &prte_pmix_server_globals.scheduler,
                                        info, 2);
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s scheduler attach returned %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        prte_pmix_server_globals.scheduler_connected = true;
    }

    /* direct our client-side calls at the scheduler.  This is not a
     * once-only step: an external data server is a second connection we may
     * hold, and whichever of the two was used last is the one PMIx would
     * otherwise send this request to. */
    return prte_pmix_set_primary_server(&prte_pmix_server_globals.scheduler);
}

pmix_status_t prte_pmix_set_primary_server(const pmix_proc_t *target)
{
    pmix_status_t rc;

    /* PMIX_CHECK_PROCID answers "true" for an empty nspace, so the flag -
     * not the identity - is what says we have ever designated one */
    if (prte_pmix_server_globals.primary_server_set &&
        PMIX_CHECK_PROCID(&prte_pmix_server_globals.primary_server, target)) {
        return PMIX_SUCCESS;
    }

    rc = PMIx_tool_set_server(target, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_XFER_PROCID(&prte_pmix_server_globals.primary_server, target);
    prte_pmix_server_globals.primary_server_set = true;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s primary server is now %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(target));
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
    } else if (PRTE_PMIX_SESSION_CTRL == cmd) {
        /* pack the sessionID */
        rc = PMIx_Data_pack(NULL, buf, &req->sessionID, 1, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }
    /* PRTE_PMIX_GROUP_CTXID has no field of its own - it asks for a number
     * and nothing about that number depends on the requestor. Keep this in
     * step with the matching unpack in pmix_server_sched(). */

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
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_SCHED);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    return PMIX_SUCCESS;
}

static void _alloc_request(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(req);

    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    if (!PRTE_PROC_IS_MASTER) {
        /* if we are not the DVM master, then we have to send
         * this request to the master for processing */
        rc = prte_server_send_request(PRTE_PMIX_ALLOC_REQ, req);
        if (PRTE_SUCCESS != rc) {
            goto callback;
        }
        return;
    }

    /* pass this to the RAS framework for handling - we are
     * already on the progress thread */
    prte_ras_base_modify(0, 0, req);
    return;

callback:
    /* only executed on error - let the requestor know */
    if (NULL != req->infocbfunc) {
        req->infocbfunc(rc, NULL, 0, req->cbdata, prte_pmix_server_req_release, req);
        return;
    }
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* this is the upcall from the PMIx server for the allocation
 * request support. Since we are going to touch global structures
 * (e.g., the local request tracker array), we have to threadshift
 * this request into our own internal progress thread. Note that the
 * allocation request could have come to this host from the
 * scheduler, or a tool, or even an application process. */
pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;

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

    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, _alloc_request, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}
