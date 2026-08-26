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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <ctype.h>
#include <limits.h>
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
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/runtime/prte_globals.h"

#include "src/prted/prted.h"

/* One occurrence of one option: the option's name, and one of the values
 * it was given. */
typedef struct {
    const char *key;
    const char *value;
} prte_cli_occ_t;

/*
 * Collect every value the cmd line carried, in the order it was given.
 *
 * A parse result groups the occurrences of an option onto that option's
 * single instance, which loses the interleaving: once a key exists, a
 * later occurrence of it is filed behind whatever came in between. PMIx
 * therefore also stamps each stored value with the position it was given
 * at, and reports the flat, ordered view built from those stamps.
 *
 * Built against a PMIx that predates PMIX_CAP_CLI_ORDER there are no
 * stamps, so fall back to walking the instances in list order. That is
 * correct for every command line except one that repeats an option after
 * another has intervened - which is precisely the case the stamps exist
 * for, and the reason to prefer a PMIx that has them.
 *
 * Either way the entries point into the result, so the array is valid
 * for as long as the result is and is released with a plain free().
 */
static int collect_ordered(pmix_cli_result_t *results,
                           prte_cli_occ_t **ordered, size_t *nordered)
{
    pmix_cli_item_t *opt;
    prte_cli_occ_t *out;
    size_t num = 0, m = 0;

    *ordered = NULL;
    *nordered = 0;

    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        num += (size_t) PMIx_Argv_count(opt->values);
    }
    if (0 == num) {
        return PRTE_SUCCESS;
    }
    out = (prte_cli_occ_t *) malloc(num * sizeof(prte_cli_occ_t));
    if (NULL == out) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

#if PRTE_PMIX_CLI_ORDER
    {
        pmix_cli_occurrence_t *occ = NULL;
        size_t nocc = 0, n;

        if (PMIX_SUCCESS != pmix_cmd_line_get_ordered(results, &occ, &nocc)) {
            free(out);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        for (n = 0; n < nocc; n++) {
            if (NULL == occ[n].value) {
                /* an option given without a value carries no directive -
                 * it is its own presence that says something */
                continue;
            }
            out[m].key = occ[n].key;
            out[m].value = occ[n].value;
            ++m;
        }
        free(occ);
    }
#else
    {
        int n;

        PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
            for (n = 0; NULL != opt->values && NULL != opt->values[n]; n++) {
                out[m].key = opt->key;
                out[m].value = opt->values[n];
                ++m;
            }
        }
    }
#endif

    *ordered = out;
    *nordered = m;
    return PRTE_SUCCESS;
}

/*
 * Turn the environment options into directives on the app's info list.
 *
 * These are applied to the process environment IN THE ORDER GIVEN, and
 * the info list is what carries that order downstream (see the
 * PRTE_JOB_*_ENVAR loop in odls_base_default_fns.c). The order matters
 * because the directives edit each other: SET replaces a value outright
 * while PREPEND/APPEND edit the one already there, so
 *
 *     --prepend-env "FOO[:]" x --set-env FOO=1     leaves  FOO=1
 *     --set-env FOO=1 --prepend-env "FOO[:]" x     leaves  FOO=x:1
 *
 * Both are what the user asked for; neither is a merge policy we get to
 * choose. So walk the occurrences in the order they were given rather
 * than looking each key up in a fixed sequence - and rather than walking
 * the instances list, which orders keys by first appearance and so puts
 * the second --set-env of
 *
 *     --set-env FOO=1 --prepend-env "FOO[:]" x --set-env FOO=2
 *
 * ahead of the prepend, arriving at FOO=x:2 where the user asked for
 * FOO=2.
 */
static int add_envar_directives(prte_pmix_app_t *app,
                                pmix_cli_result_t *results)
{
    prte_cli_occ_t *ordered = NULL;
    size_t nordered = 0, k;
    char *param, *value, *ptr, *tval;
    const char *key;
    pmix_envar_t envt;
    int i, rc;

    rc = collect_ordered(results, &ordered, &nordered);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    for (k = 0; k < nordered; k++) {
        key = ordered[k].key;

        if (0 == strcmp(key, PRTE_CLI_FWD_ENVAR)) {
            /* Construct the envar before every use.  Forwarding a variable
             * has no separator, but the field is packed and shipped with
             * the rest of the directive, so leaving it whatever was on the
             * stack put an uninitialized byte into the launch message. */
            PMIX_ENVAR_CONSTRUCT(&envt);
            param = strdup(ordered[k].value);
            /* if there is an '=' in it, then they are setting a value */
            if (NULL != (value = strchr(param, '='))) {
                *value = '\0';
                ++value;
                envt.envar = param;
                envt.value = strdup(value);
                PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
                PMIX_ENVAR_DESTRUCT(&envt);
            } else {
                // have to support the wildcard here
                if (NULL != (ptr = strchr(param, '*'))) {
                    *ptr = '\0';
                    for (i=0; NULL != environ[i]; i++) {
                        if (0 == strncmp(environ[i], param, strlen(param))) {
                            // this is a var to fwd
                            // extract the name and value
                            PMIX_ENVAR_CONSTRUCT(&envt);
                            ptr = strdup(environ[i]);
                            value = strchr(ptr, '=');
                            *value = '\0';
                            ++value;
                            envt.envar = ptr;
                            envt.value = strdup(value);
                            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
                            PMIX_ENVAR_DESTRUCT(&envt);
                        }
                    }
                    free(param);
                } else {
                    // given a unique name
                    value = getenv(param);
                    if (NULL == value) {
                        prte_show_help("help-schizo-base.txt", "missing-envar-param", true, param);
                        free(param);
                    } else {
                        PMIX_ENVAR_CONSTRUCT(&envt);
                        envt.envar = param;
                        envt.value = strdup(value);
                        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
                        PMIX_ENVAR_DESTRUCT(&envt);
                    }
                }
            }

        } else if (0 == strcmp(key, PMIX_CLI_PREPEND_ENVAR) ||
                   0 == strcmp(key, PMIX_CLI_APPEND_ENVAR)) {
            /* these two store the variable's name and the value as SEPARATE
             * occurrences, given one immediately after the other */
            bool prepend = (0 == strcmp(key, PMIX_CLI_PREPEND_ENVAR));

            PMIX_ENVAR_CONSTRUCT(&envt);
            param = strdup(ordered[k].value);
            if (k + 1 >= nordered || 0 != strcmp(ordered[k + 1].key, key)) {
                // the value it edits with is missing
                prte_show_help("help-prun.txt", "malformed-envar", true,
                               prepend ? "prepend" : "append", app->app.cmd, param);
                rc = PRTE_ERR_SILENT;
                free(param);
                goto done;
            }
            // find the [] enclosing the separator
            i = strlen(param);
            if (3 > i || ']' != param[i-1] || '[' != param[i-3]) {
                prte_show_help("help-prun.txt", "malformed-envar", true,
                               prepend ? "prepend" : "append", app->app.cmd, param);
                rc = PRTE_ERR_SILENT;
                free(param);
                goto done;
            }
            param[i-3] = '\0';
            envt.envar = param;
            envt.value = strdup(ordered[k + 1].value);
            envt.separator = param[i-2];
            PMIX_INFO_LIST_ADD(rc, app->info,
                               prepend ? PMIX_PREPEND_ENVAR : PMIX_APPEND_ENVAR,
                               &envt, PMIX_ENVAR);
            PMIX_ENVAR_DESTRUCT(&envt);
            // the value has now been consumed
            ++k;

        } else if (0 == strcmp(key, PMIX_CLI_SET_ENVAR)) {
            /* --set-env stores ONE value per occurrence ("NAME=value"),
             * unlike --prepend-env/--append-env above */
            param = strdup(ordered[k].value);
            // find the '=' separating name from value
            tval = strchr(param, '=');
            if (NULL == tval) {
                prte_show_help("help-prun.txt", "malformed-envar", true,
                               "set", app->app.cmd, param);
                rc = PRTE_ERR_SILENT;
                free(param);
                goto done;
            }
            *tval = '\0';
            ++tval;
            PMIX_ENVAR_CONSTRUCT(&envt);
            envt.envar = param;
            envt.value = strdup(tval);
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
            PMIX_ENVAR_DESTRUCT(&envt);

        } else if (0 == strcmp(key, PMIX_CLI_UNSET_ENVAR)) {
            PMIX_INFO_LIST_ADD(rc, app->info, PMIX_UNSET_ENVAR,
                               (char *) ordered[k].value, PMIX_STRING);
        }
    }
    rc = PRTE_SUCCESS;

done:
    if (NULL != ordered) {
        free(ordered);
    }
    return rc;
}

/*
 * Build one app context from one segment of the command line.
 *
 * There used to be a "char ***app_env" parameter here, described as the
 * base environment to carry across the recursive create_app() calls that an
 * appfile produced.  There is no recursion any more - an appfile is read
 * into the command line up front, by prte_parse_appfile() in prte.c, before
 * any of this runs - and this function had long since stopped writing
 * through the parameter, so the caller's variable was always NULL and
 * everything it was plumbed through was a no-op.  It is gone rather than
 * left looking load-bearing.
 */
static int create_app(prte_schizo_base_module_t *schizo, char **argv,
                      prte_pmix_app_t **app_ptr, bool *made_app,
                      char ***hostfiles, char ***hosts, pmix_list_t *jobdata)
{
    char cwd[PRTE_PATH_MAX];
    int i, count, rc;
    char *param, *value, *ptr;
    prte_pmix_app_t *app = NULL;
    pmix_cli_item_t *opt, *opt2;
    pmix_cli_result_t results;
    char *tval;
    prte_info_item_t *iptr;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    if ('-' != argv[1][0]) {
        results.tail = PMIx_Argv_copy(&argv[1]);
    } else {
        rc = schizo->parse_cli(argv, &results, PMIX_CLI_SILENT);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_SILENT != rc) {
                fprintf(stderr, "%s: command line error (%s)\n", argv[0], prte_strerror(rc));
            }
            PMIX_DESTRUCT(&results);
            return rc;
        }
    }
    // sanity check the results
    rc = schizo->check_sanity(&results);
    if (PRTE_SUCCESS != rc) {
        // sanity checker prints the reason
        PMIX_DESTRUCT(&results);
        return rc;
    }

    /* See if we have anything left */
    if (NULL == results.tail) {
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    /* Setup application context */
    app = PMIX_NEW(prte_pmix_app_t);
    app->app.argv = PMIx_Argv_copy(results.tail);
    // app->app.cmd is setup below.

    /* get the cwd - we may need it in several places */
    if (PRTE_SUCCESS != (rc = pmix_getcwd(cwd, sizeof(cwd)))) {
        prte_show_help("help-prun.txt", "prun:init-failure", true, "get the cwd", rc);
        goto cleanup;
    }

    /* Did the user specify a path to the executable? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PATH);
    if (NULL != opt) {
        param = opt->values[0];
        /* if this is a relative path, convert it to an absolute path */
        if (pmix_path_is_absolute(param)) {
            value = strdup(param);
        } else {
            /* construct the absolute path */
            value = pmix_os_path(false, cwd, param, NULL);
        }
        /* construct the new argv[0] */
        ptr = pmix_os_path(false, value, app->app.argv[0], NULL);
        free(value);
        free(app->app.argv[0]);
        app->app.argv[0] = ptr;
    }

    /* Did the user request a specific wdir? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_WDIR);
    if (NULL != opt) {
        param = opt->values[0];
        /* if this is a relative path, convert it to an absolute path */
        if (pmix_path_is_absolute(param)) {
            app->app.cwd = strdup(param);
        } else {
            /* construct the absolute path */
            app->app.cwd = pmix_os_path(false, cwd, param, NULL);
        }
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_WDIR_USER_SPECIFIED, NULL, PMIX_BOOL);
    } else if (pmix_cmd_line_is_taken(&results, PRTE_CLI_SET_CWD_SESSION)) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
    } else {
        app->app.cwd = strdup(cwd);
    }

    /* if they specified a process set name, then pass it along */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PSET);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PSET_NAME,
                           opt->values[0], PMIX_STRING);
    }

    /* Did the user specify a hostfile? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOSTFILE);
    if (NULL != opt) {
        for (i=0; NULL != opt->values[i]; i++) {
            if (!pmix_path_is_absolute(opt->values[i])) {
                value = pmix_os_path(false, cwd, opt->values[i], NULL);
                free(opt->values[i]);
                opt->values[i] = value;
            }
        }
        tval = PMIx_Argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOSTFILE,
                           tval, PMIX_STRING);
        free(tval);
        if (NULL != hostfiles) {
            for (i=0; NULL != opt->values[i]; i++) {
                PMIx_Argv_append_nosize(hostfiles, opt->values[i]);
            }
        }
    }

    /* Did the user specify an add-hostfile? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_ADDHOSTFILE);
    if (NULL != opt) {
        for (i=0; NULL != opt->values[i]; i++) {
            if (!pmix_path_is_absolute(opt->values[i])) {
                value = pmix_os_path(false, cwd, opt->values[i], NULL);
                free(opt->values[i]);
                opt->values[i] = value;
            }
        }
        tval = PMIx_Argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_ADD_HOSTFILE,
                           tval, PMIX_STRING);
        free(tval);
        // we don't add these to the hostfiles array as they
        // are not part of an initial DVM
    }

    /* Did the user specify any hosts? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_HOST);
    if (NULL != opt) {
        tval = PMIx_Argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_HOST, tval, PMIX_STRING);
        free(tval);
        if (NULL != hosts) {
            for (i=0; NULL != opt->values[i]; i++) {
                PMIx_Argv_append_nosize(hosts, opt->values[i]);
            }
        }
    }

    /* Did the user specify any add-hosts? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_ADDHOST);
    if (NULL != opt) {
        tval = PMIx_Argv_join(opt->values, ',');
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_ADD_HOST, tval, PMIX_STRING);
        free(tval);
        // we don't add these to the hosts array as they
        // are not part of an initial DVM
    }

    /* Did the user ask that nodes already in the allocation be brought
     * into the DVM? */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_ACTIVATE);
    if (NULL != opt) {
        char **atokens;
        int a;

        tval = PMIx_Argv_join(opt->values, ',');
        /* a "file=" entry names a hostfile that the DVM MASTER will read, so
         * a relative path has to be resolved against OUR cwd here - the same
         * thing --hostfile and --add-hostfile do above, and for the same
         * reason: the HNP is another process, very likely on another node,
         * and its cwd is not ours */
        atokens = PMIx_Argv_split(tval, ',');
        free(tval);
        for (a = 0; NULL != atokens[a]; a++) {
            if (0 != strncmp(atokens[a], "file=", 5) ||
                '\0' == atokens[a][5] ||
                pmix_path_is_absolute(&atokens[a][5])) {
                /* leave a bare "file=" alone: turning it into the cwd would
                 * hand the DVM a directory, where what it should report is
                 * that no filename was given */
                continue;
            }
            value = pmix_os_path(false, cwd, &atokens[a][5], NULL);
            free(atokens[a]);
            pmix_asprintf(&atokens[a], "file=%s", value);
            free(value);
        }
        tval = PMIx_Argv_join(atokens, ',');
        PMIx_Argv_free(atokens);
        PMIX_INFO_LIST_ADD(rc, app->info, PRTE_ACTIVATE_HOSTS, tval, PMIX_STRING);
        free(tval);
        // these name nodes the allocation already holds, so they are
        // likewise no part of an initial DVM
    }

    /* check for bozo error */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_NP);
    if (NULL != opt) {
        char *npend = NULL;
        long nprocs;

        /* A value that is not a number at all reads as 0, which is the
         * spelling of "let the mapping policy compute it" - so "-n four"
         * quietly launched some other number of processes rather than
         * being refused.  maxprocs is an int, so a value that does not fit
         * one has to go the same way: truncating it silently turns, say,
         * 4294967297 into 1. */
        errno = 0;
        nprocs = strtol(opt->values[0], &npend, 10);
        if (!isdigit((unsigned char) opt->values[0][0]) ||
            NULL == npend || '\0' != *npend || 0 != errno ||
            nprocs > (long) INT_MAX) {
            prte_show_help("help-prun.txt", "bad-option-input", true,
                           prte_tool_basename, "--" PRTE_CLI_NP,
                           opt->values[0], "a number of processes");
            rc = PRTE_ERR_FATAL;
            goto cleanup;
        }
        count = (int) nprocs;
        /* the leading-digit test above already refuses a sign, so this is
         * unreachable from the command line - it is kept because the check
         * is what the message names and the two must not drift apart */
        if (0 > count) {
            prte_show_help("help-prun.txt", "prun:negative-nprocs", true,
                           prte_tool_basename,
                           app->app.argv[0], count);
            rc = PRTE_ERR_FATAL;
            goto cleanup;
        }
        /* we don't require that the user provide --np or -n because
         * the cmd line might stipulate a mapping policy that computes
         * the number of procs - e.g., a map-by ppr option */
        app->app.maxprocs = count;
    }

    // check for PRRTE or PMIx prefixes for the daemons
    if (NULL != jobdata) {
        opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PREFIX);
        if (NULL != opt) {
            // only allow one instance of it, or all must be the same
            if (1 < (count = pmix_cmd_line_get_ninsts(&results, PRTE_CLI_PREFIX))) {
                param = NULL;
                for (i=0; i < count; i++) {
                    if (NULL == param) {
                        param = opt->values[i];
                    } else if (0 != strcmp(param, opt->values[i])) {
                        prte_show_help("help-plm-base.txt", "multiple-prefixes", true,
                                       prte_tool_basename, PRTE_CLI_PREFIX,
                                       PRTE_CLI_PREFIX, "PRRTE", PRTE_CLI_PREFIX,
                                       param, opt->values[i]);
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                }
            }
            iptr = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&iptr->info, "PRTE_PREFIX", opt->values[0], PMIX_STRING);
            pmix_list_append(jobdata, &iptr->super);
        }

        opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PMIX_PREFIX);
        if (NULL != opt) {
            /* Only allow one instance of it, or all must be the same.
             * Count THIS option's values: counting PRTE_CLI_PREFIX's here
             * both skipped the check whenever no --prefix was given, and
             * walked opt->values past its terminator when more --prefix
             * values were given than --pmix-prefix ones - "--prefix /x
             * --prefix /x --pmix-prefix /y" segfaulted on the NULL. */
            if (1 < (count = pmix_cmd_line_get_ninsts(&results, PRTE_CLI_PMIX_PREFIX))) {
                param = NULL;
                for (i=0; i < count; i++) {
                    if (NULL == param) {
                        param = opt->values[i];
                    } else if (0 != strcmp(param, opt->values[i])) {
                        prte_show_help("help-plm-base.txt", "multiple-prefixes", true,
                                       prte_tool_basename, PRTE_CLI_PMIX_PREFIX,
                                       PRTE_CLI_PMIX_PREFIX, "PMIx", PRTE_CLI_PMIX_PREFIX,
                                       param, opt->values[i]);
                        rc = PRTE_ERR_FATAL;
                        goto cleanup;
                    }
                }
            }
            iptr = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&iptr->info, PMIX_PREFIX, opt->values[0], PMIX_STRING);
            pmix_list_append(jobdata, &iptr->super);
        }
    }

    /* check for preload files */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PRELOAD_FILES);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_FILES, opt->values[0], PMIX_STRING);
    }

    /* check for preload binary */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_PRELOAD_BIN);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
    }

    /* Do not try to find argv[0] here -- the starter is responsible
     for that because it may not be relevant to try to find it on
     the node where prun is executing.  So just strdup() argv[0]
     into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        prte_show_help("help-prun.txt", "prun:call-failed", true, "prun", "library",
                       "strdup returned NULL", errno);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }

    /*
     * does the schizo being used have a setup_app method?
     * if so call here
     */
    if(NULL != schizo->setup_app) {
        rc = schizo->setup_app(app);
        if (PRTE_SUCCESS != rc) {
            goto cleanup;
        }
    }

    // parse any environment-related cmd line options
    rc = schizo->parse_env(prte_launch_environ, &app->app.env, &results);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }

    // check for any envar directives
    rc = add_envar_directives(app, &results);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }

    // check for PMIx prefix for the application
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_APP_PREFIX);
    if (NULL != opt) {
        // only allow one instance of it, or all must be the same
        if (1 < (count = pmix_cmd_line_get_ninsts(&results, PRTE_CLI_APP_PREFIX))) {
            param = NULL;
            for (i=0; i < count; i++) {
                if (NULL == param) {
                    param = opt->values[i];
                } else if (0 != strcmp(param, opt->values[i])) {
                    prte_show_help("help-plm-base.txt", "multiple-app-prefixes", true,
                                   prte_tool_basename, PRTE_CLI_APP_PREFIX,
                                   PRTE_CLI_APP_PREFIX, param, opt->values[i]);
                    rc = PRTE_ERR_FATAL;
                    goto cleanup;
                }
            }
        }
        // add it to the app's info
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PREFIX, opt->values[0], PMIX_STRING);
    } else if (NULL != (param = getenv("PMIX_APP_PREFIX"))) {
        // allow the cmd line to override the environment
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PREFIX, param, PMIX_STRING);
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_NO_APP_PREFIX);
    if (NULL != opt) {
        // cannot have a prefix as well as no-prefix
        if (NULL != (opt2 = pmix_cmd_line_get_param(&results, PRTE_CLI_APP_PREFIX))) {
            prte_show_help("help-plm-base.txt", "prefix-conflict", true,
                           prte_tool_basename, PRTE_CLI_APP_PREFIX, opt2->values[0],
                           PRTE_CLI_NO_APP_PREFIX);
            rc = PRTE_ERR_FATAL;
            goto cleanup;
        }
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PREFIX, NULL, PMIX_STRING);
    } else if (NULL != getenv("PMIX_APP_NO_PREFIX")) {
        // cannot have a prefix as well as no-prefix
        if (NULL != (opt2 = pmix_cmd_line_get_param(&results, PRTE_CLI_APP_PREFIX))) {
            prte_show_help("help-plm-base.txt", "prefix-conflict", true,
                           prte_tool_basename, PRTE_CLI_APP_PREFIX, opt2->values[0],
                           "PMIX_APP_NO_PREFIX");
            rc = PRTE_ERR_FATAL;
            goto cleanup;
        }
        // allow the cmd line to override the environment
        PMIX_INFO_LIST_ADD(rc, app->info, PMIX_PREFIX, NULL, PMIX_STRING);
    }

    // Hold the mapping/ranking/binding directives on the app object rather
    // than adding them to its spec. Whether they are per-app at all is not
    // known until every segment has been parsed - one directive applies to
    // the whole job, however many apps there are - so prte_parse_locals()
    // makes that call once it has seen them all.
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        app->mapby = strdup(opt->values[0]);
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        app->rankby = strdup(opt->values[0]);
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        app->bindto = strdup(opt->values[0]);
    }

    // Hold the job-level directives this segment carried, for the same
    // reason: the tool's global parse of the cmd line stops at the first
    // app, so a directive written in a later segment never reaches it.
    // These are not per-app at all - prte_parse_locals() hands every
    // segment's contribution back to that parse once the line is done.
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        app->output = PMIx_Argv_join(opt->values, ',');
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        app->display = PMIx_Argv_join(opt->values, ',');
    }
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_RTOS);
    if (NULL != opt) {
        app->rtos = PMIx_Argv_join(opt->values, ',');
    }

    /* The app is made.  Say so unconditionally rather than returning
     * whatever the last PMIX_INFO_LIST_ADD happened to leave in rc: a
     * failure from any of them is not acted on where it happens, so a stale
     * one here would hand the caller an error alongside made_app == true -
     * and the caller, seeing the error, drops the app it was just given. */
    rc = PRTE_SUCCESS;
    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

cleanup:
    if (NULL != app) {
        PMIX_RELEASE(app);
    }
    PMIX_DESTRUCT(&results);
    return rc;
}

/*
 * Hand out one class of directive (mapping, ranking or binding) once the
 * whole cmd line has been parsed.
 *
 * The first app segment is where a command line speaks for the job: it is
 * what a single-app line has, and it is what the reader sees first. So a
 * directive written there and nowhere else describes the whole job, however
 * many apps follow - which is also what lets it carry a qualifier that spans
 * the job (OVERSUBSCRIBE and friends), since an app that holds one has to
 * have it hoisted back out again later.
 *
 * Written anywhere else, it describes the app that carries it and only that
 * app. Its silent siblings are not agreeing with it - they said nothing, and
 * what an app that says nothing gets is the default. Reading a lone
 * directive on the third of four apps as the job's meant that asking for one
 * app to be placed differently silently placed all four that way, and there
 * was no way to say what was plainly meant.
 */
static int distribute_directive(pmix_list_t *apps, pmix_list_t *jobdata,
                                size_t offset, const char *key)
{
    prte_pmix_app_t *app;
    prte_info_item_t *item;
    char **held;
    int count = 0, rc;
    bool first_only;

    app = (prte_pmix_app_t *) pmix_list_get_first(apps);
    if (NULL == app || pmix_list_is_empty(apps)) {
        return PRTE_SUCCESS;
    }
    held = (char **) ((char *) app + offset);
    first_only = (NULL != *held);

    PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t) {
        held = (char **) ((char *) app + offset);
        if (NULL != *held) {
            ++count;
        }
    }
    if (0 == count) {
        return PRTE_SUCCESS;
    }
    /* only the first app carried it? */
    first_only = first_only && (1 == count);

    if (!first_only) {
        /* the user distinguished the apps - give each its own, and leave the
         * apps that gave none to the defaults */
        PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t) {
            held = (char **) ((char *) app + offset);
            if (NULL != *held) {
                PMIX_INFO_LIST_ADD(rc, app->info, key, *held, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    return prte_pmix_convert_status(rc);
                }
            }
        }
        return PRTE_SUCCESS;
    }

    /* the first app spoke for the job */
    if (NULL == jobdata) {
        /* nowhere to put it - leave it with the app that carried it so the
         * directive is not simply lost */
        PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t) {
            held = (char **) ((char *) app + offset);
            if (NULL != *held) {
                PMIX_INFO_LIST_ADD(rc, app->info, key, *held, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    return prte_pmix_convert_status(rc);
                }
            }
        }
        return PRTE_SUCCESS;
    }
    PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t) {
        held = (char **) ((char *) app + offset);
        if (NULL == *held) {
            continue;
        }
        item = PMIX_NEW(prte_info_item_t);
        if (NULL == item) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        PMIX_INFO_LOAD(&item->info, key, *held, PMIX_STRING);
        pmix_list_append(jobdata, &item->super);
        break;
    }
    return PRTE_SUCCESS;
}

/*
 * Gather one job-level option from every app segment that wrote it and hand
 * the result back to the tool's parse of the whole command line.
 *
 * Unlike the mapping directives above, these are never per-app: there is no
 * such thing as one app being displayed, or one app not launching.  The
 * only question is whether the segments agree, and that is decided in the
 * schizo base, where the option's vocabulary lives.
 */
static int hoist_job_option(pmix_list_t *apps, pmix_cli_result_t *results,
                            size_t offset, const char *key)
{
    prte_pmix_app_t *app;
    char **held, **contributions = NULL;
    int rc;

    if (NULL == results) {
        return PRTE_SUCCESS;
    }
    PMIX_LIST_FOREACH(app, apps, prte_pmix_app_t) {
        held = (char **) ((char *) app + offset);
        if (NULL != *held) {
            PMIx_Argv_append_nosize(&contributions, *held);
        }
    }
    rc = prte_schizo_base_hoist_job_option(results, key, contributions);
    PMIx_Argv_free(contributions);
    return rc;
}

int prte_parse_locals(prte_schizo_base_module_t *schizo,
                      pmix_list_t *jdata, char *argv[],
                      char ***hostfiles, char ***hosts,
                      pmix_list_t *jobdata,
                      pmix_cli_result_t *results)
{
    int i, rc;
    char **temp_argv;
    prte_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argv = NULL;
    PMIx_Argv_append_nosize(&temp_argv, argv[0]);

    for (i = 1; NULL != argv[i]; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (PMIx_Argv_count(temp_argv) > 1) {
                app = NULL;
                rc = create_app(schizo, temp_argv, &app, &made_app,
                                hostfiles, hosts, jobdata);
                if (PRTE_SUCCESS != rc) {
                    /* assume that the error message has already been
                     printed */
                    PMIx_Argv_free(temp_argv);
                    return rc;
                }
                if (made_app) {
                    pmix_list_append(jdata, &app->super);
                }

                /* Reset the temps */
                PMIx_Argv_free(temp_argv);
                temp_argv = NULL;
                PMIx_Argv_append_nosize(&temp_argv, argv[0]);
            }
        } else {
            PMIx_Argv_append_nosize(&temp_argv, argv[i]);
        }
    }

    if (PMIx_Argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(schizo, temp_argv, &app, &made_app,
                        hostfiles, hosts, jobdata);
        if (PRTE_SUCCESS != rc) {
            /* this return used to skip the free below entirely, so any
             * command line that fails its final segment leaked temp_argv -
             * "--display map --display cpus" is enough, since a repeated
             * option is refused here */
            PMIx_Argv_free(temp_argv);
            return rc;
        }
        if (made_app) {
            pmix_list_append(jdata, &app->super);
        }
    }

    PMIx_Argv_free(temp_argv);

    /* every segment has now been seen, so it can be decided whether the
     * mapping/ranking/binding directives are per-app or belong to the job */
    rc = distribute_directive(jdata, jobdata,
                              offsetof(prte_pmix_app_t, mapby), PMIX_MAPBY);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = distribute_directive(jdata, jobdata,
                              offsetof(prte_pmix_app_t, rankby), PMIX_RANKBY);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = distribute_directive(jdata, jobdata,
                              offsetof(prte_pmix_app_t, bindto), PMIX_BINDTO);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* the job-level options go back to the tool's own parse, which is where
     * every consumer of them looks - including the ones that apply them to
     * the DVM itself rather than to the job */
    rc = hoist_job_option(jdata, results,
                          offsetof(prte_pmix_app_t, output), PRTE_CLI_OUTPUT);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = hoist_job_option(jdata, results,
                          offsetof(prte_pmix_app_t, display), PRTE_CLI_DISPLAY);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = hoist_job_option(jdata, results,
                          offsetof(prte_pmix_app_t, rtos), PRTE_CLI_RTOS);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* All done */

    return PRTE_SUCCESS;
}
