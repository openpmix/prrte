/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests prte_rmaps_base_compute_vpids() - the step that turns a placement
 * into ranks.
 *
 * Ranking runs after the mappers and only reads the job map, so it can be
 * driven from a hand-built map: a couple of synthetic nodes carrying
 * synthetic procs.  That is what makes the traversal orders (by-slot,
 * by-node, by-fill, by-span), the per-app rank cursor, and the by-user
 * pass-through checkable without a DVM.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"

int test_ranking(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* A job with napps apps and nnodes nodes, each node carrying nper procs of
 * every app.  Procs come in unranked (PMIX_RANK_INVALID), which is the state
 * a mapper leaves them in, and with no locale - the object-based schemes are
 * given one explicitly by the caller that wants it. */
static prte_job_t *build_map(int nnodes, int napps, int nper)
{
    prte_job_t *jdata;
    prte_app_context_t *app;
    prte_node_t *node;
    prte_proc_t *proc;
    int n, a, p;

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "rankingtest");
    jdata->map = PMIX_NEW(prte_job_map_t);

    for (a = 0; a < napps; a++) {
        app = PMIX_NEW(prte_app_context_t);
        app->app = strdup("hostname");
        app->idx = pmix_pointer_array_add(jdata->apps, app);
        app->num_procs = nnodes * nper;
        jdata->num_apps++;
    }

    for (n = 0; n < nnodes; n++) {
        node = PMIX_NEW(prte_node_t);
        pmix_asprintf(&node->name, "node%d", n);
        node->index = n;
        node->slots = napps * nper;
        pmix_pointer_array_add(jdata->map->nodes, node);
        jdata->map->num_nodes++;
        /* app-major within a node, so "procs of one app" is never simply
         * "the first k entries" - the app filter has to do real work */
        for (a = 0; a < napps; a++) {
            for (p = 0; p < nper; p++) {
                proc = PMIX_NEW(prte_proc_t);
                PMIX_LOAD_NSPACE(proc->name.nspace, jdata->nspace);
                proc->name.rank = PMIX_RANK_INVALID;
                proc->app_idx = a;
                proc->node = node;
                pmix_pointer_array_add(node->procs, proc);
                node->num_procs++;
            }
        }
    }
    return jdata;
}

/* the rank given to the p'th proc of app a on node n, or PMIX_RANK_INVALID */
static pmix_rank_t rank_of(prte_job_t *jdata, int n, int a, int p, int nper)
{
    prte_node_t *node;
    prte_proc_t *proc;

    node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, n);
    if (NULL == node) {
        return PMIX_RANK_INVALID;
    }
    proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, (a * nper) + p);
    if (NULL == proc) {
        return PMIX_RANK_INVALID;
    }
    return proc->name.rank;
}

static pmix_rank_t local_of(prte_job_t *jdata, int n, int idx)
{
    prte_node_t *node;
    prte_proc_t *proc;

    node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, n);
    proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, idx);
    return (NULL == proc) ? PMIX_RANK_INVALID : proc->local_rank;
}

static void opts_init(prte_rmaps_options_t *opts, prte_ranking_policy_t rank)
{
    memset(opts, 0, sizeof(*opts));
    opts->app_idx = -1;
    opts->rank = rank;
    opts->maptype = HWLOC_OBJ_MACHINE;
}

int test_ranking(void)
{
    int failures = 0;
    int rc;
    prte_job_t *jdata;
    prte_rmaps_options_t opts;
    uint32_t next;

    /* === by-slot: fill a node's ranks before moving to the next === */
    jdata = build_map(2, 1, 2);
    opts_init(&opts, PRTE_RANK_BY_SLOT);
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, -1, NULL);
    CHECK("byslot: rc", PRTE_SUCCESS == rc);
    CHECK("byslot: n0p0", 0 == rank_of(jdata, 0, 0, 0, 2));
    CHECK("byslot: n0p1", 1 == rank_of(jdata, 0, 0, 1, 2));
    CHECK("byslot: n1p0", 2 == rank_of(jdata, 1, 0, 0, 2));
    CHECK("byslot: n1p1", 3 == rank_of(jdata, 1, 0, 1, 2));
    /* local ranks are back-filled from the node's own proc list */
    CHECK("byslot: local n0", 0 == local_of(jdata, 0, 0) && 1 == local_of(jdata, 0, 1));
    CHECK("byslot: local n1", 0 == local_of(jdata, 1, 0) && 1 == local_of(jdata, 1, 1));
    PMIX_RELEASE(jdata);

    /* === by-node: one rank per node, cycling === */
    jdata = build_map(2, 1, 2);
    opts_init(&opts, PRTE_RANK_BY_NODE);
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, -1, NULL);
    CHECK("bynode: rc", PRTE_SUCCESS == rc);
    CHECK("bynode: n0p0", 0 == rank_of(jdata, 0, 0, 0, 2));
    CHECK("bynode: n1p0", 1 == rank_of(jdata, 1, 0, 0, 2));
    CHECK("bynode: n0p1", 2 == rank_of(jdata, 0, 0, 1, 2));
    CHECK("bynode: n1p1", 3 == rank_of(jdata, 1, 0, 1, 2));
    PMIX_RELEASE(jdata);

    /* === per-app dispatch: each app is ranked on its own, and the cursor
     * carries the next free rank across the call boundary.  Without that,
     * the second app would start again at zero and collide === */
    jdata = build_map(2, 2, 1);
    opts_init(&opts, PRTE_RANK_BY_SLOT);
    opts.app_idx = 0;
    next = 0;
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, 0, &next);
    CHECK("perapp: app0 rc", PRTE_SUCCESS == rc);
    CHECK("perapp: app0 ranks", 0 == rank_of(jdata, 0, 0, 0, 1)
                             && 1 == rank_of(jdata, 1, 0, 0, 1));
    CHECK("perapp: cursor advanced", 2 == next);
    /* app 1's procs are still untouched */
    CHECK("perapp: app1 unranked", PMIX_RANK_INVALID == rank_of(jdata, 0, 1, 0, 1));
    opts.app_idx = 1;
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, 1, &next);
    CHECK("perapp: app1 rc", PRTE_SUCCESS == rc);
    CHECK("perapp: app1 ranks", 2 == rank_of(jdata, 0, 1, 0, 1)
                             && 3 == rank_of(jdata, 1, 1, 0, 1));
    CHECK("perapp: cursor final", 4 == next);
    /* app 0's ranks were not rewritten by the second pass */
    CHECK("perapp: app0 intact", 0 == rank_of(jdata, 0, 0, 0, 1));
    PMIX_RELEASE(jdata);

    /* === by-user: the mapper already set the ranks, so ranking only
     * back-fills the local ranks and leaves the vpids alone === */
    jdata = build_map(1, 1, 2);
    {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, 0);
        prte_proc_t *p0 = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, 0);
        prte_proc_t *p1 = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, 1);
        p0->name.rank = 7;
        p1->name.rank = 3;
    }
    opts_init(&opts, PRTE_RANKING_BYUSER);
    opts.userranked = true;
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, -1, NULL);
    CHECK("byuser: rc", PRTE_SUCCESS == rc);
    CHECK("byuser: ranks kept", 7 == rank_of(jdata, 0, 0, 0, 2)
                             && 3 == rank_of(jdata, 0, 0, 1, 2));
    CHECK("byuser: locals filled", 0 == local_of(jdata, 0, 0) && 1 == local_of(jdata, 0, 1));
    PMIX_RELEASE(jdata);

    /* === an unrecognized ranking policy is reported, not guessed at === */
    jdata = build_map(1, 1, 1);
    opts_init(&opts, 0);
    rc = prte_rmaps_base_compute_vpids(jdata, &opts, -1, NULL);
    CHECK("unknown policy: refused", PRTE_ERR_NOT_IMPLEMENTED == rc);
    PMIX_RELEASE(jdata);

    /* === by-span must terminate even when a pass ranks nothing.
     *
     * Every span pass looks for a proc whose locale is the object it is
     * holding; the procs here have no locale at all, so no pass can ever
     * match one.  The loop is bounded by app->num_procs, and with nothing
     * to count it used to spin forever - a wedged HNP, not an error.  A
     * regression here shows up as this test never returning. */
    {
        hwloc_topology_t topo;
        prte_topology_t *t;
        prte_node_t *node;

        if (0 != hwloc_topology_init(&topo) ||
            0 != hwloc_topology_set_synthetic(topo, "package:2 core:2 pu:1") ||
            0 != hwloc_topology_load(topo)) {
            fprintf(stdout, "  SKIP span-termination (no synthetic topology support)\n");
        } else {
            jdata = build_map(1, 1, 2);
            /* the app claims more procs than the map actually holds, which is
             * the other way a pass can come up empty */
            ((prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0))->num_procs = 4;
            node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, 0);
            t = PMIX_NEW(prte_topology_t);
            t->topo = topo;
            node->topology = t;
            opts_init(&opts, PRTE_RANK_BY_SPAN);
            opts.maptype = HWLOC_OBJ_CORE;
            rc = prte_rmaps_base_compute_vpids(jdata, &opts, -1, NULL);
            CHECK("span: terminates", PRTE_SUCCESS == rc);
            CHECK("span: ranked nothing it could not place",
                  PMIX_RANK_INVALID == rank_of(jdata, 0, 0, 0, 2));
            PMIX_RELEASE(jdata);   /* releases the node, which releases t/topo */
        }
    }

    if (0 == failures) {
        fprintf(stdout, "  PASS test_ranking\n");
    }
    return failures;
}
