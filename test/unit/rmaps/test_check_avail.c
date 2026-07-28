/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests prte_rmaps_base_check_avail() - the base helper every mapper asks
 * "can this node take another proc?", and which also does two things that
 * are easy to miss: it adds the node to the job map exactly once, and it
 * drops a node that has reached its max_slots from the caller's node list.
 *
 * That second job is why this is worth a test.  The node it removes is one
 * it also releases, so a caller that offers the same node again asks it to
 * remove an item that is no longer on the list and to drop a reference it
 * no longer holds - which crashed the HNP for a --map-by <object> job on a
 * hostfile carrying max_slots.  A caller whose list holds something other
 * than the nodes it is walking (the sequential mapper walks hostfile
 * entries) passes NULL instead and must simply be told "no".
 *
 * With bind-to-none there is no cpuset arithmetic, so these paths are
 * reachable without a topology.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"

int test_check_avail(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

static prte_node_t *mknode(const char *name, int slots, int inuse, int maxslots)
{
    prte_node_t *node = PMIX_NEW(prte_node_t);

    node->name = strdup(name);
    node->slots = slots;
    node->slots_available = slots - inuse;
    node->slots_inuse = inuse;
    node->slots_max = maxslots;
    return node;
}

static void opts_init(prte_rmaps_options_t *opts, bool oversubscribe)
{
    memset(opts, 0, sizeof(*opts));
    opts->app_idx = -1;
    opts->bind = PRTE_BIND_TO_NONE;
    opts->oversubscribe = oversubscribe;
    opts->cpus_per_rank = 1;
    opts->nprocs = 1;
}

int test_check_avail(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_app_context_t *app;
    prte_node_t *node;
    prte_rmaps_options_t opts;
    pmix_list_t node_list;
    bool avail;
    int32_t mapped;

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "availtest");
    jdata->map = PMIX_NEW(prte_job_map_t);
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup("hostname");
    app->idx = pmix_pointer_array_add(jdata->apps, app);
    jdata->num_apps++;

    /* === a node with room is available, and joins the job map once === */
    node = mknode("node0", 4, 0, 0);
    PMIX_CONSTRUCT(&node_list, pmix_list_t);
    PMIX_RETAIN(node);
    pmix_list_append(&node_list, &node->super);
    opts_init(&opts, false);
    avail = prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, &opts);
    CHECK("room: available", avail);
    CHECK("room: added to map", 1 == jdata->map->num_nodes);
    CHECK("room: counted for this app", 1 == opts.nnodes);
    /* asking a second time must not add it again */
    avail = prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, &opts);
    CHECK("room: still available", avail);
    CHECK("room: added only once", 1 == jdata->map->num_nodes);
    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
    PMIX_LIST_DESTRUCT(&node_list);
    PMIX_RELEASE(node);

    /* === a full node is refused when oversubscription is not allowed,
     * and accepted when it is === */
    node = mknode("node1", 2, 2, 0);
    PMIX_CONSTRUCT(&node_list, pmix_list_t);
    PMIX_RETAIN(node);
    pmix_list_append(&node_list, &node->super);
    opts_init(&opts, false);
    avail = prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, &opts);
    CHECK("full: refused", !avail);
    CHECK("full: left on the list", 1 == pmix_list_get_size(&node_list));
    opts_init(&opts, true);
    avail = prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, &opts);
    CHECK("full+oversub: accepted", avail);
    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
    PMIX_LIST_DESTRUCT(&node_list);
    PMIX_RELEASE(node);

    /* === max_slots is a hard bound: the node is refused however generous
     * the oversubscribe directive, and it is taken off the caller's list
     * so no later pass reconsiders it === */
    node = mknode("node2", 8, 2, 2);
    PMIX_CONSTRUCT(&node_list, pmix_list_t);
    PMIX_RETAIN(node);
    pmix_list_append(&node_list, &node->super);
    PMIX_RETAIN(node);   /* our own handle, so the removal below cannot free it */
    opts_init(&opts, true);
    avail = prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, &opts);
    CHECK("maxslots: refused", !avail);
    CHECK("maxslots: removed from the list", 0 == pmix_list_get_size(&node_list));
    PMIX_LIST_DESTRUCT(&node_list);
    PMIX_RELEASE(node);

    /* === the same node offered without a list: refused, and nothing is
     * removed or released.  This is the sequential mapper's case, whose
     * list holds hostfile entries rather than the nodes being placed on -
     * handing it to the removal path corrupted that list === */
    mapped = jdata->map->num_nodes;
    node = mknode("node3", 8, 4, 4);
    opts_init(&opts, true);
    avail = prte_rmaps_base_check_avail(jdata, app, node, NULL, NULL, &opts);
    CHECK("maxslots, no list: refused", !avail);
    CHECK("maxslots, no list: node survives", 4 == node->slots_max);
    CHECK("maxslots, no list: not added to map", mapped == jdata->map->num_nodes);
    PMIX_RELEASE(node);

    /* === a node with room and no list is still usable === */
    mapped = jdata->map->num_nodes;
    node = mknode("node4", 4, 0, 0);
    opts_init(&opts, false);
    avail = prte_rmaps_base_check_avail(jdata, app, node, NULL, NULL, &opts);
    CHECK("room, no list: available", avail);
    CHECK("room, no list: added to map", mapped + 1 == jdata->map->num_nodes);
    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
    PMIX_RELEASE(node);

    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_check_avail\n");
    }
    return failures;
}
