/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Enumerating the devices a node offers, for the mappers that place
 * processes against them.
 *
 * This lives in the base rather than in one mapper because two of them need
 * the same answer: round_robin places one process per device (--map-by
 * device=), and ppr places N per device (--map-by ppr:N:device=).  They
 * differ only in how many processes a device takes, which is no reason for
 * two enumerations that could disagree about what the devices ARE.
 *
 * The enumeration itself is PMIx's - see pmix_hwloc_get_devices() - because
 * the names PRRTE assigns have to be the names PMIx reports to the same
 * process through PMIX_DEVICE_DISTANCES.  What is here is everything
 * between that list and a placement: the class spelling, the interleave
 * ordering, the grouping into one group per process, the locality each
 * group resolves to, and the two checks that have to happen before any
 * process is placed on a node.
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/prte_show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

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
    /* Devices are handed out in groups of "ndev" - one group per proc.  The
     * group's locality is the common ancestor of its members' localities,
     * because a proc given two GPUs on different NUMA domains is local to
     * neither of them alone; it is local to whatever contains both.  With
     * the default of one device per proc a group IS a device and the
     * ancestor is that device's own locality, so nothing changes. */
    hwloc_obj_t *grouploc;
    size_t ngroups;
    size_t per;                 /* devices per group */
} prte_rmaps_device_map_t;

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

int prte_rmaps_base_devices_begin(prte_node_t *node, prte_rmaps_options_t *opts,
                                void **ctx)
{
    prte_rmaps_device_map_t *dc;
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

    dc = (prte_rmaps_device_map_t *) calloc(1, sizeof(prte_rmaps_device_map_t));
    if (NULL == dc) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    topo.source = "hwloc";
    topo.topology = node->topology->topo;
    /* Name the node, do not let PMIx assume it.  A device uuid embeds the
     * host the device lives on, and the mapper runs on the HNP: left to
     * default, every device on every node in the job would come back stamped
     * with the HNP's hostname, and the process that later computes the same
     * uuid from its own topology would fail to match the one it was given -
     * which is the whole reason the uuid travels rather than an ordinal. */
    prc = pmix_hwloc_get_devices(&topo, node->name, type, byname,
                                 &dc->devs, &dc->ndevs);
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

    /* Group the devices, one group per proc */
    dc->per = (0 == opts->map_ndev) ? 1 : opts->map_ndev;
    dc->ngroups = dc->ndevs / dc->per;
    if (0 == dc->ngroups) {
        /* not enough devices on this node to make even one group - the
         * caller reports it as "no such device", naming what was asked for */
        pmix_hwloc_release_devices(dc->devs, dc->ndevs);
        dc->devs = NULL;
        dc->ndevs = 0;
        *ctx = dc;
        return PRTE_SUCCESS;
    }
    dc->grouploc = (hwloc_obj_t *) calloc(dc->ngroups, sizeof(hwloc_obj_t));
    if (NULL == dc->grouploc) {
        pmix_hwloc_release_devices(dc->devs, dc->ndevs);
        free(dc);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    for (n = 0; n < dc->ngroups; n++) {
        size_t m;
        hwloc_obj_t loc = dc->devs[n * dc->per].locality;
        for (m = 1; m < dc->per; m++) {
            hwloc_obj_t other = dc->devs[n * dc->per + m].locality;
            if (NULL == loc || NULL == other) {
                loc = NULL;
                break;
            }
            loc = hwloc_get_common_ancestor_obj(node->topology->topo, loc, other);
        }
        dc->grouploc[n] = loc;
    }

    /* Refuse a binding coarser than the devices are local to, before any
     * proc is placed on this node.  Checked against the GROUP locality:
     * with several devices per proc that is deliberately coarser, and a
     * binding matching it is then legitimate. */
    for (n = 0; n < dc->ngroups; n++) {
        if (!binding_fits(node, dc->grouploc[n], opts)) {
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
    for (n = 1; n < dc->ngroups; n++) {
        if (dc->grouploc[n] != dc->grouploc[0]) {
            degenerate = false;
            break;
        }
    }
    if (degenerate && 1 < dc->ngroups) {
        prte_show_help("help-prte-rmaps-base.txt", "rmaps:degenerate-device-locality",
                       true, opts->map_device, node->name, (int) dc->ngroups);
    }

    *ctx = dc;
    return PRTE_SUCCESS;
}

unsigned prte_rmaps_base_devices_count(prte_node_t *node, prte_rmaps_options_t *opts,
                                     void *ctx)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;

    PRTE_HIDE_UNUSED_PARAMS(node, opts);
    if (NULL == dc) {
        return 0;
    }
    return (unsigned) dc->ngroups;
}

hwloc_obj_t prte_rmaps_base_devices_locale(prte_node_t *node, prte_rmaps_options_t *opts,
                                       void *ctx, unsigned j)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;

    PRTE_HIDE_UNUSED_PARAMS(node, opts);
    if (NULL == dc || (size_t) j >= dc->ngroups) {
        return NULL;
    }
    return dc->grouploc[j];
}

/* Record which device this proc was placed against.  PRRTE cannot bind a
 * process to a device - no such mechanism exists - so telling the process
 * which one it got is the whole of what the assignment is worth.  The UUID
 * is what travels, not an index: it is the same string PMIx reports for that
 * device through PMIX_DEVICE_DISTANCES, so the process can correlate the
 * two, whereas an ordinal would depend on whose numbering was meant. */
void prte_rmaps_base_devices_record(prte_proc_t *proc, prte_rmaps_options_t *opts,
                                  void *ctx, unsigned j)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;
    pmix_data_array_t *darray;
    pmix_device_t *dev;
    size_t m;

    PRTE_HIDE_UNUSED_PARAMS(opts);
    if (NULL == dc || (size_t) j >= dc->ngroups) {
        return;
    }
    /* LOCAL, even though the daemon that forks this proc needs it: no proc
     * attribute list goes on the wire at all (see prte_proc_pack), so the
     * value travels as its own field, packed only for a job that was mapped
     * by device. Marking it global would put it in a list nothing packs and
     * trip the guard that exists to catch exactly that. */
    /* Always an array of pmix_device_t, even when it holds one entry.
     *
     * A process assigned two devices and a process assigned one are the same
     * kind of answer differing in length, and flattening the common case
     * into a bare string would make them different kinds - so every reader
     * would need both paths, and the one-device path would be the only one
     * anybody tested.  The array also carries what a bare uuid cannot: each
     * device's OS name and type, which is what PMIx reports for a device
     * everywhere else. */
    PMIX_DATA_ARRAY_CREATE(darray, dc->per, PMIX_DEVICE);
    if (NULL == darray) {
        return;
    }
    dev = (pmix_device_t *) darray->array;
    for (m = 0; m < dc->per; m++) {
        pmix_device_t *src = &dc->devs[j * dc->per + m].dev;
        PMIx_Device_construct(&dev[m]);
        if (NULL != src->uuid) {
            dev[m].uuid = strdup(src->uuid);
        }
        if (NULL != src->osname) {
            dev[m].osname = strdup(src->osname);
        }
        dev[m].type = src->type;
    }
    prte_set_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID, PRTE_ATTR_LOCAL,
                       darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(darray);
}

void prte_rmaps_base_devices_end(void *ctx)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;

    if (NULL == dc) {
        return;
    }
    if (NULL != dc->devs) {
        pmix_hwloc_release_devices(dc->devs, dc->ndevs);
    }
    if (NULL != dc->grouploc) {
        free(dc->grouploc);
    }
    free(dc);
}

/* How many devices of the requested class the whole node list offers.  Used
 * only to answer "are there enough?" before any proc is placed; the mapper
 * enumerates each node again as it reaches it. */
size_t prte_rmaps_base_devices_total(pmix_list_t *node_list, prte_rmaps_options_t *options)
{
    prte_node_t *node;
    void *ctx = NULL;
    size_t total = 0;

    PMIX_LIST_FOREACH(node, node_list, prte_node_t) {
        if (PRTE_SUCCESS != prte_rmaps_base_devices_begin(node, options, &ctx)) {
            continue;
        }
        total += prte_rmaps_base_devices_count(node, options, ctx);
        prte_rmaps_base_devices_end(ctx);
        ctx = NULL;
    }
    return total;
}

