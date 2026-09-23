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
#include "src/util/prte_show_help.h"

#define NOVAL  PMIX_CLI_VALUE_NONE
#define OPTVAL PMIX_CLI_VALUE_OPTIONAL
#define REQVAL PMIX_CLI_VALUE_REQUIRED

const pmix_cli_choice_t prte_cli_mappers[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_SLOT, PRTE_MAPPER_SLOT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_HWT, PRTE_MAPPER_HWT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CORE, PRTE_MAPPER_CORE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L1CACHE, PRTE_MAPPER_L1CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L2CACHE, PRTE_MAPPER_L2CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L3CACHE, PRTE_MAPPER_L3CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NUMA, PRTE_MAPPER_NUMA, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PACKAGE, PRTE_MAPPER_PACKAGE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NODE, PRTE_MAPPER_NODE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_SEQ, PRTE_MAPPER_SEQ, NOVAL),
    /* its N and object follow as ':'-delimited fields, not as a value */
    PMIX_CLI_CHOICE(PRTE_CLI_PPR, PRTE_MAPPER_PPR, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_RANKFILE, PRTE_MAPPER_RANKFILE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PELIST, PRTE_MAPPER_PELIST, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_DEVICE, PRTE_MAPPER_DEVICE, REQVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_mapquals[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_PE, PRTE_MAPQUAL_PE, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_SPAN, PRTE_MAPQUAL_SPAN, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_OVERSUB, PRTE_MAPQUAL_OVERSUB, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOOVER, PRTE_MAPQUAL_NOOVER, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOLOCAL, PRTE_MAPQUAL_NOLOCAL, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_HWTCPUS, PRTE_MAPQUAL_HWTCPUS, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CORECPUS, PRTE_MAPQUAL_CORECPUS, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_INHERIT, PRTE_MAPQUAL_INHERIT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOINHERIT, PRTE_MAPQUAL_NOINHERIT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_QFILE, PRTE_MAPQUAL_FILE, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_ORDERED, PRTE_MAPQUAL_ORDERED, NOVAL),
    /* the level to interleave across; the package when not given */
    PMIX_CLI_CHOICE(PRTE_CLI_INTERLEAVE, PRTE_MAPQUAL_INTERLEAVE, OPTVAL),
    /* a truth value; true when not given */
    PMIX_CLI_CHOICE(PRTE_CLI_SHARED, PRTE_MAPQUAL_SHARED, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NDEV, PRTE_MAPQUAL_NDEV, REQVAL),
    PMIX_CLI_CHOICE_END
};

/* The objects a ppr pattern may count per.  "thread", "skt", "socket" and
 * "nm" are the older spellings, still accepted. */
const pmix_cli_choice_t prte_cli_ppr_objects[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_NODE, PRTE_PPROBJ_NODE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_HWT, PRTE_PPROBJ_HWT, NOVAL),
    PMIX_CLI_CHOICE("thread", PRTE_PPROBJ_HWT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CORE, PRTE_PPROBJ_CORE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PACKAGE, PRTE_PPROBJ_PACKAGE, NOVAL),
    PMIX_CLI_CHOICE("socket", PRTE_PPROBJ_PACKAGE, NOVAL),
    PMIX_CLI_CHOICE("skt", PRTE_PPROBJ_PACKAGE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NUMA, PRTE_PPROBJ_NUMA, NOVAL),
    PMIX_CLI_CHOICE("nm", PRTE_PPROBJ_NUMA, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L1CACHE, PRTE_PPROBJ_L1CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L2CACHE, PRTE_PPROBJ_L2CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L3CACHE, PRTE_PPROBJ_L3CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_DEVICE, PRTE_PPROBJ_DEVICE, REQVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_rankers[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_SLOT, PRTE_RANKER_SLOT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NODE, PRTE_RANKER_NODE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_FILL, PRTE_RANKER_FILL, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_SPAN, PRTE_RANKER_SPAN, NOVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_binders[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_NONE, PRTE_BINDER_NONE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_HWT, PRTE_BINDER_HWT, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CORE, PRTE_BINDER_CORE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L1CACHE, PRTE_BINDER_L1CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L2CACHE, PRTE_BINDER_L2CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_L3CACHE, PRTE_BINDER_L3CACHE, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NUMA, PRTE_BINDER_NUMA, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PACKAGE, PRTE_BINDER_PACKAGE, NOVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_bindquals[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_OVERLOAD, PRTE_BINDQUAL_OVERLOAD, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOOVERLOAD, PRTE_BINDQUAL_NOOVERLOAD, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_IF_SUPP, PRTE_BINDQUAL_IF_SUPP, NOVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_LIMIT, PRTE_BINDQUAL_LIMIT, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_REPORT, PRTE_BINDQUAL_REPORT, NOVAL),
    PMIX_CLI_CHOICE_END
};

/* Every boolean directive of --output, --display and --rtos may be written
 * bare or with a truth value (see prte_cli_bool_value()), so the value is
 * optional for all of them. */
const pmix_cli_choice_t prte_cli_output_directives[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_TAG, PRTE_OUTPUT_TAG, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_TAG_DET, PRTE_OUTPUT_TAG_DET, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_TAG_FULL, PRTE_OUTPUT_TAG_FULL, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_RANK, PRTE_OUTPUT_RANK, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_TIMESTAMP, PRTE_OUTPUT_TIMESTAMP, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_XML, PRTE_OUTPUT_XML, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_MERGE_ERROUT, PRTE_OUTPUT_MERGE_ERROUT, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_DIR, PRTE_OUTPUT_DIR, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_FILE, PRTE_OUTPUT_FILE, REQVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_output_quals[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_COPY, PRTE_OUTQUAL_COPY, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOCOPY, PRTE_OUTQUAL_NOCOPY, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_RAW, PRTE_OUTQUAL_RAW, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PATTERN, PRTE_OUTQUAL_PATTERN, OPTVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_display_directives[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_ALLOC, PRTE_DISPLAY_ALLOC, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_MAP, PRTE_DISPLAY_MAP, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_BIND, PRTE_DISPLAY_BIND, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_MAPDEV, PRTE_DISPLAY_MAPDEV, OPTVAL),
    /* the nodes to show; every node when not given */
    PMIX_CLI_CHOICE(PRTE_CLI_TOPO, PRTE_DISPLAY_TOPO, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CPUS, PRTE_DISPLAY_CPUS, OPTVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_display_quals[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_PARSEABLE, PRTE_DISPQUAL_PARSEABLE, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PARSABLE, PRTE_DISPQUAL_PARSEABLE, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_PHYSICAL_CPUS, PRTE_DISPQUAL_PHYSICAL, OPTVAL),
    PMIX_CLI_CHOICE_END
};

const pmix_cli_choice_t prte_cli_rtos_directives[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_ERROR_NZ, PRTE_RTOS_ERROR_NZ, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOLAUNCH, PRTE_RTOS_NOLAUNCH, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOSPAWN, PRTE_RTOS_NOSPAWN, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_SHOW_PROGRESS, PRTE_RTOS_SHOW_PROGRESS, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_RECOVERABLE, PRTE_RTOS_RECOVERABLE, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_AUTORESTART, PRTE_RTOS_AUTORESTART, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_CONTINUOUS, PRTE_RTOS_CONTINUOUS, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_MAX_RESTARTS, PRTE_RTOS_MAX_RESTARTS, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_EXEC_AGENT, PRTE_RTOS_EXEC_AGENT, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_DEFAULT_EXEC_AGENT, PRTE_RTOS_DEFAULT_EXEC_AGENT, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_STOP_ON_EXEC, PRTE_RTOS_STOP_ON_EXEC, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_STOP_IN_INIT, PRTE_RTOS_STOP_IN_INIT, OPTVAL),
    /* a truth value, or the name of a breakpoint */
    PMIX_CLI_CHOICE(PRTE_CLI_STOP_IN_APP, PRTE_RTOS_STOP_IN_APP, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_TIMEOUT, PRTE_RTOS_TIMEOUT, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_SPAWN_TIMEOUT, PRTE_RTOS_SPAWN_TIMEOUT, REQVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_REPORT_STATE, PRTE_RTOS_REPORT_STATE, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_STACK_TRACES, PRTE_RTOS_STACK_TRACES, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_REPORT_CHILD_SEP, PRTE_RTOS_REPORT_CHILD_SEP, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_AGG_HELP, PRTE_RTOS_AGG_HELP, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_NOTIFY_ERRORS, PRTE_RTOS_NOTIFY_ERRORS, OPTVAL),
    /* where to write it; stdout when not given */
    PMIX_CLI_CHOICE(PRTE_CLI_OUTPUT_PROCTABLE, PRTE_RTOS_OUTPUT_PROCTABLE, OPTVAL),
    PMIX_CLI_CHOICE(PRTE_CLI_FWD_ENVIRON, PRTE_RTOS_FWD_ENVIRON, OPTVAL),
    PMIX_CLI_CHOICE_END
};

const char *prte_cli_name(const pmix_cli_choice_t *choices, int tag)
{
    size_t n;

    for (n = 0; NULL != choices[n].name; n++) {
        if (tag == choices[n].tag) {
            return choices[n].name;
        }
    }
    return NULL;
}

int prte_cli_match(const pmix_nspace_t nspace, const char *option,
                   const char *input, const pmix_cli_choice_t *choices,
                   int *tag)
{
    const pmix_cli_choice_t *entry;
    char *list, *name;
    size_t len, n;

    switch (pmix_cli_match(input, choices, tag)) {
        case PMIX_CLI_MATCH_FOUND:
            return PRTE_SUCCESS;
        case PMIX_CLI_MATCH_NONE:
            return PRTE_ERR_NOT_FOUND;
        case PMIX_CLI_MATCH_AMBIGUOUS:
            list = pmix_cli_match_list(input, choices, ',');
            prte_show_help(nspace, "help-prte-util.txt", "cli-ambiguous", true,
                           option, input, (NULL == list) ? "" : list);
            free(list);
            return PRTE_ERR_SILENT;
        default:
            break;
    }

    /* a value error: name the entry by its full spelling, which the user
     * may have abbreviated */
    entry = NULL;
    for (n = 0; NULL != choices[n].name; n++) {
        if (*tag == choices[n].tag) {
            entry = &choices[n];
            break;
        }
    }
    name = NULL;
    if (NULL != entry) {
        len = strcspn(entry->name, "=");
        name = (char *) malloc(len + 1);
        if (NULL != name) {
            memcpy(name, entry->name, len);
            name[len] = '\0';
        }
    }
    if (NULL != entry && PMIX_CLI_VALUE_NONE == entry->value) {
        prte_show_help(nspace, "help-prte-util.txt", "cli-unexpected-value", true,
                       option, (NULL == name) ? input : name, input);
    } else {
        prte_show_help(nspace, "help-prte-util.txt", "cli-missing-value", true,
                       option, (NULL == name) ? input : name, input,
                       (NULL == name) ? input : name);
    }
    free(name);
    return PRTE_ERR_SILENT;
}

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
