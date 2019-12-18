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
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/class/prrte_pointer_array.h"
#include "src/util/if.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/threads/tsd.h"

#include "types.h"
#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"

static int rank_span(prrte_job_t *jdata,
                     hwloc_obj_type_t target,
                     unsigned cache_level)
{
    prrte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc;
    prrte_vpid_t num_ranked=0;
    prrte_node_t *node;
    prrte_proc_t *proc, *pptr;
    prrte_vpid_t vpid;
    int cnt;
    hwloc_obj_t locale;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_span: for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* if the ranking is spanned, then we perform the
     * ranking as if it was one big node - i.e., we
     * rank one proc on each object, step to the next object
     * moving across all the nodes, then wrap around to the
     * first object on the first node.
     *
     *        Node 0                Node 1
     *    Obj 0     Obj 1       Obj 0     Obj 1
     *     0 4       1 5         2 6       3 7
     *     8 12      9 13       10 14     11 15
     */

    /* In the interest of getting this committed in finite time,
     * just loop across the nodes and objects until all procs
     * are mapped
     */

    vpid = 0;
    for (n=0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }

        cnt = 0;
        while (cnt < app->num_procs) {
            for (m=0; m < jdata->map->nodes->size; m++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                    continue;
                }
                /* get the number of objects - only consider those we can actually use */
                num_objs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                              cache_level, PRRTE_HWLOC_AVAILABLE);
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rank_span: found %d objects on node %s with %d procs",
                                    num_objs, node->name, (int)node->num_procs);
                if (0 == num_objs) {
                    return PRRTE_ERR_NOT_SUPPORTED;
                }

                /* for each object */
                for (i=0; i < num_objs && cnt < app->num_procs; i++) {
                    obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo, target,
                                                          cache_level, i, PRRTE_HWLOC_AVAILABLE);

                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rank_span: working object %d", i);

                    /* cycle thru the procs on this node */
                    for (j=0; j < node->procs->size && cnt < app->num_procs; j++) {
                        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (proc->name.jobid != jdata->jobid) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_span skipping proc %s - from another job, num_ranked %d",
                                                PRRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* ignore procs that are already assigned */
                        if (PRRTE_VPID_INVALID != proc->name.vpid) {
                            continue;
                        }
                        /* ignore procs from other apps */
                        if (proc->app_idx != app->idx) {
                            continue;
                        }
                        /* protect against bozo case */
                        locale = NULL;
                        if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                            PRRTE_ERROR_LOG(PRRTE_ERROR);
                            return PRRTE_ERROR;
                        }
                        /* ignore procs not on this object */
                        if (!hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_span: proc at position %d is not on object %d",
                                                j, i);
                            continue;
                        }
                        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_span: assigning vpid %s", PRRTE_VPID_PRINT(vpid));
                        proc->name.vpid = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.vpid;
                        }
                        cnt++;

                        /* insert the proc into the jdata array */
                        if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->name.vpid))) {
                            PRRTE_RELEASE(pptr);
                        }
                        PRRTE_RETAIN(proc);
                        if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                            PRRTE_ERROR_LOG(rc);
                            return rc;
                        }
                        /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                        /* move to next object */
                        break;
                    }
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

static int rank_fill(prrte_job_t *jdata,
                     hwloc_obj_type_t target,
                     unsigned cache_level)
{
    prrte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc;
    prrte_vpid_t num_ranked=0;
    prrte_node_t *node;
    prrte_proc_t *proc, *pptr;
    prrte_vpid_t vpid;
    int cnt;
    hwloc_obj_t locale;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_fill: for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* if the ranking is fill, then we rank all the procs
     * within a given object before moving on to the next
     *
     *        Node 0                Node 1
     *    Obj 0     Obj 1       Obj 0     Obj 1
     *     0 1       4 5         8 9      12 13
     *     2 3       6 7        10 11     14 15
     */

    vpid = 0;
    for (n=0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }

        cnt = 0;
        for (m=0; m < jdata->map->nodes->size; m++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            /* get the number of objects - only consider those we can actually use */
            num_objs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                          cache_level, PRRTE_HWLOC_AVAILABLE);
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_fill: found %d objects on node %s with %d procs",
                                num_objs, node->name, (int)node->num_procs);
            if (0 == num_objs) {
                return PRRTE_ERR_NOT_SUPPORTED;
            }

            /* for each object */
            for (i=0; i < num_objs && cnt < app->num_procs; i++) {
                obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo, target,
                                                      cache_level, i, PRRTE_HWLOC_AVAILABLE);

                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rank_fill: working object %d", i);

                /* cycle thru the procs on this node */
                for (j=0; j < node->procs->size && cnt < app->num_procs; j++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    /* ignore procs from other jobs */
                    if (proc->name.jobid != jdata->jobid) {
                        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_fill skipping proc %s - from another job, num_ranked %d",
                                            PRRTE_NAME_PRINT(&proc->name), num_ranked);
                        continue;
                    }
                    /* ignore procs that are already assigned */
                    if (PRRTE_VPID_INVALID != proc->name.vpid) {
                        continue;
                    }
                    /* ignore procs from other apps */
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                     /* protect against bozo case */
                    locale = NULL;
                    if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                        PRRTE_ERROR_LOG(PRRTE_ERROR);
                        return PRRTE_ERROR;
                    }
                    /* ignore procs not on this object */
                    if (!hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_fill: proc at position %d is not on object %d",
                                            j, i);
                        continue;
                    }
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rank_fill: assigning vpid %s", PRRTE_VPID_PRINT(vpid));
                    proc->name.vpid = vpid++;
                    if (0 == cnt) {
                        app->first_rank = proc->name.vpid;
                    }
                    cnt++;

                    /* insert the proc into the jdata array */
                    if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->name.vpid))) {
                        PRRTE_RELEASE(pptr);
                    }
                    PRRTE_RETAIN(proc);
                    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                        PRRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    /* track where the highest vpid landed - this is our
                     * new bookmark
                     */
                    jdata->bookmark = node;
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

static int rank_by(prrte_job_t *jdata,
                   hwloc_obj_type_t target,
                   unsigned cache_level)
{
    prrte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc, nn;
    prrte_vpid_t num_ranked=0;
    prrte_node_t *node;
    prrte_proc_t *proc, *pptr;
    prrte_vpid_t vpid;
    int cnt;
    prrte_pointer_array_t objs;
    hwloc_obj_t locale;
    prrte_app_idx_t napp;
    bool noassign;

    if (PRRTE_RANKING_SPAN & PRRTE_GET_RANKING_DIRECTIVE(jdata->map->ranking)) {
        return rank_span(jdata, target, cache_level);
    } else if (PRRTE_RANKING_FILL & PRRTE_GET_RANKING_DIRECTIVE(jdata->map->ranking)) {
        return rank_fill(jdata, target, cache_level);
    }

    /* if ranking is not spanned or filled, then we
     * default to assign ranks sequentially across
     * target objects within a node until that node
     * is fully ranked, and then move on to the next
     * node
     *
     *        Node 0                Node 1
     *    Obj 0     Obj 1       Obj 0     Obj 1
     *     0 2       1 3         8 10      9 11
     *     4 6       5 7        12 14     13 15
     */
    vpid = 0;
    for (n=0, napp=0; napp < jdata->num_apps && n < jdata->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        napp++;
        /* setup the pointer array */
        PRRTE_CONSTRUCT(&objs, prrte_pointer_array_t);
        prrte_pointer_array_init(&objs, 2, INT_MAX, 2);

        cnt = 0;
        for (m=0, nn=0; nn < jdata->map->num_nodes && m < jdata->map->nodes->size; m++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            nn++;

            /* get the number of objects - only consider those we can actually use */
            num_objs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                          cache_level, PRRTE_HWLOC_AVAILABLE);
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_by: found %d objects on node %s with %d procs",
                                num_objs, node->name, (int)node->num_procs);
            if (0 == num_objs) {
                PRRTE_DESTRUCT(&objs);
                return PRRTE_ERR_NOT_SUPPORTED;
            }
            /* collect all the objects */
            for (i=0; i < num_objs; i++) {
                obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo, target,
                                                      cache_level, i, PRRTE_HWLOC_AVAILABLE);
                prrte_pointer_array_set_item(&objs, i, obj);
            }

            /* cycle across the objects, assigning a proc to each one,
             * until all procs have been assigned - unfortunately, since
             * more than this job may be mapped onto a node, the number
             * of procs on the node can't be used to tell us when we
             * are done. Instead, we have to just keep going until all
             * procs are ranked - which means we have to make one extra
             * pass thru the loop. In addition, if we pass thru the entire
             * loop without assigning anything then we are done
             *
             * Perhaps someday someone will come up with a more efficient
             * algorithm, but this works for now.
             */
            while (cnt < app->num_procs) {
                noassign = true;
                for (i=0; i < num_objs && cnt < app->num_procs; i++) {
                    /* get the next object */
                    obj = (hwloc_obj_t)prrte_pointer_array_get_item(&objs, i % num_objs);
                    if (NULL == obj) {
                        break;
                    }
                    /* scan across the procs and find the one that is on this object */
                    for (j=0; j < node->procs->size && cnt < app->num_procs; j++) {
                        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (proc->name.jobid != jdata->jobid) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - from another job, num_ranked %d",
                                                PRRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* ignore procs that are already ranked */
                        if (PRRTE_VPID_INVALID != proc->name.vpid) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - already ranked, num_ranked %d",
                                                PRRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* ignore procs from other apps */
                        if (proc->app_idx != app->idx) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - from another app, num_ranked %d",
                                                PRRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                         /* protect against bozo case */
                        locale = NULL;
                        if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                            PRRTE_ERROR_LOG(PRRTE_ERROR);
                            return PRRTE_ERROR;
                        }
                        /* ignore procs not on this object */
                        if (NULL == locale ||
                            !hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by: proc at position %d is not on object %d",
                                                j, i);
                            continue;
                        }
                        /* assign the vpid */
                        proc->name.vpid = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.vpid;
                        }
                        cnt++;
                        noassign = false;
                        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_by: proc in position %d is on object %d assigned rank %s",
                                            j, i, PRRTE_VPID_PRINT(proc->name.vpid));
                        /* insert the proc into the jdata array */
                        if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->name.vpid))) {
                            PRRTE_RELEASE(pptr);
                        }
                        PRRTE_RETAIN(proc);
                        if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                            PRRTE_ERROR_LOG(rc);
                            PRRTE_DESTRUCT(&objs);
                            return rc;
                        }
                        num_ranked++;
                        /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                        /* move to next object */
                        break;
                    }
                }
                if (noassign) {
                    break;
                }
            }
        }
        /* cleanup */
        PRRTE_DESTRUCT(&objs);
    }
    return PRRTE_SUCCESS;
}

int prrte_rmaps_base_compute_vpids(prrte_job_t *jdata)
{
    prrte_job_map_t *map;
    prrte_app_context_t *app;
    prrte_vpid_t vpid;
    int j, m, n, cnt;
    prrte_node_t *node;
    prrte_proc_t *proc, *pptr;
    int rc;
    bool one_found;
    hwloc_obj_type_t target;
    unsigned cache_level;

    map = jdata->map;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "RANKING POLICY: %s", prrte_rmaps_base_print_ranking(map->ranking));

    /* start with the rank-by object options - if the object isn't
     * included in the topology, then we obviously cannot rank by it.
     * However, if this was the default ranking policy (as opposed to
     * something given by the user), then fall back to rank-by slot
     */
    if (PRRTE_RANK_BY_NUMA == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by NUMA for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_NODE, 0))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_SOCKET == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by socket for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_SOCKET, 0))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_L3CACHE == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L3cache for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        PRRTE_HWLOC_MAKE_OBJ_CACHE(3, target, cache_level);
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_L2CACHE == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L2cache for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        PRRTE_HWLOC_MAKE_OBJ_CACHE(2, target, cache_level);
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_L1CACHE == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L1cache for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        PRRTE_HWLOC_MAKE_OBJ_CACHE(1, target, cache_level);
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_CORE == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by core for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_CORE, 0))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_HWTHREAD == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by hwthread for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        if (PRRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_PU, 0))) {
            if (PRRTE_ERR_NOT_SUPPORTED == rc &&
                !(PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRRTE_SET_RANKING_POLICY(map->ranking, PRRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRRTE_RANK_BY_NODE == PRRTE_GET_RANKING_POLICY(map->ranking) ||
        PRRTE_RANK_BY_BOARD == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:base: computing vpids by node for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        /* assign the ranks round-robin across nodes - only one board/node
         * at this time, so they are equivalent
         */
        vpid=0;
        for (n=0; n < jdata->apps->size; n++) {
            if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
                continue;
            }
            cnt=0;
            one_found = true;
            while (cnt < app->num_procs && one_found) {
                one_found = false;
                for (m=0; m < jdata->map->nodes->size; m++) {
                    if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                        continue;
                    }
                    for (j=0; j < node->procs->size; j++) {
                        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (proc->name.jobid != jdata->jobid) {
                            continue;
                        }
                        /* ignore procs from other apps */
                        if (proc->app_idx != app->idx) {
                            continue;
                        }
                        if (PRRTE_VPID_INVALID != proc->name.vpid) {
                            continue;
                        }
                        proc->name.vpid = vpid++;
                        /* insert the proc into the jdata array */
                        if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->name.vpid))) {
                            PRRTE_RELEASE(pptr);
                        }
                        PRRTE_RETAIN(proc);
                        if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                            PRRTE_ERROR_LOG(rc);
                            return rc;
                        }
                        cnt++;
                        one_found = true;
                        /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                        break;  /* move on to next node */
                    }
                }
            }
            if (cnt < app->num_procs) {
                PRRTE_ERROR_LOG(PRRTE_ERR_FATAL);
                return PRRTE_ERR_FATAL;
            }
        }
        return PRRTE_SUCCESS;
    }

  rankbyslot:
    if (PRRTE_RANK_BY_SLOT == PRRTE_GET_RANKING_POLICY(map->ranking)) {
        /* assign the ranks sequentially */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:base: computing vpids by slot for job %s",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        vpid = 0;
        for (n=0; n < jdata->apps->size; n++) {
            if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
                continue;
            }
            for (m=0; m < jdata->map->nodes->size; m++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                    continue;
                }

                for (j=0; j < node->procs->size; j++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    /* ignore procs from other jobs */
                    if (proc->name.jobid != jdata->jobid) {
                        continue;
                    }
                    /* ignore procs from other apps */
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                    if (PRRTE_VPID_INVALID == proc->name.vpid) {
                        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:base: assigning rank %s to node %s",
                                            PRRTE_VPID_PRINT(vpid), node->name);
                        proc->name.vpid = vpid++;
                       /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                    }
                    /* insert the proc into the jdata array */
                    if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->name.vpid))) {
                        PRRTE_RELEASE(pptr);
                    }
                    PRRTE_RETAIN(proc);
                    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                        PRRTE_ERROR_LOG(rc);
                        return rc;
                    }
                }
            }
        }
        return PRRTE_SUCCESS;
    }

    return PRRTE_ERR_NOT_IMPLEMENTED;
}

int prrte_rmaps_base_compute_local_ranks(prrte_job_t *jdata)
{
    prrte_std_cntr_t i;
    int j, k;
    prrte_node_t *node;
    prrte_proc_t *proc, *psave, *psave2;
    prrte_vpid_t minv, minv2;
    prrte_local_rank_t local_rank;
    prrte_job_map_t *map;
    prrte_app_context_t *app;

    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:base:compute_usage",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* point to map */
    map = jdata->map;

    /* for each node in the map... */
    for (i=0; i < map->nodes->size; i++) {
        /* cycle through the array of procs on this node, setting
         * local and node ranks, until we
         * have done so for all procs on nodes in this map
         */
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }

        /* init search values */
        local_rank = 0;

        /* the proc map may have holes in it, so cycle
         * all the way through and avoid the holes
         */
        for (k=0; k < node->procs->size; k++) {
            /* if this proc is NULL, skip it */
            if (NULL == prrte_pointer_array_get_item(node->procs, k)) {
                continue;
            }
            minv = PRRTE_VPID_MAX;
            minv2 = PRRTE_VPID_MAX;
            psave = NULL;
            psave2 = NULL;
            /* find the minimum vpid proc */
            for (j=0; j < node->procs->size; j++) {
                /* if this proc is NULL, skip it */
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                /* only look at procs for this job when
                 * determining local rank
                 */
                if (proc->name.jobid == jdata->jobid &&
                    PRRTE_LOCAL_RANK_INVALID == proc->local_rank &&
                    proc->name.vpid < minv) {
                    minv = proc->name.vpid;
                    psave = proc;
                }
                /* no matter what job...still have to handle node_rank */
                if (PRRTE_NODE_RANK_INVALID == proc->node_rank &&
                    proc->name.vpid < minv2) {
                    minv2 = proc->name.vpid;
                    psave2 = proc;
                }
            }
            if (NULL == psave && NULL == psave2) {
                /* we must have processed them all for this node! */
                break;
            }
            if (NULL != psave) {
                psave->local_rank = local_rank;
                ++local_rank;
            }
            if (NULL != psave2) {
                psave2->node_rank = node->next_node_rank;
                node->next_node_rank++;
            }
        }
    }

    /* compute app_rank */
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        k=0;
        /* loop thru all procs in job to find those from this app_context */
        for (j=0; j < jdata->procs->size; j++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, j))) {
                continue;
            }
            if (proc->app_idx != app->idx) {
                continue;
            }
            proc->app_rank = k++;
        }
    }

    return PRRTE_SUCCESS;
}

/* when we restart a process on a different node, we have to
 * ensure that the node and local ranks assigned to the proc
 * don't overlap with any pre-existing proc on that node. If
 * we don't, then it would be possible for procs to conflict
 * when opening static ports, should that be enabled.
 */
void prrte_rmaps_base_update_local_ranks(prrte_job_t *jdata, prrte_node_t *oldnode,
                                        prrte_node_t *newnode, prrte_proc_t *newproc)
{
    int k;
    prrte_node_rank_t node_rank;
    prrte_local_rank_t local_rank;
    prrte_proc_t *proc;

    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:base:update_usage",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* if the node hasn't changed, then we can just use the
     * pre-defined values
     */
    if (oldnode == newnode) {
        return;
    }

    /* if the node has changed, then search the new node for the
     * lowest unused local and node rank
     */
    node_rank = 0;
retry_nr:
    for (k=0; k < newnode->procs->size; k++) {
        /* if this proc is NULL, skip it */
        if (NULL == (proc = (prrte_proc_t *) prrte_pointer_array_get_item(newnode->procs, k))) {
            continue;
        }
        if (node_rank == proc->node_rank) {
            node_rank++;
            goto retry_nr;
        }
    }
    newproc->node_rank = node_rank;

    local_rank = 0;
retry_lr:
    for (k=0; k < newnode->procs->size; k++) {
        /* if this proc is NULL, skip it */
        if (NULL == (proc = (prrte_proc_t *) prrte_pointer_array_get_item(newnode->procs, k))) {
            continue;
        }
        /* ignore procs from other jobs */
        if (proc->name.jobid != jdata->jobid) {
            continue;
        }
        if (local_rank == proc->local_rank) {
            local_rank++;
            goto retry_lr;
        }
    }
    newproc->local_rank = local_rank;
}
