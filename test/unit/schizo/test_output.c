/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_schizo_base_parse_output() and _parse_display() - where the
 * human-facing "--output"/"--display" directive strings become the PMIx
 * keys carried on the spawn request.  A directive that silently fails to
 * produce its key is invisible: the job runs, just not the way it was asked
 * to, so these are checked by inspecting the resulting info array.
 */

#include "test_schizo.h"

#include "src/pmix/pmix-internal.h"

/* Run a parser over an option value set and hand back the info array it
 * produced.  Returns the parser's rc; the returned array is only valid on
 * PRTE_SUCCESS and must be released by the caller. */
static int run_parser(int (*parser)(pmix_cli_item_t *, void *),
                      const char *key, char **values,
                      pmix_info_t **iptr, size_t *ninfo)
{
    pmix_cli_item_t opt;
    pmix_data_array_t darray;
    void *jinfo;
    int rc;
    pmix_status_t ret;
    int n;

    *iptr = NULL;
    *ninfo = 0;

    PMIX_CONSTRUCT(&opt, pmix_cli_item_t);
    opt.key = strdup(key);
    for (n = 0; NULL != values[n]; n++) {
        PMIx_Argv_append_nosize(&opt.values, values[n]);
    }

    PMIX_INFO_LIST_START(jinfo);
    rc = parser(&opt, jinfo);
    if (PRTE_SUCCESS == rc) {
        PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
        if (PMIX_SUCCESS == ret) {
            *iptr = (pmix_info_t *) darray.array;
            *ninfo = darray.size;
        } else if (PMIX_ERR_EMPTY != ret) {
            rc = prte_pmix_convert_status(ret);
        }
    }
    PMIX_INFO_LIST_RELEASE(jinfo);
    PMIX_DESTRUCT(&opt);
    return rc;
}

static bool has_key(pmix_info_t *iptr, size_t ninfo, const char *key)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&iptr[n], key)) {
            return true;
        }
    }
    return false;
}

static bool key_is_true(pmix_info_t *iptr, size_t ninfo, const char *key,
                        bool *found)
{
    size_t n;

    *found = false;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&iptr[n], key)) {
            *found = true;
            return PMIX_INFO_TRUE(&iptr[n]);
        }
    }
    return false;
}

static char *key_string(pmix_info_t *iptr, size_t ninfo, const char *key)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&iptr[n], key)) {
            return iptr[n].value.data.string;
        }
    }
    return NULL;
}

int test_output(void)
{
    int failures = 0, rc;
    pmix_info_t *iptr;
    size_t ninfo;
    bool found, val;
    char *vals1[] = {"tag,timestamp", NULL};
    char *vals2[] = {"tag", "timestamp", NULL};
    char *vals3[] = {"file=/tmp/prte-schizo-test:raw:nocopy", NULL};
    char *vals4[] = {"file=/tmp/prte-schizo-test:raw:copy", NULL};
    char *vals5[] = {"file=/tmp/prte-schizo-test:copy:nocopy", NULL};
    char *vals6[] = {"dir=/tmp/prte-schizo-testdir", NULL};
    char *vals7[] = {"file=/tmp/a", "dir=/tmp/b", NULL};
    char *dvals1[] = {"map,bind", NULL};
    char *dvals2[] = {"map", "bind", NULL};
    char *dvals3[] = {"map:parseable", NULL};
    char *dvals4[] = {"topo=node1;node2", NULL};
    char *dvals5[] = {"map:bogus", NULL};

    /*** comma-delimited directives in ONE value ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals1,
                    &iptr, &ninfo);
    CHECK("output:comma-rc", PRTE_SUCCESS == rc);
    CHECK("output:comma-tag", has_key(iptr, ninfo, PMIX_IOF_TAG_OUTPUT));
    CHECK("output:comma-timestamp",
          has_key(iptr, ninfo, PMIX_IOF_TIMESTAMP_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** the SAME directives given as two instances of --output.  Repeating
     *** an option appends to one instance's value array, and the parser used
     *** to re-read values[0] on every pass - so "timestamp" was dropped and
     *** "tag" applied twice. ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals2,
                    &iptr, &ninfo);
    CHECK("output:multi-instance-rc", PRTE_SUCCESS == rc);
    CHECK("output:multi-instance-tag", has_key(iptr, ninfo, PMIX_IOF_TAG_OUTPUT));
    CHECK("output:multi-instance-timestamp",
          has_key(iptr, ninfo, PMIX_IOF_TIMESTAMP_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** qualifiers are ':'-delimited, and EVERY one of them counts.  They
     *** used to be split on ',' - which the directive split had already
     *** consumed - so only the first qualifier in the run was ever seen and
     *** "raw:nocopy" quietly lost its nocopy. ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals3,
                    &iptr, &ninfo);
    CHECK("output:qual-rc", PRTE_SUCCESS == rc);
    CHECK("output:qual-file", has_key(iptr, ninfo, PMIX_IOF_OUTPUT_TO_FILE));
    CHECK("output:qual-raw", has_key(iptr, ninfo, PMIX_IOF_OUTPUT_RAW));
    val = key_is_true(iptr, ninfo, PMIX_IOF_FILE_ONLY, &found);
    CHECK("output:qual-nocopy-present", found);
    CHECK("output:qual-nocopy-true", val);
    PMIX_INFO_FREE(iptr, ninfo);

    /*** the same, with "copy" second ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals4,
                    &iptr, &ninfo);
    CHECK("output:copy-rc", PRTE_SUCCESS == rc);
    CHECK("output:copy-raw", has_key(iptr, ninfo, PMIX_IOF_OUTPUT_RAW));
    val = key_is_true(iptr, ninfo, PMIX_IOF_FILE_ONLY, &found);
    CHECK("output:copy-present", found);
    CHECK("output:copy-false", !val);
    PMIX_INFO_FREE(iptr, ninfo);

    /*** copy and nocopy together are contradictory ***/
    fprintf(stderr, "--- expected error output follows (copy + nocopy) ---\n");
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals5,
                    &iptr, &ninfo);
    CHECK("output:copy-nocopy", PRTE_SUCCESS != rc);

    /*** a relative directory is made absolute ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals6,
                    &iptr, &ninfo);
    CHECK("output:dir-rc", PRTE_SUCCESS == rc);
    CHECK("output:dir-key",
          NULL != key_string(iptr, ninfo, PMIX_IOF_OUTPUT_TO_DIRECTORY));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** file and directory at once is refused ***/
    fprintf(stderr, "--- expected error output follows (file + dir) ---\n");
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, vals7,
                    &iptr, &ninfo);
    CHECK("output:file-and-dir", PRTE_SUCCESS != rc);

    /*** display: comma-delimited, and repeated ***/
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dvals1,
                    &iptr, &ninfo);
    CHECK("display:comma-rc", PRTE_SUCCESS == rc);
    CHECK("display:comma-map", has_key(iptr, ninfo, PMIX_DISPLAY_MAP));
    CHECK("display:comma-bind", has_key(iptr, ninfo, PMIX_REPORT_BINDINGS));
    PMIX_INFO_FREE(iptr, ninfo);

    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dvals2,
                    &iptr, &ninfo);
    CHECK("display:multi-rc", PRTE_SUCCESS == rc);
    CHECK("display:multi-map", has_key(iptr, ninfo, PMIX_DISPLAY_MAP));
    CHECK("display:multi-bind", has_key(iptr, ninfo, PMIX_REPORT_BINDINGS));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** display qualifiers ***/
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dvals3,
                    &iptr, &ninfo);
    CHECK("display:qual-rc", PRTE_SUCCESS == rc);
    CHECK("display:qual-map", has_key(iptr, ninfo, PMIX_DISPLAY_MAP));
    CHECK("display:qual-parseable",
          has_key(iptr, ninfo, PMIX_DISPLAY_PARSEABLE_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** "topo=<nodes>" carries its value through ***/
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dvals4,
                    &iptr, &ninfo);
    CHECK("display:topo-rc", PRTE_SUCCESS == rc);
    CHECK("display:topo-value",
          NULL != key_string(iptr, ninfo, PMIX_DISPLAY_TOPOLOGY) &&
          0 == strcmp(key_string(iptr, ninfo, PMIX_DISPLAY_TOPOLOGY),
                      "node1;node2"));
    PMIX_INFO_FREE(iptr, ninfo);

    /*** an unknown display qualifier is an error ***/
    fprintf(stderr, "--- expected error output follows (bad display qualifier) ---\n");
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dvals5,
                    &iptr, &ninfo);
    CHECK("display:bad-qual", PRTE_SUCCESS != rc);

    return failures;
}
