/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>
#include <getopt.h>


#include "src/util/name_fns.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/pmix_init_util.h"

#include "src/mca/schizo/base/base.h"
#include "src/tools/psched/psched.h"

static int parse_cli(char **argv, pmix_cli_result_t *results, bool silent);
static int detect_proxy(char *argv);
static int parse_env(char **srcenv, char ***dstenv, pmix_cli_result_t *cli);
static void allow_run_as_root(pmix_cli_result_t *results);
static void job_info(pmix_cli_result_t *results,
                     void *jobinfo);
static int set_default_rto(prte_job_t *jdata,
                           prte_rmaps_options_t *options);

prte_schizo_base_module_t psched_schizo_module = {
    .name = "psched",
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = prte_schizo_base_setup_fork,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info,
    .set_default_rto = set_default_rto,
    .check_sanity = prte_schizo_base_sanity
};

static struct option pschedoptions[] = {
    /* basic options */
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),

    // MCA parameters
    PMIX_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_TUNE, PMIX_ARG_REQD),

    // resource options
    PMIX_OPTION_DEFINE(PRTE_CLI_DEFAULT_HOSTFILE, PMIX_ARG_REQD),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PMIX_ARG_REQD, 'H'),
    PMIX_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("machinefile", PMIX_ARG_REQD),

    // DVM options
    PMIX_OPTION_DEFINE(PRTE_CLI_RUN_AS_ROOT, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_DAEMONIZE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_NO_READY_MSG, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_SET_SID, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_TMPDIR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_PID, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_URI, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_KEEPALIVE, PMIX_ARG_REQD),

    // debug options
    PMIX_OPTION_DEFINE(PRTE_CLI_DEBUG, PMIX_ARG_NONE),

    PMIX_OPTION_END
};
static char *pschedshorts = "h::vVH:";

static int schizo_base_verbose = -1;
void psched_schizo_init(void)
{
    pmix_output_stream_t lds;

    pmix_mca_base_var_register("prte", "schizo", "base", "verbose",
                               "Verbosity for debugging schizo framework",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &schizo_base_verbose);
    if (0 <= schizo_base_verbose) {
        PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
        lds.lds_want_stdout = true;
        prte_schizo_base_framework.framework_output = pmix_output_open(&lds);
        PMIX_DESTRUCT(&lds);
        pmix_output_set_verbosity(prte_schizo_base_framework.framework_output, schizo_base_verbose);
    }

    pmix_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s schizo:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
}

static int parse_cli(char **argv, pmix_cli_result_t *results,
                     bool silent)
{
    int rc, n;
    pmix_cli_item_t *opt;
    PRTE_HIDE_UNUSED_PARAMS(silent);

    pmix_tool_msg = "Report bugs to: https://github.com/openpmix/prrte";
    pmix_tool_org = "PRRTE";
    pmix_tool_version = prte_util_make_version_string("all", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                                      PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, NULL);

    rc = pmix_cmd_line_parse(argv, pschedshorts, pschedoptions, NULL,
                             results, "help-psched.txt");
    if (PMIX_SUCCESS != rc) {
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* pmix cmd line interpreter output result
             * successfully - usually means version or
             * some other stock output was generated */
            return PRTE_OPERATION_SUCCEEDED;
        }
        rc = prte_pmix_convert_status(rc);
        return rc;
    }

    // handle relevant MCA params
    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        if (0 == strcmp(opt->key, PRTE_CLI_PRTEMCA)) {
            for (n=0; NULL != opt->values[n]; n++) {
                prte_schizo_base_expose(opt->values[n], "PRTE_MCA_");
            }
        } else if (0 == strcmp(opt->key, PRTE_CLI_PMIXMCA)) {
            for (n=0; NULL != opt->values[n]; n++) {
                prte_schizo_base_expose(opt->values[n], "PMIX_MCA_");
            }
        }
    }
    return PRTE_SUCCESS;
}

static int parse_env(char **srcenv, char ***dstenv,
                     pmix_cli_result_t *cli)
{
    int i, j, n;
    char *p1, *p2;
    char **env;
    char **xparams = NULL, **xvals = NULL;
    char *param, *value;
    pmix_cli_item_t *opt;
    PRTE_HIDE_UNUSED_PARAMS(srcenv);

    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (NULL == cli) {
        return PRTE_SUCCESS;
    }

    env = *dstenv;

    /* look for -x options - not allowed to conflict with a -mca option */
    opt = pmix_cmd_line_get_param(cli, PRTE_CLI_FWD_ENVAR);
    if (NULL != opt) {
        for (j=0; NULL != opt->values[j]; j++) {
            /* the value is the envar */
            p1 = opt->values[j];
            /* if there is an '=' in it, then they are setting a value */
            if (NULL != (p2 = strchr(p1, '='))) {
                *p2 = '\0';
                ++p2;
            } else {
                p2 = getenv(p1);
                if (NULL == p2) {
                    pmix_show_help("help-schizo-base.txt", "missing-envar-param", true, p1);
                    continue;
                }
            }

            /* check if it is already present in the environment */
            for (n = 0; NULL != env && NULL != env[n]; n++) {
                param = strdup(env[n]);
                value = strchr(param, '=');
                *value = '\0';
                value++;
                /* check if parameter is already present */
                if (0 == strcmp(param, p1)) {
                    /* we do have it - check for same value */
                    if (0 != strcmp(value, p2)) {
                        /* this is an error - different values */
                        pmix_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                       value);
                        free(param);
                        PMIX_ARGV_FREE_COMPAT(xparams);
                        PMIX_ARGV_FREE_COMPAT(xvals);
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
                free(param);
            }

            /* check if we already processed a conflicting -x version with MCA prefix */
            if (NULL != xparams) {
                for (i = 0; NULL != xparams[i]; i++) {
                    if (0 == strncmp("PRTE_MCA_", p1, strlen("PRTE_MCA_"))) {
                        /* this is an error - different values */
                        pmix_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                       xvals[i]);
                        PMIX_ARGV_FREE_COMPAT(xparams);
                        PMIX_ARGV_FREE_COMPAT(xvals);
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
            }

            /* cache this for later inclusion - do not modify dstenv in this loop */
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&xparams, p1);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&xvals, p2);
        }
    }

    /* add the -x values */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            PMIX_SETENV_COMPAT(xparams[i], xvals[i], true, dstenv);
        }
        PMIX_ARGV_FREE_COMPAT(xparams);
        PMIX_ARGV_FREE_COMPAT(xvals);
    }

    return PRTE_SUCCESS;
}

static int detect_proxy(char *personalities)
{
    pmix_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__,
                        (NULL == personalities) ? "NULL" : personalities,
                        prte_tool_basename);

    if (NULL != personalities) {
        /* this is a list of personalities we need to check -
         * if it contains "psched", then we are available
         * to be used */
        if (NULL != strstr(personalities, "psched")) {
            return 100;
        }
        return 0;
    }

    /* not us */
    return -1;
}

static void allow_run_as_root(pmix_cli_result_t *cli)
{
    char *r1, *r2;

    if (pmix_cmd_line_is_taken(cli, "allow-run-as-root")) {
        prte_allow_run_as_root = true;
        return;
    }

    if (NULL != (r1 = getenv("PRTE_ALLOW_RUN_AS_ROOT"))
        && NULL != (r2 = getenv("PRTE_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            prte_allow_run_as_root = true;
            return;
        }
    }

    prte_schizo_base_root_error_msg();
}

static void job_info(pmix_cli_result_t *results,
                     void *jobinfo)
{
    PRTE_HIDE_UNUSED_PARAMS(results, jobinfo);
    return;
}

static int set_default_rto(prte_job_t *jdata,
                           prte_rmaps_options_t *options)
{
    PRTE_HIDE_UNUSED_PARAMS(options);
    return prte_state_base_set_runtime_options(jdata, NULL);
}
