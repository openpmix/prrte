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
 * group resolves to, and the checks that have to happen before any process
 * is placed on a node - that every GPU can be named to its runtime, that
 * the binding is not coarser than the devices, and whether the devices say
 * anything about cpus at all.
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
    /* ...and the cpus actually local to its members - the union of their
     * localities, which is what a binding finer than the ancestor has to
     * be chosen from.  NULL for a group whose locality is unknown. */
    hwloc_cpuset_t *groupcpus;
    size_t ngroups;
    size_t per;                 /* devices per group */
} prte_rmaps_device_map_t;

/* The device classes, and the spellings of each.
 *
 * Every spelling of "the thing this node talks to the network with" means
 * the same set, deliberately.  One HCA presents itself twice - an
 * OpenFabrics OS device (mlx5_0) and a network one (ib0) on the same PCI
 * function - and a user asking for a NIC wants the card, not one of
 * hwloc's two views of it.  Splitting the spellings, as this once did,
 * made "network" and "openfabrics" return the same hardware under
 * different names and gave whichever the user did not type an answer that
 * looked wrong.  The enumeration dedupes by PCI function, so the union is
 * one entry per card; a particular interface is still reachable by naming
 * it (device=eno6).
 *
 * The cost is that there is no longer a spelling for "ethernet only".
 * That is a narrower question than the directive is for, and naming the
 * interface answers it exactly.
 *
 * A coprocessor is a GPU that hwloc happened to learn about through a
 * vendor backend rather than through DRM, so the GPU class takes both. */
enum {
    DEVCLASS_GPU,
    DEVCLASS_NETWORK,
    DEVCLASS_BLOCK
};

static const pmix_cli_choice_t device_classes[] = {
    PMIX_CLI_CHOICE("gpu", DEVCLASS_GPU, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE("network", DEVCLASS_NETWORK, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE("openfabrics", DEVCLASS_NETWORK, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE("fabric", DEVCLASS_NETWORK, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE("nic", DEVCLASS_NETWORK, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE("block", DEVCLASS_BLOCK, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE_END
};

/* Map a --map-by device= value to a device class.  Returns
 * PMIX_DEVTYPE_UNKNOWN when the value is not a class, in which case it is
 * taken as the name or uuid of one particular device.
 *
 * A class may be abbreviated, but nothing may follow it: the comparison
 * used to stop at the end of the class name, so "gpu,ndev=2" - a request
 * for two GPUs per process written with a comma - was the class "gpu" and
 * each process silently got one. */
static pmix_device_type_t device_class(const char *spec)
{
    int tag;

    if (PMIX_CLI_MATCH_FOUND != pmix_cli_match(spec, device_classes, &tag)) {
        return PMIX_DEVTYPE_UNKNOWN;
    }
    switch (tag) {
        case DEVCLASS_GPU:
            return PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
        case DEVCLASS_NETWORK:
            return PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS;
        case DEVCLASS_BLOCK:
            return PMIX_DEVTYPE_BLOCK;
        default:
            return PMIX_DEVTYPE_UNKNOWN;
    }
}

/* Does the spec name one device rather than a class?
 *
 * The two forms are different requests, not one request of different sizes.
 * A class is a set of devices to hand out, one process each unless "shared"
 * says otherwise.  A name is "put every process near this device" - the old
 * dist policy in one directive - so the processes share it by definition,
 * and refusing -n 8 against the one device the user named, as the class rule
 * would, refuses the only thing the form is for. */
bool prte_rmaps_base_devices_named(const char *spec)
{
    return (NULL != spec && PMIX_DEVTYPE_UNKNOWN == device_class(spec));
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
/* The <level> object a device's locality lies within, or NULL if none does.
 *
 * Found by cpuset rather than by walking up from the locality, because the
 * walk cannot find a NUMA domain: in hwloc 2 a NUMA node is a memory child
 * of the object it is attached to, never anybody's ancestor, so asking for
 * the NUMANODE ancestor of a device's locality answers NULL on every
 * machine.  "interleave=numa" then put every device in one group and
 * reproduced the input order - silently, since that is also the documented
 * answer for a level that does not partition the devices.  For the other
 * levels containment by cpuset is the same answer the walk gave.
 *
 * Asked through the PRRTE wrappers so that NUMA means the CPU NUMA domains
 * only - see src/hwloc/AGENTS.md. */
static hwloc_obj_t interleave_key(hwloc_topology_t topo, hwloc_obj_type_t level,
                                  hwloc_obj_t locality)
{
    hwloc_obj_t obj;
    unsigned n, nobjs;

    if (NULL == locality || NULL == locality->cpuset
        || hwloc_bitmap_iszero(locality->cpuset)) {
        return NULL;
    }
    nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, level);
    for (n = 0; n < nobjs; n++) {
        obj = prte_hwloc_base_get_obj_by_type(topo, level, n);
        if (NULL != obj && NULL != obj->cpuset
            && hwloc_bitmap_isincluded(locality->cpuset, obj->cpuset)) {
            return obj;
        }
    }
    return NULL;
}

static void interleave_devices(hwloc_topology_t topo, hwloc_obj_type_t level,
                               pmix_hwloc_device_t *devs, size_t ndevs)
{
    pmix_hwloc_device_t *out;
    hwloc_obj_t *keys;
    hwloc_obj_t *devkey;    /* the group key of each device */
    size_t *headof;     /* next index to take from each group */
    size_t *counts;
    size_t ngroups = 0, n, g, o = 0;

    if (2 > ndevs) {
        return;
    }
    out = (pmix_hwloc_device_t *) malloc(ndevs * sizeof(pmix_hwloc_device_t));
    keys = (hwloc_obj_t *) calloc(ndevs, sizeof(hwloc_obj_t));
    devkey = (hwloc_obj_t *) calloc(ndevs, sizeof(hwloc_obj_t));
    headof = (size_t *) calloc(ndevs, sizeof(size_t));
    counts = (size_t *) calloc(ndevs, sizeof(size_t));
    if (NULL == out || NULL == keys || NULL == devkey || NULL == headof || NULL == counts) {
        free(out);
        free(keys);
        free(devkey);
        free(headof);
        free(counts);
        return;     /* the plain order is a valid answer */
    }

    /* group key: the <level> object containing this device's locality.  A
     * device with no such object gets a NULL key and forms its own group,
     * which keeps it in the rotation rather than dropping it */
    for (n = 0; n < ndevs; n++) {
        devkey[n] = interleave_key(topo, level, devs[n].locality);
        for (g = 0; g < ngroups; g++) {
            if (keys[g] == devkey[n]) {
                break;
            }
        }
        if (g == ngroups) {
            keys[ngroups] = devkey[n];
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
                if (devkey[n] == keys[g]) {
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
    free(devkey);
    free(headof);
    free(counts);
}

/* The devices the request names on this node, in PMIx's order. */
static int enumerate_devices(prte_node_t *node, prte_rmaps_options_t *opts,
                             pmix_hwloc_device_t **devs, size_t *ndevs)
{
    pmix_device_type_t type;
    const char *byname = NULL;
    pmix_topology_t topo;
    pmix_status_t prc;

    type = device_class(opts->map_device);
    if (PMIX_DEVTYPE_UNKNOWN == type) {
        /* not a class, so it names one particular device - which is then
         * the node's only target, and every proc on the node shares it */
        byname = opts->map_device;
    }

    topo.source = "hwloc";
    topo.topology = node->topology->topo;
    /* Name the node, do not let PMIx assume it.  A device uuid embeds the
     * host the device lives on, and the mapper runs on the HNP: left to
     * default, every device on every node in the job would come back stamped
     * with the HNP's hostname, and the process that later computes the same
     * uuid from its own topology would fail to match the one it was given -
     * which is the whole reason the uuid travels rather than an ordinal. */
    prc = pmix_hwloc_get_devices(&topo, node->name, type, byname, devs, ndevs);
    if (PMIX_SUCCESS != prc) {
        return prte_pmix_convert_status(prc);
    }
    return PRTE_SUCCESS;
}

int prte_rmaps_base_devices_begin(prte_job_t *jdata, prte_node_t *node,
                                  prte_rmaps_options_t *opts, void **ctx)
{
    prte_rmaps_device_map_t *dc;
    size_t n;
    int rc;
    bool degenerate = true;

    *ctx = NULL;

    dc = (prte_rmaps_device_map_t *) calloc(1, sizeof(prte_rmaps_device_map_t));
    if (NULL == dc) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    rc = enumerate_devices(node, opts, &dc->devs, &dc->ndevs);
    if (PRTE_SUCCESS != rc) {
        free(dc);
        return rc;
    }

    /* A node with none of the requested devices cannot answer the request.
     * Say so rather than dropping the node, which would shrink the
     * allocation the user gave us without telling them - the shared loop
     * reports this when the count comes back zero, so just hand it over. */
    if (0 == dc->ndevs) {
        *ctx = dc;
        return PRTE_SUCCESS;
    }

    /* Refuse to map by a GPU we cannot name.
     *
     * An assignment is only worth making if the process can act on it, and
     * for a GPU that means naming the device to the vendor's runtime -
     * which accepts the vendor's own identifier and nothing else.  hwloc
     * records that identifier only from its vendor backends (NVML, RSMI,
     * Level Zero), so a PRRTE built against an hwloc without them sees the
     * GPUs, places against them correctly, and hands the process a name no
     * library it links has ever heard of.
     *
     * Mapping anyway and quietly setting nothing is the worst of the
     * options: the job runs, the placement looks right in --display map,
     * and the only symptom is that every rank on the node contends for the
     * same GPU - which nobody discovers until somebody measures.  So say so
     * up front, and name the fix.
     *
     * Only for GPUs.  A fabric or network device is named by its own GUIDs
     * or MAC, which hwloc always has and which is the identifier the fabric
     * libraries use; there is no vendor backend to be missing.
     *
     * Decided by what each device IS, not by the class the user asked for:
     * naming one GPU by its OS device ("renderD129") is not a class request,
     * and gets the process exactly the same unactionable assignment.
     *
     * Checked against this node's recorded topology, which is sound even
     * though the HNP shares one topology between nodes reporting identical
     * hardware: an absent info attribute changes an object's info count,
     * hwloc reports that as a "too complex" difference rather than an
     * expressible one, and PRRTE records such a node separately.  So
     * WHETHER identity exists cannot vary within a shared topology - only
     * its value can, and that is read on the node itself. */
    for (n = 0; n < dc->ndevs; n++) {
        if (0 != (dc->devs[n].dev.type & (PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC))
            && NULL == dc->devs[n].vendor_id) {
            prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:device-not-nameable",
                           true, opts->map_device, node->name,
                           dc->devs[n].dev.osname);
            pmix_hwloc_release_devices(dc->devs, dc->ndevs);
            free(dc);
            return PRTE_ERR_SILENT;
        }
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
    dc->groupcpus = (hwloc_cpuset_t *) calloc(dc->ngroups, sizeof(hwloc_cpuset_t));
    if (NULL == dc->grouploc || NULL == dc->groupcpus) {
        prte_rmaps_base_devices_end(dc);
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
        if (NULL == loc) {
            continue;
        }
        dc->groupcpus[n] = hwloc_bitmap_alloc();
        if (NULL == dc->groupcpus[n]) {
            prte_rmaps_base_devices_end(dc);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        for (m = 0; m < dc->per; m++) {
            hwloc_bitmap_or(dc->groupcpus[n], dc->groupcpus[n],
                            dc->devs[n * dc->per + m].locality->cpuset);
        }
    }

    /* Refuse a binding coarser than the devices are local to, before any
     * proc is placed on this node.  Checked against the GROUP locality:
     * with several devices per proc that is deliberately coarser, and a
     * binding matching it is then legitimate. */
    for (n = 0; n < dc->ngroups; n++) {
        if (!binding_fits(node, dc->grouploc[n], opts)) {
            prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:bind-above-device", true,
                           prte_hwloc_base_print_binding(opts->bind),
                           opts->map_device, node->name);
            prte_rmaps_base_devices_end(dc);
            return PRTE_ERR_SILENT;
        }
    }

    /* If every device resolves to the same place, "near this device" is
     * saying nothing about cpus - each proc still gets a distinct device,
     * which is half of what was asked for, so proceed and say so.
     *
     * Decided by the devices, not by the groups: that is what the message
     * says - that the machine hangs every device off one place.  Groups can
     * coincide on a machine where no two devices do - with ndev and
     * interleave together, each proc gets one GPU from each package, so
     * every group's common ancestor is the whole node - and the message
     * then described a machine the user did not have. */
    for (n = 1; n < dc->ngroups * dc->per; n++) {
        if (dc->devs[n].locality != dc->devs[0].locality) {
            degenerate = false;
            break;
        }
    }
    if (degenerate && 1 < dc->ngroups) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:degenerate-device-locality",
                       true, opts->map_device, node->name, (int) (dc->ngroups * dc->per));
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

    PRTE_HIDE_UNUSED_PARAMS(node);
    if (NULL == dc || (size_t) j >= dc->ngroups) {
        return NULL;
    }
    /* what a binding finer than the returned object is chosen from - see
     * narrow_to_devices() in rmaps_base_binding.c.  Copied, not lent: the
     * context is gone by the time the options are, and a group whose
     * locality is unknown leaves nothing to narrow to. */
    if (NULL == dc->groupcpus[j]) {
        if (NULL != opts->devcpus) {
            hwloc_bitmap_free(opts->devcpus);
            opts->devcpus = NULL;
        }
    } else if (NULL == opts->devcpus) {
        opts->devcpus = hwloc_bitmap_dup(dc->groupcpus[j]);
    } else {
        hwloc_bitmap_copy(opts->devcpus, dc->groupcpus[j]);
    }
    return dc->grouploc[j];
}

/* Record which device this proc was placed against.  PRRTE cannot bind a
 * process to a device - no such mechanism exists - so telling the process
 * which one it got is the whole of what the assignment is worth.  The UUID
 * is what travels, not an index: it is the same string PMIx reports for that
 * device through PMIX_DEVICE_DISTANCES, so the process can correlate the
 * two, whereas an ordinal would depend on whose numbering was meant. */
int prte_rmaps_base_devices_record(prte_proc_t *proc, prte_rmaps_options_t *opts,
                                   void *ctx, unsigned j)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;
    pmix_data_array_t *darray;
    pmix_device_t *dev;
    size_t m;
    int rc;

    PRTE_HIDE_UNUSED_PARAMS(opts);
    if (NULL == dc || (size_t) j >= dc->ngroups) {
        /* the caller placed a proc against a target this context never
         * offered - there is no device to tell it about */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
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
     * everywhere else.
     *
     * A failure here fails the map.  The proc is already placed, and a proc
     * launched without its assignment is the silent outcome this whole
     * feature exists to prevent: it runs, it looks right, and it computes
     * on whichever device its runtime picks by default. */
    PMIX_DATA_ARRAY_CREATE(darray, dc->per, PMIX_DEVICE);
    if (NULL == darray) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
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
        if ((NULL != src->uuid && NULL == dev[m].uuid)
            || (NULL != src->osname && NULL == dev[m].osname)) {
            /* an entry with its identity missing is the same silent
             * non-assignment as no entry at all */
            PMIX_DATA_ARRAY_FREE(darray);
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        dev[m].type = src->type;
    }
    rc = prte_set_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID, PRTE_ATTR_LOCAL,
                            darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(darray);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

void prte_rmaps_base_devices_end(void *ctx)
{
    prte_rmaps_device_map_t *dc = (prte_rmaps_device_map_t *) ctx;
    size_t n;

    if (NULL == dc) {
        return;
    }
    if (NULL != dc->devs) {
        pmix_hwloc_release_devices(dc->devs, dc->ndevs);
    }
    if (NULL != dc->grouploc) {
        free(dc->grouploc);
    }
    if (NULL != dc->groupcpus) {
        for (n = 0; n < dc->ngroups; n++) {
            if (NULL != dc->groupcpus[n]) {
                hwloc_bitmap_free(dc->groupcpus[n]);
            }
        }
        free(dc->groupcpus);
    }
    free(dc);
}

/* How many processes the devices on the whole node list can take.  Used
 * only to answer "are there enough?" before any proc is placed; the mapper
 * enumerates each node again as it reaches it.
 *
 * This counts the hardware and deliberately judges nothing.  The refusals in
 * begin() are made - and explained - when the mapper reaches the node; run
 * here too, each one was printed, its node was then skipped, and the count
 * came up short, so the user was also told there were too few devices on a
 * node list that had plenty.  An enumeration that fails outright is a
 * different matter and is returned: a count that silently leaves a node out
 * is exactly that wrong answer again. */
int prte_rmaps_base_devices_total(pmix_list_t *node_list, prte_rmaps_options_t *options,
                                  size_t *total)
{
    prte_node_t *node;
    pmix_hwloc_device_t *devs;
    size_t ndevs, per;
    int rc;

    *total = 0;
    per = (0 == options->map_ndev) ? 1 : options->map_ndev;
    PMIX_LIST_FOREACH(node, node_list, prte_node_t) {
        devs = NULL;
        ndevs = 0;
        rc = enumerate_devices(node, options, &devs, &ndevs);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        *total += ndevs / per;
        pmix_hwloc_release_devices(devs, ndevs);
    }
    return PRTE_SUCCESS;
}

