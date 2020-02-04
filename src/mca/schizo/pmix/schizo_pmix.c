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
#include "schizo_pmix.h"

static int define_cli(prrte_cmd_line_t *cli);
static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target);
static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static void wrap_args(char **args);

prrte_schizo_base_module_t prrte_schizo_pmix_module = {
    .define_cli = define_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
    .wrap_args = wrap_args
};


/* Cmd-line options common to PRRTE master/daemons/tools */
static prrte_cmd_line_init_t cmd_line_init[] = {
    /* setup MCA parameters */
    { '\0', "pmixmca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific PMIx MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gpmixmca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass global PMIx MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "pmixam", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Aggregate PMIx MCA parameter set file list",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static char *frameworks[] = {
    "bfrops",
    "gds",
    "pcompress",
    "pdl",
    "pfexec",
    "pif",
    "pinstalldirs",
    "plog",
    "pmdl",
    "pnet",
    "preg",
    "psec",
    "psensor",
    "pshmem",
    "psquash",
    "ptl",
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

static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target)
{
    int i, j;
    bool ignore;
    char *p1, *p2, *param;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:pmix: parse_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    if (NULL != personality &&
        NULL != strstr(personality, "pmix")) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    for (i = 0; i < (argc-start); ++i) {
        ignore = true;
        if (0 == strcmp("--pmixmca", argv[i]) ||
            0 == strcmp("--gpmixmca", argv[i])) {
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
                p2[strlen(p2)-1] = '\0';
            }
            if (NULL == target) {
                /* push it into our environment */
                asprintf(&param, "PMIX_MCA_%s", p1);
                prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                     "%s schizo:pmix:parse_cli pushing %s into environment",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param);
                prrte_setenv(param, p2, true, &environ);
            } else {
                prrte_argv_append_nosize(target, argv[i]);
                prrte_argv_append_nosize(target, p1);
                prrte_argv_append_nosize(target, p2);
            }
        } else if (0 == strcmp("--mca", argv[i]) ||
                   0 == strcmp("--gmca", argv[i])) {
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
                p2[strlen(p2)-1] = '\0';
            }

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            if (0 == strncmp("pmix", p1, strlen("pmix"))) {
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
                    asprintf(&param, "PMIX_MCA_%s", p1);
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:pmix:parse_cli pushing %s into environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param);
                    prrte_setenv(param, p2, true, &environ);
                } else {
                    prrte_argv_append_nosize(target, "--pmixmca");
                    prrte_argv_append_nosize(target, p1);
                    prrte_argv_append_nosize(target, p2);
                }
            }
            i += 2;
        }
    }
    return PRRTE_SUCCESS;
}

static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline)
{
    int i;
    char *param, *p1;
    char *value;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:pmix: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("PMIX_MCA_", srcenv[i], strlen("PMIX_MCA_"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(srcenv[i]);
            p1 = param + strlen("PMIX_MCA_");
            value = strchr(param, '=');
            *value = '\0';
            value++;
            if (cmdline) {
                prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                     "%s schizo:pmix:parse_env adding %s %s to cmd line",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), p1, value);
                prrte_argv_append_nosize(dstenv, "--pmixmca");
                prrte_argv_append_nosize(dstenv, p1);
                prrte_argv_append_nosize(dstenv, value);
            } else {
                if (environ != srcenv) {
                    /* push it into our environment */
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:pmix:parse_env pushing %s=%s into my environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param, value);
                    prrte_setenv(param, value, true, &environ);
                }
                if (NULL != dstenv) {
                    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                                         "%s schizo:pmix:parse_env pushing %s=%s into dest environment",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), param, value);
                    prrte_setenv(param, value, true, dstenv);
                }
            }
        }
    }
    return PRRTE_SUCCESS;
}

static void wrap_args(char **args)
{
    int i;
    char *tstr;

    for (i=0; NULL != args && NULL != args[i]; i++) {
        if (0 == strcmp(args[i], "--pmixmca")) {
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
    int i;
    char *ptr;
    char *param, *value;

    /* harvest all the MCA params in the environ */
    for (i = 0; NULL != environ[i]; ++i) {
        if (0 == strncmp("PMIX_MCA", environ[i], strlen("PMIX_MCA"))) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(environ[i]);
            ptr = &param[strlen("PMIX_MCA_")];
            value = strchr(param, '=');
            *value = '\0';
            value++;
            prrte_argv_append_nosize(argv, "--pmixmca");
            prrte_argv_append_nosize(argv, ptr);
            prrte_argv_append_nosize(argv, value);
            free(param);
        }
    }
}
