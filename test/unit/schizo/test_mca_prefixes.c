/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * PMIx decides whether a generic "--mca" parameter is PRRTE's by the first
 * segment of its name, taking the list of segments from PRTE_MCA_PREFIXES
 * on its FIRST check and keeping it for the life of the process.  The
 * tools' argv pre-scan makes that first check, before any PRRTE init has
 * run - which is why this is a program of its own rather than a case in
 * test_schizo: that one initializes PRRTE before its first test.
 *
 * The pre-scan used to run before PRRTE published the variable, so any
 * command line carrying a "--mca" fixed PMIx on its built-in table and
 * PRRTE's own list was never read.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/include/prte_frameworks.h"
#include "src/mca/schizo/base/base.h"
#include "src/util/pmix_argv.h"

static int failures = 0;

#define CHECK(name, cond)                                    \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL [%s]: %s\n", name, #cond); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static bool listed(char **list, const char *word)
{
    int n;

    for (n = 0; NULL != list && NULL != list[n]; n++) {
        if (0 == strcmp(list[n], word)) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    char **argv = NULL, **published;
    const char *p;
    int n;

    unsetenv("PRTE_MCA_PREFIXES");
    unsetenv("PRTE_MCA_rml_base_radix");
    unsetenv("PRTE_MCA_routed_radix");
    unsetenv("PMIX_MCA_pif_base_do_not_resolve");

    PMIx_Argv_append_nosize(&argv, "--mca");
    PMIx_Argv_append_nosize(&argv, "rml_base_radix");
    PMIx_Argv_append_nosize(&argv, "4");
    PMIx_Argv_append_nosize(&argv, "--mca");
    PMIx_Argv_append_nosize(&argv, "btl_tcp_if_include");
    PMIx_Argv_append_nosize(&argv, "eth0");
    PMIx_Argv_append_nosize(&argv, "--mca");
    PMIx_Argv_append_nosize(&argv, "routed_radix");
    PMIx_Argv_append_nosize(&argv, "3");
    PMIx_Argv_append_nosize(&argv, "--mca");
    PMIx_Argv_append_nosize(&argv, "if_base_do_not_resolve");
    PMIx_Argv_append_nosize(&argv, "1");

    /* nothing initialized: this is the state a tool's pre-scan runs in */
    CHECK("prescan-rc",
          PRTE_SUCCESS == prte_schizo_base_parse_prte(PMIx_Argv_count(argv), 0, argv, NULL));
    CHECK("prescan-pmix-rc",
          PRTE_SUCCESS == prte_schizo_base_parse_pmix(PMIx_Argv_count(argv), 0, argv, NULL));

    /* the pre-scan published our list itself, before asking PMIx */
    p = getenv("PRTE_MCA_PREFIXES");
    CHECK("published", NULL != p);
    published = (NULL == p) ? NULL : PMIx_Argv_split(p, ',');
    for (n = 0; NULL != prte_framework_names[n]; n++) {
        if (!listed(published, prte_framework_names[n])) {
            fprintf(stderr, "FAIL [published-complete]: %s missing\n",
                    prte_framework_names[n]);
            ++failures;
        }
    }
    /* and the list is of parameter prefixes, not just framework
     * directories: these name live parameters but are not frameworks */
    CHECK("prefix-grpcomm", listed(published, "grpcomm"));
    CHECK("prefix-rml", listed(published, "rml"));
    CHECK("prefix-routed", listed(published, "routed"));
    CHECK("prefix-oob", listed(published, "oob"));
    /* PRRTE has no "if" framework - that is PMIx's pif */
    CHECK("prefix-no-if", !listed(published, "if"));
    PMIx_Argv_free(published);

    /* PRRTE's parameters were claimed and applied ... */
    CHECK("rml-rewritten", 0 == strcmp(argv[0], "--prtemca"));
    p = getenv("PRTE_MCA_rml_base_radix");
    CHECK("rml-applied", NULL != p && 0 == strcmp(p, "4"));
    CHECK("routed-rewritten", 0 == strcmp(argv[6], "--prtemca"));
    p = getenv("PRTE_MCA_routed_radix");
    CHECK("routed-applied", NULL != p && 0 == strcmp(p, "3"));
    /* ... and somebody else's was left alone */
    CHECK("foreign-untouched", 0 == strcmp(argv[3], "--mca"));

    /* "if" goes to PMIx as pif, never to a PRRTE "prteif" nothing reads -
     * and the generic option stays for Open MPI's own if framework */
    p = getenv("PMIX_MCA_pif_base_do_not_resolve");
    CHECK("if-to-pif", NULL != p && 0 == strcmp(p, "1"));
    CHECK("if-not-prte", NULL == getenv("PRTE_MCA_prteif_base_do_not_resolve"));
    CHECK("if-left-for-ompi", 0 == strcmp(argv[9], "--mca"));

    PMIx_Argv_free(argv);

    if (0 == failures) {
        printf("PASS: test_mca_prefixes\n");
        return 0;
    }
    return 1;
}
