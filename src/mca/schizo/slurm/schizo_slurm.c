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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include "schizo_slurm.h"
#include "src/mca/schizo/base/base.h"

static int parse_cli(char **argv, pmix_cli_result_t *results, bool silent);
static int parse_env(char **srcenv, char ***dstenv, pmix_cli_result_t *cli);
static int setup_fork(prte_job_t *jdata, prte_app_context_t *context);
static int detect_proxy(char *argv);
static void allow_run_as_root(pmix_cli_result_t *results);
static void job_info(pmix_cli_result_t *results,
                     void *jobinfo);
static int set_default_rto(prte_job_t *jdata,
                           prte_rmaps_options_t *options);
static int check_sanity(pmix_cli_result_t *results);

prte_schizo_base_module_t prte_schizo_slurm_module = {
    .name = "slurm",
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info,
    .set_default_rto = set_default_rto,
    .check_sanity = check_sanity
};

static struct option srunoptions[] = {
    /* basic options */
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_PARSEABLE, PMIX_ARG_NONE, 'p'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_PARSABLE, PMIX_ARG_NONE, 'p'), // synonym for parseable

    // MCA parameters
    PMIX_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PMIX_ARG_REQD),

    PMIX_OPTION_SHORT_DEFINE("ntasks", PMIX_ARG_REQD, 'n'),
    PMIX_OPTION_SHORT_DEFINE("distribution", PMIX_ARG_REQD, 'm'),
    PMIX_OPTION_DEFINE("cpu_bind", PMIX_ARG_REQD),

    PMIX_OPTION_END
};
static char *srunshorts = "h::m:n:";

static int convert_results(pmix_cli_result_t *results);

static int parse_cli(char **argv, pmix_cli_result_t *results,
                     bool silent)
{
    char *shorts, *helpfile;
    struct option *myoptions;
    int rc, n;
    pmix_cli_item_t *opt;
    PRTE_HIDE_UNUSED_PARAMS(silent);

    if (0 == strcmp(prte_tool_actual, "prte")) {
        myoptions = srunoptions;
        shorts = srunshorts;
        helpfile = "help-schizo-srun.txt";
    } else {
        /* this is an error */
        return PRTE_ERR_NOT_SUPPORTED;
    }

    pmix_tool_msg = "Report bugs to: https://github.com/openpmix/prrte";
    pmix_tool_org = "PRRTE";
    pmix_tool_version = prte_util_make_version_string("all", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                                      PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, NULL);

    rc = pmix_cmd_line_parse(argv, shorts, myoptions, NULL,
                             results, helpfile);
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

    rc = convert_results(results);
    if (PRTE_SUCCESS != rc) {
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

static int convert_results(pmix_cli_result_t *results)
{
    pmix_cli_item_t *opt;

    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        if (0 == strcmp(opt->key, "ntasks")) {
            /* translate this to PRTE_CLI_NP */
            free(opt->key);
            opt->key = strdup(PRTE_CLI_NP);
        } else if (0 == strcmp(opt->key, "distribution")) {
            /* translate this to PRTE_CLI_MAPBY */
            free(opt->key);
            opt->key = strdup(PRTE_CLI_MAPBY);
        } else if (0 == strcmp(opt->key, "cpu_bind")) {
            free(opt->key);
            opt->key = strdup(PRTE_CLI_BINDTO);
        }
    }
    return PRTE_SUCCESS;
}

static int parse_env(char **srcenv, char ***dstenv,
                     pmix_cli_result_t *cli)
{
    PRTE_HIDE_UNUSED_PARAMS(srcenv, dstenv, cli);
    return PRTE_SUCCESS;
}

static int setup_fork(prte_job_t *jdata, prte_app_context_t *app)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata, app);
    return PRTE_SUCCESS;
}

static int detect_proxy(char *personalities)
{
    char *evar;

    pmix_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__,
                        (NULL == personalities) ? "NULL" : personalities,
                        prte_tool_basename);

    /* COMMAND-LINE OVERRRULES ALL */
    if (NULL != personalities) {
        /* this is a list of personalities we need to check -
         * if it contains "slurm", then we are available but
         * at a low priority */
        if (NULL != strstr(personalities, "slurm")) {
            return prte_mca_schizo_slurm_component.priority;
        }
        return 0;
    }

    /* if we were told the proxy, then use it */
    if (NULL != (evar = getenv("PRTE_MCA_schizo_proxy"))) {
        if (0 == strcmp(evar, "slurm")) {
            /* they asked exclusively for us */
            return 100;
        } else {
            /* they asked for somebody else */
            return 0;
        }
    }

    /* if the tool is "srun", then use us */
    if (0 == strcmp(prte_tool_basename, "srun")) {
        return 100;
    }

    /* if none of those were true, then it isn't us */
    return 0;
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

static int check_sanity(pmix_cli_result_t *results)
{
    PRTE_HIDE_UNUSED_PARAMS(results);
    return PRTE_SUCCESS;
}
