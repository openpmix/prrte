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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include <sys/types.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"

#include "src/runtime/prte_globals.h"

/*
 * JOB
 * NOTE: We do not pack all of the job object's fields as many of them have no
 * value in sending them to another location. The only purpose in packing and
 * sending a job object is to communicate the data required to dynamically
 * spawn another job - so we only pack that limited set of required data.
 * Therefore, only unpack what was packed
 */

/* Decode one map written by pack_map() back into its argv. */
static int unpack_map(pmix_data_buffer_t *bkt, char delim, char ***list)
{
    pmix_status_t rc;
    int32_t n = 1;
    char *joined = NULL;

#if PRTE_PMIX_HAVE_REGEX2
    {
        pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;
        rc = PMIx_Data_unpack(NULL, bkt, &regex, &n, PMIX_REGEX2);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_parse_regex2(&regex, NULL, 0, &joined);
        PMIx_Regex2_destruct(&regex);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }
#else
    rc = PMIx_Data_unpack(NULL, bkt, &joined, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
#endif
    if (NULL == joined) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    *list = PMIx_Argv_split(joined, delim);
    free(joined);
    if (NULL == *list) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    return PRTE_SUCCESS;
}

/* Get (creating if need be) the proc holding a given rank of this job. */
static prte_proc_t *get_proc(prte_job_t *jptr, pmix_rank_t rank)
{
    prte_proc_t *proc;

    proc = (prte_proc_t *) pmix_pointer_array_get_item(jptr->procs, (int) rank);
    if (NULL == proc) {
        proc = PMIX_NEW(prte_proc_t);
        if (NULL == proc) {
            return NULL;
        }
        PMIX_LOAD_PROCID(&proc->name, jptr->nspace, rank);
        pmix_pointer_array_set_item(jptr->procs, (int) rank, proc);
    }
    return proc;
}

static int rank_cmp(const void *a, const void *b)
{
    pmix_rank_t x = *(const pmix_rank_t *) a;
    pmix_rank_t y = *(const pmix_rank_t *) b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static int unpack_layout(pmix_data_buffer_t *bkt, prte_job_t *jptr)
{
    int32_t nnodes, n, cnt = 1;
    int rc, a, i, j;
    char **nodenames = NULL, **fields = NULL, **ranks = NULL;
    prte_node_t **nodes = NULL, *nd;
    prte_local_rank_t *lranks = NULL;
    pmix_rank_t *applist = NULL;
    size_t napp;
    prte_proc_t *proc;
    prte_app_context_t *app;
    pmix_status_t prc;

    prc = PMIx_Data_unpack(NULL, bkt, &nnodes, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != prc) {
        PMIX_ERROR_LOG(prc);
        return prte_pmix_convert_status(prc);
    }
    if (0 == nnodes) {
        /* an unmapped job - a spawn request that has not been through
         * rmaps yet - carries no placement at all */
        return PRTE_SUCCESS;
    }

    rc = unpack_map(bkt, ',', &nodenames);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    if (nnodes != PMIx_Argv_count(nodenames)) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        rc = PRTE_ERR_BAD_PARAM;
        goto cleanup;
    }
    /* resolve each name against the node pool, which the nidmap filled in
     * before any of this arrived */
    nodes = (prte_node_t **) calloc(nnodes, sizeof(prte_node_t *));
    lranks = (prte_local_rank_t *) calloc(nnodes, sizeof(prte_local_rank_t));
    if (NULL == nodes || NULL == lranks) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }
    for (n = 0; n < nnodes; n++) {
        /* a NULL list means "search the global node pool" */
        nodes[n] = prte_node_match(NULL, nodenames[n]);
        if (NULL == nodes[n]) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }
    }

    /* one proc map per app, in the order the apps were packed */
    for (a = 0; a < jptr->apps->size; a++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jptr->apps, a);
        if (NULL == app) {
            continue;
        }
        rc = unpack_map(bkt, ';', &fields);
        if (PRTE_SUCCESS != rc) {
            goto cleanup;
        }
        if (nnodes != PMIx_Argv_count(fields)) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            rc = PRTE_ERR_BAD_PARAM;
            goto cleanup;
        }
        napp = 0;
        applist = (pmix_rank_t *) calloc(jptr->num_procs, sizeof(pmix_rank_t));
        if (NULL == applist) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto cleanup;
        }
        for (n = 0; n < nnodes; n++) {
            if (0 == strcmp(fields[n], "-")) {
                continue;
            }
            ranks = PMIx_Argv_split(fields[n], ',');
            for (i = 0; NULL != ranks && NULL != ranks[i]; i++) {
                pmix_rank_t rank = (pmix_rank_t) strtoul(ranks[i], NULL, 10);
                if (jptr->num_procs <= rank) {
                    PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                    rc = PRTE_ERR_BAD_PARAM;
                    goto cleanup;
                }
                proc = get_proc(jptr, rank);
                if (NULL == proc) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto cleanup;
                }
                proc->app_idx = app->idx;
                nd = nodes[n];
                if (NULL == nd->daemon) {
                    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                    rc = PRTE_ERR_NOT_FOUND;
                    goto cleanup;
                }
                proc->parent = nd->daemon->name.rank;
                /* apps outermost, node->procs order within: the same walk
                 * compute_local_rank() makes */
                proc->local_rank = lranks[n]++;
                applist[napp++] = rank;
            }
            PMIx_Argv_free(ranks);
            ranks = NULL;
        }
        PMIx_Argv_free(fields);
        fields = NULL;
        /* app ranks are assigned in ascending rank order */
        if (0 < napp) {
            qsort(applist, napp, sizeof(pmix_rank_t), rank_cmp);
            for (i = 0; (size_t) i < napp; i++) {
                proc = (prte_proc_t *) pmix_pointer_array_get_item(jptr->procs,
                                                                   (int) applist[i]);
                if (NULL != proc) {
                    proc->app_rank = (pmix_rank_t) i;
                }
            }
        }
        free(applist);
        applist = NULL;
    }

    /* and now what the maps could not say, in rank order */
    for (j = 0; j < (int) jptr->num_procs; j++) {
        proc = (prte_proc_t *) pmix_pointer_array_get_item(jptr->procs, j);
        if (NULL == proc) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }
        rc = prte_proc_unpack(bkt, proc);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    rc = PRTE_SUCCESS;

cleanup:
    if (NULL != nodenames) {
        PMIx_Argv_free(nodenames);
    }
    if (NULL != fields) {
        PMIx_Argv_free(fields);
    }
    if (NULL != ranks) {
        PMIx_Argv_free(ranks);
    }
    if (NULL != nodes) {
        free(nodes);
    }
    if (NULL != lranks) {
        free(lranks);
    }
    if (NULL != applist) {
        free(applist);
    }
    return rc;
}

int prte_job_unpack(pmix_data_buffer_t *bkt, prte_job_t **job)
{
    int rc;
    int32_t k, n, count, bookmark;
    prte_job_t *jptr;
    prte_app_idx_t j;
    prte_attribute_t *kv;
    char *tmp;
    prte_info_item_t *val;
    pmix_info_t pval;
    pmix_list_t *cache;

    /* create the prte_job_t object */
    jptr = PMIX_NEW(prte_job_t);
    if (NULL == jptr) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* unpack the nspace */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->nspace, &n, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the flags */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->flags, &n, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the attributes */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        kv = PMIX_NEW(prte_attribute_t);
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &kv->key, &n, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(jptr);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_unpack(NULL, bkt, &kv->data, &n, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(jptr);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        kv->local = PRTE_ATTR_GLOBAL; // obviously not a local value
        pmix_list_append(&jptr->attributes, &kv->super);
    }
    /* unpack any job info */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    if (0 < count) {
        cache = PMIX_NEW(pmix_list_t);
        prte_set_attribute(&jptr->attributes, PRTE_JOB_INFO_CACHE, PRTE_ATTR_LOCAL, (void *) cache,
                           PMIX_POINTER);
        for (k = 0; k < count; k++) {
            n = 1;
            rc = PMIx_Data_unpack(NULL, bkt, &pval, &n, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(jptr);
                return prte_pmix_convert_status(rc);
            }
            val = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_XFER(&val->info, &pval);
            PMIX_INFO_DESTRUCT(&pval);
            pmix_list_append(cache, &val->super);
        }
    }

    /* unpack the personality */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &tmp, &n, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(jptr);
            return prte_pmix_convert_status(rc);
        }
        PMIx_Argv_append_nosize(&jptr->personality, tmp);
        free(tmp);
    }

    /* unpack the num apps */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->num_apps, &n, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    /* if there are apps, unpack them */
    if (0 < jptr->num_apps) {
        prte_app_context_t *app;
        for (j = 0; j < jptr->num_apps; j++) {
            n = 1;
            rc = prte_app_unpack(bkt, &app);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(jptr);
                return prte_pmix_convert_status(rc);
            }
            pmix_pointer_array_add(jptr->apps, app);
        }
    }

    /* unpack num procs and offset */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->num_procs, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->offset, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* Rebuild the placement from the maps.  See prte_job_pack for the
     * format; what follows is why each derived field is derivable.
     *
     *  rank       - it IS the map: the proc maps list ranks by node.
     *  parent     - the node's daemon, which the node pool already holds.
     *  app_idx    - which app's proc map the rank appeared in.
     *  app_rank   - compute_app_rank() walks jdata->procs, which is indexed
     *               by rank, so an app's ranks take app ranks in ascending
     *               rank order.  We see them in node order, so they are
     *               sorted before being numbered.
     *  local_rank - compute_local_rank() walks each node's procs in array
     *               order, apps outermost, which is exactly the order the
     *               packer emitted them in.  (The one function that could
     *               have broken that correspondence, update_local_ranks(),
     *               had no callers and is gone.)
     */
    rc = unpack_layout(bkt, jptr);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return rc;
    }

    /* unpack stdin target */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->stdin_target, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the total slots allocated to the job */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->total_slots_alloc, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* if the map is NULL, then we didn't pack it as there was
     * nothing to pack. Instead, we packed a flag to indicate whether or not
     * the map is included */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &j, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    if (0 < j) {
        /* unpack the map */
        n = 1;
        rc = prte_map_unpack(bkt, &(jptr->map));
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(jptr);
            return prte_pmix_convert_status(rc);
        }
    }

    /* unpack the bookmark */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &bookmark, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }
    if (0 <= bookmark) {
        /* retrieve it */
        jptr->bookmark = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, bookmark);
    }

    /* unpack the job state */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->state, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the launcher ID */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &jptr->launcher, &n, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(jptr);
        return prte_pmix_convert_status(rc);
    }

    *job = jptr;
    return PRTE_SUCCESS;
}

/*
 * NODE
 */
int prte_node_unpack(pmix_data_buffer_t *bkt, prte_node_t **nd)
{
    pmix_status_t rc;
    int32_t n, k, count;
    prte_node_t *node;
    uint8_t flag;
    prte_attribute_t *kv;

    /* create the node object */
    node = PMIX_NEW(prte_node_t);
    if (NULL == node) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* unpack the node name */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &node->name, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(node);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the number of procs on the node */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &node->num_procs, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(node);
        return prte_pmix_convert_status(rc);
    }

    /* unpack whether we are oversubscribed */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &flag, &n, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(node);
        return prte_pmix_convert_status(rc);
    }
    if (flag) {
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
    }

    /* unpack the state */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &node->state, &n, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(node);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the attributes */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(node);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        kv = PMIX_NEW(prte_attribute_t);
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &kv->key, &n, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(node);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_unpack(NULL, bkt, &kv->data, &n, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(node);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        kv->local = PRTE_ATTR_GLOBAL; // obviously not a local value
        pmix_list_append(&node->attributes, &kv->super);
    }
    *nd = node;
    return PRTE_SUCCESS;
}

/*
 * PROC
 */
int prte_proc_unpack(pmix_data_buffer_t *bkt, prte_proc_t *proc)
{
    pmix_status_t rc;
    int32_t n, count, k;
    prte_attribute_t *kv;

    /* Everything the job's maps already say has been set on this proc by
     * prte_job_unpack before we are called - its rank, its hosting daemon,
     * its app, its app rank and its local rank.  What follows is only what
     * the maps cannot say.  The proc belongs to the job, not to us: an
     * error here leaves it for prte_job_unpack to release along with
     * everything else it built. */

    /* unpack the node rank */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &proc->node_rank, &n, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the state */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &proc->state, &n, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the cpuset */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &proc->cpuset, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the attributes */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        kv = PMIX_NEW(prte_attribute_t);
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &kv->key, &n, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_unpack(NULL, bkt, &kv->data, &n, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        kv->local = PRTE_ATTR_GLOBAL; // obviously not a local value
        pmix_list_append(&proc->attributes, &kv->super);
    }
    return PRTE_SUCCESS;
}

/*
 * APP_CONTEXT
 */
int prte_app_unpack(pmix_data_buffer_t *bkt, prte_app_context_t **ap)
{
    int rc;
    prte_app_context_t *app;
    int32_t n, count, k;
    prte_attribute_t *kv;
    char *tmp;

    /* create the app_context object */
    app = PMIX_NEW(prte_app_context_t);
    if (NULL == app) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* get the app index number */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &app->idx, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the application name */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &app->app, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* get the number of processes */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &app->num_procs, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* get the first rank for this app */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &app->first_rank, &n, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* get the number of argv strings that were packed */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &tmp, &n, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(app);
            return prte_pmix_convert_status(rc);
        }
        PMIx_Argv_append_nosize(&app->argv, tmp);
        free(tmp);
    }

    /* get the number of env strings */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &tmp, &n, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(app);
            return prte_pmix_convert_status(rc);
        }
        PMIx_Argv_append_nosize(&app->env, tmp);
        free(tmp);
    }

    /* unpack the cwd */
    rc = PMIx_Data_unpack(NULL, bkt, &app->cwd, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* get the flags */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &app->flags, &n, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the attributes */
    rc = PMIx_Data_unpack(NULL, bkt, &count, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(app);
        return prte_pmix_convert_status(rc);
    }
    for (k = 0; k < count; k++) {
        kv = PMIX_NEW(prte_attribute_t);
        n = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &kv->key, &n, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(app);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_unpack(NULL, bkt, &kv->data, &n, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(app);
            PMIX_RELEASE(kv);
            return prte_pmix_convert_status(rc);
        }
        kv->local = PRTE_ATTR_GLOBAL; // obviously not a local value
        pmix_list_append(&app->attributes, &kv->super);
    }
    *ap = app;
    return PRTE_SUCCESS;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map - hence, we do not pack that field, so don't unpack it here
 */
int prte_map_unpack(pmix_data_buffer_t *bkt, struct prte_job_map_t **mp)
{
    int rc;
    int32_t n;
    prte_job_map_t *map;

    /* create the prte_rmaps_base_map_t object */
    map = PMIX_NEW(prte_job_map_t);
    if (NULL == map) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* unpack the policies - see prte_map_pack(): no mapper name is sent */
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &map->mapping, &n, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(map);
        return prte_pmix_convert_status(rc);
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &map->ranking, &n, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(map);
        return prte_pmix_convert_status(rc);
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &map->binding, &n, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(map);
        return prte_pmix_convert_status(rc);
    }

    /* unpack the number of nodes involved in the job */
    n = 1;
    n = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &map->num_nodes, &n, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(map);
        return prte_pmix_convert_status(rc);
    }

    *mp = map;
    return PRTE_SUCCESS;
}
