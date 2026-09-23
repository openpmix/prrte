/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"

#include "src/common/pmix_iof.h"
#include "src/mca/schizo/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */

#include "src/mca/schizo/base/static-components.h"

/*
 * Global variables
 */
prte_schizo_base_t prte_schizo_base = {
    .active_modules = PMIX_LIST_STATIC_INIT(prte_schizo_base.active_modules),
    .test_proxy_launch = false,
    .default_display_options = NULL,
    .default_runtime_options = NULL,
    .default_output_options = NULL,
    .default_personality = NULL
};

static int prte_schizo_base_register(pmix_mca_base_register_flag_t flags)
{
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(flags);

    /* test proxy launch */
    prte_schizo_base.test_proxy_launch = false;
    (void)pmix_mca_base_var_register("prte", "schizo", "base", "test_proxy_launch",
                                     "Test proxy launches",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                     &prte_schizo_base.test_proxy_launch);
    prte_schizo_base.default_personality = NULL;
    ret = pmix_mca_base_var_register("prte", NULL, NULL, "personality",
                                     "Default personality to use",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_schizo_base.default_personality);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", NULL, "schizo", "proxy",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_schizo_base.default_display_options = NULL;
    (void)pmix_mca_base_var_register("prte", NULL, NULL, "display",
                                     "Comma-delimited list of values about the job and/or allocation "
                                     "that are to be displayed. Supported values include: [allocation | bindings "
                                     "| map | map-devel | topo[=semi-colon delimited list of nodes whose topology is to be displayed] "
                                     "| cpus[=semi-colon delimited list of nodes whose cpus are to be displayed], with supported colon-delimited "
                                     "modifiers: [parseable | physical]. For more details, see \"prterun --help display\". "
                                     "The full directive need not be provided — "
                                     "only enough characters are required to uniquely identify the "
                                     "directive. For example, \"ALL\" is sufficient to represent "
                                     "the \"ALLOCATION\" directive — while \"MAP\" can not be used "
                                     "to represent \"MAP-DEVEL\" (though \"MAP-D\" would suffice).",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_schizo_base.default_display_options);

    prte_schizo_base.default_output_options = NULL;
    (void)pmix_mca_base_var_register("prte", NULL, NULL, "output",
                                     "Comma-delimited list of case-insensitive options that control how "
                                     "output is generated. The full directive need not be provided — only "
                                     "enough characters are required to uniquely identify the directive. For "
                                     "example, \"MERGE\" is sufficient to represent the \"MERGE-STDERR-TO-STDOUT\" "
                                     "directive — while \"TAG\" can not be used to represent \"TAG-DETAILED\" "
                                     "(though \"TAG-D\" would suffice). Supported values include: [tag | "
                                     "tag-detailed | tag-fullname | timestamp | xml | dir=[dirname] | "
                                     "file=[filename]. Supported qualifiers include [nocopy | raw]",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_schizo_base.default_output_options);

    prte_schizo_base.default_runtime_options = NULL;
    ret = pmix_mca_base_var_register("prte", NULL, NULL, "rtos",
                                     "Comma-delimited list of options specifying desired behavior of "
                                     "the runtime itself. Supported values include: [error-nonzero-status, donotlaunch, "
                                     "show-progress, notifyerrors, recoverable, autorestart, continuous, max-restarts, "
                                     "exec-agent, default-exec-agent, output-proctable, stop-on-exec, stop-in-init, "
                                     "stop-in-app, timeout, spawn-timeout, report-state-on-timeout, get-stack-traces, "
                                     "report-child-jobs-seperately, aggregate-help-messages, fwd-environment]. "
                                     "For more details, see \"prterun --help runtime-options\". "
                                     "The full directive need not be provided — "
                                     "only enough characters are required to uniquely identify the "
                                     "directive. For example, \"donot\" is sufficient to represent "
                                     "the \"donotlaunch\" directive — while \"STOP\" can not be used "
                                     "to represent \"STOP-ON-EXEC\" (though \"STOP-ON-E\" would suffice).",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_schizo_base.default_runtime_options);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", NULL, NULL, "runtime_options",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    return PRTE_SUCCESS;
}

static int prte_schizo_base_close(void)
{
    /* cleanup globals */
    PMIX_LIST_DESTRUCT(&prte_schizo_base.active_modules);

    return pmix_mca_base_framework_components_close(&prte_schizo_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_schizo_base_open(pmix_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PMIX_CONSTRUCT(&prte_schizo_base.active_modules, pmix_list_t);

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&prte_schizo_base_framework, flags);

    /* All done */
    return rc;
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(schizo, "PRTE Schizo Subsystem", prte_schizo_base_register,
                                prte_schizo_base_open, prte_schizo_base_close,
                                prte_schizo_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

void prte_schizo_base_expose(char *param, char *prefix)
{
    char *value, *pm;

    value = strchr(param, '=');
    if (NULL == value) {
        /* pmix_cmd_line_parse always hands us "name=value" for an MCA option,
         * so this cannot happen from the CLI path - but the function is
         * exported, and a bare name has nothing to expose */
        return;
    }
    *value = '\0';
    ++value;
    pmix_asprintf(&pm, "%s%s", prefix, param);
    setenv(pm, value, true);
    free(pm);
    --value;
    *value = '=';
}

/* An option as a user writes it, for a message that has to name it: the
 * keys are the options' internal spellings, and nobody types "--mapby". */
static const char *option_label(const char *key)
{
    static const struct {
        const char *key;
        const char *label;
    } labels[] = {
        {PRTE_CLI_MAPBY, "--map-by"},
        {PRTE_CLI_RANKBY, "--rank-by"},
        {PRTE_CLI_BINDTO, "--bind-to"},
        {PRTE_CLI_OUTPUT, "--output"},
        {PRTE_CLI_DISPLAY, "--display"},
        {PRTE_CLI_RTOS, "--rtos"},
        {NULL, NULL}
    };
    size_t n;

    for (n = 0; NULL != labels[n].key; n++) {
        if (0 == strcmp(key, labels[n].key)) {
            return labels[n].label;
        }
    }
    return key;
}

bool prte_schizo_base_check_qualifiers(const char *directive,
                                       const pmix_cli_choice_t *valid,
                                       char *qual)
{
    int rc, tag;
    char *v;

    /* An option that accepts no qualifiers at all is checked with a NULL
     * table - "--runtime-options" is - so every qualifier given is
     * unrecognized.  Walking it anyway indexed the NULL: "--rtos :anything"
     * crashed the tool. */
    if (NULL != valid) {
        rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, option_label(directive), qual, valid, &tag);
        if (PRTE_SUCCESS == rc) {
            return true;
        }
        if (PRTE_ERR_NOT_FOUND != rc) {
            /* ambiguous, or a value it should or should not have had -
             * already explained */
            return false;
        }
    }
    v = (NULL == valid) ? NULL : pmix_cli_match_list(NULL, valid, ',');
    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                   "unrecognized-qualifier", true,
                   directive, qual, (NULL == v) ? "" : v);
    free(v);
    return false;
}

bool prte_schizo_base_check_directives(const char *directive,
                                       const pmix_cli_choice_t *valid,
                                       const pmix_cli_choice_t *quals,
                                       char *dir)
{
    size_t m;
    int rc, tag;
    char **args, **qls, *v, *q;

    /* An option written with no value, or with one that is nothing but
     * delimiters, names no directive.  It has to be refused here rather
     * than split: PMIx_Argv_split() hands back NULL - not an empty array -
     * for a string that is empty or yields no non-empty token, and every
     * walk below indexes what it returns.  "--map-by=", "--map-by=:" and
     * "--output=" all reached this function and dereferenced that NULL. */
    if (NULL == dir || '\0' == dir[0]) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt",
                       "empty-directive", true, directive, directive);
        return false;
    }

    /* if it starts with a ':', then these are just qualifiers */
    if (':' == dir[0]) {
        qls = PMIx_Argv_split(&dir[1], ':');
        if (NULL == qls) {
            /* nothing followed the ':' but more of them */
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt",
                           "empty-directive", true, directive, directive);
            return false;
        }
        for (m=0; NULL != qls[m]; m++) {
            if (!prte_schizo_base_check_qualifiers(directive, quals, qls[m])) {
                PMIx_Argv_free(qls);
                return false;
            }
        }
        PMIx_Argv_free(qls);
        return true;
    }

    /* always accept the "help" directive */
    if (0 == strcasecmp(dir, "help") ||
        0 == strcasecmp(dir, "-help") ||
        0 == strcasecmp(dir, "--help")) {
        return true;
    }

    /* An option without qualifiers is not split: nothing in its value is a
     * qualifier, and the value of one of its directives may itself contain
     * a ':' ("timeout=1:30:00"). */
    if (NULL == quals) {
        args = NULL;
        if (PMIX_SUCCESS != PMIx_Argv_append_nosize(&args, dir)) {
            return false;
        }
    } else {
        args = PMIx_Argv_split(dir, ':');
    }
    if (NULL == args) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt",
                       "empty-directive", true, directive, directive);
        return false;
    }

    /* The directive, with any value it carries - the vocabulary says
     * whether it may have one, and a value given to a directive that takes
     * none is refused rather than dropped. */
    rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, option_label(directive), args[0], valid, &tag);
    if (PRTE_ERR_NOT_FOUND == rc) {
        v = pmix_cli_match_list(NULL, valid, ':');
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                       "unrecognized-directive", true,
                       directive, dir, (NULL == v) ? "" : v);
        free(v);
        PMIx_Argv_free(args);
        return false;
    } else if (PRTE_SUCCESS != rc) {
        PMIx_Argv_free(args);
        return false;
    }

    if (NULL == args[1] || NULL == quals) {
        PMIx_Argv_free(args);
        return true;
    }

    if (0 == strcmp(directive, PRTE_CLI_MAPBY) && PRTE_MAPPER_PPR == tag) {
        /* unfortunately, this is a special case that must be checked
         * separately due to the format of the qualifier */
        if (3 > PMIx_Argv_count(args)) {
            /* this is an error as there must be at least the "ppr"
             * directive, a number, and then the resource type. There may
             * also be additional qualifiers given, so the count could be
             * greater than 3 - but it has to at least contain those three
             * fields */
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                           "invalid-pattern", true,
                           dir);
            PMIx_Argv_free(args);
            return false;
        }
        v = NULL;
        /* only the end pointer matters here - the count itself is the
         * mapper's business, not this validator's */
        (void) strtoul(args[1], &v, 10);
        if (NULL != v && 0 < strlen(v)) {
            /* the first entry had to be a pure number */
            pmix_asprintf(&v, "ppr:[Number of procs/object]:%s", args[2]);
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                           "unrecognized-qualifier", true,
                           directive, dir, v);
            free(v);
            PMIx_Argv_free(args);
            return false;
        }
        /* the object - held to the same vocabulary the mapper reads it
         * with, so the two cannot disagree about what is one */
        rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, option_label(directive), args[2],
                            prte_cli_ppr_objects, &tag);
        if (PRTE_ERR_NOT_FOUND == rc) {
            v = pmix_cli_match_list(NULL, prte_cli_ppr_objects, ':');
            pmix_asprintf(&q, "ppr:%s:[%s]", args[1], (NULL == v) ? "" : v);
            free(v);
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                           "unrecognized-qualifier", true,
                           directive, dir, q);
            free(q);
            PMIx_Argv_free(args);
            return false;
        } else if (PRTE_SUCCESS != rc) {
            PMIx_Argv_free(args);
            return false;
        }
        m = 3;
    } else {
        m = 1;
    }
    for (; NULL != args[m]; m++) {
        if (!prte_schizo_base_check_qualifiers(directive, quals, args[m])) {
            PMIx_Argv_free(args);
            return false;
        }
    }
    PMIx_Argv_free(args);
    return true;
}

typedef struct {
    const char *alias;
    const char *name;
} prte_synonym_t;

static prte_synonym_t synonyms[] = {
    {.alias = PRTE_CLI_MACHINEFILE, .name = PRTE_CLI_HOSTFILE},
    {.alias = PRTE_CLI_WD, .name = PRTE_CLI_WDIR},
    {.alias = NULL, .name = NULL}
};

static const char* check_synonym(const char *alias)
{
    int n;

    for (n=0; NULL != synonyms[n].alias; n++) {
        if (0 == strcmp(alias, synonyms[n].alias)) {
            return synonyms[n].name;
        }
    }
    return NULL;
}

static char *limits[] = {
    PRTE_CLI_PATH,
    PRTE_CLI_WDIR,
    PRTE_CLI_PSET,
    PRTE_CLI_NP,
    PRTE_CLI_KEEPALIVE,
    NULL
};

/* The options in limits[] carry exactly one value apiece.  Repeating one on
 * a command line does not override the earlier value - pmix_cmd_line_parse
 * appends every occurrence to the same instance's value array, and every
 * consumer of these keys reads values[0] - so the second and subsequent
 * values are silently discarded.  Reject that rather than guess which one
 * the user meant.  MPMD is not affected: prte_parse_locals() splits the
 * command line at each ':' and parses/sanity-checks one app segment at a
 * time, so a per-app --np/--wdir/--path is a single value in its own
 * result set. */
static int check_ndirs(pmix_cli_item_t *opt)
{
    int n, count;
    char *param;

    for (n=0; NULL != limits[n]; n++) {
        if (0 == strcmp(opt->key, limits[n])) {
            count = PMIx_Argv_count(opt->values);
            if (1 < count) {
                param = PMIx_Argv_join(opt->values, ' ');
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "too-many-instances", true,
                               param, opt->key, count, 1);
                free(param);
                return PRTE_ERR_SILENT;
            }
        }
    }
    return PRTE_SUCCESS;
}

/* Every other directive of "--output", "--display" and "--rtos" is a
 * BOOLEAN: written bare to assert it, or with a truth value to say so
 * explicitly.  These are the ones that carry a value of their own instead,
 * so "timeout=60" and "timeout=30" are two different answers rather than
 * two ways of saying "true".  (Their vocabularies are in
 * src/util/prte_cmd_line.c, shared with the parsers that act on them.) */
static const char *valued_directives[] = {
    PRTE_CLI_DIR,
    PRTE_CLI_FILE,
    PRTE_CLI_TOPO,
    PRTE_CLI_CPUS,
    PRTE_CLI_MAX_RESTARTS,
    PRTE_CLI_EXEC_AGENT,
    PRTE_CLI_TIMEOUT,
    PRTE_CLI_SPAWN_TIMEOUT,
    PRTE_CLI_OUTPUT_PROCTABLE,
    NULL
};

/* Is this - a directive's full name, as a vocabulary spells it - one that
 * carries a value?  Compared in full, not as an abbreviation: the callers
 * hold the canonical name, and "t" is not "timeout". */
bool prte_schizo_base_directive_is_valued(const char *directive)
{
    size_t n, len;

    if (NULL == directive) {
        return false;
    }
    len = strcspn(directive, "=");
    for (n = 0; NULL != valued_directives[n]; n++) {
        if (len == strcspn(valued_directives[n], "=") &&
            0 == strncasecmp(directive, valued_directives[n], len)) {
            return true;
        }
    }
    return false;
}

/* The vocabularies of the three JOB-LEVEL options, which an MPMD command
 * line may write in any app segment - prte_schizo_base_hoist_job_option()
 * has to recognize the directives it is collecting from those segments in
 * order to tell a repeat from a contradiction. */
static bool option_vocabulary(const char *key, const pmix_cli_choice_t **dirs,
                              const pmix_cli_choice_t **quals)
{
    if (0 == strcmp(key, PRTE_CLI_OUTPUT)) {
        *dirs = prte_cli_output_directives;
        *quals = prte_cli_output_quals;
        return true;
    }
    if (0 == strcmp(key, PRTE_CLI_DISPLAY)) {
        *dirs = prte_cli_display_directives;
        *quals = prte_cli_display_quals;
        return true;
    }
    if (0 == strcmp(key, PRTE_CLI_RTOS)) {
        *dirs = prte_cli_rtos_directives;
        *quals = NULL;
        return true;
    }
    return false;
}

/* One directive or qualifier, as one app segment wrote it. */
typedef struct {
    const char *name;   /* canonical spelling - borrowed from a table above */
    char *value;        /* the text after its '=' - NULL for the bare form */
    char *token;        /* as the user wrote it, for the error message */
    bool invert;        /* this spelling is the negation of "name" */
} prte_hoist_item_t;

/* Directives that ask ONE question under two names.  Comparing the names
 * alone would let "copy" in one app segment and "nocopy" in another pass as
 * two unrelated requests, when they are opposite answers to the same
 * question.  (Two spellings of the SAME answer - "parseable" and
 * "parsable" - share a tag in their vocabulary, and so already resolve to
 * one name.) */
static struct {
    const char *spelling;
    const char *question;
    bool invert;
} directive_aliases[] = {
    {PRTE_CLI_NOCOPY, PRTE_CLI_COPY, true},
    {NULL, NULL, false}
};

/*
 * Name the question a token asks, or NULL for a token that belongs to
 * neither vocabulary, or fits more than one word of it - the sanity checker
 * reports those, and it reports them better than this can.
 */
static const char *hoist_canonical(char *token, const pmix_cli_choice_t *dirs,
                                   const pmix_cli_choice_t *quals, bool *invert)
{
    const char *found = NULL;
    int tag;
    size_t n;

    *invert = false;
    if (NULL != dirs && PMIX_CLI_MATCH_FOUND == pmix_cli_match(token, dirs, &tag)) {
        found = prte_cli_name(dirs, tag);
    } else if (NULL != quals && PMIX_CLI_MATCH_FOUND == pmix_cli_match(token, quals, &tag)) {
        found = prte_cli_name(quals, tag);
    }
    if (NULL == found) {
        return NULL;
    }
    for (n = 0; NULL != directive_aliases[n].spelling; n++) {
        if (0 == strcmp(found, directive_aliases[n].spelling)) {
            *invert = directive_aliases[n].invert;
            return directive_aliases[n].question;
        }
    }
    return found;
}

/* Do two writings of the same directive ask for the same thing? */
static bool hoist_agree(prte_hoist_item_t *a, prte_hoist_item_t *b)
{
    bool aflag, bflag;

    if (prte_schizo_base_directive_is_valued(a->name)) {
        /* the value IS the answer - "timeout=60" and "timeout=30" are two
         * different ones, however alike they look to a truth test */
        if (NULL == a->value || NULL == b->value) {
            return (NULL == a->value && NULL == b->value);
        }
        return (0 == strcmp(a->value, b->value));
    }
    /* a boolean: the bare form and "=1" are the same answer, and a value
     * that is neither true nor false is refused where it is parsed - here
     * it can only be compared literally */
    if (PRTE_SUCCESS != prte_cli_bool_value(a->value, &aflag) ||
        PRTE_SUCCESS != prte_cli_bool_value(b->value, &bflag)) {
        return (NULL != a->value && NULL != b->value &&
                0 == strcasecmp(a->value, b->value));
    }
    if (a->invert) {
        aflag = !aflag;
    }
    if (b->invert) {
        bflag = !bflag;
    }
    return (aflag == bflag);
}

int prte_schizo_base_hoist_job_option(pmix_cli_result_t *results,
                                      const char *key, char **contributions)
{
    const pmix_cli_choice_t *dirs = NULL, *quals = NULL;
    char **toks = NULL, **parts = NULL;
    prte_hoist_item_t *items = NULL;
    size_t nitems = 0, alloc = 0, i, j;
    pmix_cli_item_t *opt;
    const char *name;
    char *merged;
    bool invert;
    int n, m, p, rc = PRTE_SUCCESS;

    if (NULL == contributions || NULL == contributions[0]) {
        /* no app segment wrote this option - whatever the global parse
         * already holds is the whole of it */
        return PRTE_SUCCESS;
    }
    if (!option_vocabulary(key, &dirs, &quals)) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (n = 0; NULL != contributions[n]; n++) {
        toks = PMIx_Argv_split(contributions[n], ',');
        for (m = 0; NULL != toks[m]; m++) {
            /* a directive and its qualifiers are ':'-delimited, and each of
             * them answers a question of its own */
            parts = PMIx_Argv_split(toks[m], ':');
            for (p = 0; NULL != parts[p]; p++) {
                name = hoist_canonical(parts[p], dirs, quals, &invert);
                if (NULL == name) {
                    continue;
                }
                if (nitems == alloc) {
                    prte_hoist_item_t *tmp;
                    alloc += 16;
                    tmp = (prte_hoist_item_t *) realloc(items,
                                                        alloc * sizeof(prte_hoist_item_t));
                    if (NULL == tmp) {
                        PMIx_Argv_free(parts);
                        PMIx_Argv_free(toks);
                        rc = PRTE_ERR_OUT_OF_RESOURCE;
                        goto cleanup;
                    }
                    items = tmp;
                }
                items[nitems].name = name;
                items[nitems].token = strdup(parts[p]);
                items[nitems].value = PMIX_CLI_QUALIFIER_VALUE(items[nitems].token);
                items[nitems].invert = invert;
                ++nitems;
            }
            PMIx_Argv_free(parts);
            parts = NULL;
        }
        PMIx_Argv_free(toks);
        toks = NULL;
    }

    for (i = 0; i < nitems; i++) {
        for (j = i + 1; j < nitems; j++) {
            /* compare by content: the canonical names come from the
             * vocabulary tables and from the alias table, and nothing
             * guarantees two identical string literals share an address */
            if (0 != strcmp(items[i].name, items[j].name)) {
                continue;
            }
            if (hoist_agree(&items[i], &items[j])) {
                continue;
            }
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "conflicting-job-directives", true,
                           key, items[i].name, items[i].token, items[j].token);
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }
    }

    /* they agree, so the job's directive list is all of them.  It replaces
     * whatever the global parse recorded rather than adding to it: that
     * parse stops at the first app, so everything it saw is the first app
     * segment's contribution and is already in this list. */
    merged = PMIx_Argv_join(contributions, ',');
    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt) {
        opt = PMIX_NEW(pmix_cli_item_t);
        if (NULL == opt) {
            free(merged);
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto cleanup;
        }
        opt->key = strdup(key);
        pmix_list_append(&results->instances, &opt->super);
    } else {
        PMIx_Argv_free(opt->values);
        opt->values = NULL;
    }
    PMIx_Argv_append_nosize(&opt->values, merged);
    free(merged);

cleanup:
    for (i = 0; i < nitems; i++) {
        free(items[i].token);
    }
    if (NULL != items) {
        free(items);
    }
    return rc;
}

/* The value a single-valued option was given, or NULL when it was recorded
 * with none.  An option can reach the sanity checker with an empty value
 * array - a synonym is appended by presence alone - and with an empty
 * string for a value, since "--map-by=" parses perfectly well.  Neither is
 * a directive; the directive checker refuses both, and refuses them with
 * the same message wherever they appear. */
static char *first_value(pmix_cli_item_t *opt)
{
    if (NULL == opt->values || NULL == opt->values[0]) {
        return NULL;
    }
    return opt->values[0];
}

/* Check every directive of an option that carries a ','-delimited list of
 * them.
 *
 * Two things this walks that the original did not.  It walks EVERY value,
 * not just values[0]: "--output" may be repeated, and each repetition is
 * appended to the same instance, so checking the first one left every later
 * one to be validated by nothing.  And it frees the split on the refusal
 * path as well as the accepting one - it used to be abandoned there,
 * leaking a copy of the whole list for each such option. */
static int check_directive_list(char *option, const pmix_cli_choice_t *valid,
                                const pmix_cli_choice_t *quals, pmix_cli_item_t *opt)
{
    char **vtmp;
    int n, v, rc = PRTE_SUCCESS;

    if (NULL == opt->values || NULL == opt->values[0]) {
        /* the option was given no directive at all - let the checker be
         * the one that says so */
        prte_schizo_base_check_directives(option, valid, quals, NULL);
        return PRTE_ERR_SILENT;
    }
    for (v = 0; NULL != opt->values[v]; v++) {
        vtmp = PMIx_Argv_split(opt->values[v], ',');
        if (NULL == vtmp) {
            prte_schizo_base_check_directives(option, valid, quals, NULL);
            return PRTE_ERR_SILENT;
        }
        for (n = 0; NULL != vtmp[n]; n++) {
            if (!prte_schizo_base_check_directives(option, valid, quals, vtmp[n])) {
                rc = PRTE_ERR_SILENT;
                break;
            }
        }
        PMIx_Argv_free(vtmp);
        if (PRTE_SUCCESS != rc) {
            break;
        }
    }
    return rc;
}

/* the sanity checker is provided for DEVELOPERS as it checks that
 * the options contained in the cmd line being passed to PRRTE for
 * execution meet PRRTE requirements. Although it does emit
 * show_help messages, it really isn't intended for USERS - any
 * problems in translating user cmd lines to PRRTE internal
 * structs should be worked out by the developers */
int prte_schizo_base_sanity(pmix_cli_result_t *cmd_line)
{
    pmix_cli_item_t *opt, *newopt;
    int n, rc;
    const char *tgt;
    char **tmp;
    bool haspe = false;

    /* --rank-by takes no qualifiers, and says so if given one - unlike
     * --rtos, whose values are not split into qualifiers at all */
    static const pmix_cli_choice_t no_quals[] = {
        PMIX_CLI_CHOICE_END
    };
    int tag;

    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_MAPBY)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "multi-instances", true, PRTE_CLI_MAPBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_RANKBY)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "multi-instances", true, PRTE_CLI_RANKBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_BINDTO)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "multi-instances", true, PRTE_CLI_BINDTO);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_DISPLAY)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "multi-instances", true, PRTE_CLI_DISPLAY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_RTOS)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "multi-instances", true, PRTE_CLI_RTOS);
        return PRTE_ERR_SILENT;
    }

    /* check for synonyms */
    PMIX_LIST_FOREACH(opt, &cmd_line->instances, pmix_cli_item_t) {
        if (NULL != (tgt = check_synonym(opt->key))) {
            if (NULL == opt->values) {
                // the presence is adequate
                if (NULL == pmix_cmd_line_get_param(cmd_line, tgt)) {
                    newopt = PMIX_NEW(pmix_cli_item_t);
                    newopt->key = strdup(tgt);
                    pmix_list_append(&cmd_line->instances, &newopt->super);
                }
            } else {
                for (n=0; NULL != opt->values[n]; n++) {
                    rc = prte_schizo_base_add_directive(cmd_line, opt->key, tgt,
                                                        opt->values[n], false);
                    if (PRTE_SUCCESS != rc) {
                        return rc;
                    }
                }
            }
        }
    }

    /* quick check that we have valid directives */
    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_MAPBY, prte_cli_mappers, prte_cli_mapquals,
                                               first_value(opt))) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_RANKBY, prte_cli_rankers, no_quals,
                                               first_value(opt))) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_BINDTO, prte_cli_binders, prte_cli_bindquals,
                                               first_value(opt))) {
            return PRTE_ERR_SILENT;
        }
    }

    /* the following have multiple directives */
    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        rc = check_directive_list(PRTE_CLI_OUTPUT, prte_cli_output_directives, prte_cli_output_quals, opt);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        rc = check_directive_list(PRTE_CLI_DISPLAY, prte_cli_display_directives, prte_cli_display_quals, opt);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_RTOS);
    if (NULL != opt) {
        rc = check_directive_list(PRTE_CLI_RTOS, prte_cli_rtos_directives, NULL, opt);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    // check too many values given to a single command line option
    PMIX_LIST_FOREACH(opt, &cmd_line->instances, pmix_cli_item_t) {
        rc = check_ndirs(opt);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    // check for map-by - bind-to conflicts
    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_MAPBY);
    newopt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_BINDTO);
    if (NULL != opt && NULL != newopt) {
        /* Asking for specific cpus and then binding to something coarser is
         * a conflict. Find that request by parsing the directive, not by
         * searching the whole --map-by value for the letters "PE": that
         * matched any spelling containing them, so "device=openfabrics" or
         * a rankfile under /home/pete were refused as PE requests. Split
         * off the directive, which is what "pe-list=" is, and test the
         * qualifiers, which is what "PE=n" is. */
        tmp = PMIx_Argv_split(first_value(opt), ':');
        if (NULL != tmp) {
            /* a value that opens with ':' has no policy word - its first
             * field is already a qualifier */
            n = 0;
            if (':' != first_value(opt)[0]) {
                if (PMIX_CLI_MATCH_FOUND == pmix_cli_match(tmp[0], prte_cli_mappers, &tag) &&
                    PRTE_MAPPER_PELIST == tag) {
                    haspe = true;
                }
                n = 1;
            }
            for (; !haspe && NULL != tmp[n]; n++) {
                if (PMIX_CLI_MATCH_FOUND == pmix_cli_match(tmp[n], prte_cli_mapquals, &tag) &&
                    PRTE_MAPQUAL_PE == tag) {
                    haspe = true;
                }
            }
            PMIx_Argv_free(tmp);
        }
        if (haspe) {
            char *bnd = first_value(newopt);
            bool pebind = false;

            /* Binding to a cpu is what makes a PE request coherent, and the
             * BINDING DIRECTIVE is what says so - not the letters "core" or
             * "hwt" appearing anywhere in the value.  That is the same
             * lesson as the --map-by scan just above, applied to the other
             * half of the same test; it also drops the tree's only use of
             * strcasestr(), which is a GNU/BSD extension present in neither
             * POSIX nor C11 and guarded by no configure check here. */
            tmp = PMIx_Argv_split(bnd, ':');
            if (NULL != tmp && NULL != bnd && ':' != bnd[0] &&
                PMIX_CLI_MATCH_FOUND == pmix_cli_match(tmp[0], prte_cli_binders, &tag) &&
                (PRTE_BINDER_CORE == tag || PRTE_BINDER_HWT == tag)) {
                pebind = true;
            }
            PMIx_Argv_free(tmp);
            if (pebind) {
                /* if we are binding to a PE, then there is no conflict */
                return PRTE_SUCCESS;
            }
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "binding-pe-conflict", true,
                           first_value(opt), (NULL == bnd) ? "" : bnd);
            return PRTE_ERR_SILENT;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * The boolean directives and qualifiers of one option, and the PMIx key
 * each of them turns into.
 *
 * They are recorded here as the directive list is walked and emitted once
 * it has been walked in full, rather than emitted where they are matched.
 * That is what makes the value form work: "tag=0" has to leave the key
 * ABSENT - every consumer of these keys tests them by presence - so a
 * directive cannot be emitted before the whole list has had its say.  It
 * also makes a repeat harmless, which matters now that a directive list can
 * be assembled from several app segments of one MPMD command line.
 */
typedef struct {
    const char *directive;
    const char *key;
    bool given;
    bool flag;
} prte_bool_directive_t;

/*
 * Record the truth of one boolean directive, named by its full spelling -
 * the caller has already matched what the user wrote against the option's
 * vocabulary, which is where an abbreviation is resolved (or refused as
 * ambiguous).
 *
 * Returns PRTE_ERR_NOT_FOUND if the name is no boolean directive in this
 * table.  On a malformed value it reports the error itself and returns
 * PRTE_ERR_SILENT.
 */
static int set_bool_directive(prte_bool_directive_t *tbl, const char *option,
                              const char *name, const char *value)
{
    size_t n;
    bool flag;

    for (n = 0; NULL != name && NULL != tbl[n].directive; n++) {
        if (0 != strcmp(name, tbl[n].directive)) {
            continue;
        }
        if (PRTE_SUCCESS != prte_cli_bool_value(value, &flag)) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "non-boolean-value", true,
                           option, tbl[n].directive, value);
            return PRTE_ERR_SILENT;
        }
        tbl[n].given = true;
        tbl[n].flag = flag;
        return PRTE_SUCCESS;
    }
    return PRTE_ERR_NOT_FOUND;
}

/* Emit the keys of every boolean directive that was given as true.
 *
 * Two entries may share a key - "parseable" and "parsable" are one
 * question spelled two ways - so a key already emitted is skipped rather
 * than added twice. */
static int emit_bool_directives(prte_bool_directive_t *tbl, void *jinfo)
{
    pmix_status_t ret;
    size_t n, m;
    bool dup;

    for (n = 0; NULL != tbl[n].directive; n++) {
        if (!tbl[n].given || !tbl[n].flag) {
            continue;
        }
        dup = false;
        for (m = 0; m < n; m++) {
            if (tbl[m].given && tbl[m].flag &&
                0 == strcmp(tbl[m].key, tbl[n].key)) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        PMIX_INFO_LIST_ADD(ret, jinfo, tbl[n].key, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            return prte_pmix_convert_status(ret);
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_parse_display(pmix_cli_item_t *opt, void *jinfo)
{
    int n, idx, rc, tag;
    size_t m;
    pmix_status_t ret;
    char **targv = NULL, *ptr, *cptr, **quals = NULL;
    char *topo = NULL, *cpus = NULL, *valid;
    bool topogiven = false, cpusgiven = false;
    prte_bool_directive_t bools[] = {
        {PRTE_CLI_ALLOC, PMIX_DISPLAY_ALLOCATION, false, false},
        {PRTE_CLI_MAP, PMIX_DISPLAY_MAP, false, false},
        {PRTE_CLI_MAPDEV, PMIX_DISPLAY_MAP_DETAILED, false, false},
        {PRTE_CLI_BIND, PMIX_REPORT_BINDINGS, false, false},
        {NULL, NULL, false, false}
    };
    prte_bool_directive_t qualtbl[] = {
        {PRTE_CLI_PARSEABLE, PMIX_DISPLAY_PARSEABLE_OUTPUT, false, false},
        {PRTE_CLI_PHYSICAL_CPUS, PMIX_REPORT_PHYSICAL_CPUS, false, false},
        {NULL, NULL, false, false}
    };

    for (n=0; NULL != opt->values[n]; n++) {
        targv = PMIx_Argv_split(opt->values[n], ',');
        for (idx = 0; NULL != targv && NULL != targv[idx]; idx++) {
            /* check for qualifiers */
            cptr = strchr(targv[idx], ':');
            if (NULL != cptr) {
                *cptr = '\0';
                ++cptr;
                quals = PMIx_Argv_split(cptr, ':');
                /* check qualifiers */
                for (m=0; NULL != quals && NULL != quals[m]; m++) {
                    rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--display", quals[m],
                                        prte_cli_display_quals, &tag);
                    if (PRTE_ERR_NOT_FOUND == rc) {
                        valid = pmix_cli_match_list(NULL, prte_cli_display_quals, ',');
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-qualifier", true,
                                       PRTE_CLI_DISPLAY, quals[m], (NULL == valid) ? "" : valid);
                        free(valid);
                        rc = PRTE_ERR_SILENT;
                        goto cleanup;
                    } else if (PRTE_SUCCESS != rc) {
                        goto cleanup;
                    }
                    rc = set_bool_directive(qualtbl, PRTE_CLI_DISPLAY,
                                            prte_cli_name(prte_cli_display_quals, tag),
                                            PMIX_CLI_QUALIFIER_VALUE(quals[m]));
                    if (PRTE_SUCCESS != rc) {
                        goto cleanup;
                    }
                }
                PMIx_Argv_free(quals);
                quals = NULL;
            }
            if ('\0' == targv[idx][0]) {
                // only qualifiers were given
                continue;
            }

            /* Refuse what we do not understand rather than drop it.  The
             * sanity checker validates a directive list that came from a
             * command line, but the "prte_display" MCA param reaches this
             * parser without passing through it - so a misspelling there
             * used to be honored as silence: the job ran, displayed
             * nothing, and exited 0. */
            rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--display", targv[idx],
                                prte_cli_display_directives, &tag);
            if (PRTE_ERR_NOT_FOUND == rc) {
                valid = pmix_cli_match_list(NULL, prte_cli_display_directives, ':');
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                               "unrecognized-directive", true,
                               PRTE_CLI_DISPLAY, targv[idx], (NULL == valid) ? "" : valid);
                free(valid);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            } else if (PRTE_SUCCESS != rc) {
                goto cleanup;
            }
            ptr = PMIX_CLI_QUALIFIER_VALUE(targv[idx]);

            if (PRTE_DISPLAY_TOPO == tag) {
                /* "topo" with no value means "every node", which is a
                 * legitimate request - so record that it was asked for
                 * separately from the value it may have carried */
                if (NULL != topo) {
                    free(topo);
                }
                topo = (NULL == ptr) ? NULL : strdup(ptr);
                topogiven = true;

            } else if (PRTE_DISPLAY_CPUS == tag) {
                if (NULL != cpus) {
                    free(cpus);
                }
                cpus = (NULL == ptr) ? NULL : strdup(ptr);
                cpusgiven = true;

            } else {
                rc = set_bool_directive(bools, PRTE_CLI_DISPLAY,
                                        prte_cli_name(prte_cli_display_directives, tag), ptr);
                if (PRTE_SUCCESS != rc) {
                    goto cleanup;
                }
            }
        }
        PMIx_Argv_free(targv);
        targv = NULL;
    }

    /* the whole list has now been seen, so what it asked for is known - see
     * the comment on prte_bool_directive_t for why nothing above emitted a
     * key of its own */
    rc = emit_bool_directives(bools, jinfo);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }
    rc = emit_bool_directives(qualtbl, jinfo);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }
    if (topogiven) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DISPLAY_TOPOLOGY, topo, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            goto cleanup;
        }
    }
    if (cpusgiven) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DISPLAY_PROCESSORS, cpus, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            goto cleanup;
        }
    }
    rc = PRTE_SUCCESS;

cleanup:
    PMIx_Argv_free(quals);
    PMIx_Argv_free(targv);
    if (NULL != topo) {
        free(topo);
    }
    if (NULL != cpus) {
        free(cpus);
    }
    return rc;
}

int prte_schizo_base_parse_output(pmix_cli_item_t *opt, void *jinfo)
{
    char *outdir=NULL;
    char *outfile=NULL;
    char *outfileraw=NULL;
    char **targv = NULL, *ptr, *cptr, **options = NULL, *valid;
    int m, n, idx, rc, tag;
    pmix_status_t ret;
    bool fileonly = true;
    bool copyqualgiven = false;
    bool patternqualgiven = false;
    bool flag;
    prte_bool_directive_t bools[] = {
        {PRTE_CLI_TAG, PMIX_IOF_TAG_OUTPUT, false, false},
        {PRTE_CLI_TAG_DET, PMIX_IOF_TAG_DETAILED_OUTPUT, false, false},
        {PRTE_CLI_TAG_FULL, PMIX_IOF_TAG_FULLNAME_OUTPUT, false, false},
        {PRTE_CLI_RANK, PMIX_IOF_RANK_OUTPUT, false, false},
        {PRTE_CLI_TIMESTAMP, PMIX_IOF_TIMESTAMP_OUTPUT, false, false},
        {PRTE_CLI_XML, PMIX_IOF_XML_OUTPUT, false, false},
        {PRTE_CLI_MERGE_ERROUT, PMIX_IOF_MERGE_STDERR_STDOUT, false, false},
        {NULL, NULL, false, false}
    };
    prte_bool_directive_t qualtbl[] = {
        {PRTE_CLI_RAW, PMIX_IOF_OUTPUT_RAW, false, false},
        {NULL, NULL, false, false}
    };

    for (n=0; NULL != opt->values[n]; n++) {
        /* every value on this option is a directive list of its own - the
         * user may repeat "--output" as well as comma-delimit within one
         * instance, and both spellings have to be honored */
        targv = PMIx_Argv_split(opt->values[n], ',');
        for (idx = 0; NULL != targv && NULL != targv[idx]; idx++) {
            /* check for qualifiers */
            cptr = strchr(targv[idx], ':');
            if (NULL != cptr) {
                *cptr = '\0';
                ++cptr;
                /* could be multiple qualifiers, and a qualifier is separated
                 * from the directive - and from its fellows - by a ':'.  The
                 * ',' that separates directives has already been consumed
                 * above, so splitting on ',' here would hand the whole
                 * ':'-joined run to the matcher as one token, and every
                 * qualifier after the first would be lost */
                options = PMIx_Argv_split(cptr, ':');
                for (m=0; NULL != options && NULL != options[m]; m++) {
                    /* see the directive below - the same refusal, for a
                     * qualifier */
                    rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--output", options[m],
                                        prte_cli_output_quals, &tag);
                    if (PRTE_ERR_NOT_FOUND == rc) {
                        valid = pmix_cli_match_list(NULL, prte_cli_output_quals, ':');
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       PRTE_CLI_OUTPUT, options[m], (NULL == valid) ? "" : valid);
                        free(valid);
                        rc = PRTE_ERR_SILENT;
                        goto cleanup;
                    } else if (PRTE_SUCCESS != rc) {
                        goto cleanup;
                    }
                    ptr = PMIX_CLI_QUALIFIER_VALUE(options[m]);

                    if (PRTE_OUTQUAL_RAW == tag) {
                        rc = set_bool_directive(qualtbl, PRTE_CLI_OUTPUT, PRTE_CLI_RAW, ptr);
                        if (PRTE_SUCCESS != rc) {
                            goto cleanup;
                        }
                        continue;
                    }

                    if (PRTE_SUCCESS != prte_cli_bool_value(ptr, &flag)) {
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-base.txt", "non-boolean-value", true,
                                       PRTE_CLI_OUTPUT, prte_cli_name(prte_cli_output_quals, tag), ptr);
                        rc = PRTE_ERR_SILENT;
                        goto cleanup;
                    }
                    if (PRTE_OUTQUAL_NOCOPY == tag || PRTE_OUTQUAL_COPY == tag) {
                        if (copyqualgiven) {
                            // cannot give both copy and nocopy
                            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-output.txt", "copy-nocopy", true, cptr);
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
                        fileonly = (PRTE_OUTQUAL_NOCOPY == tag) ? flag : !flag;
                        copyqualgiven = true;

                    } else if (PRTE_OUTQUAL_PATTERN == tag) {
#if PRTE_PMIX_IOF_FILE_PATTERN
                        /* only record it here - the key is emitted with the
                         * filename it qualifies, since it means nothing
                         * without one */
                        patternqualgiven = flag;
#else
                        /* expanding the pattern is PMIx's job, and this one
                         * cannot do it - say so rather than accept a
                         * qualifier that would be silently ignored */
                        if (flag) {
                            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-output.txt", "pattern-unsupported", true);
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
#endif
                    }
                }
                PMIx_Argv_free(options);
                options = NULL;
            }
            if (0 == strlen(targv[idx])) {
                // only qualifiers were given
                continue;
            }

            /* Refuse what we do not understand rather than drop it.  The
             * sanity checker validates a directive list that came from a
             * command line, but the "prte_output" MCA param reaches this
             * parser without passing through it - so a misspelling there
             * used to be honored as silence: the job ran with no tagging,
             * no file, and an exit status of 0, while the identical word
             * written on the command line was reported. */
            rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--output", targv[idx],
                                prte_cli_output_directives, &tag);
            if (PRTE_ERR_NOT_FOUND == rc) {
                valid = pmix_cli_match_list(NULL, prte_cli_output_directives, ':');
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt",
                               "unrecognized-directive", true,
                               PRTE_CLI_OUTPUT, targv[idx], (NULL == valid) ? "" : valid);
                free(valid);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            } else if (PRTE_SUCCESS != rc) {
                goto cleanup;
            }
            /* the directory and the filename are required by the
             * vocabulary to carry their value, so a NULL here is a boolean
             * directive given bare */
            ptr = PMIX_CLI_QUALIFIER_VALUE(targv[idx]);

            if (PRTE_OUTPUT_DIR == tag) {
                if (NULL != outfile) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prted.txt", "both-file-and-dir-set", true, outfile, ptr);
                    rc = PRTE_ERR_FATAL;
                    goto cleanup;
                }
                /* If the given filename isn't an absolute path, then
                 * convert it to one so the name will be relative to
                 * the directory where prun was given as that is what
                 * the user will have seen */
                if (NULL != outdir) {
                    free(outdir);
                    outdir = NULL;
                }
                if (!pmix_path_is_absolute(ptr)) {
                    char cwd[PRTE_PATH_MAX];
                    if (NULL == getcwd(cwd, sizeof(cwd))) {
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                    outdir = pmix_os_path(false, cwd, ptr, NULL);
                } else {
                    outdir = strdup(ptr);
                }

            } else if (PRTE_OUTPUT_FILE == tag) {
                if (NULL != outdir) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prted.txt", "both-file-and-dir-set", true, ptr, outdir);
                    rc = PRTE_ERR_FATAL;
                    goto cleanup;
                }
                /* If the given filename isn't an absolute path, then
                 * convert it to one so the name will be relative to
                 * the directory where prun was given as that is what
                 * the user will have seen */
                if (NULL != outfile) {
                    free(outfile);
                    outfile = NULL;
                    free(outfileraw);
                }
                outfileraw = strdup(ptr);
                if (!pmix_path_is_absolute(ptr)) {
                    char cwd[PRTE_PATH_MAX];
                    if (NULL == getcwd(cwd, sizeof(cwd))) {
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                    outfile = pmix_os_path(false, cwd, ptr, NULL);
                } else {
                    outfile = strdup(ptr);
                }

            } else {
                rc = set_bool_directive(bools, PRTE_CLI_OUTPUT,
                                        prte_cli_name(prte_cli_output_directives, tag), ptr);
                if (PRTE_SUCCESS != rc) {
                    goto cleanup;
                }
            }
        }
        PMIx_Argv_free(targv);
        targv = NULL;
    }
    if (patternqualgiven && NULL == outfile) {
        /* "pattern" says how to name an output FILE.  Given without one it
         * has nothing to qualify, and silently ignoring it would leave the
         * user believing they had asked for something */
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-output.txt", "pattern-needs-file", true);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }

    /* the whole list has now been seen, so what it asked for is known - see
     * the comment on prte_bool_directive_t for why nothing above emitted a
     * key of its own */
    rc = emit_bool_directives(bools, jinfo);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }
    rc = emit_bool_directives(qualtbl, jinfo);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }

    if (NULL != outdir) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_DIRECTORY, outdir, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            goto cleanup;
        }
    }
    if (NULL != outfile) {
#if PRTE_PMIX_IOF_FILE_PATTERN
        if (patternqualgiven) {
            /* the name is a pattern the user composed, so check its
             * conversions now - while they are still looking at the
             * command line they typed.  PMIx owns the expansion, so
             * it owns the definition of what is valid; asking it
             * here is what keeps the two answers the same. */
            char *badconv = NULL;
            ret = pmix_iof_check_pattern(outfile, &badconv);
            if (PMIX_SUCCESS != ret) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-schizo-output.txt", "bad-pattern", true,
                               outfileraw, (NULL == badconv) ? "(none)" : badconv);
                if (NULL != badconv) {
                    free(badconv);
                }
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            }
        }
#endif
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_FILE, outfile, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            goto cleanup;
        }
        if (patternqualgiven) {
            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_PATTERN, NULL, PMIX_BOOL);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                rc = prte_pmix_convert_status(ret);
                goto cleanup;
            }
        }
    }
    /* "copy"/"nocopy" says whether the output ALSO reaches the terminal, so
     * it qualifies a directory of output files exactly as it qualifies a
     * single one - it used to be applied only alongside "file=", which left
     * "--output dir=X:copy" asking for something it silently did not get */
    if (copyqualgiven && (NULL != outfile || NULL != outdir)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_ONLY, &fileonly, PMIX_BOOL);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            goto cleanup;
        }
    }
    rc = PRTE_SUCCESS;

cleanup:
    PMIx_Argv_free(options);
    PMIx_Argv_free(targv);
    if (NULL != outdir) {
        free(outdir);
    }
    if (NULL != outfile) {
        free(outfile);
    }
    if (NULL != outfileraw) {
        free(outfileraw);
    }

    return rc;
}

PMIX_CLASS_INSTANCE(prte_schizo_base_active_module_t,
                    pmix_list_item_t, NULL, NULL);
