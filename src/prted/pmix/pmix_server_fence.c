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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/proc_info.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

/* this function is called when all the local participants have
 * called fence - thus, the collective is already locally
 * complete at this point. We therefore just need to create the
 * signature and pass the collective into grpcomm */
pmix_status_t pmix_server_fencenb_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo, char *data,
                                     size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    int rc;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s FENCE UPCALLED ON NODE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        prte_process_info.nodename);

    // just pass this along
    rc = prte_grpcomm.fence(procs, nprocs, info, ninfo,
                            data, ndata, cbfunc, cbdata);
    return rc;
}


static void dmodex_req(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    prte_pmix_server_req_t *r;
    prte_job_t *jdata;
    prte_proc_t *proct, *dmn;
    int rc, rnum;
    pmix_data_buffer_t *buf;
    pmix_status_t prc = PMIX_ERROR;
    bool refresh_cache = false;
    pmix_value_t *pval;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(rq);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s DMODX REQ FOR %s:%u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        req->tproc.nspace, req->tproc.rank);

    /* check if they want us to refresh the cache */
    if (NULL != req->info) {
        size_t n;
        for (n = 0; n < req->ninfo; n++) {
            if (PMIX_CHECK_KEY(&req->info[n], PMIX_GET_REFRESH_CACHE)) {
                refresh_cache = PMIX_INFO_TRUE(&req->info[n]);
            } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_REQUIRED_KEY)) {
                req->key = strdup(req->info[n].value.data.string);
            }
        }
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s DMODX REQ REFRESH %s REQUIRED KEY %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), refresh_cache ? "TRUE" : "FALSE",
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
            /* respond to the request so we trigger release of the
             * waiting procs */
            if (NULL != req->mdxcbfunc) {
                req->mdxcbfunc(PMIX_SUCCESS, NULL, 0, req->cbdata, NULL, NULL);
            }
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            PMIX_RELEASE(req);
            return;
        }
    }

    /* has anyone already requested data for this target? If so,
     * then the data is already on its way */
    for (rnum = 0; rnum < prte_pmix_server_globals.local_reqs.size; rnum++) {
        r = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, rnum);
        if (NULL == r) {
            continue;
        }
        if (PMIX_CHECK_PROCID(&r->target, &req->tproc)) {
            /* save the request in the array until the
             * data is returned */
            req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
            return;
        }
    }

    /* lookup who is hosting this proc */
    if (NULL == (jdata = prte_get_job_data_object(req->tproc.nspace))) {
        /* if we don't know the job, then it could be a race
         * condition where we are being asked about a process
         * that we don't know about yet. In this case, just
         * record the request and we will process it later */
        req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
        return;
    }
    /* if this is a request for rank=WILDCARD, then they want the job-level data
     * for this job. It was probably not stored locally because we aren't hosting
     * any local procs. There is no need to request the data as we already have
     * it - so just register the nspace so the local PMIx server gets it */
    if (PMIX_RANK_WILDCARD == req->tproc.rank) {
        rc = prte_pmix_server_register_nspace(jdata);
        if (PRTE_SUCCESS != rc) {
            prc = prte_pmix_convert_rc(rc);
            goto callback;
        }
        /* let the server know that the data is now available */
        if (NULL != req->mdxcbfunc) {
            req->mdxcbfunc(rc, NULL, 0, req->cbdata, NULL, NULL);
        }
        PMIX_RELEASE(req);
        return;
    }

    /* if they are asking about a specific proc, then fetch it */
    proct = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, req->tproc.rank);
    if (NULL == proct) {
        /* if we find the job, but not the process, then that is an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prc = PMIX_ERR_NOT_FOUND;
        goto callback;
    }

    if (NULL == (dmn = proct->node->daemon)) {
        /* we don't know where this proc is located - since we already
         * found the job, and therefore know about its locations, this
         * must be an error */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prc = PMIX_ERR_NOT_FOUND;
        goto callback;
    }
    /* point the request to the daemon that is hosting the
     * target process */
    req->proxy = dmn->name;
    /* track the request so we know the function and cbdata
     * to callback upon completion */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s:%d MY REQ INDEX IS %d FOR KEY %s",
                        __FILE__, __LINE__, req->local_index,
                        (NULL == req->key) ? "NULL" : req->key);
    /* if we are the host daemon, then this is a local request, so
     * just wait for the data to come in */
    if (PRTE_PROC_MY_NAME->rank == dmn->name.rank) {
        return;
    }

    /* construct a request message */
    PMIX_DATA_BUFFER_CREATE(buf);
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, buf, &req->tproc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }
    /* include the request index for quick retrieval */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, buf, &req->local_index, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }
    /* add any qualifiers */
    if (PRTE_SUCCESS != (prc = PMIx_Data_pack(NULL, buf, &req->ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(prc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }
    if (0 < req->ninfo) {
        if (PRTE_SUCCESS != (prc = PMIx_Data_pack(NULL, buf, req->info, req->ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(prc);
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto callback;
        }
    }

    /* send it to the host daemon */
    PRTE_RML_SEND(rc, dmn->name.rank, buf, PRTE_RML_TAG_DIRECT_MODEX);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        prc = prte_pmix_convert_rc(rc);
        goto callback;
    }
    return;

callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(prc, NULL, 0, req->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(req);
}

/* the local PMIx embedded server will use this function to call
 * us and request that we obtain data from a remote daemon */
pmix_status_t pmix_server_dmodex_req_fn(const pmix_proc_t *proc, const pmix_info_t info[],
                                        size_t ninfo, pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    /*  we have to shift threads to the PRTE thread, so
     * create a request and push it into that thread */
    PRTE_DMX_REQ(proc, info, ninfo, dmodex_req, cbfunc, cbdata);
    return PMIX_SUCCESS;
}
