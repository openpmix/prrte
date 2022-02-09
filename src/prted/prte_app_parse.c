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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#include "src/mca/schizo/base/base.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_environ.h"
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
static int create_app(prte_schizo_base_module_t *schizo, char **argv, prte_list_t *jdata,
                      prte_pmix_app_t **app_ptr, bool *made_app, char ***app_env,
                      char ***hostfiles, char ***hosts)
{
    char cwd[PRTE_PATH_MAX];
    int i, j, count, rc;
    char *param, *value, *ptr;
    prte_pmix_app_t *app = NULL;
    bool found = false;
    char *appname = NULL;
    prte_cli_item_t *opt;
    prte_value_t *pvalue;
    prte_cli_result_t results;
    char *tval;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    PRTE_CONSTRUCT(&results, prte_cli_result_t);
    rc = schizo->parse_cli(argv, &results, PRTE_CLI_SILENT);
    if (PRTE_SUCCESS != rc) {
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], prte_strerror(rc));
        }
        PRTE_DESTRUCT(&results);
        return rc;
    }
    // sanity check the results
    rc = prte_schizo_base_sanity(&results);
    if (PRTE_SUCCESS != rc) {
        // sanity checker prints the reason
        PRTE_DESTRUCT(&results);
        return rc;
    }

    /* See if we have anything left */
    if (NULL == results.tail) {
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    /* Setup application context */
    app = PRTE_NEW(prte_pmix_app_t);
    app->app.argv = pmix_argv_copy(results.tail);
    app->app.cmd = strdup(app->app.argv[0]);

    /* get the cwd - we may need it in several places */
    if (PRTE_SUCCESS != (rc = prte_getcwd(cwd, sizeof(cwd)))) {
        prte_show_help("help-prun.txt", "prun:init-failure", true, "get the cwd", rc);
        goto cleanup;
    }

    /* Did the user specify a path to the executable? */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_PATH);
    if (NULL != opt) {
        param = opt->values[0];
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
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_WDIR);
    if (NULL != opt) {
        param = opt->values[0];
        /* if this is a relative path, convert it to an absolute path */
        if (prte_path_is_absolute(param)) {
            app->app.cwd = strdup(param);
        } else {
            /* construct the absolute path */
            app->app.cwd = prte_os_path(false, cwd, param, NULL);
        }
    } else if (prte_cmd_line_is_taken(&results, "set-cwd-to-session-dir")) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
    } else {
        app->app.cwd = strdup(cwd);
    }

    /* if they specified a process set name, then pass it along */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_PSET);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PSET_NAME,
                           opt->values[0], PMIX_STRING);
    }

    /* Did the user specify a hostfile? */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
    if (NULL != opt) {
        tval = pmix_argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOSTFILE,
                           tval, PMIX_STRING);
        free(tval);
        if (NULL != hostfiles) {
            for (i=0; NULL != opt->values[i]; i++) {
                pmix_argv_append_nosize(hostfiles, opt->values[i]);
            }
        }
    }

    /* Did the user specify any hosts? */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_HOST);
    if (NULL != opt) {
        tval = pmix_argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOST, tval, PMIX_STRING);
        free(tval);
        if (NULL != hosts) {
            for (i=0; NULL != opt->values[i]; i++) {
                pmix_argv_append_nosize(hosts, opt->values[i]);
            }
        }
    }

    /* check for bozo error */
    opt = prte_cmd_line_get_param(&results, PRTE_CLI_NP);
    if (NULL != opt) {
        count = strtol(opt->values[0], NULL, 10);
        if (0 > count) {
            prte_show_help("help-prun.txt", "prun:negative-nprocs", true,
                           prte_tool_basename,
                           app->app.argv[0], count, NULL);
            return PRTE_ERR_FATAL;
        }
        /* we don't require that the user provide --np or -n because
         * the cmd line might stipulate a mapping policy that computes
         * the number of procs - e.g., a map-by ppr option */
        app->app.maxprocs = count;
    }

    /* see if we need to preload the binary to
     * find the app - don't do this for java apps, however, as we
     * can't easily find the class on the cmd line. Java apps have to
     * preload their binary via the preload_files option
     */
    appname = pmix_basename(app->app.cmd);
    if (0 == strcmp(appname, "java")) {
        opt = prte_cmd_line_get_param(&results, PRTE_CLI_PRELOAD_BIN);
        if (NULL != opt) {
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
        }
    }
    opt = prte_cmd_line_get_param(&results, "preload-files");
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_FILES, opt->values[0], PMIX_STRING);
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
                        pmix_asprintf(&value, "-Djava.library.path=%s%s", dptr,
                                      prte_install_dirs.libdir);
                    } else {
                        pmix_asprintf(&value, "-Djava.library.path=%s:%s", dptr,
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
            pmix_asprintf(&value, "-Djava.library.path=%s", prte_install_dirs.libdir);
            pmix_argv_insert_element(&app->app.argv, 1, value);
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
                pmix_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[i + 1]);
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
                    pmix_argv_insert_element(&app->app.argv, 1, value);
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
                    pmix_asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[1]);
                    free(app->app.argv[1]);
                    app->app.argv[1] = value;
                    pmix_argv_insert_element(&app->app.argv, 1, "-cp");
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
                    pmix_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                /* check for shmem.jar */
                value = prte_os_path(false, prte_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK) != -1) {
                    pmix_asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                pmix_argv_insert_element(&app->app.argv, 1, str);
                free(str);
                pmix_argv_insert_element(&app->app.argv, 1, "-cp");
            }
        }
    }

    // parse any environment-related cmd line options
    rc = schizo->parse_env(prte_launch_environ, &app->app.env, &results);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }

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
    PRTE_DESTRUCT(&results);
    return rc;
}

int prte_parse_locals(prte_schizo_base_module_t *schizo,
                      prte_list_t *jdata, char *argv[],
                      char ***hostfiles, char ***hosts)
{
    int i, rc;
    char **temp_argv, **env;
    prte_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argv = NULL;
    pmix_argv_append_nosize(&temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
     create_app(), below.  See comment immediately before the
     create_app() function for an explanation. */

    env = NULL;
    for (i = 1; NULL != argv[i]; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (pmix_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    pmix_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(schizo, temp_argv, jdata, &app, &made_app, &env,
                                hostfiles, hosts);
                if (PRTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                     printed; */
                    pmix_argv_free(temp_argv);
                    return rc;
                }
                if (made_app) {
                    prte_list_append(jdata, &app->super);
                }

                /* Reset the temps */
                pmix_argv_free(temp_argv);
                temp_argv = NULL;
                pmix_argv_append_nosize(&temp_argv, argv[0]);
            }
        } else {
            pmix_argv_append_nosize(&temp_argv, argv[i]);
        }
    }

    if (pmix_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(schizo, temp_argv, jdata, &app, &made_app, &env,
                        hostfiles, hosts);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        if (made_app) {
            prte_list_append(jdata, &app->super);
        }
    }
    if (NULL != env) {
        pmix_argv_free(env);
    }
    pmix_argv_free(temp_argv);

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
        pmix_asprintf(&str, fmt, app->app.argv[index], prte_install_dirs.libdir, jarfile);
        free(app->app.argv[index]);
        app->app.argv[index] = str;
    }
}
