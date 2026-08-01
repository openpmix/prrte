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

#include "src/common/pmix_iof.h"
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
    char *bvals1[] = {"tag=0", NULL};
    char *bvals2[] = {"tag=1,timestamp=false", NULL};
    char *bvals3[] = {"tag=maybe", NULL};
    char *bvals4[] = {"tag", "tag=0", NULL};
    char *bvals5[] = {"file=/tmp/prte-schizo-test:raw=0", NULL};
    char *bvals6[] = {"dir=/tmp/prte-schizo-testdir:copy", NULL};
    char *dbvals1[] = {"map=0,bind", NULL};
    char *dbvals2[] = {"map:parseable=no", NULL};
    char *dbvals3[] = {"map:parseable=perhaps", NULL};

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

    /*** THE VALUE FORM.
     ***
     *** Every boolean directive may be written bare or with a truth value.
     *** "tag=0" has to leave the key ABSENT, because every consumer of
     *** these keys tests them by presence - emitting a false one reads as
     *** ENABLED everywhere.  Before this existed, "--output tag=0" parsed
     *** happily and applied the tag. ***/
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals1,
                    &iptr, &ninfo);
    CHECK("output:false-rc", PRTE_SUCCESS == rc);
    CHECK("output:false-absent", !has_key(iptr, ninfo, PMIX_IOF_TAG_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals2,
                    &iptr, &ninfo);
    CHECK("output:mixed-rc", PRTE_SUCCESS == rc);
    CHECK("output:mixed-tag", has_key(iptr, ninfo, PMIX_IOF_TAG_OUTPUT));
    CHECK("output:mixed-timestamp",
          !has_key(iptr, ninfo, PMIX_IOF_TIMESTAMP_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    /* a value that is neither true nor false is refused, not read as false */
    fprintf(stderr, "--- expected error output follows (non-boolean value) ---\n");
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals3,
                    &iptr, &ninfo);
    CHECK("output:non-boolean", PRTE_SUCCESS != rc);

    /* the whole list has its say before anything is emitted, so the last
     * writing of a directive is the one that counts */
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals4,
                    &iptr, &ninfo);
    CHECK("output:last-wins-rc", PRTE_SUCCESS == rc);
    CHECK("output:last-wins", !has_key(iptr, ninfo, PMIX_IOF_TAG_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    /* the qualifiers take a value too */
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals5,
                    &iptr, &ninfo);
    CHECK("output:raw-false-rc", PRTE_SUCCESS == rc);
    CHECK("output:raw-false-file", has_key(iptr, ninfo, PMIX_IOF_OUTPUT_TO_FILE));
    CHECK("output:raw-false", !has_key(iptr, ninfo, PMIX_IOF_OUTPUT_RAW));
    PMIX_INFO_FREE(iptr, ninfo);

    /* "copy"/"nocopy" says whether the output also reaches the terminal, so
     * it qualifies a directory of files exactly as it qualifies one file -
     * it used to be applied only alongside "file=" */
    rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, bvals6,
                    &iptr, &ninfo);
    CHECK("output:dir-copy-rc", PRTE_SUCCESS == rc);
    val = key_is_true(iptr, ninfo, PMIX_IOF_FILE_ONLY, &found);
    CHECK("output:dir-copy-present", found);
    CHECK("output:dir-copy-false", !val);
    PMIX_INFO_FREE(iptr, ninfo);

    /*** the same, for --display ***/
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dbvals1,
                    &iptr, &ninfo);
    CHECK("display:false-rc", PRTE_SUCCESS == rc);
    CHECK("display:false-map", !has_key(iptr, ninfo, PMIX_DISPLAY_MAP));
    CHECK("display:false-bind", has_key(iptr, ninfo, PMIX_REPORT_BINDINGS));
    PMIX_INFO_FREE(iptr, ninfo);

    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dbvals2,
                    &iptr, &ninfo);
    CHECK("display:qual-false-rc", PRTE_SUCCESS == rc);
    CHECK("display:qual-false-map", has_key(iptr, ninfo, PMIX_DISPLAY_MAP));
    CHECK("display:qual-false",
          !has_key(iptr, ninfo, PMIX_DISPLAY_PARSEABLE_OUTPUT));
    PMIX_INFO_FREE(iptr, ninfo);

    fprintf(stderr, "--- expected error output follows (non-boolean qualifier) ---\n");
    rc = run_parser(prte_schizo_base_parse_display, PRTE_CLI_DISPLAY, dbvals3,
                    &iptr, &ninfo);
    CHECK("display:qual-non-boolean", PRTE_SUCCESS != rc);

#if PRTE_PMIX_IOF_FILE_PATTERN
    /*** the "pattern" qualifier hands the naming of the output files to the
     *** user.  It only means anything alongside a "file=" directive, and the
     *** conversions in it are checked here - while the user is still looking
     *** at the command line they typed - rather than at the moment a daemon
     *** fails to open a file. ***/
    {
        char *pvals1[] = {"file=/tmp/prte-schizo-test-%h/rank-%R:pattern", NULL};
        char *pvals2[] = {"file=/tmp/prte-schizo-test-%q:pattern", NULL};
        char *pvals3[] = {"tag:pattern", NULL};
        char *pvals4[] = {"file=/tmp/prte-schizo-test:pattern:copy", NULL};

        rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, pvals1,
                        &iptr, &ninfo);
        CHECK("output:pattern-rc", PRTE_SUCCESS == rc);
        CHECK("output:pattern-file", has_key(iptr, ninfo, PMIX_IOF_OUTPUT_TO_FILE));
        CHECK("output:pattern-key", has_key(iptr, ninfo, PMIX_IOF_FILE_PATTERN));
        PMIX_INFO_FREE(iptr, ninfo);

        /* an unrecognized conversion is refused up front */
        fprintf(stderr, "--- expected error output follows (bad pattern) ---\n");
        rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, pvals2,
                        &iptr, &ninfo);
        CHECK("output:pattern-bad-conversion", PRTE_SUCCESS != rc);

        /* ...and so is a pattern with nothing to name */
        fprintf(stderr, "--- expected error output follows (pattern, no file) ---\n");
        rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, pvals3,
                        &iptr, &ninfo);
        CHECK("output:pattern-needs-file", PRTE_SUCCESS != rc);

        /* pattern composes with the other qualifiers */
        rc = run_parser(prte_schizo_base_parse_output, PRTE_CLI_OUTPUT, pvals4,
                        &iptr, &ninfo);
        CHECK("output:pattern-with-copy-rc", PRTE_SUCCESS == rc);
        CHECK("output:pattern-with-copy-key", has_key(iptr, ninfo, PMIX_IOF_FILE_PATTERN));
        val = key_is_true(iptr, ninfo, PMIX_IOF_FILE_ONLY, &found);
        CHECK("output:pattern-with-copy-present", found);
        CHECK("output:pattern-with-copy-false", !val);
        PMIX_INFO_FREE(iptr, ninfo);
    }

    /*** the conversions PMIx accepts are the conversions we advertise ***/
    {
        char *bad = NULL;
        CHECK("pattern:all-conversions",
              PMIX_SUCCESS == pmix_iof_check_pattern("%n/%r/%R/%h/%%", &bad));
        CHECK("pattern:no-conversions",
              PMIX_SUCCESS == pmix_iof_check_pattern("plain-name", &bad));
        CHECK("pattern:unknown-conversion",
              PMIX_SUCCESS != pmix_iof_check_pattern("out-%q", &bad));
        CHECK("pattern:names-the-offender",
              NULL != bad && 0 == strcmp(bad, "%q"));
        if (NULL != bad) {
            free(bad);
            bad = NULL;
        }
        /* a trailing '%' has no conversion character at all - report it
         * rather than read past the end of the string */
        CHECK("pattern:trailing-percent",
              PMIX_SUCCESS != pmix_iof_check_pattern("out-%", &bad));
        if (NULL != bad) {
            free(bad);
        }
    }
#endif

    return failures;
}
