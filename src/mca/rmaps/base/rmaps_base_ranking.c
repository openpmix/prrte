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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Huawei Technologies Co., Ltd.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/class/prte_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/threads/tsd.h"
#include "src/util/if.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "types.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int rank_span(prte_job_t *jdata, hwloc_obj_type_t target, unsigned cache_level)
{
    prte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc;
    pmix_rank_t num_ranked = 0;
    prte_node_t *node;
    prte_proc_t *proc, *pptr;
    pmix_rank_t vpid;
    int cnt;
    hwloc_obj_t locale;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_span: for job %s", PRTE_JOBID_PRINT(jdata->nspace));

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
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }

        cnt = 0;
        while (cnt < app->num_procs) {
            for (m = 0; m < jdata->map->nodes->size; m++) {
                if (NULL
                    == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                    continue;
                }
                /* get the number of objects - only consider those we can actually use */
                num_objs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                              cache_level);
                prte_output_verbose(
                    5, prte_rmaps_base_framework.framework_output,
                    "mca:rmaps:rank_span: found %d objects on node %s with %d procs", num_objs,
                    node->name, (int) node->num_procs);
                if (0 == num_objs) {
                    return PRTE_ERR_NOT_SUPPORTED;
                }

                /* for each object */
                for (i = 0; i < num_objs && cnt < app->num_procs; i++) {
                    obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target, cache_level,
                                                          i);

                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rank_span: working object %d", i);

                    /* cycle thru the procs on this node */
                    for (j = 0; j < node->procs->size && cnt < app->num_procs; j++) {
                        if (NULL
                            == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs,
                                                                                   j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_span skipping proc %s - from "
                                                "another job, num_ranked %d",
                                                PRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* tie proc to its job */
                        proc->job = jdata;
                        /* ignore procs that are already assigned */
                        if (PMIX_RANK_INVALID != proc->name.rank) {
                            continue;
                        }
                        /* ignore procs from other apps */
                        if (proc->app_idx != app->idx) {
                            continue;
                        }
                        /* protect against bozo case */
                        locale = NULL;
                        if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                                (void **) &locale, PMIX_POINTER)
                            || NULL == locale) {
                            /* all mappers are _required_ to set the locale where the proc
                             * has been mapped - it is therefore an error for this attribute
                             * not to be set. Likewise, only a programming error could allow
                             * the attribute to be set to a NULL value - however, we add that
                             * conditional here to silence any compiler warnings */
                            PRTE_ERROR_LOG(PRTE_ERROR);
                            return PRTE_ERROR;
                        }
                        /* ignore procs not on this object */
                        if (!hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                            prte_output_verbose(
                                5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_span: proc at position %d is not on object %d", j,
                                i);
                            continue;
                        }
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_span: assigning vpid %s",
                                            PRTE_VPID_PRINT(vpid));
                        proc->name.rank = vpid;
                        proc->rank = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.rank;
                        }
                        cnt++;

                        /* insert the proc into the jdata array */
                        if (NULL
                            != (pptr = (prte_proc_t *)
                                    prte_pointer_array_get_item(jdata->procs, proc->name.rank))) {
                            PRTE_RELEASE(pptr);
                        }
                        PRTE_RETAIN(proc);
                        if (PRTE_SUCCESS
                            != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank,
                                                                 proc))) {
                            PRTE_ERROR_LOG(rc);
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

        /* Are all the procs ranked? we don't want to crash on INVALID ranks */
        if (cnt < app->num_procs) {
            return PRTE_ERR_FAILED_TO_MAP;
        }
    }

    return PRTE_SUCCESS;
}

static int rank_fill(prte_job_t *jdata, hwloc_obj_type_t target, unsigned cache_level)
{
    prte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc;
    pmix_rank_t num_ranked = 0;
    prte_node_t *node;
    prte_proc_t *proc, *pptr;
    pmix_rank_t vpid;
    int cnt;
    hwloc_obj_t locale;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_fill: for job %s", PRTE_JOBID_PRINT(jdata->nspace));

    /* if the ranking is fill, then we rank all the procs
     * within a given object before moving on to the next
     *
     *        Node 0                Node 1
     *    Obj 0     Obj 1       Obj 0     Obj 1
     *     0 1       4 5         8 9      12 13
     *     2 3       6 7        10 11     14 15
     */

    vpid = 0;
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }

        cnt = 0;
        for (m = 0; m < jdata->map->nodes->size; m++) {
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            /* get the number of objects - only consider those we can actually use */
            num_objs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                          cache_level);
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_fill: found %d objects on node %s with %d procs",
                                num_objs, node->name, (int) node->num_procs);
            if (0 == num_objs) {
                return PRTE_ERR_NOT_SUPPORTED;
            }

            /* for each object */
            for (i = 0; i < num_objs && cnt < app->num_procs; i++) {
                obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target, cache_level, i);

                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rank_fill: working object %d", i);

                /* cycle thru the procs on this node */
                for (j = 0; j < node->procs->size && cnt < app->num_procs; j++) {
                    if (NULL
                        == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    /* ignore procs from other jobs */
                    if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_fill skipping proc %s - from another "
                                            "job, num_ranked %d",
                                            PRTE_NAME_PRINT(&proc->name), num_ranked);
                        continue;
                    }
                    /* tie proc to its job */
                    proc->job = jdata;
                    /* ignore procs that are already assigned */
                    if (PMIX_RANK_INVALID != proc->name.rank) {
                        continue;
                    }
                    /* ignore procs from other apps */
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                    /* protect against bozo case */
                    locale = NULL;
                    if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                            (void **) &locale, PMIX_POINTER)
                        || NULL == locale) {
                        /* all mappers are _required_ to set the locale where the proc
                         * has been mapped - it is therefore an error for this attribute
                         * not to be set. Likewise, only a programming error could allow
                         * the attribute to be set to a NULL value - however, we add that
                         * conditional here to silence any compiler warnings */
                        PRTE_ERROR_LOG(PRTE_ERROR);
                        return PRTE_ERROR;
                    }
                    /* ignore procs not on this object */
                    if (!hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                        prte_output_verbose(
                            5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rank_fill: proc at position %d is not on object %d", j, i);
                        continue;
                    }
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rank_fill: assigning vpid %s",
                                        PRTE_VPID_PRINT(vpid));
                    proc->name.rank = vpid;
                    proc->rank = vpid++;
                    if (0 == cnt) {
                        app->first_rank = proc->name.rank;
                    }
                    cnt++;

                    /* insert the proc into the jdata array */
                    if (NULL
                        != (pptr = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs,
                                                                               proc->name.rank))) {
                        PRTE_RELEASE(pptr);
                    }
                    PRTE_RETAIN(proc);
                    if (PRTE_SUCCESS
                        != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank,
                                                             proc))) {
                        PRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    /* track where the highest vpid landed - this is our
                     * new bookmark
                     */
                    jdata->bookmark = node;
                }
            }
        }

        /* Are all the procs ranked? we don't want to crash on INVALID ranks */
        if (cnt < app->num_procs) {
            return PRTE_ERR_FAILED_TO_MAP;
        }
    }

    return PRTE_SUCCESS;
}

static int rank_by(prte_job_t *jdata, hwloc_obj_type_t target, unsigned cache_level)
{
    prte_app_context_t *app;
    hwloc_obj_t obj;
    int num_objs, i, j, m, n, rc, nn;
    pmix_rank_t num_ranked = 0;
    prte_node_t *node;
    prte_proc_t *proc, *pptr;
    pmix_rank_t vpid;
    int cnt;
    prte_pointer_array_t objs;
    hwloc_obj_t locale;
    prte_app_idx_t napp;
    bool noassign;

    if (PRTE_RANKING_SPAN & PRTE_GET_RANKING_DIRECTIVE(jdata->map->ranking)) {
        return rank_span(jdata, target, cache_level);
    } else if (PRTE_RANKING_FILL & PRTE_GET_RANKING_DIRECTIVE(jdata->map->ranking)) {
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
    for (n = 0, napp = 0; napp < jdata->num_apps && n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        napp++;
        /* setup the pointer array */
        PRTE_CONSTRUCT(&objs, prte_pointer_array_t);
        prte_pointer_array_init(&objs, 2, INT_MAX, 2);

        cnt = 0;
        for (m = 0, nn = 0; nn < jdata->map->num_nodes && m < jdata->map->nodes->size; m++) {
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            nn++;

            /* get the number of objects - only consider those we can actually use */
            num_objs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target,
                                                          cache_level);
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_by: found %d objects on node %s with %d procs",
                                num_objs, node->name, (int) node->num_procs);
            if (0 == num_objs) {
                PRTE_DESTRUCT(&objs);
                return PRTE_ERR_NOT_SUPPORTED;
            }
            /* collect all the objects */
            for (i = 0; i < num_objs; i++) {
                obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target, cache_level, i);
                prte_pointer_array_set_item(&objs, i, obj);
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
                for (i = 0; i < num_objs && cnt < app->num_procs; i++) {
                    /* get the next object */
                    obj = (hwloc_obj_t) prte_pointer_array_get_item(&objs, i);
                    if (NULL == obj) {
                        break;
                    }
                    /* scan across the procs and find the first unassigned one that includes this
                     * object */
                    for (j = 0; j < node->procs->size && cnt < app->num_procs; j++) {
                        if (NULL
                            == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs,
                                                                                   j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - from another "
                                                "job, num_ranked %d",
                                                PRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* ignore procs that are already ranked */
                        if (PMIX_RANK_INVALID != proc->name.rank) {
                            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - already "
                                                "ranked, num_ranked %d",
                                                PRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* ignore procs from other apps - we will get to them */
                        if (proc->app_idx != app->idx) {
                            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps:rank_by skipping proc %s - from another "
                                                "app, num_ranked %d",
                                                PRTE_NAME_PRINT(&proc->name), num_ranked);
                            continue;
                        }
                        /* tie proc to its job */
                        proc->job = jdata;
                        /* protect against bozo case */
                        locale = NULL;
                        if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                                (void **) &locale, PMIX_POINTER)
                            || NULL == locale) {
                            /* all mappers are _required_ to set the locale where the proc
                             * has been mapped - it is therefore an error for this attribute
                             * not to be set. Likewise, only a programming error could allow
                             * the attribute to be set to a NULL value - however, we add that
                             * conditional here to silence any compiler warnings */
                            PRTE_ERROR_LOG(PRTE_ERROR);
                            return PRTE_ERROR;
                        }
                        /* ignore procs not on this object */
                        if (!hwloc_bitmap_intersects(obj->cpuset, locale->cpuset)) {
                            prte_output_verbose(
                                5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rank_by: proc at position %d is not on object %d", j, i);
                            continue;
                        }
                        /* assign the vpid */
                        proc->name.rank = vpid;
                        proc->rank = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.rank;
                        }
                        cnt++;
                        noassign = false;
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:rank_by: proc in position %d is on object "
                                            "%d assigned rank %s",
                                            j, i, PRTE_VPID_PRINT(proc->name.rank));
                        /* insert the proc into the jdata array */
                        if (NULL
                            != (pptr = (prte_proc_t *)
                                    prte_pointer_array_get_item(jdata->procs, proc->name.rank))) {
                            PRTE_RELEASE(pptr);
                        }
                        PRTE_RETAIN(proc);
                        if (PRTE_SUCCESS
                            != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank,
                                                                 proc))) {
                            PRTE_ERROR_LOG(rc);
                            PRTE_DESTRUCT(&objs);
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
        PRTE_DESTRUCT(&objs);

        /* Are all the procs ranked? we don't want to crash on INVALID ranks */
        if (cnt < app->num_procs) {
            return PRTE_ERR_FAILED_TO_MAP;
        }
    }
    return PRTE_SUCCESS;
}

int prte_rmaps_base_compute_vpids(prte_job_t *jdata)
{
    prte_job_map_t *map;
    prte_app_context_t *app;
    pmix_rank_t vpid;
    int j, m, n, cnt;
    prte_node_t *node;
    prte_proc_t *proc, *pptr;
    int rc;
    bool one_found;
    hwloc_obj_type_t target;
    unsigned cache_level;

    map = jdata->map;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output, "RANKING POLICY: %s",
                        prte_rmaps_base_print_ranking(map->ranking));

    /* start with the rank-by object options - if the object isn't
     * included in the topology, then we obviously cannot rank by it.
     * However, if this was the default ranking policy (as opposed to
     * something given by the user), then fall back to rank-by slot
     */
    if (PRTE_RANK_BY_PACKAGE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by package for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        if (PRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_PACKAGE, 0))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_L3CACHE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L3cache for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        PRTE_HWLOC_MAKE_OBJ_CACHE(3, target, cache_level);
        if (PRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_L2CACHE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L2cache for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        PRTE_HWLOC_MAKE_OBJ_CACHE(2, target, cache_level);
        if (PRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_L1CACHE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by L1cache for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        PRTE_HWLOC_MAKE_OBJ_CACHE(1, target, cache_level);
        if (PRTE_SUCCESS != (rc = rank_by(jdata, target, cache_level))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_CORE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by core for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        if (PRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_CORE, 0))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_HWTHREAD == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: computing ranks by hwthread for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        if (PRTE_SUCCESS != (rc = rank_by(jdata, HWLOC_OBJ_PU, 0))) {
            if (PRTE_ERR_NOT_SUPPORTED == rc
                && !(PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(map->ranking))) {
                PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
                goto rankbyslot;
            }
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PRTE_RANK_BY_NODE == PRTE_GET_RANKING_POLICY(map->ranking)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:base: computing vpids by node for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        /* assign the ranks round-robin across nodes - only one board/node
         * at this time, so they are equivalent
         */
        vpid = 0;
        for (n = 0; n < jdata->apps->size; n++) {
            if (NULL
                == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
                continue;
            }
            cnt = 0;
            one_found = true;
            while (cnt < app->num_procs && one_found) {
                one_found = false;
                for (m = 0; m < jdata->map->nodes->size; m++) {
                    if (NULL
                        == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes,
                                                                               m))) {
                        continue;
                    }
                    for (j = 0; j < node->procs->size; j++) {
                        if (NULL
                            == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs,
                                                                                   j))) {
                            continue;
                        }
                        /* ignore procs from other jobs */
                        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                            continue;
                        }
                        /* tie proc to its job */
                        proc->job = jdata;
                        /* ignore procs from other apps */
                        if (proc->app_idx != app->idx) {
                            continue;
                        }
                        if (PMIX_RANK_INVALID != proc->name.rank) {
                            continue;
                        }
                        proc->name.rank = vpid;
                        proc->rank = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.rank;
                        }
                        cnt++;

                        /* insert the proc into the jdata array */
                        if (NULL
                            != (pptr = (prte_proc_t *)
                                    prte_pointer_array_get_item(jdata->procs, proc->name.rank))) {
                            PRTE_RELEASE(pptr);
                        }
                        PRTE_RETAIN(proc);
                        if (PRTE_SUCCESS
                            != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank,
                                                                 proc))) {
                            PRTE_ERROR_LOG(rc);
                            return rc;
                        }
                        one_found = true;
                        /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                        break; /* move on to next node */
                    }
                }
            }
            if (cnt < app->num_procs) {
                PRTE_ERROR_LOG(PRTE_ERR_FATAL);
                return PRTE_ERR_FATAL;
            }
        }
        return PRTE_SUCCESS;
    }

rankbyslot:
    if (PRTE_RANK_BY_SLOT == PRTE_GET_RANKING_POLICY(map->ranking)) {
        /* assign the ranks sequentially */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:base: computing vpids by slot for job %s",
                            PRTE_JOBID_PRINT(jdata->nspace));
        vpid = 0;
        for (n = 0; n < jdata->apps->size; n++) {
            cnt = 0;
            if (NULL
                == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
                continue;
            }
            for (m = 0; m < jdata->map->nodes->size; m++) {
                if (NULL
                    == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                    continue;
                }

                for (j = 0; j < node->procs->size; j++) {
                    if (NULL
                        == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                        continue;
                    }
                    /* ignore procs from other jobs */
                    if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                        continue;
                    }
                    /* tie proc to its job */
                    proc->job = jdata;
                    /* ignore procs from other apps */
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                    if (PMIX_RANK_INVALID == proc->name.rank) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:base: assigning rank %s to node %s",
                                            PRTE_VPID_PRINT(vpid), node->name);
                        proc->name.rank = vpid;
                        proc->rank = vpid++;
                        if (0 == cnt) {
                            app->first_rank = proc->name.rank;
                        }
                        cnt++;

                        /* track where the highest vpid landed - this is our
                         * new bookmark
                         */
                        jdata->bookmark = node;
                    }
                    /* insert the proc into the jdata array */
                    if (NULL
                        != (pptr = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs,
                                                                               proc->name.rank))) {
                        PRTE_RELEASE(pptr);
                    }
                    PRTE_RETAIN(proc);
                    if (PRTE_SUCCESS
                        != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank,
                                                             proc))) {
                        PRTE_ERROR_LOG(rc);
                        return rc;
                    }
                }
            }
        }
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_NOT_IMPLEMENTED;
}

int prte_rmaps_base_compute_local_ranks(prte_job_t *jdata)
{
    int32_t i;
    int j, k;
    prte_node_t *node;
    prte_proc_t *proc, *psave, *psave2;
    pmix_rank_t minv, minv2;
    prte_local_rank_t local_rank;
    prte_job_map_t *map;
    prte_app_context_t *app;

    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:base:compute_usage", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* point to map */
    map = jdata->map;

    /* for each node in the map... */
    for (i = 0; i < map->nodes->size; i++) {
        /* cycle through the array of procs on this node, setting
         * local and node ranks, until we
         * have done so for all procs on nodes in this map
         */
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }

        /* init search values */
        local_rank = 0;

        /* the proc map may have holes in it, so cycle
         * all the way through and avoid the holes
         */
        for (k = 0; k < node->procs->size; k++) {
            /* if this proc is NULL, skip it */
            if (NULL == prte_pointer_array_get_item(node->procs, k)) {
                continue;
            }
            minv = PMIX_RANK_VALID;
            minv2 = PMIX_RANK_VALID;
            psave = NULL;
            psave2 = NULL;
            /* find the minimum vpid proc */
            for (j = 0; j < node->procs->size; j++) {
                /* if this proc is NULL, skip it */
                if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                /* only look at procs for this job when
                 * determining local rank
                 */
                if (PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)
                    && PRTE_LOCAL_RANK_INVALID == proc->local_rank && proc->name.rank < minv) {
                    minv = proc->name.rank;
                    psave = proc;
                }
                /* no matter what job...still have to handle node_rank */
                if (PRTE_NODE_RANK_INVALID == proc->node_rank && proc->name.rank < minv2) {
                    minv2 = proc->name.rank;
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
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        k = 0;
        /* loop thru all procs in job to find those from this app_context */
        for (j = 0; j < jdata->procs->size; j++) {
            if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, j))) {
                continue;
            }
            if (proc->app_idx != app->idx) {
                continue;
            }
            proc->app_rank = k++;
        }
    }

    return PRTE_SUCCESS;
}

/* when we restart a process on a different node, we have to
 * ensure that the node and local ranks assigned to the proc
 * don't overlap with any pre-existing proc on that node. If
 * we don't, then it would be possible for procs to conflict
 * when opening static ports, should that be enabled.
 */
void prte_rmaps_base_update_local_ranks(prte_job_t *jdata, prte_node_t *oldnode,
                                        prte_node_t *newnode, prte_proc_t *newproc)
{
    int k;
    prte_node_rank_t node_rank;
    prte_local_rank_t local_rank;
    prte_proc_t *proc;

    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:base:update_usage", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

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
    for (k = 0; k < newnode->procs->size; k++) {
        /* if this proc is NULL, skip it */
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(newnode->procs, k))) {
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
    for (k = 0; k < newnode->procs->size; k++) {
        /* if this proc is NULL, skip it */
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(newnode->procs, k))) {
            continue;
        }
        /* ignore procs from other jobs */
        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
            continue;
        }
        if (local_rank == proc->local_rank) {
            local_rank++;
            goto retry_lr;
        }
    }
    newproc->local_rank = local_rank;
}
