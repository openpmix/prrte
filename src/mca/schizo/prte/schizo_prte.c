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
#include <getopt.h>


#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "schizo_prte.h"
#include "src/mca/schizo/base/base.h"

static int parse_cli(char **argv, prte_cli_result_t *results, bool silent);
static int detect_proxy(char *argv);
static int parse_env(char **srcenv, char ***dstenv, prte_cli_result_t *cli);
static void allow_run_as_root(prte_cli_result_t *results);
static int setup_fork(prte_job_t *jdata, prte_app_context_t *context);
static void job_info(prte_cli_result_t *results,
                     void *jobinfo);

prte_schizo_base_module_t prte_schizo_prte_module = {
    .name = "prte",
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info
};

static struct option prteoptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSEABLE, PRTE_ARG_NONE, 'p'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSABLE, PRTE_ARG_NONE, 'p'), // synonym for parseable

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TUNE, PRTE_ARG_REQD),

    // DVM options
    PRTE_OPTION_DEFINE(PRTE_CLI_NO_READY_MSG, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DAEMONIZE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SYSTEM_SERVER, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SET_SID, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_PID, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_URI, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TEST_SUICIDE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEFAULT_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SINGLETON, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_KEEPALIVE, PRTE_ARG_NONE),
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

    // Launch options
    PRTE_OPTION_DEFINE(PRTE_CLI_TIMEOUT, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_FWD_ENVAR, PRTE_ARG_REQD, 'x'),
    PRTE_OPTION_DEFINE(PRTE_CLI_SHOW_PROGRESS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PRTE_ARG_REQD, 'H'),
    PRTE_OPTION_DEFINE(PRTE_CLI_EXEC_AGENT, PRTE_ARG_REQD),

    // output options
    PRTE_OPTION_DEFINE(PRTE_CLI_STREAM_BUF, PRTE_ARG_REQD),

    /* developer options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DO_NOT_LAUNCH, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DISPLAY, PRTE_ARG_REQD),

    // deprecated options
    PRTE_OPTION_DEFINE("mca", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("machinefile", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("xml", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-devel-map", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-topo", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("report-bindings", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-devel-allocation", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-map", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("display-allocation", PRTE_ARG_NONE),

    PRTE_OPTION_END
};
static char *prteshorts = "h::vVpx:H:";

static struct option prterunoptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSEABLE, PRTE_ARG_NONE, 'p'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSABLE, PRTE_ARG_NONE, 'p'), // synonym for parseable

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TUNE, PRTE_ARG_REQD),

    // DVM options
    PRTE_OPTION_DEFINE(PRTE_CLI_DAEMONIZE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SYSTEM_SERVER, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SET_SID, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_PID, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_URI, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TEST_SUICIDE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEFAULT_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SINGLETON, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_KEEPALIVE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_LAUNCH_AGENT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_MAX_VM_SIZE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS_FILE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_LEAVE_SESSION_ATTACHED, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_TMPDIR, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PREFIX, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NOPREFIX, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_FWD_SIGNALS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PERSONALITY, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_RUN_AS_ROOT, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_CHILD_SEP, PRTE_ARG_NONE),

    // Launch options
    PRTE_OPTION_DEFINE(PRTE_CLI_TIMEOUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_STATE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STACK_TRACES, PRTE_ARG_NONE),
#ifdef PMIX_SPAWN_TIMEOUT
    PRTE_OPTION_DEFINE(PRTE_CLI_SPAWN_TIMEOUT, PRTE_ARG_REQD),
#endif
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'n'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'c'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NPERNODE, PRTE_ARG_REQD, 'N'),
    PRTE_OPTION_DEFINE(PRTE_CLI_APPFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_XTERM, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_ON_EXEC, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_INIT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_APP, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_FWD_ENVAR, PRTE_ARG_REQD, 'x'),
    PRTE_OPTION_DEFINE(PRTE_CLI_WDIR, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("wd", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SET_CWD_SESSION, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_PATH, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SHOW_PROGRESS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PSET, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("machinefile", PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PRTE_ARG_REQD, 'H'),
    PRTE_OPTION_DEFINE(PRTE_CLI_PRELOAD_FILES, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PRELOAD_BIN, PRTE_ARG_NONE, 's'),

    // output options
    PRTE_OPTION_DEFINE(PRTE_CLI_OUTPUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STREAM_BUF, PRTE_ARG_REQD),

    // input options
    PRTE_OPTION_DEFINE(PRTE_CLI_STDIN, PRTE_ARG_REQD),

    /* Mapping options */
    PRTE_OPTION_DEFINE(PRTE_CLI_MAPBY, PRTE_ARG_REQD),

    /* Ranking options */
    PRTE_OPTION_DEFINE(PRTE_CLI_RANKBY, PRTE_ARG_REQD),

    /* Binding options */
    PRTE_OPTION_DEFINE(PRTE_CLI_BINDTO, PRTE_ARG_REQD),

    /* display options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DISPLAY, PRTE_ARG_REQD),

    /* developer options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DO_NOT_LAUNCH, PRTE_ARG_REQD),

#if PRTE_ENABLE_FT
    PRTE_OPTION_DEFINE(PRTE_CLI_ENABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_MAX_RESTARTS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DISABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_CONTINUOUS, PRTE_ARG_NONE),
#endif

    // deprecated options
    PRTE_OPTION_DEFINE("mca", PRTE_ARG_REQD),
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
    PRTE_OPTION_DEFINE("rankfile", PRTE_ARG_REQD),

    PRTE_OPTION_END
};
static char *prterunshorts = "h::vVpn:c:N:sH:x:";

static struct option prunoptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSEABLE, PRTE_ARG_NONE, 'p'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PARSABLE, PRTE_ARG_NONE, 'p'), // synonym for parseable

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TUNE, PRTE_ARG_REQD),
    // DVM options
    PRTE_OPTION_DEFINE(PRTE_CLI_SYS_SERVER_FIRST, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SYS_SERVER_ONLY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DO_NOT_CONNECT, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_WAIT_TO_CONNECT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NUM_CONNECT_RETRIES, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PID, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NAMESPACE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DVM_URI, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PERSONALITY, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_RUN_AS_ROOT, PRTE_ARG_NONE),

    // Launch options
    PRTE_OPTION_DEFINE(PRTE_CLI_TIMEOUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_REPORT_STATE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STACK_TRACES, PRTE_ARG_NONE),
#ifdef PMIX_SPAWN_TIMEOUT
    PRTE_OPTION_DEFINE(PRTE_CLI_SPAWN_TIMEOUT, PRTE_ARG_REQD),
#endif
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'n'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NP, PRTE_ARG_REQD, 'c'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_NPERNODE, PRTE_ARG_REQD, 'N'),
    PRTE_OPTION_DEFINE(PRTE_CLI_APPFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_APPFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_XTERM, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_ON_EXEC, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_INIT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STOP_IN_APP, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_FWD_ENVAR, PRTE_ARG_REQD, 'x'),
    PRTE_OPTION_DEFINE(PRTE_CLI_WDIR, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("wd", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SET_CWD_SESSION, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_PATH, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_SHOW_PROGRESS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PSET, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_HOSTFILE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("machinefile", PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HOST, PRTE_ARG_REQD, 'H'),
    PRTE_OPTION_DEFINE(PRTE_CLI_PRELOAD_FILES, PRTE_ARG_REQD),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_PRELOAD_BIN, PRTE_ARG_NONE, 's'),

    // output options
    PRTE_OPTION_DEFINE(PRTE_CLI_OUTPUT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_STREAM_BUF, PRTE_ARG_REQD),

    // input options
    PRTE_OPTION_DEFINE(PRTE_CLI_STDIN, PRTE_ARG_REQD),

    /* Mapping options */
    PRTE_OPTION_DEFINE(PRTE_CLI_MAPBY, PRTE_ARG_REQD),

    /* Ranking options */
    PRTE_OPTION_DEFINE(PRTE_CLI_RANKBY, PRTE_ARG_REQD),

    /* Binding options */
    PRTE_OPTION_DEFINE(PRTE_CLI_BINDTO, PRTE_ARG_REQD),

    /* display options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DISPLAY, PRTE_ARG_REQD),

    /* developer options */
    PRTE_OPTION_DEFINE(PRTE_CLI_DO_NOT_LAUNCH, PRTE_ARG_REQD),

#if PRTE_ENABLE_FT
    PRTE_OPTION_DEFINE(PRTE_CLI_ENABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_MAX_RESTARTS, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DISABLE_RECOVERY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_CONTINUOUS, PRTE_ARG_NONE),
#endif

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
    PRTE_OPTION_DEFINE("rankfile", PRTE_ARG_REQD),

    PRTE_OPTION_END
};
static char *prunshorts = "h::vVpn:c:N:sH:x:";

static struct option prtedoptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),

    // DVM options
    PRTE_OPTION_DEFINE(PRTE_CLI_PUBSUB_SERVER, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_CONTROLLER_URI, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PARENT_URI, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_TREE_SPAWN, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DAEMONIZE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SYSTEM_SERVER, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SET_SID, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_DEBUG_DAEMONS_FILE, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_LEAVE_SESSION_ATTACHED, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_TEST_SUICIDE, PRTE_ARG_NONE),

    PRTE_OPTION_END
};
static char *prtedshorts = "hvV";

static struct option ptermoptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERBOSE, PRTE_ARG_NONE, 'v'),

    // MCA parameters
    PRTE_OPTION_DEFINE(PRTE_CLI_PRTEMCA, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PMIXMCA, PRTE_ARG_REQD),

    // DVM options
    PRTE_OPTION_DEFINE(PRTE_CLI_SYS_SERVER_FIRST, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_SYS_SERVER_ONLY, PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE(PRTE_CLI_WAIT_TO_CONNECT, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NUM_CONNECT_RETRIES, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_PID, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_NAMESPACE, PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE(PRTE_CLI_DVM_URI, PRTE_ARG_REQD),

    PRTE_OPTION_END
};
static char *ptermshorts = "hvV";

static struct option pinfooptions[] = {
    /* basic options */
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_HELP, PRTE_ARG_OPTIONAL, 'h'),
    PRTE_OPTION_SHORT_DEFINE(PRTE_CLI_VERSION, PRTE_ARG_NONE, 'V'),
    PRTE_OPTION_SHORT_DEFINE("all", PRTE_ARG_NONE, 'a'),
    PRTE_OPTION_DEFINE("arch", PRTE_ARG_NONE),
    PRTE_OPTION_SHORT_DEFINE("config", PRTE_ARG_NONE, 'c'),
    PRTE_OPTION_DEFINE("hostname", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("internal", PRTE_ARG_NONE),
    PRTE_OPTION_DEFINE("param", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("path", PRTE_ARG_REQD),
    PRTE_OPTION_DEFINE("show-version", PRTE_ARG_REQD),

    /* End of list */
    PRTE_OPTION_END
};
static char *pinfoshorts = "hVac";

static int convert_deprecated_cli(prte_cli_result_t *results,
                                  bool silent);

static int parse_cli(char **argv, prte_cli_result_t *results,
                     bool silent)
{
    char *shorts, *helpfile;
    struct option *myoptions;
    int rc, n;
    prte_cli_item_t *opt;

    if (0 == strcmp(prte_tool_actual, "prte")) {
        myoptions = prteoptions;
        shorts = prteshorts;
        helpfile = "help-schizo-prte.txt";
    } else if (0 == strcmp(prte_tool_actual, "prterun")) {
        myoptions = prterunoptions;
        shorts = prterunshorts;
        helpfile = "help-schizo-prterun.txt";
    } else if (0 == strcmp(prte_tool_actual, "prted")) {
        myoptions = prtedoptions;
        shorts = prtedshorts;
        helpfile = "help-schizo-prted.txt";
    } else if (0 == strcmp(prte_tool_actual, "prun")) {
        myoptions = prunoptions;
        shorts = prunshorts;
        helpfile = "help-schizo-prun.txt";
    } else if (0 == strcmp(prte_tool_actual, "pterm")) {
        myoptions = ptermoptions;
        shorts = ptermshorts;
        helpfile = "help-schizo-pterm.txt";
    } else if (0 == strcmp(prte_tool_actual, "prte_info")) {
        myoptions = pinfooptions;
        shorts = pinfoshorts;
        helpfile = "help-schizo-pinfo.txt";
    }
    rc = prte_cmd_line_parse(argv, shorts, myoptions, NULL,
                             results, helpfile);
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
                prte_schizo_base_expose(opt->values[n], "PRTE_MCA_");
            }
        } else if (0 == strcmp(opt->key, PRTE_CLI_PMIXMCA)) {
            for (n=0; NULL != opt->values[n]; n++) {
                prte_schizo_base_expose(opt->values[n], "PMIX_MCA_");
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
        warn = prte_schizo_prte_component.warn_deprecations;
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
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_MAPBY, p2,
                                                warn);
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
            pmix_asprintf(&p2, "dir=%s", opt->values[0]);
            rc = prte_schizo_base_add_directive(results, option,
                                                PRTE_CLI_OUTPUT, p2,
                                                warn);
            free(p2);
            PRTE_CLI_REMOVE_DEPRECATED(results, opt);
        }
        /* --output-filename DIR  ->  --output file=file */
        else if (0 == strcmp(option, "--output-filename")) {
            pmix_asprintf(&p2, "file=%s", opt->values[0]);
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
                    free(p2);
                    free(tmp2);
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
                    free(p2);
                    free(tmp2);
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
                    free(p2);
                    free(tmp2);
                }
                free(p1);
                free(opt->values[0]);
                opt->values[0] = tmp;
            }
        }
    }

    return rc;
}

static int parse_env(char **srcenv, char ***dstenv,
                     prte_cli_result_t *cli)
{
    int i, j, n;
    char *p1, *p2;
    char **env;
    prte_value_t *pval;
    char **xparams = NULL, **xvals = NULL;
    char *param, *value;
    prte_cli_item_t *opt;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (NULL == cli) {
        return PRTE_SUCCESS;
    }

    env = *dstenv;

    /* look for -x options - not allowed to conflict with a -mca option */
    opt = prte_cmd_line_get_param(cli, PRTE_CLI_FWD_ENVAR);
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
                    prte_show_help("help-schizo-base.txt", "missing-envar-param", true, p1);
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
                        prte_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                       value);
                        free(param);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
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
                        prte_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                       xvals[i]);
                        pmix_argv_free(xparams);
                        pmix_argv_free(xvals);
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
            }

            /* cache this for later inclusion - do not modify dstenv in this loop */
            pmix_argv_append_nosize(&xparams, p1);
            pmix_argv_append_nosize(&xvals, p2);
        }
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
    char *evar;

    prte_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__,
                        (NULL == personalities) ? "NULL" : personalities,
                        prte_tool_basename);

    /* COMMAND-LINE OVERRRULES ALL */
    if (NULL != personalities) {
        /* this is a list of personalities we need to check -
         * if it contains "prte", then we are available but
         * at a low priority */
        if (NULL != strstr(personalities, "prte")) {
            return prte_schizo_prte_component.priority;
        }
        return 0;
    }

    /* if we were told the proxy, then use it */
    if (NULL != (evar = getenv("PRTE_MCA_schizo_proxy"))) {
        if (0 == strcmp(evar, "prte")) {
            /* they asked exclusively for us */
            return 100;
        } else {
            /* they asked for somebody else */
            return 0;
        }
    }

    /* if neither of those were true, then just use our default */
    return prte_schizo_prte_component.priority;
}

static void allow_run_as_root(prte_cli_result_t *cli)
{
    char *r1, *r2;

    if (prte_cmd_line_is_taken(cli, "allow-run-as-root")) {
        return;
    }

    if (NULL != (r1 = getenv("PRTE_ALLOW_RUN_AS_ROOT"))
        && NULL != (r2 = getenv("PRTE_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return;
        }
    }

    prte_schizo_base_root_error_msg();
}

static void job_info(prte_cli_result_t *results,
                     void *jobinfo)
{
    return;
}
