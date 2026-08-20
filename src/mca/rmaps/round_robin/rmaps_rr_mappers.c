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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "src/pmix/pmix-internal.h"
#include "src/hwloc/pmix_hwloc.h"

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
    int i, rc=PRTE_SUCCESS, nprocs_mapped, ncpus;
    prte_node_t *node, *nd;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    float balance;
    prte_proc_t *proc;
    bool second_pass = false;
    prte_binding_policy_t savebind = options->bind;

    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by slot for job %s slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) num_slots,
                        (unsigned long) num_procs);

    /* check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        } else {
            if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                jdata->map->binding = PRTE_BIND_TO_NONE;
                options->bind = PRTE_BIND_TO_NONE;
                savebind = options->bind;
            }
        }
    }

    nprocs_mapped = 0;

pass:
    PMIX_LIST_FOREACH_SAFE(node, nd, node_list, prte_node_t)
    {
        pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s", node->name);

        prte_rmaps_base_get_cpuset(jdata, node, options);
        if (NULL == options->job_cpuset) {
            // the prior function will have printed out the error
            rc = PRTE_ERR_SILENT;
            goto errout;
        }

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

        pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot assigning %d procs to node %s",
                            (int) options->nprocs, node->name);

        for (i = 0; i < options->nprocs && nprocs_mapped < app->num_procs; i++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                /* move on to the next node */
                rc = PRTE_ERR_SILENT;
                break;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                PMIX_RELEASE(proc);
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                PMIX_RELEASE(proc);
                goto errout;
            }
            PMIX_RELEASE(proc);
        }

        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
        options->bind = savebind;
        if(NULL != options->target)
        {
            hwloc_bitmap_free(options->target);
            options->target = NULL;
        }
    }

    if (second_pass) {
    errout:
        if (PRTE_ERR_SILENT != rc) {
            prte_show_help("help-prte-rmaps-base.txt",
                           "failed-map", true,
                           PRTE_ERROR_NAME(rc),
                           (NULL == app) ? "N/A" : app->app,
                           (NULL == app) ? -1 : app->num_procs,
                           prte_rmaps_base_print_mapping(options->map),
                           prte_hwloc_base_print_binding(options->bind));
        }
        return PRTE_ERR_SILENT;
    }

    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
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
    int rc=PRTE_SUCCESS, j, nprocs_mapped, ncpus;
    prte_node_t *node, *nd;
    bool second_pass = false;
    prte_proc_t *proc;
    prte_binding_policy_t savebind = options->bind;

    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by node for job %s app %d slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) app->idx, (int) num_slots,
                        (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        } else {
            if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                jdata->map->binding = PRTE_BIND_TO_NONE;
                options->bind = PRTE_BIND_TO_NONE;
                savebind = PRTE_BIND_TO_NONE;
            }
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
        if (NULL == options->job_cpuset) {
            // the prior function will have printed out the error
            rc = PRTE_ERR_SILENT;
            goto errout;
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

        PMIX_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                             "%s NODE %s ASSIGNING %d PROCS",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             node->name, options->nprocs));

        for (j=0; j < options->nprocs && nprocs_mapped < app->num_procs; j++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                /* move to next node */
                rc = PRTE_ERR_SILENT;
                break;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                PMIX_RELEASE(proc);
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                PMIX_RELEASE(proc);
                goto errout;
            }
            PMIX_RELEASE(proc);
        }
        if (nprocs_mapped == app->num_procs) {
            return PRTE_SUCCESS;
        }
        options->bind = savebind;
        if(NULL != options->target)
        {
            hwloc_bitmap_free(options->target);
            options->target = NULL;
        }
    }

    if (second_pass) {
    errout:
        /* unable to do it */
        if (PRTE_ERR_SILENT != rc) {
            prte_show_help("help-prte-rmaps-base.txt",
                           "failed-map", true,
                           PRTE_ERROR_NAME(rc),
                           (NULL == app) ? "N/A" : app->app,
                           (NULL == app) ? -1 : app->num_procs,
                           prte_rmaps_base_print_mapping(options->map),
                           prte_hwloc_base_print_binding(options->bind));
        }
        return PRTE_ERR_SILENT;
    }
    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
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
int prte_rmaps_rr_bydevice(prte_job_t *jdata, prte_app_context_t *app,
                           pmix_list_t *node_list, int32_t num_slots,
                           pmix_rank_t num_procs,
                           prte_rmaps_options_t *options)
{
    size_t ndevs;
    prte_rmaps_target_enum_t tgts = {
        .begin = prte_rmaps_base_devices_begin,
        .count = prte_rmaps_base_devices_count,
        .item = prte_rmaps_base_devices_locale,
        .placed = prte_rmaps_base_devices_record,
        .end = prte_rmaps_base_devices_end,
        .name = options->map_device,
        /* A device is assigned to a process, not subdivided between them:
         * two procs sharing a GPU is a different thing from two procs
         * sharing a core.  So it has its own qualifier rather than riding on
         * binding's "overload-allowed", which is about running more procs
         * than there are CPUs - a different resource, a different question,
         * and answering both with one word would leave neither sayable on
         * its own. */
        .nowrap = !options->map_shared
    };

    if (NULL == options->map_device) {
        /* the policy cannot be set without a value - see the two --map-by
         * parsers - so this is a programming error, not user input */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    /* Say up front when there are not enough devices to go round, rather
     * than letting the placement run out partway and report a generic
     * failure. */
    if (!options->map_shared) {
        ndevs = prte_rmaps_base_devices_total(node_list, options);
        if (0 < ndevs && ndevs < (size_t) app->num_procs) {
            prte_show_help("help-prte-rmaps-base.txt", "rmaps:too-few-devices", true,
                           (int) app->num_procs, options->map_device, (int) ndevs);
            return PRTE_ERR_SILENT;
        }
    }

    return prte_rmaps_rr_map_targets(jdata, app, node_list, num_slots,
                                     num_procs, options, &tgts);
}

int prte_rmaps_rr_bycpu(prte_job_t *jdata, prte_app_context_t *app,
                        pmix_list_t *node_list, int32_t num_slots,
                        pmix_rank_t num_procs, prte_rmaps_options_t *options)
{
    int i, rc, nprocs_mapped, ncpus;
    prte_node_t *node, *nd;
    prte_proc_t *proc;
    char **tmp;
    int ntomap;
    bool second_pass = false;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    float balance;
    char *savecpuset = NULL;
    prte_binding_policy_t savebind = options->bind;
    PRTE_HIDE_UNUSED_PARAMS(num_procs);

    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping by cpu for job %s slots %d num_procs %lu",
                        PRTE_JOBID_PRINT(jdata->nspace), (int) num_slots,
                        (unsigned long)app->num_procs);

    /* check to see if we can map all the procs */
    if (num_slots < (int) app->num_procs) {
        if (!options->oversubscribe) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        } else {
            if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                jdata->map->binding = PRTE_BIND_TO_NONE;
                options->bind = PRTE_BIND_TO_NONE;
                savebind = PRTE_BIND_TO_NONE;
            }
        }
    }

    nprocs_mapped = 0;
    savecpuset = (NULL != options->cpuset) ? strdup(options->cpuset) : NULL;

pass:
    PMIX_LIST_FOREACH_SAFE(node, nd, node_list, prte_node_t)
    {
        pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:cpu working node %s", node->name);

        // get the cpuset and check that all specified PEs are available
        // on this node
        prte_rmaps_base_get_cpuset(jdata, node, options);
        if (NULL == options->job_cpuset) {
            // the prior function will have printed out the error
            rc = PRTE_ERR_SILENT;
            goto errout;
        }

        if (second_pass) {
            options->nprocs = extra_procs_to_assign;
            if (0 < nxtra_nodes) {
                --nxtra_nodes;
                if (0 == nxtra_nodes) {
                    --extra_procs_to_assign;
                }
            }
        } else  if (options->ordered || !options->overload) {
            // see how many PEs we were given
            tmp = PMIx_Argv_split(options->cpuset, ',');
            ntomap = PMIx_Argv_count(tmp);
            PMIx_Argv_free(tmp);
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

        pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:cpu assigning %d procs to node %s",
                            (int) options->nprocs, node->name);

        for (i = 0; i < options->nprocs && nprocs_mapped < app->num_procs; i++) {
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                rc = PRTE_ERR_SILENT;
                goto errout;
            }
            nprocs_mapped++;
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* move to next node */
                PMIX_RELEASE(proc);
                break;
            } else if (PRTE_SUCCESS != rc) {
                /* got an error */
                PMIX_RELEASE(proc);
                goto errout;
            }
            PMIX_RELEASE(proc);
        }
        if (nprocs_mapped == app->num_procs) {
            if (NULL != options->target) {
                hwloc_bitmap_free(options->target);
                options->target = NULL;
            }
            if (NULL != options->job_cpuset) {
                hwloc_bitmap_free(options->job_cpuset);
                options->job_cpuset = NULL;
            }
            /* restore the list the caller handed us - what is in options
             * now is the remainder this pass did not consume, and it is
             * ours to release, exactly as on the per-node path below */
            if (NULL != options->cpuset) {
                free(options->cpuset);
            }
            options->cpuset = savecpuset;
            return PRTE_SUCCESS;
        }
        if (NULL != options->target) {
            hwloc_bitmap_free(options->target);
            options->target = NULL;
        }
        if (NULL != options->job_cpuset) {
            hwloc_bitmap_free(options->job_cpuset);
            options->job_cpuset = NULL;
        }
        if (NULL != options->cpuset) {
            free(options->cpuset);
        }
        options->cpuset = (NULL != savecpuset) ? strdup(savecpuset) : NULL;
    } // next node

    /* second pass: if we haven't mapped everyone yet, it is
     * because we are oversubscribed. All of the nodes that are
     * at max_slots have been removed from the list as that specifies
     * a hard boundary, so the nodes remaining are available for
     * handling the oversubscription. Figure out how many procs
     * to add to each of them.
     */
    if (options->oversubscribe && !second_pass) {
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
        /* restore the cpuset */
        if (NULL != options->cpuset) {
            free(options->cpuset);
        }
        options->cpuset = (NULL != savecpuset) ? strdup(savecpuset) : NULL;
        // Rescan the nodes
        second_pass = true;
        goto pass;
    }

errout:
    /* if we get here, then we were unable to map all the procs */
    if (PRTE_ERR_SILENT != rc) {
        prte_show_help("help-prte-rmaps-rr.txt",
                       "prte-rmaps-rr:not-enough-cpus", true,
                       app->app, app->num_procs, savecpuset);
    }
    if (NULL != savecpuset) {
        free(savecpuset);
    }
    return PRTE_ERR_SILENT;
}

/* mapping by hwloc object looks a lot like mapping by node,
 * but has the added complication of possibly having different
 * numbers of objects on each node
 */
/* The hwloc-object enumerator: the targets are every object of
 * options->maptype on the node.  Stateless - no begin/end needed. */
static unsigned hwloc_targets_count(prte_node_t *node,
                                    prte_rmaps_options_t *opts,
                                    void *ctx)
{
    PRTE_HIDE_UNUSED_PARAMS(ctx);
    return prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, opts->maptype);
}

static hwloc_obj_t hwloc_targets_item(prte_node_t *node,
                                      prte_rmaps_options_t *opts,
                                      void *ctx, unsigned j)
{
    PRTE_HIDE_UNUSED_PARAMS(ctx);
    return prte_hwloc_base_get_obj_by_type(node->topology->topo, opts->maptype, j);
}

int prte_rmaps_rr_byobj(prte_job_t *jdata, prte_app_context_t *app,
                        pmix_list_t *node_list, int32_t num_slots,
                        pmix_rank_t num_procs,
                        prte_rmaps_options_t *options)
{
    prte_rmaps_target_enum_t tgts = {
        .begin = NULL,
        .count = hwloc_targets_count,
        .item = hwloc_targets_item,
        .placed = NULL,
        .end = NULL,
        /* hwloc_obj_type_string() returns a static string */
        .name = hwloc_obj_type_string(options->maptype)
    };

    return prte_rmaps_rr_map_targets(jdata, app, node_list, num_slots,
                                     num_procs, options, &tgts);
}

int prte_rmaps_rr_map_targets(prte_job_t *jdata, prte_app_context_t *app,
                              pmix_list_t *node_list, int32_t num_slots,
                              pmix_rank_t num_procs,
                              prte_rmaps_options_t *options,
                              prte_rmaps_target_enum_t *tgts)
{
    int rc=PRTE_SUCCESS, nprocs_mapped;
    prte_node_t *node, *nnext;
    int ncpus, budget, nplaced, ndx;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    float balance;
    prte_proc_t *proc;
    bool nodefull, allfull, outofcpus=false, firstpass, wasfirst, interleave;
    hwloc_obj_t obj = NULL;
    unsigned i, j, nobjs, start;
    void *ctx = NULL;
    bool began = false;
    int *cursor = NULL;
    int ncursor = 0;

    pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr:byobj mapping by %s for job %s slots %d num_procs %lu",
                        tgts->name,
                        PRTE_JOBID_PRINT(jdata->nspace),
                        (int) num_slots, (unsigned long) num_procs);

    /* quick check to see if we can map all the procs */
    if (num_slots < app->num_procs) {
        if (!options->oversubscribe) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                           app->num_procs, app->app, prte_process_info.nodename);
            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
            return PRTE_ERR_SILENT;
        } else {
            if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                jdata->map->binding = PRTE_BIND_TO_NONE;
                options->bind = PRTE_BIND_TO_NONE;
            }
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
    /* Span cycles across the nodes, so it has to remember which target it
     * used last on each of them: the k'th proc a node receives goes on its
     * k'th target. Keyed by the node's slot in the global pool, which is the
     * one identifier that survives a node being dropped from the list.
     * A target set that cannot be revisited (nowrap) is left alone - it gets
     * one pass, so there is nothing to cycle. */
    interleave = (options->mapspan && !tgts->nowrap);
    if (interleave) {
        PMIX_LIST_FOREACH(node, node_list, prte_node_t) {
            if (node->index >= ncursor) {
                ncursor = node->index + 1;
            }
        }
        if (0 < ncursor) {
            cursor = (int *) calloc(ncursor, sizeof(int));
            if (NULL == cursor) {
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto errout;
            }
        } else {
            /* no node carries a pool index - nothing to key on, so place
             * these the way a non-span map would rather than guess */
            interleave = false;
        }
    }

    allfull = true;
    nprocs_mapped = 0;
    firstpass = true;
    do {
        allfull = true;
        /* Every pass after the first is placing procs the nodes have no room
         * for, which only happens when we were told we may oversubscribe.
         * Spread those evenly rather than letting the head of the list absorb
         * them: the same even split by-slot and by-node use for their second
         * pass, so the mappers answer an oversubscribed job alike. Recomputed
         * each time round because a node may take less than its share (its
         * cpus ran out, or it hit max_slots and left the list), and the next
         * pass should then divide what is left among the nodes still here. */
        if (!firstpass && options->oversubscribe && !interleave) {
            int nnodes = (int) pmix_list_get_size(node_list);
            int remaining = (int) app->num_procs - nprocs_mapped;
            if (0 >= nnodes) {
                break;
            }
            balance = (float) remaining / (float) nnodes;
            extra_procs_to_assign = (int) balance;
            nxtra_nodes = 0;
            if (0 < (balance - (float) extra_procs_to_assign)) {
                /* the first few nodes take one more than the rest */
                nxtra_nodes = remaining - (extra_procs_to_assign * nnodes);
                extra_procs_to_assign++;
            }
        }
        PMIX_LIST_FOREACH_SAFE(node, nnext, node_list, prte_node_t)
        {
            outofcpus = false;
            prte_rmaps_base_get_cpuset(jdata, node, options);
            if (NULL == options->job_cpuset) {
                // the prior function will have printed out the error
                rc = PRTE_ERR_SILENT;
                goto errout;
            }
            if (!options->donotlaunch) {
                rc = prte_rmaps_base_check_support(jdata, node, options);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    goto errout;
                }
            }

            options->nobjs = 0;
            /* have to delay checking for availability until we have the object */

            /* let the enumerator set up whatever it needs for this node */
            if (NULL != tgts->begin) {
                rc = tgts->begin(node, options, &ctx);
                if (PRTE_SUCCESS != rc) {
                    goto errout;
                }
                began = true;
            }

            /* get the number of targets on this node */
            nobjs = tgts->count(node, options, ctx);
            if (0 == nobjs) {
                /* We only ever map by an object because the user asked us
                 * to, so a node that has no such object is a request we
                 * cannot answer - say so. Quietly dropping the node instead
                 * shrank the allocation the user gave us without telling
                 * them, and quietly falling back to by-slot (which the
                 * caller used to do) placed the job by a rule they never
                 * asked for. */
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:mapping-target-not-found",
                               true, tgts->name, node->name);
                rc = PRTE_ERR_SILENT;
                goto errout;
            }
            pmix_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: found %u %s objects on node %s",
                                nobjs, tgts->name, node->name);

            /* How many procs may this node take on this pass?
             *
             * What a node offers a job is node->slots_available, not its slot
             * count: a ":N" suffix on a -host entry caps what this job may
             * take from it, and get_target_nodes records the cap there
             * (setup_proc consumes one per placed proc). Nothing below sees
             * it - check_avail and check_oversubscribed both measure against
             * node->slots - so without this the loop filled each node to its
             * slot count and left the rest of the -host list empty.
             *
             * Permission to oversubscribe does not change how the procs are
             * distributed, only how many a node may end up with. So the first
             * pass still hands each node exactly what it offers, and the
             * passes after it hand out the even share computed above. A tool
             * does not consume slots at all, so nothing bounds it here. */
            ndx = (NULL != cursor && 0 <= node->index && node->index < ncursor)
                  ? node->index : -1;
            start = 0;
            if (PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                budget = (int) app->num_procs;
            } else if (interleave) {
                /* one target per node per trip round the list - the cycling
                 * IS the load balance, so there is no share to compute here
                 * and no need to treat the first pass differently: a node
                 * simply stops taking procs when it has given what it has,
                 * unless we may oversubscribe, in which case it keeps its
                 * place in the rotation and the overflow spreads itself. */
                budget = (options->oversubscribe || 0 < node->slots_available) ? 1 : 0;
                if (0 <= ndx) {
                    start = (unsigned) (cursor[ndx] % (int) nobjs);
                }
            } else if (firstpass || !options->oversubscribe) {
                budget = node->slots_available;
            } else {
                budget = extra_procs_to_assign;
                if (0 < nxtra_nodes) {
                    --nxtra_nodes;
                    if (0 == nxtra_nodes) {
                        --extra_procs_to_assign;
                    }
                }
            }
            nodefull = false;
            nplaced = 0;
        redo:
            for (i=0; i < nobjs && nprocs_mapped < app->num_procs && !nodefull; i++) {
                /* the node-major walk takes the targets in order; the
                 * interleaved one resumes where this node left off, wrapping
                 * so an oversubscribed second lap starts over at the front */
                j = (0 == start) ? i : (unsigned) ((start + i) % nobjs);
                pmix_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr: assigning proc to object %d", j);
                if (nplaced >= budget) {
                    /* this node has had its share for this pass */
                    nodefull = true;
                    break;
                }
                /* get the target object */
                obj = tgts->item(node, options, ctx, j);
                if (NULL == obj) {
                    /* out of objects on this node */
                    break;
                }
                /* does this object have enough available cpus to
                 * support the requested cpus_per_rank? This only matters
                 * when we are actually going to bind. If binding has been
                 * turned off - e.g., because the node is oversubscribed and
                 * the top-of-function check reset an unset binding policy to
                 * BIND_TO_NONE - then a shortage of free cpus on the object
                 * must not block placement. Otherwise a genuinely
                 * oversubscribed node (whose cpus were already consumed by an
                 * earlier job, such as the parent of a PMIx_Spawn) would be
                 * wrongly rejected as overloaded and the unbound proc would
                 * fail to map. */
                ncpus = prte_rmaps_base_get_ncpus(node, obj, options);
                if (PRTE_BIND_TO_NONE != options->bind &&
                    ncpus < options->cpus_per_rank && !options->overload) {
                    outofcpus = true;
                    continue;
                }
                options->nprocs = 1;

                if (!prte_rmaps_base_check_avail(jdata, app, node, node_list, obj, options)) {
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    /* This node can take no more. Mark it done rather than
                     * merely breaking out of the object loop: without this the
                     * "redo" retry below re-enters the loop on the same node,
                     * and if check_avail declined because the node reached its
                     * max_slots bound it has already removed the node from
                     * node_list and released it - so the retry asks it to
                     * remove and release the very same node a second time,
                     * corrupting the list and dropping the node's last
                     * reference. That crashed the HNP for a --map-by <object>
                     * job on a hostfile carrying max_slots. */
                    nodefull = true;
                    break;
                }

                proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, obj, options);
                if (NULL == proc) {
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto errout;
                }
                /* let the enumerator record whatever the target means to it -
                 * a device map notes which device this proc was placed
                 * against, which the proc has no other way to learn */
                if (NULL != tgts->placed) {
                    tgts->placed(proc, options, ctx, j);
                }
                nprocs_mapped++;
                nplaced++;
                if (0 <= ndx) {
                    cursor[ndx]++;
                }
                rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
                if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                    /* move to next node */
                    pmix_list_remove_item(node_list, &node->super);
                    PMIX_RELEASE(node);
                    nodefull = true;
                    PMIX_RELEASE(proc);
                    break;
                } else if (PRTE_SUCCESS != rc) {
                    /* got an error */
                    PMIX_RELEASE(proc);
                    goto errout;
                }
                PMIX_RELEASE(proc);
                allfull = false;
            }
            if (nprocs_mapped < app->num_procs && !allfull &&
                !nodefull && !outofcpus && !options->mapspan &&
                !tgts->nowrap) {
                // keep working these objects until full
                goto redo;
            }
            // move to the next node
            if (began) {
                if (NULL != tgts->end) {
                    tgts->end(ctx);
                }
                ctx = NULL;
                began = false;
            }
            if (NULL != options->target) {
                hwloc_bitmap_free(options->target);
                options->target = NULL;
            }
        }
        /* one pass is all an unshareable target gets: coming round again
         * would put a second proc on a target already assigned */
        if (tgts->nowrap) {
            break;
        }
        wasfirst = firstpass;
        firstpass = false;
        /* A first pass that placed nothing at all normally means the job
         * cannot be placed - but not when we may oversubscribe: there every
         * node offering nothing is exactly the case oversubscription exists
         * for, and it is the pass after this one that places those procs.
         * Ending the loop here failed the map instead, which is what a
         * PMIx_Spawn onto a node its parent job has filled looks like. */
    } while (nprocs_mapped < app->num_procs &&
             (!allfull || (options->oversubscribe && wasfirst)));

    if (nprocs_mapped == app->num_procs) {
        if (NULL != cursor) {
            free(cursor);
        }
        return PRTE_SUCCESS;
    }

errout:
    if (NULL != cursor) {
        free(cursor);
        cursor = NULL;
    }
    /* an early exit can leave the enumerator holding this node's state */
    if (began && NULL != tgts->end) {
        tgts->end(ctx);
    }
    if (PRTE_ERR_SILENT == rc) {
        return rc;
    }
    if (outofcpus) {
        /* ran out of cpus */
        prte_show_help("help-prte-rmaps-base.txt",
                       "allocation-overload", true,
                       app->app, app->num_procs,
                       prte_rmaps_base_print_mapping(options->map),
                       prte_hwloc_base_print_binding(options->bind));
        return PRTE_ERR_SILENT;
    }
    prte_show_help("help-prte-rmaps-base.txt",
                   "failed-map", true,
                   PRTE_ERROR_NAME(rc),
                   app->app, app->num_procs,
                   prte_rmaps_base_print_mapping(options->map),
                   prte_hwloc_base_print_binding(options->bind));
    return PRTE_ERR_SILENT;
}
