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
/** @file:
 * schizo framework base functionality.
 */

#ifndef PRTE_MCA_SCHIZO_BASE_H
#define PRTE_MCA_SCHIZO_BASE_H

/*
 * includes
 */
#include "prte_config.h"
#include "types.h"

#include "src/class/prte_list.h"
#include "src/mca/base/prte_mca_base_framework.h"
#include "src/mca/mca.h"
#include "src/util/cmd_line.h"
#include "src/util/printf.h"

#include "src/runtime/prte_globals.h"

#include "src/mca/schizo/schizo.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRTE_EXPORT extern prte_mca_base_framework_t prte_schizo_base_framework;
/* select all components */
PRTE_EXPORT int prte_schizo_base_select(void);

/**
 * Struct to hold data global to the schizo framework
 */
typedef struct {
    /* list of active modules */
    prte_list_t active_modules;
    bool test_proxy_launch;
} prte_schizo_base_t;

/**
 * Global instance of schizo-wide framework data
 */
PRTE_EXPORT extern prte_schizo_base_t prte_schizo_base;

/**
 * Active schizo component / module
 */
typedef struct {
    prte_list_item_t super;
    int pri;
    prte_schizo_base_module_t *module;
    prte_mca_base_component_t *component;
} prte_schizo_base_active_module_t;
PRTE_CLASS_DECLARATION(prte_schizo_base_active_module_t);

/* base support functions */
PRTE_EXPORT int prte_schizo_base_convert(char ***argv, int idx, int ntodelete, char *option,
                                         char *directive, char *modifier, bool report);

/* the base stub functions */

PRTE_EXPORT int prte_schizo_base_parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv,
                                           bool cmdline);
PRTE_EXPORT prte_schizo_base_module_t *prte_schizo_base_detect_proxy(char *cmdpath);

PRTE_EXPORT int prte_schizo_base_setup_app(prte_app_context_t *app);
PRTE_EXPORT int prte_schizo_base_setup_fork(prte_job_t *jdata, prte_app_context_t *context);
PRTE_EXPORT int prte_schizo_base_setup_child(prte_job_t *jobdat, prte_proc_t *child,
                                             prte_app_context_t *app, char ***env);
PRTE_EXPORT void prte_schizo_base_job_info(prte_cmd_line_t *cmdline, void *jobinfo);
PRTE_EXPORT int prte_schizo_base_check_sanity(prte_cmd_line_t *cmdline);
PRTE_EXPORT void prte_schizo_base_finalize(void);
PRTE_EXPORT void prte_schizo_base_root_error_msg(void);
PRTE_EXPORT char *prte_schizo_base_getline(FILE *fp);
PRTE_EXPORT bool prte_schizo_base_check_ini(char *cmdpath, char *file);
PRTE_EXPORT char *prte_schizo_base_strip_quotes(char *p);
PRTE_EXPORT int prte_schizo_base_process_deprecated_cli(prte_cmd_line_t *cmdline, int *argc,
                                                        char ***argv, char **options,
                                                        prte_schizo_convertor_fn_t convert);
PRTE_EXPORT int prte_schizo_base_parse_prte(int argc, int start, char **argv, char ***target);
PRTE_EXPORT int prte_schizo_base_parse_pmix(int argc, int start, char **argv, char ***target);

END_C_DECLS

#endif
