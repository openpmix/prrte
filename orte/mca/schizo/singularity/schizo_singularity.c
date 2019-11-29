/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Sylabs, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "orte_config.h"
#include "orte/types.h"
#include "opal/types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "opal/util/argv.h"
#include "opal/util/basename.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_dirpath.h"
#include "opal/util/path.h"

#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/mca/schizo/base/base.h"

#include "schizo_singularity.h"

static const char *singularity_exec_argc_env_var_name = "SY_EXEC_ARGS";
static int setup_fork(orte_job_t *jdata, orte_app_context_t *context);

orte_schizo_base_module_t orte_schizo_singularity_module = {
    .setup_fork = setup_fork
};

static int setup_fork(orte_job_t *jdata, orte_app_context_t *app)
{
    int i;
    bool takeus = false;
    char *pth = NULL;
    char *exec_args = NULL;
    opal_envar_t envar;
    char **cmd_args = NULL;

    if (NULL != orte_schizo_base.personalities &&
        NULL != jdata->personality) {
        /* see if we are included */
        for (i=0; NULL != jdata->personality[i]; i++) {
            if (0 == strcmp(jdata->personality[i], "singularity")) {
                takeus = true;
                break;
            }
        }
    }
    if (!takeus) {
        /* even if they didn't specify, check to see if
         * this involves a singularity container */
        if (0 == strcmp(app->argv[0],"singularity") ||
            NULL != strstr(app->argv[0], ".sif")) {
            goto process;
        }
        /* guess not */
        return ORTE_ERR_TAKE_NEXT_OPTION;
    }

  process:
    opal_output_verbose(1, orte_schizo_base_framework.framework_output,
                        "%s schizo:singularity: configuring app environment %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app->argv[0]);

    /* We do not assume that the location of the singularity binary is set at
     * compile time as it can change between jobs (not during a job). If it does,
     * then it will be in the application's environment */
    if (NULL != app->env) {
        pth = opal_path_findv("singularity", X_OK, app->env, NULL);
    }
    if (NULL == pth) {
        /* wasn't in the environment - see if it was found somewhere */
        if (0 < strlen(OPAL_SINGULARITY_PATH)) {
            if (0 != strcmp(OPAL_SINGULARITY_PATH, "DEFAULT")) {
                pth = OPAL_SINGULARITY_PATH;
            }
        } else {
            return ORTE_ERR_TAKE_NEXT_OPTION;
        }
    }
    if (NULL != pth) {
        /* tell the odls component to prepend this to our PATH */
        envar.envar = "PATH";
        envar.value = pth;
        envar.separator = ':';
        orte_add_attribute(&jdata->attributes, ORTE_JOB_PREPEND_ENVAR,
            ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
    }

    /* start building the final cmd */
    opal_argv_append_nosize(&cmd_args, "singularity");
    opal_argv_append_nosize(&cmd_args, "exec");

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
            char **args = opal_argv_split(exec_args, ' ');
            for (i=0; NULL != args[i]; i++) {
                opal_argv_append_nosize(&cmd_args, args[i]);
            }
        }
    }
    /* now add in the rest of the args they provided */
    for(i=0; NULL != app->argv[i]; i++) {
        opal_argv_append_nosize(&cmd_args, app->argv[i]);
    }

    /* replace their argv with the new one */
    opal_argv_free(app->argv);
    app->argv = cmd_args;

    /* set the singularity cache dir, unless asked not to do so */
    if (!orte_get_attribute(&app->attributes, ORTE_APP_NO_CACHEDIR, NULL, OPAL_BOOL)) {
        /* Set the Singularity sessiondir to exist within the OMPI sessiondir */
        opal_setenv("SINGULARITY_SESSIONDIR", orte_process_info.job_session_dir, true, &app->env);
        /* No need for Singularity to clean up after itself if OMPI will */
        opal_setenv("SINGULARITY_NOSESSIONCLEANUP", "1", true, &app->env);
    }

    return ORTE_SUCCESS;
}
