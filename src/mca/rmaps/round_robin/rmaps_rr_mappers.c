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
/* ---------------------------------------------------------------------
 * Mapping by device
 *
 * The targets are the devices in the node's topology, and the object a proc
 * is placed against is the device's *locality* - the nearest ancestor with a
 * cpuset.  Everything after that is the shared loop: the proc is set up
 * against that object and bound within it exactly as it would be against a
 * package or a core.
 *
 * Enumerating the devices is PMIx's job, deliberately.  PMIx already reports
 * devices to applications through PMIX_DEVICE_DISTANCES, and the name PRRTE
 * tells a process it was assigned has to be the name that process will see
 * there - a second enumerator here could differ, and an assignment nobody
 * can correlate is worth nothing.
 * --------------------------------------------------------------------- */

/* per-node state: the device list, held for the life of one node's
 * placement and released when we move on */
typedef struct {
    pmix_hwloc_device_t *devs;
    size_t ndevs;
} prte_rmaps_devctx_t;

/* Map a --map-by device= value to a device class.  Returns
 * PMIX_DEVTYPE_UNKNOWN when the value is not a class, in which case it is
 * taken as the name or uuid of one particular device. */
static pmix_device_type_t device_class(const char *spec)
{
    char *s = (char *) spec;

    if (PMIX_CHECK_CLI_OPTION(s, "gpu")) {
        /* a coprocessor is a GPU that hwloc happened to learn about through
         * a vendor backend rather than through DRM */
        return PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
    }
    if (PMIX_CHECK_CLI_OPTION(s, "openfabrics")) {
        return PMIX_DEVTYPE_OPENFABRICS;
    }
    if (PMIX_CHECK_CLI_OPTION(s, "network")) {
        return PMIX_DEVTYPE_NETWORK;
    }
    if (PMIX_CHECK_CLI_OPTION(s, "nic")) {
        return PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS;
    }
    if (PMIX_CHECK_CLI_OPTION(s, "block")) {
        return PMIX_DEVTYPE_BLOCK;
    }
    return PMIX_DEVTYPE_UNKNOWN;
}

/* Can we bind where we were asked to, given where the device is?
 *
 * "Near this device" is the whole request, and an object that strictly
 * contains the device's locality is not near it - binding there would hand
 * the proc cpus the device is not local to, which is the locality loss the
 * directive exists to avoid.  Compare cpusets rather than PRTE_BIND_TO_*
 * levels: the locality is frequently an hwloc Group, which has no position
 * in that ladder, so there is no level to compare against.
 */
static bool binding_fits(prte_node_t *node, hwloc_obj_t locality,
                         prte_rmaps_options_t *options)
{
    hwloc_obj_t obj;
    int nobjs, n;

    if (PRTE_BIND_TO_NONE == options->bind || NULL == locality
        || NULL == locality->cpuset) {
        return true;
    }
    nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, options->hwb);
    for (n = 0; n < nobjs; n++) {
        obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, options->hwb, n);
        if (NULL == obj || NULL == obj->cpuset) {
            continue;
        }
        if (!hwloc_bitmap_intersects(obj->cpuset, locality->cpuset)) {
            continue;
        }
        /* a binding target that covers the locality and more is above it */
        if (hwloc_bitmap_isincluded(locality->cpuset, obj->cpuset)
            && !hwloc_bitmap_isequal(locality->cpuset, obj->cpuset)) {
            return false;
        }
    }
    return true;
}

/* Reorder the device list so that consecutive processes land on different
 * objects of the given level.
 *
 * Group the devices by the <level> object containing each one's locality,
 * then take one device from each group in turn, dropping a group when it is
 * exhausted.  On a node whose GPUs are two per socket, interleaving across
 * packages turns 0,1,2,3 into 0,2,1,3 - so -n 2 lands on different sockets
 * rather than filling the first.
 *
 * This never invents an ordering, it only redistributes across groups: a
 * level that does not partition the devices (one group, or one device per
 * group) reproduces the input order exactly.  That is what makes the
 * qualifier safe to leave in a site's default mapping policy.
 */
static void interleave_devices(hwloc_topology_t topo, hwloc_obj_type_t level,
                               pmix_hwloc_device_t *devs, size_t ndevs)
{
    pmix_hwloc_device_t *out;
    hwloc_obj_t *keys;
    size_t *headof;     /* next index to take from each group */
    size_t *counts;
    size_t ngroups = 0, n, g, o = 0;
    hwloc_obj_t key;

    if (2 > ndevs) {
        return;
    }
    out = (pmix_hwloc_device_t *) malloc(ndevs * sizeof(pmix_hwloc_device_t));
    keys = (hwloc_obj_t *) calloc(ndevs, sizeof(hwloc_obj_t));
    headof = (size_t *) calloc(ndevs, sizeof(size_t));
    counts = (size_t *) calloc(ndevs, sizeof(size_t));
    if (NULL == out || NULL == keys || NULL == headof || NULL == counts) {
        free(out);
        free(keys);
        free(headof);
        free(counts);
        return;     /* the plain order is a valid answer */
    }

    /* group key: the <level> object containing this device's locality.  A
     * device with no such ancestor gets a NULL key and forms its own group,
     * which keeps it in the rotation rather than dropping it */
    for (n = 0; n < ndevs; n++) {
        key = NULL;
        if (NULL != devs[n].locality) {
            if (level == devs[n].locality->type) {
                key = devs[n].locality;
            } else {
                key = hwloc_get_ancestor_obj_by_type(topo, level, devs[n].locality);
            }
        }
        for (g = 0; g < ngroups; g++) {
            if (keys[g] == key) {
                break;
            }
        }
        if (g == ngroups) {
            keys[ngroups] = key;
            ++ngroups;
        }
        ++counts[g];
    }

    /* round-robin across the groups, in order of first appearance */
    while (o < ndevs) {
        for (g = 0; g < ngroups; g++) {
            if (0 == counts[g]) {
                continue;   /* this group is exhausted */
            }
            /* the next device belonging to group g */
            for (n = headof[g]; n < ndevs; n++) {
                key = NULL;
                if (NULL != devs[n].locality) {
                    if (level == devs[n].locality->type) {
                        key = devs[n].locality;
                    } else {
                        key = hwloc_get_ancestor_obj_by_type(topo, level, devs[n].locality);
                    }
                }
                if (key == keys[g]) {
                    out[o++] = devs[n];
                    headof[g] = n + 1;
                    --counts[g];
                    break;
                }
            }
        }
    }

    memcpy(devs, out, ndevs * sizeof(pmix_hwloc_device_t));
    free(out);
    free(keys);
    free(headof);
    free(counts);
}

static int device_targets_begin(prte_node_t *node, prte_rmaps_options_t *opts,
                                void **ctx)
{
    prte_rmaps_devctx_t *dc;
    pmix_device_type_t type;
    const char *byname = NULL;
    pmix_topology_t topo;
    pmix_status_t prc;
    size_t n;
    bool degenerate = true;

    *ctx = NULL;

    type = device_class(opts->map_device);
    if (PMIX_DEVTYPE_UNKNOWN == type) {
        /* not a class, so it names one particular device: every proc is
         * placed near that one */
        byname = opts->map_device;
    }

    dc = (prte_rmaps_devctx_t *) calloc(1, sizeof(prte_rmaps_devctx_t));
    if (NULL == dc) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    topo.source = "hwloc";
    topo.topology = node->topology->topo;
    prc = pmix_hwloc_get_devices(&topo, type, byname, &dc->devs, &dc->ndevs);
    if (PMIX_SUCCESS != prc) {
        free(dc);
        return prte_pmix_convert_status(prc);
    }

    /* A node with none of the requested devices cannot answer the request.
     * Say so rather than dropping the node, which would shrink the
     * allocation the user gave us without telling them - the shared loop
     * reports this when the count comes back zero, so just hand it over. */
    if (0 == dc->ndevs) {
        *ctx = dc;
        return PRTE_SUCCESS;
    }

    /* reorder before anything reads the list */
    if (NULL != opts->map_interleave) {
        hwloc_obj_type_t level = HWLOC_OBJ_PACKAGE;
        if (prte_rmaps_base_interleave_level(opts->map_interleave, &level)) {
            interleave_devices(node->topology->topo, level, dc->devs, dc->ndevs);
        }
    }

    /* Refuse a binding coarser than the devices are local to, before any
     * proc is placed on this node. */
    for (n = 0; n < dc->ndevs; n++) {
        if (!binding_fits(node, dc->devs[n].locality, opts)) {
            prte_show_help("help-prte-rmaps-base.txt", "rmaps:bind-above-device", true,
                           prte_hwloc_base_print_binding(opts->bind),
                           opts->map_device, node->name);
            pmix_hwloc_release_devices(dc->devs, dc->ndevs);
            free(dc);
            return PRTE_ERR_SILENT;
        }
    }

    /* If every device resolves to the same place, "near this device" is
     * saying nothing about cpus - each proc still gets a distinct device,
     * which is half of what was asked for, so proceed and say so. */
    for (n = 1; n < dc->ndevs; n++) {
        if (dc->devs[n].locality != dc->devs[0].locality) {
            degenerate = false;
            break;
        }
    }
    if (degenerate && 1 < dc->ndevs) {
        prte_show_help("help-prte-rmaps-base.txt", "rmaps:degenerate-device-locality",
                       true, opts->map_device, node->name, (int) dc->ndevs);
    }

    *ctx = dc;
    return PRTE_SUCCESS;
}

static unsigned device_targets_count(prte_node_t *node, prte_rmaps_options_t *opts,
                                     void *ctx)
{
    prte_rmaps_devctx_t *dc = (prte_rmaps_devctx_t *) ctx;

    PRTE_HIDE_UNUSED_PARAMS(node, opts);
    if (NULL == dc) {
        return 0;
    }
    return (unsigned) dc->ndevs;
}

static hwloc_obj_t device_targets_item(prte_node_t *node, prte_rmaps_options_t *opts,
                                       void *ctx, unsigned j)
{
    prte_rmaps_devctx_t *dc = (prte_rmaps_devctx_t *) ctx;

    PRTE_HIDE_UNUSED_PARAMS(node, opts);
    if (NULL == dc || (size_t) j >= dc->ndevs) {
        return NULL;
    }
    return dc->devs[j].locality;
}

/* Record which device this proc was placed against.  PRRTE cannot bind a
 * process to a device - no such mechanism exists - so telling the process
 * which one it got is the whole of what the assignment is worth.  The UUID
 * is what travels, not an index: it is the same string PMIx reports for that
 * device through PMIX_DEVICE_DISTANCES, so the process can correlate the
 * two, whereas an ordinal would depend on whose numbering was meant. */
static void device_targets_placed(prte_proc_t *proc, prte_rmaps_options_t *opts,
                                  void *ctx, unsigned j)
{
    prte_rmaps_devctx_t *dc = (prte_rmaps_devctx_t *) ctx;

    PRTE_HIDE_UNUSED_PARAMS(opts);
    if (NULL == dc || (size_t) j >= dc->ndevs || NULL == dc->devs[j].dev.uuid) {
        return;
    }
    /* LOCAL, even though the daemon that forks this proc needs it: no proc
     * attribute list goes on the wire at all (see prte_proc_pack), so the
     * value travels as its own field, packed only for a job that was mapped
     * by device. Marking it global would put it in a list nothing packs and
     * trip the guard that exists to catch exactly that. */
    prte_set_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID, PRTE_ATTR_LOCAL,
                       dc->devs[j].dev.uuid, PMIX_STRING);
}

static void device_targets_end(void *ctx)
{
    prte_rmaps_devctx_t *dc = (prte_rmaps_devctx_t *) ctx;

    if (NULL == dc) {
        return;
    }
    if (NULL != dc->devs) {
        pmix_hwloc_release_devices(dc->devs, dc->ndevs);
    }
    free(dc);
}

int prte_rmaps_rr_bydevice(prte_job_t *jdata, prte_app_context_t *app,
                           pmix_list_t *node_list, int32_t num_slots,
                           pmix_rank_t num_procs,
                           prte_rmaps_options_t *options)
{
    prte_rmaps_target_enum_t tgts = {
        .begin = device_targets_begin,
        .count = device_targets_count,
        .item = device_targets_item,
        .placed = device_targets_placed,
        .end = device_targets_end,
        .name = options->map_device
    };

    if (NULL == options->map_device) {
        /* the policy cannot be set without a value - see the two --map-by
         * parsers - so this is a programming error, not user input */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
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
    int ncpus;
    prte_proc_t *proc;
    bool nodefull, allfull, outofcpus=false;
    hwloc_obj_t obj = NULL;
    unsigned j, nobjs;
    void *ctx = NULL;
    bool began = false;

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
    allfull = true;
    nprocs_mapped = 0;
    do {
        allfull = true;
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

            nodefull = false;
        redo:
            for (j=0; j < nobjs && nprocs_mapped < app->num_procs && !nodefull; j++) {
                pmix_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr: assigning proc to object %d", j);
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
                !nodefull && !outofcpus && !options->mapspan) {
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
    } while (nprocs_mapped < app->num_procs && !allfull);

    if (nprocs_mapped == app->num_procs) {
        return PRTE_SUCCESS;
    }

errout:
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
