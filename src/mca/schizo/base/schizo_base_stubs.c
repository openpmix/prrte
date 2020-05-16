/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prte_config.h"
#include "constants.h"

#include "src/class/prte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/argv.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/mca/schizo/base/base.h"

static int process_deprecated_cli(prte_cmd_line_t *cmdline,
                                  int *argc, char ***argv,
                                  prte_list_t *convertors);

 int prte_schizo_base_define_cli(prte_cmd_line_t *cli)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->define_cli) {
            rc = mod->module->define_cli(cli);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_parse_cli(int argc, int start, char **argv,
                                char *personality, char ***target)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_cli) {
            rc = mod->module->parse_cli(argc, start, argv, personality, target);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_parse_deprecated_cli(prte_cmd_line_t *cmdline,
                                           int *argc, char ***argv)
{
    int rc;
    prte_schizo_base_active_module_t *mod;
    prte_list_t convertors;

    PRTE_CONSTRUCT(&convertors, prte_list_t);

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->register_deprecated_cli) {
            mod->module->register_deprecated_cli(&convertors);
        }
    }

    rc = process_deprecated_cli(cmdline, argc, argv, &convertors);
    PRTE_LIST_DESTRUCT(&convertors);

    return rc;
}

void prte_schizo_base_parse_proxy_cli(prte_cmd_line_t *cmd_line,
                                       char ***argv)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_proxy_cli) {
            mod->module->parse_proxy_cli(cmd_line, argv);
        }
    }
}

int prte_schizo_base_parse_env(prte_cmd_line_t *cmd_line,
                                char **srcenv,
                                char ***dstenv,
                                bool cmdline)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->parse_env) {
            rc = mod->module->parse_env(cmd_line, srcenv, dstenv, cmdline);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_detect_proxy(char **argv)
{
    int rc, ret = PRTE_ERR_TAKE_NEXT_OPTION;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->detect_proxy) {
            rc = mod->module->detect_proxy(argv);
            if (PRTE_SUCCESS == rc) {
                ret = PRTE_SUCCESS;  // indicate we are running as a proxy
            } else if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return ret;
}

int prte_schizo_base_define_session_dir(char **tmpdir)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    *tmpdir = NULL;
    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->define_session_dir) {
            rc = mod->module->define_session_dir(tmpdir);
            if (PRTE_SUCCESS == rc) {
                return rc;
            }
            if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->allow_run_as_root) {
            rc = mod->module->allow_run_as_root(cmd_line);
            if (PRTE_SUCCESS == rc) {
                return rc;
            }
        }
    }

    /* show_help is not yet available, so print an error manually */
    fprintf(stderr, "--------------------------------------------------------------------------\n");
    if (prte_cmd_line_is_taken(cmd_line, "help")) {
        fprintf(stderr, "%s cannot provide the help message when run as root.\n\n", prte_tool_basename);
    } else {
        fprintf(stderr, "%s has detected an attempt to run as root.\n\n", prte_tool_basename);
    }

    fprintf(stderr, "Running as root is *strongly* discouraged as any mistake (e.g., in\n");
    fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
    fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");

    fprintf(stderr, "We strongly suggest that you run %s as a non-root user.\n\n", prte_tool_basename);

    fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
    fprintf(stderr, "option to your command line.  However, we reiterate our strong advice\n");
    fprintf(stderr, "against doing so - please do so at your own risk.\n");
    fprintf(stderr, "--------------------------------------------------------------------------\n");
    exit(1);
}

void prte_schizo_base_wrap_args(char **args)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->wrap_args) {
            mod->module->wrap_args(args);
        }
    }
}

int prte_schizo_base_setup_app(prte_app_context_t *app)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_app) {
            rc = mod->module->setup_app(app);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_setup_fork(prte_job_t *jdata,
                                prte_app_context_t *context)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_fork) {
            rc = mod->module->setup_fork(jdata, context);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_setup_child(prte_job_t *jdata,
                                 prte_proc_t *child,
                                 prte_app_context_t *app,
                                 char ***env)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->setup_child) {
            rc = mod->module->setup_child(jdata, child, app, env);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}

void prte_schizo_base_job_info(prte_cmd_line_t *cmdline, prte_list_t *jobinfo)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->job_info) {
            mod->module->job_info(cmdline, jobinfo);
        }
    }
}

int prte_schizo_base_get_remaining_time(uint32_t *timeleft)
{
    int rc;
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->get_remaining_time) {
            rc = mod->module->get_remaining_time(timeleft);
            if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }
    return PRTE_ERR_NOT_SUPPORTED;
}

void prte_schizo_base_finalize(void)
{
    prte_schizo_base_active_module_t *mod;

    PRTE_LIST_FOREACH(mod, &prte_schizo_base.active_modules, prte_schizo_base_active_module_t) {
        if (NULL != mod->module->finalize) {
            mod->module->finalize();
        }
    }
}


static int process_deprecated_cli(prte_cmd_line_t *cmdline,
                                  int *argc, char ***argv,
                                  prte_list_t *convertors)
{
    int pargc;
    char **pargs, *p2;
    int i, n, rc, ret;
    prte_cmd_line_init_t e;
    prte_cmd_line_option_t *option;
    bool found;
    prte_convertor_t *cv;

    pargs = *argv;
    pargc = *argc;
    ret = PRTE_SUCCESS;

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
            prte_asprintf(&pargs[i], "-%s", p2);
            /* if it is the special "-np" option, we silently
             * change it and don't emit an error */
            if (0 == strcmp(p2, "-np")) {
                free(p2);
            } else {
                prte_show_help("help-schizo-base.txt", "single-dash-error", true,
                                p2, pargs[i]);
                free(p2);
                ret = PRTE_OPERATION_SUCCEEDED;
            }
        }

        /* is this an argument someone needs to convert? */
        found = false;
        PRTE_LIST_FOREACH(cv, convertors, prte_convertor_t) {
            for (n=0; NULL != cv->options[n]; n++) {
                if (0 == strcmp(pargs[i], cv->options[n])) {
                    rc = cv->convert(cv->options[n], argv, i);
                    if (PRTE_SUCCESS != rc &&
                        PRTE_ERR_SILENT != rc &&
                        PRTE_ERR_TAKE_NEXT_OPTION != rc &&
                        PRTE_OPERATION_SUCCEEDED != rc) {
                        return rc;
                    }
                    if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                        /* we did the conversion but don't want
                         * to deprecate i */
                        rc = PRTE_SUCCESS;
                    } else if (PRTE_OPERATION_SUCCEEDED == rc) {
                        /* we did not do a conversion but don't
                         * want to deprecate i */
                        rc = PRTE_ERR_SILENT;
                    } else {
                        --i;
                    }
                    found = true;
                    if (PRTE_ERR_SILENT != rc) {
                        ret = PRTE_OPERATION_SUCCEEDED;
                    }
                    pargs = *argv;
                    pargc = prte_argv_count(pargs);
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
                memset(&e, 0, sizeof(prte_cmd_line_init_t));
                e.ocl_cmd_short_name = pargs[i][1];
                option = prte_cmd_line_find_option(cmdline, &e);

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
                memset(&e, 0, sizeof(prte_cmd_line_init_t));
                e.ocl_cmd_long_name = &pargs[i][2];
                option = prte_cmd_line_find_option(cmdline, &e);

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
