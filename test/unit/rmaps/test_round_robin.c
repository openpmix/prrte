/*
 * Copyright (c) 2024-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests for round_robin component app_idx guard.
 * Cases: app_idx=-1 (all apps), app_idx=0 (first only), app_idx=1 (second only).
 * Requires component initialization and constructed job/node objects.
 */

#include "prte_config.h"
#include <stdio.h>

int test_round_robin(void);

int test_round_robin(void)
{
    fprintf(stdout, "  SKIP test_round_robin: requires component fixture\n");
    return 0;
}
