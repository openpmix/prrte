/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests for prte_rmaps_base_resolve_app_options() and the
 * prte_rmaps_base_derive_ranking() / derive_binding() helpers.
 *
 * resolve_app_options() only reads app->attributes and writes the per-app
 * options struct, so it can be exercised directly without a node pool,
 * topology, or framework selection.  Attributes are populated through the
 * real set_app_*_policy() parsers so the stored encoding (including the
 * GIVEN/overload directive bits that resolve must mask off) matches production.
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

int test_resolve_options(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* Build a baseline "job-level" options struct: map-by slot, rank-by slot,
 * bind-to none - the values a per-app resolve should inherit when the app
 * supplies no directive of its own. */
static void baseline(prte_rmaps_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->map = PRTE_MAPPING_BYSLOT;
    opts->rank = PRTE_RANK_BY_SLOT;
    opts->bind = PRTE_BIND_TO_NONE;
    opts->maptype = HWLOC_OBJ_MACHINE;
    opts->app_idx = 0;
}

int test_resolve_options(void)
{
    int failures = 0;
    prte_app_context_t *app;
    prte_rmaps_options_t opts;

    /* === derive_ranking(): map policy -> default ranking === */
    CHECK("derive_rank node", PRTE_RANK_BY_NODE == prte_rmaps_base_derive_ranking(PRTE_MAPPING_BYNODE));
    CHECK("derive_rank slot", PRTE_RANK_BY_SLOT == prte_rmaps_base_derive_ranking(PRTE_MAPPING_BYSLOT));
    CHECK("derive_rank package", PRTE_RANK_BY_FILL == prte_rmaps_base_derive_ranking(PRTE_MAPPING_BYPACKAGE));
    CHECK("derive_rank core", PRTE_RANK_BY_FILL == prte_rmaps_base_derive_ranking(PRTE_MAPPING_BYCORE));
    {
        prte_mapping_policy_t span = PRTE_MAPPING_BYSLOT;
        PRTE_SET_MAPPING_DIRECTIVE(span, PRTE_MAPPING_SPAN);
        /* by-slot resolves before the SPAN check, so by-slot stays by-slot;
         * an object map with SPAN resolves to by-span */
        prte_mapping_policy_t pkgspan = PRTE_MAPPING_BYPACKAGE;
        PRTE_SET_MAPPING_DIRECTIVE(pkgspan, PRTE_MAPPING_SPAN);
        CHECK("derive_rank package+span", PRTE_RANK_BY_SPAN == prte_rmaps_base_derive_ranking(pkgspan));
    }

    /* === derive_binding(): map policy -> default binding === */
    CHECK("derive_bind package", PRTE_BIND_TO_PACKAGE == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYPACKAGE, false));
    CHECK("derive_bind numa", PRTE_BIND_TO_NUMA == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYNUMA, false));
    CHECK("derive_bind core", PRTE_BIND_TO_CORE == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYCORE, false));
    CHECK("derive_bind core+hwt", PRTE_BIND_TO_HWTHREAD == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYCORE, true));
    CHECK("derive_bind hwthread", PRTE_BIND_TO_HWTHREAD == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYHWTHREAD, false));
    CHECK("derive_bind node->core", PRTE_BIND_TO_CORE == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYNODE, false));
    CHECK("derive_bind node+hwt", PRTE_BIND_TO_HWTHREAD == prte_rmaps_base_derive_binding(PRTE_MAPPING_BYNODE, true));

    /* === resolve: app with no per-app attributes inherits the job options === */
    app = PMIX_NEW(prte_app_context_t);
    baseline(&opts);
    CHECK("noattr: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("noattr: map", PRTE_MAPPING_BYSLOT == opts.map);
    CHECK("noattr: rank", PRTE_RANK_BY_SLOT == opts.rank);
    CHECK("noattr: bind", PRTE_BIND_TO_NONE == opts.bind);
    PMIX_RELEASE(app);

    /* === resolve: per-app map-by node, no rank/bind -> both default from the
     *     app's own map (node->by-node rank, node->core bind) === */
    app = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app, "node");
    baseline(&opts);
    CHECK("mapnode: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("mapnode: map masked", PRTE_MAPPING_BYNODE == opts.map);   /* no GIVEN bit leaked */
    CHECK("mapnode: maptype", HWLOC_OBJ_MACHINE == opts.maptype);
    CHECK("mapnode: rank default", PRTE_RANK_BY_NODE == opts.rank);
    CHECK("mapnode: bind default", PRTE_BIND_TO_CORE == opts.bind);
    PMIX_RELEASE(app);

    /* === resolve: per-app map-by package refreshes maptype and defaults === */
    app = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app, "package");
    baseline(&opts);
    CHECK("mappkg: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("mappkg: map", PRTE_MAPPING_BYPACKAGE == opts.map);
    CHECK("mappkg: maptype", HWLOC_OBJ_PACKAGE == opts.maptype);
    CHECK("mappkg: rank default fill", PRTE_RANK_BY_FILL == opts.rank);
    CHECK("mappkg: bind default package", PRTE_BIND_TO_PACKAGE == opts.bind);
    PMIX_RELEASE(app);

    /* === resolve: explicit per-app rank-by wins over the map default === */
    app = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app, "node");
    prte_rmaps_base_set_app_ranking_policy(app, "slot");
    baseline(&opts);
    CHECK("explicitrank: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("explicitrank: rank slot", PRTE_RANK_BY_SLOT == opts.rank);  /* not node */
    PMIX_RELEASE(app);

    /* === resolve: explicit bind-to is masked and overload is lifted out === */
    app = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_binding_policy(app, "package:overload-allowed");
    baseline(&opts);
    CHECK("bindovl: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("bindovl: bind masked", PRTE_BIND_TO_PACKAGE == opts.bind);  /* no overload bit in bind */
    CHECK("bindovl: overload flag", opts.overload);
    PMIX_RELEASE(app);

    /* === resolve: pe=N and hwtcpus modifiers feed the cpu fields === */
    app = PMIX_NEW(prte_app_context_t);
    prte_rmaps_base_set_app_mapping_policy(app, "core:pe=2:hwtcpus");
    baseline(&opts);
    CHECK("pehwt: rc", PRTE_SUCCESS == prte_rmaps_base_resolve_app_options(NULL, app, &opts));
    CHECK("pehwt: cpus_per_rank", 2 == opts.cpus_per_rank);
    CHECK("pehwt: use_hwthreads", opts.use_hwthreads);
    PMIX_RELEASE(app);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_resolve_options\n");
    }
    return failures;
}
