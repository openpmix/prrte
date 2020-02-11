/*
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/argv.h"

#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_ppr.h"

static int ppr_mapper(prrte_job_t *jdata);
static int assign_locations(prrte_job_t *jdata);

prrte_rmaps_base_module_t prrte_rmaps_ppr_module = {
    .map_job = ppr_mapper,
    .assign_locations = assign_locations
};

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
    PRRTE_HWLOC_NODE_LEVEL=0,
    PRRTE_HWLOC_NUMA_LEVEL,
    PRRTE_HWLOC_SOCKET_LEVEL,
    PRRTE_HWLOC_L3CACHE_LEVEL,
    PRRTE_HWLOC_L2CACHE_LEVEL,
    PRRTE_HWLOC_L1CACHE_LEVEL,
    PRRTE_HWLOC_CORE_LEVEL,
    PRRTE_HWLOC_HWTHREAD_LEVEL
} prrte_hwloc_level_t;

static void prune(prrte_jobid_t jobid,
                  prrte_app_idx_t app_idx,
                  prrte_node_t *node,
                  prrte_hwloc_level_t *level,
                  prrte_vpid_t *nmapped);

static int ppr[PRRTE_HWLOC_HWTHREAD_LEVEL+1];

static int ppr_mapper(prrte_job_t *jdata)
{
    int rc = PRRTE_SUCCESS, j, n;
    prrte_proc_t *proc;
    prrte_mca_base_component_t *c=&prrte_rmaps_ppr_component.base_version;
    prrte_node_t *node;
    prrte_app_context_t *app;
    prrte_vpid_t total_procs, nprocs_mapped;
    prrte_hwloc_level_t start=PRRTE_HWLOC_NODE_LEVEL;
    hwloc_obj_t obj;
    hwloc_obj_type_t lowest;
    unsigned cache_level=0;
    unsigned int nobjs, i;
    bool pruning_reqd = false;
    prrte_hwloc_level_t level;
    prrte_list_t node_list;
    prrte_list_item_t *item;
    prrte_std_cntr_t num_slots;
    prrte_app_idx_t idx;
    char **ppr_req, **ck;
    size_t len;
    bool initial_map=true;

    /* only handle initial launch of loadbalanced
     * or NPERxxx jobs - allow restarting of failed apps
     */
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s being restarted - ppr cannot map",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper &&
        0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == jdata->map->ppr ||
        PRRTE_MAPPING_PPR != PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* not for us */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr mapper PPR %s policy %s",
                            PRRTE_JOBID_PRINT(jdata->jobid),
                            (NULL == jdata->map->ppr) ? "NULL" : jdata->map->ppr,
                            (PRRTE_MAPPING_PPR == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) ? "PPRSET" : "PPR NOTSET");
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: mapping job %s with ppr %s",
                        PRRTE_JOBID_PRINT(jdata->jobid), jdata->map->ppr);

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* initialize */
    memset(ppr, 0, PRRTE_HWLOC_HWTHREAD_LEVEL * sizeof(prrte_hwloc_level_t));

    /* parse option */
    n=0;
    ppr_req = prrte_argv_split(jdata->map->ppr, ',');
    for (j=0; NULL != ppr_req[j]; j++) {
        /* split on the colon */
        ck = prrte_argv_split(ppr_req[j], ':');
        if (2 != prrte_argv_count(ck)) {
            /* must provide a specification */
            prrte_show_help("help-prrte-rmaps-ppr.txt", "invalid-ppr", true, jdata->map->ppr);
            prrte_argv_free(ppr_req);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        len = strlen(ck[1]);
        if (0 == strncasecmp(ck[1], "node", len)) {
            ppr[PRRTE_HWLOC_NODE_LEVEL] = strtol(ck[0], NULL, 10);
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYNODE);
            start = PRRTE_HWLOC_NODE_LEVEL;
            n++;
        } else if (0 == strncasecmp(ck[1], "hwthread", len) ||
                   0 == strncasecmp(ck[1], "thread", len)) {
            ppr[PRRTE_HWLOC_HWTHREAD_LEVEL] = strtol(ck[0], NULL, 10);
            start = PRRTE_HWLOC_HWTHREAD_LEVEL;
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYHWTHREAD);
            n++;
        } else if (0 == strncasecmp(ck[1], "core", len)) {
            ppr[PRRTE_HWLOC_CORE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_CORE_LEVEL) {
                start = PRRTE_HWLOC_CORE_LEVEL;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYCORE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "socket", len) ||
                   0 == strncasecmp(ck[1], "skt", len)) {
            ppr[PRRTE_HWLOC_SOCKET_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_SOCKET_LEVEL) {
                start = PRRTE_HWLOC_SOCKET_LEVEL;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSOCKET);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l1cache", len)) {
            ppr[PRRTE_HWLOC_L1CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_L1CACHE_LEVEL) {
                start = PRRTE_HWLOC_L1CACHE_LEVEL;
                cache_level = 1;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYL1CACHE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l2cache", len)) {
            ppr[PRRTE_HWLOC_L2CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_L2CACHE_LEVEL) {
                start = PRRTE_HWLOC_L2CACHE_LEVEL;
                cache_level = 2;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYL2CACHE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "l3cache", len)) {
            ppr[PRRTE_HWLOC_L3CACHE_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_L3CACHE_LEVEL) {
                start = PRRTE_HWLOC_L3CACHE_LEVEL;
                cache_level = 3;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYL3CACHE);
            }
            n++;
        } else if (0 == strncasecmp(ck[1], "numa", len)) {
            ppr[PRRTE_HWLOC_NUMA_LEVEL] = strtol(ck[0], NULL, 10);
            if (start < PRRTE_HWLOC_NUMA_LEVEL) {
                start = PRRTE_HWLOC_NUMA_LEVEL;
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYNUMA);
            }
            n++;
        } else {
            /* unknown spec */
            prrte_show_help("help-prrte-rmaps-ppr.txt", "unrecognized-ppr-option", true, ck[1], jdata->map->ppr);
            prrte_argv_free(ppr_req);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        prrte_argv_free(ck);
    }
    prrte_argv_free(ppr_req);
    /* if nothing was given, that's an error */
    if (0 == n) {
        prrte_output(0, "NOTHING GIVEN");
        return PRRTE_ERR_SILENT;
    }
    /* if more than one level was specified, then pruning will be reqd */
    if (1 < n) {
        pruning_reqd = true;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: job %s assigned policy %s",
                        PRRTE_JOBID_PRINT(jdata->jobid),
                        prrte_rmaps_base_print_mapping(jdata->map->mapping));

    /* convenience */
    level = start;
    lowest = prrte_hwloc_levels[start];

    for (idx=0; idx < (prrte_app_idx_t)jdata->apps->size; idx++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, idx))) {
            continue;
        }

        /* if the number of total procs was given, set that
         * limit - otherwise, set to max so we simply fill
         * all the nodes with the pattern
         */
        if (0 < app->num_procs) {
            total_procs = app->num_procs;
        } else {
            total_procs = PRRTE_VPID_MAX;
        }

        /* get the available nodes */
        PRRTE_CONSTRUCT(&node_list, prrte_list_t);
        if(PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                                  jdata->map->mapping, initial_map, false))) {
            PRRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* if a bookmark exists from some prior mapping, set us to start there */
        jdata->bookmark = prrte_rmaps_base_get_starting_point(&node_list, jdata);

        /* cycle across the nodes */
        nprocs_mapped = 0;
        for (item = prrte_list_get_first(&node_list);
             item != prrte_list_get_end(&node_list);
             item = prrte_list_get_next(item)) {
            node = (prrte_node_t*)item;
            /* bozo check */
            if (NULL == node->topology || NULL == node->topology->topo) {
                prrte_show_help("help-prrte-rmaps-ppr.txt", "ppr-topo-missing",
                               true, node->name);
                rc = PRRTE_ERR_SILENT;
                goto error;
            }
            /* add the node to the map, if needed */
            if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_MAPPED);
                PRRTE_RETAIN(node);
                prrte_pointer_array_add(jdata->map->nodes, node);
                jdata->map->num_nodes++;
            }
            /* if we are mapping solely at the node level, just put
             * that many procs on this node
             */
            if (PRRTE_HWLOC_NODE_LEVEL == start) {
                obj = hwloc_get_root_obj(node->topology->topo);
                for (j=0; j < ppr[start] && nprocs_mapped < total_procs; j++) {
                    if (NULL == (proc = prrte_rmaps_base_setup_proc(jdata, node, idx))) {
                        rc = PRRTE_ERR_OUT_OF_RESOURCE;
                        goto error;
                    }
                    nprocs_mapped++;
                    prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
                }
            } else {
                /* get the number of lowest resources on this node */
                nobjs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                           lowest, cache_level,
                                                           PRRTE_HWLOC_AVAILABLE);

                /* map the specified number of procs to each such resource on this node,
                 * recording the locale of each proc so we know its cpuset
                 */
                for (i=0; i < nobjs; i++) {
                    obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo,
                                                          lowest, cache_level,
                                                          i, PRRTE_HWLOC_AVAILABLE);
                    for (j=0; j < ppr[start] && nprocs_mapped < total_procs; j++) {
                        if (NULL == (proc = prrte_rmaps_base_setup_proc(jdata, node, idx))) {
                            rc = PRRTE_ERR_OUT_OF_RESOURCE;
                            goto error;
                        }
                        nprocs_mapped++;
                        prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
                    }
                }

                if (pruning_reqd) {
                    /* go up the ladder and prune the procs according to
                     * the specification, adjusting the count of procs on the
                     * node as we go
                     */
                    level--;
                    prune(jdata->jobid, idx, node, &level, &nprocs_mapped);
                }
            }

            if (!(PRRTE_MAPPING_DEBUGGER & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                /* set the total slots used */
                if ((int)node->num_procs <= node->slots) {
                    node->slots_inuse = (int)node->num_procs;
                } else {
                    node->slots_inuse = node->slots;
                }

                /* if no-oversubscribe was specified, check to see if
                 * we have violated the total slot specification - regardless,
                 * if slots_max was given, we are not allowed to violate it!
                 */
                if ((node->slots < (int)node->num_procs) ||
                    (0 < node->slots_max && node->slots_max < (int)node->num_procs)) {
                    if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                       true, node->num_procs, app->app);
                        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                        rc = PRRTE_ERR_SILENT;
                        goto error;
                    }
                    /* flag the node as oversubscribed so that sched-yield gets
                     * properly set
                     */
                    PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_OVERSUBSCRIBED);
                    PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_OVERSUBSCRIBED);
                    /* check for permission */
                    if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
                        /* if we weren't given a directive either way, then we will error out
                         * as the #slots were specifically given, either by the host RM or
                         * via hostfile/dash-host */
                        if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                           true, app->num_procs, app->app);
                            PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                            return PRRTE_ERR_SILENT;
                        } else if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                            /* if we were explicitly told not to oversubscribe, then don't */
                            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                           true, app->num_procs, app->app);
                            PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                            return PRRTE_ERR_SILENT;
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
        if (PRRTE_VPID_MAX != total_procs && nprocs_mapped < total_procs) {
            /* couldn't map them all */
            prrte_show_help("help-prrte-rmaps-ppr.txt", "ppr-too-many-procs",
                           true, app->app, app->num_procs, nprocs_mapped, total_procs, jdata->map->ppr);
            rc = PRRTE_ERR_SILENT;
            goto error;
        }

        /* track the total number of processes we mapped - must update
         * this AFTER we compute vpids so that computation is done
         * correctly
         */
        jdata->num_procs += app->num_procs;

        while (NULL != (item = prrte_list_remove_first(&node_list))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&node_list);
    }
    return PRRTE_SUCCESS;

  error:
    while (NULL != (item = prrte_list_remove_first(&node_list))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&node_list);
    return rc;
}

static hwloc_obj_t find_split(hwloc_topology_t topo, hwloc_obj_t obj)
{
    unsigned k;
    hwloc_obj_t nxt;

    if (1 < obj->arity) {
        return obj;
    }
    for (k=0; k < obj->arity; k++) {
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
static void prune(prrte_jobid_t jobid,
                  prrte_app_idx_t app_idx,
                  prrte_node_t *node,
                  prrte_hwloc_level_t *level,
                  prrte_vpid_t *nmapped)
{
    hwloc_obj_t obj, top;
    unsigned int i, nobjs;
    hwloc_obj_type_t lvl;
    unsigned cache_level = 0, k;
    int nprocs;
    hwloc_cpuset_t avail;
    int n, limit, nmax, nunder, idx, idxmax = 0;
    prrte_proc_t *proc, *pptr, *procmax;
    prrte_hwloc_level_t ll;
    char dang[64];
    hwloc_obj_t locale;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: pruning level %d",
                        *level);

    /* convenience */
    ll = *level;

    /* convenience */
    lvl = prrte_hwloc_levels[ll];
    limit = ppr[ll];

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
    if (PRRTE_HWLOC_L3CACHE_LEVEL == ll) {
        cache_level = 3;
    } else if (PRRTE_HWLOC_L2CACHE_LEVEL == ll) {
        cache_level = 2;
    } else if (PRRTE_HWLOC_L1CACHE_LEVEL == ll) {
        cache_level = 1;
    }

    /* get the number of resources at this level on this node */
    nobjs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                               lvl, cache_level,
                                               PRRTE_HWLOC_AVAILABLE);

    /* for each resource, compute the number of procs sitting
     * underneath it and check against the limit
     */
    for (i=0; i < nobjs; i++) {
        obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo,
                                              lvl, cache_level,
                                              i, PRRTE_HWLOC_AVAILABLE);
        /* get the available cpuset */
        avail = obj->cpuset;

        /* look at the intersection of this object's cpuset and that
         * of each proc in the job/app - if they intersect, then count this proc
         * against the limit
         */
        nprocs = 0;
        for (n=0; n < node->procs->size; n++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, n))) {
                continue;
            }
            if (proc->name.jobid != jobid ||
                proc->app_idx != app_idx) {
                continue;
            }
            locale = NULL;
            if (prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                return;
            }
            if (hwloc_bitmap_intersects(avail, locale->cpuset)) {
                nprocs++;
            }
        }
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: found %d procs limit %d",
                            nprocs, limit);

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
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:ppr: SPLIT AT LEVEL %s", dang);

            /* cycle across the children of this object */
            nmax = 0;
            procmax = NULL;
            idx = 0;
            /* find the child with the most procs underneath it */
            for (k=0; k < top->arity && limit < nprocs; k++) {
                /* get this object's available cpuset */
                nunder = 0;
                pptr = NULL;
                for (n=0; n < node->procs->size; n++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, n))) {
                        continue;
                    }
                    if (proc->name.jobid != jobid ||
                        proc->app_idx != app_idx) {
                        continue;
                    }
                    locale = NULL;
                    if (prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
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
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:ppr: PROCS UNDER CHILD %d %d MAX %d",
                                        k, nunder, nmax);
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
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:ppr: removing proc at posn %d",
                                idxmax);
            prrte_pointer_array_set_item(node->procs, idxmax, NULL);
            node->num_procs--;
            node->slots_inuse--;
            if (node->slots_inuse < 0) {
                node->slots_inuse = 0;
            }
            nprocs--;
            *nmapped -= 1;
            PRRTE_RELEASE(procmax);
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
    prrte_output(0, "INFINITE LOOP");
}

static int assign_locations(prrte_job_t *jdata)
{
    int i, j, m, n;
    prrte_mca_base_component_t *c=&prrte_rmaps_ppr_component.base_version;
    prrte_node_t *node;
    prrte_proc_t *proc;
    prrte_app_context_t *app;
    hwloc_obj_type_t level;
    hwloc_obj_t obj;
    unsigned int cache_level=0;
    int ppr, cnt, nobjs, nprocs_mapped;
    char **ppr_req, **ck;

    if (NULL == jdata->map->last_mapper ||
        0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:ppr: job %s not using ppr assign: %s",
                            PRRTE_JOBID_PRINT(jdata->jobid),
                            (NULL == jdata->map->last_mapper) ? "NULL" : jdata->map->last_mapper);
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:ppr: assigning locations for job %s with ppr %s policy %s",
                        PRRTE_JOBID_PRINT(jdata->jobid), jdata->map->ppr,
                        prrte_rmaps_base_print_mapping(jdata->map->mapping));

    /* pickup the object level */
    if (PRRTE_MAPPING_BYNODE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_MACHINE;
    } else if (PRRTE_MAPPING_BYHWTHREAD == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_PU;
    } else if (PRRTE_MAPPING_BYCORE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_CORE;
    } else if (PRRTE_MAPPING_BYSOCKET == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_SOCKET;
    } else if (PRRTE_MAPPING_BYL1CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L1CACHE;
        cache_level = 1;
    } else if (PRRTE_MAPPING_BYL2CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L2CACHE;
        cache_level = 2;
    } else if (PRRTE_MAPPING_BYL3CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_L3CACHE;
        cache_level = 3;
    } else if (PRRTE_MAPPING_BYNUMA == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        level = HWLOC_OBJ_NUMANODE;
    } else {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* get the ppr value */
    ppr_req = prrte_argv_split(jdata->map->ppr, ',');
    ck = prrte_argv_split(ppr_req[0], ':');
    ppr = strtol(ck[0], NULL, 10);
    prrte_argv_free(ck);
    prrte_argv_free(ppr_req);

    /* start assigning procs to objects, filling each object as we go until
     * all procs are assigned. */
    for (n=0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        nprocs_mapped = 0;
        for (m=0; m < jdata->map->nodes->size; m++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            if (NULL == node->topology || NULL == node->topology->topo) {
                prrte_show_help("help-prrte-rmaps-ppr.txt", "ppr-topo-missing",
                               true, node->name);
                return PRRTE_ERR_SILENT;
            }
            if (HWLOC_OBJ_MACHINE == level) {
                obj = hwloc_get_root_obj(node->topology->topo);
                for (j=0; j < node->procs->size; j++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    if (proc->name.jobid != jdata->jobid) {
                        continue;
                    }
                    prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
                }
            } else {
                /* get the number of resources on this node at this level */
                nobjs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                           level, cache_level,
                                                           PRRTE_HWLOC_AVAILABLE);

                /* map the specified number of procs to each such resource on this node,
                 * recording the locale of each proc so we know its cpuset
                 */
                for (i=0; i < nobjs; i++) {
                    cnt = 0;
                    obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo,
                                                          level, cache_level,
                                                          i, PRRTE_HWLOC_AVAILABLE);
                    for (j=0; j < node->procs->size && cnt < ppr && nprocs_mapped < app->num_procs; j++) {
                        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                            continue;
                        }
                        if (proc->name.jobid != jdata->jobid) {
                            continue;
                        }
                        /* if we already assigned it, then skip */
                        if (prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, NULL, PRRTE_PTR)) {
                            continue;
                        }
                        nprocs_mapped++;
                        cnt++;
                        prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
                    }
                }
            }
        }
    }
    return PRRTE_SUCCESS;
}
