/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the rank_file component's dispatch guards (reachable without a node
 * pool).  Per-app rankfile selection (PRTE_APP_MAP_FILE before PRTE_JOB_FILE)
 * and the app_idx guard are verified end-to-end via the offline
 * "prterun --rtos donotlaunch --display map" method documented in AGENTS.md.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"

extern prte_rmaps_base_module_t prte_rmaps_rank_file_module;

int test_rank_file(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

int test_rank_file(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_rmaps_options_t opts;
    int rc;

    /* not a rankfile (BYUSER) job -> defer */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    rc = prte_rmaps_rank_file_module.map_job(jdata, &opts);
    CHECK("rf defers non-byuser policy", PRTE_ERR_TAKE_NEXT_OPTION == rc);
    PMIX_RELEASE(jdata);

    /* a different mapper was requested -> defer */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYUSER);
    jdata->map->req_mapper = strdup("round_robin");
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    rc = prte_rmaps_rank_file_module.map_job(jdata, &opts);
    CHECK("rf defers other req_mapper", PRTE_ERR_TAKE_NEXT_OPTION == rc);
    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_rank_file\n");
    }
    return failures;
}
