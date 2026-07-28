/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
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

#include <ctype.h>

#include "src/class/pmix_list.h"

#include "src/include/pmix_frameworks.h"
#include "src/mca/pinstalldirs/pinstalldirs_types.h"
#include "src/include/prte_frameworks.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_cmd_line.h"

prte_schizo_base_module_t *prte_schizo_base_detect_proxy(char *cmdpath)
{
    prte_schizo_base_active_module_t *mod;
    prte_schizo_base_module_t *md = NULL;
    int pri = 0, p;

    /* A confidence of zero means "this is not me" - every component returns
     * it for a personality list that does not name it.  So the bar for being
     * selected is a STRICTLY POSITIVE bid, not merely the highest one.  Seeded
     * at -1, the first component queried won a field of all-zero bids, which
     * made a personality nobody claims select whichever component happens to
     * carry the highest static priority - so a typo in "--personality" ran the
     * command line in the ompi dialect, complete with its own option table and
     * MCA translation, rather than the native one. */
    PMIX_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->detect_proxy) {
            p = mod->module->detect_proxy(cmdpath);
            if (pri < p) {
                pri = p;
                md = mod->module;
            }
        }
    }

    /* Nobody claimed it.  Fall back to the DEFAULT personality - which is
     * what a component tells us it would be when asked with no hint at all -
     * rather than refusing outright.  A name we do not recognize is not
     * necessarily a mistake: Open MPI starts a singleton's DVM with
     * "--prtemca schizo prte", so only the native component is even loaded,
     * and then spawns with PMIX_PERSONALITY="ompi5".  Refusing that fails
     * every MPI_Comm_spawn from a singleton.  The catch-all is the right
     * answer there, and remains the right answer for a typo - what was wrong
     * before was landing on some *other* personality's dialect, not the
     * falling back itself. */
    if (NULL == md && NULL != cmdpath) {
        md = prte_schizo_base_detect_proxy(NULL);
    }
    return md;
}

/* the deprecated hyphenated spelling of each option whose canonical key is
 * un-hyphenated, paired with the key the option tables actually carry */
static struct {
    const char *deprecated;
    const char *canonical;
} normalized_opts[] = {
    {.deprecated = "--map-by",          .canonical = "--mapby"},
    {.deprecated = "--rank-by",         .canonical = "--rankby"},
    {.deprecated = "--bind-to",         .canonical = "--bindto"},
    {.deprecated = "--runtime-options", .canonical = "--rtos"},
    {.deprecated = NULL,                .canonical = NULL}
};

char *prte_schizo_base_normalize_argv(char **argv)
{
    char *personality = NULL, *value, *tmp;
    size_t len;
    int i, n;

    /* Normalize the deprecated hyphenated option spellings to their canonical
     * forms and capture any personality specification.  --rank-by and --bind-to
     * may legitimately appear more than once in a command line - once per app
     * context in an MPMD invocation - so, exactly like --map-by, they are
     * renamed unconditionally on every occurrence.  Detecting an erroneous
     * duplicate (e.g. two job-level --rank-by with no intervening MPMD
     * separator) is the schizo MPMD parser's responsibility, since it has the
     * app-context boundaries that this flat argv scan lacks.
     *
     * Both spellings getopt_long accepts have to be handled here - "--opt value"
     * AND "--opt=value".  Only recognizing the space-separated form leaves the
     * "=" form to be rejected later as an unrecognized option (for the renamed
     * options) or, worse, silently ignored (for --personality, which would then
     * drop the invocation onto the default personality). */
    for (i = 0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            personality = argv[i + 1];
            continue;
        }
        if (0 == strncmp(argv[i], "--personality=", strlen("--personality="))) {
            value = argv[i] + strlen("--personality=");
            personality = ('\0' == value[0]) ? NULL : value;
            continue;
        }
        for (n = 0; NULL != normalized_opts[n].deprecated; n++) {
            len = strlen(normalized_opts[n].deprecated);
            if (0 == strcmp(argv[i], normalized_opts[n].deprecated)) {
                free(argv[i]);
                argv[i] = strdup(normalized_opts[n].canonical);
                break;
            }
            if (0 == strncmp(argv[i], normalized_opts[n].deprecated, len) &&
                '=' == argv[i][len]) {
                pmix_asprintf(&tmp, "%s%s", normalized_opts[n].canonical, &argv[i][len]);
                free(argv[i]);
                argv[i] = tmp;
                break;
            }
        }
    }
    return personality;
}

PRTE_EXPORT void prte_schizo_base_root_error_msg(void)
{
    fprintf(stderr, "%s has detected an attempt to run as root.\n\n", prte_tool_basename);
    fprintf(stderr, "Running as root is *strongly* discouraged as any mistake (e.g., in\n");
    fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
    fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");

    fprintf(stderr, "We strongly suggest that you run %s as a non-root user.\n\n",
            prte_tool_basename);

    fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
    fprintf(stderr, "option to your command line.  However, we reiterate our strong advice\n");
    fprintf(stderr, "against doing so - please do so at your own risk.\n");
    fprintf(stderr, "--------------------------------------------------------------------------\n");
    exit(1);
}

static bool check_multi(const char *target)
{
    char *multi_dirs[] = {
        PRTE_CLI_DISPLAY,
        PRTE_CLI_OUTPUT,
        PRTE_CLI_TUNE,
        PRTE_CLI_RTOS,
        NULL
    };
    int n;

    for (n=0; NULL != multi_dirs[n]; n++) {
        if (0 == strcmp(target, multi_dirs[n])) {
            return true;
        }
    }
    return false;
}

/* add directive to a PRRTE option that takes a single value */
int prte_schizo_base_add_directive(pmix_cli_result_t *results,
                                   const char *deprecated, const char *target,
                                   char *directive, bool report)
{
    pmix_cli_item_t *opt;
    char *ptr, *tmp;

    /* does the matching key already exist? */
    opt = pmix_cmd_line_get_param(results, target);
    if (NULL != opt) {
        // does it already have a value?
        if (NULL == opt->values) {
            // technically this should never happen, but...
            PMIx_Argv_append_nosize(&opt->values, directive);
        } else if (1 < PMIx_Argv_count(opt->values)) {
            // cannot use this function
            ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-values",
                                        true, target);
            fprintf(stderr, "%s\n", ptr);
            free(ptr);
            return PRTE_ERR_SILENT;
        } else {
            // does this contain only a qualifier?
            if (':' == opt->values[0][0]) {
                // prepend it with the directive
                pmix_asprintf(&tmp, "%s%s", directive, opt->values[0]);
                free(opt->values[0]);
                opt->values[0] = tmp;
            } else {
                // do we allow multiple directives?
                if (!check_multi(target)) {
                    // report the error
                    tmp = PMIx_Argv_join(opt->values, ',');
                    ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-directives",
                                                true, target, tmp, deprecated, directive);
                    free(tmp);
                    fprintf(stderr, "%s\n", ptr);
                    free(ptr);
                    return PRTE_ERR_SILENT;
                }
                // does the value contain a qualifier?
                if (NULL != (ptr = strchr(opt->values[0], ':'))) {
                    // split the value at the qualifier
                    *ptr = '\0';
                    ++ptr;
                    // form the new value
                    pmix_asprintf(&tmp, "%s,%s:%s", opt->values[0], directive, ptr);
                    free(opt->values[0]);
                    opt->values[0] = tmp;
                } else {
                    // append the directive to the pre-existing ones
                    pmix_asprintf(&tmp, "%s,%s", opt->values[0], directive);
                    free(opt->values[0]);
                    opt->values[0] = tmp;
                }
            }
        }
    } else {
        // add the new option
        opt = PMIX_NEW(pmix_cli_item_t);
        opt->key = strdup(target);
        PMIx_Argv_append_nosize(&opt->values, directive);
        pmix_list_append(&results->instances, &opt->super);
    }

    if (report) {
        pmix_asprintf(&tmp, "--%s %s", target, directive);
        /* can't just call show_help as we want every instance to be reported */
        ptr = pmix_show_help_string("help-schizo-base.txt", "deprecated-converted",
                                    true, deprecated, tmp);
        fprintf(stderr, "%s\n", ptr);
        free(tmp);
        free(ptr);
    }
    return PRTE_SUCCESS;
}

/* add qualifier to a PRRTE option that takes a single value */
int prte_schizo_base_add_qualifier(pmix_cli_result_t *results,
                                   char *deprecated, char *target,
                                   char *qualifier, bool report)
{
    pmix_cli_item_t *opt;
    char *ptr, *tmp;

    /* does the matching key already exist? */
    opt = pmix_cmd_line_get_param(results, target);
    if (NULL != opt) {
        // does it already have a value?
        if (NULL == opt->values) {
            // technically this should never happen, but...
            pmix_asprintf(&tmp, ":%s", qualifier);
            PMIx_Argv_append_nosize(&opt->values, tmp);
            free(tmp);
        } else if (1 < PMIx_Argv_count(opt->values)) {
            // cannot use this function
            ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-values",
                                        true, target);
            fprintf(stderr, "%s\n", ptr);
            free(ptr);
            return PRTE_ERR_SILENT;
        } else {
            // append with a colon delimiter
            pmix_asprintf(&tmp, "%s:%s", opt->values[0], qualifier);
            free(opt->values[0]);
            opt->values[0] = tmp;
        }
    } else {
        // add the new option
        opt = PMIX_NEW(pmix_cli_item_t);
        opt->key = strdup(target);
        pmix_asprintf(&tmp, ":%s", qualifier);
        PMIx_Argv_append_nosize(&opt->values, tmp);
        free(tmp);
        pmix_list_append(&results->instances, &opt->super);
    }

    if (report) {
        pmix_asprintf(&tmp, "--%s :%s", target, qualifier);
        /* can't just call show_help as we want every instance to be reported */
        ptr = pmix_show_help_string("help-schizo-base.txt", "deprecated-converted",
                                    true, deprecated, tmp);
        fprintf(stderr, "%s\n", ptr);
        free(tmp);
        free(ptr);
    }
    return PRTE_SUCCESS;
}

char *prte_schizo_base_getline(FILE *fp)
{
    char *ret, *buff;
    size_t len;
    char input[2048];

    memset(input, 0, 2048);
    ret = fgets(input, 2048, fp);
    if (NULL != ret) {
        /* strip the newline - but only if there IS one.  The last line of a
         * file that does not end in a newline has none, and blindly chopping
         * its final character silently corrupts that line (an MCA param file
         * saved without a trailing newline would lose a character off its
         * last parameter) */
        len = strlen(input);
        if (0 < len && '\n' == input[len - 1]) {
            input[len - 1] = '\0';
        }
        buff = strdup(input);
        return buff;
    }

    return NULL;
}

char *prte_schizo_base_strip_quotes(char *p)
{
    char *pout;
    size_t len;

    /* strip any quotes around the args */
    if ('\"' == p[0]) {
        pout = strdup(&p[1]);
    } else {
        pout = strdup(p);
    }
    /* an empty string has no trailing character to inspect - reading (and
     * writing) pout[-1] here walks off the front of the allocation */
    len = strlen(pout);
    if (0 < len && '\"' == pout[len - 1]) {
        pout[len - 1] = '\0';
    }
    return pout;
}

int prte_schizo_base_parse_prte(int argc, int start, char **argv, char ***target)
{
    int i;
    bool use;
    char *p1, *p2, *param;

    for (i = 0; i < (argc - start); ++i) {
        if (0 == strcmp("--", argv[i])) {
            // quit processing
            return PRTE_SUCCESS;
        }
        if (0 == strcmp("--prtemca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                pmix_show_help("help-schizo-base.txt", "missing-values", true,
                               "--prtemca");
                return PRTE_ERR_SILENT;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);
            if (NULL == target) {
                /* push it into our environment */
                pmix_asprintf(&param, "PRTE_MCA_%s", p1);
                pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                                    "%s schizo:prte:parse_cli pushing %s=%s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param, p2);
                setenv(param, p2, true);
                free(param);
            } else {
                PMIx_Argv_append_nosize(target, "--prtemca");
                PMIx_Argv_append_nosize(target, p1);
                PMIx_Argv_append_nosize(target, p2);
            }
            free(p1);
            free(p2);
            i += 2;
            continue;
        }
        if (0 == strcmp("--mca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                pmix_show_help("help-schizo-base.txt", "missing-values", true,
                               "--mca");
                return PRTE_ERR_SILENT;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            use = pmix_pmdl_base_check_prte_param(p1);
            if (use) {
                /* replace the generic directive with a PRRTE specific
                 * one so we know this has been processed */
                free(argv[i]);
                argv[i] = strdup("--prtemca");
                /* if this refers to the "if" framework, convert to "prteif" */
                if (0 == strncasecmp(p1, "if", 2)) {
                    pmix_asprintf(&param, "prteif_%s", &p1[3]);
                    free(p1);
                    p1 = param;
                } else if (0 == strncasecmp(p1, "reachable", strlen("reachable"))) {
                    pmix_asprintf(&param, "prtereachable_%s", &p1[strlen("reachable_")]);
                    free(p1);
                    p1 = param;
                } else if (0 == strncasecmp(p1, "plm_rsh", strlen("plm_rsh"))) {
                    pmix_asprintf(&param, "plm_ssh_%s", &p1[strlen("plm_rsh_")]);
                    free(p1);
                    p1 = param;
                }
                if (NULL == target) {
                    /* push it into our environment */
                    pmix_asprintf(&param, "PRTE_MCA_%s", p1);
                    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:prte:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1);
                    setenv(param, p2, true);
                    free(param);
                } else {
                    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:prte:parse_cli adding %s to target",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1);
                    PMIx_Argv_append_nosize(target, "--prtemca");
                    PMIx_Argv_append_nosize(target, p1);
                    PMIx_Argv_append_nosize(target, p2);
                }
                i += 2;
            }
            free(p1);
            free(p2);
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_parse_pmix(int argc, int start, char **argv, char ***target)
{
    int i;
    bool use;
    char *p1, *p2, *param;

    for (i = 0; i < (argc - start); ++i) {
        if (0 == strcmp("--", argv[i])) {
            // quit processing
            return PRTE_SUCCESS;
        }
        if (0 == strcmp("--pmixmca", argv[i]) || 0 == strcmp("--gpmixmca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                pmix_show_help("help-schizo-base.txt", "missing-values", true,
                               "--pmixmca");
                return PRTE_ERR_SILENT;
            }
            /* strip any quotes around the args */
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);
            if (NULL == target) {
                /* push it into our environment */
                pmix_asprintf(&param, "PMIX_MCA_%s", p1);
                pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                                    "%s schizo:pmix:parse_cli pushing %s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                setenv(param, p2, true);
                free(param);
            } else {
                PMIx_Argv_append_nosize(target, argv[i]);
                PMIx_Argv_append_nosize(target, p1);
                PMIx_Argv_append_nosize(target, p2);
            }
            free(p1);
            free(p2);
            i += 2;
            continue;
        }
        if (0 == strcmp("--mca", argv[i]) || 0 == strcmp("--gmca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            /* strip any quotes around the args */
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);

            // see if this param references the MCA "base"
            if (0 == strncmp(p1, "mca_base_", strlen("mca_base_"))) {
                /* we have multiple projects that utilize the MCA
                 * framework system. Since this was given as a generic
                 * parameter, we cannot tell which of those projects
                 * are being targeted. So we have no choice but to
                 * apply the param to ALL of them
                 */
                if (NULL == target) {
                    pmix_asprintf(&param, "PMIX_MCA_%s", p1);
                    setenv(param, p2, true);
                    free(param);
                    // PRRTE shares the MCA base with PMIx, so no
                    // need to cover that project
                    pmix_asprintf(&param, "OMPI_MCA_%s", p1);
                    setenv(param, p2, true);
                    free(param);
                } else {
                    PMIx_Argv_append_nosize(target, "--pmixmca");
                    PMIx_Argv_append_nosize(target, p1);
                    PMIx_Argv_append_nosize(target, p2);
                    PMIx_Argv_append_nosize(target, "--omca");
                    PMIx_Argv_append_nosize(target, p1);
                    PMIx_Argv_append_nosize(target, p2);
                }
                free(p1);
                free(p2);
                i += 2;
                continue;
            }

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            use = pmix_pmdl_base_check_pmix_param(p1);
            if (use) {
                /* replace the generic directive with a PMIx specific
                 * one so we know this has been processed */
                free(argv[i]);
                argv[i] = strdup("--pmixmca");
                /* if this refers to the "if" framework, convert to "pif" */
                if (0 == strncasecmp(p1, "if", 2)) {
                    pmix_asprintf(&param, "pif_%s", &p1[3]);
                    free(p1);
                    p1 = param;
                } else if (0 == strncasecmp(p1, "reachable", strlen("reachable"))) {
                    pmix_asprintf(&param, "preachable_%s", &p1[strlen("reachable_")]);
                    free(p1);
                    p1 = param;
                } else if (0 == strncasecmp(p1, "dl", strlen("dl"))) {
                    pmix_asprintf(&param, "pdl_%s", &p1[strlen("dl_")]);
                    free(p1);
                    p1 = param;
                }
                if (NULL == target) {
                    /* push it into our environment */
                    pmix_asprintf(&param, "PMIX_MCA_%s", p1);
                    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:pmix:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    setenv(param, p2, true);
                    free(param);
                } else {
                    PMIx_Argv_append_nosize(target, "--pmixmca");
                    PMIx_Argv_append_nosize(target, p1);
                    PMIx_Argv_append_nosize(target, p2);
                }
            }
            free(p1);
            free(p2);
            i += 2;
            continue;
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_setup_fork(prte_job_t *jdata, prte_app_context_t *app)
{
    prte_attribute_t *attr;
    bool exists, prefix_defined = false;
    char *param, *p2, *saveptr, *p, *defprefix;
    int i;
    prte_job_t *daemons;

    /* flag that we started this job */
    PMIx_Setenv("PRTE_LAUNCHED", "1", true, &app->env);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PMIX_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t)
    {
        if (PRTE_JOB_SET_ENVAR == attr->key) {
            PMIx_Setenv(attr->data.data.envar.envar,
                               attr->data.data.envar.value,
                               true, &app->env);
        } else if (PRTE_JOB_ADD_ENVAR == attr->key) {
            PMIx_Setenv(attr->data.data.envar.envar,
                               attr->data.data.envar.value,
                               false, &app->env);
        } else if (PRTE_JOB_UNSET_ENVAR == attr->key) {
            pmix_unsetenv(attr->data.data.string, &app->env);
        } else if (PRTE_JOB_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                PMIx_Setenv(attr->data.data.envar.envar,
                                   attr->data.data.envar.value,
                                   true, &app->env);
            }
        } else if (PRTE_JOB_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                PMIx_Setenv(attr->data.data.envar.envar,
                                   attr->data.data.envar.value,
                                   true, &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PMIX_LIST_FOREACH(attr, &app->attributes, prte_attribute_t)
    {
        if (PRTE_APP_PMIX_PREFIX == attr->key) {
            prefix_defined = true;
            /* if the prefix string is NULL, then the user doesn't
             * want any prefix applied to this application */
            if (NULL == attr->data.data.string) {
                continue;
            }
            // need to set the prefix into the environment
            PMIx_Setenv("PMIX_PREFIX",
                               attr->data.data.string,
                               true, &app->env);
            // and need to set LD_LIBRARY_PATH
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], "LD_LIBRARY_PATH")) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    p = pmix_basename(pmix_pinstall_dirs.libdir);
                    pmix_asprintf(&p2, "%s/%s:%s", attr->data.data.string, p, param);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv("LD_LIBRARY_PATH", p2, true, &app->env);
                    free(p2);
                    free(p);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                param = pmix_basename(pmix_pinstall_dirs.libdir);
                pmix_asprintf(&p2, "%s/%s", attr->data.data.string, param);
                PMIx_Setenv("LD_LIBRARY_PATH",
                                   p2,
                                   true, &app->env);
                free(p2);
                free(param);
            }

        } else if (PRTE_APP_SET_ENVAR == attr->key) {
            PMIx_Setenv(attr->data.data.envar.envar,
                               attr->data.data.envar.value,
                               true, &app->env);

        } else if (PRTE_APP_ADD_ENVAR == attr->key) {
            PMIx_Setenv(attr->data.data.envar.envar,
                               attr->data.data.envar.value,
                               false, &app->env);

        } else if (PRTE_APP_UNSET_ENVAR == attr->key) {
            pmix_unsetenv(attr->data.data.string, &app->env);

        } else if (PRTE_APP_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                PMIx_Setenv(attr->data.data.envar.envar,
                                   attr->data.data.envar.value,
                                   true, &app->env);
            }

        } else if (PRTE_APP_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                PMIx_Setenv(attr->data.data.envar.envar,
                                   attr->data.data.envar.value,
                                   true, &app->env);
            }
        }
    }

    /* if the app's prefix wasn't defined, then check for presence
     * of a default one */
    if (!prefix_defined) {
        daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        if (prte_get_attribute(&daemons->attributes, PRTE_JOB_PMIX_PREFIX, (void **)&defprefix, PMIX_STRING)) {
            PMIx_Setenv("PMIX_PREFIX",
                               defprefix,
                               true, &app->env);
            // and need to set LD_LIBRARY_PATH
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], "LD_LIBRARY_PATH")) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    p = pmix_basename(pmix_pinstall_dirs.libdir);
                    pmix_asprintf(&p2, "%s/%s:%s", defprefix, p, param);
                    *saveptr = '='; // restore the current envar setting
                    PMIx_Setenv("LD_LIBRARY_PATH", p2, true, &app->env);
                    free(p2);
                    free(p);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                param = pmix_basename(pmix_pinstall_dirs.libdir);
                pmix_asprintf(&p2, "%s/%s", defprefix, param);
                PMIx_Setenv("LD_LIBRARY_PATH",
                                   p2,
                                   true, &app->env);
                free(p2);
                free(param);
            }
            free(defprefix);
        }
    }

    return PRTE_SUCCESS;
}
