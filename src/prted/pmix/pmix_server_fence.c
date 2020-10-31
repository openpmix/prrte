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

#include "prte_config.h"

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
#include "src/runtime/prte_globals.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/rml/rml.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"

static void relcb(void *cbdata)
{
    uint8_t *data = (uint8_t*)cbdata;

    if (NULL != data) {
        free(data);
    }
}
static void pmix_server_release(int status, prte_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd=(prte_pmix_mdx_caddy_t*)cbdata;
    char *data = NULL;
    int32_t ndata = 0;
    int rc = PRTE_SUCCESS;

    PRTE_ACQUIRE_OBJECT(cd);

    /* unload the buffer */
    if (NULL != buf) {
        rc = prte_dss.unload(buf, (void**)&data, &ndata);
    }
    if (PRTE_SUCCESS == rc) {
        rc = status;
    }
    cd->cbfunc(rc, data, ndata, cd->cbdata, relcb, data);
    PRTE_RELEASE(cd);
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
    prte_pmix_mdx_caddy_t *cd=NULL;
    int rc;
    size_t i;
    prte_buffer_t *buf=NULL;

    cd = PRTE_NEW(prte_pmix_mdx_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

   /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PRTE_NEW(prte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (prte_process_name_t*)malloc(cd->sig->sz * sizeof(prte_process_name_t));
        memset(cd->sig->signature, 0, cd->sig->sz * sizeof(prte_process_name_t));
        for (i=0; i < nprocs; i++) {
            PRTE_PMIX_CONVERT_PROCT(rc, &cd->sig->signature[i], (pmix_proc_t*)&procs[i]);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(cd);
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }
    buf = PRTE_NEW(prte_buffer_t);

    if (NULL != data) {
        prte_dss.load(buf, data, ndata);
    }

    if (4 < prte_output_get_verbosity(prte_pmix_server_globals.output)) {
        char *tmp=NULL;
        (void)prte_dss.print(&tmp, NULL, cd->sig, PRTE_SIGNATURE);
        free(tmp);
    }

    /* pass it to the global collective algorithm */
    /* pass along any data that was collected locally */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd->sig, buf, 0, pmix_server_release, cd))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return PMIX_ERROR;
    }
    PRTE_RELEASE(buf);
    return PMIX_SUCCESS;
}

static void modex_resp(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    prte_buffer_t *reply;
    pmix_status_t prc;
    pmix_data_buffer_t pbuf;
    char *pdata;
    size_t psz;

    PRTE_ACQUIRE_OBJECT(req);

    /* pack the status */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &status, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }
    /* pack the id of the requested proc */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->tproc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }

    /* pack the remote daemon's request room number */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->remote_room_num, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }
    if (PMIX_SUCCESS == status) {
        /* return any provided data */
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &sz, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(prc);
            goto error;
        }
        if (0 < sz) {
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, data, sz, PMIX_BYTE))) {
                PMIX_ERROR_LOG(prc);
                goto error;
            }
        }
    }

    /* send the response */
    reply = PRTE_NEW(prte_buffer_t);
    PMIX_DATA_BUFFER_UNLOAD(&pbuf, pdata, psz);
    prte_dss.load(reply, pdata, psz);
    prte_rml.send_buffer_nb(&req->proxy, reply,
                            PRTE_RML_TAG_DIRECT_MODEX_RESP,
                            prte_rml_send_callback, NULL);

  error:
    PRTE_RELEASE(req);
    return;
}

static void dmodex_req(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    pmix_server_req_t *r;
    prte_job_t *jdata;
    prte_proc_t *proct, *dmn;
    prte_process_name_t prtenm;
    int rc, rnum;
    prte_buffer_t *buf;
    char *data=NULL;
    int32_t sz=0;
    pmix_data_buffer_t pbuf;
    pmix_status_t prc = PMIX_ERROR;
    bool refresh_cache = false;
    pmix_value_t *pval;

    PRTE_ACQUIRE_OBJECT(rq);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                         "%s DMODX REQ FOR %s:%u",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         req->tproc.nspace, req->tproc.rank);

    PRTE_PMIX_CONVERT_PROCT(rc, &prtenm, &req->tproc);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return;
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* check if they want us to refresh the cache */
    if (NULL != req->info) {
        size_t n;
        for (n=0; n < req->ninfo; n++) {
            if (PMIX_CHECK_KEY(&req->info[n], PMIX_GET_REFRESH_CACHE)) {
                refresh_cache = PMIX_INFO_TRUE(&req->info[n]);
            } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_REQUIRED_KEY)) {
                req->key = strdup(req->info[n].value.data.string);
            }
        }
    }
#endif

    prte_output_verbose(2, prte_pmix_server_globals.output,
                         "%s DMODX REQ REFRESH %s REQUIRED KEY %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         refresh_cache ? "TRUE" : "FALSE",
                         (NULL == req->key) ? "NULL" : req->key);

    if (!refresh_cache && NULL != req->key) {
        /* a race condition exists here because of the thread-shift - it is
         * possible that data for the specified proc arrived while we were
         * waiting to be serviced. In that case, the tracker that would have
         * indicated the data was already requested will have been removed,
         * and we would therefore think that we had to request it again.
         * So do a quick check to ensure we don't already have the desired
         * data */
        if (PMIX_SUCCESS == PMIx_Get(&req->tproc, req->key, req->info, req->ninfo, &pval)) {
            PMIX_VALUE_RELEASE(pval);
            /* mark that the result is to return to us */
            req->proxy = *PRTE_PROC_MY_NAME;
            /* save the request in the hotel until the
             * data is returned */
            if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
                prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
                /* can't just return as that would cause the requestor
                 * to hang, so instead execute the callback */
                prc = prte_pmix_convert_rc(rc);
                goto callback;
            }
            /* set the "remote" room number to our own */
            req->remote_room_num = req->room_num;
            PRTE_RETAIN(req);
            /* we have it - just to be safe, get the blob and return it */
            if (PMIX_SUCCESS != (prc = PMIx_server_dmodex_request(&req->tproc, modex_resp, req))) {
                PMIX_ERROR_LOG(prc);
                req->mdxcbfunc(prc, NULL, 0, req->cbdata, NULL, NULL);
                PRTE_RELEASE(req);
            }
            return;
        }
    }

    /* adjust the timeout to reflect the size of the job as it can take some
     * amount of time to start the job */
    PRTE_ADJUST_TIMEOUT(req);

    /* has anyone already requested data for this target? If so,
     * then the data is already on its way */
    for (rnum=0; rnum < prte_pmix_server_globals.reqs.num_rooms; rnum++) {
        prte_hotel_knock(&prte_pmix_server_globals.reqs, rnum, (void**)&r);
        if (NULL == r) {
            continue;
        }
        if (r->target.jobid == prtenm.jobid &&
            r->target.vpid == prtenm.vpid) {
            /* save the request in the hotel until the
             * data is returned */
            if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
                prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
                /* can't just return as that would cause the requestor
                 * to hang, so instead execute the callback */
                prc = prte_pmix_convert_rc(rc);
                goto callback;
            }
            return;
        }
    }

    /* lookup who is hosting this proc */
    if (NULL == (jdata = prte_get_job_data_object(prtenm.jobid))) {
        /* if we don't know the job, then it could be a race
         * condition where we are being asked about a process
         * that we don't know about yet. In this case, just
         * record the request and we will process it later */
        if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
            prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
            /* can't just return as that would cause the requestor
             * to hang, so instead execute the callback */
            prc = prte_pmix_convert_rc(rc);
            goto callback;
        }
        return;
    }
    /* if this is a request for rank=WILDCARD, then they want the job-level data
     * for this job. It was probably not stored locally because we aren't hosting
     * any local procs. There is no need to request the data as we already have
     * it - so just register the nspace so the local PMIx server gets it */
    if (PRTE_VPID_WILDCARD == prtenm.vpid) {
        rc = prte_pmix_server_register_nspace(jdata);
        if (PRTE_SUCCESS != rc) {
            prc = prte_pmix_convert_rc(rc);
            goto callback;
        }
        /* let the server know that the data is now available */
        if (NULL != req->mdxcbfunc) {
            req->mdxcbfunc(rc, NULL, 0, req->cbdata, NULL, NULL);
        }
        PRTE_RELEASE(req);
        return;
    }

    /* if they are asking about a specific proc, then fetch it */
    if (NULL == (proct = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, prtenm.vpid))) {
        /* if we find the job, but not the process, then that is an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        prc = prte_pmix_convert_rc(rc);
        goto callback;
    }

    if (NULL == (dmn = proct->node->daemon)) {
        /* we don't know where this proc is located - since we already
         * found the job, and therefore know about its locations, this
         * must be an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        prc = prte_pmix_convert_rc(rc);
        goto callback;
    }
    /* point the request to the daemon that is hosting the
     * target process */
    req->proxy = dmn->name;
    /* track the request so we know the function and cbdata
     * to callback upon completion */
    if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
        prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
        prc = prte_pmix_convert_rc(rc);
        goto callback;
    }
    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s:%d MY REQ ROOM IS %d FOR KEY %s",
                        __FILE__, __LINE__, req->room_num,
                        (NULL == req->key) ? "NULL" : req->key);
    /* if we are the host daemon, then this is a local request, so
     * just wait for the data to come in */
    if (PRTE_PROC_MY_NAME->vpid == dmn->name.vpid) {
        return;
    }

    /* construct a request message */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->tproc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
        goto callback;
    }
    /* include the request room number for quick retrieval */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->room_num, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
        goto callback;
    }
    /* add any qualifiers */
    if (PRTE_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(prc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
        goto callback;
    }
    if (0 < req->ninfo) {
        if (PRTE_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, req->info, req->ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(prc);
            prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            goto callback;
        }
    }

    /* send it to the host daemon */
    buf = PRTE_NEW(prte_buffer_t);
    PMIX_DATA_BUFFER_UNLOAD(&pbuf, data, sz);
    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
    prte_dss.load(buf, data, sz);
    if (PRTE_SUCCESS != (rc = prte_rml.send_buffer_nb(&dmn->name, buf, PRTE_RML_TAG_DIRECT_MODEX,
                                                      prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PRTE_RELEASE(buf);
        prc = prte_pmix_convert_rc(rc);
        goto callback;
    }
    return;

  callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(prc, NULL, 0, req->cbdata, NULL, NULL);
    }
    PRTE_RELEASE(req);
}

/* the local PMIx embedded server will use this function to call
 * us and request that we obtain data from a remote daemon */
pmix_status_t pmix_server_dmodex_req_fn(const pmix_proc_t *proc,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    /*  we have to shift threads to the PRTE thread, so
     * create a request and push it into that thread */
    PRTE_DMX_REQ(proc, info, ninfo, dmodex_req, cbfunc, cbdata);
    return PMIX_SUCCESS;
}
