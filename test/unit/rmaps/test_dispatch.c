/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the per-app resolution that prte_rmaps_base_map_job()'s dispatch loop
 * performs once per app context.  We drive prte_rmaps_base_resolve_app_options()
 * for a two-app job where each app carries its own per-app directives, and
 * confirm that each app resolves to its own distinct options - the exact
 * behavior that was silently broken when the per-app attributes were stored
 * as PRTE_ATTR_LOCAL and dropped before mapping.
 *
 * Full placement/ranking/binding across real nodes (the app_idx dispatch
 * itself) is verified end-to-end without a DVM via the offline
 * "prterun --rtos donotlaunch --display map" method documented in AGENTS.md.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/attr.h"

int test_dispatch(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void job_baseline(prte_rmaps_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->map = PRTE_MAPPING_BYSLOT;
    opts->rank = PRTE_RANK_BY_SLOT;
    opts->bind = PRTE_BIND_TO_NONE;
    opts->maptype = HWLOC_OBJ_MACHINE;
    opts->app_idx = -1;
}

int test_dispatch(void)
{
    int failures = 0;
    prte_app_context_t *app0, *app1;
    prte_rmaps_options_t o0, o1;

    /* app0: map-by node (no explicit rank) -> rank defaults to by-node */
    app0 = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app0, "node");

    /* app1: map-by package with an explicit rank-by slot -> rank stays by-slot,
     * bind defaults from the package map */
    app1 = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app1, "package");
    prte_rmaps_base_set_app_ranking_policy(app1, "slot");

    /* resolve each app from a fresh copy of the job-level options, exactly as
     * the dispatch loop does (app_options = options; resolve(app)) */
    job_baseline(&o0);
    o0.app_idx = 0;
    CHECK("app0 resolve rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app0, &o0));

    job_baseline(&o1);
    o1.app_idx = 1;
    CHECK("app1 resolve rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app1, &o1));

    /* the two apps must end up with genuinely different, correct options */
    CHECK("app0 map node", PRTE_MAPPING_BYNODE == o0.map);
    CHECK("app0 rank node (default)", PRTE_RANK_BY_NODE == o0.rank);

    CHECK("app1 map package", PRTE_MAPPING_BYPACKAGE == o1.map);
    CHECK("app1 rank slot (explicit)", PRTE_RANK_BY_SLOT == o1.rank);
    CHECK("app1 bind package (default)", PRTE_BIND_TO_PACKAGE == o1.bind);

    CHECK("apps differ in map", o0.map != o1.map);
    CHECK("apps differ in rank", o0.rank != o1.rank);

    PMIX_RELEASE(app0);
    PMIX_RELEASE(app1);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_dispatch\n");
    }
    return failures;
}
