/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
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

int prrte_schizo_base_parse_cli(int argc, int start, char **argv)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_cli) {
            rc = mod->module->parse_cli(argc, start, argv);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
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
