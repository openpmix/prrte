/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_schizo_base_sanity() - the shared check_sanity implementation both
 * personalities install.  It runs once per app context (prte_parse_locals
 * splits an MPMD line at each ':' and calls it on each segment), so every
 * expectation below is about ONE app's option set.
 */

#include "test_schizo.h"

int test_sanity(void)
{
    int failures = 0, rc;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;

    /*** a well-formed set passes ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "node:span", NULL);
    schizo_test_add(&results, PRTE_CLI_RANKBY, "slot", NULL);
    schizo_test_add(&results, PRTE_CLI_BINDTO, "core:overload-allowed", NULL);
    schizo_test_add(&results, PRTE_CLI_OUTPUT, "tag,timestamp", NULL);
    schizo_test_add(&results, PRTE_CLI_DISPLAY, "map:parseable", NULL);
    schizo_test_add(&results, PRTE_CLI_RTOS, "donotlaunch,stop-in-init", NULL);
    schizo_test_add(&results, PRTE_CLI_NP, "4", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:clean", PRTE_SUCCESS == rc);
    PMIX_DESTRUCT(&results);

    /*** repeating a single-value placement option is rejected ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "node", "core", NULL);
    fprintf(stderr, "--- expected error output follows (two --map-by) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:dup-mapby", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_RANKBY, "slot", "node", NULL);
    fprintf(stderr, "--- expected error output follows (two --rank-by) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:dup-rankby", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    /*** and so is repeating an option that carries exactly one value.
     *** pmix_cmd_line_parse appends every occurrence to the SAME instance,
     *** and every consumer reads values[0], so "-n 2 -n 3" used to run
     *** silently with 2 procs rather than being reported. ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_NP, "2", "3", NULL);
    fprintf(stderr, "--- expected error output follows (two --np) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:dup-np", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_WDIR, "/tmp/a", "/tmp/b", NULL);
    fprintf(stderr, "--- expected error output follows (two --wdir) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:dup-wdir", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    /* one value apiece is of course fine */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_NP, "2", NULL);
    schizo_test_add(&results, PRTE_CLI_WDIR, "/tmp/a", NULL);
    schizo_test_add(&results, PRTE_CLI_PSET, "myset", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:single-values", PRTE_SUCCESS == rc);
    PMIX_DESTRUCT(&results);

    /*** synonyms are expanded onto their canonical key ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MACHINEFILE, "/tmp/hosts", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:machinefile-rc", PRTE_SUCCESS == rc);
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
    CHECK("sanity:machinefile-synonym",
          NULL != opt && NULL != opt->values &&
          0 == strcmp(opt->values[0], "/tmp/hosts"));
    PMIX_DESTRUCT(&results);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_WD, "/tmp/here", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:wd-rc", PRTE_SUCCESS == rc);
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_WDIR);
    CHECK("sanity:wd-synonym",
          NULL != opt && NULL != opt->values &&
          0 == strcmp(opt->values[0], "/tmp/here"));
    PMIX_DESTRUCT(&results);

    /*** bad directives are caught ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "bogus", NULL);
    fprintf(stderr, "--- expected error output follows (bad map-by) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:bad-mapby", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_RTOS, "donotlaunch,bogus", NULL);
    fprintf(stderr, "--- expected error output follows (bad runtime option) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:bad-rtos", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    /*** every runtime option the parser acts on has to be in the table the
     *** sanity checker validates against, or it is refused here and never
     *** reaches the parser.  "autorestart" and "default-exec-agent" were
     *** missing from it while being documented and implemented. ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_RTOS,
                    "autorestart,default-exec-agent,recoverable", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:rtos-autorestart", PRTE_SUCCESS == rc);
    PMIX_DESTRUCT(&results);

    /*** a PE=n mapping already dictates the binding: only core/hwt agree ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "slot:pe=2", NULL);
    schizo_test_add(&results, PRTE_CLI_BINDTO, "package", NULL);
    fprintf(stderr, "--- expected error output follows (pe/bind conflict) ---\n");
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:pe-bind-conflict", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "slot:pe=2", NULL);
    schizo_test_add(&results, PRTE_CLI_BINDTO, "core", NULL);
    rc = prte_schizo_base_sanity(&results);
    CHECK("sanity:pe-bind-core-ok", PRTE_SUCCESS == rc);
    PMIX_DESTRUCT(&results);

    return failures;
}
