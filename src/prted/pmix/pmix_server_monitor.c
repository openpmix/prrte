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

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

/* if this request is coming up to us, then we have to pass the request to the HNP so it
 * can xcast it to all daemons in the DVM. When a daemon receives a request, it has to
 * pass the request down to its PMIx server for execution, then return the results
 * back to the HNP. Upon completion of the collective, the HNP must send the result
 * to the daemon that hosts the requestor so that daemon can relay the results back
 * down to the requestor.
 */

static void rlfn(void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

static void mfn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    pmix_data_buffer_t msg;
    pmix_status_t rc;
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

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
    ret = prte_grpcomm.xcast(PRTE_RML_TAG_MONITOR_REQUEST, &msg);
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
        req->infocbfunc(rc, NULL, 0, req->cbdata, rlfn, req);
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
    pmix_server_req_t *req;
    PRTE_HIDE_UNUSED_PARAMS(error);

    // protection
    if (NULL == requestor || NULL == monitor) {
        return PMIX_ERR_BAD_PARAM;
    }

    // create a tracking object
    req = PMIX_NEW(pmix_server_req_t);
    memcpy(&req->target, requestor, sizeof(pmix_proc_t));
    req->monitor = (pmix_info_t*)monitor;
    req->pstatus = error;
    req->directives = (pmix_info_t*)directives;
    req->ndirs = ndirs;
    req->infocbfunc = cbfunc;
    req->cbdata = cbdata;
    req->ndaemons = prte_process_info.num_daemons - 1;

    // need to threadshift this to our event base
    prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, mfn, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void mycbfn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *rq2 = (pmix_server_req_t*)cbdata;
    pmix_server_req_t *req = (pmix_server_req_t*)rq2->cbdata;
    pmix_data_buffer_t *msg;
    pmix_status_t rc;
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

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

    // if it failed, then nothing more to pack
    if (PMIX_SUCCESS == rq2->status) {
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
    // execute the release callback
    if (NULL != rq2->rlcbfunc) {
        rq2->rlcbfunc(rq2->rlcbdata);
    }

    // cleanup
    pmix_pointer_array_set_item(&prte_pmix_server_globals.remote_reqs, req->remote_index, NULL);
    PMIX_RELEASE(req);
    PMIX_RELEASE(rq2);
}

static void mycb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                 pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_server_req_t *rq2;

    // need to threadshift this into our progress thread
    rq2 = PMIX_NEW(pmix_server_req_t);
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
    pmix_status_t rc, event, ret;
    pmix_rank_t dvpid;
    int32_t cnt;
    int remote_index;
    pmix_info_t *monitor;
    size_t ndirs;
    pmix_info_t *directives = NULL;
    pmix_server_req_t *req;
    pmix_data_buffer_t *msg;
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
    if (0 < ndirs) {
        cnt = ndirs;
        rc = PMIx_Data_unpack(NULL, buffer, directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Info_free(monitor, 1);
            PMIx_Info_free(directives, ndirs);
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
    req = PMIX_NEW(pmix_server_req_t);
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
    PMIX_DATA_BUFFER_CREATE(msg);

    // pack my vpid
    ret = PMIx_Data_pack(NULL, msg, &prte_process_info.myproc.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }

    // pack the room number for the requestor's tracker
    ret = PMIx_Data_pack(NULL, msg, &remote_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    // pack an error status to indicate a problem
    ret = PMIx_Data_pack(NULL, msg, &rc, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }

    // send to the requesting daemon
    PRTE_RML_SEND(ret, dvpid, msg, PRTE_RML_TAG_MONITOR_RESP);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
    }
}

void pmix_server_monitor_resp(int status, pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc, rstatus;
    pmix_rank_t dvpid;
    int local_index;
    pmix_server_req_t *req;
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
    req = (pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, local_index);
    if (NULL == req) {
        // bad index, or we no longer have this request
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }

    // record that another daemon reported
    ++req->nreported;

    // unpack the returned status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &rstatus, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        req->pstatus = rc;
        return;
    }
    req->pstatus = rstatus;

    // if it succeeded, then unpack the results
    if (PMIX_SUCCESS == rstatus) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            req->pstatus = rc;
            return;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            cnt = ninfo;
            rc = PMIx_Data_unpack(NULL, buffer, info, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIx_Info_free(info, ninfo);
                req->pstatus = rc;
                return;
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
            req->info = results;
            req->ninfo = sz;
        }
        req->copy = true;
    }

    // if all daemons have reported, then we are complete
    if (req->ndaemons == req->nreported) {
        if (NULL != req->infocbfunc) {
            req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata,
                            rlfn, req);
        } else {
            // nothing we can do!
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            PMIX_RELEASE(req);
        }
    }
}
