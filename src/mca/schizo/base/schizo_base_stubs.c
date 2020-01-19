/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/class/prrte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/mca/schizo/base/base.h"

 int prrte_schizo_base_define_cli(prrte_cmd_line_t *cli)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->define_cli) {
            rc = mod->module->define_cli(cli);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_parse_cli(int argc, int start, char **argv,
                                char *personality, char ***target)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_cli) {
            rc = mod->module->parse_cli(argc, start, argv, personality, target);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

void prrte_schizo_base_parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                                       char ***argv)
{
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_proxy_cli) {
            mod->module->parse_proxy_cli(cmd_line, argv);
        }
    }
}

int prrte_schizo_base_parse_env(char *path,
                               prrte_cmd_line_t *cmd_line,
                               char **srcenv,
                               char ***dstenv)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_env) {
            rc = mod->module->parse_env(path, cmd_line, srcenv, dstenv);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_allow_run_as_root(prrte_cmd_line_t *cmd_line)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->allow_run_as_root) {
            rc = mod->module->allow_run_as_root(cmd_line);
            if (PRRTE_SUCCESS == rc) {
                return rc;
            }
        }
    }

    /* show_help is not yet available, so print an error manually */
    fprintf(stderr, "--------------------------------------------------------------------------\n");
    if (prrte_cmd_line_is_taken(cmd_line, "help")) {
        fprintf(stderr, "%s cannot provide the help message when run as root.\n\n", prrte_tool_basename);
    } else {
        fprintf(stderr, "%s has detected an attempt to run as root.\n\n", prrte_tool_basename);
    }

    fprintf(stderr, "Running as root is *strongly* discouraged as any mistake (e.g., in\n");
    fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
    fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");

    fprintf(stderr, "We strongly suggest that you run %s as a non-root user.\n\n", prrte_tool_basename);

    fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
    fprintf(stderr, "option to your command line.  However, we reiterate our strong advice\n");
    fprintf(stderr, "against doing so - please do so at your own risk.\n");
    fprintf(stderr, "--------------------------------------------------------------------------\n");
    exit(1);
}

void prrte_schizo_base_wrap_args(char **args)
{
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->wrap_args) {
            mod->module->wrap_args(args);
        }
    }
}

int prrte_schizo_base_setup_app(prrte_app_context_t *app)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_app) {
            rc = mod->module->setup_app(app);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_setup_fork(prrte_job_t *jdata,
                                prrte_app_context_t *context)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_fork) {
            rc = mod->module->setup_fork(jdata, context);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_setup_child(prrte_job_t *jdata,
                                 prrte_proc_t *child,
                                 prrte_app_context_t *app,
                                 char ***env)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_child) {
            rc = mod->module->setup_child(jdata, child, app, env);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_get_remaining_time(uint32_t *timeleft)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->get_remaining_time) {
            rc = mod->module->get_remaining_time(timeleft);
            if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRRTE_ERR_NOT_SUPPORTED;
}

void prrte_schizo_base_finalize(void)
{
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->finalize) {
            mod->module->finalize();
        }
    }
}
