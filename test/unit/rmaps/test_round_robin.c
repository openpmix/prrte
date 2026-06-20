/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the round_robin component's dispatch guards - the paths that decide
 * whether the component will handle a job - which are reachable without a node
 * pool or topology.  Full placement and the app_idx guard are verified
 * end-to-end via the offline "prterun --rtos donotlaunch --display map" method
 * documented in AGENTS.md.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"

extern prte_rmaps_base_module_t prte_rmaps_round_robin_module;

int test_round_robin(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

int test_round_robin(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_rmaps_options_t opts;
    int rc;

    /* a policy this component does not handle -> defer to the next mapper */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_SEQ);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    rc = prte_rmaps_round_robin_module.map_job(jdata, &opts);
    CHECK("rr defers non-rr policy", PRTE_ERR_TAKE_NEXT_OPTION == rc);
    PMIX_RELEASE(jdata);

    /* a restarting job is never (re)mapped by round_robin */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYNODE);
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_RESTART);
    memset(&opts, 0, sizeof(opts));
    opts.app_idx = -1;
    rc = prte_rmaps_round_robin_module.map_job(jdata, &opts);
    CHECK("rr defers restart", PRTE_ERR_TAKE_NEXT_OPTION == rc);
    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_round_robin\n");
    }
    return failures;
}
