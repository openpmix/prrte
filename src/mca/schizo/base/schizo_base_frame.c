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
    .active_modules = PMIX_LIST_STATIC_INIT,
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

PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, schizo, "PRTE Schizo Subsystem", prte_schizo_base_register,
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

bool prte_schizo_base_check_qualifiers(char *directive,
                                       char **valid,
                                       char *qual)
{
    size_t n;
    char *v;

    for (n=0; NULL != valid[n]; n++) {
        if (PMIX_CHECK_CLI_OPTION(qual, valid[n])) {
            return true;
        }
    }
    v = PMIx_Argv_join(valid, ',');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-qualifier", true,
                   directive, qual, v);
    free(v);
    return false;
}

bool prte_schizo_base_check_directives(char *directive,
                                       char **valid,
                                       char **quals,
                                       char *dir)
{
    size_t n, m;
    char **args, **qls, *v, *q;
    char *pproptions[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        "socket",  // dealt with elsewhere
        "skt",     // dealt with elsewhere
        PRTE_CLI_NODE,
        NULL
    };
    bool found;

    /* if it starts with a ':', then these are just qualifiers */
    if (':' == dir[0]) {
        qls = PMIx_Argv_split(&dir[1], ':');
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

    args = PMIx_Argv_split(dir, ':');
    /* remove any '=' in the directive */
    if (NULL != (v = strchr(args[0], '='))) {
        *v = '\0';
    }
    for (n = 0; NULL != valid[n]; n++) {
        if (PMIX_CHECK_CLI_OPTION(args[0], valid[n])) {
            /* valid directive - check any qualifiers */
            if (NULL != args[1] && NULL != quals) {
                if (0 == strcmp(directive, PRTE_CLI_MAPBY) &&
                    0 == strcmp(args[0], PRTE_CLI_PPR)) {
                    /* unfortunately, this is a special case that
                     * must be checked separately due to the format
                     * of the qualifier */
                    if (3 > PMIx_Argv_count(args)) {
                        /* this is an error as there must be at least
                         * the "ppr" directive, a number, and then the
                         * resource type. There may also be additional
                         * qualifiers given, so the count could be greater
                         * than 3 - but it has to at least contain
                         * those three fields */
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "invalid-pattern", true,
                                       dir);
                        PMIx_Argv_free(args);
                        return false;
                    }
                    v = NULL;
                    m = strtoul(args[1], &v, 10);
                    if (NULL != v && 0 < strlen(v)) {
                        /* the first entry had to be a pure number */
                        pmix_asprintf(&v, "ppr:[Number of procs/object]:%s", args[2]);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, v);
                        free(v);
                        PMIx_Argv_free(args);
                        return false;
                    }
                    found = false;
                    for (m=0; NULL != pproptions[m]; m++) {
                        if (0 == strncasecmp(args[2], pproptions[m], strlen(args[2]))) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        v = PMIx_Argv_join(pproptions, ':');
                        pmix_asprintf(&q, "ppr:%s:[%s]", args[1], v);
                        free(v);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, q);
                        free(q);
                        PMIx_Argv_free(args);
                        return false;
                    }
                    if (NULL != args[3]) {
                        qls = PMIx_Argv_split(args[3], ':');
                    } else {
                        PMIx_Argv_free(args);
                        return true;
                    }
                } else {
                    qls = PMIx_Argv_split(args[1], ':');
                }
               for (m=0; NULL != qls[m]; m++) {
                    if (!prte_schizo_base_check_qualifiers(directive, quals, qls[m])) {
                        PMIx_Argv_free(qls);
                        PMIx_Argv_free(args);
                        return false;
                    }
                }
                PMIx_Argv_free(qls);
                PMIx_Argv_free(args);
                return true;
            }
            PMIx_Argv_free(args);
            return true;
        }
    }
    v = PMIx_Argv_join(valid, ':');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-directive", true,
                   directive, dir, v);
    PMIx_Argv_free(args);
    return false;
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
                prte_show_help("help-schizo-base.txt", "too-many-instances", true,
                               param, opt->key, count, 1);
                free(param);
                return PRTE_ERR_SILENT;
            }
        }
    }
    return PRTE_SUCCESS;
}

/*
 * The vocabularies of the three JOB-LEVEL options.
 *
 * These sit at file scope, and not inside the sanity checker with the
 * mapping vocabularies, because a second reader needs them: an MPMD command
 * line may write "--output"/"--display"/"--rtos" in any app segment, and
 * prte_schizo_base_hoist_job_option() has to recognize the directives it is
 * collecting from those segments in order to tell a repeat from a
 * contradiction.  One copy means the two readers cannot drift.
 */
static char *output_directives[] = {
    PRTE_CLI_TAG,
    PRTE_CLI_TAG_DET,
    PRTE_CLI_TAG_FULL,
    PRTE_CLI_RANK,
    PRTE_CLI_TIMESTAMP,
    PRTE_CLI_XML,
    PRTE_CLI_MERGE_ERROUT,
    PRTE_CLI_DIR,
    PRTE_CLI_FILE,
    NULL
};
static char *output_quals[] = {
    PRTE_CLI_COPY,
    PRTE_CLI_NOCOPY,
    PRTE_CLI_RAW,
    PRTE_CLI_PATTERN,
    NULL
};

static char *display_directives[] = {
    PRTE_CLI_ALLOC,
    PRTE_CLI_MAP,
    PRTE_CLI_BIND,
    PRTE_CLI_MAPDEV,
    PRTE_CLI_TOPO,
    PRTE_CLI_CPUS,
    NULL
};
static char *display_quals[] = {
    PRTE_CLI_PARSEABLE,
    PRTE_CLI_PARSABLE,
    PRTE_CLI_PHYSICAL_CPUS,
    NULL
};

static char *rto_directives[] = {
    PRTE_CLI_ERROR_NZ,
    PRTE_CLI_NOLAUNCH,
    PRTE_CLI_NOSPAWN,
    PRTE_CLI_SHOW_PROGRESS,
    PRTE_CLI_RECOVERABLE,
    PRTE_CLI_AUTORESTART,
    PRTE_CLI_CONTINUOUS,
    PRTE_CLI_MAX_RESTARTS,
    PRTE_CLI_EXEC_AGENT,
    PRTE_CLI_DEFAULT_EXEC_AGENT,
    PRTE_CLI_STOP_ON_EXEC,
    PRTE_CLI_STOP_IN_INIT,
    PRTE_CLI_STOP_IN_APP,
    PRTE_CLI_TIMEOUT,
    PRTE_CLI_SPAWN_TIMEOUT,
    PRTE_CLI_REPORT_STATE,
    PRTE_CLI_STACK_TRACES,
    PRTE_CLI_REPORT_CHILD_SEP,
    PRTE_CLI_AGG_HELP,
    PRTE_CLI_NOTIFY_ERRORS,
    PRTE_CLI_OUTPUT_PROCTABLE,
    PRTE_CLI_FWD_ENVIRON,
    NULL
};

/* Every other directive of those three options is a BOOLEAN: written bare
 * to assert it, or with a truth value to say so explicitly.  These are the
 * ones that carry a value of their own instead, so "timeout=60" and
 * "timeout=30" are two different answers rather than two ways of saying
 * "true". */
static char *valued_directives[] = {
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

bool prte_schizo_base_directive_is_valued(const char *directive)
{
    size_t n;

    for (n = 0; NULL != valued_directives[n]; n++) {
        if (PMIX_CHECK_CLI_OPTION((char *) directive, valued_directives[n])) {
            return true;
        }
    }
    return false;
}

static bool option_vocabulary(const char *key, char ***dirs, char ***quals)
{
    if (0 == strcmp(key, PRTE_CLI_OUTPUT)) {
        *dirs = output_directives;
        *quals = output_quals;
        return true;
    }
    if (0 == strcmp(key, PRTE_CLI_DISPLAY)) {
        *dirs = display_directives;
        *quals = display_quals;
        return true;
    }
    if (0 == strcmp(key, PRTE_CLI_RTOS)) {
        *dirs = rto_directives;
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
 * question - and "parseable"/"parsable" as two requests for the same thing,
 * which is merely untidy but would defeat the check for the pair given with
 * opposite truths. */
static struct {
    const char *spelling;
    const char *question;
    bool invert;
} directive_aliases[] = {
    {PRTE_CLI_NOCOPY, PRTE_CLI_COPY, true},
    {PRTE_CLI_PARSABLE, PRTE_CLI_PARSEABLE, false},
    {NULL, NULL, false}
};

/*
 * Name the question a token asks, or NULL for a token that belongs to
 * neither vocabulary - the sanity checker reports those, and it reports
 * them better than this can.
 */
static const char *hoist_canonical(char *token, char **dirs, char **quals,
                                   bool *invert)
{
    const char *found = NULL;
    size_t n;

    *invert = false;
    if (NULL != dirs) {
        for (n = 0; NULL == found && NULL != dirs[n]; n++) {
            if (PMIX_CHECK_CLI_OPTION(token, dirs[n])) {
                found = dirs[n];
            }
        }
    }
    if (NULL == found && NULL != quals) {
        for (n = 0; NULL == found && NULL != quals[n]; n++) {
            if (PMIX_CHECK_CLI_OPTION(token, quals[n])) {
                found = quals[n];
            }
        }
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
    char **dirs = NULL, **quals = NULL;
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
            prte_show_help("help-schizo-base.txt", "conflicting-job-directives", true,
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
    char **vtmp;

    char *mappers[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        PRTE_CLI_NODE,
        PRTE_CLI_SEQ,
        PRTE_CLI_PPR,
        PRTE_CLI_RANKFILE,
        PRTE_CLI_PELIST,
        NULL
    };
    char *mapquals[] = {
        PRTE_CLI_PE,
        PRTE_CLI_SPAN,
        PRTE_CLI_OVERSUB,
        PRTE_CLI_NOOVER,
        PRTE_CLI_NOLOCAL,
        PRTE_CLI_HWTCPUS,
        PRTE_CLI_CORECPUS,
        PRTE_CLI_INHERIT,
        PRTE_CLI_NOINHERIT,
        PRTE_CLI_QFILE,
        PRTE_CLI_ORDERED,
        NULL
    };

    char *rankers[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_NODE,
        PRTE_CLI_FILL,
        PRTE_CLI_SPAN,
        NULL
    };
    char *rkquals[] = {
        NULL
    };

    char *binders[] = {
        PRTE_CLI_NONE,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        NULL
    };
    /* This list is the front door: a qualifier missing from it is refused
     * here and never reaches the parser that implements it. REPORT was
     * missing, so the job-level arm for it in
     * prte_hwloc_base_set_binding_policy() was unreachable from any command
     * line - including the show_help that arm produces at DVM scope, which
     * tells the user "you can provide this modifier on a per-job basis".
     * Keep this in step with both parsers (src/hwloc/hwloc.c for the job,
     * rmaps_base_set_app_binding_policy() below for an app). */
    char *bndquals[] = {
        PRTE_CLI_OVERLOAD,
        PRTE_CLI_NOOVERLOAD,
        PRTE_CLI_IF_SUPP,
        PRTE_CLI_LIMIT,
        PRTE_CLI_REPORT,
        NULL
    };

    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_MAPBY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_MAPBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_RANKBY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_RANKBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_BINDTO)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_BINDTO);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_DISPLAY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_DISPLAY);
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, PRTE_CLI_RTOS)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_RTOS);
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
        if (!prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_RANKBY, rankers, rkquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_BINDTO, binders, bndquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
    }

    /* the following have multiple directives */
    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        vtmp = PMIx_Argv_split(opt->values[0], ',');
        for (n=0; NULL != vtmp[n]; n++) {
            if (!prte_schizo_base_check_directives(PRTE_CLI_OUTPUT, output_directives, output_quals, vtmp[n])) {
                return PRTE_ERR_SILENT;
            }
        }
        PMIx_Argv_free(vtmp);
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        vtmp = PMIx_Argv_split(opt->values[0], ',');
        for (n=0; NULL != vtmp[n]; n++) {
            if (!prte_schizo_base_check_directives(PRTE_CLI_DISPLAY, display_directives, display_quals, vtmp[n])) {
                return PRTE_ERR_SILENT;
            }
        }
        PMIx_Argv_free(vtmp);
    }

    opt = pmix_cmd_line_get_param(cmd_line, PRTE_CLI_RTOS);
    if (NULL != opt) {
        vtmp = PMIx_Argv_split(opt->values[0], ',');
        for (n=0; NULL != vtmp[n]; n++) {
            if (!prte_schizo_base_check_directives(PRTE_CLI_RTOS, rto_directives, NULL, vtmp[n])) {
                return PRTE_ERR_SILENT;
            }
        }
        PMIx_Argv_free(vtmp);
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
        if (NULL != strcasestr(opt->values[0], "PE")) {
            /* if we are binding to a PE, then there is no conflict */
            if (NULL != strcasestr(newopt->values[0], "core") ||
                NULL != strcasestr(newopt->values[0], "hwt")) {
                return PRTE_SUCCESS;
            }
            prte_show_help("help-schizo-base.txt", "binding-pe-conflict", true,
                           opt->values[0], newopt->values[0]);
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
 * Record the truth of one boolean directive.
 *
 * The table is searched in order because the matcher accepts any
 * unambiguous prefix and "tag" is a prefix of "tag-detailed": whichever
 * entry comes first is what a bare "tag" means, so the order here is the
 * user-facing definition of the abbreviations and must not be shuffled.
 *
 * Returns false if the token names no boolean directive in this table -
 * the caller then tries the directives that carry a real value.  On a
 * malformed value it reports the error itself and hands back *rc.
 */
static bool set_bool_directive(prte_bool_directive_t *tbl, const char *option,
                               char *token, const char *value, int *rc)
{
    size_t n;
    bool flag;

    *rc = PRTE_SUCCESS;
    for (n = 0; NULL != tbl[n].directive; n++) {
        if (!PMIX_CHECK_CLI_OPTION(token, (char *) tbl[n].directive)) {
            continue;
        }
        if (PRTE_SUCCESS != prte_cli_bool_value(value, &flag)) {
            prte_show_help("help-schizo-base.txt", "non-boolean-value", true,
                           option, tbl[n].directive, value);
            *rc = PRTE_ERR_SILENT;
            return true;
        }
        tbl[n].given = true;
        tbl[n].flag = flag;
        return true;
    }
    return false;
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
    int n, idx, rc;
    size_t m;
    pmix_status_t ret;
    char **targv = NULL, *ptr, *cptr, **quals = NULL;
    char *topo = NULL, *cpus = NULL;
    bool topogiven = false, cpusgiven = false;
    prte_bool_directive_t bools[] = {
        /* "map" must precede "map-devel"; see the comment on
         * set_bool_directive() */
        {PRTE_CLI_ALLOC, PMIX_DISPLAY_ALLOCATION, false, false},
        {PRTE_CLI_MAP, PMIX_DISPLAY_MAP, false, false},
        {PRTE_CLI_MAPDEV, PMIX_DISPLAY_MAP_DETAILED, false, false},
        {PRTE_CLI_BIND, PMIX_REPORT_BINDINGS, false, false},
        {NULL, NULL, false, false}
    };
    prte_bool_directive_t qualtbl[] = {
        {PRTE_CLI_PARSEABLE, PMIX_DISPLAY_PARSEABLE_OUTPUT, false, false},
        {PRTE_CLI_PARSABLE, PMIX_DISPLAY_PARSEABLE_OUTPUT, false, false},
        {PRTE_CLI_PHYSICAL_CPUS, PMIX_REPORT_PHYSICAL_CPUS, false, false},
        {NULL, NULL, false, false}
    };

    for (n=0; NULL != opt->values[n]; n++) {
        targv = PMIx_Argv_split(opt->values[n], ',');
        for (idx = 0; NULL != targv[idx]; idx++) {
            /* check for qualifiers */
            cptr = strchr(targv[idx], ':');
            if (NULL != cptr) {
                *cptr = '\0';
                ++cptr;
                quals = PMIx_Argv_split(cptr, ':');
                /* check qualifiers */
                for (m=0; NULL != quals[m]; m++) {
                    if (!set_bool_directive(qualtbl, PRTE_CLI_DISPLAY, quals[m],
                                            PMIX_CLI_QUALIFIER_VALUE(quals[m]), &rc)) {
                        prte_show_help("help-prte-rmaps-base.txt", "unrecognized-qualifier", true,
                                       "display", cptr, "PARSEABLE,PARSABLE,PHYSICAL");
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
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

            if (set_bool_directive(bools, PRTE_CLI_DISPLAY, targv[idx],
                                   PMIX_CLI_QUALIFIER_VALUE(targv[idx]), &rc)) {
                if (PRTE_SUCCESS != rc) {
                    goto cleanup;
                }

            } else if (PMIX_CHECK_CLI_OPTION(targv[idx], PRTE_CLI_TOPO)) {
                ptr = strchr(targv[idx], '=');
                if (NULL != ptr) {
                    ++ptr;
                    if ('\0' == *ptr) {
                        /* missing the value or value is invalid */
                        prte_show_help("help-prte-rmaps-base.txt", "invalid-value", true,
                                       "display", "TOPO", targv[idx]);
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                }
                /* "topo" with no value means "every node", which is a
                 * legitimate request - so record that it was asked for
                 * separately from the value it may have carried */
                if (NULL != topo) {
                    free(topo);
                }
                topo = (NULL == ptr) ? NULL : strdup(ptr);
                topogiven = true;

            } else if (PMIX_CHECK_CLI_OPTION(targv[idx], PRTE_CLI_CPUS)) {
                ptr = strchr(targv[idx], '=');
                if (NULL != ptr) {
                    ++ptr;
                    if ('\0' == *ptr) {
                        /* missing the value or value is invalid */
                        prte_show_help("help-prte-rmaps-base.txt", "invalid-value", true,
                                       "display", "PROCESSORS", targv[idx]);
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                }
                if (NULL != cpus) {
                    free(cpus);
                }
                cpus = (NULL == ptr) ? NULL : strdup(ptr);
                cpusgiven = true;
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
    char **targv = NULL, *ptr, *cptr, **options = NULL;
    int m, n, idx, rc;
    pmix_status_t ret;
    bool fileonly = true;
    bool copyqualgiven = false;
    bool patternqualgiven = false;
    bool flag;
    prte_bool_directive_t bools[] = {
        /* "tag" must precede "tag-detailed"/"tag-fullname"; see the comment
         * on set_bool_directive() */
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
        for (idx = 0; NULL != targv[idx]; idx++) {
            /* check for qualifiers */
            cptr = strchr(targv[idx], ':');
            if (NULL != cptr) {
                *cptr = '\0';
                ++cptr;
                /* could be multiple qualifiers, and a qualifier is separated
                 * from the directive - and from its fellows - by a ':'.  The
                 * ',' that separates directives has already been consumed
                 * above, so splitting on ',' here would hand the whole
                 * ':'-joined run to the matcher as one token; that token
                 * prefix-matches only its FIRST qualifier, and every
                 * qualifier after it is silently dropped */
                options = PMIx_Argv_split(cptr, ':');
                for (m=0; NULL != options[m]; m++) {

                    if (PMIX_CHECK_CLI_OPTION(options[m], PRTE_CLI_NOCOPY)) {
                        if (copyqualgiven) {
                            // cannot give both copy and nocopy
                            prte_show_help("help-schizo-output.txt", "copy-nocopy", true, cptr);
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
                        if (PRTE_SUCCESS != prte_cli_bool_value(PMIX_CLI_QUALIFIER_VALUE(options[m]),
                                                                &flag)) {
                            prte_show_help("help-schizo-base.txt", "non-boolean-value", true,
                                           PRTE_CLI_OUTPUT, PRTE_CLI_NOCOPY,
                                           PMIX_CLI_QUALIFIER_VALUE(options[m]));
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
                        fileonly = flag;
                        copyqualgiven = true;

                    } else if (PMIX_CHECK_CLI_OPTION(options[m], PRTE_CLI_COPY)) {
                        if (copyqualgiven) {
                            // cannot give both copy and nocopy
                            prte_show_help("help-schizo-output.txt", "copy-nocopy", true, cptr);
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
                        if (PRTE_SUCCESS != prte_cli_bool_value(PMIX_CLI_QUALIFIER_VALUE(options[m]),
                                                                &flag)) {
                            prte_show_help("help-schizo-base.txt", "non-boolean-value", true,
                                           PRTE_CLI_OUTPUT, PRTE_CLI_COPY,
                                           PMIX_CLI_QUALIFIER_VALUE(options[m]));
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
                        fileonly = !flag;
                        copyqualgiven = true;

                    } else if (PMIX_CHECK_CLI_OPTION(options[m], PRTE_CLI_PATTERN)) {
                        if (PRTE_SUCCESS != prte_cli_bool_value(PMIX_CLI_QUALIFIER_VALUE(options[m]),
                                                                &flag)) {
                            prte_show_help("help-schizo-base.txt", "non-boolean-value", true,
                                           PRTE_CLI_OUTPUT, PRTE_CLI_PATTERN,
                                           PMIX_CLI_QUALIFIER_VALUE(options[m]));
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
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
                            prte_show_help("help-schizo-output.txt", "pattern-unsupported", true);
                            rc = PRTE_ERR_SILENT;
                            goto cleanup;
                        }
#endif

                    } else if (set_bool_directive(qualtbl, PRTE_CLI_OUTPUT, options[m],
                                                  PMIX_CLI_QUALIFIER_VALUE(options[m]), &rc)) {
                        if (PRTE_SUCCESS != rc) {
                            goto cleanup;
                        }
                    }
                }
                PMIx_Argv_free(options);
                options = NULL;
            }
            if (0 == strlen(targv[idx])) {
                // only qualifiers were given
                continue;
            }
            /* remove any '=' sign in the directive */
            if (NULL != (ptr = strchr(targv[idx], '='))) {
                *ptr = '\0';
                ++ptr; // step over '=' sign
            }
            if (set_bool_directive(bools, PRTE_CLI_OUTPUT, targv[idx], ptr, &rc)) {
                if (PRTE_SUCCESS != rc) {
                    goto cleanup;
                }

            } else if (PMIX_CHECK_CLI_OPTION(targv[idx], PRTE_CLI_DIR)) {
                if (NULL == ptr || '\0' == *ptr) {
                    prte_show_help("help-prte-rmaps-base.txt",
                                   "missing-qualifier", true,
                                   "output", "directory", "directory");
                    rc = PRTE_ERR_FATAL;
                    goto cleanup;
                }
                if (NULL != outfile) {
                    prte_show_help("help-prted.txt", "both-file-and-dir-set", true, outfile, ptr);
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

            } else if (PMIX_CHECK_CLI_OPTION(targv[idx], PRTE_CLI_FILE)) {
                if (NULL == ptr || '\0' == *ptr) {
                    prte_show_help("help-prte-rmaps-base.txt",
                                   "missing-qualifier", true,
                                   "output", "filename", "filename");
                    rc = PRTE_ERR_FATAL;
                    goto cleanup;
                }
                if (NULL != outdir) {
                    prte_show_help("help-prted.txt", "both-file-and-dir-set", true, ptr, outdir);
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
            }
        }
        PMIx_Argv_free(targv);
        targv = NULL;
    }
    if (patternqualgiven && NULL == outfile) {
        /* "pattern" says how to name an output FILE.  Given without one it
         * has nothing to qualify, and silently ignoring it would leave the
         * user believing they had asked for something */
        prte_show_help("help-schizo-output.txt", "pattern-needs-file", true);
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
                prte_show_help("help-schizo-output.txt", "bad-pattern", true,
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
