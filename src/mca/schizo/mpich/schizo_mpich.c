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

#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif

#include "src/util/pmix_argv.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/prte_globals.h"

#include "schizo_mpich.h"
#include "src/mca/schizo/base/base.h"

static int parse_cli(char **argv, pmix_cli_result_t *results, bool silent);
static int detect_proxy(char *argv);
static int parse_env(char **srcenv, char ***dstenv, pmix_cli_result_t *cli);
static void allow_run_as_root(pmix_cli_result_t *results);
static void job_info(pmix_cli_result_t *results,
                     void *jobinfo);
static int set_default_rto(prte_job_t *jdata,
                           prte_rmaps_options_t *options);
static int check_sanity(pmix_cli_result_t *cmd_line);

prte_schizo_base_module_t prte_schizo_mpich_module = {
    .name = "mpich",
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = prte_schizo_base_setup_fork,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info,
    .set_default_rto = set_default_rto,
    .check_sanity = check_sanity
};

static struct option myoptions[] = {
    /* basic options */
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),

    // MCA parameters
    PMIX_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_TUNE, PMIX_ARG_REQD),

    // DVM options
    PMIX_OPTION_DEFINE(PRTE_CLI_DAEMONIZE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_SET_SID, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_PID, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_URI, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_TEST_SUICIDE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_DEFAULT_HOSTFILE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_KEEPALIVE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_LAUNCH_AGENT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_MAX_VM_SIZE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_DEBUG, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS_FILE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_LEAVE_SESSION_ATTACHED, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_TMPDIR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PREFIX, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_NOPREFIX, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_FWD_SIGNALS, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PERSONALITY, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_RUN_AS_ROOT, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_CHILD_SEP, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_DVM, PMIX_ARG_REQD),

    // Launch options
    PMIX_OPTION_DEFINE(PRTE_CLI_TIMEOUT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_REPORT_STATE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_STACK_TRACES, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_SPAWN_TIMEOUT, PMIX_ARG_REQD),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PMIX_ARG_REQD, 'n'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PMIX_ARG_REQD, 'c'),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_NPERNODE, PMIX_ARG_REQD, 'N'),
    PMIX_OPTION_DEFINE(PRTE_CLI_APPFILE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_XTERM, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_STOP_ON_EXEC, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_STOP_IN_INIT, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_STOP_IN_APP, PMIX_ARG_NONE),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_FWD_ENVAR, PMIX_ARG_REQD, 'x'),
    PMIX_OPTION_DEFINE(PRTE_CLI_WDIR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("wd", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_SET_CWD_SESSION, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_PATH, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PSET, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("machinefile", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_ADDHOSTFILE, PMIX_ARG_REQD),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PMIX_ARG_REQD, 'H'),
    PMIX_OPTION_DEFINE(PRTE_CLI_ADDHOST, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PRTE_CLI_PRELOAD_FILES, PMIX_ARG_REQD),
    PMIX_OPTION_SHORT_DEFINE(PRTE_CLI_PRELOAD_BIN, PMIX_ARG_NONE, 's'),
    PMIX_OPTION_DEFINE(PRTE_CLI_DO_NOT_AGG_HELP, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_FWD_ENVIRON, PMIX_ARG_OPTIONAL),

    // output options
    PMIX_OPTION_DEFINE(PRTE_CLI_OUTPUT, PMIX_ARG_REQD),

    // input options
    PMIX_OPTION_DEFINE(PRTE_CLI_STDIN, PMIX_ARG_REQD),

    /* Mapping options */
    PMIX_OPTION_DEFINE(PRTE_CLI_MAPBY, PMIX_ARG_REQD),

    /* Ranking options */
    PMIX_OPTION_DEFINE(PRTE_CLI_RANKBY, PMIX_ARG_REQD),

    /* Binding options */
    PMIX_OPTION_DEFINE(PRTE_CLI_BINDTO, PMIX_ARG_REQD),

    /* Runtime options */
    PMIX_OPTION_DEFINE(PRTE_CLI_RTOS, PMIX_ARG_REQD),

    /* display options */
    PMIX_OPTION_DEFINE(PRTE_CLI_DISPLAY, PMIX_ARG_REQD),

    // deprecated options
    PMIX_OPTION_DEFINE("mca", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("xml", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("tag-output", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("timestamp-output", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("output-directory", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("output-filename", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("merge-stderr-to-stdout", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("display-devel-map", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("display-topo", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("report-bindings", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("display-devel-allocation", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("display-map", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("display-allocation", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("rankfile", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("nolocal", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("oversubscribe", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("nooversubscribe", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("use-hwthread-cpus", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("cpu-set", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("cpu-list", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("bind-to-core", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("bynode", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("bycore", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("byslot", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("cpus-per-proc", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("cpus-per-rank", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("npernode", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("pernode", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("npersocket", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("ppr", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("debug", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE("do-not-launch", PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PRTE_CLI_OUTPUT_PROCTABLE, PMIX_ARG_OPTIONAL),

    PMIX_OPTION_END
};
static char *myshorts = "h::vVpn:c:N:sH:x:";

#if 0
static options_t opthelp[] = {
    {"--bind-to",
        "-bind-to: Process-core binding type to use\n\n"
        "     Binding type options:\n"
        "        Default:\n"
        "            none             -- no binding (default)\n\n"
        "        Architecture unaware options:\n"
        "            rr               -- round-robin as OS assigned processor IDs\n"
        "            user:0+2,1+4,3,2 -- user specified binding\n\n"
        "        Architecture aware options (part within the {} braces are optional):\n"
        "            machine          -- bind to machine\n"
        "            numa{:<n>}       -- bind to 'n' numa domains\n"
        "            socket{:<n>}     -- bind to 'n' sockets\n"
        "            core{:<n>}       -- bind to 'n' cores\n"
        "            hwthread{:<n>}   -- bind to 'n' hardware threads\n"
        "            l1cache{:<n>}    -- bind to 'n' L1 cache domains\n"
        "            l2cache{:<n>}    -- bind to 'n' L2 cache domains\n"
        "            l3cache{:<n>}    -- bind to 'n' L3 cache domain\n"
        "            l4cache{:<n>}    -- bind to 'n' L4 cache domain\n"
        "            l5cache{:<n>}    -- bind to 'n' L5 cache domain\n"
    },
    {"--map-by",
        "-map-by: Order of bind mapping to use\n\n"
        "    Options (T: hwthread; C: core; S: socket; N: NUMA domain; B: motherboard):\n"
        "        Default: <same option as binding>\n\n"
        "        Architecture aware options (part within the {} braces are optional):\n"
        "            machine          -- map by machine\n"
        "            numa{:<n>}       -- map by 'n' numa domains\n"
        "            socket{:<n>}     -- map by 'n' sockets\n"
        "            core{:<n>}       -- map by 'n' cores\n"
        "            hwthread{:<n>}   -- map by 'n' hardware threads\n"
        "            l1cache{:<n>}    -- map by 'n' L1 cache domains\n"
        "            l2cache{:<n>}    -- map by 'n' L2 cache domains\n"
        "            l3cache{:<n>}    -- map by 'n' L3 cache domain\n"
        "            l4cache{:<n>}    -- map by 'n' L4 cache domain\n"
        "            l5cache{:<n>}    -- map by 'n' L5 cache domain\n"
    },
    {NULL, NULL}
};

static char *genhelp =
    "\nGlobal options (passed to all executables):\n"
    "\n  Global environment options:\n"
    "    -genv {name} {value}             environment variable name and value\n"
    "    -genvlist {env1,env2,...}        environment variable list to pass\n"
    "    -genvnone                        do not pass any environment variables\n"
    "    -genvall                         pass all environment variables not managed\n"
    "                                          by the launcher (default)\n"
    "\n  Other global options:\n"
    "    -f {name}                        file containing the host names\n"
    "    -hosts {host list}               comma separated host list\n"
    "    -wdir {dirname}                  working directory to use\n"
    "    -configfile {name}               config file containing MPMD launch options\n"
    "\n\nLocal options (passed to individual executables):\n"
    "\n      Local environment options:\n"
    "    -env {name} {value}              environment variable name and value\n"
    "    -envlist {env1,env2,...}         environment variable list to pass\n"
    "    -envnone                         do not pass any environment variables\n"
    "    -envall                          pass all environment variables (default)\n"
    "\n  Other local options:\n"
    "    -n/-np {value}                   number of processes\n"
    "    {exec_name} {args}               executable name and arguments\n"
    "\n\nMpich specific options (treated as global):\n"
    "\n  Launch options:\n"
    "    -launcher                        launcher to use (ssh rsh fork slurm ll lsf sge manual persist)\n"
    "    -launcher-exec                   executable to use to launch processes\n"
    "    -enable-x/-disable-x             enable or disable X forwarding\n"
    "\n  Resource management kernel options:\n"
    "    -rmk                             resource management kernel to use (user slurm ll lsf sge pbs cobalt)\n"
    "\n  Processor topology options:\n"
    "    -topolib                         processor topology library (hwloc)\n"
    "    -bind-to                         process binding\n"
    "    -map-by                          process mapping\n"
    "    -membind                         memory binding policy\n"
    "\n  Demux engine options:\n"
    "    -demux                           demux engine (poll select)\n"
    "\n  Other Mpich options:\n"
    "    -verbose                         verbose mode\n"
    "    -info                            build information\n"
    "    -print-all-exitcodes             print exit codes of all processes\n"
    "    -iface                           network interface to use\n"
    "    -ppn                             processes per node\n"
    "    -profile                         turn on internal profiling\n"
    "    -prepend-rank                    prepend rank to output\n"
    "    -prepend-pattern                 prepend pattern to output\n"
    "    -outfile-pattern                 direct stdout to file\n"
    "    -errfile-pattern                 direct stderr to file\n"
    "    -nameserver                      name server information (host:port format)\n"
    "    -disable-auto-cleanup            don't cleanup processes on error\n"
    "    -disable-hostname-propagation    let MPICH auto-detect the hostname\n"
    "    -order-nodes                     order nodes as ascending/descending cores\n"
    "    -localhost                       local hostname for the launching node\n"
    "    -usize                           universe size (SYSTEM, INFINITE, <value>)\n"
    "    -pmi-port                        use the PMI_PORT model\n"
    "    -skip-launch-node                do not run MPI processes on the launch node\n"
    "    -gpus-per-proc                   number of GPUs per process (default: auto)\n"
;

static void check_and_replace(char **argv, int idx,
                              char *replacement)
{
    char *tmp = NULL, *ptr;

    if (NULL != (ptr = strchr(argv[idx], ':'))) {
        tmp = strdup(ptr);
    }
    free(argv[idx]);
    if (NULL == tmp) {
        argv[idx] = strdup(replacement);
    } else {
        pmix_asprintf(&argv[idx], "%s%s", replacement, tmp);
        free(tmp);
    }
}
#endif

static int convert_deprecated_cli(pmix_cli_result_t *results,
                                  bool silent)
{
    PRTE_HIDE_UNUSED_PARAMS(results, silent);
#if 0

    char **pargs, *p2;
    int rc = PRTE_SUCCESS;

    pargs = *argv;

    /* -prepend-rank  ->  "--output rank */
    if (0 == strcmp(option, "--prepend-rank")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--output", NULL, "rank", false);
        rc = PRTE_ERR_SILENT;
        return rc;
    }

    /* --skip-launch-node -> --map-by :nolocal */
    if (0 == strcmp(option, "--skip-launch-node")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "NOLOCAL", true);
        return rc;
    }

    if (0 == strcmp(option, "--map-by")) {
        /* need to perform the following mappings:
         *  - "machine"  => "slot"
         *  - "socket"   => "package"
         */
        if (NULL != strcasestr(pargs[i+1], "machine")) {
            check_and_replace(pargs, i+1, "slot");
        } else if (NULL != strcasestr(pargs[i+1], "socket")) {
            check_and_replace(pargs, i+1, "package");
        } else if ('T' == pargs[i+1][0]) {
            /* shorthand for "hwthread" */
            check_and_replace(pargs, i+1, "hwthread");
        } else if ('C' == pargs[i+1][0]) {
            /* shorthand for "core" */
            check_and_replace(pargs, i+1, "core");
        } else if ('S' == pargs[i+1][0]) {
            /* shorthand for "socket" */
            check_and_replace(pargs, i+1, "package");
        } else if ('N' == pargs[i+1][0]) {
            /* shorthand for "numa" */
            check_and_replace(pargs, i+1, "numa");
        } else if ('B' == pargs[i+1][0]) {
            /* shorthand for "motherboard" */
            check_and_replace(pargs, i+1, "slot");
        }
        return PRTE_OPERATION_SUCCEEDED;
    }

    if (0 == strcmp(option, "--bind-to")) {
        /* need to perform the following mappings:
         *  - "rr"       => "core"
         *  - "machine"  => "none"
         *  - "socket"   => "package"
         */
        if (NULL != strcasestr(pargs[i+1], "rr")) {
            check_and_replace(pargs, i+1, "core");
        } else if (NULL != strcasestr(pargs[i+1], "machine")) {
            check_and_replace(pargs, i+1, "none");
        } else if (NULL != strcasestr(pargs[i+1], "socket")) {
            check_and_replace(pargs, i+1, "package");
        } else if ('T' == pargs[i+1][0]) {
            /* shorthand for "hwthread" */
            check_and_replace(pargs, i+1, "hwthread");
        } else if ('C' == pargs[i+1][0]) {
            /* shorthand for "core" */
            check_and_replace(pargs, i+1, "core");
        } else if ('S' == pargs[i+1][0]) {
            /* shorthand for "socket" */
            check_and_replace(pargs, i+1, "package");
        } else if ('N' == pargs[i+1][0]) {
            /* shorthand for "numa" */
            check_and_replace(pargs, i+1, "numa");
        } else if ('B' == pargs[i+1][0]) {
            /* shorthand for "motherboard" */
            check_and_replace(pargs, i+1, "slot");
        }
        return PRTE_OPERATION_SUCCEEDED;
    }

    /* --outfile-pattern -> --output file= */
    if (0 == strcmp(option, "--outfile-pattern")) {
        pmix_asprintf(&p2, "file=%s:pattern", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--output", NULL, p2, false);
        return PRTE_ERR_SILENT;
    }

    return rc;
#endif
    return PRTE_SUCCESS;
}

#if 0
static int parse_deprecated_cli(pmix_cli_result_t *results,
                                bool silent)
{
    PRTE_HIDE_UNUSED_PARAMS(results, silent);
    pmix_status_t rc;

    char *options[] = {"--genv",
                       "--genvlist",
                       "--genvnone",
                       "--genvall",
                       "--f",
                       "--hosts",
                       "--wdir",
                       "--env",
                       "--envlist",
                       "--envnone",
                       "--envall",
                       "--launcher",
                       "--launcher-exec",
                       "--enable-x",
                       "--disable-x",
                       "--rmk",
                       "--topolib",
                       "--bind-to",
                       "--map-by",
                       "--membend",
                       "--demux",
                       "--verbose",
                       "--info",
                       "--print-all-exitcodes",
                       "--iface",
                       "--ppn",
                       "--profile",
                       "--prepend-rank",
                       "--prepend-pattern",
                       "--outfile-pattern",
                       "--errfile-pattern",
                       "--nameserver",
                       "--disable-auto-cleanup",
                       "--disable-hostname-propagation",
                       "--order-nodes",
                       "--localhost",
                       "--usize",
                       "--pmi-port",
                       "--skip-launch-node",
                       "--gpus-per-proc",
                       NULL};

    rc = prte_schizo_base_process_deprecated_cli(cmdline, argc, argv, options,
                                                 true, convert_deprecated_cli);

    return rc;
    return PRTE_SUCCESS;
}
#endif

static int parse_cli(char **argv, pmix_cli_result_t *results,
                     bool silent)
{
    int rc;

    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:mpich: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    rc = pmix_cmd_line_parse(argv, myshorts, myoptions, NULL,
                             results, "help-schizo-mpich.txt");
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

    /* check for deprecated options - warn and convert them */
    rc = convert_deprecated_cli(results, silent);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

#if 0
    for (i = 0; i < (argc - start); i++) {
         if (0 == strcmp("--map-by", argv[i])) {
            /* if they set "inherit", then make this the default for prte */
            if (NULL != strcasestr(argv[i + 1], "inherit")
                && NULL == strcasestr(argv[i + 1], "noinherit")) {
                if (NULL == target) {
                    /* push it into our environment */
                    PMIX_SETENV_COMPAT("PRTE_MCA_rmaps_default_inherit", "1", true, &environ);
                    PMIX_SETENV_COMPAT("PRTE_MCA_rmaps_default_mapping_policy", argv[i + 1], true,
                                &environ);
                } else {
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, "--prtemca");
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, "rmaps_default_inherit");
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, "1");
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, "--prtemca");
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, "rmaps_default_mapping_policy");
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(target, argv[i + 1]);
                }
            }
             break;
        }
    }
#endif
    return PRTE_SUCCESS;
}

static int parse_env(char **srcenv, char ***dstenv,
                     pmix_cli_result_t *cli)
{
    PRTE_HIDE_UNUSED_PARAMS(srcenv, dstenv, cli);
#if 0
    char *p1, *p2;
    prte_value_t *pval;
    int i, j;

    pmix_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:mpich: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if they are filling out a cmd line, then we don't
     * have anything to contribute */
    if (cmdline) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (0 < (j = pmix_cmd_line_get_ninsts(cmd_line, "genv"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = pmix_cmd_line_get_param(cmd_line, "genv", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = pmix_cmd_line_get_param(cmd_line, "genv", i, 1);
            p2 = prte_schizo_base_strip_quotes(pval->value.data.string);
            PMIX_SETENV_COMPAT(p1, p2, true, dstenv);
            free(p1);
            free(p2);
        }
    }
#endif
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

    /* COMMAND-LINE OVERRRIDES ALL */
    /* this is a list of personalities we need to check -
     * if it contains "mpich" or "mpich", then we are available */
    if (NULL != personalities) {
        if (NULL != strstr(personalities, "mpich") ||
            NULL != strstr(personalities, "mpich")) {
            return 100;
        }
        return 0;
    }

    /* if we were told the proxy, then use it */
    if (NULL != (evar = getenv("PRTE_MCA_schizo_proxy"))) {
        if (0 == strcmp(evar, "mpich") ||
            0 == strcmp(evar, "mpich")) {
            return 100;
        } else {
            return 0;
        }
    }

    /* if neither of those were true, then it cannot be us */
    return 0;
}

static void allow_run_as_root(pmix_cli_result_t *results)
{
    PRTE_HIDE_UNUSED_PARAMS(results);
    /* mpich always allows run-as-root */
    return;
}

static void job_info(pmix_cli_result_t *results,
                     void *jobinfo)
{
    PRTE_HIDE_UNUSED_PARAMS(results, jobinfo);
    return;
}

static int check_sanity(pmix_cli_result_t *cmd_line)
{
    PRTE_HIDE_UNUSED_PARAMS(cmd_line);
#if 0
    prte_value_t *pval;
    char *mappers[] = {"slot", "hwthread", "core", "l1cache", "l2cache",  "l3cache", "l4cache", "l5cache", "package",
                       "node", "seq",      "dist", "ppr",     "rankfile", NULL};
    char *mapquals[] = {"pe=", "span", "oversubscribe", "nooversubscribe", "nolocal",
                        "hwtcpus", "corecpus", "device=", "inherit", "noinherit", "pe-list=",
                        "file=", "donotlaunch", NULL};

    char *rankers[] = {"slot",    "hwthread", "core", "l1cache", "l2cache",
                       "l3cache", "package",  "node", NULL};
    char *rkquals[] = {"span", "fill", NULL};

    char *binders[] = {"none",    "hwthread", "core",    "l1cache",
                       "l2cache", "l3cache",  "package", NULL};
    char *bndquals[] = {"overload-allowed", "if-supported", "ordered", "report", NULL};

    char *outputs[] = {"tag", "rank", "timestamp", "xml", "merge-stderr-to-stdout", "directory", "filename", NULL};
    char *outquals[] = {"nocopy", "pattern", NULL};

    char *displays[] = {"allocation", "map", "bind", "map-devel", "topo", NULL};

    bool hwtcpus = false;

    if (1 < pmix_cmd_line_get_ninsts(cmd_line, "map-by")) {
        pmix_show_help("help-schizo-base.txt", "multi-instances", true, "map-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, "rank-by")) {
        pmix_show_help("help-schizo-base.txt", "multi-instances", true, "rank-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, "bind-to")) {
        pmix_show_help("help-schizo-base.txt", "multi-instances", true, "bind-to");
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, "output")) {
        pmix_show_help("help-schizo-base.txt", "multi-instances", true, "output");
        return PRTE_ERR_SILENT;
    }
    if (1 < pmix_cmd_line_get_ninsts(cmd_line, "display")) {
        pmix_show_help("help-schizo-base.txt", "multi-instances", true, "display");
        return PRTE_ERR_SILENT;
    }

    /* quick check that we have valid directives */
    if (NULL != (pval = pmix_cmd_line_get_param(cmd_line, "map-by", 0, 0))) {
        if (NULL != strcasestr(pval->value.data.string, "HWTCPUS")) {
            hwtcpus = true;
        }
        if (!prte_schizo_base_check_directives("map-by", mappers, mapquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = pmix_cmd_line_get_param(cmd_line, "rank-by", 0, 0))) {
        if (!prte_schizo_base_check_directives("rank-by", rankers, rkquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = pmix_cmd_line_get_param(cmd_line, "bind-to", 0, 0))) {
        if (!prte_schizo_base_check_directives("bind-to", binders, bndquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
        if (0 == strncasecmp(pval->value.data.string, "HWTHREAD", strlen("HWTHREAD")) && !hwtcpus) {
            /* if we are told to bind-to hwt, then we have to be treating
             * hwt's as the allocatable unit */
            pmix_show_help("help-prte-rmaps-base.txt", "invalid-combination", true);
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = pmix_cmd_line_get_param(cmd_line, "output", 0, 0))) {
        if (!prte_schizo_base_check_directives("output", outputs, outquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = pmix_cmd_line_get_param(cmd_line, "display", 0, 0))) {
        if (!prte_schizo_base_check_directives("display", displays, NULL, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }
#endif
    return PRTE_SUCCESS;
}

static int set_default_rto(prte_job_t *jdata,
                           prte_rmaps_options_t *options)
{
    PRTE_HIDE_UNUSED_PARAMS(options);
    return prte_state_base_set_runtime_options(jdata, NULL);
}
