/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the node rank prte_rmaps_base_setup_proc() hands each proc.
 *
 * A node rank names a process among everything alive on its node, across
 * every job running there - that is the whole difference between it and
 * local_rank, which is numbered within one job.  So the property under test
 * is uniqueness across jobs, and the case that breaks it is the ordinary
 * life of a persistent DVM: a job ends while another is still running, and
 * a third is mapped into the space the first left behind.
 *
 * This used to be read off node->num_procs, a population count that goes
 * back *down* when any job's procs leave, so the third job was handed the
 * node ranks the second was still using and two live procs answered
 * PMIX_NODE_RANK with the same number.  Repeated PMIx_Spawn makes that the
 * common case rather than a corner.
 *
 * With bind-to-none there is no cpuset arithmetic, so setup_proc is
 * reachable here without a topology.
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

int test_node_rank(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

static prte_job_t *mkjob(const char *nspace, bool tool)
{
    prte_job_t *jdata;
    prte_app_context_t *app;

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, nspace);
    jdata->map = PMIX_NEW(prte_job_map_t);
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup("hostname");
    if (tool) {
        PRTE_FLAG_SET(app, PRTE_APP_FLAG_TOOL);
    }
    app->idx = pmix_pointer_array_add(jdata->apps, app);
    jdata->num_apps++;
    return jdata;
}

static void opts_init(prte_rmaps_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->app_idx = -1;
    opts->bind = PRTE_BIND_TO_NONE;
    opts->cpus_per_rank = 1;
    opts->nprocs = 1;
}

/* Retire a job's procs from the node exactly as check_complete() does when
 * the job terminates: clear the node's slot, drop the counts, and give up
 * the map's reference. */
static void retire(prte_node_t *node, prte_job_t *jdata)
{
    prte_proc_t *proc;
    int i;

    for (i = 0; i < node->procs->size; i++) {
        proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, i);
        if (NULL == proc) {
            continue;
        }
        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
            continue;
        }
        node->slots_inuse--;
        node->num_procs--;
        pmix_pointer_array_set_item(node->procs, i, NULL);
        PMIX_RELEASE(proc);
    }
}

int test_node_rank(void)
{
    int failures = 0;
    prte_job_t *jobA, *jobB, *jobC, *jtool;
    prte_node_t *node;
    prte_proc_t *a[2], *b[2], *c[2], *t;
    prte_rmaps_options_t opts;
    int i, j;

    node = PMIX_NEW(prte_node_t);
    node->name = strdup("node0");
    node->slots = 16;
    node->slots_available = 16;

    jobA = mkjob("nrtestA", false);
    jobB = mkjob("nrtestB", false);
    jobC = mkjob("nrtestC", false);
    jtool = mkjob("nrtestT", true);

    /* === job A takes the first two ranks on an empty node === */
    opts_init(&opts);
    for (i = 0; i < 2; i++) {
        a[i] = prte_rmaps_base_setup_proc(jobA, 0, node, NULL, &opts);
    }
    CHECK("A: both procs placed", NULL != a[0] && NULL != a[1]);
    CHECK("A: rank 0", 0 == a[0]->node_rank);
    CHECK("A: rank 1", 1 == a[1]->node_rank);

    /* === job B, mapped while A is still running, gets its own === */
    for (i = 0; i < 2; i++) {
        b[i] = prte_rmaps_base_setup_proc(jobB, 0, node, NULL, &opts);
    }
    CHECK("B: both procs placed", NULL != b[0] && NULL != b[1]);
    CHECK("B: distinct from A[0]",
          b[0]->node_rank != a[0]->node_rank && b[1]->node_rank != a[0]->node_rank);
    CHECK("B: distinct from A[1]",
          b[0]->node_rank != a[1]->node_rank && b[1]->node_rank != a[1]->node_rank);
    CHECK("B: distinct from each other", b[0]->node_rank != b[1]->node_rank);

    /* === A terminates; B is still running === */
    retire(node, jobA);

    /* === job C is mapped into the space A left.  It may reuse A's ranks -
     * nothing holds them now - but it must not collide with B, which is
     * still on the node.  This is the regression: read off num_procs, C
     * came back with B's ranks. === */
    for (i = 0; i < 2; i++) {
        c[i] = prte_rmaps_base_setup_proc(jobC, 0, node, NULL, &opts);
    }
    CHECK("C: both procs placed", NULL != c[0] && NULL != c[1]);
    CHECK("C: distinct from each other", c[0]->node_rank != c[1]->node_rank);
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            CHECK("C: no collision with the live job",
                  c[i]->node_rank != b[j]->node_rank);
        }
    }

    /* === a tool proc occupies no rank at all, and does not disturb the
     * ranks of the procs mapped after it === */
    t = prte_rmaps_base_setup_proc(jtool, 0, node, NULL, &opts);
    CHECK("tool: placed", NULL != t);
    CHECK("tool: no node rank", PRTE_NODE_RANK_INVALID == t->node_rank);
    if (NULL != t) {
        prte_proc_t *after = prte_rmaps_base_setup_proc(jobC, 0, node, NULL, &opts);
        CHECK("after tool: placed", NULL != after);
        if (NULL != after) {
            CHECK("after tool: not the tool's slot value",
                  PRTE_NODE_RANK_INVALID != after->node_rank);
            for (j = 0; j < 2; j++) {
                CHECK("after tool: no collision with the live job",
                      after->node_rank != b[j]->node_rank);
                CHECK("after tool: no collision with its own job",
                      after->node_rank != c[j]->node_rank);
            }
            PMIX_RELEASE(after);
        }
        PMIX_RELEASE(t);
    }

    /* setup_proc retains each proc for the caller on top of the node's own
     * reference - drop ours, then let the node's teardown do the rest */
    for (i = 0; i < 2; i++) {
        PMIX_RELEASE(b[i]);
        PMIX_RELEASE(c[i]);
    }
    retire(node, jobB);
    retire(node, jobC);

    PMIX_RELEASE(jobA);
    PMIX_RELEASE(jobB);
    PMIX_RELEASE(jobC);
    PMIX_RELEASE(jtool);
    PMIX_RELEASE(node);

    if (0 == failures) {
        fprintf(stderr, "test_node_rank: PASS\n");
    }
    return failures;
}
