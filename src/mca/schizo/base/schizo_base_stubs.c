/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
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
#include "src/util/argv.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/mca/schizo/base/base.h"

static int process_deprecated_cli(prrte_cmd_line_t *cmdline,
                                  int *argc, char ***argv,
                                  prrte_list_t *convertors);

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

int prrte_schizo_base_parse_deprecated_cli(prrte_cmd_line_t *cmdline,
                                           int *argc, char ***argv)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;
    prrte_list_t convertors;

    PRRTE_CONSTRUCT(&convertors, prrte_list_t);

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->register_deprecated_cli) {
            mod->module->register_deprecated_cli(&convertors);
        }
    }

    rc = process_deprecated_cli(cmdline, argc, argv, &convertors);
    PRRTE_LIST_DESTRUCT(&convertors);

    return rc;
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

int prrte_schizo_base_parse_env(prrte_cmd_line_t *cmd_line,
                                char **srcenv,
                                char ***dstenv,
                                bool cmdline)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_env) {
            rc = mod->module->parse_env(cmd_line, srcenv, dstenv, cmdline);
            if (PRRTE_SUCCESS != rc && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}

int prrte_schizo_base_detect_proxy(char **argv)
{
    int rc, ret = PRRTE_ERR_TAKE_NEXT_OPTION;
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->detect_proxy) {
            rc = mod->module->detect_proxy(argv);
            if (PRRTE_SUCCESS == rc) {
                ret = PRRTE_SUCCESS;  // indicate we are running as a proxy
            } else if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return ret;
}

int prrte_schizo_base_define_session_dir(char **tmpdir)
{
    int rc;
    prrte_schizo_base_active_module_t *mod;

    *tmpdir = NULL;
    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->define_session_dir) {
            rc = mod->module->define_session_dir(tmpdir);
            if (PRRTE_SUCCESS == rc) {
                return rc;
            }
            if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
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

void prrte_schizo_base_job_info(prrte_cmd_line_t *cmdline, prrte_list_t *jobinfo)
{
    prrte_schizo_base_active_module_t *mod;

    PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
        if (NULL != mod->module->job_info) {
            mod->module->job_info(cmdline, jobinfo);
        }
    }
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


static int process_deprecated_cli(prrte_cmd_line_t *cmdline,
                                  int *argc, char ***argv,
                                  prrte_list_t *convertors)
{
    int pargc;
    char **pargs, *p2;
    int i, n, rc, ret;
    prrte_cmd_line_init_t e;
    prrte_cmd_line_option_t *option;
    bool found;
    prrte_convertor_t *cv;

    pargs = *argv;
    pargc = *argc;
    ret = PRRTE_SUCCESS;

    /* check for deprecated cmd line options */
    for (i=1; NULL != pargs[i]; i++) {
        /* Are we done?  i.e., did we find the special "--" token? */
        if (0 == strcmp(pargs[i], "--")) {
            break;
        }

        /* check for option */
        if ('-' != pargs[i][0]) {
            /* not an option - we are done. Note that options
             * are required to increment past their arguments
             * so we don't mistakenly think we are at the end */
            break;
        }

        if ('-' != pargs[i][1] && 2 < strlen(pargs[i])) {
            /* we know this is incorrect */
            p2 = strdup(pargs[i]);
            free(pargs[i]);
            prrte_asprintf(&pargs[i], "-%s", p2);
            /* if it is the special "-np" option, we silently
             * change it and don't emit an error */
            if (0 == strcmp(p2, "-np")) {
                free(p2);
            } else {
                prrte_show_help("help-schizo-base.txt", "single-dash-error", true,
                                p2, pargs[i]);
                free(p2);
                ret = PRRTE_OPERATION_SUCCEEDED;
            }
        }

        /* is this an argument someone needs to convert? */
        found = false;
        PRRTE_LIST_FOREACH(cv, convertors, prrte_convertor_t) {
            for (n=0; NULL != cv->options[n]; n++) {
                if (0 == strcmp(pargs[i], cv->options[n])) {
                    rc = cv->convert(cv->options[n], argv, i);
                    if (PRRTE_SUCCESS != rc) {
                        return rc;
                    }
                    --i;
                    found = true;
                    ret = PRRTE_OPERATION_SUCCEEDED;
                    pargs = *argv;
                    pargc = prrte_argv_count(pargs);
                    break;  // for loop
                }
            }
            if (found) {
                break;  // foreach loop
            }
        }

        if (!found) {
            /* check for single-dash option */
            if (2 == strlen(pargs[i])) {
                /* find the option */
                memset(&e, 0, sizeof(prrte_cmd_line_init_t));
                e.ocl_cmd_short_name = pargs[i][1];
                option = prrte_cmd_line_find_option(cmdline, &e);

                /* if this isn't an option, then we are done */
                if (NULL == option) {
                    break;
                }

                /* increment past the number of arguments for this option */
                i += option->clo_num_params;
            }
            /* check if we are done */
            else {
                /* find the option */
                memset(&e, 0, sizeof(prrte_cmd_line_init_t));
                e.ocl_cmd_long_name = &pargs[i][2];
                option = prrte_cmd_line_find_option(cmdline, &e);

                /* if this isn't an option, then we are done */
                if (NULL == option) {
                    break;
                }

                /* increment past the number of arguments for this option */
                i += option->clo_num_params;
            }
        }
    }
    *argc = pargc;

    return ret;
}
