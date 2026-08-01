/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the seq component's dispatch guards (reachable without a node pool).
 * Per-app file selection (PRTE_APP_MAP_FILE before PRTE_JOB_FILE) and the
 * app_idx guard are verified end-to-end via the offline
 * "prterun --rtos donotlaunch --display map" method documented in AGENTS.md.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"

extern prte_rmaps_base_module_t *test_rmaps_module(const char *name);

int test_seq(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

int test_seq(void)
{
    int failures = 0;
    prte_rmaps_base_module_t *mod;
    prte_job_t *jdata;
    prte_rmaps_options_t opts;
    int rc;

    mod = test_rmaps_module("seq");
    if (NULL == mod) {
        fprintf(stdout, "  SKIP test_seq (component not built)\n");
        return 0;
    }

    /* Not a seq job -> defer. The policy the gate has to read is the
     * resolved one in "opts", which in per-app dispatch is this app's own:
     * the job map is deliberately left saying "seq", so a gate that asks
     * the job rather than the options fails here. */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_SEQ);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = 0;
    opts.map = PRTE_MAPPING_BYSLOT;
    rc = mod->map_job(jdata, &opts);
    CHECK("seq defers non-seq policy", PRTE_ERR_TAKE_NEXT_OPTION == rc);
    PMIX_RELEASE(jdata);


    if (0 == failures) {
        fprintf(stdout, "  PASS test_seq\n");
    }
    return failures;
}
