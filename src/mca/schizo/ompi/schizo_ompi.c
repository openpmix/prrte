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

prrte_schizo_base_module_t prrte_schizo_ompi_module = {
    .define_cli = define_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
    .allow_run_as_root = allow_run_as_root
};

static char *frameworks[] = {
    /* OPAL frameworks */
    "allocator",
    "backtrace",
    "btl",
    "compress",
    "crs",
    "dl",
    "event",
    "hwloc",
    "if",
    "installdirs",
    "memchecker",
    "memcpy",
    "memory",
    "mpool",
    "patcher",
    "pmix",
    "pstat",
    "rcache",
    "reachable",
    "shmem",
    "timer",
    /* OMPI frameworks */
    "bml",
    "coll",
    "crcp",
    "fbtl",
    "fcoll",
    "fs",
    "hook",
    "io",
    "mtl",
    "op",
    "osc",
    "pml",
    "sharedfp",
    "topo",
    "vprotocol",
    /* OSHMEM frameworks */
    "memheap",
    "scoll",
    "spml",
    "sshmem"
};


/* Cmd-line options common to PRRTE master/daemons/tools */
static prrte_cmd_line_init_t cmd_line_init[] = {

    /* setup MCA parameters */
    { '\0', "omca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific OMPI MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gomca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass global OMPI MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "am", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Aggregate OMPI MCA parameter set file list",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "tune", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Profile options file list for OMPI applications",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Debug options */
    { '\0', "debug-daemons", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Debug daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { 'd', "debug-devel", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of PRRTE",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-daemons-file", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of any PRRTE daemons used by this application, storing output in files",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "leave-session-attached", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not discard stdout/stderr of remote PRTE daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0',  "test-suicide", 1, PRRTE_CMD_LINE_TYPE_BOOL,
      "Suicide instead of clean abort after delay",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* DVM-specific options */
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
    /* uri of PMIx publish/lookup server, or at least where to get it */
    { '\0', "ompi-server", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* fwd mpirun port */
    { '\0', "fwd-mpirun-port", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Forward mpirun port to compute node daemons so all will use it",
      PRRTE_CMD_LINE_OTYPE_DVM },


    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static int define_cli(prrte_cmd_line_t *cli)
{
    int rc, i;
    bool takeus = false;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: define_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prrte_schizo_base.personalities) {
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "ompi")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    rc = prrte_cmd_line_add(cli, cmd_line_init);
    if (PRRTE_SUCCESS != rc){
        return rc;
    }

    /* see if we were given a location where we can get
     * the list of OMPI frameworks */

    return PRRTE_SUCCESS;
}

static int parse_cli(int argc, int start, char **argv,
                     char *personality, char ***target)
{
    int i, j;
    bool takeus = false;
    char *ptr, *param, *p1, *p2;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prrte_schizo_base.personalities) {
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "ompi")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    for (i = 0; i < (argc-start); ++i) {
        if (0 == strcmp("--mca", argv[i]) ||
            0 == strcmp("--gmca", argv[i])) {
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
            if (0 == strncmp("opal", p1, strlen("opal"))) {
                /* this is a base (non-framework) parameter - we need to
                 * convert it to the PRRTE equivalent */
                ptr = strchr(p1, '_');
                ++ptr;  // step over the '_'
                if (NULL == target) {
                    prrte_asprintf(&param, "PRRTE_MCA_prrte_%s", ptr);
                    prrte_setenv(param, p2, true, &environ);
                } else {
                    prrte_asprintf(&param, "prrte_%s", ptr);
                    prrte_argv_append_nosize(target, param);
                }
                free(param);
                /* and also push it as an OMPI param in case
                 * it has to apply over there as well */
                prrte_asprintf(&param, "OMPI_MCA_%s", p1);
                prrte_setenv(param, p2, true, &environ);
                free(param);
            } else if (0 == strncmp("orte", p1, strlen("orte"))) {
                /* this is a base (non-framework) parameter - we need to
                 * convert it to the PRRTE equivalent */
                ptr = strchr(p1, '_');
                ++ptr;  // step over the '_'
                if (NULL == target) {
                    prrte_asprintf(&param, "PRRTE_MCA_prrte_%s", ptr);
                    prrte_setenv(param, p2, true, &environ);
                } else {
                    prrte_asprintf(&param, "prrte_%s", ptr);
                    prrte_argv_append_nosize(target, param);
                }
                free(param);
            } else if (0 == strncmp("ompi", p1, strlen("ompi"))) {
                /* just push it into the environment - we will pick it up later */
               ptr = strchr(p1, '_');
                ++ptr;  // step over the '_'
                prrte_asprintf(&param, "OMPI_MCA_prrte_%s", ptr);
                prrte_setenv(param, p2, true, &environ);
                free(param);
             } else {
                for (j=0; NULL != frameworks[j]; j++) {
                    if (0 == strncmp(p1, frameworks[j], strlen(frameworks[j]))) {
                        /* push them into the environment, we will pick
                         * them up later */
                        prrte_asprintf(&param, "OMPI_MCA_%s", p1);
                        prrte_setenv(param, p2, true, &environ);
                        free(param);
                    }
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
    char *env_set_flag;
    char **vars;
    bool takeus = false;
    prrte_value_t *pval;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prrte_schizo_base.personalities) {
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "ompi")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("OMPI_", srcenv[i], 5)) {
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

    /* set necessary env variables for external usage from tune conf file*/
    int set_from_file = 0;
    vars = NULL;
    if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list_from_file(&vars) &&
            NULL != vars) {
        for (i=0; NULL != vars[i]; i++) {
            value = strchr(vars[i], '=');
            /* terminate the name of the param */
            *value = '\0';
            /* step over the equals */
            value++;
            /* overwrite any prior entry */
            prrte_setenv(vars[i], value, true, dstenv);
            /* save it for any comm_spawn'd apps */
            prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
        }
        set_from_file = 1;
        prrte_argv_free(vars);
    }
    /* Did the user request to export any environment variables on the cmd line? */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (prrte_cmd_line_is_taken(cmd_line, "x")) {
        if (NULL != env_set_flag) {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
        j = prrte_cmd_line_get_ninsts(cmd_line, "x");
        for (i = 0; i < j; ++i) {
            pval = prrte_cmd_line_get_param(cmd_line, "x", i, 0);
            param = strdup(pval->data.string);
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
            free(param);
        }
    } else if (NULL != env_set_flag) {
        /* if mca_base_env_list was set, check if some of env vars were set via -x from a conf file.
         * If this is the case, error out.
         */
        if (!set_from_file) {
            /* set necessary env variables for external usage */
            vars = NULL;
            if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list(env_set_flag, &vars) &&
                    NULL != vars) {
                for (i=0; NULL != vars[i]; i++) {
                    value = strchr(vars[i], '=');
                    /* terminate the name of the param */
                    *value = '\0';
                    /* step over the equals */
                    value++;
                    /* overwrite any prior entry */
                    prrte_setenv(vars[i], value, true, dstenv);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
                }
                prrte_argv_free(vars);
            }
        } else {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
    }

    /* If the user specified --path, store it in the user's app
       environment via the OMPI_exec_path variable. */
    if (NULL != path) {
        prrte_asprintf(&value, "OMPI_exec_path=%s", path);
        prrte_argv_append_nosize(dstenv, value);
        /* save it for any comm_spawn'd apps */
        prrte_argv_append_nosize(&prrte_forwarded_envars, value);
        free(value);
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

    if (NULL != (r1 = getenv("OMPI_ALLOW_RUN_AS_ROOT")) &&
        NULL != (r2 = getenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_TAKE_NEXT_OPTION;
}

static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv)
{
    prrte_value_t *pval;

    if (NULL != (pval = prrte_cmd_line_get_param(cmd_line, "orte_tmpdir_base", 0, 0))) {
        prrte_argv_append_nosize(argv, "--prtemca");
        prrte_argv_append_nosize(argv, "prrte_tmpdir_base");
        prrte_argv_append_nosize(argv, pval->data.string);
    }
}
