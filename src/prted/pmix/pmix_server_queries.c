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
 * Copyright (c) 2024      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/prted/pmix/pmix_server_internal.h"

/* A query's qualifiers are the requesting client's own: PMIx forwards the
 * array to the host without inspecting what any entry holds, so reading a
 * fixed member of the value union is reading whatever eight bytes the caller
 * chose to put there.  Every qualifier this function acts on other than the
 * two numeric ones is a string that it then hands to strlen(), strcmp() or
 * PMIx_Check_nspace(), so a mistyped one is a fault - which any process or
 * tool attached to a daemon could produce with a single PMIx_Query_info. */
static bool qual_string(const pmix_info_t *qual, char **str)
{
    if (PMIX_STRING != qual->value.type) {
        return false;
    }
    *str = qual->value.data.string;
    return true;
}

static void qrel(void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

/* ==================================================================== *
 * Queries a daemon cannot answer
 *
 * A prted holds the identity half of the DVM's node table and nothing
 * else - the nidmap ships node names, aliases, daemon vpids and pool
 * slots, never slot counts or node state - and holds no session but the
 * default one.  A key whose answer comes out of either therefore has to
 * be asked of the DVM master.
 *
 * Which keys those are is deliberately not written down anywhere.  Such
 * a list is a point-in-time snapshot that goes stale the first time
 * someone adds a key, and a stale entry does not fail loudly: it returns
 * a default-constructed zero that reads exactly like a real answer.  So
 * the decision is made by the *read* instead.  A branch reaches
 * allocation state only through prte_get_allocated_nodes() and its two
 * companions, which succeed on the master and return
 * PRTE_ERR_NOT_AUTHORITATIVE anywhere else; a branch that gets that back
 * jumps to the "defer" label and its key is forwarded.  A key added
 * tomorrow that reads capacity is relayed with no edit here, and one
 * that reads only what the nidmap and the launch message already deliver
 * stays local with no edit either.
 *
 * Deferral is per key, not per query and not per request, because the
 * three things a daemon must answer for *itself* can arrive in the same
 * PMIx_Query_info as a key only the master can answer: PMIX_HWLOC_XML_*
 * exports this node's topology, an unqualified PMIX_SERVER_URI is this
 * daemon's own URI, and PMIX_QUERY_LOCAL_PROC_TABLE means the procs this
 * daemon is hosting.  Relaying a whole query would answer those about
 * the master.
 * ==================================================================== */

/* Finish a query: convert what was gathered, decide the status, and hand
 * it to the client.  Reached either directly, when every key was answered
 * here, or from the master's reply once its results have been merged in. */
static void query_complete(prte_pmix_server_op_caddy_t *cd, void *results,
                           size_t nkeys, pmix_status_t ret)
{
    prte_pmix_server_op_caddy_t *rcd;
    pmix_status_t rc;
    /* PMIx initializes this before anything in PMIx_Info_list_convert() can
     * fail, but every path here depends on that and the early ones arrive
     * having never touched it - so initialize it here rather than rely on
     * the callee to initialize its caller's stack */
    pmix_data_array_t dry = PMIX_DATA_ARRAY_STATIC_INIT;

    rcd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    PMIX_INFO_LIST_CONVERT(rc, results, &dry);
    if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    PMIX_INFO_LIST_RELEASE(results);
    /* only decide the status here if no arm has already decided it.  An error
     * an arm set is what actually happened - a malformed qualifier, a job we
     * do not know - and reporting "not found" in its place tells the caller
     * the DVM looked.  PMIX_ERR_NOT_SUPPORTED used to be the one error
     * protected from this, which is how the shape of it is visible: every
     * other error an arm could set was being thrown away with an empty
     * result list, which is exactly what an arm that failed leaves behind */
    if (PMIX_SUCCESS == ret) {
        if (PMIX_ERR_EMPTY == rc || 0 == dry.size) {
            ret = PMIX_ERR_NOT_FOUND;
        } else if (dry.size < nkeys) {
            /* against cd->ninfo, which a query caddy never sets, this compared
             * with zero and PMIX_QUERY_PARTIAL_SUCCESS could not be reached.
             * The count that means something here is the number of keys that
             * were asked for: fewer results than that is what partial success
             * is for, and a caller has no other way to learn that one of its
             * keys went unanswered */
            ret = PMIX_QUERY_PARTIAL_SUCCESS;
        }
    }
    rcd->ninfo = dry.size;
    rcd->info = (pmix_info_t *) dry.array;
    // memory allocated in the data array will be free'd when rcd is released
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
    PMIX_RELEASE(cd);
}

/* Record one key for the master, carrying the qualifiers of the query it
 * came from - they are what select the job, node or allocation it is
 * asking about, so the key is meaningless without them. */
static void defer_key(pmix_query_t *dq, size_t *ndq, pmix_query_t *q, char *key)
{
    pmix_query_t *d = &dq[*ndq];
    size_t i;

    PMIx_Argv_append_nosize(&d->keys, key);
    if (NULL != q->qualifiers && 0 < q->nqual) {
        PMIX_INFO_CREATE(d->qualifiers, q->nqual);
        for (i = 0; i < q->nqual; i++) {
            PMIX_INFO_XFER(&d->qualifiers[i], &q->qualifiers[i]);
        }
        d->nqual = q->nqual;
    }
    ++(*ndq);
}

/* Send the deferred keys to the master.  The results gathered locally ride
 * on the tracker and are merged with the master's reply, so the client sees
 * one answer covering every key it asked for. */
static pmix_status_t relay_query(prte_pmix_server_op_caddy_t *cd, void *results,
                                 size_t nkeys, pmix_query_t *dq, size_t ndq)
{
    prte_pmix_server_req_t *req;
    pmix_data_buffer_t *buf;
    pmix_status_t rc;
    int ret;

    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "QUERY");
    req->qcaddy = cd;
    req->qresults = results;
    req->nkeys = nkeys;
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    PMIX_DATA_BUFFER_CREATE(buf);
    /* our tracking room number, which the master echoes back */
    rc = PMIx_Data_pack(NULL, buf, &req->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    /* the client that asked - the master defaults a query to the requestor's
     * own job, so answering as ourselves would answer about the wrong one */
    rc = PMIx_Data_pack(NULL, buf, &cd->proct, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    rc = PMIx_Data_pack(NULL, buf, &ndq, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    rc = PMIx_Data_pack(NULL, buf, dq, ndq, PMIX_QUERY);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    PRTE_RML_RELIABLE_SEND(ret, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_QUERY);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(buf);
        rc = prte_pmix_convert_rc(ret);
        goto error_sent;
    }
    return PMIX_SUCCESS;

error:
    PMIX_DATA_BUFFER_RELEASE(buf);
error_sent:
    /* nothing will answer this tracker, and the caller is about to complete
     * the query itself - so let go of the caddy and the results it holds */
    req->qcaddy = NULL;
    req->qresults = NULL;
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);
    PMIX_RELEASE(req);
    return rc;
}

static void _query(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_query_t *q;
    pmix_status_t ret = PMIX_SUCCESS;
    void *results, *plist, *stack, *cache;
    pmix_pointer_array_t *alloc_nodes, *dvm_nodes, *alloc_sessions;
    pmix_nspace_t jobid;
    prte_job_t *jdata;
    prte_node_t *node, *ndptr;
    int j, k, rc, prc;
    size_t m, n, p;
    /* keys this daemon cannot answer, bound for the master.  Sized to the
     * total number of keys asked for, so a deferral never has to grow it. */
    pmix_query_t *dq = NULL;
    size_t ndq = 0, nq_alloc = 0;
    uint32_t key, nodeid, sessionid = UINT32_MAX, nslots;
    char **nspaces, *hostname, *uri;
    char *cmdline;
    const char *allocid;
#ifdef PMIX_ALLOC_PROPERTY
        const char *allocprop;
#endif
    char **ans, *tmp;
    size_t nkeys = 0;
    char *psetname;
    prte_app_context_t *app;
    prte_session_t *session;
    int matched;
    pmix_proc_info_t *procinfo;
    /* PMIx initializes this before anything in PMIx_Info_list_convert() can
     * fail, but every path to the done: label depends on that and the early
     * ones reach it having never touched this - so initialize it here rather
     * than rely on the callee to initialize its caller's stack */
    pmix_data_array_t dry = PMIX_DATA_ARRAY_STATIC_INIT;
    prte_proc_t *proct;
    pmix_proc_t *proc;
    size_t sz;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s processing query",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* On the master nothing ever defers, so do not pay for the array there.
     * Elsewhere, count the keys first: a deferral must not fail, and a
     * fixed-size array bounded by the key count cannot. */
    if (!PRTE_PROC_IS_MASTER) {
        for (m = 0; m < cd->nqueries; m++) {
            q = &cd->queries[m];
            if (NULL == q->keys) {
                continue;
            }
            for (n = 0; NULL != q->keys[n]; n++) {
                ++nq_alloc;
            }
        }
        if (0 < nq_alloc) {
            PMIX_QUERY_CREATE(dq, nq_alloc);
        }
    }

    PMIX_INFO_LIST_START(results);

    /* see what they wanted */
    for (m = 0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        hostname = NULL;
        nodeid = UINT32_MAX;
        psetname = NULL;
        allocid = NULL;
#ifdef PMIX_ALLOC_PROPERTY
        allocprop = NULL;
#endif
        /* default to the requestor's jobid */
        PMIX_LOAD_NSPACE(jobid, cd->proct.nspace);
        /* see if they provided any qualifiers */
        if (NULL != q->qualifiers && 0 < q->nqual) {
            for (n = 0; n < q->nqual; n++) {
                pmix_output_verbose(2, prte_pmix_server_globals.output,
                                    "%s qualifier key \"%s\" : value \"%s\"",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                    (q->qualifiers[n].value.type == PMIX_STRING
                                         ? q->qualifiers[n].value.data.string
                                         : "(not a string)"));

                if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NSPACE)) {
                    char *nsq;
                    if (!qual_string(&q->qualifiers[n], &nsq)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    // the nspace could be NULL, indicating that this is a
                    // wildcard request - if so, then ignore it here
                    if (NULL == nsq || 0 == strlen(nsq)) {
                        PMIX_LOAD_NSPACE(jobid, NULL);
                        continue;
                    }
                    /* Never trust the namespace string that is provided.
                     * First check to see if we know about this namespace. If
                     * not then return an error. If so then continue on.
                     */
                    /* Make sure the qualifier namespace exists */
                    matched = 0;
                    for (k = 0; k < prte_job_data->size; k++) {
                        jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                        if (NULL != jdata) {
                            if (PMIX_CHECK_NSPACE(nsq, jdata->nspace)) {
                                matched = 1;
                                break;
                            }
                        }
                    }
                    if (0 == matched) {
                        pmix_output_verbose(2, prte_pmix_server_globals.output,
                                            "%s qualifier key \"%s\" : value \"%s\" is an unknown namespace",
                                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                            nsq);
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                    PMIX_LOAD_NSPACE(jobid, jdata->nspace);
                    if (PMIX_NSPACE_INVALID(jobid)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_GROUP_ID)) {
                    char *grpq;
                    prte_pmix_server_pset_t *ps;
                    /* Never trust the group string that is provided.
                     * First check to see if we know about this group. If
                     * not then return an error. If so then continue on.
                     */
                    if (!qual_string(&q->qualifiers[n], &grpq) || NULL == grpq) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    /* Make sure the qualifier group exists */
                    matched = 0;
                    PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, prte_pmix_server_pset_t)
                    {
                        if (NULL != ps->name && PMIX_CHECK_NSPACE(grpq, ps->name)) {
                            matched = 1;
                            break;
                        }
                    }
                    if (0 == matched) {
                        pmix_output_verbose(2, prte_pmix_server_globals.output,
                                            "%s qualifier key \"%s\" : value \"%s\" is an unknown group",
                                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                            grpq);
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    PMIX_LOAD_NSPACE(jobid, grpq);
                    if (PMIX_NSPACE_INVALID(jobid)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_HOSTNAME)) {
                    if (!qual_string(&q->qualifiers[n], &hostname)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NODEID)) {
                    rc = PMIx_Value_get_number(&q->qualifiers[n].value, &nodeid, PMIX_UINT32);
                    if (PMIX_SUCCESS != rc) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_PSET_NAME)) {
                    if (!qual_string(&q->qualifiers[n], &psetname)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_SESSION_ID)) {
                    rc = PMIx_Value_get_number(&q->qualifiers[n].value, &sessionid, PMIX_UINT32);
                    if (PMIX_SUCCESS != rc) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_ALLOC_ID)) {
                    if (!qual_string(&q->qualifiers[n], (char **) &allocid)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

#ifdef PMIX_ALLOC_PROPERTY
                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_ALLOC_PROPERTY)) {
                    if (!qual_string(&q->qualifiers[n], (char **) &allocprop)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
#endif
                }

            }
        }
        for (n = 0; NULL != q->keys[n]; n++) {
            ++nkeys;
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s processing key %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->keys[n]);

            if (PMIx_Check_key(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
                        PMIx_Argv_append_nosize(&nspaces, jdata->nspace);
                    }
                }
                /* join the results into a single comma-delimited string */
                tmp = PMIx_Argv_join(nspaces, ',');
                PMIx_Argv_free(nspaces);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                PMIX_INFO_LIST_START(cache);
                /* get the current jobids */
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    // if the session ID was given, then ignore jobs not from that session.
                    // A job's session pointer is optional - the launch path falls back to
                    // prte_default_session where it finds none - so a job that has none
                    // belongs to no session the caller can have named
                    if (UINT32_MAX != sessionid &&
                        (NULL == jdata->session || jdata->session->session_id != sessionid)) {
                        continue;
                    }
                    PMIX_INFO_LIST_START(stack);
                    /* add the nspace name */
                    PMIX_INFO_LIST_ADD(rc, stack, PMIX_NSPACE, jdata->nspace, PMIX_STRING);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_INFO_LIST_RELEASE(stack);
                        PMIX_INFO_LIST_RELEASE(cache);
                        goto done;
                    }
                    /* add the cmd line */
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
                    if (NULL == app) {
                        ret = PMIX_ERR_NOT_FOUND;
                        PMIX_INFO_LIST_RELEASE(stack);
                        PMIX_INFO_LIST_RELEASE(cache);
                        goto done;
                    }
                    cmdline = PMIx_Argv_join(app->argv, ' ');
                    PMIX_INFO_LIST_ADD(rc, stack, PMIX_CMD_LINE, cmdline, PMIX_STRING);
                    free(cmdline);
                    /* add the job size */
                    PMIX_INFO_LIST_ADD(rc, stack, PMIX_JOB_SIZE, &jdata->num_procs, PMIX_UINT32);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_INFO_LIST_RELEASE(stack);
                        PMIX_INFO_LIST_RELEASE(cache);
                        goto done;
                    }
                    /* construct info on each process in the job */
                    for (j=0; j < jdata->procs->size; j++) {
                        proct = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, j);
                        if (NULL == proct) {
                            continue;
                        }
                        PMIX_INFO_LIST_START(plist);
                        /* add the proc's rank */
                        PMIX_INFO_LIST_ADD(rc, plist, PMIX_RANK, &proct->name.rank, PMIX_PROC_RANK);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            PMIX_INFO_LIST_RELEASE(plist);
                            PMIX_INFO_LIST_RELEASE(cache);
                            goto done;
                        }
                        /* add the proc's hostname.  A proc has no node until the
                         * mapper places it, and this loop walks every job in the
                         * array including one still being mapped - the proc table
                         * below makes the same check and this did not */
                        if (NULL != proct->node && NULL != proct->node->name) {
                            PMIX_INFO_LIST_ADD(rc, plist, PMIX_HOSTNAME, proct->node->name,
                                               PMIX_STRING);
                            if (PMIX_SUCCESS != rc) {
                                PMIX_ERROR_LOG(rc);
                                PMIX_INFO_LIST_RELEASE(stack);
                                PMIX_INFO_LIST_RELEASE(plist);
                                PMIX_INFO_LIST_RELEASE(cache);
                                goto done;
                            }
                        }
                        /* add the proc's local rank */
                        PMIX_INFO_LIST_ADD(rc, plist, PMIX_LOCAL_RANK, &proct->local_rank, PMIX_UINT16);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            PMIX_INFO_LIST_RELEASE(plist);
                            PMIX_INFO_LIST_RELEASE(cache);
                            goto done;
                        }
                        /* add to the stack */
                        PMIX_INFO_LIST_CONVERT(rc, plist, &dry);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            PMIX_INFO_LIST_RELEASE(plist);
                            PMIX_INFO_LIST_RELEASE(cache);
                            goto done;
                        }
                        PMIX_INFO_LIST_RELEASE(plist);
                        PMIX_INFO_LIST_ADD(rc, stack, PMIX_PROC_INFO_ARRAY, &dry, PMIX_DATA_ARRAY);
                        PMIX_DATA_ARRAY_DESTRUCT(&dry);
                    }
                    /* add the result to our cache */
                    PMIX_INFO_LIST_CONVERT(rc, stack, &dry);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_INFO_LIST_RELEASE(stack);
                        PMIX_INFO_LIST_RELEASE(cache);
                        goto done;
                    }
                    PMIX_INFO_LIST_RELEASE(stack);
                    PMIX_INFO_LIST_ADD(rc, cache, PMIX_JOB_INFO_ARRAY, &dry, PMIX_DATA_ARRAY);
                    PMIX_DATA_ARRAY_DESTRUCT(&dry);
                }
                /* add our findings to the results */
                PMIX_INFO_LIST_CONVERT(rc, cache, &dry);
                if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
                    // if the array is empty, then there is nothing wrong - we
                    // simply didn't find any runnning jobs
                    // otherwise, report the error and abort
                    PMIX_ERROR_LOG(rc);
                    PMIX_INFO_LIST_RELEASE(cache);
                    goto done;
                }
                PMIX_INFO_LIST_RELEASE(cache);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NAMESPACE_INFO, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                PMIx_Argv_append_nosize(&ans, PMIX_HOST);
                PMIx_Argv_append_nosize(&ans, PMIX_HOSTFILE);
                PMIx_Argv_append_nosize(&ans, PMIX_ADD_HOST);
                PMIx_Argv_append_nosize(&ans, PMIX_ADD_HOSTFILE);
                /* not a PMIx attribute: PMIx has no standard way to say
                 * "start a daemon on a node I already hold". It is still a
                 * spawn directive this DVM honors, and a tool that wants to
                 * know whether it can use it has nowhere else to ask. */
                PMIx_Argv_append_nosize(&ans, PRTE_ACTIVATE_HOSTS);
                PMIx_Argv_append_nosize(&ans, PMIX_PREFIX);
                PMIx_Argv_append_nosize(&ans, PMIX_WDIR);
                /* deliberately not PMIX_MAPPER: naming a mapping component
                 * is not a directive PRRTE can act on - the mapping policy
                 * IS the choice of mapper - and a spawn carrying it is
                 * refused, so advertising it here would be a promise we
                 * break at the next call */
                PMIx_Argv_append_nosize(&ans, PMIX_PPR);
                PMIx_Argv_append_nosize(&ans, PMIX_MAPBY);
                PMIx_Argv_append_nosize(&ans, PMIX_RANKBY);
                PMIx_Argv_append_nosize(&ans, PMIX_BINDTO);
                PMIx_Argv_append_nosize(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                tmp = PMIx_Argv_join(ans, ',');
                PMIx_Argv_free(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                PMIx_Argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                PMIx_Argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_APP);
#if PRTE_HAVE_STOP_ON_EXEC
                PMIx_Argv_append_nosize(&ans, PMIX_DEBUG_STOP_ON_EXEC);
#endif
                PMIx_Argv_append_nosize(&ans, PMIX_DEBUG_TARGET);
                /* create the return kv */
                tmp = PMIx_Argv_join(ans, ',');
                PMIx_Argv_free(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    /* get it from the v2 API */
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len,
                                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, results, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto done;
                    }
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_HWLOC_XML_V2)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len, 0)) {
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, results, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto done;
                    }
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                PMIX_INFO_LIST_ADD(rc, results, PMIX_PROC_URI, prte_process_info.my_hnp_uri, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_SERVER_URI)) {
                /* they want the PMIx URI */
                if (NULL != hostname) {
                    /* find the node object */
                    node = NULL;
                    for (k = 0; k < prte_node_pool->size; k++) {
                        ndptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, k);
                        if (NULL == ndptr) {
                            continue;
                        }
                        if (0 == strcmp(hostname, ndptr->name)) {
                            node = ndptr;
                            break;
                        }
                    }
                    if (NULL == node) {
                        /* unknown node */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    /* we want the info for the server on that node */
                    if (NULL == node->daemon) {
                        /* not found */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    proct = node->daemon;
                } else if (UINT32_MAX != nodeid) {
                    /* get the node object at that index */
                    node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, nodeid);
                    if (NULL == node) {
                        /* bad index */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    /* we want the info for the server on that node */
                    if (NULL == node->daemon) {
                        /* not found */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    proct = node->daemon;
                } else {
                    /* send them ours */
                    proct = prte_get_proc_object(PRTE_PROC_MY_NAME);
                    if (NULL == proct) {
                        ret = PMIX_ERR_NOT_FOUND;
                        goto done;
                    }
                }
                /* get the server uri value - we can block here as we are in
                 * an PRTE progress thread */
                PRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_SERVER_URI, &proct->name,
                                               (char **) &uri, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    /* the macro yields a PMIx status - it is already in the
                     * space this function returns. Running it through
                     * prte_pmix_convert_rc() converts the wrong direction
                     * and turned every failure here into PMIX_ERROR. */
                    ret = rc;
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_SERVER_URI, uri, PMIX_STRING);
                free(uri);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_PROC_TABLE)) {
                /* construct a list of values with prte_proc_info_t
                 * entries for each proc in the indicated job */
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in global proctable */
                if (0 == jdata->num_procs) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, jdata->num_procs, PMIX_PROC_INFO);
                procinfo = (pmix_proc_info_t*)dry.array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    proct = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, k);
                    if (NULL == proct) {
                        continue;
                    }
                    PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                    if (NULL != proct->node && NULL != proct->node->name) {
                        procinfo[p].hostname = strdup(proct->node->name);
                    }
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps,
                                                                             proct->app_idx);
                    if (NULL != app && NULL != app->app) {
                        if (pmix_path_is_absolute(app->app)) {
                            procinfo[p].executable_name = strdup(app->app);
                        } else {
                            procinfo[p].executable_name = pmix_os_path(false, app->cwd, app->app, NULL);
                        }
                    }
                    procinfo[p].pid = proct->pid;
                    procinfo[p].exit_code = proct->exit_code;
                    procinfo[p].state = prte_pmix_convert_state(proct->state);
                    ++p;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PROC_TABLE, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_LOCAL_PROC_TABLE)) {
                /* construct a list of values with prte_proc_info_t
                 * entries for each LOCAL proc in the indicated job */
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in local proctable */
                if (0 == jdata->num_local_procs) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, jdata->num_local_procs, PMIX_PROC_INFO);
                procinfo = (pmix_proc_info_t *) dry.array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    proct = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, k);
                    if (NULL == proct) {
                        continue;
                    }
                    if (PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_LOCAL)) {
                        PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                        if (NULL != proct->node && NULL != proct->node->name) {
                            procinfo[p].hostname = strdup(proct->node->name);
                        }
                        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps,
                                                                                 proct->app_idx);
                        if (NULL != app && NULL != app->app) {
                            if (pmix_path_is_absolute(app->app)) {
                                procinfo[p].executable_name = strdup(app->app);
                            } else {
                                procinfo[p].executable_name = pmix_os_path(false, app->cwd, app->app, NULL);
                            }
                        }
                        procinfo[p].pid = proct->pid;
                        procinfo[p].exit_code = proct->exit_code;
                        procinfo[p].state = prte_pmix_convert_state(proct->state);
                        ++p;
                    }
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_LOCAL_PROC_TABLE, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                sz = pmix_list_get_size(&prte_pmix_server_globals.psets);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                prte_pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, prte_pmix_server_pset_t)
                {
                    PMIx_Argv_append_nosize(&ans, ps->name);
                }
                if (NULL == ans) {
                    tmp = NULL;;
                } else {
                    tmp = PMIx_Argv_join(ans, ',');
                    PMIx_Argv_free(ans);
                    ans = NULL;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                if (NULL != tmp) {
                    free(tmp);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_PSET_MEMBERSHIP)) {
                prte_pmix_server_pset_t *ps, *psptr;
                /* must have provided us with a pset name qualifier */
                if (NULL == psetname) {
                    ret = PMIX_ERR_BAD_PARAM;
                    goto done;
                }
                ans = NULL;
                /* find the referenced pset */
                psptr = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, prte_pmix_server_pset_t) {
                    if (0 == strcmp(psetname, ps->name)) {
                        psptr = ps;
                        break;
                    }
                }
                if (NULL == psptr) {
                    /* we don't know that pset */
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* define the array that holds the membership - no need to allocate anything if we are careful */
                dry.array = psptr->members;
                dry.type = PMIX_PROC;
                dry.size = psptr->num_members;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PSET_MEMBERSHIP, &dry, PMIX_DATA_ARRAY);
                dry.array = NULL;  /* say no to array destructor freeing the pset members array */
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                key = jdata->num_procs;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_NUM_GROUPS)) {
                sz = pmix_list_get_size(&prte_pmix_server_globals.groups);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NUM_GROUPS, &sz, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_GROUP_NAMES)) {
                prte_pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, prte_pmix_server_pset_t)
                {
                    PMIx_Argv_append_nosize(&ans, ps->name);
                }
                tmp = PMIx_Argv_join(ans, ',');
                PMIx_Argv_free(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_GROUP_NAMES, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_GROUP_MEMBERSHIP)) {
                /* construct a list of values with pmix_proc_t
                 * entries for each proc in the indicated group */
                prte_pmix_server_pset_t *ps, *grp = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, prte_pmix_server_pset_t)
                {
                    if (PMIX_CHECK_NSPACE(ps->name, jobid)) {
                        grp = ps;
                        break;
                    }
                }
                if (NULL == grp) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in the group */
                if (0 == grp->num_members) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, grp->num_members, PMIX_PROC);
                proc = (pmix_proc_t *) dry.array;
                memcpy(proc, grp->members, grp->num_members * sizeof(pmix_proc_t));
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_GROUP_MEMBERSHIP, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_ALLOCATION)) {
                /* collect all the node info */
                void *nodelist, *nodeinfolist;
                char *str;
                prte_topology_t *topo;
                int len;

                /* slot counts and node state live only on the master */
                prc = prte_get_allocated_nodes(allocid, &alloc_nodes);
                if (PRTE_ERR_NOT_AUTHORITATIVE == prc) {
                    goto defer;
                }
                if (PRTE_SUCCESS != prc) {
                    ret = prte_pmix_convert_rc(prc);
                    goto done;
                }
                PMIX_INFO_LIST_START(nodelist);
                p = 0;
                for (k=0; k < alloc_nodes->size; k++) {
                    node = (prte_node_t*)pmix_pointer_array_get_item(alloc_nodes, k);
                    if (NULL == node) {
                        continue;
                    }
                    PMIX_INFO_LIST_START(nodeinfolist);
                    /* start with the node name */
                    PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_HOSTNAME, node->name, PMIX_STRING);
                    /* add any aliases */
                    if (NULL != node->aliases) {
                        str = PMIx_Argv_join(node->aliases, ',');
                        PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_HOSTNAME_ALIASES, str, PMIX_STRING);
                        free(str);
                    }
                    /* add the number of slots allocated on this node */
                    nslots = (uint32_t) node->slots;
                    PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_NUM_SLOTS, &nslots, PMIX_UINT32);
                    /* add topology index */
                    if (NULL != node->topology) {
                        PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_TOPOLOGY_INDEX,
                                           &node->topology->index, PMIX_INT);
                    }
                    /* convert to array */
                    PMIX_INFO_LIST_CONVERT(rc, nodeinfolist, &dry);
                    PMIX_INFO_LIST_RELEASE(nodeinfolist);
                    /* now add the entry to the main list */
                    PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_NODE_INFO, &dry, PMIX_DATA_ARRAY);
                    ++p;
                    PMIX_DATA_ARRAY_DESTRUCT(&dry);
                }
                /* add topology info */
                for (k=0; k < prte_node_topologies->size; k++) {
                    topo = (prte_topology_t*)pmix_pointer_array_get_item(prte_node_topologies, k);
                    if (NULL == topo) {
                        continue;
                    }
                    /* convert the topology to XML representation */
                    if (0 != hwloc_topology_export_xmlbuffer(topo->topo, &str, &len, 0)) {
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_HWLOC_XML_V2, str, PMIX_STRING);
                    free(str);
                }
                /* convert list to array */
                PMIX_INFO_LIST_CONVERT(rc, nodelist, &dry);
                PMIX_INFO_LIST_RELEASE(nodelist);
                /* add to results */
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_ALLOCATION, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

#ifdef PMIX_QUERY_ALLOC_IDS
            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_ALLOC_IDS)) {
                void *alloclist;

                /* the session table is the master's - a daemon holds only
                 * the default session, so it would answer "none" */
                prc = prte_get_allocation_sessions(&alloc_sessions);
                if (PRTE_ERR_NOT_AUTHORITATIVE == prc) {
                    goto defer;
                }
                if (PRTE_SUCCESS != prc) {
                    ret = prte_pmix_convert_rc(prc);
                    goto done;
                }
                PMIX_INFO_LIST_START(alloclist);
                if (NULL != alloc_sessions) {
                    for (k = 0; k < alloc_sessions->size; k++) {
                        session = (prte_session_t *) pmix_pointer_array_get_item(alloc_sessions, k);
                        if (NULL == session || NULL == session->alloc_refid) {
                            continue;
                        }
                        PMIX_INFO_LIST_ADD(rc, alloclist, PMIX_ALLOC_ID, session->alloc_refid, PMIX_STRING);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(alloclist);
                            goto done;
                        }
                    }
                }
                PMIX_INFO_LIST_CONVERT(rc, alloclist, &dry);
                PMIX_INFO_LIST_RELEASE(alloclist);
                if (PMIX_ERR_EMPTY == rc) {
                    PMIX_DATA_ARRAY_CONSTRUCT(&dry, 0, PMIX_INFO);
                } else if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_ALLOC_IDS, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
#endif

#ifdef PMIX_QUERY_ALLOC_PROPERTIES
            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_ALLOC_PROPERTIES)) {
                void *proplist;
                bool releasable;

                prc = prte_get_allocation_session(allocid, &session);
                if (PRTE_ERR_NOT_AUTHORITATIVE == prc) {
                    goto defer;
                }
                if (PRTE_SUCCESS != prc) {
                    ret = prte_pmix_convert_rc(prc);
                    goto done;
                }
                PMIX_INFO_LIST_START(proplist);
                rc = PMIX_SUCCESS;
                /* a named property selects it; naming none returns all */
                if (NULL == allocprop || PMIx_Check_key(allocprop, PMIX_ALLOC_RELEASABLE)) {
                    /* If we added the session dynamically to extend the DVM, we can release it fully */
                    releasable = PRTE_FLAG_TEST(session, PRTE_SESSION_FLAG_DYNAMIC);
                    PMIX_INFO_LIST_ADD(rc, proplist, PMIX_ALLOC_RELEASABLE, &releasable, PMIX_BOOL);
                }
                if (PMIX_SUCCESS == rc &&
                    (NULL == allocprop || PMIx_Check_key(allocprop, PMIX_ALLOC_SEQUENCE))) {
                    PMIX_INFO_LIST_ADD(rc, proplist, PMIX_ALLOC_SEQUENCE,
                                       &session->acquisition, PMIX_UINT32);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_INFO_LIST_RELEASE(proplist);
                    goto done;
                }
                PMIX_INFO_LIST_CONVERT(rc, proplist, &dry);
                PMIX_INFO_LIST_RELEASE(proplist);
                if (PMIX_ERR_EMPTY == rc) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_ALLOC_PROPERTIES, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
#endif

            } else if (PMIx_Check_key(q->keys[n], PMIX_NUM_SLOTS)) {
                /* the slots allocated to a specific node, if one was named,
                 * otherwise the total across the allocation
                 */
                /* every arm below reads node->slots, which the nidmap does
                 * not ship - so settle authority once, before any of them */
                prc = prte_get_allocated_nodes(allocid, &alloc_nodes);
                if (PRTE_ERR_NOT_AUTHORITATIVE == prc) {
                    goto defer;
                }
                if (PRTE_SUCCESS != prc) {
                    ret = prte_pmix_convert_rc(prc);
                    goto done;
                }
                if (NULL != hostname) {
                    node = prte_node_match(NULL, hostname);
                    if (NULL == node) {
                        ret = PMIX_ERR_NOT_FOUND;
                        goto done;
                    }
                    nslots = (uint32_t) node->slots;
                } else if (UINT32_MAX != nodeid) {
                    /* a nodeid is a slot in the DVM-wide pool, not in the
                     * allocation the qualifier named */
                    prc = prte_get_allocated_nodes(NULL, &dvm_nodes);
                    if (PRTE_SUCCESS != prc) {
                        ret = prte_pmix_convert_rc(prc);
                        goto done;
                    }
                    node = (prte_node_t *) pmix_pointer_array_get_item(dvm_nodes, nodeid);
                    if (NULL == node) {
                        ret = PMIX_ERR_NOT_FOUND;
                        goto done;
                    }
                    nslots = (uint32_t) node->slots;
                } else {
                    nslots = 0;
                    for (k = 0; k < alloc_nodes->size; k++) {
                        node = (prte_node_t *) pmix_pointer_array_get_item(alloc_nodes, k);
                        if (NULL == node) {
                            continue;
                        }
                        nslots += (uint32_t) node->slots;
                    }
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_NUM_SLOTS, &nslots, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_AVAILABLE_SLOTS)) {
                /* compute the slots currently available for assignment. Note that
                 * this is purely a point-in-time measurement as jobs may be working
                 * there way thru the state machine for mapping, and more jobs may
                 * be submitted at any moment.
                 */
                /* slots, slots_inuse, slots_max and node state are all
                 * master-only - a daemon would report every node as empty
                 * and every filter as inert */
                prc = prte_get_allocated_nodes(NULL, &alloc_nodes);
                if (PRTE_ERR_NOT_AUTHORITATIVE == prc) {
                    goto defer;
                }
                if (PRTE_SUCCESS != prc) {
                    ret = prte_pmix_convert_rc(prc);
                    goto done;
                }
                nslots = 0;
                for (k=0; k < alloc_nodes->size; k++) {
                    node = (prte_node_t*)pmix_pointer_array_get_item(alloc_nodes, k);
                    if (NULL == node) {
                        continue;
                    }
                    /* ignore nodes that are non-usable */
                    if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
                        continue;
                    }
                    // ignore nodes that are down
                    if (PRTE_NODE_STATE_DOWN == node->state) {
                        continue;
                    }
                    // ignore nodes that are at/above max
                    if (0 != node->slots_max && node->slots_inuse >= node->slots_max) {
                        continue;
                    }
                    /* if the hnp was not allocated, then ignore it here */
                    if (!prte_hnp_is_allocated && 0 == node->index) {
                            continue;
                    }
                    // ignore oversubscribed nodes
                    if (node->slots <= node->slots_inuse) {
                        continue;
                    }
                    nslots += (uint32_t)(node->slots - node->slots_inuse);
                }
                /* the count is handed to PMIx by address, so it has to be the
                 * width PMIx is told it is - a size_t read as a PMIX_UINT32
                 * takes the correct four bytes only on a little-endian machine */
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_AVAILABLE_SLOTS, &nslots, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_MEM_ALLOC_KIND)) {
                pmix_proc_t pproc;
                pmix_info_t info;
                pmix_value_t *value;
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                PMIX_LOAD_PROCID(&pproc, jobid, PMIX_RANK_WILDCARD);
                PMIX_INFO_LOAD(&info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
                ret = PMIx_Get(&pproc, PMIX_MEM_ALLOC_KIND, &info, 1, &value);
                if (PMIX_SUCCESS != ret) {
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_MEM_ALLOC_KIND, value->data.string, PMIX_STRING);
                PMIX_VALUE_RELEASE(value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_RESOLVE_PEERS)) {
                char *nm;
                pmix_list_t procs;
                int idx;
                prte_proc_t *p2;
                bool found = false;
                // must at least have given us a hostname
                if (NULL == hostname) {
                    ret = PMIX_ERR_BAD_PARAM;
                    goto done;
                }
                /* does the name refer to me? */
                if (prte_check_host_is_local(hostname)) {
                    nm = prte_process_info.nodename;
                } else {
                    nm = hostname;
                }
                PMIX_CONSTRUCT(&procs, pmix_list_t);
                // could ask for info on all jobs, so have to check for that case
                for (k = 1; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    if (NULL == jdata->map) {
                        continue;
                    }
                    if (!PMIX_NSPACE_INVALID(jobid) &&
                        !PMIX_CHECK_NSPACE(jobid, jdata->nspace)) {
                        continue;
                    }
                    // see if this job has any procs on the indicated node
                    for (j=0; j < jdata->map->nodes->size; j++) {
                        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, j);
                        if (NULL == node) {
                            continue;
                        }
                        if (0 != strcmp(node->name, nm)) {
                            if (NULL == node->aliases) {
                                continue;
                            }
                            found = false;
                            for (p = 0; NULL != node->aliases[p]; p++) {
                                if (0 == strcmp(nm, node->aliases[p])) {
                                    /* this is the node! */
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                continue;
                            }
                        }
                        // we want the procs from this node
                        for (idx=0; idx < node->procs->size; idx++) {
                            proct = (prte_proc_t*)pmix_pointer_array_get_item(node->procs, idx);
                            if (NULL == proct) {
                                continue;
                            }
                            if (PMIX_CHECK_NSPACE(jdata->nspace, proct->name.nspace)) {
                                pmix_list_append(&procs, &proct->super);
                            }
                        }
                    }
                }
                sz = pmix_list_get_size(&procs);
                if (0 < sz) {
                    PMIX_PROC_CREATE(proc, sz);
                    idx = 0;
                    PMIX_LIST_FOREACH_SAFE(proct, p2, &procs, prte_proc_t) {
                        memcpy(&proc[idx], &proct->name, sizeof(pmix_proc_t));
                        pmix_list_remove_item(&procs, &proct->super);
                        ++idx;
                    }
                } else {
                    proc = NULL;
                }
                dry.type = PMIX_PROC;
                dry.array = proc;
                dry.size = sz;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_RESOLVE_PEERS, &dry, PMIX_DATA_ARRAY);
                if (NULL != proc) {
                    free(proc);
                }
                PMIX_DESTRUCT(&procs);

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_RESOLVE_NODE)) {
                char **nodes = NULL, *nodelist;
                // could ask for info on all jobs, so have to check for that case
                for (k = 1; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    if (NULL == jdata->map) {
                        continue;
                    }
                    if (!PMIX_NSPACE_INVALID(jobid) &&
                        !PMIX_CHECK_NSPACE(jobid, jdata->nspace)) {
                        continue;
                    }
                    // assemble the nodes
                    for (j=0; j < jdata->map->nodes->size; j++) {
                        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, j);
                        if (NULL == node) {
                            continue;
                        }
                        PMIx_Argv_append_unique_nosize(&nodes, node->name);
                    }
                }
                nodelist = PMIx_Argv_join(nodes, ',');
                PMIx_Argv_free(nodes);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_RESOLVE_NODE, nodelist, PMIX_STRING);
                if (NULL != nodelist) {
                    free(nodelist);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_PROC_RESOURCE_USAGE)) {

            } else if (PMIx_Check_key(q->keys[n], PMIX_QUERY_NODE_RESOURCE_USAGE)) {


            } else {
                pmix_output_verbose(2, prte_pmix_server_globals.output,
                                    "%s Query for unrecognized attribute: %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    PMIx_Get_attribute_name(q->keys[n]));
                ret = PMIX_ERR_NOT_SUPPORTED;
                goto done;
            }
            continue;

        defer:
            /* this key wants state only the master holds - see the block
             * comment above query_complete().  The array was sized to hold
             * every key, so this cannot overrun. */
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s deferring key %s to the DVM master",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->keys[n]);
            defer_key(dq, &ndq, q, q->keys[n]);
        } // for
    }     // for

done:
    if (0 < ndq) {
        if (PMIX_SUCCESS == ret) {
            rc = relay_query(cd, results, nkeys, dq, ndq);
            if (PMIX_SUCCESS == rc) {
                /* the client is answered when the master replies */
                PMIX_QUERY_FREE(dq, nq_alloc);
                return;
            }
            /* we never reached the master, so say why rather than reporting
             * the empty result as "not found" */
            ret = rc;
        }
    }
    if (NULL != dq) {
        PMIX_QUERY_FREE(dq, nq_alloc);
    }
    query_complete(cd, results, nkeys, ret);
}


pmix_status_t pmix_server_query_fn(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    if (NULL == queries || NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proct, proct, sizeof(pmix_proc_t));
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _query, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

/* ---- the master's half of a relayed query ---------------------------- */

/* Ship the master's answer back to the daemon that asked.  Runs on the PRRTE
 * progress thread: query_complete() invokes it from _query, which is itself
 * an event handler, so there is nothing to thread-shift. */
static void send_query_resp(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                            void *cbdata, pmix_release_cbfunc_t release_fn,
                            void *release_cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) req->qcaddy;
    pmix_data_buffer_t *buf;
    pmix_status_t rc;
    int ret;

    PMIX_DATA_BUFFER_CREATE(buf);
    rc = PMIx_Data_pack(NULL, buf, &status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto cleanup;
    }
    /* the asking daemon's room number, not ours */
    rc = PMIx_Data_pack(NULL, buf, &req->remote_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto cleanup;
    }
    rc = PMIx_Data_pack(NULL, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto cleanup;
    }
    if (0 < ninfo) {
        rc = PMIx_Data_pack(NULL, buf, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto cleanup;
        }
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s sending query response to %s for their req %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&req->proxy), req->remote_index);
    PRTE_RML_RELIABLE_SEND(ret, req->proxy.rank, buf, PRTE_RML_TAG_QUERY_RESP);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }

cleanup:
    /* every exit comes through here - a pack failure that returned early
     * would strand the asking daemon's client for the life of the DVM */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    /* the queries were unpacked into this caddy and are ours to free; the
     * caddy itself is released by query_complete() as we return */
    if (NULL != cd && NULL != cd->queries) {
        PMIX_QUERY_FREE(cd->queries, cd->nqueries);
        cd->queries = NULL;
        cd->nqueries = 0;
    }
    req->qcaddy = NULL;
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* A daemon could not answer some keys and has sent them here.  Answer them
 * with the very same _query() the daemon ran - on the master the accessors
 * always succeed, so nothing defers and it completes locally. */
void pmix_server_query_request(int status, pmix_proc_t *sender,
                               pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    prte_pmix_server_req_t *req;
    pmix_query_t *queries = NULL;
    pmix_proc_t source;
    size_t nqueries = 0;
    pmix_status_t rc;
    int32_t cnt;
    int refid;
    PRTE_HIDE_UNUSED_PARAMS(status, tg, cbdata);

    /* unpack the asking daemon's room number first - with it we can at least
     * send back an error it can match to a waiting client */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &refid, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nqueries, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 == nqueries) {
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }
    PMIX_QUERY_CREATE(queries, nqueries);
    cnt = nqueries;
    rc = PMIx_Data_unpack(NULL, buffer, queries, &cnt, PMIX_QUERY);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_QUERY_FREE(queries, nqueries);
        queries = NULL;
        goto error;
    }

    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "QUERY");
    req->remote_index = refid;
    PMIX_PROC_LOAD(&req->proxy, sender->nspace, sender->rank);
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    /* answer about the job of the client that originally asked, not ours */
    memcpy(&cd->proct, &source, sizeof(pmix_proc_t));
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = send_query_resp;
    cd->cbdata = req;
    req->qcaddy = cd;

    /* post it rather than calling inline: _query reaches back into the RML
     * to answer, and we are still inside the RML's dispatch */
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _query, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return;

error:
    /* the daemon is holding a tracker under refid and nothing else will ever
     * complete it - an error reply is what releases its client */
    {
        pmix_data_buffer_t *reply;
        size_t none = 0;
        int ret;

        PMIX_DATA_BUFFER_CREATE(reply);
        /* the empty info count keeps this reply the same shape as a real
         * one, so the receiver unpacks one format rather than guessing
         * which it got from the status */
        if (PMIX_SUCCESS != PMIx_Data_pack(NULL, reply, &rc, 1, PMIX_STATUS) ||
            PMIX_SUCCESS != PMIx_Data_pack(NULL, reply, &refid, 1, PMIX_INT) ||
            PMIX_SUCCESS != PMIx_Data_pack(NULL, reply, &none, 1, PMIX_SIZE)) {
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        PRTE_RML_RELIABLE_SEND(ret, sender->rank, reply, PRTE_RML_TAG_QUERY_RESP);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(reply);
        }
    }
}

/* ---- the asking daemon's half ---------------------------------------- */

/* The master has answered the keys we deferred.  Merge its results into the
 * ones we gathered locally and complete the client's query - it asked once
 * and gets one answer covering every key. */
void pmix_server_query_resp(int status, pmix_proc_t *sender,
                            pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_req_t *req;
    prte_pmix_server_op_caddy_t *cd;
    pmix_info_t *info = NULL;
    size_t ninfo = 0, n;
    pmix_status_t ret, rc;
    int32_t cnt;
    int req_index;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* the status is already a PMIx value - it is the one we report */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

    /* fall through on the above in the hope the room number still unpacks,
     * which is the only thing that lets us answer the waiting client */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &req_index, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    req = pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, req_index);
    if (NULL == req || NULL == req->qcaddy) {
        /* the index came off the wire, but it is OUR index echoed back, so a
         * miss means the request was retired early and whoever waits on it
         * waits forever.  Say so rather than returning in silence. */
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s query response names local req %d, which is gone",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req_index);
        return;
    }
    cd = (prte_pmix_server_op_caddy_t *) req->qcaddy;

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        ninfo = 0;
        goto complete;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            ret = rc;
            ninfo = 0;
            goto complete;
        }
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_LIST_XFER(rc, req->qresults, &info[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                ret = rc;
                break;
            }
        }
        PMIX_INFO_FREE(info, ninfo);
    }

complete:
    /* query_complete() owns the list and the caddy from here.  "Nothing to
     * report" from the master is not the answer to the client's whole
     * request - keys this daemon answered are in the list - so hand those
     * two statuses back to the accounting there, which weighs the merged
     * result against every key that was asked for and lands on partial
     * success or, if the list really is empty, not-found.  Any other error
     * says something specific went wrong and is reported as it stands. */
    if (PMIX_QUERY_PARTIAL_SUCCESS == ret || PMIX_ERR_NOT_FOUND == ret) {
        ret = PMIX_SUCCESS;
    }
    query_complete(cd, req->qresults, req->nkeys, ret);
    req->qcaddy = NULL;
    req->qresults = NULL;
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);
    PMIX_RELEASE(req);
}
