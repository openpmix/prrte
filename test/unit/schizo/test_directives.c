/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The option->directive plumbing every deprecated-option conversion goes
 * through (add_directive / add_qualifier) and the directive validators the
 * sanity checker is built on (check_directives / check_qualifiers).
 */

#include "test_schizo.h"

static char *first_value(pmix_cli_result_t *results, const char *key)
{
    pmix_cli_item_t *opt;

    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt || NULL == opt->values) {
        return NULL;
    }
    return opt->values[0];
}

int test_directives(void)
{
    int failures = 0, rc;
    pmix_cli_result_t results;
    char *v;

    /* a cut-down vocabulary of the kind the sanity checker is handed; the
     * tags are the real ones, since the ppr special case reads the tag */
    const pmix_cli_choice_t mappers[] = {
        PMIX_CLI_CHOICE(PRTE_CLI_SLOT, PRTE_MAPPER_SLOT, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE(PRTE_CLI_NODE, PRTE_MAPPER_NODE, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE(PRTE_CLI_PPR, PRTE_MAPPER_PPR, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE(PRTE_CLI_PACKAGE, PRTE_MAPPER_PACKAGE, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE_END
    };
    const pmix_cli_choice_t mapquals[] = {
        PMIX_CLI_CHOICE(PRTE_CLI_PE, PRTE_MAPQUAL_PE, PMIX_CLI_VALUE_REQUIRED),
        PMIX_CLI_CHOICE(PRTE_CLI_SPAN, PRTE_MAPQUAL_SPAN, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE(PRTE_CLI_OVERSUB, PRTE_MAPQUAL_OVERSUB, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE(PRTE_CLI_NOLOCAL, PRTE_MAPQUAL_NOLOCAL, PMIX_CLI_VALUE_NONE),
        PMIX_CLI_CHOICE_END
    };

    /*** a directive lands on an option that did not exist yet ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = prte_schizo_base_add_directive(&results, "bynode", PRTE_CLI_MAPBY,
                                        PRTE_CLI_NODE, false);
    CHECK("add_directive:new-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_MAPBY);
    CHECK("add_directive:new-value", NULL != v && 0 == strcmp(v, PRTE_CLI_NODE));
    PMIX_DESTRUCT(&results);

    /*** a qualifier on an option that did not exist yet keeps its ':' ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = prte_schizo_base_add_qualifier(&results, "nolocal", PRTE_CLI_MAPBY,
                                        PRTE_CLI_NOLOCAL, false);
    CHECK("add_qualifier:new-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_MAPBY);
    CHECK("add_qualifier:new-value", NULL != v && 0 == strcmp(v, ":nolocal"));

    /*** and a directive added afterwards is PREPENDED to that qualifier ***/
    rc = prte_schizo_base_add_directive(&results, "bynode", PRTE_CLI_MAPBY,
                                        PRTE_CLI_NODE, false);
    CHECK("add_directive:prepend-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_MAPBY);
    CHECK("add_directive:prepend-value", NULL != v && 0 == strcmp(v, "node:nolocal"));
    PMIX_DESTRUCT(&results);

    /*** a second qualifier is appended with a ':' ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "node", NULL);
    rc = prte_schizo_base_add_qualifier(&results, "nolocal", PRTE_CLI_MAPBY,
                                        PRTE_CLI_NOLOCAL, false);
    CHECK("add_qualifier:append-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_MAPBY);
    CHECK("add_qualifier:append-value", NULL != v && 0 == strcmp(v, "node:nolocal"));
    PMIX_DESTRUCT(&results);

    /*** --map-by takes ONE directive: a second is an error, not a merge ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_MAPBY, "node", NULL);
    fprintf(stderr, "--- expected error output follows (map-by, two directives) ---\n");
    rc = prte_schizo_base_add_directive(&results, "bycore", PRTE_CLI_MAPBY,
                                        PRTE_CLI_CORE, false);
    CHECK("add_directive:single-value-option", PRTE_SUCCESS != rc);
    PMIX_DESTRUCT(&results);

    /*** --runtime-options does take several, comma-joined ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_RTOS, PRTE_CLI_NOLAUNCH, NULL);
    rc = prte_schizo_base_add_directive(&results, "stop-in-init", PRTE_CLI_RTOS,
                                        PRTE_CLI_STOP_IN_INIT, false);
    CHECK("add_directive:multi-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_RTOS);
    CHECK("add_directive:multi-value",
          NULL != v && 0 == strcmp(v, "donotlaunch,stop-in-init"));
    PMIX_DESTRUCT(&results);

    /*** ...and a multi-directive option keeps its qualifier at the end ***/
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    schizo_test_add(&results, PRTE_CLI_OUTPUT, "tag:raw", NULL);
    rc = prte_schizo_base_add_directive(&results, "timestamp-output",
                                        PRTE_CLI_OUTPUT, PRTE_CLI_TIMESTAMP,
                                        false);
    CHECK("add_directive:multi-qual-rc", PRTE_SUCCESS == rc);
    v = first_value(&results, PRTE_CLI_OUTPUT);
    CHECK("add_directive:multi-qual-value",
          NULL != v && 0 == strcmp(v, "tag,timestamp:raw"));
    PMIX_DESTRUCT(&results);

    /*** check_directives: plain directives ***/
    CHECK("check_directives:valid",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "node"));
    CHECK("check_directives:abbreviated",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "pack"));
    fprintf(stderr, "--- expected error output follows (unrecognized directive) ---\n");
    CHECK("check_directives:invalid",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "bogus"));

    /*** check_directives: qualifier-only form ***/
    CHECK("check_directives:qual-only",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, ":span"));
    fprintf(stderr, "--- expected error output follows (unrecognized qualifier) ---\n");
    CHECK("check_directives:qual-only-bad",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, ":bogus"));

    /*** check_directives: the ppr:N:resource special case ***/
    CHECK("check_directives:ppr-ok",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                            "ppr:2:node"));
    CHECK("check_directives:ppr-plus-qual",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                            "ppr:2:node:span"));
    fprintf(stderr, "--- expected error output follows (bad ppr patterns) ---\n");
    CHECK("check_directives:ppr-short",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                             "ppr:2"));
    CHECK("check_directives:ppr-not-a-number",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                             "ppr:x:node"));
    CHECK("check_directives:ppr-bad-resource",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                             "ppr:2:bogus"));

    /* A BARE "ppr" carries no pattern to check, so this validator passes it
     * through - rmaps is what refuses it later.  Recorded here because it is
     * exactly the string that reaches the personalities' socket->package
     * rewrite, where it used to be dereferenced past its end (see
     * test_personality). */
    CHECK("check_directives:ppr-bare-passes-through",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                            "ppr"));

    /*** an option written with no directive at all ***
     *
     * PMIx_Argv_split() hands back NULL - not an empty array - for a string
     * that is empty or yields no non-empty token, and every walk in this
     * validator indexes what it returns.  "--map-by=" and "--map-by=:" both
     * arrived here and dereferenced that NULL, killing the tool before it
     * could report anything.  They have to be refusals, not crashes.
     */
    fprintf(stderr, "--- expected error output follows (empty directives) ---\n");
    CHECK("check_directives:empty",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, ""));
    CHECK("check_directives:null",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, NULL));
    CHECK("check_directives:colon-only",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, ":"));
    CHECK("check_directives:colons-only",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, ":::"));

    /*** an option that accepts no qualifiers at all ***
     *
     * "--runtime-options" is checked with a NULL qualifier table, so a
     * qualifier written on it has nothing to match against.  Walking the
     * NULL table indexed it: "--rtos :anything" crashed the tool.
     */
    fprintf(stderr, "--- expected error output follows (qualifier, no table) ---\n");
    CHECK("check_directives:null-qualtable",
          !prte_schizo_base_check_directives(PRTE_CLI_RTOS, mappers, NULL, ":span"));
    CHECK("check_qualifiers:null-table",
          !prte_schizo_base_check_qualifiers(PRTE_CLI_RTOS, NULL, "span"));

    /*** check_qualifiers directly ***/
    CHECK("check_qualifiers:valid",
          prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "span"));
    CHECK("check_qualifiers:with-value",
          prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "pe=2"));
    fprintf(stderr, "--- expected error output follows (bad qualifier) ---\n");
    CHECK("check_qualifiers:invalid",
          !prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "bogus"));

    /*** a word is matched against the whole vocabulary ***
     *
     * One comparison at a time settled an abbreviation that fits two words
     * by whichever was tested first, and let a word that merely BEGAN with
     * a directive's name be that directive - "gpu,ndev=2" was "gpu" with
     * the rest thrown away.  A value given to a word that takes none was
     * dropped the same way: "span=false" turned SPAN on.
     */
    fprintf(stderr, "--- expected error output follows (ambiguous, over-long, values) ---\n");
    CHECK("check_directives:ambiguous",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "p"));
    CHECK("check_directives:longer-than-the-name",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "nodes"));
    CHECK("check_directives:value-on-a-directive-that-takes-none",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, "node=3"));
    CHECK("check_qualifiers:value-on-a-qualifier-that-takes-none",
          !prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "span=false"));
    CHECK("check_qualifiers:required-value-missing",
          !prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "pe"));
    CHECK("check_qualifiers:longer-than-the-name",
          !prte_schizo_base_check_qualifiers(PRTE_CLI_MAPBY, mapquals, "spanish"));

    /* the ppr object is held to the vocabulary the mapper reads it with:
     * "slot" used to pass here and be refused there, and "n" - node, numa
     * or nm - is ambiguous */
    CHECK("check_directives:ppr-object-the-mapper-cannot-place",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                             "ppr:2:slot"));
    CHECK("check_directives:ppr-object-ambiguous",
          !prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                             "ppr:2:n"));
    CHECK("check_directives:ppr-object-older-spelling",
          prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals,
                                            "ppr:2:socket"));

    /* --rtos is not split at ':' - the value of one of its directives may
     * contain one */
    CHECK("check_directives:rtos-value-with-colons",
          prte_schizo_base_check_directives(PRTE_CLI_RTOS, prte_cli_rtos_directives, NULL,
                                            "timeout=1:30:00"));

    return failures;
}
