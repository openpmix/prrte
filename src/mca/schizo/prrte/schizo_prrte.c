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
 * Copyright (c) 2009-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/schizo/base/base.h"
#include "schizo_prrte.h"

static int define_cli(prrte_cmd_line_t *cli);
static void register_deprecated_cli(prrte_list_t *convertors);
static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target);
static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static int setup_fork(prrte_job_t *jdata,
                      prrte_app_context_t *context);
static int detect_proxy(char **argv, char **rfile);
static int define_session_dir(char **tmpdir);
static int allow_run_as_root(prrte_cmd_line_t *cmd_line);
static void wrap_args(char **args);

prrte_schizo_base_module_t prrte_schizo_prrte_module = {
    .define_cli = define_cli,
    .register_deprecated_cli = register_deprecated_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .detect_proxy = detect_proxy,
    .define_session_dir = define_session_dir,
    .allow_run_as_root = allow_run_as_root,
    .wrap_args = wrap_args
};


/* Cmd-line options common to PRRTE master/daemons/tools */
static prrte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" generalized options */
    { 'h', "help", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "This help message", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'V', "version", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Print version and exit", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'v', "verbose", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Be verbose", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { 'q', "quiet", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Suppress helpful messages", PRRTE_CMD_LINE_OTYPE_GENERAL },

      /* DVM-related options */
    { '\0', "report-pid", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Printout pid on stdout [-], stderr [+], or a file [anything else]",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "report-uri", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Printout URI on stdout [-], stderr [+], or a file [anything else]",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "tmpdir", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Set the root for the session directory tree",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "allow-run-as-root", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Allow execution as root (STRONGLY DISCOURAGED)",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "personality", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Comma-separated list of programming model, languages, and containers being used (default=\"prrte\")",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prefix", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Prefix to be used to look for PRRTE executables",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "noprefix", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Disable automatic --prefix behavior",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "daemonize", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Daemonize the DVM daemons into the background",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "set-sid", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Direct the DVM daemons to separate from the current session",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* Specify the launch agent to be used */
    { '\0', "launch-agent", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Name of daemon executable used to start processes on remote nodes (default: prted)",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* maximum size of VM - typically used to subdivide an allocation */
    { '\0', "max-vm-size", 1, PRRTE_CMD_LINE_TYPE_INT,
      "Number of daemons to start",
      PRRTE_CMD_LINE_OTYPE_DVM },


    /* setup MCA parameters */
    { '\0', "mca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prtemca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific PRRTE MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Request parseable help output */
    { '\0', "prrte_info_pretty", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "When used in conjunction with other parameters, the output is displayed in 'prrte_info_prettyprint' format (default)",
      PRRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parsable", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "When used in conjunction with other parameters, the output is displayed in a machine-parsable format",
       PRRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parseable", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Synonym for --parsable",
      PRRTE_CMD_LINE_OTYPE_GENERAL },

    /* Set a hostfile */
    { '\0', "hostfile", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "machinefile", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "default-hostfile", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a default hostfile",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { 'H', "host", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "List of hosts to invoke processes on",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static char *frameworks[] = {
    "backtrace",
    "prtecompress",
    "prtedl",
    "errmgr",
    "ess",
    "filem",
    "grpcomm",
    "prteif",
    "prteinstalldirs",
    "iof",
    "odls",
    "oob",
    "plm",
    "pstat",
    "ras",
    "prtereachable",
    "rmaps",
    "rml",
    "routed",
    "rtc",
    "schizo",
    "state",
    NULL,
};


static int define_cli(prrte_cmd_line_t *cli)
{
    int rc;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: define_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* we always are used, so just add ours to the end */
    rc = prrte_cmd_line_add(cli, cmd_line_init);
    if (PRRTE_SUCCESS != rc){
        return rc;
    }

    return PRRTE_SUCCESS;
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

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: parse_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    if (NULL != personality &&
        NULL != strstr(personality, "prrte")) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    for (i = 0; i < (argc-start); ++i) {
        ignore = true;
        if (0 == strcmp("--prtemca", argv[i])) {
            if (NULL == argv[i+1] || NULL == argv[i+2]) {
                /* this is an error */
                return PRRTE_ERR_FATAL;
            }
            p1 = strip_quotes(argv[i+1]);
            p2 = strip_quotes(argv[i+2]);
            if (NULL == target) {
                /* push it into our environment */
                asprintf(&param, "PRRTE_MCA_%s", p1);
                prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                     "%s schizo:prrte:parse_cli pushing %s into environment",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param);
                prrte_setenv(param, p2, true, &environ);
            } else {
                prrte_argv_append_nosize(target, "--prtemca");
                prrte_argv_append_nosize(target, p1);
                prrte_argv_append_nosize(target, p2);
            }
            free(p1);
            free(p2);
        } else if (0 == strcmp("--mca", argv[i])) {
            if (NULL == argv[i+1] || NULL == argv[i+2]) {
                /* this is an error */
                return PRRTE_ERR_FATAL;
            }
            p1 = strip_quotes(argv[i+1]);
            p2 = strip_quotes(argv[i+2]);

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            if (0 == strncmp("prrte", p1, strlen("prrte"))) {
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
                    asprintf(&param, "PRRTE_MCA_%s", p1);
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:prrte:parse_cli pushing %s into environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param);
                    prrte_setenv(param, p2, true, &environ);
                } else {
                    prrte_argv_append_nosize(target, "--prtemca");
                    prrte_argv_append_nosize(target, p1);
                    prrte_argv_append_nosize(target, p2);
                }
            }
            free(p1);
            free(p2);
            i += 2;
        }
    }
    return PRRTE_SUCCESS;
}

static int parse_deprecated_cli(char *option, char ***argv, int i)
{
    int rc = PRRTE_SUCCESS;

    /* --display-devel-map  ->  PRRTE_MCA_rmaps_base_display_devel_map */
    if (0 == strcmp(option, "--display-devel-map")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYDEVEL");
    }
    /* --display-map  ->  PRRTE_MCA_rmaps_base_display_map */
    else if (0 == strcmp(option, "--display-map")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAY");
    }
    /* --display-topo  ->  PRRTE_MCA_rmaps_base_display_topo_with_map */
    else if (0 == strcmp(option, "--display-topo")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYTOPO");
    }
    /* --display-diffable-map  ->  PRRTE_MCA_rmaps_base_display_diffable_map */
    else if (0 == strcmp(option, "--display-diff")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYDIFF");
    }
    /* --report-bindings  ->  PRRTE_MCA_hwloc_base_report_bindings */
    else if (0 == strcmp(option, "--report-bindings")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--bind-to", NULL, "REPORT");
    }
    /* --display-allocation  ->  PRRTE_MCA_prrte_display_alloc */
    else if (0 == strcmp(option, "--display-allocation")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DISPLAYALLOC");
    }
    /* --do-not-launch  ->   */
    else if (0 == strcmp(option, "--do-not-launch")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DONOTLAUNCH");
    }
    /* --xml-output  ->   */
    else if (0 == strcmp(option, "--xml-output")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "XMLOUTPUT");
    }

    return rc;
}

static void register_deprecated_cli(prrte_list_t *convertors)
{
    prrte_convertor_t *cv;
    char *options[] = {
        "--display-devel-map",
        "--display-map",
        "--display-topo",
        "--display-diff",
        "--report-bindings",
        "--display-allocation",
        "--do-not-launch",
        "--xml-output",
        NULL
    };

    cv = PRRTE_NEW(prrte_convertor_t);
    cv->options = prrte_argv_copy(options);
    cv->convert = parse_deprecated_cli;
    prrte_list_append(convertors, &cv->super);
}


static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline)
{
    int i, n;
    char *param, *p1;
    char *value;
    char **env = *dstenv;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("PRRTE_MCA_", srcenv[i], strlen("PRRTE_MCA_"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(srcenv[i]);
            p1 = param + strlen("PRRTE_MCA_");
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
                prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                     "%s schizo:prrte:parse_env adding %s %s to cmd line",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), p1, value);
                prrte_argv_append_nosize(dstenv, "--prtemca");
                prrte_argv_append_nosize(dstenv, p1);
                prrte_argv_append_nosize(dstenv, value);
            } else {
                if (environ != srcenv) {
                    /* push it into our environment */
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:prrte:parse_env pushing %s=%s into my environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param, value);
                    prrte_setenv(param, value, true, &environ);
                }
                if (NULL != dstenv) {
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:prrte:parse_env pushing %s=%s into dest environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param, value);
                    prrte_setenv(param, value, true, dstenv);
                }
            }
          next:
            free(param);
            env = *dstenv;
        }
    }

    return PRRTE_SUCCESS;
}

static int setup_fork(prrte_job_t *jdata,
                      prrte_app_context_t *app)
{
    prrte_attribute_t *attr;
    bool exists;
    char *param, *p2, *saveptr;
    int i;

    /* flag that we started this job */
    prrte_setenv("PRRTE_LAUNCHED", "1", true, &app->env);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PRRTE_LIST_FOREACH(attr, &jdata->attributes, prrte_attribute_t) {
        if (PRRTE_JOB_SET_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
        } else if (PRRTE_JOB_ADD_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, false, &app->env);
        } else if (PRRTE_JOB_UNSET_ENVAR == attr->key) {
            prrte_unsetenv(attr->data.string, &app->env);
        } else if (PRRTE_JOB_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", attr->data.envar.value,
                                   attr->data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        } else if (PRRTE_JOB_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", param, attr->data.envar.separator,
                                   attr->data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PRRTE_LIST_FOREACH(attr, &app->attributes, prrte_attribute_t) {
        if (PRRTE_APP_SET_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
        } else if (PRRTE_APP_ADD_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, false, &app->env);
        } else if (PRRTE_APP_UNSET_ENVAR == attr->key) {
            prrte_unsetenv(attr->data.string, &app->env);
        } else if (PRRTE_APP_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", attr->data.envar.value,
                                   attr->data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        } else if (PRRTE_APP_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", param, attr->data.envar.separator,
                                   attr->data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        }
    }

    return PRRTE_SUCCESS;
}

static int detect_proxy(char **argv, char **rfile)
{
    pid_t mypid;

    mypid = getpid();
    /* if the basename isn't prun, and nobody before us recognized
     * it, then we need to treat it as a proxy */
    if (prrte_schizo_base.test_proxy_launch) {
        /* create a rendezvous file */
        prrte_asprintf(rfile, "%s.rndz.%lu", prrte_tool_basename, (unsigned long)mypid);
        return PRRTE_SUCCESS;
    }

    return PRRTE_ERR_TAKE_NEXT_OPTION;
}

static int define_session_dir(char **tmpdir)
{
    int uid;
    pid_t mypid;

    /* setup a session directory based on our userid and pid */
    uid = geteuid();
    mypid = getpid();

    prrte_asprintf(tmpdir, "%s/%s.session.%lu.%lu",
                   prrte_tmp_directory(), prrte_tool_basename,
                   (unsigned long)uid,
                   (unsigned long)mypid);

    return PRRTE_SUCCESS;
}

static int allow_run_as_root(prrte_cmd_line_t *cmd_line)
{
    /* we always run last */
    char *r1, *r2;

    if (prrte_cmd_line_is_taken(cmd_line, "allow_run_as_root")) {
        return PRRTE_SUCCESS;
    }

    if (NULL != (r1 = getenv("PRRTE_ALLOW_RUN_AS_ROOT")) &&
        NULL != (r2 = getenv("PRRTE_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_TAKE_NEXT_OPTION;
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
            prrte_asprintf(&tstr, "\"%s\"", args[i]);
            free(args[i]);
            args[i] = tstr;
        }
    }
}

static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv)
{
    int i, j;
    prrte_value_t *pval;
    char **hostfiles = NULL;
    char **hosts = NULL;
    char *ptr;
    char *param, *value;

    /* check for hostfile and/or dash-host options */
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "hostfile"))) {
        prrte_argv_append_nosize(argv, "--hostfile");
        for (j=0; j < i; j++) {
            pval = prrte_cmd_line_get_param(cmd_line, "hostfile", j, 0);
            if (NULL == pval) {
                break;
            }
            prrte_argv_append_nosize(&hostfiles, pval->data.string);
        }
        ptr = prrte_argv_join(hostfiles, ',');
        prrte_argv_free(hostfiles);
        prrte_argv_append_nosize(argv, ptr);
        free(ptr);
    }
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "host"))) {
        prrte_argv_append_nosize(argv, "--host");
        for (j=0; j < i; j++) {
            pval = prrte_cmd_line_get_param(cmd_line, "host", j, 0);
            if (NULL == pval) {
                break;
            }
            prrte_argv_append_nosize(&hosts, pval->data.string);
        }
        ptr = prrte_argv_join(hosts, ',');
        prrte_argv_append_nosize(argv, ptr);
        free(ptr);
    }
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "daemonize"))) {
        prrte_argv_append_nosize(argv, "--daemonize");
    }
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "set-sid"))) {
        prrte_argv_append_nosize(argv, "--set-sid");
    }
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "launch-agent"))) {
        prrte_argv_append_nosize(argv, "--launch-agent");
        pval = prrte_cmd_line_get_param(cmd_line, "launch-agent", 0, 0);
        prrte_argv_append_nosize(argv, pval->data.string);
    }
    if (0 < (i = prrte_cmd_line_get_ninsts(cmd_line, "max-vm-size"))) {
        prrte_argv_append_nosize(argv, "--max-vm-size");
        pval = prrte_cmd_line_get_param(cmd_line, "max-vm-size", 0, 0);
        prrte_asprintf(&value, "%d", pval->data.integer);
        prrte_argv_append_nosize(argv, value);
        free(value);
    }
    /* harvest all the MCA params in the environ */
    for (i = 0; NULL != environ[i]; ++i) {
        if (0 == strncmp("PRRTE_MCA", environ[i], strlen("PRRTE_MCA"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(environ[i]);
            ptr = &param[strlen("PRRTE_MCA_")];
            value = strchr(param, '=');
            if (NULL == value) {
                /* should never happen */
                free(param);
                continue;
            }
            *value = '\0';
            value++;
            prrte_argv_append_nosize(argv, "--prtemca");
            prrte_argv_append_nosize(argv, ptr);
            prrte_argv_append_nosize(argv, value);
            free(param);
        }
    }
}
