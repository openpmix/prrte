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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "src/util/show_help.h"

#include "rmaps_rr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

int prte_rmaps_rr_byslot(prte_job_t *jdata, prte_app_context_t *app, prte_list_t *node_list,
                         int32_t num_slots, pmix_rank_t num_procs)
{
    int i, nprocs_mapped;
    prte_node_t *node;
    int num_procs_to_assign, extra_procs_to_assign = 0, nxtra_nodes = 0;
    hwloc_obj_t obj = NULL;
    float balance;
    bool add_one = false;
    prte_proc_t *proc;
    int orig_extra_procs;
    bool made_progress = false;
    bool orig_add_one;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by slot for job %s slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) num_slots,
                        (unsigned long) num_procs);

    /* check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    /* first pass: map the number of procs to each node until we
     * map all specified procs or use all allocated slots
     */
    nprocs_mapped = 0;
    PRTE_LIST_FOREACH(node, node_list, prte_node_t)
    {
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s", node->name);
        /* get the root object as we are not assigning
         * locale here except at the node level
         */
        if (NULL != node->topology && NULL != node->topology->topo) {
            obj = hwloc_get_root_obj(node->topology->topo);
        }
        if (node->slots <= node->slots_inuse) {
            prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr:slot node %s is full - skipping", node->name);
            continue;
        }

        /* assign a number of procs equal to the number of available slots */
        num_procs_to_assign = node->slots_available;

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot assigning %d procs to node %s",
                            (int) num_procs_to_assign, node->name);

        for (i = 0; i < num_procs_to_assign && nprocs_mapped < app->num_procs; i++) {
            /* add this node to the map - do it only once */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                PRTE_RETAIN(node);
                prte_pointer_array_add(jdata->map->nodes, node);
                ++(jdata->map->num_nodes);
            }
            if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            nprocs_mapped++;
            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                               PRTE_ATTR_LOCAL, obj, PMIX_POINTER);
        }
    }

    if (nprocs_mapped == app->num_procs) {
        /* we are done */
        return PRTE_SUCCESS;
    }

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr:slot job %s is oversubscribed - performing second pass",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* second pass: if we haven't mapped everyone yet, it is
     * because we are oversubscribed. Figure out how many procs
     * to add
     */
    balance = (float) ((int) app->num_procs - nprocs_mapped)
              / (float) prte_list_get_size(node_list);
    extra_procs_to_assign = (int) balance;
    if (0 < (balance - (float) extra_procs_to_assign)) {
        /* compute how many nodes need an extra proc */
        nxtra_nodes = app->num_procs - nprocs_mapped
                      - (extra_procs_to_assign * prte_list_get_size(node_list));
        /* add one so that we add an extra proc to the first nodes
         * until all procs are mapped
         */
        extra_procs_to_assign++;
        /* flag that we added one */
        add_one = true;
    }

    // Rescan the nodes due to a max_slots issue
 rescan_nodes:

    PRTE_LIST_FOREACH(node, node_list, prte_node_t)
    {
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s", node->name);
        /* get the root object as we are not assigning
         * locale except at the node level
         */
        if (NULL != node->topology && NULL != node->topology->topo) {
            obj = hwloc_get_root_obj(node->topology->topo);
        }

        if (0 != node->slots_max && node->slots_max <= node->slots_inuse) {
            /* cannot use this node - already at max_slots */
            continue;
        }

        /* Save original values in case we need to reset them due to max_slots */
        orig_extra_procs = extra_procs_to_assign;
        orig_add_one = add_one;
        if (add_one) {
            if (0 == nxtra_nodes) {
                --extra_procs_to_assign;
                add_one = false;
            } else {
                --nxtra_nodes;
            }
        }
        if (node->slots <= node->slots_inuse) {
            /* nodes are already oversubscribed */
            num_procs_to_assign = extra_procs_to_assign;
        } else {
            /* nodes have some room */
            num_procs_to_assign = node->slots - node->slots_inuse + extra_procs_to_assign;
        }

        if (0 != node->slots_max) {
            if (node->slots_max < (node->slots_inuse + num_procs_to_assign)) {
                num_procs_to_assign = node->slots_max - node->slots_inuse;
                if (0 >= num_procs_to_assign) {
                    /* Undo the adjustments to these variables from above */
                    extra_procs_to_assign = orig_extra_procs;
                    if (orig_add_one) {
                        if (0 == nxtra_nodes) {
                            ++extra_procs_to_assign;
                            add_one = true;
                        } else {
                            ++nxtra_nodes;
                        }
                    }
                    continue;
                }
            }
        }

        /* add this node to the map - do it only once */
        if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            PRTE_RETAIN(node);
            prte_pointer_array_add(jdata->map->nodes, node);
            ++(jdata->map->num_nodes);
        }

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot adding up to %d procs to node %s",
                            num_procs_to_assign, node->name);
        for (i = 0; i < num_procs_to_assign && nprocs_mapped < app->num_procs; i++) {
            if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            nprocs_mapped++;
            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                               PMIX_POINTER);
        }
        /* We made progress mapping at least 1 process in this loop */
        made_progress = true;
        /* not all nodes are equal, so only set oversubscribed for
         * this node if it is in that state
         */
        if (node->slots < (int) node->num_procs) {
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
                    prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                   app->num_procs, app->app, prte_process_info.nodename);
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    return PRTE_ERR_SILENT;
                } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                           & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    /* if we were explicitly told not to oversubscribe, then don't */
                    prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                   app->num_procs, app->app, prte_process_info.nodename);
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    return PRTE_ERR_SILENT;
                }
            }
        }
        /* if we have mapped everything, then we are done */
        if (nprocs_mapped == app->num_procs) {
            break;
        }
    }

    /* If we went through the loop and did not find a place for any one process
     * then all of the nodes are full.
     */
    if (!made_progress) {
        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                       true, app->num_procs, app->app, prte_process_info.nodename);
        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
        return PRTE_ERR_SILENT;
    }
    if (nprocs_mapped != app->num_procs) {
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot Re-scan all nodes. Mapped %d, Target %d (%c)",
                            nprocs_mapped, app->num_procs,
                            made_progress ? 'T' : 'F');
        made_progress = false;
        goto rescan_nodes;
    }

    return PRTE_SUCCESS;
}

int prte_rmaps_rr_bynode(prte_job_t *jdata, prte_app_context_t *app, prte_list_t *node_list,
                         int32_t num_slots, pmix_rank_t num_procs)
{
    int j, nprocs_mapped, nnodes;
    prte_node_t *node;
    int num_procs_to_assign, navg;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    hwloc_obj_t obj = NULL;
    float balance;
    bool add_one = false;
    bool oversubscribed = false;
    prte_proc_t *proc;
    int orig_extra_procs;
    bool made_progress = false;
    bool orig_add_one;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by node for job %s app %d slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) app->idx, (int) num_slots,
                        (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
        oversubscribed = true;
    }

    nnodes = prte_list_get_size(node_list);
    nprocs_mapped = 0;

    do {
        /* divide the procs evenly across all nodes - this is the
         * average we have to maintain as we go, but we adjust
         * the number on each node to reflect its available slots.
         * Obviously, if all nodes have the same number of slots,
         * then the avg is what we get on each node - this is
         * the most common situation.
         */
        navg = ((int) app->num_procs - nprocs_mapped) / nnodes;
        if (0 == navg) {
            /* if there are less procs than nodes, we have to
             * place at least one/node
             */
            navg = 1;
        }

        /* compute how many extra procs to put on each node */
        balance = (float) (((int) app->num_procs - nprocs_mapped) - (navg * nnodes))
                  / (float) nnodes;
        extra_procs_to_assign = (int) balance;
        nxtra_nodes = 0;
        add_one = false;
        if (0 < (balance - (float) extra_procs_to_assign)) {
            /* compute how many nodes need an extra proc */
            nxtra_nodes = ((int) app->num_procs - nprocs_mapped)
                          - ((navg + extra_procs_to_assign) * nnodes);
            /* add one so that we add an extra proc to the first nodes
             * until all procs are mapped
             */
            extra_procs_to_assign++;
            /* flag that we added one */
            add_one = true;
        }

        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: mapping by node navg %d extra_procs %d extra_nodes %d",
                            navg, extra_procs_to_assign, nxtra_nodes);

        nnodes = 0;
        PRTE_LIST_FOREACH(node, node_list, prte_node_t)
        {
            /* get the root object as we are not assigning
             * locale except at the node level
             */
            if (NULL != node->topology && NULL != node->topology->topo) {
                obj = hwloc_get_root_obj(node->topology->topo);
            }

            if (0 != node->slots_max && node->slots_max <= node->slots_inuse) {
                /* cannot use this node - already at max_slots */
                continue;
            }

            if (oversubscribed) {
                /* Save original values in case we need to reset them due to max_slots */
                orig_extra_procs = extra_procs_to_assign;
                orig_add_one = add_one;
                /* compute the number of procs to go on this node */
                if (add_one) {
                    if (0 == nxtra_nodes) {
                        --extra_procs_to_assign;
                        add_one = false;
                    } else {
                        --nxtra_nodes;
                    }
                }
                /* everybody just takes their share */
                num_procs_to_assign = navg + extra_procs_to_assign;

                if (0 != node->slots_max) {
                    if (node->slots_max < (node->slots_inuse + num_procs_to_assign)) {
                        num_procs_to_assign = node->slots_max - node->slots_inuse;
                        if (0 >= num_procs_to_assign) {
                            /* Undo the adjustments to these variables from above */
                            extra_procs_to_assign = orig_extra_procs;
                            if (orig_add_one) {
                                if (0 == nxtra_nodes) {
                                    ++extra_procs_to_assign;
                                    add_one = true;
                                } else {
                                    ++nxtra_nodes;
                                }
                            }
                            continue;
                        }
                    }
                }
            } else if (node->slots <= node->slots_inuse) {
                /* since we are not oversubcribed, ignore this node */
                continue;
            } else {
                /* if we are not oversubscribed, then there are enough
                 * slots to handle all the procs. However, not every
                 * node will have the same number of slots, so we
                 * have to track how many procs to "shift" elsewhere
                 * to make up the difference
                 */

                /* compute the number of procs to go on this node */
                if (add_one) {
                    if (0 == nxtra_nodes) {
                        --extra_procs_to_assign;
                        add_one = false;
                    } else {
                        --nxtra_nodes;
                    }
                }
                /* if slots < avg + extra (adjusted for cpus/proc), then try to take all */
                if (node->slots_available < (navg + extra_procs_to_assign)) {
                    num_procs_to_assign = node->slots_available;
                    /* if we can't take any proc, skip following steps */
                    if (num_procs_to_assign == 0) {
                        continue;
                    }
                } else {
                    /* take the avg + extra */
                    num_procs_to_assign = navg + extra_procs_to_assign;
                }
                PRTE_OUTPUT_VERBOSE((20, prte_rmaps_base_framework.framework_output,
                                     "%s NODE %s AVG %d ASSIGN %d EXTRA %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, navg,
                                     num_procs_to_assign, extra_procs_to_assign));
            }
            /* add this node to the map, but only do so once */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                PRTE_RETAIN(node);
                prte_pointer_array_add(jdata->map->nodes, node);
                ++(jdata->map->num_nodes);
            }
            nnodes++; // track how many nodes remain available
            PRTE_OUTPUT_VERBOSE((20, prte_rmaps_base_framework.framework_output,
                                 "%s NODE %s ASSIGNING %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 node->name, num_procs_to_assign));
            for (j = 0; j < num_procs_to_assign && nprocs_mapped < app->num_procs; j++) {
                if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                nprocs_mapped++;
                prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                                   PMIX_POINTER);
            }
            /* not all nodes are equal, so only set oversubscribed for
             * this node if it is in that state
             */
            if (node->slots < (int) node->num_procs) {
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
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRTE_ERR_SILENT;
                    } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                               & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        /* if we were explicitly told not to oversubscribe, then don't */
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRTE_ERR_SILENT;
                    }
                }
            }
            if (nprocs_mapped == app->num_procs) {
                /* we are done */
                break;
            }
        }
    } while (nprocs_mapped < app->num_procs && 0 < nnodes);

    /* now fillin as required until fully mapped */
    while (nprocs_mapped < app->num_procs) {
        made_progress = false;
        PRTE_LIST_FOREACH(node, node_list, prte_node_t)
        {
            /* get the root object as we are not assigning
             * locale except at the node level
             */
            if (NULL != node->topology && NULL != node->topology->topo) {
                obj = hwloc_get_root_obj(node->topology->topo);
            }

            if (0 != node->slots_max && node->slots_max <= node->slots_inuse) {
                /* cannot use this node - already at max_slots */
                continue;
            }

            PRTE_OUTPUT_VERBOSE((20, prte_rmaps_base_framework.framework_output,
                                 "%s ADDING PROC TO NODE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 node->name));
            if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            made_progress = true;
            nprocs_mapped++;
            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                               PMIX_POINTER);
            /* not all nodes are equal, so only set oversubscribed for
             * this node if it is in that state
             */
            if (node->slots < (int) node->num_procs) {
                /* flag the node as oversubscribed so that sched-yield gets
                 * properly set
                 */
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
            }
            if (nprocs_mapped == app->num_procs) {
                /* we are done */
                break;
            }
        }
        if (!made_progress) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                           true, app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    return PRTE_SUCCESS;
}

static int byobj_span(prte_job_t *jdata, prte_app_context_t *app, prte_list_t *node_list,
                      int32_t num_slots, pmix_rank_t num_procs, hwloc_obj_type_t target,
                      unsigned cache_level);

/* mapping by hwloc object looks a lot like mapping by node,
 * but has the added complication of possibly having different
 * numbers of objects on each node
 */
int prte_rmaps_rr_byobj(prte_job_t *jdata, prte_app_context_t *app, prte_list_t *node_list,
                        int32_t num_slots, pmix_rank_t num_procs, hwloc_obj_type_t target,
                        unsigned cache_level)
{
    int i, nmapped, nprocs_mapped;
    prte_node_t *node;
    int nprocs, start, cpus_per_rank, npus;
    hwloc_obj_t obj = NULL, root;
    unsigned int nobjs;
    bool add_one;
    bool second_pass, use_hwthread_cpus;
    prte_proc_t *proc;
    uint16_t u16, *u16ptr = &u16;
    char *job_cpuset;
    prte_hwloc_topo_data_t *rdata;
    hwloc_cpuset_t available, mycpus;
    bool found_obj;

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
    if (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        return byobj_span(jdata, app, node_list, num_slots, num_procs, target, cache_level);
    }

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping no-span by %s for job %s slots %d num_procs %lu",
                        hwloc_obj_type_string(target), PRTE_JOBID_PRINT(jdata->nspace),
                        (int) num_slots, (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < app->num_procs) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* see if they want multiple cpus/rank */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr,
                           PMIX_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* we know we have enough slots, or that oversubscrption is allowed, so
     * start mapping procs onto objects, filling each object as we go until
     * all procs are mapped. If one pass doesn't catch all the required procs,
     * then loop thru the list again to handle the oversubscription
     */
    nprocs_mapped = 0;
    second_pass = false;
    do {
        add_one = false;
        PRTE_LIST_FOREACH(node, node_list, prte_node_t)
        {
            if (NULL == node->topology || NULL == node->topology->topo) {
                prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
                return PRTE_ERR_SILENT;
            }
            start = 0;
            /* get the number of objects of this type on this node */
            nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target, cache_level);
            if (0 == nobjs) {
                continue;
            }
            prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: found %u %s objects on node %s", nobjs,
                                hwloc_obj_type_string(target), node->name);

            /* if this is a comm_spawn situation, start with the object
             * where the parent left off and increment */
            if (!PMIX_NSPACE_INVALID(jdata->originator.nspace) && UINT_MAX != jdata->bkmark_obj) {
                start = (jdata->bkmark_obj + 1) % nobjs;
            }
            /* compute the number of procs to go on this node */
            nprocs = node->slots_available;
            prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: calculated nprocs %d", nprocs);
            if (nprocs < 1) {
                if (second_pass) {
                    if (0 != node->slots_max && node->slots_max <= node->slots_inuse) {
                        /* cannot use this node - already at max_slots */
                        continue;
                    }
                    /* already checked for oversubscription permission, so at least put
                     * one proc on it
                     */
                    nprocs = 1;
                    /* offset our starting object position to avoid always
                     * hitting the first one
                     */
                    start = node->num_procs % nobjs;
                } else {
                    continue;
                }
            } else if (0 != node->slots_max && (node->slots_inuse + nprocs) > node->slots_max) {
                nprocs = node->slots_max - node->slots_inuse;
                if (0 >= nprocs) {
                    /* cannot use this node */
                    continue;
                }
            }
            /* get the available processors on this node */
            root = hwloc_get_root_obj(node->topology->topo);
            if (NULL == root->userdata) {
                /* incorrect */
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                return PRTE_ERR_BAD_PARAM;
            }
            rdata = (prte_hwloc_topo_data_t *) root->userdata;
            available = hwloc_bitmap_dup(rdata->available);
            if (NULL != job_cpuset) {
                /* deal with any "soft" cgroup specification */
                mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                         job_cpuset);
                hwloc_bitmap_and(available, mycpus, available);
                hwloc_bitmap_free(mycpus);
            }

            /* add this node to the map, if reqd */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                PRTE_RETAIN(node);
                prte_pointer_array_add(jdata->map->nodes, node);
                ++(jdata->map->num_nodes);
            }
            nmapped = 0;
            prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: assigning nprocs %d", nprocs);

            do {
                found_obj = false;
                /* loop through the number of objects */
                for (i = 0;
                     i < (int) nobjs && nmapped < nprocs && nprocs_mapped < (int) app->num_procs;
                     i++) {
                    prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rr: assigning proc to object %d",
                                        (i + start) % nobjs);
                    /* get the hwloc object */
                    obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target,
                                                          cache_level, (i + start) % nobjs);
                    if (NULL == obj) {
                        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                        hwloc_bitmap_free(available);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_NOT_FOUND;
                    }
                    npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                    available, obj);
                    if (0 == npus) {
                        continue;
                    }
                    found_obj = true;
                    if (cpus_per_rank > npus) {
                        prte_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                                       cpus_per_rank, npus,
                                       prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
                        hwloc_bitmap_free(available);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_SILENT;
                    }
                    if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                        hwloc_bitmap_free(available);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_OUT_OF_RESOURCE;
                    }
                    nprocs_mapped++;
                    nmapped++;
                    prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL,
                                       obj, PMIX_POINTER);
                    /* track the bookmark */
                    jdata->bkmark_obj = (i + start) % nobjs;
                }
            } while (found_obj && nmapped < nprocs && nprocs_mapped < (int) app->num_procs);
            if (!found_obj) {
                char *err;
                hwloc_bitmap_list_asprintf(&err, available);
                prte_show_help("help-prte-rmaps-base.txt", "insufficient-cpus", true,
                               prte_rmaps_base_print_mapping(prte_rmaps_base.mapping),
                               (NULL == prte_hwloc_default_cpu_list) ? "N/A"
                                                                     : prte_hwloc_default_cpu_list,
                               (NULL == job_cpuset) ? "N/A" : job_cpuset, err);
                hwloc_bitmap_free(available);
                free(err);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            hwloc_bitmap_free(available);
            add_one = true;
            /* not all nodes are equal, so only set oversubscribed for
             * this node if it is in that state
             */
            if (node->slots < (int) node->num_procs) {
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
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_SILENT;
                    } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                               & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        /* if we were explicitly told not to oversubscribe, then don't */
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_SILENT;
                    }
                }
            }
            if (nprocs_mapped == app->num_procs) {
                /* we are done */
                break;
            }
        }
        second_pass = true;
    } while (add_one && nprocs_mapped < app->num_procs);

    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    if (nprocs_mapped < app->num_procs) {
        /* usually means there were no objects of the requested type */
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

static int byobj_span(prte_job_t *jdata, prte_app_context_t *app, prte_list_t *node_list,
                      int32_t num_slots, pmix_rank_t num_procs, hwloc_obj_type_t target,
                      unsigned cache_level)
{
    int i, j, nprocs_mapped, navg;
    prte_node_t *node;
    int nprocs, nxtra_objs, npus, cpus_per_rank;
    hwloc_obj_t obj = NULL, root;
    unsigned int nobjs;
    prte_proc_t *proc;
    uint16_t u16, *u16ptr = &u16;
    char *job_cpuset;
    prte_hwloc_topo_data_t *rdata;
    hwloc_cpuset_t available, mycpus;
    bool use_hwthread_cpus;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping span by %s for job %s slots %d num_procs %lu",
                        hwloc_obj_type_string(target), PRTE_JOBID_PRINT(jdata->nspace),
                        (int) num_slots, (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        }
    }

    /* we know we have enough slots, or that oversubscrption is allowed, so
     * next determine how many total objects we have to work with
     */
    nobjs = 0;
    PRTE_LIST_FOREACH(node, node_list, prte_node_t)
    {
        if (NULL == node->topology || NULL == node->topology->topo) {
            prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
            return PRTE_ERR_SILENT;
        }
        /* get the number of objects of this type on this node */
        nobjs += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target, cache_level);
    }

    if (0 == nobjs) {
        return PRTE_ERR_NOT_FOUND;
    }

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* see if they want multiple cpus/rank */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr,
                           PMIX_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* divide the procs evenly across all objects */
    navg = app->num_procs / nobjs;
    if (0 == navg) {
        /* if there are less procs than objects, we have to
         * place at least one/obj
         */
        navg = 1;
    }

    /* compute how many objs need an extra proc */
    if (0 > (nxtra_objs = app->num_procs - (navg * nobjs))) {
        nxtra_objs = 0;
    }

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by %s navg %d extra_objs %d",
                        hwloc_obj_type_string(target), navg, nxtra_objs);

    nprocs_mapped = 0;
    PRTE_LIST_FOREACH(node, node_list, prte_node_t)
    {
        /* add this node to the map, if reqd */
        if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            PRTE_RETAIN(node);
            prte_pointer_array_add(jdata->map->nodes, node);
            ++(jdata->map->num_nodes);
        }
        /* get the available processors on this node */
        root = hwloc_get_root_obj(node->topology->topo);
        if (NULL == root->userdata) {
            /* incorrect */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            if (NULL != job_cpuset) {
                free(job_cpuset);
            }
            return PRTE_ERR_BAD_PARAM;
        }
        rdata = (prte_hwloc_topo_data_t *) root->userdata;
        available = hwloc_bitmap_dup(rdata->available);
        if (NULL != job_cpuset) {
            /* deal with any "soft" cgroup specification */
            mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                     job_cpuset);
            hwloc_bitmap_and(available, mycpus, available);
            hwloc_bitmap_free(mycpus);
        }
        /* get the number of objects of this type on this node */
        nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target, cache_level);
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:byobj: found %d objs on node %s", nobjs, node->name);
        /* loop through the number of objects */
        for (i = 0; i < (int) nobjs && nprocs_mapped < (int) app->num_procs; i++) {
            /* get the hwloc object */
            obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target, cache_level, i);
            if (NULL == obj) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_NOT_FOUND;
            }
            npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus, available, obj);
            if (cpus_per_rank > npus) {
                prte_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true, cpus_per_rank,
                               npus, prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            /* determine how many to map */
            if (navg <= node->slots_available) {
                nprocs = navg;
            } else {
                nprocs = node->slots_available;
            }
            if (0 < nxtra_objs) {
                nprocs++;
                nxtra_objs--;
            }
            /* map the reqd number of procs */
            for (j = 0; j < nprocs && nprocs_mapped < app->num_procs; j++) {
                if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, app->idx))) {
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                nprocs_mapped++;
                prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                                   PMIX_POINTER);
            }
            /* keep track of the node we last used */
            jdata->bookmark = node;
        }
        /* not all nodes are equal, so only set oversubscribed for
         * this node if it is in that state
         */
        if (node->slots < (int) node->num_procs) {
            /* flag the node as oversubscribed so that sched-yield gets
             * properly set
             */
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
        }
        if (nprocs_mapped == app->num_procs) {
            /* we are done */
            break;
        }
        hwloc_bitmap_free(available);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRTE_SUCCESS;
}
