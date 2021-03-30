/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/show_help.h"

#include "rmaps_ppr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int ppr_mapper(prte_job_t *jdata);
static int assign_locations(prte_job_t *jdata);

prte_rmaps_base_module_t prte_rmaps_ppr_module = {.map_job = ppr_mapper,
                                                  .assign_locations = assign_locations};

/* RHC: will eventually remove this
 * definition as it is no longer reqd
 * in the rest of OMPI system.
 *
 * Define a hierarchical level value that
 * helps resolve the hwloc behavior of
 * treating caches as a single type of
 * entity - must always be available
 */
typedef enum {
    PRTE_HWLOC_NODE_LEVEL = 0,
    PRTE_HWLOC_NUMA_LEVEL,
    PRTE_HWLOC_PACKAGE_LEVEL,
    PRTE_HWLOC_L3CACHE_LEVEL,
    PRTE_HWLOC_L2CACHE_LEVEL,
    PRTE_HWLOC_L1CACHE_LEVEL,
    PRTE_HWLOC_CORE_LEVEL,
    PRTE_HWLOC_HWTHREAD_LEVEL
} prte_hwloc_level_t;

static void prune(pmix_nspace_t jobid, prte_app_idx_t app_idx, prte_node_t *node,
                  prte_hwloc_level_t *level, pmix_rank_t *nmapped);

static int rmaps_ppr_global[PRTE_HWLOC_HWTHREAD_LEVEL + 1];

static int ppr_mapper(prte_job_t *jdata)
{
    int rc = PRTE_SUCCESS, j, n;
    prte_proc_t *proc;
    prte_mca_base_component_t *c = &prte_rmaps_ppr_component.base_version;
    prte_node_t *node;
    prte_app_context_t *app;
    pmix_rank_t total_procs, nprocs_mapped;
    prte_hwloc_level_t start = PRTE_HWLOC_NODE_LEVEL;
    hwloc_obj_t obj;
    hwloc_obj_type_t lowest;
    unsigned cache_level = 0;
    unsigned int nobjs, i, num_available;
    ;
    bool pruning_reqd = false;
    prte_hwloc_level_t level;
    prte_list_t node_list;
    prte_list_item_t *item;
    int32_t num_slots;
    prte_app_idx_t idx;
    char **ppr_req, **ck, *jobppr = NULL;
    size_t len;
    bool initial_map = true;

    /* only handle initial launch of loadbalanced
     * or NPERxxx jobs - allow restarting of failed apps
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s being restarted - ppr cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper
        && 0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, (void **) &jobppr, PMIX_STRING)
        || NULL == jobppr || PRTE_MAPPING_PPR != PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* not for us */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr mapper PPR %s policy %s",
                            PRTE_JOBID_PRINT(jdata->nspace), (NULL == jobppr) ? "NULL" : jobppr,
                            (PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping))
                                ? "PPRSET"
                                : "PPR NOTSET");
        if (NULL != jobppr) {
            free(jobppr);
        }
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: mapping job %s with ppr %s",
                        PRTE_JOBID_PRINT(jdata->nspace), jobppr);

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* initialize */
    memset(rmaps_ppr_global, 0, PRTE_HWLOC_HWTHREAD_LEVEL * sizeof(prte_hwloc_level_t));

    /* parse option */
    n = 0;
    ppr_req = prte_argv_split(jobppr, ',');
    for (j = 0; NULL != ppr_req[j]; j++) {
        /* split on the colon */
        ck = prte_argv_split(ppr_req[j], ':');
        if (2 != prte_argv_count(ck)) {
            /* must provide a specification */
            prte_show_help("help-prte-rmaps-ppr.txt", "invalid-ppr", true, jobppr);
            prte_argv_free(ppr_req);
            prte_argv_free(ck);
            free(jobppr);
            return PRTE_ERR_SILENT;
        }
        len = strlen(ck[1]);
        if (0 == strncasecmp(ck[1], "node", len)) {
            rmaps_ppr_global[PRTE_HWLOC_NODE_LEVEL] = strtol(ck[0], NULL, 10);
            PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYNODE);
            start = PRTE_HWLOC_NODE_LEVEL;
            n++;
        } else if (0 == strncasecmp(ck[1], "hwthread", len)
                   || 0 == strncasecmp(ck[1], "thread", len)) {
            rmaps_ppr_global[PRTE_HWLOC_HWTHREAD_LEVEL] = strtol(ck[0], NULL, 10);
            start = PRTE_HWLOC_HWTHREAD_LEVEL;
            PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYHWTHREAD);
            n++;
        } else if (0 == strncasecmp(ck[1], "core", len)) {
            rmaps_ppr_global[PRTE_HWLOC_CORE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRTE_HWLOC_CORE_LEVEL) {
                start = PRTE_HWLOC_CORE_LEVEL;
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYCORE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "package", len) || 0 == strncasecmp(ck[1], "skt", len)) {
            rmaps_ppr_global[PRTE_HWLOC_PACKAGE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRTE_HWLOC_PACKAGE_LEVEL) {
                start = PRTE_HWLOC_PACKAGE_LEVEL;
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYPACKAGE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l1cache", len)) {
            rmaps_ppr_global[PRTE_HWLOC_L1CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRTE_HWLOC_L1CACHE_LEVEL) {
                start = PRTE_HWLOC_L1CACHE_LEVEL;
                cache_level = 1;
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL1CACHE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l2cache", len)) {
            rmaps_ppr_global[PRTE_HWLOC_L2CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRTE_HWLOC_L2CACHE_LEVEL) {
                start = PRTE_HWLOC_L2CACHE_LEVEL;
                cache_level = 2;
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL2CACHE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l3cache", len)) {
            rmaps_ppr_global[PRTE_HWLOC_L3CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRTE_HWLOC_L3CACHE_LEVEL) {
                start = PRTE_HWLOC_L3CACHE_LEVEL;
                cache_level = 3;
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL3CACHE);
            }
            n++;
        } else {
            /* unknown spec */
            prte_show_help("help-prte-rmaps-ppr.txt", "unrecognized-ppr-option", true, ck[1],
                           jobppr);
            prte_argv_free(ppr_req);
            prte_argv_free(ck);
            free(jobppr);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(ck);
    }
    prte_argv_free(ppr_req);
    /* if nothing was given, that's an error */
    if (0 == n) {
        prte_output(0, "NOTHING GIVEN");
        free(jobppr);
        return PRTE_ERR_SILENT;
    }
    /* if more than one level was specified, then pruning will be reqd */
    if (1 < n) {
        pruning_reqd = true;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: job %s assigned policy %s", PRTE_JOBID_PRINT(jdata->nspace),
                        prte_rmaps_base_print_mapping(jdata->map->mapping));

    /* convenience */
    level = start;
    lowest = prte_hwloc_levels[start];

    for (idx = 0; idx < (prte_app_idx_t) jdata->apps->size; idx++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, idx))) {
            continue;
        }

        /* if the number of total procs was given, set that
         * limit - otherwise, set to max so we simply fill
         * all the nodes with the pattern
         */
        if (0 < app->num_procs) {
            total_procs = app->num_procs;
        } else {
            total_procs = PMIX_RANK_VALID;
        }

        /* get the available nodes */
        PRTE_CONSTRUCT(&node_list, prte_list_t);
        if (PRTE_SUCCESS
            != (rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                      jdata->map->mapping, initial_map, false))) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* if a bookmark exists from some prior mapping, set us to start there */
        jdata->bookmark = prte_rmaps_base_get_starting_point(&node_list, jdata);

        /* cycle across the nodes */
        nprocs_mapped = 0;
        for (item = prte_list_get_first(&node_list); item != prte_list_get_end(&node_list);
             item = prte_list_get_next(item)) {
            node = (prte_node_t *) item;
            /* bozo check */
            if (NULL == node->topology || NULL == node->topology->topo) {
                prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* add the node to the map, if needed */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                PRTE_RETAIN(node);
                prte_pointer_array_add(jdata->map->nodes, node);
                jdata->map->num_nodes++;
            }
            /* if we are mapping solely at the node level, just put
             * that many procs on this node
             */
            if (PRTE_HWLOC_NODE_LEVEL == start) {
                if (rmaps_ppr_global[start] > node->slots_available) {
                    /* not enough slots available for this request */
                    prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                   rmaps_ppr_global[start], app->app);
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                obj = hwloc_get_root_obj(node->topology->topo);
                for (j = 0; j < rmaps_ppr_global[start] && nprocs_mapped < total_procs; j++) {
                    if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, idx))) {
                        rc = PRTE_ERR_OUT_OF_RESOURCE;
                        goto error;
                    }
                    nprocs_mapped++;
                    prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL,
                                       obj, PMIX_POINTER);
                }
            } else {
                /* get the number of lowest resources on this node */
                nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, lowest,
                                                           cache_level);
                /* Map up to number of slots_available on node or number of specified resource on
                 * node whichever is less. */
                if (node->slots_available < (int) nobjs) {
                    num_available = node->slots_available;
                } else {
                    num_available = nobjs;
                }
                /* map the specified number of procs to each such resource on this node,
                 * recording the locale of each proc so we know its cpuset
                 */
                for (i = 0; i < num_available; i++) {
                    obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, lowest, cache_level,
                                                          i);
                    for (j = 0; j < rmaps_ppr_global[start] && nprocs_mapped < total_procs; j++) {
                        if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, idx))) {
                            rc = PRTE_ERR_OUT_OF_RESOURCE;
                            goto error;
                        }
                        nprocs_mapped++;
                        prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                           PRTE_ATTR_LOCAL, obj, PMIX_POINTER);
                    }
                }

                if (pruning_reqd) {
                    /* go up the ladder and prune the procs according to
                     * the specification, adjusting the count of procs on the
                     * node as we go
                     */
                    level--;
                    prune(jdata->nspace, idx, node, &level, &nprocs_mapped);
                }
            }

            if (!(PRTE_MAPPING_DEBUGGER & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                /* set the total slots used */
                if ((int) node->num_procs <= node->slots) {
                    node->slots_inuse = (int) node->num_procs;
                } else {
                    node->slots_inuse = node->slots;
                }

                /* if no-oversubscribe was specified, check to see if
                 * we have violated the total slot specification - regardless,
                 * if slots_max was given, we are not allowed to violate it!
                 */
                if ((node->slots < (int) node->num_procs)
                    || (0 < node->slots_max && node->slots_max < (int) node->num_procs)) {
                    if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                        & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, node->num_procs, app->app);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        rc = PRTE_ERR_SILENT;
                        goto error;
                    }
                    /* flag the node as oversubscribed so that sched-yield gets
                     * properly set
                     */
                    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                    /* check for permission */
                    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                        /* if we weren't given a directive either way, then we will error out
                         * as the #slots were specifically given, either by the host RM or
                         * via hostfile/dash-host */
                        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN
                              & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                            prte_show_help("help-prte-rmaps-base.txt",
                                           "prte-rmaps-base:alloc-error", true, app->num_procs,
                                           app->app);
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                            rc = PRTE_ERR_SILENT;
                            goto error;
                        } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                                   & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                            /* if we were explicitly told not to oversubscribe, then don't */
                            prte_show_help("help-prte-rmaps-base.txt",
                                           "prte-rmaps-base:alloc-error", true, app->num_procs,
                                           app->app);
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                            rc = PRTE_ERR_SILENT;
                            goto error;
                        }
                    }
                }
            }

            /* if we haven't mapped all the procs, continue on to the
             * next node
             */
            if (total_procs == nprocs_mapped) {
                break;
            }
        }
        if (0 == app->num_procs) {
            app->num_procs = nprocs_mapped;
        }
        if (PMIX_RANK_VALID != total_procs && nprocs_mapped < total_procs) {
            /* couldn't map them all */
            prte_show_help("help-prte-rmaps-ppr.txt", "ppr-too-many-procs", true, app->app,
                           app->num_procs, nprocs_mapped, total_procs, jobppr);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

        /* track the total number of processes we mapped - must update
         * this AFTER we compute vpids so that computation is done
         * correctly
         */
        jdata->num_procs += app->num_procs;

        PRTE_LIST_DESTRUCT(&node_list);
    }
    free(jobppr);
    return PRTE_SUCCESS;

error:
    PRTE_LIST_DESTRUCT(&node_list);
    free(jobppr);
    return rc;
}

static hwloc_obj_t find_split(hwloc_topology_t topo, hwloc_obj_t obj)
{
    unsigned k;
    hwloc_obj_t nxt;

    if (1 < obj->arity) {
        return obj;
    }
    for (k = 0; k < obj->arity; k++) {
        nxt = find_split(topo, obj->children[k]);
        if (NULL != nxt) {
            return nxt;
        }
    }
    return NULL;
}

/* recursively climb the topology, pruning procs beyond that allowed
 * by the given ppr
 */
static void prune(pmix_nspace_t jobid, prte_app_idx_t app_idx, prte_node_t *node,
                  prte_hwloc_level_t *level, pmix_rank_t *nmapped)
{
    hwloc_obj_t obj, top;
    unsigned int i, nobjs;
    hwloc_obj_type_t lvl;
    unsigned cache_level = 0, k;
    int nprocs;
    hwloc_cpuset_t avail;
    int n, limit, nmax, nunder, idx, idxmax = 0;
    prte_proc_t *proc, *pptr, *procmax;
    prte_hwloc_level_t ll;
    char dang[64];
    hwloc_obj_t locale;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: pruning level %d", *level);

    /* convenience */
    ll = *level;

    /* convenience */
    lvl = prte_hwloc_levels[ll];
    limit = rmaps_ppr_global[ll];

    if (0 == limit) {
        /* no limit at this level, so move up if necessary */
        if (0 == ll) {
            /* done */
            return;
        }
        --(*level);
        prune(jobid, app_idx, node, level, nmapped);
        return;
    }

    /* handle the darn cache thing again */
    if (PRTE_HWLOC_L3CACHE_LEVEL == ll) {
        cache_level = 3;
    } else if (PRTE_HWLOC_L2CACHE_LEVEL == ll) {
        cache_level = 2;
    } else if (PRTE_HWLOC_L1CACHE_LEVEL == ll) {
        cache_level = 1;
    }

    /* get the number of resources at this level on this node */
    nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, lvl, cache_level);

    /* for each resource, compute the number of procs sitting
     * underneath it and check against the limit
     */
    for (i = 0; i < nobjs; i++) {
        obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, lvl, cache_level, i);
        /* get the available cpuset */
        avail = obj->cpuset;

        /* look at the intersection of this object's cpuset and that
         * of each proc in the job/app - if they intersect, then count this proc
         * against the limit
         */
        nprocs = 0;
        for (n = 0; n < node->procs->size; n++) {
            if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, n))) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(proc->name.nspace, jobid) || proc->app_idx != app_idx) {
                continue;
            }
            locale = NULL;
            if (prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, (void **) &locale,
                                   PMIX_POINTER)) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return;
            }
            if (hwloc_bitmap_intersects(avail, locale->cpuset)) {
                nprocs++;
            }
        }
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: found %d procs limit %d", nprocs, limit);

        /* check against the limit */
        while (limit < nprocs) {
            /* need to remove procs - do this in a semi-intelligent
             * manner to provide a little load balancing by cycling
             * across the objects beneath this one, removing procs
             * in a round-robin fashion until the limit is satisfied
             *
             * NOTE: I'm sure someone more knowledgeable with hwloc
             * will come up with a more efficient way to do this, so
             * consider this is a starting point
             */

            /* find the first level that has more than
             * one child beneath it - if all levels
             * have only one child, then return this
             * object
             */
            top = find_split(node->topology->topo, obj);
            hwloc_obj_type_snprintf(dang, 64, top, 1);
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:ppr: SPLIT AT LEVEL %s", dang);

            /* cycle across the children of this object */
            nmax = 0;
            procmax = NULL;
            idx = 0;
            /* find the child with the most procs underneath it */
            for (k = 0; k < top->arity && limit < nprocs; k++) {
                /* get this object's available cpuset */
                nunder = 0;
                pptr = NULL;
                for (n = 0; n < node->procs->size; n++) {
                    if (NULL
                        == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, n))) {
                        continue;
                    }
                    if (!PMIX_CHECK_NSPACE(proc->name.nspace, jobid) || proc->app_idx != app_idx) {
                        continue;
                    }
                    locale = NULL;
                    if (prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                           (void **) &locale, PMIX_POINTER)) {
                        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                        return;
                    }
                    if (hwloc_bitmap_intersects(top->children[k]->cpuset, locale->cpuset)) {
                        nunder++;
                        if (NULL == pptr) {
                            /* save the location of the first proc under this object */
                            pptr = proc;
                            idx = n;
                        }
                    }
                }
                if (nmax < nunder) {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:ppr: PROCS UNDER CHILD %d %d MAX %d", k, nunder,
                                        nmax);
                    nmax = nunder;
                    procmax = pptr;
                    idxmax = idx;
                }
            }
            if (NULL == procmax) {
                /* can't find anything to remove - error out */
                goto error;
            }
            /* remove it */
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:ppr: removing proc at posn %d", idxmax);
            prte_pointer_array_set_item(node->procs, idxmax, NULL);
            node->num_procs--;
            node->slots_inuse--;
            if (node->slots_inuse < 0) {
                node->slots_inuse = 0;
            }
            nprocs--;
            *nmapped -= 1;
            PRTE_RELEASE(procmax);
        }
    }
    /* finished with this level - move up if necessary */
    if (0 == ll) {
        return;
    }
    --(*level);
    prune(jobid, app_idx, node, level, nmapped);
    return;

error:
    prte_output(0, "INFINITE LOOP");
}

static int assign_locations(prte_job_t *jdata)
{
    int i, j, m, n;
    prte_mca_base_component_t *c = &prte_rmaps_ppr_component.base_version;
    prte_node_t *node;
    prte_proc_t *proc;
    prte_app_context_t *app;
    hwloc_obj_type_t level;
    hwloc_obj_t obj;
    unsigned int cache_level = 0;
    int ppr, cnt, nobjs, nprocs_mapped;
    char **ppr_req, **ck, *jobppr;

    if (NULL == jdata->map->last_mapper
        || 0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr assign: %s",
                            PRTE_JOBID_PRINT(jdata->nspace),
                            (NULL == jdata->map->last_mapper) ? "NULL" : jdata->map->last_mapper);
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, (void **) &jobppr, PMIX_STRING);

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: assigning locations for job %s with ppr %s policy %s",
                        PRTE_JOBID_PRINT(jdata->nspace), jobppr,
                        prte_rmaps_base_print_mapping(jdata->map->mapping));

    /* pickup the object level */
    if (PRTE_MAPPING_BYNODE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_MACHINE;
    } else if (PRTE_MAPPING_BYHWTHREAD == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_PU;
    } else if (PRTE_MAPPING_BYCORE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_CORE;
    } else if (PRTE_MAPPING_BYPACKAGE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_PACKAGE;
    } else if (PRTE_MAPPING_BYL1CACHE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L1CACHE;
        cache_level = 1;
    } else if (PRTE_MAPPING_BYL2CACHE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L2CACHE;
        cache_level = 2;
    } else if (PRTE_MAPPING_BYL3CACHE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L3CACHE;
        cache_level = 3;
    } else {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* get the ppr value */
    ppr_req = prte_argv_split(jobppr, ',');
    ck = prte_argv_split(ppr_req[0], ':');
    ppr = strtol(ck[0], NULL, 10);
    prte_argv_free(ck);
    prte_argv_free(ppr_req);

    /* start assigning procs to objects, filling each object as we go until
     * all procs are assigned. */
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        nprocs_mapped = 0;
        for (m = 0; m < jdata->map->nodes->size; m++) {
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            if (NULL == node->topology || NULL == node->topology->topo) {
                prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
                return PRTE_ERR_SILENT;
            }
            if (HWLOC_OBJ_MACHINE == level) {
                obj = hwloc_get_root_obj(node->topology->topo);
                for (j = 0; j < node->procs->size; j++) {
                    if (NULL
                        == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                        continue;
                    }
                    prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL,
                                       obj, PMIX_POINTER);
                }
            } else {
                /* get the number of resources on this node at this level */
                nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, level,
                                                           cache_level);

                /* map the specified number of procs to each such resource on this node,
                 * recording the locale of each proc so we know its cpuset
                 */
                for (i = 0; i < nobjs; i++) {
                    cnt = 0;
                    obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, level, cache_level,
                                                          i);
                    for (j = 0;
                         j < node->procs->size && cnt < ppr && nprocs_mapped < app->num_procs;
                         j++) {
                        if (NULL
                            == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs,
                                                                                   j))) {
                            continue;
                        }
                        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                            continue;
                        }
                        /* if we already assigned it, then skip */
                        if (prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, NULL,
                                               PMIX_POINTER)) {
                            continue;
                        }
                        nprocs_mapped++;
                        cnt++;
                        prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                           PRTE_ATTR_LOCAL, obj, PMIX_POINTER);
                    }
                }
            }
        }
    }
    return PRTE_SUCCESS;
}
