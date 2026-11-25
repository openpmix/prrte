/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the device assignment prte_rmaps_base_devices_record() publishes.
 *
 * The question this answers is whose device the process is told it got.  A
 * device uuid names the node the device lives on, and the mapper runs on the
 * HNP against every compute node's topology in turn - so the node whose
 * topology is being read has to be the node named in the uuid, and that is
 * not the node the mapper is running on.
 *
 * The case that brought this file into being: the enumerator built the uuid
 * from pmix_globals.hostname, which in the mapper is the HNP.  Every device
 * on every node in the job therefore came back stamped with the HNP's name.
 * Nothing detects that - the strings are well-formed and unique per device -
 * but the uuid travels instead of an ordinal precisely so the process can
 * compute the same string locally from PMIX_DEVICE_DISTANCES and correlate
 * the two, and that correlation then succeeded on exactly one node.
 *
 * So the test enumerates one topology as two different nodes and checks that
 * the assignment moved with the node.  It needs no DVM: a node carrying an
 * XML topology with GPUs in it, and a hand-built options struct, is enough.
 */

#include "prte_config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "constants.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/pmix_argv.h"

int test_devices(bool pmix_up);

static int failures = 0;

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* The reporter's machine from OMPI #14169: four GPUs spread unevenly across
 * eight NUMA domains, which is why "map by device" exists at all.  This is
 * the reading of it by an hwloc built with the CUDA and NVML backends, so
 * its GPUs carry a vendor identity and can be mapped against. */
#define TOPO_FILE "turin-4gpu-nvml.xml"

/* The same machine as the distro hwloc saw it: the GPUs are there, and
 * nothing names them. */
#define TOPO_FILE_NOID "turin-4gpu.xml"

static prte_node_t *build_node(const char *name, prte_topology_t *t)
{
    prte_node_t *node;

    node = PMIX_NEW(prte_node_t);
    node->name = strdup(name);
    PMIX_RETAIN(t);
    node->topology = t;
    /* the node constructor leaves "available" NULL - it is filled in when a
     * node is inserted with a topology, so a hand-built one allocates it */
    node->available = hwloc_bitmap_alloc();
    hwloc_bitmap_copy(node->available, hwloc_topology_get_allowed_cpuset(t->topo));
    hwloc_bitmap_copy(node->jobcache, node->available);
    return node;
}

/* Place one proc against device group 0 of this node and hand back the uuid
 * it was told about, or NULL.  Caller frees. */
static char *assigned_uuid(prte_node_t *node, char **osname)
{
    prte_rmaps_options_t opts;
    prte_proc_t *proc;
    pmix_data_array_t *darray = NULL;
    pmix_device_t *dev;
    void *ctx = NULL;
    char *uuid = NULL;

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    /* the bind ceiling is checked in begin(); binding to nothing skips it,
     * which keeps this test about identity rather than placement */
    opts.bind = PRTE_BIND_TO_NONE;

    if (PRTE_SUCCESS != prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx)) {
        return NULL;
    }
    if (0 == prte_rmaps_base_devices_count(node, &opts, ctx)) {
        prte_rmaps_base_devices_end(ctx);
        return NULL;
    }

    proc = PMIX_NEW(prte_proc_t);
    prte_rmaps_base_devices_record(proc, &opts, ctx, 0);

    if (prte_get_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID,
                           (void **) &darray, PMIX_DATA_ARRAY)
        && NULL != darray && 0 < darray->size) {
        dev = (pmix_device_t *) darray->array;
        if (NULL != dev[0].uuid) {
            uuid = strdup(dev[0].uuid);
        }
        if (NULL != osname && NULL != dev[0].osname) {
            *osname = strdup(dev[0].osname);
        }
    }
    if (NULL != darray) {
        PMIX_DATA_ARRAY_FREE(darray);
    }
    PMIX_RELEASE(proc);
    prte_rmaps_base_devices_end(ctx);
    return uuid;
}

/* Load one of the shared test topologies, or NULL. */
static prte_topology_t *load_topo(const char *file)
{
    hwloc_topology_t topo;
    prte_topology_t *t;
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", PRTE_TEST_TOPO_DIR, file);
    if (0 != hwloc_topology_init(&topo)) {
        return NULL;
    }
    if (0 != hwloc_topology_set_xml(topo, path)
        || 0 != hwloc_topology_set_io_types_filter(topo, HWLOC_TYPE_FILTER_KEEP_IMPORTANT)
        || 0 != hwloc_topology_load(topo)) {
        hwloc_topology_destroy(topo);
        return NULL;
    }
    t = PMIX_NEW(prte_topology_t);
    t->topo = topo;
    return t;
}

/* A GPU nothing can name is not one we will map against.  Same machine as
 * TOPO_FILE, read by an hwloc without the vendor backends: the devices are
 * all still there and still placeable, so the only thing that can refuse
 * the request is the identity check itself. */
static void check_unnameable_refused(void)
{
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    void *ctx = NULL;
    int rc;

    t = load_topo(TOPO_FILE_NOID);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices unnameable case (no %s)\n", TOPO_FILE_NOID);
        return;
    }
    node = build_node("node-gamma", t);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.bind = PRTE_BIND_TO_NONE;
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("unnameable refused", PRTE_ERR_SILENT == rc);
    CHECK("unnameable yields no context", NULL == ctx);
    prte_rmaps_base_devices_end(ctx);

    /* ...but a class with no vendor-identity concept is unaffected: a
     * fabric device is named by its own GUIDs, which hwloc always has */
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "network";
    opts.bind = PRTE_BIND_TO_NONE;
    ctx = NULL;
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("network unaffected", PRTE_SUCCESS == rc);
    CHECK("network finds devices",
          0 < prte_rmaps_base_devices_count(node, &opts, ctx));
    prte_rmaps_base_devices_end(ctx);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* Write the file the NVIDIA driver publishes for one GPU, under a fake
 * procfs root, recording what was created so it can be removed. */
static bool write_driver_file(const char *root, const char *busid,
                              const char *uuid, char ***made)
{
    const char *parts[] = {"driver", "nvidia", "gpus", busid, NULL};
    char path[1024];
    size_t n, len;
    FILE *fp;

    len = (size_t) snprintf(path, sizeof(path), "%s", root);
    for (n = 0; NULL != parts[n]; n++) {
        len += (size_t) snprintf(path + len, sizeof(path) - len, "/%s", parts[n]);
        if (0 == mkdir(path, 0700)) {
            PMIx_Argv_append_nosize(made, path);
        } else if (EEXIST != errno) {
            return false;
        }
    }
    snprintf(path + len, sizeof(path) - len, "/information");
    fp = fopen(path, "w");
    if (NULL == fp) {
        return false;
    }
    fprintf(fp, "Model: \t\t NVIDIA H100\nGPU UUID: \t %s\nBus Location: \t %s\n",
            uuid, busid);
    fclose(fp);
    PMIx_Argv_append_nosize(made, path);
    return true;
}

static void remove_driver_tree(const char *root, char **made)
{
    int n;

    for (n = PMIx_Argv_count(made) - 1; 0 <= n; n--) {
        if (0 != unlink(made[n])) {
            (void) rmdir(made[n]);
        }
    }
    PMIx_Argv_free(made);
    (void) rmdir(root);
}

/* The fallback for an hwloc without NVML: the identity the NVIDIA driver
 * publishes, recorded on the topology when the daemon discovers it.
 *
 * TOPO_FILE_NOID is refused by check_unnameable_refused() above.  Here the
 * same topology is given what pmix_hwloc_add_driver_identities() records on
 * a node where the driver is loaded - which is what a daemon now sends the
 * HNP - and must map, with each process told the driver's identity.  Named
 * for only some of its GPUs, it must still be refused: one GPU nothing can
 * name is enough for every process on the node to end up sharing it.
 *
 * TOPO_FILE, read with NVML, is the other half: its identities come from
 * NVML, and the driver's must not displace them. */
static void check_driver_identity(void)
{
    prte_rmaps_options_t opts;
    pmix_topology_t ptopo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_hwloc_device_t *devs = NULL;
    pmix_data_array_t *darray = NULL;
    pmix_device_t *dev;
    prte_topology_t *t;
    prte_proc_t *proc;
    prte_node_t *node;
    char root[1024], uuid[64], busids[4][32];
    char **made = NULL;
    const char *tmp;
    size_t ndevs = 0, n;
    void *ctx = NULL;
    int rc;

    tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/prte-nvproc-XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(root)) {
        fprintf(stdout, "  SKIP test_devices driver identity (no scratch dir)\n");
        return;
    }

    t = load_topo(TOPO_FILE_NOID);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices driver identity (no %s)\n", TOPO_FILE_NOID);
        remove_driver_tree(root, made);
        return;
    }
    ptopo.topology = t->topo;
    rc = pmix_hwloc_get_devices(&ptopo, "node-delta", PMIX_DEVTYPE_GPU, NULL, &devs, &ndevs);
    CHECK("driver: four GPUs", PMIX_SUCCESS == rc && 4 == ndevs);
    for (n = 0; n < ndevs && n < 4; n++) {
        snprintf(busids[n], sizeof(busids[n]), "%s", devs[n].busid);
    }
    if (NULL != devs) {
        pmix_hwloc_release_devices(devs, ndevs);
    }
    if (4 != ndevs) {
        PMIX_RELEASE(t);
        remove_driver_tree(root, made);
        return;
    }

    /* the driver knows three of the four: still refused */
    for (n = 0; n < 3; n++) {
        snprintf(uuid, sizeof(uuid), "GPU-5eed0000-0000-0000-0000-00000000000%d", (int) n);
        CHECK("driver: tree written", write_driver_file(root, busids[n], uuid, &made));
    }
    CHECK("driver: three recorded", 3 == pmix_hwloc_add_driver_identities(t->topo, root));
    node = build_node("node-delta", t);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.bind = PRTE_BIND_TO_NONE;
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("driver: one unnamed GPU still refuses the node", PRTE_ERR_SILENT == rc);
    prte_rmaps_base_devices_end(ctx);
    ctx = NULL;

    /* ...and all four: mapped, each process told its GPU's identity */
    snprintf(uuid, sizeof(uuid), "GPU-5eed0000-0000-0000-0000-000000000003");
    CHECK("driver: tree written", write_driver_file(root, busids[3], uuid, &made));
    CHECK("driver: the fourth recorded", 1 == pmix_hwloc_add_driver_identities(t->topo, root));
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("driver: mapped once every GPU is named", PRTE_SUCCESS == rc);
    CHECK("driver: four groups", 4 == prte_rmaps_base_devices_count(node, &opts, ctx));
    for (n = 0; PRTE_SUCCESS == rc && n < 4; n++) {
        proc = PMIX_NEW(prte_proc_t);
        CHECK("driver: recorded",
              PRTE_SUCCESS == prte_rmaps_base_devices_record(proc, &opts, ctx, (unsigned) n));
        darray = NULL;
        if (prte_get_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID,
                               (void **) &darray, PMIX_DATA_ARRAY)
            && NULL != darray && 1 == darray->size) {
            dev = (pmix_device_t *) darray->array;
            /* the process resolves its osname against its own node's
             * topology, where the identity now sits on the same function */
            CHECK("driver: assignment names a device", NULL != dev[0].osname);
        } else {
            CHECK("driver: one device per process", false);
        }
        if (NULL != darray) {
            PMIX_DATA_ARRAY_FREE(darray);
        }
        PMIX_RELEASE(proc);
    }
    prte_rmaps_base_devices_end(ctx);
    PMIX_RELEASE(node);
    PMIX_RELEASE(t);

    /* NVML's identities stand, whatever the driver files say */
    t = load_topo(TOPO_FILE);
    if (NULL != t) {
        CHECK("driver: NVML-named GPUs are left alone",
              0 == pmix_hwloc_add_driver_identities(t->topo, root));
        ptopo.topology = t->topo;
        devs = NULL;
        rc = pmix_hwloc_get_devices(&ptopo, "node-delta", PMIX_DEVTYPE_GPU, NULL,
                                    &devs, &ndevs);
        CHECK("driver: NVML topology still names its GPUs",
              PMIX_SUCCESS == rc && 4 == ndevs && NULL != devs[0].vendor_id
                  && 0 != strncmp(devs[0].vendor_id, "GPU-5eed", 8));
        if (NULL != devs) {
            pmix_hwloc_release_devices(devs, ndevs);
        }
        PMIX_RELEASE(t);
    }

    remove_driver_tree(root, made);
}

/* Every spelling of the network class names the same set.
 *
 * The four are synonyms because one card is routinely two OS devices - an
 * OpenFabrics one and a network one on the same PCI function - and the
 * enumeration dedupes by function, so the union is one entry per card.
 * When the spellings meant different types, "network" and "openfabrics"
 * returned the same hardware under different names and different counts,
 * and whichever the user did not type looked like a wrong answer.
 *
 * The topology is the reporter's machine: six Mellanox functions carrying
 * both views, plus a management NIC that has only the network view - so a
 * naive union would have counted the six cards twice and the answer here
 * (seven) is the one the dedup produces.
 */
static void check_network_synonyms(void)
{
    static const char *spellings[] = {"network", "nic", "fabric",
                                      "openfabrics", NULL};
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    void *ctx = NULL;
    unsigned count[4];
    int n;

    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices network synonyms (no %s)\n", TOPO_FILE);
        return;
    }
    node = build_node("node-delta", t);

    for (n = 0; NULL != spellings[n]; n++) {
        memset(&opts, 0, sizeof(opts));
        opts.app_idx = -1;
        opts.map_device = (char *) spellings[n];
        opts.bind = PRTE_BIND_TO_NONE;
        ctx = NULL;
        CHECK(spellings[n], PRTE_SUCCESS == prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx));
        count[n] = prte_rmaps_base_devices_count(node, &opts, ctx);
        prte_rmaps_base_devices_end(ctx);
    }

    CHECK("network finds the cards", 7 == count[0]);
    CHECK("nic means network", count[1] == count[0]);
    CHECK("fabric means network", count[2] == count[0]);
    CHECK("openfabrics means network", count[3] == count[0]);

    /* and it is a different set from the GPUs on the same node, which is
     * the check that would catch a class collapsing into "everything" */
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.bind = PRTE_BIND_TO_NONE;
    ctx = NULL;
    CHECK("gpu", PRTE_SUCCESS == prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx));
    CHECK("gpu is its own class", 4 == prte_rmaps_base_devices_count(node, &opts, ctx));
    prte_rmaps_base_devices_end(ctx);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* A GPU named by its OS device is still a GPU.
 *
 * The refusal above is about the device, not about how the user spelled
 * the request: naming one GPU ("renderD129") instead of the class hands the
 * process the very same unactionable assignment the refusal exists to
 * prevent.  So the unnameable topology must refuse it by name too - while a
 * NIC named the same way, which needs no vendor identity, still maps. */
static void check_named_unnameable_refused(void)
{
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    void *ctx = NULL;
    int rc;

    t = load_topo(TOPO_FILE_NOID);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices named unnameable case (no %s)\n", TOPO_FILE_NOID);
        return;
    }
    node = build_node("node-epsilon", t);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "renderD129";
    opts.bind = PRTE_BIND_TO_NONE;
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("named unnameable gpu refused", PRTE_ERR_SILENT == rc);
    CHECK("named unnameable yields no context", NULL == ctx);
    prte_rmaps_base_devices_end(ctx);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "mlx5_1";
    opts.bind = PRTE_BIND_TO_NONE;
    ctx = NULL;
    rc = prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx);
    CHECK("named nic unaffected", PRTE_SUCCESS == rc);
    CHECK("named nic found", 1 == prte_rmaps_base_devices_count(node, &opts, ctx));
    prte_rmaps_base_devices_end(ctx);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* The order the devices come out in, as the OS names each group was given:
 * groups joined with ',', the members of one group with '+'.  Caller
 * frees. */
static char *device_order(prte_node_t *node, prte_rmaps_options_t *opts)
{
    pmix_data_array_t *darray;
    pmix_device_t *dev;
    prte_proc_t *proc;
    void *ctx = NULL;
    char *order = NULL, *tmp;
    unsigned n, count;
    size_t m;

    if (PRTE_SUCCESS != prte_rmaps_base_devices_begin(NULL, node, opts, &ctx)) {
        return NULL;
    }
    count = prte_rmaps_base_devices_count(node, opts, ctx);
    for (n = 0; n < count; n++) {
        proc = PMIX_NEW(prte_proc_t);
        darray = NULL;
        if (PRTE_SUCCESS == prte_rmaps_base_devices_record(proc, opts, ctx, n)
            && prte_get_attribute(&proc->attributes, PRTE_PROC_DEVICE_ID,
                                  (void **) &darray, PMIX_DATA_ARRAY)
            && NULL != darray && 0 < darray->size) {
            dev = (pmix_device_t *) darray->array;
            for (m = 0; m < darray->size; m++) {
                pmix_asprintf(&tmp, "%s%s%s", (NULL == order) ? "" : order,
                              (NULL == order) ? "" : ((0 == m) ? "," : "+"),
                              dev[m].osname);
                free(order);
                order = tmp;
            }
        }
        if (NULL != darray) {
            PMIX_DATA_ARRAY_FREE(darray);
        }
        PMIX_RELEASE(proc);
    }
    prte_rmaps_base_devices_end(ctx);
    return order;
}

/* Interleaving by NUMA domain has to find the NUMA domain.
 *
 * In hwloc 2 a NUMA node is a memory child, never an ancestor of anything,
 * so a device's NUMA domain cannot be found by walking up from its
 * locality - and "interleave=numa" that looked for it that way found no
 * domain for any device, formed one group, and silently reproduced the
 * input order.
 *
 * On this machine two of the seven network cards (mlx5_1 and mlx5_2) share
 * NUMA domain 3 and sit next to each other in bus order; every other card
 * has a domain to itself.  Taking one card per domain per round moves the
 * second of the pair to the end, and that is the only change - which is
 * also what makes the expected order unambiguous. */
static void check_interleave_numa(void)
{
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    char *plain, *numa;

    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices interleave (no %s)\n", TOPO_FILE);
        return;
    }
    node = build_node("node-zeta", t);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "network";
    opts.bind = PRTE_BIND_TO_NONE;
    plain = device_order(node, &opts);
    opts.map_interleave = "numa";
    numa = device_order(node, &opts);

    CHECK("plain order", NULL != plain
          && 0 == strcmp(plain, "mlx5_0,mlx5_3,eno6,mlx5_1,mlx5_2,mlx5_4,mlx5_5"));
    CHECK("numa interleave separates the shared domain", NULL != numa
          && 0 == strcmp(numa, "mlx5_0,mlx5_3,eno6,mlx5_1,mlx5_4,mlx5_5,mlx5_2"));
    if (NULL != numa && NULL != plain && 0 != strcmp(numa, "mlx5_0,mlx5_3,eno6,mlx5_1,mlx5_4,mlx5_5,mlx5_2")) {
        fprintf(stderr, "  plain: %s\n  numa:  %s\n", plain, numa);
    }
    free(plain);
    free(numa);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* An ndev group stays under one PCI root complex where the counts allow.
 *
 * Seven network cards in bus order, each under a host bridge of its own
 * except mlx5_1 and mlx5_2 - the two ports of one card - which share one.
 * Cut into pairs from the top of that list, every pair spanned two
 * complexes and the one pair the machine has was split between two
 * processes.  The pair now comes first, the cards that have to share a
 * group with a card from another complex follow in bus order, and mlx5_5
 * is left over exactly as before: seven cards still make three pairs.
 *
 * The GPUs are one per complex, so nothing can be kept together and the
 * order must be the one it always was.  And interleave asks for groups
 * that span, so it must not be overridden. */
static void check_ndev_root_complex(void)
{
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    char *order;

    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices ndev root complex (no %s)\n", TOPO_FILE);
        return;
    }
    node = build_node("node-eta", t);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "network";
    opts.map_ndev = 2;
    opts.bind = PRTE_BIND_TO_NONE;
    order = device_order(node, &opts);
    CHECK("nic pairs keep the shared complex together", NULL != order
          && 0 == strcmp(order, "mlx5_1+mlx5_2,mlx5_0+mlx5_3,eno6+mlx5_4"));
    if (NULL != order && 0 != strcmp(order, "mlx5_1+mlx5_2,mlx5_0+mlx5_3,eno6+mlx5_4")) {
        fprintf(stderr, "  network ndev=2: %s\n", order);
    }
    free(order);

    /* one device per proc is not a group, and the order is untouched */
    opts.map_ndev = 1;
    order = device_order(node, &opts);
    CHECK("ndev=1 keeps bus order", NULL != order
          && 0 == strcmp(order, "mlx5_0,mlx5_3,eno6,mlx5_1,mlx5_2,mlx5_4,mlx5_5"));
    free(order);

    /* interleave asks for groups that span the level */
    opts.map_ndev = 2;
    opts.map_interleave = "numa";
    order = device_order(node, &opts);
    CHECK("interleave is not overridden", NULL != order
          && 0 == strcmp(order, "mlx5_0+mlx5_3,eno6+mlx5_1,mlx5_4+mlx5_5"));
    if (NULL != order && 0 != strcmp(order, "mlx5_0+mlx5_3,eno6+mlx5_1,mlx5_4+mlx5_5")) {
        fprintf(stderr, "  network ndev=2 interleave=numa: %s\n", order);
    }
    free(order);

    /* the GPUs sit one per complex: nothing to keep together */
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_ndev = 2;
    opts.bind = PRTE_BIND_TO_NONE;
    order = device_order(node, &opts);
    CHECK("gpu pairs keep bus order", NULL != order
          && 0 == strcmp(order, "cuda0+cuda1,cuda2+cuda3"));
    if (NULL != order && 0 != strcmp(order, "cuda0+cuda1,cuda2+cuda3")) {
        fprintf(stderr, "  gpu ndev=2: %s\n", order);
    }
    free(order);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* Load a shared test topology with some of its text replaced, or NULL.
 * Each "from" must occur exactly once - an edit that could land in two
 * places, or none, is a test that no longer says what it was written to. */
static prte_topology_t *load_topo_edited(const char *file, const char **from,
                                         const char **to)
{
    hwloc_topology_t topo;
    prte_topology_t *t;
    char path[1024], *buf, *hit, *nbuf;
    size_t flen, tlen, blen;
    FILE *fp;
    long len;
    int n;

    snprintf(path, sizeof(path), "%s/%s", PRTE_TEST_TOPO_DIR, file);
    fp = fopen(path, "r");
    if (NULL == fp) {
        return NULL;
    }
    if (0 != fseek(fp, 0, SEEK_END) || 0 > (len = ftell(fp)) || 0 != fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return NULL;
    }
    buf = (char *) malloc(len + 1);
    if (NULL == buf || (size_t) len != fread(buf, 1, len, fp)) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[len] = '\0';
    for (n = 0; NULL != from[n]; n++) {
        hit = strstr(buf, from[n]);
        if (NULL == hit || NULL != strstr(hit + 1, from[n])) {
            free(buf);
            return NULL;
        }
        flen = strlen(from[n]);
        tlen = strlen(to[n]);
        blen = strlen(buf);
        nbuf = (char *) malloc(blen - flen + tlen + 1);
        if (NULL == nbuf) {
            free(buf);
            return NULL;
        }
        memcpy(nbuf, buf, hit - buf);
        memcpy(nbuf + (hit - buf), to[n], tlen);
        memcpy(nbuf + (hit - buf) + tlen, hit + flen, blen - (hit - buf) - flen + 1);
        free(buf);
        buf = nbuf;
    }
    if (0 != hwloc_topology_init(&topo)) {
        free(buf);
        return NULL;
    }
    if (0 != hwloc_topology_set_xmlbuffer(topo, buf, (int) strlen(buf) + 1)
        || 0 != hwloc_topology_set_io_types_filter(topo, HWLOC_TYPE_FILTER_KEEP_IMPORTANT)
        || 0 != hwloc_topology_load(topo)) {
        hwloc_topology_destroy(topo);
        free(buf);
        return NULL;
    }
    free(buf);
    t = PMIX_NEW(prte_topology_t);
    t->topo = topo;
    return t;
}

/* A management port beside a GPU is not the GPU's NIC.
 *
 * The Slingshot node: the A100 at c1:00.0 shares its host bridge with a
 * Slingshot NIC (hsn0, a 31.5 GB/s link) and a 1 GbE management port
 * (nmn0, 0.25 GB/s).  Neither carries an OpenFabrics device and each sits
 * one root port below the host bridge, so the OpenFabrics and depth rules
 * cannot tell them apart.  The topology's own bus order happens to put
 * hsn0 first; here the management port is moved ahead of it, which is what
 * a different slot assignment would do, and the GPU must still get hsn0.
 *
 * That hwloc recorded no GPU backend, so each A100 is given the cuda and
 * nvml OS devices an hwloc with one would have.  All four need them: PMIx
 * counts a 3D controller with no OS device as a GPU it cannot name, and a
 * node holding one is refused for mapping by GPU at all. */
#define A100_HEAD(bus)                                                                     \
    "pci_busid=\"0000:" bus "\" pci_type=\"0302 [10de:20b0] [10de:144e] a1\""              \
    " pci_link_speed=\"31.507692\">\n"
#define A100_INFO                                                                          \
    "              <info name=\"PCIVendor\" value=\"NVIDIA Corporation\"/>\n"              \
    "              <info name=\"PCIDevice\" value=\"GA100 [A100 SXM4 40GB]\"/>\n"
#define A100_SLOT "              <info name=\"PCISlot\" value=\"6\"/>\n"
#define A100_OSDEVS(n)                                                                     \
    "              <object type=\"OSDev\" gp_index=\"900" n "1\" name=\"cuda" n "\""     \
    " subtype=\"CUDA\" osdev_type=\"5\">\n"                                                \
    "                <info name=\"Backend\" value=\"CUDA\"/>\n"                            \
    "              </object>\n"                                                            \
    "              <object type=\"OSDev\" gp_index=\"900" n "2\" name=\"nvml" n "\""     \
    " subtype=\"NVML\" osdev_type=\"1\">\n"                                                \
    "                <info name=\"Backend\" value=\"NVML\"/>\n"                            \
    "                <info name=\"NVIDIAUUID\""                                            \
    " value=\"GPU-00000000-0000-0000-0000-00000000000" n "\"/>\n"                          \
    "              </object>\n"
static void check_nic_link_speed(void)
{
    static const char *from[] = {
        A100_HEAD("03:00.0") A100_SLOT A100_INFO,
        A100_HEAD("41:00.0") A100_INFO,
        A100_HEAD("82:00.0") A100_INFO,
        A100_HEAD("c1:00.0") A100_INFO,
        "pci_busid=\"0000:c3:00.0\"",
        NULL};
    static const char *to[] = {
        A100_HEAD("03:00.0") A100_SLOT A100_INFO A100_OSDEVS("0"),
        A100_HEAD("41:00.0") A100_INFO A100_OSDEVS("1"),
        A100_HEAD("82:00.0") A100_INFO A100_OSDEVS("2"),
        A100_HEAD("c1:00.0") A100_INFO A100_OSDEVS("3"),
        "pci_busid=\"0000:c1:00.1\"",
        NULL};
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    char *order;

    t = load_topo_edited("slingshot.xml", from, to);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices nic link speed (could not load slingshot.xml)\n");
        failures++;
        return;
    }
    node = build_node("node-lambda", t);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    /* each of the other three complexes holds one GPU and one Slingshot
     * NIC; the A100 at c1:00.0 is cuda3 */
    order = device_order(node, &opts);
    CHECK("the gpu gets the fabric nic, not the management port", NULL != order
          && 0 == strcmp(order, "cuda0+hsn3,cuda1+hsn2,cuda2+hsn1,cuda3+hsn0"));
    if (NULL != order && 0 != strcmp(order, "cuda0+hsn3,cuda1+hsn2,cuda2+hsn1,cuda3+hsn0")) {
        fprintf(stderr, "  slingshot gpu nic: %s\n", order);
    }
    free(order);

    /* asked for two, the other three complexes have one NIC each, so the
     * map is refused rather than giving those GPUs one */
    opts.map_nic = 2;
    fprintf(stderr, "--- expected error output follows (nic=2 on one nic per gpu) ---\n");
    order = device_order(node, &opts);
    CHECK("nic=2 with one nic on most complexes is refused", NULL == order);
    free(order);
    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* The same rule on a node from another pair of vendors.
 *
 * mi350-bcm57608.xml is a real node: eight AMD MI350s, which report PCI
 * class 0x1200 (a processing accelerator) rather than a display class, and
 * eight dual-port Broadcom BCM57608 RoCE adapters, one GPU and one adapter
 * under every host bridge.  The GPU and the adapter hang off one PCIe
 * switch, and the adapter's two ports are at the same depth with the same
 * link, so nothing but bus order chooses between them: port 0 for nic,
 * both ports for nic=2.  The RDMA
 * devices carry rdma-core's PCI-path names (rocep6s0f0), which the device
 * map uses as it finds them. */
static void check_mi350_gpu_nic(void)
{
    static const char *one =
        "rsmi0+rocep6s0f0,rsmi1+rocep22s0f0,rsmi2+rocep102s0f0,rsmi3+rocep118s0f0,"
        "rsmi4+rocep134s0f0,rsmi5+rocep150s0f0,rsmi6+rocep230s0f0,rsmi7+rocep246s0f0";
    static const char *two =
        "rsmi0+rocep6s0f0+rocep6s0f1,rsmi1+rocep22s0f0+rocep22s0f1,"
        "rsmi2+rocep102s0f0+rocep102s0f1,rsmi3+rocep118s0f0+rocep118s0f1,"
        "rsmi4+rocep134s0f0+rocep134s0f1,rsmi5+rocep150s0f0+rocep150s0f1,"
        "rsmi6+rocep230s0f0+rocep230s0f1,rsmi7+rocep246s0f0+rocep246s0f1";
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    char *order;

    t = load_topo("mi350-bcm57608.xml");
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices mi350 gpu nic (could not load mi350-bcm57608.xml)\n");
        failures++;
        return;
    }
    node = build_node("node-mu", t);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    order = device_order(node, &opts);
    CHECK("mi350: each accelerator gets port 0 of the adapter on its complex",
          NULL != order && 0 == strcmp(order, one));
    if (NULL != order && 0 != strcmp(order, one)) {
        fprintf(stderr, "  mi350 gpu nic: %s\n", order);
    }
    free(order);

    opts.map_nic = 2;
    order = device_order(node, &opts);
    CHECK("mi350: nic=2 gets both ports of that adapter",
          NULL != order && 0 == strcmp(order, two));
    if (NULL != order && 0 != strcmp(order, two)) {
        fprintf(stderr, "  mi350 gpu nic=2: %s\n", order);
    }
    free(order);
    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* An AWS node: only the RDMA NICs on a GPU's complex count.
 *
 * aws-p6-b200.xml is a real p6-b200.48xlarge.  Each pair of B200s shares a
 * PCI root complex with two EFA devices (rdmapNs0, OpenFabrics) and two ENA
 * ports (enpNs0, plain Ethernet); one pair's complex also holds the two
 * ConnectX-7 InfiniBand ports.  The ENA ports are named to no communication
 * library, so they must never make up a NIC count: nic=2 gives each GPU the
 * two EFA devices on its complex, and nic=4 - which used to come back as
 * two EFA devices and two ENA ports - is refused.
 *
 * That hwloc had no GPU backend, so the B200s carry no OS device; each is
 * given the nvml device an hwloc with one would have, or the map would be
 * refused for want of a GPU identity before the NICs were looked at. */
#define B200_SLOT(slot, n)                                                                 \
    "<info name=\"PCISlot\" value=\"" slot "\"/>\n"                                        \
    "                <object type=\"OSDev\" gp_index=\"9100" n "\" name=\"nvml" n "\""     \
    " subtype=\"NVML\" osdev_type=\"1\">\n"                                                \
    "                  <info name=\"Backend\" value=\"NVML\"/>\n"                          \
    "                  <info name=\"NVIDIAUUID\""                                          \
    " value=\"GPU-00000000-0000-0000-0000-00000000000" n "\"/>\n"                          \
    "                </object>"
static void check_aws_efa_nic(void)
{
    static const char *from[] = {
        "<info name=\"PCISlot\" value=\"79-1\"/>",  "<info name=\"PCISlot\" value=\"80-1\"/>",
        "<info name=\"PCISlot\" value=\"93-1\"/>",  "<info name=\"PCISlot\" value=\"94-1\"/>",
        "<info name=\"PCISlot\" value=\"109-1\"/>", "<info name=\"PCISlot\" value=\"110-1\"/>",
        "<info name=\"PCISlot\" value=\"123-1\"/>", "<info name=\"PCISlot\" value=\"124-1\"/>",
        NULL};
    static const char *to[] = {
        B200_SLOT("79-1", "0"),  B200_SLOT("80-1", "1"),  B200_SLOT("93-1", "2"),
        B200_SLOT("94-1", "3"),  B200_SLOT("109-1", "4"), B200_SLOT("110-1", "5"),
        B200_SLOT("123-1", "6"), B200_SLOT("124-1", "7"), NULL};
    static const char *one =
        "nvml0+rdmap79s0,nvml1+rdmap80s0,nvml2+rdmap96s0,nvml3+rdmap97s0,"
        "nvml4+rdmap113s0,nvml5+rdmap114s0,nvml6+rdmap132s0,nvml7+rdmap133s0";
    static const char *two =
        "nvml0+rdmap79s0+rdmap80s0,nvml1+rdmap79s0+rdmap80s0,"
        "nvml2+rdmap96s0+rdmap97s0,nvml3+rdmap96s0+rdmap97s0,"
        "nvml4+rdmap113s0+rdmap114s0,nvml5+rdmap113s0+rdmap114s0,"
        "nvml6+rdmap132s0+rdmap133s0,nvml7+rdmap132s0+rdmap133s0";
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    void *ctx = NULL;
    char *order;

    t = load_topo_edited("aws-p6-b200.xml", from, to);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices aws efa nic (could not load aws-p6-b200.xml)\n");
        failures++;
        return;
    }
    node = build_node("node-nu", t);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    order = device_order(node, &opts);
    CHECK("aws: each gpu gets an efa device of its own", NULL != order && 0 == strcmp(order, one));
    if (NULL != order && 0 != strcmp(order, one)) {
        fprintf(stderr, "  aws gpu nic: %s\n", order);
    }
    free(order);

    opts.map_nic = 2;
    order = device_order(node, &opts);
    CHECK("aws: nic=2 gets the complex's two efa devices", NULL != order && 0 == strcmp(order, two));
    if (NULL != order && 0 != strcmp(order, two)) {
        fprintf(stderr, "  aws gpu nic=2: %s\n", order);
    }
    free(order);

    /* four is more than three of the four complexes have, counting only
     * EFA and InfiniBand - the ENA ports beside them do not make it up */
    opts.map_nic = 4;
    fprintf(stderr, "--- expected error output follows (nic=4 on two efa per complex) ---\n");
    CHECK("aws: nic=4 is refused rather than padded with ena ports",
          PRTE_SUCCESS != prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx));
    CHECK("aws: nic=4 leaves no context", NULL == ctx);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* "nic" gives each GPU the NICs under its own root complex.
 *
 * On this machine every GPU shares a host bridge with exactly one HCA, so
 * the answer is a pairing nobody has to guess at: cuda0 with mlx5_0, cuda1
 * with mlx5_3, cuda2 with mlx5_4, cuda3 with mlx5_5.  Asking for more NICs
 * than a complex has refuses the map rather than taking one from elsewhere
 * or quietly giving fewer, and a GPU whose complex has no NIC keeps its GPU
 * and gets no NIC. */
static void check_gpu_nic(void)
{
    static const char *from[] = {"name=\"ib0\" osdev_type=\"2\"",
                                 "name=\"mlx5_5\" osdev_type=\"3\"", NULL};
    static const char *to[] = {"name=\"ib0\" osdev_type=\"0\"",
                               "name=\"mlx5_5\" osdev_type=\"0\"", NULL};
    prte_rmaps_options_t opts;
    prte_topology_t *t;
    prte_node_t *node;
    void *ctx = NULL;
    char *order;

    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices gpu nic (no %s)\n", TOPO_FILE);
        return;
    }
    node = build_node("node-iota", t);

    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    order = device_order(node, &opts);
    CHECK("each gpu gets the nic on its complex", NULL != order
          && 0 == strcmp(order, "cuda0+mlx5_0,cuda1+mlx5_3,cuda2+mlx5_4,cuda3+mlx5_5"));
    if (NULL != order && 0 != strcmp(order, "cuda0+mlx5_0,cuda1+mlx5_3,cuda2+mlx5_4,cuda3+mlx5_5")) {
        fprintf(stderr, "  gpu nic: %s\n", order);
    }
    free(order);

    /* more than the complex has: refused, and nothing is left behind */
    opts.map_nic = 3;
    fprintf(stderr, "--- expected error output follows (nic=3 on one hca per gpu) ---\n");
    CHECK("nic=3: more than the complex has is refused",
          PRTE_SUCCESS != prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx));
    CHECK("nic=3: no context", NULL == ctx);

    /* two GPUs per proc: the NICs of both complexes */
    opts.map_ndev = 2;
    opts.map_nic = 2;
    order = device_order(node, &opts);
    CHECK("ndev=2 nic=2 takes both complexes' nics", NULL != order
          && 0 == strcmp(order, "cuda0+cuda1+mlx5_0+mlx5_3,cuda2+cuda3+mlx5_4+mlx5_5"));
    if (NULL != order && 0 != strcmp(order, "cuda0+cuda1+mlx5_0+mlx5_3,cuda2+cuda3+mlx5_4+mlx5_5")) {
        fprintf(stderr, "  gpu ndev=2 nic=2: %s\n", order);
    }
    free(order);

    /* not GPUs: refused, and nothing is left behind */
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "network";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    fprintf(stderr, "--- expected error output follows (nic on a nic map) ---\n");
    CHECK("nic on a nic map: refused",
          PRTE_SUCCESS != prte_rmaps_base_devices_begin(NULL, node, &opts, &ctx));
    CHECK("nic on a nic map: no context", NULL == ctx);

    PMIX_RELEASE(node);
    PMIX_RELEASE(t);

    /* the same machine with the HCA under cuda3's host bridge turned into
     * something that is not a NIC: cuda3 keeps its GPU and gets no NIC */
    t = load_topo_edited(TOPO_FILE, from, to);
    CHECK("edited topology loads", NULL != t);
    if (NULL == t) {
        return;
    }
    node = build_node("node-kappa", t);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    opts.map_device = "gpu";
    opts.map_nic = 1;
    opts.bind = PRTE_BIND_TO_NONE;
    fprintf(stderr, "--- expected error output follows (no nic near cuda3) ---\n");
    order = device_order(node, &opts);
    CHECK("no nic on the complex: gpu only", NULL != order
          && 0 == strcmp(order, "cuda0+mlx5_0,cuda1+mlx5_3,cuda2+mlx5_4,cuda3"));
    if (NULL != order && 0 != strcmp(order, "cuda0+mlx5_0,cuda1+mlx5_3,cuda2+mlx5_4,cuda3")) {
        fprintf(stderr, "  gpu nic, cuda3 bare: %s\n", order);
    }
    free(order);
    PMIX_RELEASE(node);
    PMIX_RELEASE(t);
}

/* Two devices with the same locality can be under different root complexes.
 *
 * That is why the grouping keys on the host bridge and not on the locality
 * every other part of the device mapper uses: on this machine both network
 * cards report the same cpuset and hang off different host bridges, so
 * nothing on the CPU side of the topology could tell a group holding both
 * from a group under one complex. */
static void check_host_bridge_not_locality(void)
{
    pmix_hwloc_device_t *devs = NULL, *pci[2] = {NULL, NULL};
    pmix_topology_t topo;
    prte_topology_t *t;
    size_t ndevs = 0, n, npci = 0;

    t = load_topo("test-topo.xml");
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices host bridge (no test-topo.xml)\n");
        return;
    }
    topo.source = "hwloc";
    topo.topology = t->topo;
    CHECK("enumerate", PMIX_SUCCESS == pmix_hwloc_get_devices(&topo, "node-theta",
                                                              PMIX_DEVTYPE_NETWORK
                                                                  | PMIX_DEVTYPE_OPENFABRICS,
                                                              NULL, &devs, &ndevs));
    /* the two PCI cards; eth0 has no PCI ancestor and no host bridge */
    for (n = 0; n < ndevs && npci < 2; n++) {
        if (NULL != devs[n].busid) {
            pci[npci++] = &devs[n];
        }
    }
    CHECK("two cards", 2 == npci);
    if (2 == npci) {
        CHECK("same locality", NULL != pci[0]->locality && NULL != pci[1]->locality
              && hwloc_bitmap_isequal(pci[0]->locality->cpuset, pci[1]->locality->cpuset));
        CHECK("each has a host bridge", NULL != prte_hwloc_base_host_bridge(pci[0]->obj)
              && NULL != prte_hwloc_base_host_bridge(pci[1]->obj));
        CHECK("different host bridges", prte_hwloc_base_host_bridge(pci[0]->obj)
              != prte_hwloc_base_host_bridge(pci[1]->obj));
    }
    for (n = 0; n < ndevs; n++) {
        if (NULL == devs[n].busid) {
            CHECK("no PCI, no host bridge", NULL == prte_hwloc_base_host_bridge(devs[n].obj));
        }
    }
    if (NULL != devs) {
        pmix_hwloc_release_devices(devs, ndevs);
    }
    PMIX_RELEASE(t);
}

int test_devices(bool pmix_up)
{
    prte_topology_t *t;
    prte_node_t *alpha, *beta;
    char *ua = NULL, *ub = NULL, *osname = NULL, *expect = NULL;

    failures = 0;

    /* The assignment is published as a PMIX_DATA_ARRAY attribute, and
     * prte_attr_load() copies it with PMIx_Data_copy, which will not run
     * until PMIx itself is up.  main() brings the server up and takes it
     * down again, because the finalize has to come after the frameworks
     * close: in an --enable-mca-dso build it dlcloses the very components
     * the framework teardown is about to call into. */
    if (!pmix_up) {
        fprintf(stdout, "  SKIP test_devices (no PMIx server)\n");
        return 0;
    }

    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_devices (could not load %s)\n", TOPO_FILE);
        return 0;
    }

    /* One topology, two nodes.  In a real DVM these are two daemons that
     * reported identical hardware, which is the case the HNP collapses onto
     * a single recorded topology. */
    alpha = build_node("node-alpha", t);
    beta = build_node("node-beta", t);

    ua = assigned_uuid(alpha, &osname);
    ub = assigned_uuid(beta, NULL);

    CHECK("assigned", NULL != ua && NULL != ub);
    CHECK("osname", NULL != osname);

    if (NULL != ua && NULL != ub && NULL != osname) {
        /* the uuid names the node whose topology was read... */
        pmix_asprintf(&expect, "gpu://node-alpha::%s", osname);
        CHECK("names the node", 0 == strcmp(ua, expect));
        free(expect);
        expect = NULL;
        pmix_asprintf(&expect, "gpu://node-beta::%s", osname);
        CHECK("names the other node", 0 == strcmp(ub, expect));
        free(expect);
        /* ...so two nodes never claim the same device, however identical
         * their hardware */
        CHECK("distinct across nodes", 0 != strcmp(ua, ub));
    }

    free(ua);
    free(ub);
    free(osname);
    PMIX_RELEASE(alpha);
    PMIX_RELEASE(beta);
    PMIX_RELEASE(t);

    check_unnameable_refused();
    check_named_unnameable_refused();
    check_driver_identity();
    check_network_synonyms();
    check_interleave_numa();
    check_ndev_root_complex();
    check_host_bridge_not_locality();
    check_gpu_nic();
    check_nic_link_speed();
    check_mi350_gpu_nic();
    check_aws_efa_nic();

    if (0 == failures) {
        fprintf(stdout, "  PASS test_devices\n");
    }
    return failures;
}
