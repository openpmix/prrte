/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The PRTE Personality Framework (schizo)
 *
 * Multi-select framework so that multiple personalities can be
 * simultaneously supported
 *
 */

#ifndef PRTE_MCA_SCHIZO_H
#define PRTE_MCA_SCHIZO_H

#include "prte_config.h"
#include "types.h"

#include "src/class/prte_list.h"
#include "src/util/cmd_line.h"

#include "src/mca/mca.h"

#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

typedef int (*prte_schizo_convertor_fn_t)(char *option, char ***argv, int idx);

/*
 * schizo module functions
 */

/**
 * SCHIZO module functions - the modules are accessed via
 * the base stub functions
 */

/* initialize the module - allow it to do whatever one-time
 * things it requires */
typedef int (*prte_schizo_base_module_init_fn_t)(void);

/* provide an opportunity for components to add personality and/or
 * environment-specific command line options. The PRTE cli tools
 * will add provided options to the CLI definition, and so the
 * resulting CLI array will include the _union_ of options provided
 * by the various components. Where there is overlap (i.e., an option
 * is added that was also defined earlier in the stack), then the
 * first definition is used. This reflects the higher priority of
 * the original definition - note that this only impacts the help
 * message that will be displayed */
typedef int (*prte_schizo_base_module_define_cli_fn_t)(prte_cmd_line_t *cli);

/* parse a tool command line
 * starting from the given location according to the cmd line options
 * known to this module's personality. First, of course, check that
 * this module is included in the base array of personalities, or is
 * automatically recognizable! */
typedef int (*prte_schizo_base_module_parse_cli_fn_t)(int argc, int start, char **argv,
                                                      char ***target);

typedef int (*prte_schizo_base_parse_deprecated_cli_fn_t)(prte_cmd_line_t *cmdline, int *argc,
                                                          char ***argv);

/* detect if we are running as a proxy
 * Check the environment to determine what, if any, host we are running
 * under. Check the argv to see if we are running as a proxy for some
 * other command and to see which environment we are proxying. Return
 * a priority indicating the level of confidence this component has
 * that it is the proxy, with 100 being a definitive "yes". Highest
 * confidence wins.
 */
typedef int (*prte_schizo_base_detect_proxy_fn_t)(char *cmdpath);

/* parse the environment of the
 * tool to extract any personality-specific envars that need to be
 * forward to the app's environment upon execution */
typedef int (*prte_schizo_base_module_parse_env_fn_t)(prte_cmd_line_t *cmd_line, char **srcenv,
                                                      char ***dstenv, bool cmdline);

/* check if running as root is allowed in this environment */
typedef void (*prte_schizo_base_module_allow_run_as_root_fn_t)(prte_cmd_line_t *cmd_line);

/* do whatever preparation work
 * is required to setup the app for execution. This is intended to be
 * used by prun and other launcher tools to, for example, change
 * an executable's relative-path to an absolute-path, or add a command
 * required for starting a particular kind of application (e.g., adding
 * "java" to start a Java application) */
typedef int (*prte_schizo_base_module_setup_app_fn_t)(prte_app_context_t *app);

/* add any personality-specific envars required at the job level prior
 * to beginning to execute local procs */
typedef int (*prte_schizo_base_module_setup_fork_fn_t)(prte_job_t *jdata,
                                                       prte_app_context_t *context);

/* add any personality-specific envars required for this specific local
 * proc upon execution */
typedef int (*prte_schizo_base_module_setup_child_fn_t)(prte_job_t *jdata, prte_proc_t *child,
                                                        prte_app_context_t *app, char ***env);

/* give the component a chance to cleanup */
typedef void (*prte_schizo_base_module_finalize_fn_t)(void);

/* give the components a chance to add job info */
typedef void (*prte_schizo_base_module_job_info_fn_t)(prte_cmd_line_t *cmdline, void *jobinfo);

/* give the components a chance to check sanity */
typedef int (*prte_schizo_base_module_check_sanity_fn_t)(prte_cmd_line_t *cmdline);

/*
 * schizo module version 1.3.0
 */
typedef struct {
    char *name;
    prte_schizo_base_module_init_fn_t init;
    prte_schizo_base_module_define_cli_fn_t define_cli;
    prte_schizo_base_module_parse_cli_fn_t parse_cli;
    prte_schizo_base_parse_deprecated_cli_fn_t parse_deprecated_cli;
    prte_schizo_base_module_parse_env_fn_t parse_env;
    prte_schizo_base_detect_proxy_fn_t detect_proxy;
    prte_schizo_base_module_allow_run_as_root_fn_t allow_run_as_root;
    prte_schizo_base_module_setup_app_fn_t setup_app;
    prte_schizo_base_module_setup_fork_fn_t setup_fork;
    prte_schizo_base_module_setup_child_fn_t setup_child;
    prte_schizo_base_module_job_info_fn_t job_info;
    prte_schizo_base_module_check_sanity_fn_t check_sanity;
    prte_schizo_base_module_finalize_fn_t finalize;
} prte_schizo_base_module_t;

typedef prte_schizo_base_module_t *(*prte_schizo_API_detect_proxy_fn_t)(char *cmdpath);

typedef struct {
    prte_schizo_base_module_init_fn_t init;
    prte_schizo_base_module_parse_env_fn_t parse_env;
    prte_schizo_API_detect_proxy_fn_t detect_proxy;
    prte_schizo_base_module_setup_app_fn_t setup_app;
    prte_schizo_base_module_setup_fork_fn_t setup_fork;
    prte_schizo_base_module_setup_child_fn_t setup_child;
    prte_schizo_base_module_job_info_fn_t job_info;
    prte_schizo_base_module_check_sanity_fn_t check_sanity;
    prte_schizo_base_module_finalize_fn_t finalize;
} prte_schizo_API_module_t;

PRTE_EXPORT extern prte_schizo_API_module_t prte_schizo;
/*
 * schizo component
 */

/**
 * schizo component version 1.3.0
 */
typedef struct {
    /** Base MCA structure */
    prte_mca_base_component_t base_version;
    /** Base MCA data */
    prte_mca_base_component_data_t base_data;
} prte_schizo_base_component_t;

/**
 * Macro for use in components that are of type schizo
 */
#define PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0 PRTE_MCA_BASE_VERSION_2_1_0("schizo", 1, 0, 0)

END_C_DECLS

#endif
