/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A job that counts hwthreads as its cpus is owed a node's hwthreads as
 * its slots - for its own map only.
 *
 * A node whose slot count nobody stated is sized when it joins the DVM by
 * counting its cores (the default prte_set_default_slots policy), and that
 * count is fixed for the life of the DVM.  "--mapby :hwtcpus" (or the
 * deprecated --use-hwthread-cpus) given to prun used to change only how
 * cpus were counted for binding: the job still saw the core count, so it
 * got half the processes by default and, asked for the hwthread count,
 * was mapped as oversubscribed and left unbound.
 *
 * prte_rmaps_base_get_target_nodes() now counts such a node's hwthreads
 * for an app that asks for them, recording the change so that
 * prte_rmaps_base_restore_resized() - run at the end of every map - puts
 * the core count back for the next job.  These checks drive that directly
 * against a real two-way SMT topology (24 cores, 48 hwthreads).
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"

int test_hwt_slots(void);

#define TOPO_FILE "test-topo2.xml"

static int failures = 0;

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

static prte_topology_t *load_topo(const char *file)
{
    hwloc_topology_t topo;
    prte_topology_t *t;
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", PRTE_TEST_TOPO_DIR, file);
    if (0 != hwloc_topology_init(&topo)) {
        return NULL;
    }
    if (0 != hwloc_topology_set_xml(topo, path) || 0 != hwloc_topology_load(topo)) {
        hwloc_topology_destroy(topo);
        return NULL;
    }
    t = PMIX_NEW(prte_topology_t);
    t->topo = topo;
    return t;
}

/* Ask for the app's target nodes and hand back the one node's slot count
 * as the job saw it, plus the total the call reported. */
static int32_t target_slots(prte_job_t *jdata, prte_app_context_t *app,
                            prte_node_t *node, int32_t *total)
{
    pmix_list_t nodes;
    int32_t seen = -1;
    prte_node_t *nd;
    int rc;

    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_rmaps_base_get_target_nodes(&nodes, total, jdata, app,
                                          PRTE_MAPPING_BYCORE, true, true, false);
    if (PRTE_SUCCESS == rc) {
        PMIX_LIST_FOREACH(nd, &nodes, prte_node_t) {
            if (nd == node) {
                seen = nd->slots;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);
    return seen;
}

int test_hwt_slots(void)
{
    prte_topology_t *t;
    prte_node_t *hnp, *node;
    prte_job_t *jdata;
    prte_app_context_t *app, *app2;
    bool made_pool = (NULL == prte_node_pool);
    bool hnp_alloc = prte_hnp_is_allocated;
    int32_t total, ncores, npus;

    failures = 0;
    t = load_topo(TOPO_FILE);
    if (NULL == t) {
        fprintf(stdout, "  SKIP test_hwt_slots (could not load %s)\n", TOPO_FILE);
        return 0;
    }
    ncores = (int32_t) prte_hwloc_base_get_nbobjs_by_type(t->topo, HWLOC_OBJ_CORE);
    npus = (int32_t) prte_hwloc_base_get_nbobjs_by_type(t->topo, HWLOC_OBJ_PU);
    if (npus <= ncores) {
        fprintf(stdout, "  SKIP test_hwt_slots (%s has no SMT)\n", TOPO_FILE);
        PMIX_RELEASE(t);
        return 0;
    }

    if (made_pool) {
        prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_node_pool, 8, INT_MAX, 8);
    }
    /* pool slot 0 is the HNP's node; keep it out of the way */
    hnp = PMIX_NEW(prte_node_t);
    hnp->name = strdup("hwtHNP");
    hnp->index = pmix_pointer_array_add(prte_node_pool, hnp);
    prte_hnp_is_allocated = false;

    node = PMIX_NEW(prte_node_t);
    node->name = strdup("hwtnode");
    PMIX_RETAIN(t);
    node->topology = t;
    node->daemon = PMIX_NEW(prte_proc_t);
    node->daemon->name.rank = 1;
    node->state = PRTE_NODE_STATE_UP;
    node->index = pmix_pointer_array_add(prte_node_pool, node);
    /* sized the way prte_plm_base_set_slots() sizes a node nobody described */
    node->slots = ncores;
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_FROM_CORES);

    jdata = PMIX_NEW(prte_job_t);
    /* the node has no session of its own, so it belongs to the default one */
    jdata->session = prte_default_session;
    app = PMIX_NEW(prte_app_context_t);
    app2 = PMIX_NEW(prte_app_context_t);

    /* a job that asks for nothing sees the cores, and nothing is recorded */
    CHECK("cores: job sees the core count", ncores == target_slots(jdata, app, node, &total));
    CHECK("cores: total matches", ncores == total);
    CHECK("cores: nothing recorded", 0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();

    /* a job that asks for hwthreads sees them - and only until the map ends */
    prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
    CHECK("hwt: job sees the hwthread count", npus == target_slots(jdata, app, node, &total));
    CHECK("hwt: total matches", npus == total);
    CHECK("hwt: the resize was recorded",
          1 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();
    CHECK("hwt: node back to its core count", ncores == node->slots);
    CHECK("hwt: slots-given flag unchanged", PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN));
    CHECK("hwt: from-cores flag unchanged",
          PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_FROM_CORES));

    /* the next job, asking for nothing, gets the cores back */
    prte_remove_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS);
    CHECK("after: next job sees the core count", ncores == target_slots(jdata, app, node, &total));
    prte_rmaps_base_restore_resized();

    /* apps of one job may disagree: an app that counts cores must not map
     * against the hwthread count an earlier app of the same map set, and
     * the node still goes back to its own count at the end */
    prte_set_bool_attribute(&app->attributes, PRTE_APP_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
    CHECK("mixed: hwt app sees hwthreads", npus == target_slots(jdata, app, node, &total));
    CHECK("mixed: core app sees cores", ncores == target_slots(jdata, app2, node, &total));
    CHECK("mixed: hwt app again sees hwthreads", npus == target_slots(jdata, app, node, &total));
    prte_rmaps_base_restore_resized();
    CHECK("mixed: node back to its core count", ncores == node->slots);

    /* an app's own CORECPUS wins over a job-level HWTCPUS */
    prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
    prte_set_bool_attribute(&app2->attributes, PRTE_APP_CORE_CPUS, PRTE_ATTR_GLOBAL, true);
    CHECK("override: app corecpus sees cores", ncores == target_slots(jdata, app2, node, &total));
    prte_rmaps_base_restore_resized();
    prte_remove_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS);

    /* a count somebody stated is what the node offers, whatever the job
     * counts: a hostfile or resource manager never set the from-cores flag */
    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_SLOTS_FROM_CORES);
    CHECK("stated: hwt app still sees the stated count",
          ncores == target_slots(jdata, app, node, &total));
    CHECK("stated: nothing recorded", 0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();

    /* ...and a count re-described after it was derived is no longer the
     * core count, so the flag alone does not license a recount */
    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_FROM_CORES);
    node->slots = 10;
    CHECK("redescribed: hwt app sees the new count", 10 == target_slots(jdata, app, node, &total));
    CHECK("redescribed: nothing recorded",
          0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();

    PMIX_RELEASE(app);
    PMIX_RELEASE(app2);
    jdata->session = NULL;
    PMIX_RELEASE(jdata);
    pmix_pointer_array_set_item(prte_node_pool, node->index, NULL);
    pmix_pointer_array_set_item(prte_node_pool, hnp->index, NULL);
    PMIX_RELEASE(node);
    PMIX_RELEASE(hnp);
    PMIX_RELEASE(t);
    if (made_pool) {
        PMIX_RELEASE(prte_node_pool);
        prte_node_pool = NULL;
    }
    prte_hnp_is_allocated = hnp_alloc;

    if (0 == failures) {
        fprintf(stdout, "  PASS test_hwt_slots\n");
    }
    return failures;
}
