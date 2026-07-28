/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "test_schizo.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

int main(void)
{
    int rc, failures = 0;

    /* the schizo modules read prte_tool_actual to pick a per-tool option
     * table, and prte_tool_basename for their error text - a tool sets both
     * in main() long before schizo is opened */
    prte_tool_actual = "prterun";
    prte_tool_basename = "prterun";

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* the personality tests take their modules from the framework, so it
     * has to be open and selected before they run */
    rc = pmix_mca_base_framework_open(&prte_schizo_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "schizo framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }
    rc = prte_schizo_base_select();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "schizo select failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_normalize();
    failures += test_directives();
    failures += test_sanity();
    failures += test_output();
    failures += test_personality();

    (void) pmix_mca_base_framework_close(&prte_schizo_base_framework);
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all schizo unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d schizo unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
