/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009      Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/proc_info.h"
#include "src/util/prte_environ.h"
#include "src/util/prte_getcwd.h"
#include "src/util/show_help.h"

#include "src/runtime/prte_globals.h"

#include "src/prted/prted.h"

static void set_classpath_jar_file(prte_pmix_app_t *app, int index, char *jarfile);

/*
 * This function takes a "char ***app_env" parameter to handle the
 * specific case:
 *
 *   prun --mca foo bar -app appfile
 *
 * That is, we'll need to keep foo=bar, but the presence of the app
 * file will cause an invocation of parse_appfile(), which will cause
 * one or more recursive calls back to create_app().  Since the
 * foo=bar value applies globally to all apps in the appfile, we need
 * to pass in the "base" environment (that contains the foo=bar value)
 * when we parse each line in the appfile.
 *
 * This is really just a special case -- when we have a simple case like:
 *
 *   prun --mca foo bar -np 4 hostname
 *
 * Then the upper-level function (parse_locals()) calls create_app()
 * with a NULL value for app_env, meaning that there is no "base"
 * environment that the app needs to be created from.
 */
static int create_app(prte_cmd_line_t *prte_cmd_line, int argc, char *argv[], prte_list_t *jdata,
                      prte_pmix_app_t **app_ptr, bool *made_app, char ***app_env, char ***hostfiles,
                      char ***hosts)
{
    char cwd[PRTE_PATH_MAX];
    int i, j, count, rc;
    char *param, *value, *ptr;
    prte_pmix_app_t *app = NULL;
    bool found = false;
    char *appname = NULL;
    prte_value_t *pvalue;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    if (PRTE_SUCCESS != (rc = prte_cmd_line_parse(prte_cmd_line, true, false, argc, argv))) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], prte_strerror(rc));
        }
        return rc;
    }

    /* Setup application context */
    app = PRTE_NEW(prte_pmix_app_t);
    prte_cmd_line_get_tail(prte_cmd_line, &count, &app->app.argv);

    /* See if we have anything left */
    if (0 == count) {
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    app->app.cmd = strdup(app->app.argv[0]);

    /* get the cwd - we may need it in several places */
    if (PRTE_SUCCESS != (rc = prte_getcwd(cwd, sizeof(cwd)))) {
        prte_show_help("help-prun.txt", "prun:init-failure", true, "get the cwd", rc);
        goto cleanup;
    }

    /* Did the user specify a path to the executable? */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "path", 0, 0))) {
        param = pvalue->value.data.string;
        /* if this is a relative path, convert it to an absolute path */
        if (prte_path_is_absolute(param)) {
            value = strdup(param);
        } else {
            /* construct the absolute path */
            value = prte_os_path(false, cwd, param, NULL);
        }
        /* construct the new argv[0] */
        ptr = prte_os_path(false, value, app->app.argv[0], NULL);
        free(value);
        free(app->app.argv[0]);
        app->app.argv[0] = ptr;
    }

    /* Did the user request a specific wdir? */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "wdir", 0, 0))) {
        param = pvalue->value.data.string;
        /* if this is a relative path, convert it to an absolute path */
        if (prte_path_is_absolute(param)) {
            app->app.cwd = strdup(param);
        } else {
            /* construct the absolute path */
            app->app.cwd = prte_os_path(false, cwd, param, NULL);
        }
    } else if (prte_cmd_line_is_taken(prte_cmd_line, "set-cwd-to-session-dir")) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
    } else {
        app->app.cwd = strdup(cwd);
    }

    /* if they specified a process set name, then pass it along */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "pset", 0, 0))) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PSET_NAME, pvalue->value.data.string, PMIX_STRING);
    }

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    found = false;
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "hostfile"))) {
        if (1 < j) {
            prte_show_help("help-prun.txt", "prun:multiple-hostfiles", true, "prun", NULL);
            return PRTE_ERR_FATAL;
        } else {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "hostfile", 0, 0);
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOSTFILE, pvalue->value.data.string,
                               PMIX_STRING);
            if (NULL != hostfiles) {
                prte_argv_append_nosize(hostfiles, pvalue->value.data.string);
            }
            found = true;
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "machinefile"))) {
        if (1 < j || found) {
            prte_show_help("help-prun.txt", "prun:multiple-hostfiles", true, "prun", NULL);
            return PRTE_ERR_FATAL;
        } else {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "machinefile", 0, 0);
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOSTFILE, pvalue->value.data.string,
                               PMIX_STRING);
            if (NULL != hostfiles) {
                prte_argv_append_nosize(hostfiles, pvalue->value.data.string);
            }
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prte_cmd_line_get_ninsts(prte_cmd_line, "host"))) {
        char **targ = NULL, *tval;
        for (i = 0; i < j; ++i) {
            pvalue = prte_cmd_line_get_param(prte_cmd_line, "host", i, 0);
            prte_argv_append_nosize(&targ, pvalue->value.data.string);
            if (NULL != hosts) {
                prte_argv_append_nosize(hosts, pvalue->value.data.string);
            }
        }
        tval = prte_argv_join(targ, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOST, tval, PMIX_STRING);
        free(tval);
    }

    /* check for bozo error */
    if (NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "np", 0, 0))
        || NULL != (pvalue = prte_cmd_line_get_param(prte_cmd_line, "n", 0, 0))) {
        if (0 > pvalue->value.data.integer) {
            prte_show_help("help-prun.txt", "prun:negative-nprocs", true, "prun", app->app.argv[0],
                           pvalue->value.data.integer, NULL);
            return PRTE_ERR_FATAL;
        }
    }
    if (NULL != pvalue) {
        /* we don't require that the user provide --np or -n because
         * the cmd line might stipulate a mapping policy that computes
         * the number of procs - e.g., a map-by ppr option */
        app->app.maxprocs = pvalue->value.data.integer;
    }

    /* see if we need to preload the binary to
     * find the app - don't do this for java apps, however, as we
     * can't easily find the class on the cmd line. Java apps have to
     * preload their binary via the preload_files option
     */
    if (NULL == strstr(app->app.argv[0], "java")) {
        if (prte_cmd_line_is_taken(prte_cmd_line, "preload-binaries")) {
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
        }
    }
    if (prte_cmd_line_is_taken(prte_cmd_line, "preload-files")) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_FILES, NULL, PMIX_BOOL);
    }

    /* Do not try to find argv[0] here -- the starter is responsible
     for that because it may not be relevant to try to find it on
     the node where prun is executing.  So just strdup() argv[0]
     into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        prte_show_help("help-prun.txt", "prun:call-failed", true, "prun", "library",
                       "strdup returned NULL", errno);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* if this is a Java application, we have a bit more work to do. Such
     * applications actually need to be run under the Java virtual machine
     * and the "java" command will start the "executable". So we need to ensure
     * that all the proper java-specific paths are provided
     */
    appname = prte_basename(app->app.cmd);
    if (0 == strcmp(appname, "java")) {
        /* see if we were given a library path */
        found = false;
        for (i = 1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "java.library.path")) {
                char *dptr;
                /* find the '=' that delineates the option from the path */
                if (NULL == (dptr = strchr(app->app.argv[i], '='))) {
                    /* that's just wrong */
                    rc = PRTE_ERR_BAD_PARAM;
                    goto cleanup;
                }
                /* step over the '=' */
                ++dptr;
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                if (NULL == strstr(app->app.argv[i], prte_install_dirs.libdir)) {
                    /* doesn't appear to - add it to be safe */
                    if (':' == app->app.argv[i][strlen(app->app.argv[i] - 1)]) {
                        prte_asprintf(&value, "-Djava.library.path=%s%s", dptr,
                                      prte_install_dirs.libdir);
                    } else {
                        prte_asprintf(&value, "-Djava.library.path=%s:%s", dptr,
                                      prte_install_dirs.libdir);
                    }
                    free(app->app.argv[i]);
                    app->app.argv[i] = value;
                }
                break;
            }
        }
        if (!found) {
            /* need to add it right after the java command */
            prte_asprintf(&value, "-Djava.library.path=%s", prte_install_dirs.libdir);
            prte_argv_insert_element(&app->app.argv, 1, value);
            free(value);
        }

        /* see if we were given a class path */
        found = false;
        for (i = 1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "cp")
                || NULL != strstr(app->app.argv[i], "classpath")) {
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                /* check if mpi.jar exists - if so, add it */
                value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK) != -1) {
                    set_classpath_jar_file(app, i + 1, "mpi.jar");
                }
                free(value);
                /* check for oshmem support */
                value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK) != -1) {
                    set_classpath_jar_file(app, i + 1, "shmem.jar");
                }
                free(value);
                /* always add the local directory */
                prte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[i + 1]);
                free(app->app.argv[i + 1]);
                app->app.argv[i + 1] = value;
                break;
            }
        }
        if (!found) {
            /* check to see if CLASSPATH is in the environment */
            found = false; // just to be pedantic
            for (i = 0; NULL != environ[i]; i++) {
                if (0 == strncmp(environ[i], "CLASSPATH", strlen("CLASSPATH"))) {
                    value = strchr(environ[i], '=');
                    ++value; /* step over the = */
                    prte_argv_insert_element(&app->app.argv, 1, value);
                    /* check for mpi.jar */
                    value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                    if (access(value, F_OK) != -1) {
                        set_classpath_jar_file(app, 1, "mpi.jar");
                    }
                    free(value);
                    /* check for shmem.jar */
                    value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                    if (access(value, F_OK) != -1) {
                        set_classpath_jar_file(app, 1, "shmem.jar");
                    }
                    free(value);
                    /* always add the local directory */
                    prte_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[1]);
                    free(app->app.argv[1]);
                    app->app.argv[1] = value;
                    prte_argv_insert_element(&app->app.argv, 1, "-cp");
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* need to add it right after the java command - have
                 * to include the working directory and trust that
                 * the user set cwd if necessary
                 */
                char *str, *str2;
                /* always start with the working directory */
                str = strdup(app->app.cwd);
                /* check for mpi.jar */
                value = prte_os_path(false, prte_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK) != -1) {
                    prte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                /* check for shmem.jar */
                value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK) != -1) {
                    prte_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                prte_argv_insert_element(&app->app.argv, 1, str);
                free(str);
                prte_argv_insert_element(&app->app.argv, 1, "-cp");
            }
        }
    }

#if 0
    /* if this is a singularity app, a little more to do */
    if (0 == strcmp(app->argv[0],"singularity") ||
        NULL != strstr(app->argv[0], ".sif")) {
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
    }
#endif

    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

cleanup:
    if (NULL != app) {
        PRTE_RELEASE(app);
    }
    if (NULL != appname) {
        free(appname);
    }
    return rc;
}

int prte_parse_locals(prte_cmd_line_t *prte_cmd_line, prte_list_t *jdata, int argc, char *argv[],
                      char ***hostfiles, char ***hosts)
{
    int i, rc;
    int temp_argc;
    char **temp_argv, **env;
    prte_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argc = 0;
    temp_argv = NULL;
    prte_argv_append(&temp_argc, &temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
     create_app(), below.  See comment immediately before the
     create_app() function for an explanation. */

    env = NULL;
    for (i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (prte_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    prte_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(prte_cmd_line, temp_argc, temp_argv, jdata, &app, &made_app, &env,
                                hostfiles, hosts);
                if (PRTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                     printed; */
                    return rc;
                }
                if (made_app) {
                    prte_list_append(jdata, &app->super);
                }

                /* Reset the temps */

                temp_argc = 0;
                temp_argv = NULL;
                prte_argv_append(&temp_argc, &temp_argv, argv[0]);
            }
        } else {
            prte_argv_append(&temp_argc, &temp_argv, argv[i]);
        }
    }

    if (prte_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(prte_cmd_line, temp_argc, temp_argv, jdata, &app, &made_app, &env,
                        hostfiles, hosts);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        if (made_app) {
            prte_list_append(jdata, &app->super);
        }
    }
    if (NULL != env) {
        prte_argv_free(env);
    }
    prte_argv_free(temp_argv);

    /* All done */

    return PRTE_SUCCESS;
}

static void set_classpath_jar_file(prte_pmix_app_t *app, int index, char *jarfile)
{
    if (NULL == strstr(app->app.argv[index], jarfile)) {
        /* nope - need to add it */
        char *fmt = ':' == app->app.argv[index][strlen(app->app.argv[index] - 1)] ? "%s%s/%s"
                                                                                  : "%s:%s/%s";
        char *str;
        prte_asprintf(&str, fmt, app->app.argv[index], prte_install_dirs.libdir, jarfile);
        free(app->app.argv[index]);
        app->app.argv[index] = str;
    }
}
