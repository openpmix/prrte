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

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/grpcomm/grpcomm.h"
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
    rc = prte_grpcomm_fence(procs, nprocs, info, ninfo,
                            data, ndata, cbfunc, cbdata);
    /* grpcomm answers in PRTE codes and our caller reads PMIx ones - the two
     * agree only on success, so anything else has to be converted */
    if (PRTE_SUCCESS != rc) {
        return prte_pmix_convert_rc(rc);
    }
    return PMIX_SUCCESS;
}


/* completion of the nspace registration for a wildcard dmodex
 * request - executes on the PRRTE progress thread */
static void wildcard_reg_complete(pmix_status_t status, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;

    /* let the server know that the data is now available */
    if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(status, NULL, 0, req->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(req);
}

static void derived_relfn(void *cbdata);

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

    PMIX_ACQUIRE_OBJECT(req);

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
     * then the data is already on its way.
     *
     * Compare against the other request's tproc - the proc whose data was
     * asked for.  It must NOT be compared against r->target, which only the
     * monitor and tool-connection paths ever set and which is therefore
     * {"", PMIX_RANK_INVALID} for every dmodex request: PMIx_Check_nspace
     * treats an empty nspace as matching anything, so a request for
     * PMIX_RANK_WILDCARD (the job-level data fetch below) would "match" the
     * first entry in the array, get parked here, and never be answered. */
    for (rnum = 0; rnum < prte_pmix_server_globals.local_reqs.size; rnum++) {
        r = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, rnum);
        if (NULL == r) {
            continue;
        }
        if (PMIX_CHECK_PROCID(&r->tproc, &req->tproc)) {
            /* save the request in the array until the
             * data is returned */
            req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
            return;
        }
    }

    /* lookup who is hosting this proc */
    if (NULL == (jdata = prte_get_job_data_object(req->tproc.nspace))) {
        /* Two very different situations bring us here, and only one of them
         * is worth waiting for.
         *
         * A job we have not heard of YET is a race - the requestor got the
         * namespace from somewhere and our own record of it is still on its
         * way - so park the request and answer it when the job turns up.
         *
         * A job that has already FINISHED is never coming back.  Its object
         * was released when it terminated, so parking a request on it parks
         * it forever: nothing drains this array on a timer, and PMIx
         * deliberately sets no timeout on a host request so as not to race
         * us.  That is a real hang, and an easy one to hit - a process that
         * asks about a job it just spawned (PMIx_Get of the child's job
         * size; examples/dynamic.c does exactly this) wedges permanently if
         * the child was short-lived enough to be gone before the question
         * arrived.  The bigger the job, the longer the question takes to
         * arrive, and the likelier that is.
         *
         * So refuse what we can prove is over rather than waiting on it.
         * PMIX_ERR_NOT_FOUND is the truth and is what the caller's PMIx_Get
         * returns; what we must not do is leave them holding. */
        if (prte_pmix_server_job_has_departed(req->tproc.nspace)) {
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s DMODX REQ FOR %s:%u - JOB HAS ENDED",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                req->tproc.nspace, req->tproc.rank);
            prc = PMIX_ERR_NOT_FOUND;
            goto callback;
        }
        req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
        return;
    }
    /* if this is a request for rank=WILDCARD, then they want the job-level data
     * for this job. It was probably not stored locally because we aren't hosting
     * any local procs. There is no need to request the data as we already have
     * it - so just register the nspace so the local PMIx server gets it. The
     * registration completes asynchronously */
    if (PMIX_RANK_WILDCARD == req->tproc.rank) {
        /* ...but that registration is assembled from the job's map, and the
         * map is not always there.  It is created when the job is mapped and
         * released again the moment the job completes
         * (check_complete_resume), while the job object itself survives until
         * cleanup_job runs as a later event - and only cleanup_job records
         * the departure the guard above tests.  So a request landing in
         * between finds a jdata that is present, is not yet "departed", and
         * has nothing left to say about placement;
         * prte_pmix_server_register_nspace() walks map->nodes and takes the
         * daemon down.  That window is easy to hit exactly where the comment
         * above says it is - a parent asking about the child it just spawned
         * (examples/dynamic.c) when the child was short-lived.
         *
         * Refuse it the way the proc-level branch below refuses a proc the
         * mapper has not placed: PMIX_ERR_NOT_FOUND is the truth, and it is
         * what the caller's PMIx_Get returns. */
        if (NULL == jdata->map) {
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s DMODX REQ FOR %s:WILDCARD - JOB IS NO LONGER MAPPED",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->tproc.nspace);
            prc = PMIX_ERR_NOT_FOUND;
            goto callback;
        }
        rc = prte_pmix_server_register_nspace(jdata, wildcard_reg_complete, req);
        if (PRTE_SUCCESS != rc) {
            prc = prte_pmix_convert_rc(rc);
            goto callback;
        }
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

    /* A proc object exists from the moment the job is created, and it is the
     * mapper that gives it a node - so a request that arrives for a job whose
     * mapping has not finished finds one with nothing to say about where it
     * is.  Nothing below can be answered without that, derived or fetched. */
    if (NULL == proct->node) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prc = PMIX_ERR_NOT_FOUND;
        goto callback;
    }

    /* If what they want is something we decided when we mapped this job, we
     * already know it - asking the daemon that hosts the proc would be asking
     * it to read back our own answer.  This is the other half of the lazy
     * registration in prte_pmix_server_register_nspace(): that one declines to
     * publish what nobody may ever want, and this one produces it the moment
     * somebody does.  A refresh is the caller telling us our copy may be
     * stale, which is exactly when we must not answer from it.
     *
     * This is deliberately ahead of resolving the hosting daemon, because it
     * does not need one.  A proc whose daemon has gone - a lost node, a
     * shrink - can still be said to have been placed where it was placed, and
     * that is an answer eager publication would have had in hand.  Requiring
     * a live daemon here would turn it into a failure. */
    if (prte_pmix_server_globals.lazy_procdata && !refresh_cache &&
        prte_pmix_server_derivable_key(req->key) &&
        /* ...but not a binding we do not hold.  The launch message scatters
         * the cpusets, so a NULL one on a proc we do not host means "never
         * sent", not "not bound", and answering either way would be a
         * guess.  Send it to the daemon that forks the proc, which answers
         * this same closed set of keys straight out of its job object
         * (pmix_server_dmdx_recv) rather than out of anything the process
         * has to have published. */
        (NULL != proct->cpuset || proct->parent == PRTE_PROC_MY_NAME->rank ||
         (!PMIx_Check_key(req->key, PMIX_CPUSET) &&
          !PMIx_Check_key(req->key, PMIX_LOCALITY_STRING)))) {
        pmix_data_buffer_t dbuf;
        pmix_byte_object_t bo;

        PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
        prc = prte_pmix_server_derive_proc_data(jdata, proct, &dbuf);
        if (PMIX_SUCCESS == prc) {
            PMIX_DATA_BUFFER_UNLOAD(&dbuf, bo.bytes, bo.size);
            PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s DMODX REQ FOR %s:%u KEY %s ANSWERED LOCALLY (%lu bytes)",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                req->tproc.nspace, req->tproc.rank, req->key,
                                (unsigned long) bo.size);
            if (NULL != req->mdxcbfunc) {
                req->mdxcbfunc(PMIX_SUCCESS, bo.bytes, bo.size, req->cbdata,
                               derived_relfn, bo.bytes);
            } else {
                free(bo.bytes);
            }
            /* we answered before ever being tracked, so there is usually
             * nothing to clear - the constructor leaves local_index at -1 */
            if (0 <= req->local_index) {
                pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                            req->local_index, NULL);
            }
            PMIX_RELEASE(req);
            return;
        }
        /* we could not build it - fall through and ask the host daemon, which
         * is what we would have done anyway */
        PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
        prc = PMIX_ERROR;
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
    PRTE_RML_RELIABLE_SEND(rc, dmn->name.rank, buf, PRTE_RML_TAG_DIRECT_MODEX);
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

/* ---- deriving a remote proc's data instead of fetching it -------------
 *
 * With prte_pmix_server_globals.lazy_procdata set, register_nspace publishes
 * per-proc data only for the procs this daemon hosts.  Everything it would
 * have published about the others is still right here, in the job object the
 * launch message built, so a request for one of them is answered from local
 * memory rather than by asking the daemon that hosts it.
 *
 * These are exactly the keys PRRTE is the *authority* for - the placement and
 * binding it decided when it mapped the job.  Anything else a proc holds was
 * put there by the application, which PRRTE knows nothing about, so those
 * requests still go out on the wire as they always did.  That is what makes
 * the split safe without knowing anything about who is asking or why: we are
 * not guessing which keys matter, only answering for the ones we own.
 *
 * The answer goes back as a packed blob of pmix_kval_t, byte-identical in
 * shape to what a real direct modex returns, because PMIx stores the reply
 * itself and the scope it stores it in has to match what the re-satisfy after
 * a dmodex reply then looks in (PMIX_REMOTE, for a specific rank).  Storing
 * the values ourselves with PMIx_Data_store_internal would put them in the
 * INTERNAL scope, which that lookup does not reach.
 */
bool prte_pmix_server_derivable_key(const char *key)
{
    if (NULL == key) {
        /* no key named means "everything you have", and we do not have the
         * application's half of it */
        return false;
    }
    return PMIx_Check_key(key, PMIX_RANK) ||
           PMIx_Check_key(key, PMIX_GLOBAL_RANK) ||
           PMIx_Check_key(key, PMIX_APP_RANK) ||
           PMIx_Check_key(key, PMIX_APPNUM) ||
           PMIx_Check_key(key, PMIX_LOCAL_RANK) ||
           PMIx_Check_key(key, PMIX_NODE_RANK) ||
           PMIx_Check_key(key, PMIX_NODEID) ||
           PMIx_Check_key(key, PMIX_HOSTNAME) ||
           PMIx_Check_key(key, PMIX_CPUSET) ||
           PMIx_Check_key(key, PMIX_LOCALITY_STRING) ||
           PMIx_Check_key(key, PMIX_REINCARNATION);
}

static pmix_status_t pack_derived(pmix_data_buffer_t *buf, const char *key,
                                  pmix_value_t *val)
{
    pmix_kval_t kv;
    pmix_status_t rc;

    /* a stack kval is fine here: the packer reads only key and value, and the
     * value's storage belongs to our caller either way */
    kv.key = (char *) key;
    kv.value = val;
    rc = PMIx_Data_pack(NULL, buf, &kv, 1, PMIX_KVAL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

#define PACK_DERIVED(r, b, k, t, d)                     \
    do {                                                \
        pmix_value_t _v = PMIX_VALUE_STATIC_INIT;       \
        _v.type = (t);                                  \
        _v.d;                                           \
        (r) = pack_derived((b), (k), &_v);              \
    } while (0)

/* Build the blob describing one proc.  Mirrors the per-proc section of
 * prte_pmix_server_register_nspace() - if a key is added there and a peer can
 * ask for it, it belongs here too, or that key becomes a wire round trip. */
pmix_status_t prte_pmix_server_derive_proc_data(prte_job_t *jdata, prte_proc_t *proct,
                                               pmix_data_buffer_t *buf)
{
    pmix_status_t rc;
    pmix_rank_t vpid;
    uint32_t ui32;

    PACK_DERIVED(rc, buf, PMIX_RANK, PMIX_PROC_RANK, data.rank = proct->name.rank);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    vpid = proct->name.rank + jdata->offset;
    PACK_DERIVED(rc, buf, PMIX_GLOBAL_RANK, PMIX_PROC_RANK, data.rank = vpid);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PACK_DERIVED(rc, buf, PMIX_APP_RANK, PMIX_PROC_RANK, data.rank = proct->app_rank);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PACK_DERIVED(rc, buf, PMIX_APPNUM, PMIX_UINT32, data.uint32 = proct->app_idx);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PRTE_LOCAL_RANK_INVALID != proct->local_rank) {
        PACK_DERIVED(rc, buf, PMIX_LOCAL_RANK, PMIX_UINT16, data.uint16 = proct->local_rank);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    if (PRTE_NODE_RANK_INVALID != proct->node_rank) {
        PACK_DERIVED(rc, buf, PMIX_NODE_RANK, PMIX_UINT16, data.uint16 = proct->node_rank);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    PACK_DERIVED(rc, buf, PMIX_NODEID, PMIX_UINT32, data.uint32 = proct->node->index);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the hostname is gated by prte_hostname_cutoff in the eager registration,
     * because there it is paid for every proc in the job whether or not anyone
     * wants it.  Here it is paid only when somebody asks, so there is nothing
     * to ration. */
    PACK_DERIVED(rc, buf, PMIX_HOSTNAME, PMIX_STRING, data.string = proct->node->name);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    ui32 = 0;
    PACK_DERIVED(rc, buf, PMIX_REINCARNATION, PMIX_UINT32, data.uint32 = ui32);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (NULL != proct->cpuset) {
        char *locality = NULL;
        pmix_cpuset_t cpuset;

        PACK_DERIVED(rc, buf, PMIX_CPUSET, PMIX_STRING, data.string = proct->cpuset);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        PMIX_CPUSET_CONSTRUCT(&cpuset);
        cpuset.source = "hwloc";
        cpuset.bitmap = hwloc_bitmap_alloc();
        hwloc_bitmap_list_sscanf(cpuset.bitmap, proct->cpuset);
        rc = PMIx_server_generate_locality_string(&cpuset, &locality);
        hwloc_bitmap_free(cpuset.bitmap);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PACK_DERIVED(rc, buf, PMIX_LOCALITY_STRING, PMIX_STRING, data.string = locality);
        if (NULL != locality) {
            free(locality);
        }
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    } else if (proct->parent == PRTE_PROC_MY_NAME->rank) {
        /* An unbound proc still has to answer the locality question, and the
         * answer is "nothing to say" - the eager path registers a NULL for
         * exactly this case.  Only for a proc we HOST, though: for any other,
         * a NULL cpuset means the launch message scattered the bindings and
         * we were not sent this one, which is a different statement.  The
         * caller above has already refused to derive those two keys in that
         * case, so what is left here is a request for some other key on a
         * proc whose binding we cannot speak to - leave both out. */
        PACK_DERIVED(rc, buf, PMIX_LOCALITY_STRING, PMIX_STRING, data.string = NULL);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

/* PMIx is done with the blob we synthesized */
static void derived_relfn(void *cbdata)
{
    free(cbdata);
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
