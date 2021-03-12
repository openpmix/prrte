/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Sylabs, Inc. All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "src/include/types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/prte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/path.h"

#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/mca/schizo/base/base.h"

#include "schizo_singularity.h"

static const char *singularity_exec_argc_env_var_name = "SY_EXEC_ARGS";
static int setup_fork(prte_job_t *jdata, prte_app_context_t *context);

prte_schizo_base_module_t prte_schizo_singularity_module = {
    .name = "singularity",
    .setup_fork = setup_fork
};

static int setup_fork(prte_job_t *jdata, prte_app_context_t *app)
{
    int i;
    char *pth = NULL; // Path to the directory where the Singularity binary is
    char *exec_args = NULL;
    pmix_envar_t envar;
    char **cmd_args = NULL;

     /* even if they didn't specify, check to see if
     * this involves a singularity container */
    if (0 == strcmp(app->argv[0],"singularity") ||
        NULL != strstr(app->argv[0], ".sif")) {
        goto process;
    }
    /* guess not */
    return PRTE_ERR_TAKE_NEXT_OPTION;

  process:
    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:singularity: configuring app environment %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), app->argv[0]);

    /* We do not assume that the location of the singularity binary is set at
     * compile time as it can change between jobs (not during a job). If it does,
     * then it will be in the application's environment */
    if (NULL != app->env) {
        pth = prte_path_findv("singularity", X_OK, app->env, NULL);
    }
    if (NULL != pth) {
        /* prte_path_findv returned the absolute path to the Singularity binary,
         * we want the directory where the binary is. */
        pth = prte_dirname(pth);
        prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                            "%s schizo:singularity: Singularity found from env: %s\n",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), pth);
    } else {
        /* wasn't in the environment - see if it was found somewhere */
        if (0 < strlen(PRTE_SINGULARITY_PATH)) {
            if (0 != strcmp(PRTE_SINGULARITY_PATH, "DEFAULT")) {
                pth = PRTE_SINGULARITY_PATH;
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                     "%s schizo:singularity: using default Singularity from %s\n",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), pth);
                 /* Update (if possible) the PATH of the app so it can find singularity otherwise
                    it will likely not find it and create a failure. The default path to singularity
                    that is set at configuration time may not be in the environment that is passed in
                    by the user. */
                 for (i = 0; NULL != app->env[i]; i++) {
                     if (0 == strncmp(app->env[i], "PATH", 4)) {
                         char *cur_path_val = &app->env[i][5];
                         if (app->env[i] != NULL) {
                             free(app->env[i]);
                         }
                         prte_asprintf(&app->env[i], "PATH=%s:%s", pth, cur_path_val);
                         break;
                     }
                 }
            }
        } else {
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
    }
    if (NULL == pth) {
        // at this point, if we do not have a valid path to Singularity, there is nothing we can do
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* tell the odls component to prepend this to our PATH */
    envar.envar = "PATH";
    envar.value = pth;
    envar.separator = ':';
    prte_add_attribute(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR,
                       PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);

    // the final command is now singularity
    if (app->app) {
        free(app->app);
    }
    asprintf(&app->app, "%s/singularity", pth);

    /* start building the final cmd */
    prte_argv_append_nosize(&cmd_args, "singularity");
    prte_argv_append_nosize(&cmd_args, "exec");

    /* We need to parse the environment of the application because there is important
     * information in it to be able to set everything up. For example, the arguments
     * used to start a container, i.e., the 'singularity exec' flags are based to some
     * extent on the configuration of Singularity and also on the image itself. Extra
     * 'singularity exec' flags can be set using an environment variable (its name is
     * defined by the value of  singularity_exec_argc_env_var_name). */
    if (NULL != app->env) {
        for (i=0; NULL != app->env[i]; i++) {
            if (0 == strncmp(app->env[i], singularity_exec_argc_env_var_name, strlen(singularity_exec_argc_env_var_name))) {
                exec_args = &app->env[i][strlen(singularity_exec_argc_env_var_name) + 1]; // We do not want the "="
                break;
            }
        }
        if (NULL != exec_args) {
            char **args = prte_argv_split(exec_args, ' ');
            for (i=0; NULL != args[i]; i++) {
                prte_argv_append_nosize(&cmd_args, args[i]);
            }
        }
    }
    /* now add in the rest of the args they provided */
    for(i=0; NULL != app->argv[i]; i++) {
        prte_argv_append_nosize(&cmd_args, app->argv[i]);
    }

    /* replace their argv with the new one */
    prte_argv_free(app->argv);
    app->argv = cmd_args;

    /* set the singularity cache dir, unless asked not to do so */
    if (!prte_get_attribute(&app->attributes, PRTE_APP_NO_CACHEDIR, NULL, PMIX_BOOL)) {
        /* Set the Singularity sessiondir to exist within the PRTE sessiondir */
        prte_setenv("SINGULARITY_SESSIONDIR", prte_process_info.job_session_dir, true, &app->env);
        /* No need for Singularity to clean up after itself if PRTE will */
        prte_setenv("SINGULARITY_NOSESSIONCLEANUP", "1", true, &app->env);
    }

    return PRTE_SUCCESS;
}
