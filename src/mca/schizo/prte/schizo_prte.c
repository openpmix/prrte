/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/prte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/schizo/base/base.h"
#include "schizo_prte.h"

static int define_cli(prte_cmd_line_t *cli);
static void register_deprecated_cli(prte_list_t *convertors);
static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target);
static void parse_proxy_cli(prte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(prte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static int setup_fork(prte_job_t *jdata,
                      prte_app_context_t *context);
static int define_session_dir(char **tmpdir);
static int detect_proxy(char **argv);
static int allow_run_as_root(prte_cmd_line_t *cmd_line);
static void wrap_args(char **args);
static int check_sanity(prte_cmd_line_t *cmd_line);

prte_schizo_base_module_t prte_schizo_prte_module = {
    .define_cli = define_cli,
    .register_deprecated_cli = register_deprecated_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .define_session_dir = define_session_dir,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .wrap_args = wrap_args,
    .check_sanity = check_sanity
};


/* Cmd-line options common to PRTE master/daemons/tools */
static prte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" generalized options */
    { 'h', "help", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "This help message", PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'V', "version", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Print version and exit", PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'v', "verbose", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Be verbose", PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'q', "quiet", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Suppress helpful messages", PRTE_CMD_LINE_OTYPE_GENERAL },

      /* DVM-related options */
    { '\0', "tmpdir", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Set the root for the session directory tree",
      PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "allow-run-as-root", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Allow execution as root (STRONGLY DISCOURAGED)",
      PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "personality", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Comma-separated list of programming model, languages, and containers being used (default=\"prte\")",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prefix", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Prefix to be used to look for PRTE executables",
      PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "noprefix", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Disable automatic --prefix behavior",
      PRTE_CMD_LINE_OTYPE_DVM },


    /* setup MCA parameters */
    { '\0', "mca", 2, PRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prtemca", 2, PRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific PRTE MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Request parseable help output */
    { '\0', "prte_info_pretty", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "When used in conjunction with other parameters, the output is displayed in 'prte_info_prettyprint' format (default)",
      PRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parsable", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "When used in conjunction with other parameters, the output is displayed in a machine-parsable format",
       PRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parseable", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Synonym for --parsable",
      PRTE_CMD_LINE_OTYPE_GENERAL },

    /* Set a hostfile */
    { '\0', "hostfile", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "machinefile", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "default-hostfile", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Provide a default hostfile",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { 'H', "host", 1, PRTE_CMD_LINE_TYPE_STRING,
      "List of hosts to invoke processes on",
      PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* End of list */
    { '\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL }
};

static char *frameworks[] = {
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


static int define_cli(prte_cmd_line_t *cli)
{
    int rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: define_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* we always are used, so just add ours to the end */
    rc = prte_cmd_line_add(cli, cmd_line_init);
    if (PRTE_SUCCESS != rc){
        return rc;
    }

    return PRTE_SUCCESS;
}

static char *strip_quotes(char *p)
{
    char *pout;

    /* strip any quotes around the args */
    if ('\"' == p[0]) {
        pout = strdup(&p[1]);
    } else {
        pout = strdup(p);
    }
    if ('\"' == pout[strlen(pout)- 1]) {
        pout[strlen(pout)-1] = '\0';
    }
    return pout;

}

static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target)
{
    int i, j;
    bool ignore;
    char *p1, *p2, *param;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (NULL != personality &&
        NULL != strstr(personality, "prte")) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    for (i = 0; i < (argc-start); ++i) {
        ignore = true;
        if (0 == strcmp("--prtemca", argv[i])) {
            if (NULL == argv[i+1] || NULL == argv[i+2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = strip_quotes(argv[i+1]);
            p2 = strip_quotes(argv[i+2]);
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
        } else if (0 == strcmp("--mca", argv[i])) {
            if (NULL == argv[i+1] || NULL == argv[i+2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = strip_quotes(argv[i+1]);
            p2 = strip_quotes(argv[i+2]);

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            if (0 == strncmp("prte", p1, strlen("prte"))) {
                ignore = false;
            } else {
                for (j=0; NULL != frameworks[j]; j++) {
                    if (0 == strncmp(p1, frameworks[j], strlen(frameworks[j]))) {
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
            }
            free(p1);
            free(p2);
            i += 2;
        }
    }
    return PRTE_SUCCESS;
}

static int parse_deprecated_cli(char *option, char ***argv, int i)
{
    int rc = PRTE_ERR_NOT_FOUND;
    char **pargs = *argv;
    char *p1, *p2, *tmp, *tmp2, *output;

    /* --display-devel-map  ->  map-by :displaydevel */
    if (0 == strcmp(option, "--display-devel-map")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYDEVEL", true);
    }
    /* --display-map  ->  --map-by :display */
    else if (0 == strcmp(option, "--display-map")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAY", true);
    }
    /* --display-topo  ->  --map-by :displaytopo */
    else if (0 == strcmp(option, "--display-topo")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYTOPO", true);
    }
    /* --display-diffable-map  ->  --map-by :displaydiff */
    else if (0 == strcmp(option, "--display-diff")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYDIFF", true);
    }
    /* --report-bindings  ->  --bind-to :report */
    else if (0 == strcmp(option, "--report-bindings")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", NULL, "REPORT", true);
    }
    /* --display-allocation  ->  --map-by :displayalloc */
    else if (0 == strcmp(option, "--display-allocation")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYALLOC", true);
    }
    /* --do-not-launch  ->   --map-by :donotlaunch*/
    else if (0 == strcmp(option, "--do-not-launch")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DONOTLAUNCH", true);
    }
    /* --do-not-resolve  ->   --map-by :donotresolve*/
    else if (0 == strcmp(option, "--do-not-resolve")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DONOTRESOLVE", true);
    }
    /* --tag-output  ->  --map-by :tagoutput */
    else if (0 == strcmp(option, "--tag-output")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "TAGOUTPUT", true);
    }
    /* --timestamp-output  ->  --map-by :timestampoutput */
    else if (0 == strcmp(option, "--timestamp-output")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "TIMESTAMPOUTPUT", true);
    }
    /* --xml  ->  --map-by :xmloutput */
    else if (0 == strcmp(option, "--xml")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "XMLOUTPUT", true);
    }
    /* -N ->   map-by ppr:N:node */
    else if (0 == strcmp(option, "-N")) {
        prte_asprintf(&p2, "ppr:%s:node", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    }
    /* --map-by socket ->  --map-by package */
    else if (0 == strcmp(option, "--map-by")) {
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i+1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i+1]);  // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i+1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                            p2, tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i+1]);
            pargs[i+1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }
    /* --rank-by socket ->  --rank-by package */
    else if (0 == strcmp(option, "--rank-by")) {
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i+1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i+1]);  // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i+1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                            p2, tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i+1]);
            pargs[i+1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }
    /* --bind-to socket ->  --bind-to package */
    else if (0 == strcmp(option, "--bind-to")) {
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i+1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i+1]);  // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i+1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                            p2, tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i+1]);
            pargs[i+1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }

    return rc;
}

static void register_deprecated_cli(prte_list_t *convertors)
{
    prte_convertor_t *cv;
    char *options[] = {
        "--display-devel-map",
        "--display-map",
        "--display-topo",
        "--display-diff",
        "--report-bindings",
        "--display-allocation",
        "--do-not-launch",
        "--tag-output",
        "--timestamp-output",
        "--xml",
        "-N",
        "--map-by",
        "--rank-by",
        "--bind-to",
        NULL
    };

    cv = PRTE_NEW(prte_convertor_t);
    cv->options = prte_argv_copy(options);
    cv->convert = parse_deprecated_cli;
    prte_list_append(convertors, &cv->super);
}


static int parse_env(prte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline)
{
    int i, n;
    char *param, *p1;
    char *value;
    char **env = *dstenv;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("PRTE_MCA_", srcenv[i], strlen("PRTE_MCA_"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(srcenv[i]);
            p1 = param + strlen("PRTE_MCA_");
            value = strchr(param, '=');
            *value = '\0';
            value++;
            if (cmdline) {
                /* check if it is already present */
                for (n=0; NULL != env[n]; n++) {
                    if (0 == strcmp(env[n], p1)) {
                        /* this param is already given */
                        goto next;
                    }
                }
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                     "%s schizo:prte:parse_env adding %s %s to cmd line",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1, value);
                prte_argv_append_nosize(dstenv, "--prtemca");
                prte_argv_append_nosize(dstenv, p1);
                prte_argv_append_nosize(dstenv, value);
            } else {
                if (environ != srcenv) {
                    /* push it into our environment */
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                         "%s schizo:prte:parse_env pushing %s=%s into my environment",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param, value);
                    prte_setenv(param, value, true, &environ);
                }
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                     "%s schizo:prte:parse_env pushing %s=%s into dest environment",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param, value);
                prte_setenv(param, value, true, dstenv);
            }
          next:
            free(param);
            env = *dstenv;
        }
    }

    return PRTE_SUCCESS;
}

static int setup_fork(prte_job_t *jdata,
                      prte_app_context_t *app)
{
    prte_attribute_t *attr;
    bool exists;
    char *param, *p2, *saveptr;
    int i;

    /* flag that we started this job */
    prte_setenv("PRTE_LAUNCHED", "1", true, &app->env);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PRTE_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t) {
        if (PRTE_JOB_SET_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_JOB_ADD_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_JOB_UNSET_ENVAR == attr->key) {
            prte_unsetenv(attr->data.data.string, &app->env);
        } else if (PRTE_JOB_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prte_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                   attr->data.data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
            }
        } else if (PRTE_JOB_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prte_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                   attr->data.data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PRTE_LIST_FOREACH(attr, &app->attributes, prte_attribute_t) {
        if (PRTE_APP_SET_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_APP_ADD_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_APP_UNSET_ENVAR == attr->key) {
            prte_unsetenv(attr->data.data.string, &app->env);
        } else if (PRTE_APP_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prte_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                   attr->data.data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
            }
        } else if (PRTE_APP_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prte_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                   attr->data.data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
            }
        }
    }

    return PRTE_SUCCESS;
}

static int define_session_dir(char **tmpdir)
{
    int uid;
    pid_t mypid;

    /* setup a session directory based on our userid and pid */
    uid = geteuid();
    mypid = getpid();

    prte_asprintf(tmpdir, "%s/%s.session.%lu.%lu",
                   prte_tmp_directory(), prte_tool_basename,
                   (unsigned long)uid,
                   (unsigned long)mypid);

    return PRTE_SUCCESS;
}

static int detect_proxy(char **argv)
{
    /* if the basename of the cmd was "mpirun" or "mpiexec",
     * we default to us */
    if (prte_schizo_base.test_proxy_launch ||
        0 == strcmp(prte_tool_basename, "prterun")) {
        /* add us to the personalities */
        prte_argv_append_unique_nosize(&prte_schizo_base.personalities, "prte");
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

static int allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    /* we always run last */
    char *r1, *r2;

    if (prte_cmd_line_is_taken(cmd_line, "allow-run-as-root")) {
        return PRTE_SUCCESS;
    }

    if (NULL != (r1 = getenv("PRTE_ALLOW_RUN_AS_ROOT")) &&
        NULL != (r2 = getenv("PRTE_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

static void wrap_args(char **args)
{
    int i;
    char *tstr;

    for (i=0; NULL != args && NULL != args[i]; i++) {
        if (0 == strcmp(args[i], "--prtemca")) {
            if (NULL == args[i+1] || NULL == args[i+2]) {
                /* this should be impossible as the error would
                 * have been detected well before here, but just
                 * be safe */
                return;
            }
            i += 2;
            /* if the argument already has quotes, then leave it alone */
            if ('\"' == args[i][0]) {
                continue;
            }
            prte_asprintf(&tstr, "\"%s\"", args[i]);
            free(args[i]);
            args[i] = tstr;
        }
    }
}

static void parse_proxy_cli(prte_cmd_line_t *cmd_line,
                            char ***argv)
{
    int i, j;
    prte_value_t *pval;
    char **hostfiles = NULL;
    char **hosts = NULL;
    char *ptr;
    char *param, *value;

    /* check for hostfile and/or dash-host options */
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "hostfile"))) {
        prte_argv_append_nosize(argv, "--hostfile");
        for (j=0; j < i; j++) {
            pval = prte_cmd_line_get_param(cmd_line, "hostfile", j, 0);
            if (NULL == pval) {
                break;
            }
            prte_argv_append_nosize(&hostfiles, pval->value.data.string);
        }
        ptr = prte_argv_join(hostfiles, ',');
        prte_argv_free(hostfiles);
        prte_argv_append_nosize(argv, ptr);
        free(ptr);
    }
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "host"))) {
        prte_argv_append_nosize(argv, "--host");
        for (j=0; j < i; j++) {
            pval = prte_cmd_line_get_param(cmd_line, "host", j, 0);
            if (NULL == pval) {
                break;
            }
            prte_argv_append_nosize(&hosts, pval->value.data.string);
        }
        ptr = prte_argv_join(hosts, ',');
        prte_argv_append_nosize(argv, ptr);
        free(ptr);
    }
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "daemonize"))) {
        prte_argv_append_nosize(argv, "--daemonize");
    }
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "set-sid"))) {
        prte_argv_append_nosize(argv, "--set-sid");
    }
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "launch-agent"))) {
        prte_argv_append_nosize(argv, "--launch-agent");
        pval = prte_cmd_line_get_param(cmd_line, "launch-agent", 0, 0);
        prte_argv_append_nosize(argv, pval->value.data.string);
    }
    if (0 < (i = prte_cmd_line_get_ninsts(cmd_line, "max-vm-size"))) {
        prte_argv_append_nosize(argv, "--max-vm-size");
        pval = prte_cmd_line_get_param(cmd_line, "max-vm-size", 0, 0);
        prte_asprintf(&value, "%d", pval->value.data.integer);
        prte_argv_append_nosize(argv, value);
        free(value);
    }
    /* harvest all the MCA params in the environ */
    for (i = 0; NULL != environ[i]; ++i) {
        if (0 == strncmp("PRTE_MCA", environ[i], strlen("PRTE_MCA"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(environ[i]);
            ptr = &param[strlen("PRTE_MCA_")];
            value = strchr(param, '=');
            if (NULL == value) {
                /* should never happen */
                free(param);
                continue;
            }
            *value = '\0';
            value++;
            prte_argv_append_nosize(argv, "--prtemca");
            prte_argv_append_nosize(argv, ptr);
            prte_argv_append_nosize(argv, value);
            free(param);
        }
    }
}

static int check_sanity(prte_cmd_line_t *cmd_line)
{
    prte_value_t *pval;
    int n;
    char **args;
    char *mappers[] = {
        "slot",
        "hwthread",
        "core",
        "l1cache",
        "l2cache",
        "l3cache",
        "package",
        "node",
        "seq",
        "dist",
        "ppr",
        "rankfile",
        NULL
    };
    char *rankers[] = {
        "slot",
        "hwthread",
        "core",
        "l1cache",
        "l2cache",
        "l3cache",
        "package",
        "node",
        NULL
    };
    char *binders[] = {
        "none",
        "hwthread",
        "core",
        "l1cache",
        "l2cache",
        "l3cache",
        "package",
        NULL
    };
    bool good = false;

    if (1 < prte_cmd_line_get_ninsts(cmd_line, "map-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances",
                       true, "map-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "rank-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances",
                       true, "rank-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "bind-to")) {
        prte_show_help("help-schizo-base.txt", "multi-instances",
                       true, "bind-to");
        return PRTE_ERR_SILENT;
    }

    /* quick check that we have valid directives */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "map-by", 0, 0))) {
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            goto rnk;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n=0; NULL != mappers[n]; n++) {
            if (0 == strcasecmp(args[0], mappers[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "mapping", args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(args);
    }

rnk:
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "rank-by", 0, 0))) {
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            goto bnd;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n=0; NULL != rankers[n]; n++) {
            if (0 == strcasecmp(args[0], rankers[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "ranking", args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(args);
    }

bnd:
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "bind-to", 0, 0))) {
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            goto done;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n=0; NULL != binders[n]; n++) {
            if (0 == strcasecmp(args[0], binders[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "binding", args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
       }
        prte_argv_free(args);
    }

done:
    return PRTE_SUCCESS;
}
