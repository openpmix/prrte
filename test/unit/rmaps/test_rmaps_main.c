/*
 * Copyright (c) 2024-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>

#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"
#include "constants.h"

extern int test_policy_parse(void);
extern int test_resolve_options(void);
extern int test_dispatch(void);
extern int test_round_robin(void);
extern int test_ppr(void);
extern int test_seq(void);
extern int test_rank_file(void);

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    failures += test_policy_parse();
    failures += test_resolve_options();
    failures += test_dispatch();
    failures += test_round_robin();
    failures += test_ppr();
    failures += test_seq();
    failures += test_rank_file();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rmaps unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rmaps unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
