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
 * Copyright (c) 2018-2021 IBM Corporation.  All rights reserved.
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

#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif

#include "src/util/pmix_argv.h"
#include "src/util/keyval_parse.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/path.h"
#include "src/util/pmix_environ.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "src/mca/base/prte_mca_base_vari.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"

#include "schizo_ompi.h"
#include "src/mca/schizo/base/base.h"

static int parse_cli(char **argv, prte_cli_result_t *results, bool silent);
static int detect_proxy(char *argv);
static int parse_env(char **srcenv, char ***dstenv, prte_cli_result_t *cli);
static void allow_run_as_root(prte_cli_result_t *results);
static int set_default_ranking(prte_job_t *jdata,
                               prte_schizo_options_t *options);
static int setup_fork(prte_job_t *jdata, prte_app_context_t *context);
static void job_info(prte_cli_result_t *results,
                     void *jobinfo);

prte_schizo_base_module_t prte_schizo_ompi_module = {
    .name = "ompi",
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .set_default_ranking = set_default_ranking,
    .job_info = job_info
};

static struct option ompioptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSEABLE, PRTE_ARG_NONE, 'p'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSABLE, PRTE_ARG_NONE, 'p'), // synonym for parseable
    PRTE_OPTION_DEFINE(PRTE_CLI_PERSONALITY, PRTE_ARG_REQD),

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("omca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("gomca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TUNE, PRTE_ARG_REQD),

    PRTE_OPTION_DEFINE(PRTE_CLI_LAUNCH_AGENT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_MAX_VM_SIZE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS_FILE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_LEAVE_SESSION_ATTACHED, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_TMPDIR, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PREFIX, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NOPREFIX, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_FWD_SIGNALS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_RUN_AS_ROOT, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_CHILD_SEP, PRTE_ARG_NONE),

    /* debug options */
    PRTE_OPTION_DEFINE(PRTE_CLI_XTERM, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_ON_EXEC, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_INIT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_APP, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TIMEOUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_STATE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STACK_TRACES, PRTE_ARG_NONE),
#ifdef PMIX_SPAWN_TIMEOUT
    PRTE_OPTION_DEFINE(PRTE_CLI_SPAWN_TIMEOUT, PRTE_ARG_REQD),
#endif

    /* Conventional options - for historical compatibility, support
     * both single and multi dash versions */
    /* Number of processes; -c, -n, --n, -np, and --np are all
     synonyms */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'n'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'c'),
    PRTE_OPTION_DEFINE("n", PRTE_ARG_REQD),  // will be converted to "np" after parsing
    PRTE_OPTION_SHORT_DEFINE("N", PRTE_ARG_REQD, 'N'),
    PRTE_OPTION_DEFINE(PRTE_CLI_APPFILE, PRTE_ARG_REQD),

    /* output options */
    PRTE_OPTION_DEFINE(PRTE_CLI_OUTPUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STREAM_BUF, PRTE_ARG_REQD),

    /* input options */
    PRTE_OPTION_DEFINE(PRTE_CLI_STDIN, PRTE_ARG_REQD),


    /* launch options */
    PRTE_OPTION_DEFINE(PRTE_CLI_PRELOAD_FILES, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PRELOAD_BIN, PRTE_ARG_NONE, 's'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_FWD_ENVAR, PRTE_ARG_REQD, 'x'),
    PRTE_OPTION_DEFINE(PRTE_CLI_WDIR, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("wd", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PATH, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SHOW_PROGRESS, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_PSET, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("machinefile", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEFAULT_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PRTE_ARG_REQD, 'H'),

    /* placement options */
    /* Mapping options */
    PRTE_OPTION_DEFINE(PRTE_CLI_MAPBY, PRTE_ARG_REQD),

    /* Ranking options */
    PRTE_OPTION_DEFINE(PRTE_CLI_RANKBY, PRTE_ARG_REQD),

    /* Binding options */
    PRTE_OPTION_DEFINE(PRTE_CLI_BINDTO, PRTE_ARG_REQD),

    /* display options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DISPLAY, PRTE_ARG_REQD),

    /* developer options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DO_NOT_LAUNCH, PRTE_ARG_NONE),


#if PRTE_ENABLE_FT
    PRTE_OPTION_DEFINE(PRTE_CLI_ENABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_MAX_RESTARTS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DISABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_CONTINUOUS, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("with-ft", PRTE_ARG_REQD),
#endif

    /* mpiexec mandated form launch key parameters */
    PRTE_OPTION_DEFINE("initial-errhandler", PRTE_ARG_REQD),

    /* Display Commumication Protocol : MPI_Init */
    PRTE_OPTION_DEFINE("display-comm", PRTE_ARG_NONE),

    /* Display Commumication Protocol : MPI_Finalize */
    PRTE_OPTION_DEFINE("display-comm-finalize", PRTE_ARG_NONE),

    // unsupported, but mandated options
    PRTE_OPTION_DEFINE("soft", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("arch", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("file", PRTE_ARG_REQD),

    // deprecated options
    PRTE_OPTION_DEFINE("mca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("gmca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("xml", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("tag-output", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("timestamp-output", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("output-directory", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("output-filename", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("merge-stderr-to-stdout", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-devel-map", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-topo", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("report-bindings", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-devel-allocation", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-map", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-allocation", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("nolocal", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("oversubscribe", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("nooversubscribe", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("use-hwthread-cpus", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("cpu-set", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("cpu-list", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("--bind-to-core", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("bynode", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("bycore", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("byslot", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("cpus-per-proc", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("cpus-per-rank", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("npernode", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("pernode", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("npersocket", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("ppr", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("amca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("am", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("rankfile", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("output-proctable", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("debug", PRTE_ARG_NONE),

    PRTE_OPTION_END
};
static char *ompishorts = "h::vVpn:c:N:sH:x:";

static int convert_deprecated_cli(prte_cli_result_t *results,
                                  bool silent);

static int parse_cli(char **argv, prte_cli_result_t *results,
                     bool silent)
{
    int rc, n;
    prte_cli_item_t *opt;
    char *p1;
    char **pargv;

    /* backup the argv */
    pargv = pmix_argv_copy(argv);

    /* handle the non-compliant options - i.e., the single-dash
     * multi-character options mandated by the MPI standard */
    for (n=0; NULL != pargv[n]; n++) {
        if (0 == strcmp(pargv[n], "-soft")) {
            free(pargv[n]);
            pargv[n] = strdup("--soft");
        } else if (0 == strcmp(pargv[n], "-host")) {
            free(pargv[n]);
            pargv[n] = strdup("--host");
        } else if (0 == strcmp(pargv[n], "-arch")) {
            free(pargv[n]);
            pargv[n] = strdup("--arch");
        } else if (0 == strcmp(pargv[n], "-wdir")) {
            free(pargv[n]);
            pargv[n] = strdup("--wdir");
        } else if (0 == strcmp(pargv[n], "-path")) {
            free(pargv[n]);
            pargv[n] = strdup("--path");
        } else if (0 == strcmp(pargv[n], "-file")) {
            free(pargv[n]);
            pargv[n] = strdup("--file");
        } else if (0 == strcmp(pargv[n], "-initial-errhandler")) {
            free(pargv[n]);
            pargv[n] = strdup("--initial-errhandler");
        } else if (0 == strcmp(pargv[n], "-np")) {
            free(pargv[n]);
            pargv[n] = strdup("--np");
        }
#if PRTE_ENABLE_FT
        else if (0 == strcmp(pargv[n], "-with-ft")) {
            free(pargv[n]);
            pargv[n] = strdup("--with-ft");
        }
#endif
    }

    rc = prte_cmd_line_parse(pargv, ompishorts, ompioptions, NULL,
                             results, "help-schizo-ompi.txt");
    pmix_argv_free(pargv);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* check for deprecated options - warn and convert them */
    rc = convert_deprecated_cli(results, silent);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    // handle relevant MCA params
    PRTE_LIST_FOREACH(opt, &results->instances, prte_cli_item_t) {
        if (0 == strcmp(opt->key, PRTE_CLI_PRTEMCA)) {
            for (n=0; NULL != opt->values[n]; n++) {
                p1 = opt->values[n];
                prte_schizo_base_expose(p1, "PRTE_MCA_");
            }
        } else if (0 == strcmp(opt->key, PRTE_CLI_PMIXMCA)) {
            for (n=0; NULL != opt->values[n]; n++) {
                p1 = opt->values[n];
                prte_schizo_base_expose(p1, "PMIX_MCA_");
            }
#if PRTE_ENABLE_FT
        } else if (0 == strcmp(opt->key, "with-ft")) {
            if (NULL == opt->values || NULL == opt->values[0]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = opt->values[0];
            if (0 != strcmp("no", p1) && 0 != strcmp("false", p1) && 0 != strcmp("0", p1)) {
                if (0 == strcmp("yes", p1) || 0 == strcmp("true", p1) || 0 == strcmp("1", p1)
                    || 0 == strcmp("ulfm", p1) || 0 == strcmp("mpi", p1)) {
                        /* push it into our environment */
                    prte_schizo_base_expose("prte_enable_ft=1", "PRTE_MCA_");
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:ompi:parse_cli pushing PRTE_MCA_prte_enable_ft=1 into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    prte_enable_recovery = true;
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:ompi:parse_cli pushing OMPI_MCA_mpi_ft_enable into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    prte_schizo_base_expose("mpi_ft_enable=1", "OMPI_MCA_");
                }
                else {
                    prte_output(0, "UNRECOGNIZED OPTION: --with-ft %s", p1);
                    return PRTE_ERR_FATAL;
                }
            }
#endif
        }
    }

    if (NULL != results->tail) {
        /* search for the leader of the tail */
        for (n=0; NULL != argv[n]; n++) {
            if (0 == strcmp(results->tail[0], argv[n])) {
                /* this starts the tail - replace the rest of the
                 * tail with the original argv */
                pmix_argv_free(results->tail);
                results->tail = pmix_argv_copy(&argv[n]);
                break;
            }
        }
    }

    return PRTE_SUCCESS;
};

static int convert_deprecated_cli(prte_cli_result_t *results,
                                  bool silent)
{
    char *option, *p1, *p2, *tmp, *tmp2, *output, *modifier;
    int rc = PRTE_SUCCESS;
    prte_cli_item_t *opt, *nxt;
    prte_value_t *pval, val;
    bool warn;

    if (silent) {
        warn = false;
    } else {
        warn = prte_schizo_ompi_component.warn_deprecations;
    }

    PRTE_LIST_FOREACH_SAFE(opt, nxt, &results->instances, prte_cli_item_t) {
        option = opt->key;
        if (0 == strcmp(option, "n")) {
            /* if they passed a "--n" option, we need to convert it
             * back to the "--np" one without a deprecation warning */
            rc = prte_schizo_base_add_directive(results, option, PRTE_CLI_NP, opt->values[0], false);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --nolocal -> --map-by :nolocal */
        else if (0 == strcmp(option, "nolocal")) {
            rc = prte_schizo_base_add_qualifier(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_NOLOCAL,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --oversubscribe -> --map-by :OVERSUBSCRIBE */
        else if (0 == strcmp(option, "oversubscribe")) {
            rc = prte_schizo_base_add_qualifier(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_OVERSUB,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --nooversubscribe -> --map-by :NOOVERSUBSCRIBE */
        else if (0 == strcmp(option, "nooversubscribe")) {
            rc = prte_schizo_base_add_qualifier(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_NOOVER,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --use-hwthread-cpus -> --bind-to hwthread */
        else if (0 == strcmp(option, "use-hwthread-cpus")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_BINDTO, PRTE_CLI_HWT,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --cpu-set and --cpu-list -> --map-by pe-list:X
         */
        else if (0 == strcmp(option, "cpu-set") || 0 == strcmp(option, "cpu-list")) {
            pmix_asprintf(&p2, "%s%s", PRTE_CLI_PELIST, opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --bind-to-core and --bind-to-socket -> --bind-to X */
        else if (0 == strcmp(option, "bind-to-core")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_BINDTO, PRTE_CLI_CORE,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        } else if (0 == strcmp(option, "bind-to-socket")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_BINDTO, PRTE_CLI_PACKAGE,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --bynode -> "--map-by X --rank-by X" */
        else if (0 == strcmp(option, "bynode")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_NODE,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --bycore -> "--map-by X --rank-by X" */
        else if (0 == strcmp(option, "bycore")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_CORE,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --byslot -> "--map-by X --rank-by X" */
        else if (0 == strcmp(option, "byslot")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, PRTE_CLI_SLOT,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --cpus-per-proc/rank X -> --map-by :pe=X */
        else if (0 == strcmp(option, "cpus-per-proc") || 0 == strcmp(option, "cpus-per-rank")) {
            pmix_asprintf(&p2, "%s%s", PRTE_CLI_PE, opt->values[0]);
            rc = prte_schizo_base_add_qualifier(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* -N ->   map-by ppr:N:node */
        else if (0 == strcmp(option, "N")) {
            pmix_asprintf(&p2, "ppr:%s:node", opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --npernode X and --npersocket X -> --map-by ppr:X:node/socket */
        else if (0 == strcmp(option, "npernode")) {
            pmix_asprintf(&p2, "ppr:%s:node", opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        } else if (0 == strcmp(option, "pernode")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, "ppr:1:node",
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        } else if (0 == strcmp(option, "npersocket")) {
            pmix_asprintf(&p2, "ppr:%s:package", opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --ppr X -> --map-by ppr:X */
        else if (0 == strcmp(option, "ppr")) {
            /* if they didn't specify a complete pattern, then this is an error */
            if (NULL == strchr(opt->values[0], ':')) {
                prte_show_help("help-schizo-base.txt", "bad-ppr", true, opt->values[0], true);
                return PRTE_ERR_SILENT;
            }
            pmix_asprintf(&p2, "ppr:%s", opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --am[ca] X -> --tune X */
        else if (0 == strcmp(option, "amca") || 0 == strcmp(option, "am")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_TUNE, opt->values[0],
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --rankfile X -> map-by rankfile:file=X */
        else if (0 == strcmp(option, "rankfile")) {
            pmix_asprintf(&p2, "%s%s", PRTE_CLI_QFILE, opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option, PRTE_CLI_MAPBY, p2, true);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --tag-output  ->  "--output tag */
        else if (0 == strcmp(option, "tag-output")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, PRTE_CLI_TAG,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --timestamp-output  ->  --output timestamp */
        else if (0 == strcmp(option, "timestamp-output")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, PRTE_CLI_TIMESTAMP,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --output-directory DIR  ->  --output dir=DIR */
        else if (0 == strcmp(option, "output-directory")) {
            pmix_asprintf(&p2, "%s%s", PRTE_CLI_QDIR, opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --output-filename DIR  ->  --output file=file */
        else if (0 == strcmp(option, "output-filename")) {
            pmix_asprintf(&p2, "%s%s", PRTE_CLI_QFILE, opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --xml  ->  --output xml */
        else if (0 == strcmp(option, "xml")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, PRTE_CLI_XML,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --display-devel-map  -> --display allocation-devel */
        else if (0 == strcmp(option, "display-devel-map")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_MAPDEV,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --output-proctable  ->  --display map-devel */
        else if (0 == strcmp(option, "output-proctable")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_MAPDEV,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --display-map  ->  --display map */
        else if (0 == strcmp(option, "display-map")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_MAP,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --display-topo  ->  --display topo */
        else if (0 == strcmp(option, "display-topo")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_TOPO,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --report-bindings  ->  --display bind */
        else if (0 == strcmp(option, "report-bindings")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_BIND,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --display-allocation  ->  --display allocation */
        else if (0 == strcmp(option, "display-allocation")) {
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_DISPLAY, PRTE_CLI_ALLOC,
                                                warn);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --debug will be deprecated starting with open mpi v5
         */
        else if (0 == strcmp(option, "debug")) {
            if (warn) {
                prte_show_help("help-schizo-base.txt", "deprecated-inform", true, option,
                               "This CLI option will be deprecated starting in Open MPI v5");
            }
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
       }
        /* --map-by socket ->  --map-by package */
        else if (0 == strcmp(option, PRTE_CLI_MAPBY)) {
            /* check the value of the option for "socket" */
            if (0 == strncasecmp(opt->values[0], "socket", strlen("socket"))) {
                p1 = strdup(opt->values[0]); // save the original option
                /* replace "socket" with "package" */
                if (NULL == (p2 = strchr(opt->values[0], ':'))) {
                    /* no modifiers */
                    tmp = strdup(PRTE_CLI_PACKAGE);
                } else {
                    *p2 = '\0';
                    ++p2;
                    pmix_asprintf(&tmp, "%s:%s", PRTE_CLI_PACKAGE, p2);
                }
                if (warn) {
                    pmix_asprintf(&p2, "%s %s", option, p1);
                    pmix_asprintf(&tmp2, "%s %s", option, tmp);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                                   tmp2);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(tmp2);
                    free(p2);
                }
                free(p1);
                free(opt->values[0]);
                opt->values[0] = tmp;
            }
        }
        /* --rank-by socket ->  --rank-by package */
        else if (0 == strcmp(option, PRTE_CLI_RANKBY)) {
            /* check the value of the option for "socket" */
            if (0 == strncasecmp(opt->values[0], "socket", strlen("socket"))) {
                p1 = strdup(opt->values[0]); // save the original option
                /* replace "socket" with "package" */
                if (NULL == (p2 = strchr(opt->values[0], ':'))) {
                    /* no modifiers */
                    tmp = strdup("package");
                } else {
                    *p2 = '\0';
                    ++p2;
                    pmix_asprintf(&tmp, "package:%s", p2);
                }
                if (warn) {
                    pmix_asprintf(&p2, "%s %s", option, p1);
                    pmix_asprintf(&tmp2, "%s %s", option, tmp);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                                   tmp2);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(tmp2);
                    free(p2);
                }
                free(p1);
                free(opt->values[0]);
                opt->values[0] = tmp;
            }
        }
        /* --bind-to socket ->  --bind-to package */
        else if (0 == strcmp(option, PRTE_CLI_BINDTO)) {
            /* check the value of the option for "socket" */
            if (0 == strncasecmp(opt->values[0], "socket", strlen("socket"))) {
                p1 = strdup(opt->values[0]); // save the original option
                /* replace "socket" with "package" */
                if (NULL == (p2 = strchr(opt->values[0], ':'))) {
                    /* no modifiers */
                    tmp = strdup(PRTE_CLI_PACKAGE);
                } else {
                    *p2 = '\0';
                    ++p2;
                    pmix_asprintf(&tmp, "%s:%s", PRTE_CLI_PACKAGE, p2);
                }
                if (warn) {
                    pmix_asprintf(&p2, "%s %s", option, p1);
                    pmix_asprintf(&tmp2, "%s %s", option, tmp);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                                   tmp2);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(tmp2);
                    free(p2);
                }
                free(p1);
                free(opt->values[0]);
                opt->values[0] = tmp;
            }
        }
    }

    return rc;
}

static int check_cache_noadd(char ***c1, char ***c2, char *p1, char *p2)
{
    char **cache;
    char **cachevals;
    int k;

    if (NULL == c1 || NULL == c2) {
        return PRTE_SUCCESS;
    }

    cache = *c1;
    cachevals = *c2;

    if (NULL != cache) {
        /* see if we already have these */
        for (k = 0; NULL != cache[k]; k++) {
            if (0 == strcmp(cache[k], p1)) {
                /* we do have it - check for same value */
                if (0 != strcmp(cachevals[k], p2)) {
                    /* this is an error */
                    prte_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                   cachevals[k]);
                    return PRTE_ERR_BAD_PARAM;
                }
            }
        }
    }
    return PRTE_SUCCESS;
}

static int check_cache(char ***c1, char ***c2, char *p1, char *p2)
{
    int rc;

    rc = check_cache_noadd(c1, c2, p1, p2);

    if (PRTE_SUCCESS == rc) {
        /* add them to the cache */
        pmix_argv_append_nosize(c1, p1);
        pmix_argv_append_nosize(c2, p2);
    }
    return rc;
}

static int process_envar(const char *p, char ***cache, char ***cachevals)
{
    char *value, **tmp;
    char *p1, *p2;
    size_t len;
    int k, rc = PRTE_SUCCESS;
    bool found;

    p1 = strdup(p);
    if (NULL != (value = strchr(p1, '='))) {
        /* terminate the name of the param */
        *value = '\0';
        /* step over the equals */
        value++;
        rc = check_cache(cache, cachevals, p1, value);
    } else {
        /* check for a '*' wildcard at the end of the value */
        if ('*' == p1[strlen(p1) - 1]) {
            /* search the local environment for all params
             * that start with the string up to the '*' */
            p1[strlen(p1) - 1] = '\0';
            len = strlen(p1);
            for (k = 0; NULL != environ[k]; k++) {
                if (0 == strncmp(environ[k], p1, len)) {
                    value = strdup(environ[k]);
                    /* find the '=' sign */
                    p2 = strchr(value, '=');
                    if (NULL == p2) {
                        free(p1);
                        free(value);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    *p2 = '\0';
                    ++p2;
                    rc = check_cache(cache, cachevals, value, p2);
                    free(value);
                }
            }
        } else {
            value = getenv(p1);
            if (NULL != value) {
                rc = check_cache(cache, cachevals, p1, value);
            } else {
                found = false;
                if (NULL != cache) {
                    /* see if it is already in the cache */
                    tmp = *cache;
                    for (k = 0; NULL != tmp[k]; k++) {
                        if (0 == strncmp(p1, tmp[k], strlen(p1))) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    prte_show_help("help-schizo-base.txt", "env-not-found", true, p1);
                    rc = PRTE_ERR_NOT_FOUND;
                }
            }
        }
    }
    free(p1);
    return rc;
}

/* process params from an env_list - add them to the cache */
static int process_token(char *token, char ***cache, char ***cachevals)
{
    char *ptr, *value;
    int rc;

    if (NULL == (ptr = strchr(token, '='))) {
        value = getenv(token);
        if (NULL == value) {
            return PRTE_ERR_NOT_FOUND;
        }

        /* duplicate the value to silence tainted string coverity issue */
        value = strdup(value);
        if (NULL == value) {
            /* out of memory */
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        if (NULL != (ptr = strchr(value, '='))) {
            *ptr = '\0';
            rc = check_cache(cache, cachevals, value, ptr + 1);
        } else {
            rc = check_cache(cache, cachevals, token, value);
        }

        free(value);
    } else {
        *ptr = '\0';
        rc = check_cache(cache, cachevals, token, ptr + 1);
        /* NTH: don't bother resetting ptr to = since the string will not be used again */
    }
    return rc;
}

static int process_env_list(const char *env_list, char ***xparams, char ***xvals, char sep)
{
    char **tokens;
    int rc = PRTE_SUCCESS;

    tokens = pmix_argv_split(env_list, (int) sep);
    if (NULL == tokens) {
        return PRTE_SUCCESS;
    }

    for (int i = 0; NULL != tokens[i]; ++i) {
        rc = process_token(tokens[i], xparams, xvals);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_NOT_FOUND == rc) {
                prte_show_help("help-schizo-base.txt", "incorrect-env-list-param", true, tokens[i],
                               env_list);
            }
            break;
        }
    }

    pmix_argv_free(tokens);
    return rc;
}

static int process_tune_files(char *filename, char ***dstenv, char sep)
{
    FILE *fp;
    char **tmp, **opts, *line, *param, *p1, *p2;
    int i, n, rc = PRTE_SUCCESS;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;

    tmp = pmix_argv_split(filename, sep);
    if (NULL == tmp) {
        return PRTE_SUCCESS;
    }

    /* Iterate through all the files passed in -- it is an ERROR if
     * a given param appears more than once with different values */

    for (i = 0; NULL != tmp[i]; i++) {
        fp = fopen(tmp[i], "r");
        if (NULL == fp) {
            /* if the file given wasn't absolute, check in the default location */
            if (!prte_path_is_absolute(tmp[i])) {
                p1 = pmix_os_path(false, DEFAULT_PARAM_FILE_PATH, tmp[i], NULL);
                fp = fopen(p1, "r");
                if (NULL == fp) {
                    prte_show_help("help-schizo-base.txt", "missing-param-file-def", true, tmp[i], p1);;
                    pmix_argv_free(tmp);
                    pmix_argv_free(cache);
                    pmix_argv_free(cachevals);
                    pmix_argv_free(xparams);
                    pmix_argv_free(xvals);
                    free(p1);
                    return PRTE_ERR_NOT_FOUND;
                }
                free(p1);
            } else {
                prte_show_help("help-schizo-base.txt", "missing-param-file", true, tmp[i]);;
                pmix_argv_free(tmp);
                pmix_argv_free(cache);
                pmix_argv_free(cachevals);
                pmix_argv_free(xparams);
                pmix_argv_free(xvals);
                return PRTE_ERR_NOT_FOUND;
            }
        }
        while (NULL != (line = prte_schizo_base_getline(fp))) {
            if ('\0' == line[0]) {
                continue; /* skip empty lines */
            }
            opts = pmix_argv_split_with_empty(line, ' ');
            if (NULL == opts) {
                prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                free(line);
                pmix_argv_free(tmp);
                pmix_argv_free(cache);
                pmix_argv_free(cachevals);
                pmix_argv_free(xparams);
                pmix_argv_free(xvals);
                fclose(fp);
                return PRTE_ERR_BAD_PARAM;
            }
            for (n = 0; NULL != opts[n]; n++) {
                if ('\0' == opts[n][0] || '#' == opts[n][0]) {
                    /* the line is only spaces, or a comment, ignore */
                    break;
                }
                if (0 == strcmp(opts[n], "-x")) {
                    /* the next value must be the envar */
                    if (NULL == opts[n + 1]) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        free(line);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    p1 = prte_schizo_base_strip_quotes(opts[n + 1]);
                    /* some idiot decided to allow spaces around an "=" sign, which is
                     * a violation of the Posix cmd line syntax. Rather than fighting
                     * the battle to correct their error, try to accommodate it here */
                    if (NULL != opts[n + 2] && 0 == strcmp(opts[n + 2], "=")) {
                        if (NULL == opts[n + 3]) {
                            prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                           line);
                            free(line);
                            pmix_argv_free(tmp);
                            pmix_argv_free(opts);
                            pmix_argv_free(cache);
                            pmix_argv_free(cachevals);
                            pmix_argv_free(xparams);
                            pmix_argv_free(xvals);
                            fclose(fp);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        p2 = prte_schizo_base_strip_quotes(opts[n + 3]);
                        pmix_asprintf(&param, "%s=%s", p1, p2);
                        free(p1);
                        free(p2);
                        p1 = param;
                        ++n; // need an extra step
                    }
                    rc = process_envar(p1, &xparams, &xvals);
                    free(p1);
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    ++n; // skip over the envar option
                } else if (0 == strcmp(opts[n], "--mca")) {
                    if (NULL == opts[n + 1] || NULL == opts[n + 2]) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        free(line);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    p1 = prte_schizo_base_strip_quotes(opts[n + 1]);
                    p2 = prte_schizo_base_strip_quotes(opts[n + 2]);
                    if (0 == strcmp(p1, "mca_base_env_list")) {
                        /* next option must be the list of envars */
                        rc = process_env_list(p2, &xparams, &xvals, ';');
                    } else {
                        /* treat it as an arbitrary MCA param */
                        rc = check_cache(&cache, &cachevals, p1, p2);
                    }
                    free(p1);
                    free(p2);
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    n += 2; // skip over the MCA option
                } else if (0 == strncmp(opts[n], "mca_base_env_list", strlen("mca_base_env_list"))) {
                    /* find the equal sign */
                    p1 = strchr(opts[n], '=');
                    if (NULL == p1) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        free(line);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    ++p1;
                    rc = process_env_list(p1, &xparams, &xvals, ';');
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                } else {
                    rc = process_token(opts[n], &cache, &cachevals);
                    if (PRTE_SUCCESS != rc) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        fclose(fp);
                        pmix_argv_free(tmp);
                        pmix_argv_free(opts);
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                }
            }
            free(line);
        }
        fclose(fp);
    }

    pmix_argv_free(tmp);

    if (NULL != cache) {
        /* add the results into dstenv */
        for (i = 0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                pmix_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                pmix_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                pmix_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
        pmix_argv_free(cache);
        pmix_argv_free(cachevals);
    }

    /* add the -x values */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            pmix_setenv(xparams[i], xvals[i], true, dstenv);
        }
        pmix_argv_free(xparams);
        pmix_argv_free(xvals);
    }

    return PRTE_SUCCESS;
}

static char *ompi_frameworks[] = {
    /* OPAL frameworks */
    "allocator",
    "backtrace",
    "btl",
    "compress",
    "crs",
    "dl",
    "event",
    "hwloc",
    "if",
    "installdirs",
    "memchecker",
    "memcpy",
    "memory",
    "mpool",
    "patcher",
    "pmix",
    "pstat",
    "rcache",
    "reachable",
    "smsc",
    "shmem",
    "threads",
    "timer",
    /* OMPI frameworks */
    "mpi", /* global options set in runtime/ompi_mpi_params.c */
    "bml",
    "coll",
    "fbtl",
    "fcoll",
    "fs",
    "hook",
    "io",
    "mtl",
    "op",
    "osc",
    "pml",
    "sharedfp",
    "topo",
    "vprotocol",
    /* OSHMEM frameworks */
    "memheap",
    "scoll",
    "spml",
    "sshmem",
    NULL,
};

static bool check_generic(char *p1)
{
    int j;

    /* this is a generic MCA designation, so see if the parameter it
     * refers to belongs to a project base or one of our frameworks */
    if (0 == strncmp("opal_", p1, strlen("opal_")) ||
        0 == strncmp("ompi_", p1, strlen("ompi_"))) {
        return true;
    } else if (0 == strcmp(p1, "mca_base_env_list")) {
        return true;
    } else {
        for (j = 0; NULL != ompi_frameworks[j]; j++) {
            if (0 == strncmp(p1, ompi_frameworks[j], strlen(ompi_frameworks[j]))) {
                return true;
            }
        }
    }

    return false;
}

static int parse_env(char **srcenv, char ***dstenv,
                     prte_cli_result_t *results)
{
    char *p1, *p2, *p3;
    char *env_set_flag;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;
    char **envlist = NULL, **envtgt = NULL;
    prte_cli_item_t *opt;
    int i, j, rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo input */
    if (NULL == results) {
        return PRTE_SUCCESS;
    }

    /* Begin by examining the environment as the cmd line trumps all */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (NULL != env_set_flag) {
        rc = process_env_list(env_set_flag, &xparams, &xvals, ';');
        if (PRTE_SUCCESS != rc) {
            pmix_argv_free(xparams);
            pmix_argv_free(xvals);
            return rc;
        }
    }
    /* process the resulting cache into the dstenv */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            pmix_setenv(xparams[i], xvals[i], true, dstenv);
        }
        pmix_argv_free(xparams);
        xparams = NULL;
        pmix_argv_free(xvals);
        xvals = NULL;
    }

    /* now process any tune file specification - the tune file processor
     * will police itself for duplicate values */
    if (NULL != (opt = prte_cmd_line_get_param(results, "tune"))) {
        p1 = pmix_argv_join(opt->values, ',');
        rc = process_tune_files(p1, dstenv, ',');
        free(p1);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    if (NULL != (opt = prte_cmd_line_get_param(results, "initial-errhandler"))) {
        rc = check_cache(&cache, &cachevals, "mpi_initial_errhandler", opt->values[0]);
        if (PRTE_SUCCESS != rc) {
            pmix_argv_free(cache);
            pmix_argv_free(cachevals);
            return rc;
        }
    }

    if (prte_cmd_line_is_taken(results, "display-comm") &&
        prte_cmd_line_is_taken(results, "display-comm-finalize")) {
        pmix_setenv("OMPI_MCA_ompi_display_comm", "mpi_init,mpi_finalize", true, dstenv);
    } else if (prte_cmd_line_is_taken(results, "display-comm")) {
        pmix_setenv("OMPI_MCA_ompi_display_comm", "mpi_init", true, dstenv);
    } else if (prte_cmd_line_is_taken(results, "display-comm-finalize")) {
        pmix_setenv("OMPI_MCA_ompi_display_comm", "mpi_finalize", true, dstenv);
    }

    /* now look for any "--mca" options - note that it is an error
     * for the same MCA param to be given more than once if the
     * values differ */
    if (NULL != (opt = prte_cmd_line_get_param(results, "omca"))) {
        for (i = 0; NULL != opt->values[i]; ++i) {
            /* the value is provided in "param=value" format, so
             * we need to split it here - it is okay to change
             * the value as we won't be using it again */
            p3 = strchr(opt->values[i], '=');
            *p3 = '\0';
            ++p3;
            p1 = opt->values[i];
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                pmix_argv_append_nosize(&envlist, p3);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p3);
            if (PRTE_SUCCESS != rc) {
                pmix_argv_free(cache);
                pmix_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (NULL != (opt = prte_cmd_line_get_param(results, "gomca"))) {
        for (i = 0; NULL != opt->values[i]; ++i) {
            /* the value is provided in "param=value" format, so
             * we need to split it here - it is okay to change
             * the value as we won't be using it again */
            p3 = strchr(opt->values[i], '=');
            *p3 = '\0';
            ++p3;
            p1 = opt->values[i];
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                pmix_argv_append_nosize(&envlist, p3);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p3);
            if (PRTE_SUCCESS != rc) {
                pmix_argv_free(cache);
                pmix_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (NULL != (opt = prte_cmd_line_get_param(results, "mca"))) {
        for (i = 0; NULL != opt->values[i]; ++i) {
            /* the value is provided in "param=value" format, so
             * we need to split it here - it is okay to change
             * the value as we won't be using it again */
            p3 = strchr(opt->values[i], '=');
            *p3 = '\0';
            ++p3;
            p1 = opt->values[i];
            /* check if this is one of ours */
            if (check_generic(p1)) {
                /* treat mca_base_env_list as a special case */
                if (0 == strcmp(p1, "mca_base_env_list")) {
                    pmix_argv_append_nosize(&envlist, p3);
                    continue;
                }
                rc = check_cache(&cache, &cachevals, p1, p3);
                if (PRTE_SUCCESS != rc) {
                    pmix_argv_free(cache);
                    pmix_argv_free(cachevals);
                    pmix_argv_free(envlist);
                    return rc;
                }
            }
        }
    }
    if (NULL != (opt = prte_cmd_line_get_param(results, "gmca"))) {
        for (i = 0; NULL != opt->values[i]; ++i) {
            /* the value is provided in "param=value" format, so
             * we need to split it here - it is okay to change
             * the value as we won't be using it again */
            p3 = strchr(opt->values[i], '=');
            *p3 = '\0';
            ++p3;
            p1 = opt->values[i];
            /* check if this is one of ours */
            if (check_generic(p1)) {
                /* treat mca_base_env_list as a special case */
                if (0 == strcmp(p1, "mca_base_env_list")) {
                    pmix_argv_append_nosize(&envlist, p3);
                    continue;
                }
                rc = check_cache(&cache, &cachevals, p1, p3);
                if (PRTE_SUCCESS != rc) {
                    pmix_argv_free(cache);
                    pmix_argv_free(cachevals);
                    pmix_argv_free(envlist);
                    return rc;
                }
            }
        }
    }

    /* if we got any env lists, process them here */
    if (NULL != envlist) {
        for (i = 0; NULL != envlist[i]; i++) {
            envtgt = pmix_argv_split(envlist[i], ';');
            for (j = 0; NULL != envtgt[j]; j++) {
                if (NULL == (p2 = strchr(envtgt[j], '='))) {
                    p1 = getenv(envtgt[j]);
                    if (NULL == p1) {
                        continue;
                    }
                    p1 = strdup(p1);
                    if (NULL != (p2 = strchr(p1, '='))) {
                        *p2 = '\0';
                        rc = check_cache(&xparams, &xvals, p1, p2 + 1);
                    } else {
                        rc = check_cache(&xparams, &xvals, envtgt[j], p1);
                    }
                    free(p1);
                    if (PRTE_SUCCESS != rc) {
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(envtgt);
                        pmix_argv_free(envlist);
                        return rc;
                    }
                } else {
                    *p2 = '\0';
                    rc = check_cache(&xparams, &xvals, envtgt[j], p2 + 1);
                    if (PRTE_SUCCESS != rc) {
                        pmix_argv_free(cache);
                        pmix_argv_free(cachevals);
                        pmix_argv_free(envtgt);
                        pmix_argv_free(envlist);
                        return rc;
                    }
                }
            }
            pmix_argv_free(envtgt);
        }
    }
    pmix_argv_free(envlist);

    /* now look for -x options - not allowed to conflict with a -mca option */
    if (NULL != (opt = prte_cmd_line_get_param(results, "x"))) {
        for (i = 0; NULL != opt->values[i]; ++i) {
            /* the value is the envar */
            p1 = opt->values[i];
            /* if there is an '=' in it, then they are setting a value */
            if (NULL != (p2 = strchr(p1, '='))) {
                *p2 = '\0';
                ++p2;
            } else {
                p2 = getenv(p1);
                if (NULL == p2) {
                    continue;
                }
            }
            /* not allowed to duplicate anything from an MCA param on the cmd line */
            rc = check_cache_noadd(&cache, &cachevals, p1, p2);
            if (PRTE_SUCCESS != rc) {
                pmix_argv_free(cache);
                pmix_argv_free(cachevals);
                pmix_argv_free(xparams);
                pmix_argv_free(xvals);
                return rc;
            }
            /* cache this for later inclusion */
            pmix_argv_append_nosize(&xparams, p1);
            pmix_argv_append_nosize(&xvals, p2);
        }
    }

    /* process the resulting cache into the dstenv */
    if (NULL != cache) {
        for (i = 0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                pmix_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                pmix_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                pmix_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
    }
    pmix_argv_free(cache);
    pmix_argv_free(cachevals);

    /* add the -x values */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            pmix_setenv(xparams[i], xvals[i], true, dstenv);
        }
        pmix_argv_free(xparams);
        pmix_argv_free(xvals);
    }

    return PRTE_SUCCESS;
}

static bool check_prte_overlap(char *var, char *value)
{
    char *tmp;

    if (0 == strncmp(var, "dl_", 3)) {
        pmix_asprintf(&tmp, "PRTE_MCA_prtedl_%s", &var[3]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "oob_", 4)) {
        pmix_asprintf(&tmp, "PRTE_MCA_%s", var);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "hwloc_", 6)) {
        pmix_asprintf(&tmp, "PRTE_MCA_%s", var);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "if_", 3)) {
        // need to convert if to prteif
        pmix_asprintf(&tmp, "PRTE_MCA_prteif_%s", &var[3]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "reachable_", strlen("reachable_"))) {
        // need to convert reachable to prtereachable
        pmix_asprintf(&tmp, "PRTE_MCA_prtereachable_%s", &var[strlen("reachable")]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    }
    return false;
}


static bool check_pmix_overlap(char *var, char *value)
{
    char *tmp;

    if (0 == strncmp(var, "dl_", 3)) {
        pmix_asprintf(&tmp, "PMIX_MCA_pdl_%s", &var[3]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "oob_", 4)) {
        pmix_asprintf(&tmp, "PMIX_MCA_ptl_%s", &var[4]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "hwloc_", 6)) {
        pmix_asprintf(&tmp, "PMIX_MCA_%s", var);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "if_", 3)) {
        // need to convert if to pif
        pmix_asprintf(&tmp, "PMIX_MCA_pif_%s", &var[3]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    } else if (0 == strncmp(var, "reachable_", strlen("reachable_"))) {
        // need to convert reachable to preachable
        pmix_asprintf(&tmp, "PMIX_MCA_preachable_%s", &var[strlen("reachable")]);
        // set it, but don't overwrite if they already
        // have a value in our environment
        setenv(tmp, value, false);
        free(tmp);
        return true;
    }
    return false;
}

static int setup_fork(prte_job_t *jdata, prte_app_context_t *app)
{
    prte_attribute_t *attr;
    bool exists;
    char *param, *p2, *saveptr;
    int i;

    /* flag that we started this job */
    pmix_setenv("PRTE_LAUNCHED", "1", true, &app->env);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PRTE_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t)
    {
        if (PRTE_JOB_SET_ENVAR == attr->key) {
            pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_JOB_ADD_ENVAR == attr->key) {
            pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_JOB_UNSET_ENVAR == attr->key) {
            pmix_unsetenv(attr->data.data.string, &app->env);
        } else if (PRTE_JOB_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    pmix_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        } else if (PRTE_JOB_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    pmix_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PRTE_LIST_FOREACH(attr, &app->attributes, prte_attribute_t)
    {
        if (PRTE_APP_SET_ENVAR == attr->key) {
            pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_APP_ADD_ENVAR == attr->key) {
            pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_APP_UNSET_ENVAR == attr->key) {
            pmix_unsetenv(attr->data.data.string, &app->env);
        } else if (PRTE_APP_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    pmix_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        } else if (PRTE_APP_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i = 0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '='); // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param; // move past where the '=' sign was
                    pmix_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    pmix_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                pmix_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        }
    }

    return PRTE_SUCCESS;
}


static int detect_proxy(char *personalities)
{
    char *evar, *tmp, *e2;
    char *file;
    const char *home;
    prte_list_t params;
    prte_mca_base_var_file_value_t *fv;
    uid_t uid;
    int n, len;

    prte_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__,
                        (NULL == personalities) ? "NULL" : personalities,
                        prte_tool_basename);

    /* COMMAND-LINE OVERRIDES ALL */
    if (NULL != personalities) {
        /* this is a list of personalities we need to check -
         * if it contains "ompi", then we are available */
        if (NULL != strstr(personalities, "ompi")) {
            goto weareit;
        }
        return 0;
    }

    /* if we were told the proxy, then use it */
    if (NULL != (evar = getenv("PRTE_MCA_schizo_proxy"))) {
        if (0 == strcmp(evar, "ompi")) {
            goto weareit;
        } else {
            return 0;
        }
    }

    /* if neither of those were true, then it cannot be us */
    return 0;

weareit:
    /* since we are the proxy, we need to check the OMPI default
     * MCA params to see if there is something relating to PRRTE
     * in them - this would be "old" references to things from
     * ORTE, as well as a few OPAL references that also impact us
     *
     * NOTE: we do this in the following precedence order. Note
     * that we do not overwrite at any step - this is so that we
     * don't overwrite something previously set by the user. So
     * the order to execution is the opposite of the intended
     * precedence order.
     *
     * 1. check the environmental paramaters for OMPI_MCA values
     *    that need to be translated
     *
     * 2. the user's home directory file as it should
     *    overwrite the system default file, but not the
     *    envars
     *
     * 3. the system default parameter file
     */
    len = strlen("OMPI_MCA_");
    for (n=0; NULL != environ[n]; n++) {
        if (0 == strncmp(environ[n], "OMPI_MCA_", len)) {
            e2 = strdup(environ[n]);
            evar = strrchr(e2, '=');
            *evar = '\0';
            ++evar;
            if (check_prte_overlap(&e2[len], evar)) {
                // check for pmix overlap
                check_pmix_overlap(&e2[len], evar);
            } else if (prte_schizo_base_check_prte_param(&e2[len])) {
                    pmix_asprintf(&tmp, "PRTE_MCA_%s", &e2[len]);
                    // set it, but don't overwrite if they already
                    // have a value in our environment
                    setenv(tmp, evar, false);
                    free(tmp);
                    // check for pmix overlap
                    check_pmix_overlap(&e2[len], evar);
            } else if (prte_schizo_base_check_pmix_param(&e2[len])) {
                pmix_asprintf(&tmp, "PMIX_MCA_%s", &e2[len]);
                // set it, but don't overwrite if they already
                // have a value in our environment
                setenv(tmp, evar, false);
                free(tmp);
            }
            free(e2);
        }
    }

    /* see if the user has a default MCA param file */
    uid = geteuid();

    /* try to get their home directory */
    home = pmix_home_directory(uid);
    if (NULL != home) {
        file = pmix_os_path(false, home, ".openmpi", "mca-params.conf", NULL);
        PRTE_CONSTRUCT(&params, prte_list_t);
        prte_mca_base_parse_paramfile(file, &params);
        free(file);
        PRTE_LIST_FOREACH (fv, &params, prte_mca_base_var_file_value_t) {
            // see if this param relates to PRRTE
            if (check_prte_overlap(fv->mbvfv_var, fv->mbvfv_value)) {
                check_pmix_overlap(fv->mbvfv_var, fv->mbvfv_value);
            } else if (prte_schizo_base_check_prte_param(fv->mbvfv_var)) {
                pmix_asprintf(&tmp, "PRTE_MCA_%s", fv->mbvfv_var);
                // set it, but don't overwrite if they already
                // have a value in our environment
                setenv(tmp, fv->mbvfv_value, false);
                free(tmp);
                // if this relates to the DL, OOB, HWLOC, IF, or
                // REACHABLE frameworks, then we also need to set
                // the equivalent PMIx value
                check_pmix_overlap(fv->mbvfv_var, fv->mbvfv_value);
            } else if (prte_schizo_base_check_pmix_param(fv->mbvfv_var)) {
                pmix_asprintf(&tmp, "PMIX_MCA_%s", fv->mbvfv_var);
                // set it, but don't overwrite if they already
                // have a value in our environment
                setenv(tmp, fv->mbvfv_value, false);
                free(tmp);
            }
        }
        PRTE_LIST_DESTRUCT(&params);
    }

    /* check if the user has set OMPIHOME in their environment */
    if (NULL != (evar = getenv("OMPIHOME"))) {
        /* look for the default MCA param file */
        file = pmix_os_path(false, evar, "etc", "openmpi-mca-params.conf", NULL);
        PRTE_CONSTRUCT(&params, prte_list_t);
        prte_mca_base_parse_paramfile(file, &params);
        free(file);
        PRTE_LIST_FOREACH (fv, &params, prte_mca_base_var_file_value_t) {
            // see if this param relates to PRRTE
            if (check_prte_overlap(fv->mbvfv_var, fv->mbvfv_value)) {
                check_pmix_overlap(fv->mbvfv_var, fv->mbvfv_value);
            } else if (prte_schizo_base_check_prte_param(fv->mbvfv_var)) {
                pmix_asprintf(&tmp, "PRTE_MCA_%s", fv->mbvfv_var);
                // set it, but don't overwrite if they already
                // have a value in our environment
                setenv(tmp, fv->mbvfv_value, false);
                free(tmp);
                // if this relates to the DL, OOB, HWLOC, IF, or
                // REACHABLE frameworks, then we also need to set
                // the equivalent PMIx value
                check_pmix_overlap(fv->mbvfv_var, fv->mbvfv_value);
            }
        }
        PRTE_LIST_DESTRUCT(&params);
    }

    return 100;
}

static void allow_run_as_root(prte_cli_result_t *results)
{
    /* we always run last */
    char *r1, *r2;

    if (prte_cmd_line_is_taken(results, "allow-run-as-root")) {
        return;
    }

    if (NULL != (r1 = getenv("OMPI_ALLOW_RUN_AS_ROOT"))
        && NULL != (r2 = getenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return;
        }
    }

    prte_schizo_base_root_error_msg();
}

static int set_default_ranking(prte_job_t *jdata,
                               prte_schizo_options_t *options)
{
    int rc;
    prte_mapping_policy_t map;

    /* use the base system and then we will correct it */
    rc = prte_rmaps_base_set_default_ranking(jdata, options);
    if (PRTE_SUCCESS != rc) {
        // it will have output the error message
        return rc;
    }
    // correct how we handle PPR
    if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        map = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
        // set for dense packing - but don't override any user setting
        if (PRTE_MAPPING_PPR == map && !PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
            PRTE_SET_RANKING_POLICY(jdata->map->ranking, PRTE_RANK_BY_SLOT);
        }
    }
    return PRTE_SUCCESS;
}

static void job_info(prte_cli_result_t *results,
                     void *jobinfo)
{
    prte_cli_item_t *opt;
    uint16_t u16;
    pmix_status_t rc;

    if (NULL != (opt = prte_cmd_line_get_param(results, "stream-buffering"))) {
        u16 = strtol(opt->values[0], NULL, 10);
        if (0 != u16 && 1 != u16 && 2 != u16) {
            /* bad value */
            prte_show_help("help-schizo-base.txt", "bad-stream-buffering-value", true, u16);
            return;
        }
        PMIX_INFO_LIST_ADD(rc, jobinfo, "OMPI_STREAM_BUFFERING", &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
}
