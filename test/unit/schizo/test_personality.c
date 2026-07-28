/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Personality election (detect_proxy) and the per-personality command line
 * translation (parse_cli + convert_deprecated_cli).  These decide which
 * dialect a command line is read in and what the deprecated spellings turn
 * into, so a defect here changes what a job does without changing what the
 * user typed.
 */

#include "test_schizo.h"

#include <stdarg.h>

#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

/* Parse a command line with the given module and return the value of one
 * option key, or NULL if the option is absent.  The returned string belongs
 * to the caller. */
static char *parse_and_get(prte_schizo_base_module_t *mod, const char *key,
                           int *rcp, const char *first, ...)
{
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;
    char **argv = NULL;
    char *value = NULL;
    va_list ap;
    const char *p;

    PMIx_Argv_append_nosize(&argv, first);
    va_start(ap, first);
    while (NULL != (p = va_arg(ap, const char *))) {
        PMIx_Argv_append_nosize(&argv, p);
    }
    va_end(ap);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    *rcp = mod->parse_cli(argv, &results, PMIX_CLI_SILENT);
    if (PRTE_SUCCESS == *rcp) {
        opt = pmix_cmd_line_get_param(&results, key);
        if (NULL != opt && NULL != opt->values && NULL != opt->values[0]) {
            value = strdup(opt->values[0]);
        }
    }
    PMIX_DESTRUCT(&results);
    PMIx_Argv_free(argv);
    return value;
}

static int test_prte_personality(prte_schizo_base_module_t *mod)
{
    int failures = 0, rc;
    char *v;

    /* --merge-stderr-to-stdout is in the prterun/prun tables; with no
     * conversion behind it the option parsed cleanly and then did nothing */
    v = parse_and_get(mod, PRTE_CLI_OUTPUT, &rc, "prterun",
                      "--merge-stderr-to-stdout", "hostname", NULL);
    CHECK("prte:merge-rc", PRTE_SUCCESS == rc);
    CHECK("prte:merge-value",
          NULL != v && 0 == strcmp(v, PRTE_CLI_MERGE_ERROUT));
    free(v);

    /* likewise --display-devel-allocation, which folds onto --display
     * allocation (there is no separate developer allocation display) */
    v = parse_and_get(mod, PRTE_CLI_DISPLAY, &rc, "prterun",
                      "--display-devel-allocation", "hostname", NULL);
    CHECK("prte:devel-alloc-rc", PRTE_SUCCESS == rc);
    CHECK("prte:devel-alloc-value",
          NULL != v && 0 == strcmp(v, PRTE_CLI_ALLOC));
    free(v);

    /* the classic ORTE-era spellings still land on the modern directives */
    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--npernode", "2", "hostname", NULL);
    CHECK("prte:npernode-rc", PRTE_SUCCESS == rc);
    CHECK("prte:npernode-value", NULL != v && 0 == strcmp(v, "ppr:2:node"));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--pernode", "hostname", NULL);
    CHECK("prte:pernode-rc", PRTE_SUCCESS == rc);
    CHECK("prte:pernode-value", NULL != v && 0 == strcmp(v, "ppr:1:node"));
    free(v);

    /* socket is the old name for package */
    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--mapby", "socket:pe=2", "hostname", NULL);
    CHECK("prte:socket-rc", PRTE_SUCCESS == rc);
    CHECK("prte:socket-value", NULL != v && 0 == strcmp(v, "package:pe=2"));
    free(v);

    /* ...including as the RESOURCE of a ppr pattern.  The resource is the
     * third ':'-delimited field; looking for it with strrchr() found the
     * last field instead, so a trailing qualifier hid it */
    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--mapby", "ppr:2:socket", "hostname", NULL);
    CHECK("prte:ppr-socket-rc", PRTE_SUCCESS == rc);
    CHECK("prte:ppr-socket-value", NULL != v && 0 == strcmp(v, "ppr:2:package"));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--mapby", "ppr:2:socket:pe=2", "hostname", NULL);
    CHECK("prte:ppr-socket-qual-rc", PRTE_SUCCESS == rc);
    CHECK("prte:ppr-socket-qual-value",
          NULL != v && 0 == strcmp(v, "ppr:2:package:pe=2"));
    free(v);

    /* a BARE "ppr" has no resource field at all - this used to be reached
     * for anyway and dereferenced past the end of the string */
    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "prterun",
                      "--mapby", "ppr", "hostname", NULL);
    CHECK("prte:ppr-bare-rc", PRTE_SUCCESS == rc);
    CHECK("prte:ppr-bare-value", NULL != v && 0 == strcmp(v, "ppr"));
    free(v);

    /* --rank-by objects other than "socket" are NOT renamed.  Rewriting
     * numa/core/l1cache/... to "package" was never a rename - none of them
     * is a valid ranker either way, and the rewrite only made the error
     * message name an option the user never typed */
    v = parse_and_get(mod, PRTE_CLI_RANKBY, &rc, "prterun",
                      "--rankby", "numa", "hostname", NULL);
    CHECK("prte:rankby-numa-rc", PRTE_SUCCESS == rc);
    CHECK("prte:rankby-numa-value", NULL != v && 0 == strcmp(v, "numa"));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_RANKBY, &rc, "prterun",
                      "--rankby", "core", "hostname", NULL);
    CHECK("prte:rankby-core-rc", PRTE_SUCCESS == rc);
    CHECK("prte:rankby-core-value", NULL != v && 0 == strcmp(v, "core"));
    free(v);

    /* "socket" IS renamed, since that one really is package's old name */
    v = parse_and_get(mod, PRTE_CLI_RANKBY, &rc, "prterun",
                      "--rankby", "socket", "hostname", NULL);
    CHECK("prte:rankby-socket-rc", PRTE_SUCCESS == rc);
    CHECK("prte:rankby-socket-value", NULL != v && 0 == strcmp(v, "package"));
    free(v);

    /* --bind-to socket likewise */
    v = parse_and_get(mod, PRTE_CLI_BINDTO, &rc, "prterun",
                      "--bindto", "socket", "hostname", NULL);
    CHECK("prte:bindto-socket-rc", PRTE_SUCCESS == rc);
    CHECK("prte:bindto-socket-value", NULL != v && 0 == strcmp(v, "package"));
    free(v);

    return failures;
}

static int test_ompi_personality(prte_schizo_base_module_t *mod)
{
    int failures = 0, rc;
    char *v;

    /* the crash this personality carried until the prte-side fix was
     * mirrored here: a bare "ppr" has no ':' for strrchr() to find, and the
     * result was advanced past and dereferenced */
    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "mpirun",
                      "--mapby", "ppr", "hostname", NULL);
    CHECK("ompi:ppr-bare-rc", PRTE_SUCCESS == rc);
    CHECK("ompi:ppr-bare-value", NULL != v && 0 == strcmp(v, "ppr"));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "mpirun",
                      "--mapby", "ppr:2:socket:pe=2", "hostname", NULL);
    CHECK("ompi:ppr-socket-qual-rc", PRTE_SUCCESS == rc);
    CHECK("ompi:ppr-socket-qual-value",
          NULL != v && 0 == strcmp(v, "ppr:2:package:pe=2"));
    free(v);

    /* the shared deprecations must give the same answer as prte's */
    v = parse_and_get(mod, PRTE_CLI_OUTPUT, &rc, "mpirun",
                      "--merge-stderr-to-stdout", "hostname", NULL);
    CHECK("ompi:merge-rc", PRTE_SUCCESS == rc);
    CHECK("ompi:merge-value",
          NULL != v && 0 == strcmp(v, PRTE_CLI_MERGE_ERROUT));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_DISPLAY, &rc, "mpirun",
                      "--display-devel-allocation", "hostname", NULL);
    CHECK("ompi:devel-alloc-rc", PRTE_SUCCESS == rc);
    CHECK("ompi:devel-alloc-value",
          NULL != v && 0 == strcmp(v, PRTE_CLI_ALLOC));
    free(v);

    v = parse_and_get(mod, PRTE_CLI_MAPBY, &rc, "mpirun",
                      "--npernode", "2", "hostname", NULL);
    CHECK("ompi:npernode-rc", PRTE_SUCCESS == rc);
    CHECK("ompi:npernode-value", NULL != v && 0 == strcmp(v, "ppr:2:node"));
    free(v);

    return failures;
}

int test_personality(void)
{
    int failures = 0;
    prte_schizo_base_module_t *mod, *prte_mod, *ompi_mod;

    /* the environment can name a personality; make sure it is not doing so
     * behind these checks */
    unsetenv("PRTE_MCA_schizo_proxy");
    unsetenv("PRTE_MCA_personality");

    prte_mod = schizo_test_module("prte");
    CHECK("personality:prte-present", NULL != prte_mod);
    ompi_mod = schizo_test_module("ompi");

    /*** with no hint at all, prte is the fallback ***/
    mod = prte_schizo_base_detect_proxy(NULL);
    CHECK("detect:default-is-prte",
          NULL != mod && 0 == strcmp(mod->name, "prte"));

    /*** an explicit request is honored ***/
    mod = prte_schizo_base_detect_proxy("prte");
    CHECK("detect:explicit-prte",
          NULL != mod && 0 == strcmp(mod->name, "prte"));

    if (NULL != ompi_mod) {
        mod = prte_schizo_base_detect_proxy("ompi");
        CHECK("detect:explicit-ompi",
              NULL != mod && 0 == strcmp(mod->name, "ompi"));
    }

    /*** A personality nobody claims falls back to the DEFAULT one - never to
     *** whichever component carries the highest static priority.  That
     *** distinction is the whole point: landing on some other personality's
     *** dialect changes which options are legal and how MCA params are
     *** translated, while landing on the catch-all is what Open MPI relies on
     *** (it starts a singleton's DVM with only the native component loaded,
     *** then spawns with PMIX_PERSONALITY="ompi5" - refusing that fails every
     *** MPI_Comm_spawn from a singleton). ***/
    mod = prte_schizo_base_detect_proxy("no-such-personality");
    CHECK("detect:unknown-falls-back-to-default",
          NULL != mod && 0 == strcmp(mod->name, "prte"));
    mod = prte_schizo_base_detect_proxy("ompi5");
    CHECK("detect:unclaimed-suffix-is-not-ompi",
          NULL != mod && NULL != ompi_mod
              ? 0 == strcmp(mod->name, "ompi")   /* ompi claims it by substring */
              : (NULL != mod && 0 == strcmp(mod->name, "prte")));

    /*** the per-personality translations ***/
    if (NULL != prte_mod) {
        prte_tool_actual = "prterun";
        failures += test_prte_personality(prte_mod);
    }
    if (NULL != ompi_mod) {
        failures += test_ompi_personality(ompi_mod);
    } else {
        fprintf(stdout, "SKIP schizo/ompi checks - component not built\n");
    }

    return failures;
}
