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

#include "src/class/pmix_pointer_array.h"
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
 * spawn another job - so we only pack that limited set of required data
 */

/* Encode a list of strings as a single compressed value.
 *
 * The delimiter is the caller's, not the encoder's: PMIx_generate_regex2
 * replaced PMIx_generate_ppn precisely because a per-node rank map and a
 * node map differ only in what character joins their fields.
 */
static int pack_map(pmix_data_buffer_t *bkt, char **list, char delim)
{
    pmix_status_t rc;
    char *joined;

    joined = PMIx_Argv_join(list, delim);
    if (NULL == joined) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
#if PRTE_PMIX_HAVE_REGEX2
    {
        pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;
        rc = PMIx_generate_regex2(joined, NULL, 0, &regex);
        free(joined);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_pack(NULL, bkt, &regex, 1, PMIX_REGEX2);
        PMIx_Regex2_destruct(&regex);
    }
#else
    rc = PMIx_Data_pack(NULL, bkt, &joined, 1, PMIX_STRING);
    free(joined);
#endif
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

/* One app's proc map: for each node of the job's map, in map order, the
 * ranks of that app resident on it.  A node where this app has no procs
 * contributes "-" rather than an empty field, so the map stays aligned with
 * the node map - an empty field would be dropped by the argv split at the
 * far end and silently shift every later node's ranks onto the wrong node.
 *
 * The per-node walk is node->procs in array order, which is exactly the
 * order compute_local_rank() assigns local ranks in, and the apps are walked
 * in the same order there and here.  That is what makes local_rank
 * derivable rather than merely guessable.
 */
static int pack_proc_map(pmix_data_buffer_t *bkt, prte_job_t *job, prte_app_idx_t idx)
{
    char **fields = NULL, **micro;
    char *tmp;
    prte_node_t *node;
    prte_proc_t *proc;
    int n, m, rc;

    for (n = 0; n < job->map->nodes->size; n++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(job->map->nodes, n);
        if (NULL == node) {
            continue;
        }
        micro = NULL;
        for (m = 0; m < node->procs->size; m++) {
            proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, m);
            if (NULL == proc) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(job->nspace, proc->name.nspace)) {
                continue;
            }
            if (proc->app_idx != idx) {
                continue;
            }
            PMIx_Argv_append_nosize(&micro, PRTE_VPID_PRINT(proc->name.rank));
        }
        if (NULL == micro) {
            PMIx_Argv_append_nosize(&fields, "-");
        } else {
            tmp = PMIx_Argv_join(micro, ',');
            PMIx_Argv_free(micro);
            PMIx_Argv_append_nosize(&fields, tmp);
            free(tmp);
        }
    }
    rc = pack_map(bkt, fields, ';');
    PMIx_Argv_free(fields);
    return rc;
}

int prte_job_pack(pmix_data_buffer_t *bkt, prte_job_t *job, prte_job_pack_mode_t mode)
{
    pmix_status_t rc;
    int32_t j, count, bookmark;
    prte_app_context_t *app;
    prte_proc_t *proc;
    prte_attribute_t *kv;
    pmix_list_t *cache;
    prte_info_item_t *val;
    prte_node_t *nptr;

    /* Lead with what this buffer contains.  The per-proc records are not
     * always the same shape - the launch path scatters the cpusets - and the
     * decoder has to know which one it is looking at before it reads the
     * first of them.  One byte, once per job, ahead of everything else. */
    rc = PMIx_Data_pack(NULL, bkt, &mode, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the nspace */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    /* pack the flags */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->flags, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the attributes that need to be sent */
    count = 0;
    PMIX_LIST_FOREACH(kv, &job->attributes, prte_attribute_t)
    {
        if (PRTE_ATTR_GLOBAL == kv->local) {
            ++count;
        }
    }
    rc = PMIx_Data_pack(NULL, bkt, (void *) &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    PMIX_LIST_FOREACH(kv, &job->attributes, prte_attribute_t)
    {
        if (PRTE_ATTR_GLOBAL == kv->local) {
            rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->key, 1, PMIX_UINT16);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return prte_pmix_convert_status(rc);
            }
            rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->data, 1, PMIX_VALUE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return prte_pmix_convert_status(rc);
            }
        }
    }
    /* check for job info attribute */
    cache = NULL;
    if (prte_get_attribute(&job->attributes, PRTE_JOB_INFO_CACHE, (void **) &cache, PMIX_POINTER)
        && NULL != cache) {
        /* we need to pack these as well, but they are composed
         * of prte_info_item_t's on a list. So first pack the number
         * of list elements */
        count = pmix_list_get_size(cache);
        rc = PMIx_Data_pack(NULL, bkt, (void *) &count, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        /* now pack each element on the list */
        PMIX_LIST_FOREACH(val, cache, prte_info_item_t)
        {
            rc = PMIx_Data_pack(NULL, bkt, (void *) &val->info, 1, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return prte_pmix_convert_status(rc);
            }
        }
    } else {
        /* pack a zero to indicate no job info is being passed */
        count = 0;
        rc = PMIx_Data_pack(NULL, bkt, (void *) &count, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the personality */
    count = PMIx_Argv_count(job->personality);
    rc = PMIx_Data_pack(NULL, bkt, (void *) &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    for (j = 0; j < count; j++) {
        rc = PMIx_Data_pack(NULL, bkt, (void *) &job->personality[j], 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the number of apps */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->num_apps, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* if there are apps, pack the app_contexts */
    if (0 < job->num_apps) {
        for (j = 0; j < job->apps->size; j++) {
            if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(job->apps, j))) {
                continue;
            }
            rc = prte_app_pack(bkt, app);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return prte_pmix_convert_status(rc);
            }
        }
    }

    /* pack the number of procs and offset */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->num_procs, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->offset, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* The placement, as maps rather than as a per-proc array.
     *
     * The node map names the nodes this job is mapped onto, in map order.
     * One proc map per app then gives, for each of those nodes in the same
     * order, the ranks of that app resident on it - "-" where the app has
     * none there, so the two lists stay in step.  Between them they carry
     * every proc's rank, its hosting daemon, its app, its app rank and its
     * local rank, in a form that compresses with the number of NODES rather
     * than growing with the number of processes.  See prte_job_unpack for
     * why each of those five is derivable.
     *
     * An unmapped job - a spawn request on its way to the HNP, which has not
     * been through rmaps yet - packs a zero here and nothing else. */
    if (NULL == job->map || 0 == job->map->num_nodes || 0 == job->num_procs) {
        j = 0;
        rc = PMIx_Data_pack(NULL, bkt, &j, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    } else {
        char **nodenames = NULL;
        j = job->map->num_nodes;
        rc = PMIx_Data_pack(NULL, bkt, &j, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        for (j = 0; j < job->map->nodes->size; j++) {
            nptr = (prte_node_t *) pmix_pointer_array_get_item(job->map->nodes, j);
            if (NULL == nptr) {
                continue;
            }
            PMIx_Argv_append_nosize(&nodenames, nptr->name);
        }
        rc = pack_map(bkt, nodenames, ',');
        PMIx_Argv_free(nodenames);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        for (j = 0; j < job->apps->size; j++) {
            if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(job->apps, j))) {
                continue;
            }
            rc = pack_proc_map(bkt, job, app->idx);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* ...and then what the maps cannot say, in rank order */
        for (j = 0; j < job->procs->size; j++) {
            if (NULL == (proc = (prte_proc_t *) pmix_pointer_array_get_item(job->procs, j))) {
                continue;
            }
            rc = prte_proc_pack(bkt, proc, mode);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return prte_pmix_convert_status(rc);
            }
        }
    }

    /* pack the stdin target */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->stdin_target, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the total slots allocated to the job */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->total_slots_alloc, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* if the map is NULL, then we cannot pack it as there is
     * nothing to pack. However, we have to flag whether or not
     * the map is included so the unpacking routine can know
     * what to do
     */
    if (NULL == job->map) {
        /* pack a zero value */
        j = 0;
    } else {
        /* pack a one to indicate a map is there */
        j = 1;
    }
    rc = PMIx_Data_pack(NULL, bkt, (void *) &j, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the map - this will only pack the fields that control
     * HOW a job is to be mapped. We do -not- pack the mapped procs
     * or nodes as this info does not need to be transmitted
     */
    if (NULL != job->map) {
        rc = prte_map_pack(bkt, job->map);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the bookmark */
    if (NULL == job->bookmark) {
        bookmark = -1;
    } else {
        bookmark = job->bookmark->index;
    }
    rc = PMIx_Data_pack(NULL, bkt, (void *) &bookmark, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the job state */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->state, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the launcher ID */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &job->launcher, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    return PRTE_SUCCESS;
}

int prte_node_pack(pmix_data_buffer_t *bkt, prte_node_t *node)
{
    int rc;
    int32_t count;
    uint8_t flag;
    prte_attribute_t *kv;

    /* do not pack the index - it is meaningless on the other end */

    /* pack the node name */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &node->name, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* do not pack the daemon name or launch id */

    /* pack the number of procs on the node */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &node->num_procs, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* do not pack the procs */

    /* pack whether we are oversubscribed or not */
    flag = PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
    rc = PMIx_Data_pack(NULL, bkt, (void *) &flag, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the state */
    rc = PMIx_Data_pack(NULL, bkt, (void *) &node->state, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack any shared attributes */
    count = 0;
    PMIX_LIST_FOREACH(kv, &node->attributes, prte_attribute_t)
    {
        if (PRTE_ATTR_GLOBAL == kv->local) {
            ++count;
        }
    }
    rc = PMIx_Data_pack(NULL, bkt, (void *) &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < count) {
        PMIX_LIST_FOREACH(kv, &node->attributes, prte_attribute_t)
        {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->key, 1, PMIX_UINT16);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return prte_pmix_convert_status(rc);
                }
                rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->data, 1, PMIX_VALUE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return prte_pmix_convert_status(rc);
                }
            }
        }
    }
    return PRTE_SUCCESS;
}
/*
 * PROC
 */
int prte_proc_pack(pmix_data_buffer_t *bkt, prte_proc_t *proc, prte_job_pack_mode_t mode)
{
    pmix_status_t rc;

    /* Only what the job's node and proc maps cannot say.
     *
     * A proc's rank, its hosting daemon, its app index, its app rank and its
     * local rank are all properties of WHERE it was placed, and the maps
     * packed by prte_job_pack say exactly that - so packing them here was
     * repeating the map once per process.  What is left is genuinely
     * per-proc: the node rank counts procs of every job on that node, so one
     * job's map cannot produce it; the cpuset is the binding the mapper
     * computed; the state and attributes are its own. */

    /* pack the node rank */
    rc = PMIx_Data_pack(NULL, bkt, &proc->node_rank, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the state */
    rc = PMIx_Data_pack(NULL, bkt, &proc->state, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* The cpuset, unless it is being scattered.
     *
     * It is the largest of the three and the only one that is of no use to
     * anybody but the daemon that forks the proc: node rank and state are
     * read for procs this daemon does not host, the binding is not.  In
     * PRTE_JOB_PACK_NO_CPUSETS mode it therefore travels to that daemon
     * alone - see prte_odls_base_send_cpuset_slices(). */
    if (PRTE_JOB_PACK_NO_CPUSETS != mode) {
        rc = PMIx_Data_pack(NULL, bkt, (void *) &proc->cpuset, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* NO ATTRIBUTE LIST GOES ON THE WIRE.
     *
     * Exactly one proc attribute exists anywhere in this tree -
     * PRTE_PROC_NOBARRIER - and it is PRTE_ATTR_LOCAL, which this filter
     * excluded; it is also set by the odls on the daemon that forks the
     * proc, which is after this packing and on the far side of it. So the
     * count was 4 bytes per proc introducing a list that has been empty in
     * every job ever launched - 512 KB of a 1.9 MB launch message at
     * 1000 nodes x 128 ppn, buying nothing at any scale.
     *
     * If you add a PRTE_ATTR_GLOBAL proc attribute, it has to come back
     * here and in prte_proc_unpack, together. The check below is what will
     * tell you, because nothing else would: the attribute would simply not
     * arrive, and the receiving daemon would read a default. */
#if PRTE_ENABLE_DEBUG
    prte_attribute_t *kv;
    PMIX_LIST_FOREACH(kv, &proc->attributes, prte_attribute_t)
    {
        if (PRTE_ATTR_GLOBAL == kv->local) {
            pmix_output(0, "%s prte_proc_pack: proc %s carries global attribute %d, "
                        "which is NOT packed - see the comment here",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&proc->name), (int) kv->key);
            break;
        }
    }
#endif

    return PRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prte_app_pack(pmix_data_buffer_t *bkt, prte_app_context_t *app)
{
    pmix_status_t rc;
    int32_t count, j;
    prte_attribute_t *kv;

    /* pack the application index (for multiapp jobs) */
    rc = PMIx_Data_pack(NULL, bkt, &app->idx, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the application name */
    rc = PMIx_Data_pack(NULL, bkt, &app->app, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the number of processes */
    rc = PMIx_Data_pack(NULL, bkt, &app->num_procs, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the first rank for this app */
    rc = PMIx_Data_pack(NULL, bkt, &app->first_rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the number of entries in the argv array */
    count = PMIx_Argv_count(app->argv);
    rc = PMIx_Data_pack(NULL, bkt, &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* if there are entries, pack the argv entries */
    for (j = 0; j < count; j++) {
        rc = PMIx_Data_pack(NULL, bkt, (void *) &app->argv[j], 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the number of entries in the enviro array */
    count = PMIx_Argv_count(app->env);
    rc = PMIx_Data_pack(NULL, bkt, &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* if there are entries, pack the enviro entries */
    for (j = 0; j < count; j++) {
        rc = PMIx_Data_pack(NULL, bkt, (void *) &app->env[j], 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the cwd */
    rc = PMIx_Data_pack(NULL, bkt, &app->cwd, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the flags */
    rc = PMIx_Data_pack(NULL, bkt, &app->flags, 1, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack attributes */
    count = 0;
    PMIX_LIST_FOREACH(kv, &app->attributes, prte_attribute_t)
    {
        if (PRTE_ATTR_GLOBAL == kv->local) {
            ++count;
        }
    }
    rc = PMIx_Data_pack(NULL, bkt, &count, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < count) {
        PMIX_LIST_FOREACH(kv, &app->attributes, prte_attribute_t)
        {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->key, 1, PMIX_UINT16);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return prte_pmix_convert_status(rc);
                }
                rc = PMIx_Data_pack(NULL, bkt, (void *) &kv->data, 1, PMIX_VALUE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return prte_pmix_convert_status(rc);
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map
 */
int prte_map_pack(pmix_data_buffer_t *bkt, struct prte_job_map_t *mp)
{
    pmix_status_t rc;
    prte_job_map_t *map = (prte_job_map_t *) mp;

    /* pack the policies. Note there is no mapper name on the wire: mapping
     * happens only on the HNP, so the identity of the component that did it
     * was never read at the far end - it was two strings per job, per
     * daemon, that nothing consumed. */
    rc = PMIx_Data_pack(NULL, bkt, &map->mapping, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    rc = PMIx_Data_pack(NULL, bkt, &map->ranking, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    rc = PMIx_Data_pack(NULL, bkt, &map->binding, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    /* pack the number of nodes involved in the job */
    rc = PMIx_Data_pack(NULL, bkt, &map->num_nodes, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    return PRTE_SUCCESS;
}
