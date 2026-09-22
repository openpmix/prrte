/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * --tune files: the argv pre-scan that applies them as generic MCA params
 * (prte_schizo_base_parse_tune), and the strict check the prte
 * personality runs over a parsed command line (prte_schizo_base_check_tune).
 *
 * The pre-scan is what makes --tune work at all under the prte
 * personality - nothing read the option before - and it has to route
 * each entry exactly as "--mca name value" would be routed: to PRRTE if a
 * PRRTE framework claims it, else to PMIx.
 */

#include "test_schizo.h"

#include <unistd.h>

#include "src/class/pmix_list.h"

static char *write_file(const char *tag, const char *content)
{
    char *name;
    FILE *fp;

    pmix_asprintf(&name, "prte_schizo_tune_%s_%lu.conf", tag,
                  (unsigned long) getpid());
    fp = fopen(name, "w");
    if (NULL == fp) {
        free(name);
        return NULL;
    }
    fputs(content, fp);
    fclose(fp);
    return name;
}

static int scan(const char *opt, const char *file)
{
    char **argv = NULL, *tmp;
    int rc;

    PMIx_Argv_append_nosize(&argv, "prterun");
    if (NULL != strchr(opt, '=')) {
        pmix_asprintf(&tmp, "%s%s", opt, file);
        PMIx_Argv_append_nosize(&argv, tmp);
        free(tmp);
    } else {
        PMIx_Argv_append_nosize(&argv, opt);
        PMIx_Argv_append_nosize(&argv, file);
    }
    PMIx_Argv_append_nosize(&argv, "hostname");
    /* the tools hand the pre-scan their argv without the program name */
    rc = prte_schizo_base_parse_tune(PMIx_Argv_count(argv) - 1, 0, &argv[1]);
    PMIx_Argv_free(argv);
    return rc;
}

static int check(const char *file)
{
    pmix_cli_result_t results;
    int rc;

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_TUNE, file, NULL);
    rc = prte_schizo_base_check_tune(&results);
    PMIX_DESTRUCT(&results);
    return rc;
}

static void clear(void)
{
    unsetenv("PRTE_MCA_rmaps_base_verbose");
    unsetenv("PRTE_MCA_prte_tune_test");
    unsetenv("PMIX_MCA_pmix_tune_test");
    unsetenv("PRTE_MCA_bogus_tune_test");
    unsetenv("PMIX_MCA_bogus_tune_test");
}

int test_tune(void)
{
    int failures = 0;
    char *good, *bad, *dup, *foreign, *p;

    good = write_file("good",
                      "# a comment, then a blank line\n"
                      "\n"
                      "   rmaps_base_verbose   =   7   \n"
                      "prte_tune_test=\"quoted value\"\n"
                      "pmix_tune_test = 3\n"
                      "rmaps_base_verbose = 7\n"
                      "bogus_tune_test = 1");
    bad = write_file("bad", "rmaps_base_verbose 7\n");
    dup = write_file("dup", "rmaps_base_verbose = 7\nrmaps_base_verbose = 8\n");
    foreign = write_file("foreign", "-x FOO=bar\nprte_tune_test = 1\n");
    if (NULL == good || NULL == bad || NULL == dup || NULL == foreign) {
        fprintf(stderr, "FAIL [tune]: cannot create the tune files\n");
        return 1;
    }

    /*** the pre-scan applies each entry where "--mca" would ***/
    clear();
    CHECK("tune:scan-rc", PRTE_SUCCESS == scan("--tune", good));
    p = getenv("PRTE_MCA_rmaps_base_verbose");
    CHECK("tune:prte-framework", NULL != p && 0 == strcmp(p, "7"));
    p = getenv("PRTE_MCA_prte_tune_test");
    CHECK("tune:prte-project-quoted", NULL != p && 0 == strcmp(p, "quoted value"));
    p = getenv("PMIX_MCA_pmix_tune_test");
    CHECK("tune:pmix-project", NULL != p && 0 == strcmp(p, "3"));
    /* belongs to nobody, so it is exposed to nobody */
    CHECK("tune:unclaimed-prte", NULL == getenv("PRTE_MCA_bogus_tune_test"));
    CHECK("tune:unclaimed-pmix", NULL == getenv("PMIX_MCA_bogus_tune_test"));

    /*** both spellings getopt accepts ***/
    clear();
    CHECK("tune:eq-form-rc", PRTE_SUCCESS == scan("--tune=", good));
    p = getenv("PRTE_MCA_rmaps_base_verbose");
    CHECK("tune:eq-form", NULL != p && 0 == strcmp(p, "7"));

    /*** the pre-scan passes over what it does not understand - another
     *** personality's grammar, a malformed line, a missing file - and
     *** says nothing: it runs before the help content is registered ***/
    clear();
    CHECK("tune:scan-foreign", PRTE_SUCCESS == scan("--tune", foreign));
    p = getenv("PRTE_MCA_prte_tune_test");
    CHECK("tune:foreign-rest-applied", NULL != p && 0 == strcmp(p, "1"));
    CHECK("tune:scan-bad", PRTE_SUCCESS == scan("--tune", bad));
    CHECK("tune:scan-missing", PRTE_SUCCESS == scan("--tune", "/nonexistent/tune.conf"));
    /* of two conflicting values it applies the first */
    clear();
    CHECK("tune:scan-dup", PRTE_SUCCESS == scan("--tune", dup));
    p = getenv("PRTE_MCA_rmaps_base_verbose");
    CHECK("tune:dup-first-wins", NULL != p && 0 == strcmp(p, "7"));

    /*** the strict check refuses all of those ***/
    CHECK("tune:check-unclaimed", PRTE_SUCCESS != check(good));
    CHECK("tune:check-bad-line", PRTE_SUCCESS != check(bad));
    CHECK("tune:check-dup", PRTE_SUCCESS != check(dup));
    CHECK("tune:check-foreign", PRTE_SUCCESS != check(foreign));
    CHECK("tune:check-missing", PRTE_SUCCESS != check("/nonexistent/tune.conf"));
    /* a repeat with the same value is not a conflict */
    unlink(good);
    free(good);
    good = write_file("good2", "rmaps_base_verbose = 7\n\n# c\nrmaps_base_verbose=7\n");
    CHECK("tune:check-clean", NULL != good && PRTE_SUCCESS == check(good));

    clear();
    unlink(good);
    unlink(bad);
    unlink(dup);
    unlink(foreign);
    free(good);
    free(bad);
    free(dup);
    free(foreign);
    return failures;
}
