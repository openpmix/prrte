/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"

prte_schizo_base_module_t *prte_schizo_base_detect_proxy(char *cmdpath)
{
    prte_schizo_base_active_module_t *mod;
    prte_schizo_base_module_t *md = NULL;
    int pri = -1, p;

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
    return md;
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
        "display",
        "output",
        "tune",
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
            pmix_argv_append_nosize(&opt->values, directive);
        } else if (1 < pmix_argv_count(opt->values)) {
            // cannot use this function
            ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-values",
                                        true, target);
            fprintf(stderr, "%s\n", ptr);
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
                    tmp = pmix_argv_join(opt->values, ',');
                    ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-directives",
                                                true, target, tmp, deprecated, directive);
                    free(tmp);
                    fprintf(stderr, "%s\n", ptr);
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
        pmix_argv_append_nosize(&opt->values, directive);
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
            pmix_argv_append_nosize(&opt->values, tmp);
            free(tmp);
        } else if (1 < pmix_argv_count(opt->values)) {
            // cannot use this function
            ptr = pmix_show_help_string("help-schizo-base.txt", "too-many-values",
                                        true, target);
            fprintf(stderr, "%s\n", ptr);
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
        pmix_argv_append_nosize(&opt->values, tmp);
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
    char input[2048];

    memset(input, 0, 2048);
    ret = fgets(input, 2048, fp);
    if (NULL != ret) {
        input[strlen(input) - 1] = '\0'; /* remove newline */
        buff = strdup(input);
        return buff;
    }

    return NULL;
}

char *prte_schizo_base_strip_quotes(char *p)
{
    char *pout;

    /* strip any quotes around the args */
    if ('\"' == p[0]) {
        pout = strdup(&p[1]);
    } else {
        pout = strdup(p);
    }
    if ('\"' == pout[strlen(pout) - 1]) {
        pout[strlen(pout) - 1] = '\0';
    }
    return pout;
}

static char *prte_frameworks[] = {
    "errmgr",
    "ess",
    "filem",
    "grpcomm",
    "iof",
    "odls",
    "oob",
    "plm",
    "propagate",
    "prtebacktrace",
    "prtedl",
    "prteif",
    "prteinstalldirs",
    "prtereachable",
    "ras",
    "rmaps",
    "rml",
    "routed",
    "rtc",
    "schizo",
    "state",
    // inherited from OPAL
    "hwloc",
    "if",
    "reachable",
    NULL,
};

bool prte_schizo_base_check_prte_param(char *param)
{
    char **tmp;
    size_t n;

    tmp = pmix_argv_split(param, '_');
    for (n=0; NULL != prte_frameworks[n]; n++) {
        if (0 == strncmp(tmp[0], prte_frameworks[n], strlen(prte_frameworks[n]))) {
            pmix_argv_free(tmp);
            return true;
        }
    }
    pmix_argv_free(tmp);
    return false;
}

int prte_schizo_base_parse_prte(int argc, int start, char **argv, char ***target)
{
    int i, j;
    bool ignore;
    char *p1, *p2, *param;

    for (i = 0; i < (argc - start); ++i) {
        if (0 == strcmp("--prtemca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);
            if (NULL == target) {
                /* push it into our environment */
                asprintf(&param, "PRTE_MCA_%s", p1);
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                    "%s schizo:prte:parse_cli pushing %s=%s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param, p2);
                setenv(param, p2, true);
                free(param);
            } else {
                pmix_argv_append_nosize(target, "--prtemca");
                pmix_argv_append_nosize(target, p1);
                pmix_argv_append_nosize(target, p2);
            }
            free(p1);
            free(p2);
            i += 2;
            continue;
        }
        if (0 == strcmp("--mca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            ignore = true;
            if (0 == strncmp("prte", p1, strlen("prte"))) {
                ignore = false;
            } else {
                for (j = 0; NULL != prte_frameworks[j]; j++) {
                    if (0 == strncmp(p1, prte_frameworks[j], strlen(prte_frameworks[j]))) {
                        ignore = false;
                        break;
                    }
                }
            }
            if (!ignore) {
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
                } else if (0 == strncasecmp(p1, "dl", strlen("dl"))) {
                    pmix_asprintf(&param, "prtedl_%s", &p1[strlen("dl_")]);
                    free(p1);
                    p1 = param;
                }
                if (NULL == target) {
                    /* push it into our environment */
                    asprintf(&param, "PRTE_MCA_%s", p1);
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:prte:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1);
                    setenv(param, p2, true);
                    free(param);
                } else {
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:prte:parse_cli adding %s to target",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1);
                    pmix_argv_append_nosize(target, "--prtemca");
                    pmix_argv_append_nosize(target, p1);
                    pmix_argv_append_nosize(target, p2);
                }
                i += 2;
            }
            free(p1);
            free(p2);
        }
    }
    return PRTE_SUCCESS;
}

static char *pmix_frameworks[] = {
    "bfrops",
    "gds",
    "pcompress",
    "pdl",
    "pfexec",
    "pif",
    "pinstalldirs",
    "ploc",
    "plog",
    "pmdl",
    "pnet",
    "preg",
    "prm",
    "psec",
    "psensor",
    "pshmem",
    "psquash",
    "pstat",
    "pstrg",
    "ptl",
    NULL
};

bool prte_schizo_base_check_pmix_param(char *param)
{
    char **tmp;
    size_t n;

    tmp = pmix_argv_split(param, '_');
    for (n=0; NULL != pmix_frameworks[n]; n++) {
        if (0 == strncmp(tmp[0], pmix_frameworks[n], strlen(pmix_frameworks[n]))) {
            pmix_argv_free(tmp);
            return true;
        }
    }
    pmix_argv_free(tmp);
    return false;
}

int prte_schizo_base_parse_pmix(int argc, int start, char **argv, char ***target)
{
    int i, j;
    bool ignore;
    char *p1, *p2, *param;

    for (i = 0; i < (argc - start); ++i) {
        ignore = true;
        if (0 == strcmp("--pmixmca", argv[i]) || 0 == strcmp("--gpmixmca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            /* strip any quotes around the args */
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);
            if (NULL == target) {
                /* push it into our environment */
                asprintf(&param, "PMIX_MCA_%s", p1);
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                    "%s schizo:pmix:parse_cli pushing %s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                setenv(param, p2, true);
                free(param);
            } else {
                pmix_argv_append_nosize(target, argv[i]);
                pmix_argv_append_nosize(target, p1);
                pmix_argv_append_nosize(target, p2);
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

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            if (0 == strncmp("pmix", p1, strlen("pmix"))) {
                ignore = false;
            } else {
                for (j = 0; NULL != pmix_frameworks[j]; j++) {
                    if (0 == strncmp(p1, pmix_frameworks[j], strlen(pmix_frameworks[j]))) {
                        ignore = false;
                        break;
                    }
                }
            }
            if (!ignore) {
                /* replace the generic directive with a PRRTE specific
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
                    asprintf(&param, "PMIX_MCA_%s", p1);
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:pmix:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    setenv(param, p2, true);
                    free(param);
                } else {
                    pmix_argv_append_nosize(target, "--pmixmca");
                    pmix_argv_append_nosize(target, p1);
                    pmix_argv_append_nosize(target, p2);
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
