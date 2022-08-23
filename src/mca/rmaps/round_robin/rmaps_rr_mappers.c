/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "rmaps_rr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

int prte_rmaps_rr_byslot(prte_job_t *jdata,
                         prte_app_context_t *app,
                         pmix_list_t *node_list,
                         int32_t num_slots,
                         pmix_rank_t num_procs,
                         prte_rmaps_options_t *options)
{
    int i, rc, nprocs_mapped, ncpus;
    prte_node_t *node, *nd;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    float balance;
    prte_proc_t *proc;
    bool second_pass = false;
    prte_binding_policy_t savebind = options->bind;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by slot for job %s slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) num_slots,
                        (unsigned long) num_procs);

    /* check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    nprocs_mapped = 0;

pass:
    PMIX_LIST_FOREACH_SAFE(node, nd, node_list, prte_node_t)
    {
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s", node->name);

        prte_rmaps_base_get_cpuset(jdata, node, options);

        /* compute the number of procs to go on this node */
        if (second_pass) {
            options->nprocs = extra_procs_to_assign;
            if (0 < nxtra_nodes) {
                --nxtra_nodes;
                if (0 == nxtra_nodes) {
                    --extra_procs_to_assign;
                }
            }
        } else {
            if (!options->donotlaunch) {
                rc = prte_rmaps_base_check_support(jdata, node, options);
                if (PRTE_SUCCESS != rc) {
                    return rc;
                }
            }
            /* assign a number of procs equal to the number of available slots */
            if (!PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                options->nprocs = node->slots_available;
            } else {
                options->nprocs = node->slots;
            }
        }

        if (!options->oversubscribe) {
            /* since oversubscribe is not allowed, cap our usage
             * at the number of available slots. */
            if (node->slots_available < options->nprocs) {
                options->nprocs = node->slots_available;
            }
        }

        /* if the number of procs is greater than the number of CPUs
         * on this node, but less or equal to the number of slots,
         * then we are not oversubscribed but we are overloaded. If
         * the user didn't specify a required binding, then we set
         * the binding policy to do-not-bind for this node */
        ncpus = prte_rmaps_base_get_ncpus(node, NULL, options);
        if (options->nprocs > ncpus &&
            options->nprocs <= node->slots_available &&
            !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            options->bind = PRTE_BIND_TO_NONE;
            jdata->map->binding = PRTE_BIND_TO_NONE;
        }

        if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, NULL, options)) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            options->bind = savebind;
            continue;
        }

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot assigning %d procs to node %s",
                            (int) options->nprocs, node->name);

        for (i = 0; i < options->nprocs && nprocs_mapped < app->num_procs; i++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                return rc;
            }
        }

        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
        options->bind = savebind;
    }

    if (second_pass) {
        /* unable to do it */
        if (PRTE_ERR_OUT_OF_RESOURCE == rc) {
            pmix_show_help("help-prte-rmaps-base.txt",
                           "out-of-resource", true,
                           app->num_procs, app->app,
                           prte_rmaps_base_print_mapping(options->map),
                           prte_hwloc_base_print_binding(options->bind));
            return PRTE_ERR_SILENT;
        }
        return PRTE_ERR_FAILED_TO_MAP;
    }

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr:slot job %s is oversubscribed - performing second pass",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* second pass: if we haven't mapped everyone yet, it is
     * because we are oversubscribed. All of the nodes that are
     * at max_slots have been removed from the list as that specifies
     * a hard boundary, so the nodes remaining are available for
     * handling the oversubscription. Figure out how many procs
     * to add to each of them.
     */
    balance = (float) ((int) app->num_procs - nprocs_mapped)
              / (float) pmix_list_get_size(node_list);
    extra_procs_to_assign = (int) balance;
    if (0 < (balance - (float) extra_procs_to_assign)) {
        /* compute how many nodes need an extra proc */
        nxtra_nodes = app->num_procs - nprocs_mapped
                      - (extra_procs_to_assign * pmix_list_get_size(node_list));
        /* add one so that we add an extra proc to the first nodes
         * until all procs are mapped
         */
        extra_procs_to_assign++;
    }
    // Rescan the nodes
    second_pass = true;
    goto pass;
}

int prte_rmaps_rr_bynode(prte_job_t *jdata,
                         prte_app_context_t *app,
                         pmix_list_t *node_list,
                         int32_t num_slots,
                         pmix_rank_t num_procs,
                         prte_rmaps_options_t *options)
{
    int rc, j, nprocs_mapped, nnode, ncpus;
    prte_node_t *node, *nd;
    float balance;
    bool second_pass = false;
    prte_proc_t *proc;
    prte_binding_policy_t savebind = options->bind;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by node for job %s app %d slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) app->idx, (int) num_slots,
                        (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    nprocs_mapped = 0;

pass:
    /* divide the procs evenly across all nodes - this is the
     * average we have to maintain as we go, but we adjust
     * the number on each node to reflect its available slots.
     * Obviously, if all nodes have the same number of slots,
     * then the avg is what we get on each node - this is
     * the most common situation.
     */
    options->nprocs = (app->num_procs - nprocs_mapped) / pmix_list_get_size(node_list);
    if (0 == options->nprocs) {
        /* if there are less procs than nodes, we have to
         * place at least one/node
         */
        options->nprocs = 1;
    }

    PMIX_LIST_FOREACH_SAFE(node, nd, node_list, prte_node_t)
    {
        prte_rmaps_base_get_cpuset(jdata, node, options);

        if (!options->oversubscribe) {
            /* since oversubscribe is not allowed, cap our usage
             * at the number of available slots. */
            if (node->slots_available < options->nprocs) {
                options->nprocs = node->slots_available;
            }
        }

        /* if the number of procs is greater than the number of CPUs
         * on this node, but less or equal to the number of slots,
         * then we are not oversubscribed but we are overloaded. If
         * the user didn't specify a required binding, then we set
         * the binding policy to do-not-bind for this node */
        ncpus = prte_rmaps_base_get_ncpus(node, NULL, options);
        if (options->nprocs > ncpus &&
            options->nprocs <= node->slots_available &&
            !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            options->bind = PRTE_BIND_TO_NONE;
            jdata->map->binding = PRTE_BIND_TO_NONE;
        }

        if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, NULL, options)) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            options->bind = savebind;
            continue;
        }

        PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                             "%s NODE %s ASSIGNING %d PROCS",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             node->name, options->nprocs));

        for (j=0; j < options->nprocs && nprocs_mapped < app->num_procs; j++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                return rc;
            }
        }
        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
        options->bind = savebind;
    }

    if (second_pass) {
        /* unable to do it */
        if (PRTE_ERR_OUT_OF_RESOURCE == rc) {
            pmix_show_help("help-prte-rmaps-base.txt",
                           "out-of-resource", true,
                           app->num_procs, app->app,
                           prte_rmaps_base_print_mapping(options->map),
                           prte_hwloc_base_print_binding(options->bind));
            return PRTE_ERR_SILENT;
        }
        return PRTE_ERR_FAILED_TO_MAP;
    }
    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr:node job %s is oversubscribed - performing second pass",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* second pass: if we haven't mapped everyone yet, it is
     * because we are oversubscribed. All of the nodes that are
     * at max_slots have been removed from the list as that specifies
     * a hard boundary, so the nodes remaining are available for
     * handling the oversubscription.
     */
    second_pass = true;
    goto pass;
}

/* mapping by cpu */
int prte_rmaps_rr_bycpu(prte_job_t *jdata, prte_app_context_t *app,
                        pmix_list_t *node_list, int32_t num_slots,
                        pmix_rank_t num_procs, prte_rmaps_options_t *options)
{
    int i, rc, nprocs_mapped, ncpus;
    prte_node_t *node, *nd;
    prte_proc_t *proc;
    char **tmp;
    int ntomap;
    prte_binding_policy_t savebind = options->bind;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by cpu for job %s slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) num_slots,
                        (unsigned long)app->num_procs);

    /* check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    nprocs_mapped = 0;
    tmp = pmix_argv_split(options->cpuset, ',');
    ntomap = pmix_argv_count(tmp);
    pmix_argv_free(tmp);

    PMIX_LIST_FOREACH_SAFE(node, nd, node_list, prte_node_t)
    {
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:cpu working node %s", node->name);

        prte_rmaps_base_get_cpuset(jdata, node, options);

        if (options->ordered || !options->overload) {
            options->nprocs = ntomap;
        } else {
            /* assign a number of procs equal to the number of available slots */
            if (!PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                options->nprocs = node->slots_available;
            } else {
                options->nprocs = node->slots;
            }
        }

        if (!options->oversubscribe) {
            /* oversubscribe is not allowed, so cap our usage
             * at the number of available slots. */
            if (node->slots_available < options->nprocs) {
                options->nprocs = node->slots_available;
            }
        }

        /* if the number of procs is greater than the number of CPUs
         * on this node, but less or equal to the number of slots,
         * then we are not oversubscribed but we are overloaded. If
         * the user didn't specify a required binding, then we set
         * the binding policy to do-not-bind for this node */
        ncpus = prte_rmaps_base_get_ncpus(node, NULL, options);
        if (options->nprocs > ncpus &&
            options->nprocs <= node->slots_available &&
            !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            options->bind = PRTE_BIND_TO_NONE;
            jdata->map->binding = PRTE_BIND_TO_NONE;
        }

        if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, NULL, options)) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            options->bind = savebind;
            continue;
        }

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:cpu assigning %d procs to node %s",
                            (int) options->nprocs, node->name);

        for (i = 0; i < options->nprocs && nprocs_mapped < app->num_procs; i++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                return rc;
            }
        }
        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
    }

    /* if we get here, then we were unable to map all the procs */
    if (PRTE_ERR_OUT_OF_RESOURCE == rc) {
        pmix_show_help("help-prte-rmaps-base.txt",
                       "out-of-resource", true,
                       app->num_procs, app->app,
                       prte_rmaps_base_print_mapping(options->map),
                       prte_hwloc_base_print_binding(options->bind));
        return PRTE_ERR_SILENT;
    }
    return PRTE_ERR_FAILED_TO_MAP;
}

/* mapping by hwloc object looks a lot like mapping by node,
 * but has the added complication of possibly having different
 * numbers of objects on each node
 */
int prte_rmaps_rr_byobj(prte_job_t *jdata, prte_app_context_t *app,
                        pmix_list_t *node_list, int32_t num_slots,
                        pmix_rank_t num_procs,
                        prte_rmaps_options_t *options)
{
    int i, rc, nprocs_mapped, nprocs;
    prte_node_t *node, *nd;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    int navg, nxtra_objs = 0, ncpus;
    float balance;
    prte_proc_t *proc;
    bool second_pass = false;
    bool span = false;
    bool nodefull;
    hwloc_obj_t obj = NULL;
    unsigned j, total_nobjs, nobjs;
    prte_binding_policy_t savebind = options->bind;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by %s for job %s slots %d num_procs %lu",
                        hwloc_obj_type_string(options->maptype),
                        PRTE_JOBID_PRINT(jdata->nspace),
                        (int) num_slots, (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < app->num_procs) {
        if (!options->oversubscribe) {
            pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    /* there are two modes for mapping by object: span and not-span. The
     * span mode essentially operates as if there was just a single
     * "super-node" in the system - i.e., it balances the load across
     * all objects of the indicated type regardless of their location.
     * In essence, it acts as if we placed one proc on each object, cycling
     * across all objects on all nodes, and then wrapped around to place
     * another proc on each object, doing so until all procs were placed.
     *
     * In contrast, the non-span mode operates similar to byslot mapping.
     * All slots on each node are filled, assigning each proc to an object
     * on that node in a balanced fashion, and then the mapper moves on
     * to the next node. Thus, procs tend to be "front loaded" onto the
     * list of nodes, as opposed to being "load balanced" in the span mode
     */
    if (options->mapspan) {
        /* we know we have enough slots, or that oversubscrption is allowed, so
         * next determine how many total objects we have to work with
         */
        total_nobjs = 0;
        PMIX_LIST_FOREACH(node, node_list, prte_node_t)
        {
            /* get the number of objects of this type on this node */
            total_nobjs += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                              options->maptype, options->cmaplvl);
        }

        if (0 == total_nobjs) {
            return PRTE_ERR_NOT_FOUND;
        }
        /* divide the procs evenly across all objects */
        navg = app->num_procs / total_nobjs;
        if (0 == navg) {
            /* if there are less procs than objects, we have to
             * place at least one/obj
             */
            navg = 1;
        }

        /* compute how many objs need an extra proc */
        nxtra_objs = app->num_procs - (navg * total_nobjs);
        if (0 > nxtra_objs) {
            nxtra_objs = 0;
        }
        span = true;
    }

    /* we know we have enough slots, or that oversubscrption is allowed, so
     * start mapping procs onto objects, filling each object as we go until
     * all procs are mapped. If one pass doesn't catch all the required procs,
     * then loop thru the list again to handle the oversubscription
     */
    nprocs_mapped = 0;

pass:
    options->total_nobjs = 0;
    PMIX_LIST_FOREACH(node, node_list, prte_node_t)
    {

        prte_rmaps_base_get_cpuset(jdata, node, options);
        options->nobjs = 0;
        /* have to delay checking for availability until we have the object */

        /* get the number of objects of this type on this node */
        nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                   options->maptype, options->cmaplvl);
        if (0 == nobjs) {
            continue;
        }
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: found %u %s objects on node %s",
                            nobjs, hwloc_obj_type_string(options->maptype),
                            node->name);

        /* compute the number of procs to go on this node */
        if (second_pass) {
            nprocs = extra_procs_to_assign;
            if (0 < nxtra_nodes) {
                --nxtra_nodes;
                if (0 == nxtra_nodes) {
                    --extra_procs_to_assign;
                }
            }
        } else {
            if (!options->donotlaunch) {
                rc = prte_rmaps_base_check_support(jdata, node, options);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
            if (span) {
                if (navg <= node->slots_available) {
                    nprocs = navg;
                } else {
                    nprocs = node->slots_available;
                }
                if (0 < nxtra_objs) {
                    nprocs++;
                    nxtra_objs--;
                }
            } else {
                /* assign a number of procs equal to the number of available slots */
                if (!PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                    nprocs = node->slots_available;
                } else {
                    nprocs = node->slots;
                }
            }
        }

        if (!options->oversubscribe) {
            /* since oversubscribe is not allowed, cap our usage
             * at the number of available slots. */
            if (node->slots_available < nprocs) {
                nprocs = node->slots_available;
            }
        }

        /* if the number of procs is greater than the number of CPUs
         * on this node, but less or equal to the number of slots,
         * then we are not oversubscribed but we are overloaded. If
         * the user didn't specify a required binding, then we set
         * the binding policy to do-not-bind for this node */
        ncpus = prte_rmaps_base_get_ncpus(node, NULL, options);
        if (nprocs > ncpus &&
            nprocs <= node->slots_available &&
            !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            options->bind = PRTE_BIND_TO_NONE;
            jdata->map->binding = PRTE_BIND_TO_NONE;
        }

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: assigning nprocs %d", nprocs);

        nodefull = false;
        if (span) {
            /* if we are mapping spanned, then we loop over
             * procs as the outer loop and loop over objects
             * as the inner loop so we balance procs across
             * all the objects on the node */
            for (i=0; i < nprocs && nprocs_mapped < app->num_procs && !nodefull; i++) {
                for (j=0; j < nobjs && nprocs_mapped < app->num_procs; j++) {
                    prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rr: assigning proc to object %d", j);
                    /* get the hwloc object */
                    obj = prte_hwloc_base_get_obj_by_type(node->topology->topo,
                                                          options->maptype, options->cmaplvl, j);
                    if (NULL == obj) {
                        /* out of objects on this node */
                        break;
                    }
                    options->nprocs = nprocs;
                    if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, obj, options)) {
                        rc = PRTE_ERR_OUT_OF_RESOURCE;
                        continue;
                    }
                    proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, obj, options);
                    if (NULL == proc) {
                        return PRTE_ERR_OUT_OF_RESOURCE;
                    }
                    /* setup_proc removes any node at max_slots */
                    if (0 == i) {
                        options->total_nobjs++;
                    }
                    options->nobjs++;
                    nprocs_mapped++;
                    rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
                    if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                        /* move to next node */
                        nodefull = true;
                        break;
                    } else if (PRTE_SUCCESS != rc) {
                        /* got an error */
                        return rc;
                    }
                }
            }
        } else {
            /* if we are not mapping spanned, then we loop over
             * objects as the outer loop and loop over procs
             * as the inner loop so that procs fill a given
             * object before moving to the next one on the node */
            for (j=0; j < nobjs && nprocs_mapped < app->num_procs && !nodefull; j++) {
                /* get the hwloc object */
                obj = prte_hwloc_base_get_obj_by_type(node->topology->topo,
                                                      options->maptype, options->cmaplvl, j);
                if (NULL == obj) {
                    /* out of objects on this node */
                    break;
                }
                options->nprocs = nprocs;
                if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, obj, options)) {
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    continue;
                }
                options->total_nobjs++;
                options->nobjs++;
                for (i=0; i < options->nprocs && nprocs_mapped < app->num_procs; i++) {
                    proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, obj, options);
                    if (NULL == proc) {
                        return PRTE_ERR_OUT_OF_RESOURCE;
                    }
                    /* setup_proc removes any node at max_slots */
                    nprocs_mapped++;
                    rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
                    if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                        /* move to next node */
                        nodefull = true;
                        break;
                    } else if (PRTE_SUCCESS != rc) {
                        /* got an error */
                        return rc;
                    }
                }
            }
        }
        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
        options->bind = savebind;
    }

    if (second_pass) {
        /* unable to do it */
        if (PRTE_ERR_OUT_OF_RESOURCE == rc) {
            pmix_show_help("help-prte-rmaps-base.txt",
                           "out-of-resource", true,
                           app->num_procs, app->app,
                           prte_rmaps_base_print_mapping(options->map),
                           prte_hwloc_base_print_binding(options->bind));
            return PRTE_ERR_SILENT;
        }
        return PRTE_ERR_FAILED_TO_MAP;
    }

    /* second pass: if we haven't mapped everyone yet, it is
     * because we are oversubscribed. All of the nodes that are
     * at max_slots have been removed from the list as that specifies
     * a hard boundary, so the nodes remaining are available for
     * handling the oversubscription. Figure out how many procs
     * to add to each of them.
     */
    balance = (float) ((int) app->num_procs - nprocs_mapped) / (float) options->total_nobjs;
    extra_procs_to_assign = (int) balance;
    if (0 < (balance - (float) extra_procs_to_assign)) {
        /* compute how many nodes need an extra proc */
        nxtra_nodes = app->num_procs - nprocs_mapped
                      - (extra_procs_to_assign * total_nobjs);
        /* add one so that we add an extra proc to the first nodes
         * until all procs are mapped
         */
        extra_procs_to_assign++;
    }
    // Rescan the nodes
    second_pass = true;
    goto pass;

    return PRTE_SUCCESS;
}
