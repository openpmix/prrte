/*
 * Copyright (c) 2019-2020 IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "src/include/types.h"
#include "opal/types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/prrte_environ.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/mca/schizo/base/base.h"

#include "schizo_entry.h"

#include "src/util/parse_entry.h"

/* Cmd-line option for OMPI's -entry feature */
static prrte_cmd_line_init_t cmd_line_init[] = {
    { '\0', "entry", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "PMPI layering options",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "entrybase", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "PMPI layering options",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static int define_cli(prrte_cmd_line_t *cli);
static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static int setup_fork(prrte_job_t *jdata, prrte_app_context_t *context);

prrte_schizo_base_module_t prrte_schizo_entry_module = {
    .define_cli = define_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork
};

static int
define_cli(prrte_cmd_line_t *cli)
{
    int rc;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:entry: define_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRRTE_ERR_BAD_PARAM;
    }

    rc = prrte_cmd_line_add(cli, cmd_line_init);
    if (PRRTE_SUCCESS != rc){
        return rc;
    }

    return PRRTE_SUCCESS;
}

/*
 *  Look for var in a char **env list
 */
static char *
getenv_from_list(char *var, char **env)
{
    char **penv;
    char *p;
    int len;

    if (!var || !*var) { return NULL; }
    len = strlen(var);

    penv = env;
    while (penv && *penv) {
        p = *penv;
        if (0 == strncmp(p, var, len) && p[len] == '=') {
            p += (len + 1);
            if (!*p) { return NULL; }
            return p;
        }
        ++penv;
    }

    return NULL;
}

static int
setup_fork(prrte_job_t *jdata, prrte_app_context_t *context)
{
    int entry_is_active;
    char *preload_string;
    char *ompi_root;
    opal_envar_t envar;

    prrte_entry_parse_mca(
        getenv_from_list("OMPI_MCA_ompi_tools_entry", context->env),
        getenv_from_list("OMPI_MCA_ompi_tools_entrybase", context->env),
        &entry_is_active, NULL, &preload_string,
        NULL, NULL, NULL, NULL);

    if (entry_is_active) {
        prrte_app_context_t *app;

        ompi_root = getenv("OPAL_PREFIX");
        if (!ompi_root) { ompi_root = getenv("MPI_ROOT"); }

        /* looking only at app[0] for a prefix */
        char *app_prefix_dir = NULL;
        if ((app = (prrte_app_context_t*) prrte_pointer_array_get_item(jdata->apps, 0))) {
            prrte_get_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&app_prefix_dir, OPAL_STRING);
        }
        if (app_prefix_dir) {
            ompi_root = app_prefix_dir;
        }

/* 
 * Set OMPI_ENTRY_LD_PRELOAD and OMPI_ENTRY_LD_LIBRARY_PATH and use
 * MPI_ROOT/bin/profilesupport in front of the exe
 *
 * Reason for this argv prepend method instead of PRRTE_JOB_PREPEND_ENVAR:
 * The PRRTE_JOB_PREPEND_ENVAR is almost what we want, but it's not
 * really forceful about being the last prepend, so it doesn't guarantee
 * an MPI_ROOT/lib won't be prepended in front of what it does
 */
        if (ompi_root) {
            char *str = malloc(strlen(ompi_root) + 64);
            if (!str) { return PRRTE_ERR_OUT_OF_RESOURCE; }

            sprintf(str, "%s/bin/profilesupport", ompi_root);
            prrte_argv_insert_element(&context->argv, 0, str);
            if (context->app) { free(context->app); }
            context->app = strdup(context->argv[0]);

            sprintf(str, "%s/lib/profilesupport", ompi_root);
            prrte_setenv("OMPI_ENTRY_LD_LIBRARY_PATH", str, true, &context->env);
            if (preload_string) {
                prrte_setenv("OMPI_ENTRY_LD_PRELOAD", preload_string, true, &context->env);
            }

            free(str);
        }
    }

    return PRRTE_SUCCESS;
}

static int
parse_env(prrte_cmd_line_t *cmd_line,
          char **srcenv,
          char ***dstenv,
          bool cmdline)
{
    int entry_is_active;
    int verbose;
    char **lib_names, **baselib_names;
    int nlib_names, nbaselib_names;

    /*
     *  Accept any of
     *    -entry on cmdline
     *    --mca ompi_tools_entry
     *    OMPI_MCA_ompi_tools_entry env var
     */
    prrte_value_t *pval;
    if (NULL != (pval = prrte_cmd_line_get_param(cmd_line, "entry", 0, 0))) {
        prrte_setenv("OMPI_MCA_ompi_tools_entry", pval->data.string, true, dstenv);
    }
    if (NULL != (pval = prrte_cmd_line_get_param(cmd_line, "entrybase", 0, 0))) {
        prrte_setenv("OMPI_MCA_ompi_tools_entrybase", pval->data.string, true, dstenv);
    }
    /* If it was on the cmdline as an --mca , it should be in dstenv by now */
    char *p;
    p = getenv("OMPI_MCA_ompi_tools_entry");
    if (p && *p) {
        prrte_setenv("OMPI_MCA_ompi_tools_entry", p, true, dstenv);
    }
    p = getenv("OMPI_MCA_ompi_tools_entrybase");
    if (p && *p) {
        prrte_setenv("OMPI_MCA_ompi_tools_entrybase", p, true, dstenv);
    }

    prrte_entry_parse_mca(
        getenv_from_list("OMPI_MCA_ompi_tools_entry", *dstenv),
        getenv_from_list("OMPI_MCA_ompi_tools_entrybase", *dstenv),
        &entry_is_active, &verbose, NULL,
        &lib_names, &nlib_names, &baselib_names, &nbaselib_names);

    if (entry_is_active) {
        if (verbose) {
            int has_fort_specified = 0;
            int i;
            printf("Entrypoint MPI wrapper levels:\n");
            for (i=0; i<nlib_names; ++i) {
                if (0 == strcmp(lib_names[i], "fort") ||
                    0 == strcmp(lib_names[i], "fortran"))
                {
                    has_fort_specified = 1;
                }
            }

            int level;
            level = 0;
            if (!has_fort_specified) {
                printf("  [%d] : fortran from base product\n", level);
                ++level;
            }
            for (i=0; i<nlib_names; ++i) {
                if (0 == strcmp(lib_names[i], "fort") ||
                    0 == strcmp(lib_names[i], "fortran"))
                {
                    printf("  [%d] : fortran from base product\n", level);
                } else {
                    printf("  [%d] : %s\n", level, lib_names[i]);
                }
                ++level;
            }
            printf("  [%d] : base product\n", level);

            printf("Entrypoint MPI base product:\n");
            level = 0;
            for (i=0; i<nbaselib_names; ++i) {
                printf("  [%d] : %s\n", level, baselib_names[i]);
                ++level;
            }
            fflush(stdout);
        }
        free(lib_names);
        free(baselib_names);
    }

    return PRRTE_SUCCESS;
}
