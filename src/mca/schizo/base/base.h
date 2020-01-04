/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 * schizo framework base functionality.
 */

#ifndef PRRTE_MCA_SCHIZO_BASE_H
#define PRRTE_MCA_SCHIZO_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#include "src/class/prrte_list.h"
#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/util/cmd_line.h"
#include "src/util/printf.h"
#include "src/mca/mca.h"

#include "src/runtime/prrte_globals.h"

#include "src/mca/schizo/schizo.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_schizo_base_framework;
/* select all components */
PRRTE_EXPORT    int prrte_schizo_base_select(void);

/**
 * Struct to hold data global to the schizo framework
 */
typedef struct {
    /* list of active modules */
    prrte_list_t active_modules;
    char **personalities;
} prrte_schizo_base_t;

/**
 * Global instance of schizo-wide framework data
 */
PRRTE_EXPORT extern prrte_schizo_base_t prrte_schizo_base;

/**
 * Active schizo component / module
 */
typedef struct {
    prrte_list_item_t super;
    int pri;
    prrte_schizo_base_module_t *module;
    prrte_mca_base_component_t *component;
} prrte_schizo_base_active_module_t;
PRRTE_CLASS_DECLARATION(prrte_schizo_base_active_module_t);

/* the base stub functions */
PRRTE_EXPORT int prrte_schizo_base_define_cli(prrte_cmd_line_t *cli);
PRRTE_EXPORT int prrte_schizo_base_parse_cli(int argc, int start, char **argv,
                                             char *personality, char ***target);
PRRTE_EXPORT void prrte_schizo_base_parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                                                    char ***argv);
PRRTE_EXPORT int prrte_schizo_base_parse_env(char *path,
                                             prrte_cmd_line_t *cmd_line,
                                             char **srcenv,
                                             char ***dstenv);
PRRTE_EXPORT int prrte_schizo_base_allow_run_as_root(prrte_cmd_line_t *cmd_line);
PRRTE_EXPORT void prrte_schizo_base_wrap_args(char **args);
PRRTE_EXPORT int prrte_schizo_base_setup_app(prrte_app_context_t *app);
PRRTE_EXPORT int prrte_schizo_base_setup_fork(prrte_job_t *jdata,
                                              prrte_app_context_t *context);
PRRTE_EXPORT int prrte_schizo_base_setup_child(prrte_job_t *jobdat,
                                               prrte_proc_t *child,
                                               prrte_app_context_t *app,
                                               char ***env);
PRRTE_EXPORT int prrte_schizo_base_get_remaining_time(uint32_t *timeleft);
PRRTE_EXPORT void prrte_schizo_base_finalize(void);

END_C_DECLS

#endif
