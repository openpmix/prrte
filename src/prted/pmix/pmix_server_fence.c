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
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/output.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/rml/rml.h"

#include "pmix_server_internal.h"
#include "pmix_server.h"

static void relcb(void *cbdata)
{
    uint8_t *data = (uint8_t*)cbdata;

    if (NULL != data) {
        free(data);
    }
}
static void pmix_server_release(int status, prrte_buffer_t *buf, void *cbdata)
{
    prrte_pmix_mdx_caddy_t *cd=(prrte_pmix_mdx_caddy_t*)cbdata;
    char *data = NULL;
    int32_t ndata = 0;
    int rc = PRRTE_SUCCESS;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* unload the buffer */
    if (NULL != buf) {
        rc = prrte_dss.unload(buf, (void**)&data, &ndata);
    }
    if (PRRTE_SUCCESS == rc) {
        rc = status;
    }
    cd->cbfunc(rc, data, ndata, cd->cbdata, relcb, data);
    PRRTE_RELEASE(cd);
}

/* this function is called when all the local participants have
 * called fence - thus, the collective is already locally
 * complete at this point. We therefore just need to create the
 * signature and pass the collective into grpcomm */
pmix_status_t pmix_server_fencenb_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     char *data, size_t ndata,
                                     pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_mdx_caddy_t *cd=NULL;
    int rc;
    size_t i;
    prrte_buffer_t *buf=NULL;

    cd = PRRTE_NEW(prrte_pmix_mdx_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

   /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PRRTE_NEW(prrte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (prrte_process_name_t*)malloc(cd->sig->sz * sizeof(prrte_process_name_t));
        memset(cd->sig->signature, 0, cd->sig->sz * sizeof(prrte_process_name_t));
        for (i=0; i < nprocs; i++) {
            PRRTE_PMIX_CONVERT_PROCT(rc, &cd->sig->signature[i], &procs[i]);
            if (PRRTE_SUCCESS != rc) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(cd);
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }
    buf = PRRTE_NEW(prrte_buffer_t);

    if (NULL != data) {
        prrte_dss.load(buf, data, ndata);
    }

    if (4 < prrte_output_get_verbosity(prrte_pmix_server_globals.output)) {
        char *tmp=NULL;
        (void)prrte_dss.print(&tmp, NULL, cd->sig, PRRTE_SIGNATURE);
        free(tmp);
    }

    /* pass it to the global collective algorithm */
    /* pass along any data that was collected locally */
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.allgather(cd->sig, buf, 0, pmix_server_release, cd))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return PMIX_ERROR;
    }
    PRRTE_RELEASE(buf);
    return PMIX_SUCCESS;
}

static void dmodex_req(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    pmix_server_req_t *r;
    prrte_job_t *jdata;
    prrte_proc_t *proct, *dmn;
    int rc, rnum;
    prrte_buffer_t *buf;
    char *data=NULL;
    int32_t sz=0;

    PRRTE_ACQUIRE_OBJECT(rq);

    /* a race condition exists here because of the thread-shift - it is
     * possible that data for the specified proc arrived while we were
     * waiting to be serviced. In that case, the tracker that would have
     * indicated the data was already requested will have been removed,
     * and we would therefore think that we had to request it again.
     * So do a quick check to ensure we don't already have the desired
     * data */
    PRRTE_MODEX_RECV_STRING(rc, "modex", &req->target, &data, &sz);
    if (PRRTE_SUCCESS == rc) {
        req->mdxcbfunc(rc, data, sz, req->cbdata, relcb, data);
        PRRTE_RELEASE(req);
        return;
    }

    /* adjust the timeout to reflect the size of the job as it can take some
     * amount of time to start the job */
    PRRTE_ADJUST_TIMEOUT(req);

    /* has anyone already requested data for this target? If so,
     * then the data is already on its way */
    for (rnum=0; rnum < prrte_pmix_server_globals.reqs.num_rooms; rnum++) {
        prrte_hotel_knock(&prrte_pmix_server_globals.reqs, rnum, (void**)&r);
        if (NULL == r) {
            continue;
        }
        if (r->target.jobid == req->target.jobid &&
            r->target.vpid == req->target.vpid) {
            /* save the request in the hotel until the
             * data is returned */
            if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
                prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
                /* can't just return as that would cause the requestor
                 * to hang, so instead execute the callback */
                goto callback;
            }
            return;
        }
    }

    /* lookup who is hosting this proc */
    if (NULL == (jdata = prrte_get_job_data_object(req->target.jobid))) {
        /* if we don't know the job, then it could be a race
         * condition where we are being asked about a process
         * that we don't know about yet. In this case, just
         * record the request and we will process it later */
        if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
            prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
            /* can't just return as that would cause the requestor
             * to hang, so instead execute the callback */
            goto callback;
        }
        return;
    }
    /* if this is a request for rank=WILDCARD, then they want the job-level data
     * for this job. It was probably not stored locally because we aren't hosting
     * any local procs. There is no need to request the data as we already have
     * it - so just register the nspace so the local PMIx server gets it */
    if (PRRTE_VPID_WILDCARD == req->target.vpid) {
        rc = prrte_pmix_server_register_nspace(jdata);
        if (PRRTE_SUCCESS != rc) {
            goto callback;
        }
        /* let the server know that the data is now available */
        if (NULL != req->mdxcbfunc) {
            req->mdxcbfunc(rc, NULL, 0, req->cbdata, NULL, NULL);
        }
        PRRTE_RELEASE(req);
        return;
    }

    /* if they are asking about a specific proc, then fetch it */
    if (NULL == (proct = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, req->target.vpid))) {
        /* if we find the job, but not the process, then that is an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        rc = PRRTE_ERR_NOT_FOUND;
        goto callback;
    }

    if (NULL == (dmn = proct->node->daemon)) {
        /* we don't know where this proc is located - since we already
         * found the job, and therefore know about its locations, this
         * must be an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        rc = PRRTE_ERR_NOT_FOUND;
        goto callback;
    }
    /* point the request to the daemon that is hosting the
     * target process */
    req->proxy.vpid = dmn->name.vpid;

    /* track the request so we know the function and cbdata
     * to callback upon completion */
    if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
        prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
        goto callback;
    }

    /* if we are the host daemon, then this is a local request, so
     * just wait for the data to come in */
    if (PRRTE_PROC_MY_NAME->vpid == dmn->name.vpid) {
        return;
    }

    /* construct a request message */
    buf = PRRTE_NEW(prrte_buffer_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &req->target, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(buf);
        goto callback;
    }
    /* include the request room number for quick retrieval */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &req->room_num, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(buf);
        goto callback;
    }

    /* send it to the host daemon */
    if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(&dmn->name, buf, PRRTE_RML_TAG_DIRECT_MODEX,
                                                      prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(buf);
        goto callback;
    }
    return;

  callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(rc, NULL, 0, req->cbdata, NULL, NULL);
    }
    PRRTE_RELEASE(req);
}

/* the local PMIx embedded server will use this function to call
 * us and request that we obtain data from a remote daemon */
pmix_status_t pmix_server_dmodex_req_fn(const pmix_proc_t *proc,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    prrte_process_name_t name;
    int rc;

    PRRTE_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (PRRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /*  we have to shift threads to the PRRTE thread, so
     * create a request and push it into that thread */
    PRRTE_DMX_REQ(name, dmodex_req, cbfunc, cbdata);
    return PMIX_SUCCESS;
}
