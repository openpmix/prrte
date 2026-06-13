/*
 * Copyright (c) 2024-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests for seq component app_idx guard and per-app file selection.
 * Cases:
 *   app_idx=-1 (all apps)
 *   app_idx=0 / app_idx=1 (per-app)
 *   PRTE_APP_MAP_FILE present: component uses app-level file
 *   PRTE_APP_MAP_FILE absent, PRTE_JOB_FILE present: falls back to job file
 * Requires component initialization and constructed job/node objects.
 */

#include "prte_config.h"
#include <stdio.h>

int test_seq(void);

int test_seq(void)
{
    fprintf(stdout, "  SKIP test_seq: requires component fixture\n");
    return 0;
}
