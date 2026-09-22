/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Helpers for the option *values* that more than one PRRTE tool has to
 * interpret.  Each of these was originally open-coded in the tool's
 * main() - which is why each of them had a defect that no test could
 * reach.  Keep new value-parsing logic here rather than in a tool.
 */

#include "prte_config.h"
#include "constants.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_string_copy.h"

#include "src/util/prte_cmd_line.h"

int prte_cli_bool_value(const char *value, bool *flag)
{
    pmix_value_t val;
    int rc = PRTE_SUCCESS;

    if (NULL == flag) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* the bare spelling of a boolean directive is its own assertion - the
     * user wrote it, so they want it */
    *flag = true;
    if (NULL == value || '\0' == value[0]) {
        return PRTE_SUCCESS;
    }

    PMIX_VALUE_LOAD(&val, (void *) value, PMIX_STRING);
    if (!PMIX_CHECK_BOOL(&val)) {
        /* something that is neither true nor false was written where only a
         * truth value has meaning.  PMIX_CHECK_TRUE would read it as FALSE,
         * so accepting it would turn "tag=maybe" into "no tags" without a
         * word to the user */
        rc = PRTE_ERR_BAD_PARAM;
    } else {
        *flag = PMIX_CHECK_TRUE(&val);
    }
    PMIX_VALUE_DESTRUCT(&val);
    return rc;
}

int prte_parse_pid_option(const char *value, pid_t *pid, const char **filename)
{
    char *leftover;
    unsigned long ul;
    const char *path;
    FILE *fp;
    int rc;

    if (NULL == value || NULL == pid) {
        return PRTE_ERR_BAD_PARAM;
    }
    *pid = 0;
    if (NULL != filename) {
        *filename = NULL;
    }

    /* an empty value is not an integer, and strtoul would happily
     * report success for it */
    if ('\0' == value[0]) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* see if it is a plain integer */
    errno = 0;
    leftover = NULL;
    ul = strtoul(value, &leftover, 10);
    if (NULL != leftover && '\0' == leftover[0]) {
        if (0 != errno || ul > (unsigned long) INT_MAX) {
            return PRTE_ERR_BAD_PARAM;
        }
        *pid = (pid_t) ul;
        return PRTE_SUCCESS;
    }

    /* the only other accepted form is "file:<path>" */
    if (0 != strncasecmp(value, "file", 4)) {
        return PRTE_ERR_BAD_PARAM;
    }
    path = strchr(value, ':');
    if (NULL == path) {
        return PRTE_ERR_BAD_PARAM;
    }
    ++path;
    if (NULL != filename) {
        *filename = path;
    }
    if ('\0' == path[0]) {
        return PRTE_ERR_BAD_PARAM;
    }

    fp = fopen(path, "r");
    if (NULL == fp) {
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }
    /* read into an unsigned long and then range-check it: scanning
     * "%lu" directly into a pid_t writes sizeof(long) bytes into an
     * int-sized object on every LP64 platform */
    rc = fscanf(fp, "%lu", &ul);
    fclose(fp);
    if (1 != rc || ul > (unsigned long) INT_MAX) {
        return PRTE_ERR_FILE_READ_FAILURE;
    }
    *pid = (pid_t) ul;
    return PRTE_SUCCESS;
}

int prte_load_appfile(const char *filename, char ***argv)
{
    FILE *fp;
    char *line, *p, **split;
    bool first = true;
    int n;

    if (NULL == filename || NULL == argv) {
        return PRTE_ERR_BAD_PARAM;
    }

    fp = fopen(filename, "r");
    if (NULL == fp) {
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }

    while (NULL != (line = pmix_getline(fp))) {
        /* a comment - a line whose first non-blank character is '#' -
         * contributes nothing.  Splitting it instead would hand the parser
         * an app context whose "executable" is the '#' */
        for (p = line; ' ' == *p || '\t' == *p; p++) {
        }
        if ('#' == *p) {
            free(line);
            continue;
        }
        split = PMIx_Argv_split(line, ' ');
        free(line);
        if (NULL == split) {
            /* a blank line separates nothing from nothing - and emitting
             * the delimiter for it would hand the parser an empty app
             * context */
            continue;
        }
        if (!first) {
            /* every line after the first begins a new app context */
            PMIx_Argv_append_nosize(argv, ":");
        }
        for (n = 0; NULL != split[n]; n++) {
            PMIx_Argv_append_nosize(argv, split[n]);
        }
        PMIx_Argv_free(split);
        first = false;
    }
    fclose(fp);

    return PRTE_SUCCESS;
}

int prte_check_appfile_tail(char **tail)
{
    if (NULL == tail || NULL == tail[0]) {
        return PRTE_SUCCESS;
    }
    return PRTE_ERR_BAD_PARAM;
}

bool prte_parse_umask(const char *value, mode_t *mask)
{
    char *endptr;
    unsigned long ul;

    if (NULL == value || NULL == mask) {
        return false;
    }
    /* strtoul() accepts an empty string as zero, and a umask of zero
     * leaves every file the daemon creates world-writable - so an
     * unset-but-present value must be rejected, not obeyed */
    if ('\0' == value[0]) {
        return false;
    }

    errno = 0;
    endptr = NULL;
    ul = strtoul(value, &endptr, 8);
    if (0 != errno || NULL == endptr || '\0' != endptr[0]) {
        return false;
    }
    /* a umask only masks the permission bits */
    if (ul > 0777) {
        return false;
    }
    *mask = (mode_t) ul;
    return true;
}

int prte_parse_uint_option(const char *value, unsigned long limit,
                           unsigned long *result)
{
    char *endptr;
    unsigned long ul;

    if (NULL == value || NULL == result) {
        return PRTE_ERR_BAD_PARAM;
    }
    *result = 0;

    /* A leading digit is what separates a number from everything else a
     * user might type: strtoul() would otherwise accept leading white
     * space and a sign, so "-1" would wrap into a very large value and
     * " 2" would be read as 2 - and an empty string would come back as a
     * perfectly successful zero. */
    if (!isdigit((unsigned char) value[0])) {
        return PRTE_ERR_BAD_PARAM;
    }
    errno = 0;
    endptr = NULL;
    ul = strtoul(value, &endptr, 10);
    if (0 != errno || NULL == endptr || '\0' != endptr[0]) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* the caller names the field the value has to fit: truncating into it
     * silently turns a value the user chose into a different one */
    if (ul > limit) {
        return PRTE_ERR_BAD_PARAM;
    }
    *result = ul;
    return PRTE_SUCCESS;
}

/* read one non-negative rank that has to fill the whole token */
static int xterm_rank(const char *str, uint32_t *rank, long *badrank)
{
    char *endptr;
    long val;

    if ('-' == str[0] && isdigit((unsigned char) str[1])) {
        errno = 0;
        val = strtol(str, &endptr, 10);
        if (0 == errno && '\0' == *endptr) {
            *badrank = val;
            return PRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }
        return PRTE_ERR_BAD_PARAM;
    }
    if (!isdigit((unsigned char) str[0])) {
        return PRTE_ERR_BAD_PARAM;
    }
    errno = 0;
    val = strtol(str, &endptr, 10);
    if (0 != errno || '\0' != *endptr || val >= (long) UINT32_MAX) {
        return PRTE_ERR_BAD_PARAM;
    }
    *rank = (uint32_t) val;
    return PRTE_SUCCESS;
}

int prte_parse_xterm_option(const char *value, prte_rank_range_t **ranges,
                            size_t *nranges, bool *all, bool *hold,
                            long *badrank)
{
    char *input, *dash, **tokens = NULL;
    prte_rank_range_t *out = NULL;
    size_t len, n, cnt;
    int rc = PRTE_SUCCESS;

    if (NULL == value || NULL == ranges || NULL == nranges ||
        NULL == all || NULL == hold || NULL == badrank) {
        return PRTE_ERR_BAD_PARAM;
    }
    *ranges = NULL;
    *nranges = 0;
    *all = false;
    *hold = false;

    input = strdup(value);
    if (NULL == input) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    len = strlen(input);
    if (0 < len && '!' == input[len - 1]) {
        *hold = true;
        input[len - 1] = '\0';
    }
    if ('\0' == input[0]) {
        free(input);
        return PRTE_ERR_BAD_PARAM;
    }
    if (0 == strcasecmp(input, "all") || 0 == strcmp(input, "-1")) {
        *all = true;
        free(input);
        return PRTE_SUCCESS;
    }

    /* PMIx_Argv_split drops empty tokens, which would let "1,,2" or a
     * trailing comma through - refuse those explicitly */
    if (',' == input[0] || ',' == input[strlen(input) - 1] || NULL != strstr(input, ",,")) {
        free(input);
        return PRTE_ERR_BAD_PARAM;
    }
    tokens = PMIx_Argv_split(input, ',');
    cnt = PMIx_Argv_count(tokens);
    out = (prte_rank_range_t *) calloc(cnt, sizeof(prte_rank_range_t));
    if (NULL == out) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto done;
    }
    for (n = 0; n < cnt; n++) {
        /* a leading '-' is a sign, not a range separator */
        dash = strchr(tokens[n] + 1, '-');
        if (NULL == dash) {
            rc = xterm_rank(tokens[n], &out[n].lo, badrank);
            out[n].hi = out[n].lo;
        } else {
            *dash = '\0';
            rc = xterm_rank(tokens[n], &out[n].lo, badrank);
            if (PRTE_SUCCESS == rc) {
                rc = xterm_rank(dash + 1, &out[n].hi, badrank);
            }
            if (PRTE_SUCCESS == rc && out[n].hi < out[n].lo) {
                rc = PRTE_ERR_BAD_PARAM;
            }
        }
        if (PRTE_SUCCESS != rc) {
            goto done;
        }
    }

done:
    PMIx_Argv_free(tokens);
    free(input);
    if (PRTE_SUCCESS != rc) {
        free(out);
        return rc;
    }
    *ranges = out;
    *nranges = cnt;
    return PRTE_SUCCESS;
}

bool prte_xterm_names_rank(const prte_rank_range_t *ranges, size_t nranges,
                           bool all, uint32_t rank)
{
    size_t n;

    if (all) {
        return true;
    }
    for (n = 0; n < nranges; n++) {
        if (ranges[n].lo <= rank && rank <= ranges[n].hi) {
            return true;
        }
    }
    return false;
}
