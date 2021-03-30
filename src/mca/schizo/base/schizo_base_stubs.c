/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <ctype.h>

#include "src/class/prte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/argv.h"
#include "src/util/name_fns.h"
#include "src/util/prte_environ.h"
#include "src/util/show_help.h"

int prte_schizo_base_parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv,
                               bool cmdline)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->parse_env) {
            rc = mod->module->parse_env(cmd_line, srcenv, dstenv, cmdline);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

prte_schizo_base_module_t *prte_schizo_base_detect_proxy(char *cmdpath)
{
    prte_schizo_base_active_module_t *mod;
    prte_schizo_base_module_t *md = NULL;
    int pri = -1, p;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
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

int prte_schizo_base_setup_app(prte_app_context_t *app)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->setup_app) {
            rc = mod->module->setup_app(app);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_setup_fork(prte_job_t *jdata, prte_app_context_t *context)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->setup_fork) {
            rc = mod->module->setup_fork(jdata, context);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_setup_child(prte_job_t *jdata, prte_proc_t *child, prte_app_context_t *app,
                                 char ***env)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->setup_child) {
            rc = mod->module->setup_child(jdata, child, app, env);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

void prte_schizo_base_job_info(prte_cmd_line_t *cmdline, void *jobinfo)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->job_info) {
            mod->module->job_info(cmdline, jobinfo);
        }
    }
}

int prte_schizo_base_check_sanity(prte_cmd_line_t *cmdline)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->check_sanity) {
            rc = mod->module->check_sanity(cmdline);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

void prte_schizo_base_finalize(void)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->finalize) {
            mod->module->finalize();
        }
    }
}

int prte_schizo_base_process_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv,
                                            char **options, prte_schizo_convertor_fn_t convert)
{
    int pargc;
    char **pargs, *p2;
    int i, n, rc, ret;
    prte_cmd_line_init_t e;
    prte_cmd_line_option_t *option;
    bool found;

    pargs = *argv;
    pargc = *argc;
    ret = PRTE_SUCCESS;

    /* check for deprecated cmd line options */
    for (i = 1; i < pargc && NULL != pargs[i]; i++) {
        /* Are we done?  i.e., did we find the special "--" token? */
        if (0 == strcmp(pargs[i], "--")) {
            break;
        }

        /* check for option */
        if ('-' != pargs[i][0]) {
            /* not an option - we are done. Note that options
             * are required to increment past their arguments
             * so we don't mistakenly think we are at the end */
            break;
        }

        if ('-' != pargs[i][1] && 2 < strlen(pargs[i])) {
            /* we know this is incorrect */
            p2 = strdup(pargs[i]);
            free(pargs[i]);
            prte_asprintf(&pargs[i], "-%s", p2);
            /* if it is the special "-np" option, we silently
             * change it and don't emit an error */
            if (0 == strcmp(p2, "-np")) {
                free(p2);
            } else {
                prte_show_help("help-schizo-base.txt", "single-dash-error", true, p2, pargs[i]);
                free(p2);
                ret = PRTE_OPERATION_SUCCEEDED;
            }
        }

        /* is this an argument someone needs to convert? */
        found = false;
        for (n = 0; NULL != options[n]; n++) {
            if (0 == strcmp(pargs[i], options[n])) {
                rc = convert(options[n], argv, i);
                if (PRTE_SUCCESS != rc && PRTE_ERR_SILENT != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc
                    && PRTE_OPERATION_SUCCEEDED != rc) {
                    return rc;
                }
                if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                    /* we did the conversion but don't want
                     * to deprecate i */
                    rc = PRTE_SUCCESS;
                } else if (PRTE_OPERATION_SUCCEEDED == rc) {
                    /* Advance past any command line option
                     * parameters */
                    memset(&e, 0, sizeof(prte_cmd_line_init_t));
                    e.ocl_cmd_long_name = &pargs[i][2];
                    option = prte_cmd_line_find_option(cmdline, &e);
                    i += option->clo_num_params;
                    rc = PRTE_ERR_SILENT;
                } else {
                    --i;
                }
                found = true;
                if (PRTE_ERR_SILENT != rc) {
                    ret = PRTE_OPERATION_SUCCEEDED;
                }
                pargs = *argv;
                pargc = prte_argv_count(pargs);
                break; // for loop
            }
        }

        if (!found) {
            /* check for single-dash option */
            if (2 == strlen(pargs[i])) {
                /* find the option */
                memset(&e, 0, sizeof(prte_cmd_line_init_t));
                e.ocl_cmd_short_name = pargs[i][1];
                option = prte_cmd_line_find_option(cmdline, &e);

                /* if this isn't an option, then we are done */
                if (NULL == option) {
                    break;
                }

                /* increment past the number of arguments for this option */
                i += option->clo_num_params;
            }
            /* check if we are done */
            else {
                /* find the option */
                memset(&e, 0, sizeof(prte_cmd_line_init_t));
                e.ocl_cmd_long_name = &pargs[i][2];
                option = prte_cmd_line_find_option(cmdline, &e);

                /* if this isn't an option, then we are done */
                if (NULL == option) {
                    break;
                }

                /* increment past the number of arguments for this option */
                i += option->clo_num_params;
            }
        }
    }
    *argc = pargc;

    return ret;
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

bool prte_schizo_base_check_ini(char *cmdpath, char *file)
{
    FILE *fp;
    char *line;
    size_t n;

    if (NULL == cmdpath || NULL == file) {
        return false;
    }

    /* look for an open-mpi.ini or ompi.ini file */
    fp = fopen(file, "r");
    if (NULL == fp) {
        return false;
    }
    /* read the file to find the proxy defnitions */
    while (NULL != (line = prte_schizo_base_getline(fp))) {
        if ('\0' == line[0]) {
            continue; /* skip empty lines */
        }
        /* find the start of text in the line */
        n = 0;
        while ('\0' != line[n] && isspace(line[n])) {
            ++n;
        }
        /* if the text starts with a '#' or the line
         * is empty, then ignore it */
        if ('\0' == line[n] || '#' == line[n]) {
            /* empty line or comment */
            continue;
        }
        if (0 == strcmp(cmdpath, &line[n])) {
            /* this is us! */
            return true;
        }
    }
    return false;
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
    NULL,
};

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
                                    "%s schizo:prte:parse_cli pushing %s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                prte_setenv(param, p2, true, &environ);
            } else {
                prte_argv_append_nosize(target, "--prtemca");
                prte_argv_append_nosize(target, p1);
                prte_argv_append_nosize(target, p2);
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
                if (NULL == target) {
                    /* push it into our environment */
                    asprintf(&param, "PRTE_MCA_%s", p1);
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:prte:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, p2, true, &environ);
                } else {
                    prte_argv_append_nosize(target, "--prtemca");
                    prte_argv_append_nosize(target, p1);
                    prte_argv_append_nosize(target, p2);
                }
                free(p1);
                free(p2);
                i += 2;
                continue;
            }
        }
    }
    return PRTE_SUCCESS;
}

static char *pmix_frameworks[] = {
    "bfrops",  "gds",    "pcompress", "pdl",   "pfexec", "pif", "pinstalldirs",
    "ploc",    "plog",   "pmdl",      "pnet",  "preg",   "prm", "psec",
    "psensor", "pshmem", "psquash",   "pstat", "pstrg",  "ptl", NULL,
};

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
                prte_setenv(param, p2, true, &environ);
            } else {
                prte_argv_append_nosize(target, argv[i]);
                prte_argv_append_nosize(target, p1);
                prte_argv_append_nosize(target, p2);
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
                if (NULL == target) {
                    /* push it into our environment */
                    asprintf(&param, "PMIX_MCA_%s", p1);
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:pmix:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, p2, true, &environ);
                } else {
                    prte_argv_append_nosize(target, "--pmixmca");
                    prte_argv_append_nosize(target, p1);
                    prte_argv_append_nosize(target, p2);
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
