/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The Open RTE Personality Framework (schizo)
 *
 * Multi-select framework so that multiple personalities can be
 * simultaneously supported
 *
 */

#ifndef PRRTE_MCA_SCHIZO_H
#define PRRTE_MCA_SCHIZO_H

#include "prrte_config.h"
#include "types.h"
#include "src/util/cmd_line.h"

#include "src/mca/mca.h"

#include "src/runtime/prrte_globals.h"


BEGIN_C_DECLS

/*
 * schizo module functions
 */

/**
* SCHIZO module functions - the modules are accessed via
* the base stub functions
*/

/* initialize the module - allow it to do whatever one-time
 * things it requires */
typedef int (*prrte_schizo_base_module_init_fn_t)(void);

/* provide an opportunity for components to add personality and/or
 * environment-specific command line options. The PRRTE cli tools
 * will add provided options to the CLI definition, and so the
 * resulting CLI array will include the _union_ of options provided
 * by the various components. Where there is overlap (i.e., an option
 * is added that was also defined earlier in the stack), then the
 * first definition is used. This reflects the higher priority of
 * the original definition - note that this only impacts the help
 * message that will be displayed */
typedef int (*prrte_schizo_base_module_define_cli_fn_t)(prrte_cmd_line_t *cli);

/* parse a tool command line
 * starting from the given location according to the cmd line options
 * known to this module's personality. First, of course, check that
 * this module is included in the base array of personalities, or is
 * automatically recognizable! */
typedef int (*prrte_schizo_base_module_parse_cli_fn_t)(int argc, int start,
                                                      char **argv,
                                                      char *personality,
                                                      char ***target);

/* parse the environment for proxy cmd line entries */
typedef void (*prrte_schizo_base_module_parse_proxy_cli_fn_t)(prrte_cmd_line_t *cmd_line,
                                                              char ***argv);

/* parse the environment of the
 * tool to extract any personality-specific envars that need to be
 * forward to the app's environment upon execution */
typedef int (*prrte_schizo_base_module_parse_env_fn_t)(char *path,
                                                      prrte_cmd_line_t *cmd_line,
                                                      char **srcenv,
                                                      char ***dstenv);

/* check if running as root is allowed in this environment */
typedef int (*prrte_schizo_base_module_allow_run_as_root_fn_t)(prrte_cmd_line_t *cmd_line);

/* wrap cmd line args */
typedef void (*prrte_schizo_base_module_wrap_args_fn_t)(char **args);

/* do whatever preparation work
 * is required to setup the app for execution. This is intended to be
 * used by prrterun and other launcher tools to, for example, change
 * an executable's relative-path to an absolute-path, or add a command
 * required for starting a particular kind of application (e.g., adding
 * "java" to start a Java application) */
typedef int (*prrte_schizo_base_module_setup_app_fn_t)(prrte_app_context_t *app);

/* add any personality-specific envars required at the job level prior
 * to beginning to execute local procs */
typedef int (*prrte_schizo_base_module_setup_fork_fn_t)(prrte_job_t *jdata,
                                                       prrte_app_context_t *context);

/* add any personality-specific envars required for this specific local
 * proc upon execution */
typedef int (*prrte_schizo_base_module_setup_child_fn_t)(prrte_job_t *jdata,
                                                        prrte_proc_t *child,
                                                        prrte_app_context_t *app,
                                                        char ***env);


/* give the component a chance to cleanup */
typedef void (*prrte_schizo_base_module_finalize_fn_t)(void);

/* request time remaining in this allocation - only one module
 * capable of supporting this operation should be available
 * in a given environment. However, if a module is available
 * and decides it cannot provide the info in the current situation,
 * then it can return PRRTE_ERR_TAKE_NEXT_OPTION to indicate that
 * another module should be tried */
typedef int (*prrte_schizo_base_module_get_rem_time_fn_t)(uint32_t *timeleft);

/*
 * schizo module version 1.3.0
 */
typedef struct {
    prrte_schizo_base_module_init_fn_t                   init;
    prrte_schizo_base_module_define_cli_fn_t             define_cli;
    prrte_schizo_base_module_parse_cli_fn_t              parse_cli;
    prrte_schizo_base_module_parse_proxy_cli_fn_t        parse_proxy_cli;
    prrte_schizo_base_module_parse_env_fn_t              parse_env;
    prrte_schizo_base_module_allow_run_as_root_fn_t      allow_run_as_root;
    prrte_schizo_base_module_wrap_args_fn_t              wrap_args;
    prrte_schizo_base_module_setup_app_fn_t              setup_app;
    prrte_schizo_base_module_setup_fork_fn_t             setup_fork;
    prrte_schizo_base_module_setup_child_fn_t            setup_child;
    prrte_schizo_base_module_get_rem_time_fn_t           get_remaining_time;
    prrte_schizo_base_module_finalize_fn_t               finalize;
} prrte_schizo_base_module_t;

PRRTE_EXPORT extern prrte_schizo_base_module_t prrte_schizo;

/*
 * schizo component
 */

/**
 * schizo component version 1.3.0
 */
typedef struct {
    /** Base MCA structure */
    prrte_mca_base_component_t base_version;
    /** Base MCA data */
    prrte_mca_base_component_data_t base_data;
} prrte_schizo_base_component_t;

/**
 * Macro for use in components that are of type schizo
 */
#define PRRTE_MCA_SCHIZO_BASE_VERSION_1_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("schizo", 1, 0, 0)


END_C_DECLS

#endif
