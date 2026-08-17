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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"

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

    if (PRTE_SUCCESS != prte_rmaps_base_devices_begin(node, &opts, &ctx)) {
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
    rc = prte_rmaps_base_devices_begin(node, &opts, &ctx);
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
    rc = prte_rmaps_base_devices_begin(node, &opts, &ctx);
    CHECK("network unaffected", PRTE_SUCCESS == rc);
    CHECK("network finds devices",
          0 < prte_rmaps_base_devices_count(node, &opts, ctx));
    prte_rmaps_base_devices_end(ctx);

    PMIX_RELEASE(node);
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

    if (0 == failures) {
        fprintf(stdout, "  PASS test_devices\n");
    }
    return failures;
}
