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

static int define_cli(prrte_cmd_line_t *cli);
static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target);
static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(char *path,
                     prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv);
static int allow_run_as_root(prrte_cmd_line_t *cmd_line);
static void wrap_args(char **args);

prrte_schizo_base_module_t prrte_schizo_prrte_module = {
    .define_cli = define_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
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


    /* setup MCA parameters */
    { '\0', "mca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gmca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass global MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prtemca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific PRRTE MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gprtemca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass global PRRTE MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "prteam", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Aggregate PRRTE MCA parameter set file list",
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
    "compress",
    "dl",
    "errmgr",
    "ess",
    "filem",
    "grpcomm",
    "if",
    "installdirs",
    "iof",
    "odls",
    "oob",
    "plm",
    "pstat",
    "ras",
    "reachable",
    "rmaps",
    "rml",
    "routed",
    "rtc",
    "schizo",
    "state"
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

static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target)
{
    int i, j, k;
    bool ignore;
    char *no_dups[] = {
        "grpcomm",
        "odls",
        "rml",
        "routed",
        NULL
    };
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
        if (0 == strcmp("--mca", argv[i]) ||
            0 == strcmp("--gmca", argv[i]) ||
            0 == strcmp("--prtemca", argv[i])) {
            if (NULL == argv[i+1] || NULL == argv[i+2]) {
                /* this is an error */
                return PRRTE_ERR_FATAL;
            }
            /* strip any quotes around the args */
            if ('\"' == argv[i+1][0]) {
                p1 = &argv[i+1][1];
            } else {
                p1 = argv[i+1];
            }
            if ('\"' == p1[strlen(p1)- 1]) {
                p1[strlen(p1)-1] = '\0';
            }
            if ('\"' == argv[i+2][0]) {
                p2 = &argv[i+2][1];
            } else {
                p2 = argv[i+2];
            }
            if ('\"' == p2[strlen(p2)- 1]) {
                p1[strlen(p2)-1] = '\0';
            }

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            if (0 == strcmp("--prtemca", argv[i]) ||
                0 == strncmp("prrte", p1, strlen("prrte"))) {
                ignore = false;
            } else {
                for (j=0; NULL != frameworks[j]; j++) {
                    if (0 == strncmp(p1, frameworks[j], strlen(frameworks[j]))) {
                        ignore = false;
                        break;
                    }
                }
            }
            if (ignore) {
                continue;
            }
            /* see if this is already present so we at least can
             * avoid growing the cmd line with duplicates
             */
            for (j=0; NULL != target && NULL != *target[j]; j++) {
                if (0 == strcmp(p1, *target[j])) {
                    /* already here - if the value is the same,
                     * we can quitely ignore the fact that they
                     * provide it more than once. However, some
                     * frameworks are known to have problems if the
                     * value is different. We don't have a good way
                     * to know this, but we at least make a crude
                     * attempt here to protect ourselves.
                     */
                    if (0 == strcmp(p2, *target[j+1])) {
                        /* values are the same */
                        ignore = true;
                        break;
                    } else {
                        /* values are different - see if this is a problem */
                        for (k=0; NULL != no_dups[k]; k++) {
                            if (0 == strcmp(no_dups[k], p1)) {
                                /* print help message
                                 * and abort as we cannot know which one is correct
                                 */
                                prrte_show_help("help-prrterun.txt", "prrterun:conflicting-params",
                                               true, prrte_tool_basename, p1,
                                               p2, *target[j+1]);
                                return PRRTE_ERR_BAD_PARAM;
                            }
                        }
                        /* this passed muster - just ignore it */
                        ignore = true;
                        break;
                    }
                }
            }
            if (!ignore) {
                if (NULL == target) {
                    /* push it into our environment */
                    asprintf(&param, "PRRTE_MCA_%s", p1);
                    prrte_setenv(param, p2, true, &environ);
                } else {
                    prrte_argv_append_nosize(target, argv[i]);
                    prrte_argv_append_nosize(target, p1);
                    prrte_argv_append_nosize(target, p2);
                }
            }
            i += 2;
        }
    }
    return PRRTE_SUCCESS;
}

static int parse_env(char *path,
                     prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv)
{
    int i, j;
    char *param;
    char *value;
    prrte_value_t *pval;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("PRRTE_", srcenv[i], strlen("PRRTE_"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(srcenv[i]);
            value = strchr(param, '=');
            *value = '\0';
            value++;
            prrte_setenv(param, value, false, dstenv);
            free(param);
        }
    }

    if (NULL == cmd_line) {
        /* we are done */
        return PRRTE_SUCCESS;
    }

    /* Did the user request to export any environment variables on the cmd line? */
    if (prrte_cmd_line_is_taken(cmd_line, "x")) {
        j = prrte_cmd_line_get_ninsts(cmd_line, "x");
        for (i = 0; i < j; ++i) {
            pval = prrte_cmd_line_get_param(cmd_line, "x", i, 0);
            param = pval->data.string;

            if (NULL != (value = strchr(param, '='))) {
                /* terminate the name of the param */
                *value = '\0';
                /* step over the equals */
                value++;
                /* overwrite any prior entry */
                prrte_setenv(param, value, true, dstenv);
                /* save it for any comm_spawn'd apps */
                prrte_setenv(param, value, true, &prrte_forwarded_envars);
            } else {
                value = getenv(param);
                if (NULL != value) {
                    /* overwrite any prior entry */
                    prrte_setenv(param, value, true, dstenv);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(param, value, true, &prrte_forwarded_envars);
                } else {
                    prrte_output(0, "Warning: could not find environment variable \"%s\"\n", param);
                }
            }
        }
    }

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
        if (0 == strcmp(args[i], "--mca") ||
            0 == strcmp(args[i], "--gmca") ||
            0 == strcmp(args[i], "--prtemca")) {
            if (NULL == args[i+1] || NULL == args[i+2]) {
                /* this should be impossible as the error would
                 * have been detected well before here, but just
                 * be safe */
                return;
            }
            i += 2;
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
            *value = '\0';
            value++;
            prrte_argv_append_nosize(argv, "--prtemca");
            prrte_argv_append_nosize(argv, ptr);
            prrte_argv_append_nosize(argv, value);
            free(param);
        }
    }
}
