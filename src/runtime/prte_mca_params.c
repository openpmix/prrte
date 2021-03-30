/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#include <signal.h>
#include <stdio.h>

#include "src/mca/base/prte_mca_base_var.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/proc_info.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

static bool passed_thru = false;
static int prte_progress_thread_debug_level = -1;
static char *prte_fork_agent_string = NULL;
static char *prte_tmpdir_base = NULL;
static char *prte_local_tmpdir_base = NULL;
static char *prte_remote_tmpdir_base = NULL;
static char *prte_top_session_dir = NULL;
static char *prte_jobfam_session_dir = NULL;

char *prte_signal_string = NULL;
char *prte_stacktrace_output_filename = NULL;
char *prte_net_private_ipv4 = NULL;
char *prte_set_max_sys_limits = NULL;
int prte_abort_delay = 0;
bool prte_abort_print_stack = false;
int prte_pmix_verbose_output = 0;

int prte_max_thread_in_progress = 1;

int prte_register_params(void)
{
    int ret;
    prte_output_stream_t lds;
    char *string = NULL;

    /* only go thru this once - mpirun calls it twice, which causes
     * any error messages to show up twice
     */
    if (passed_thru) {
        return PRTE_SUCCESS;
    }
    passed_thru = true;

    /*
     * This string is going to be used in prte/util/stacktrace.c
     */
    {
        int j;
        int signals[] = {
#ifdef SIGABRT
            SIGABRT,
#endif
#ifdef SIGBUS
            SIGBUS,
#endif
#ifdef SIGFPE
            SIGFPE,
#endif
#ifdef SIGSEGV
            SIGSEGV,
#endif
            -1};
        for (j = 0; signals[j] != -1; ++j) {
            if (j == 0) {
                prte_asprintf(&string, "%d", signals[j]);
            } else {
                char *tmp;
                prte_asprintf(&tmp, "%s,%d", string, signals[j]);
                free(string);
                string = tmp;
            }
        }

        prte_signal_string = string;
        ret = prte_mca_base_var_register(
            "prte", "prte", NULL, "signal",
            "Comma-delimited list of integer signal numbers to PRTE to attempt to intercept.  Upon "
            "receipt of the intercepted signal, PRTE will display a stack trace and abort.  PRTE "
            "will *not* replace signals if handlers are already installed by the time MPI_INIT is "
            "invoked.  Optionally append \":complain\" to any signal number in the comma-delimited "
            "list to make PRTE complain if it detects another signal handler (and therefore does "
            "not insert its own).",
            PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE,
            PRTE_INFO_LVL_3, PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_signal_string);
        free(string);
        if (0 > ret) {
            return ret;
        }
    }

    /*
     * Where should the stack trace output be directed
     * This string is going to be used in prte/util/stacktrace.c
     */
    string = strdup("stderr");
    prte_stacktrace_output_filename = string;
    ret = prte_mca_base_var_register(
        "prte", "prte", NULL, "stacktrace_output",
        "Specifies where the stack trace output stream goes.  "
        "Accepts one of the following: none (disabled), stderr (default), stdout, file[:filename]. "
        "  "
        "If 'filename' is not specified, a default filename of 'stacktrace' is used.  "
        "The 'filename' is appended with either '.PID' or '.RANK.PID', if RANK is available.  "
        "The 'filename' can be an absolute path or a relative path to the current working "
        "directory.",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_3,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_stacktrace_output_filename);
    free(string);
    if (0 > ret) {
        return ret;
    }

    /* RFC1918 defines
       - 10.0.0./8
       - 172.16.0.0/12
       - 192.168.0.0/16

       RFC3330 also mentions
       - 169.254.0.0/16 for DHCP onlink iff there's no DHCP server
    */
    prte_net_private_ipv4 = "10.0.0.0/8;172.16.0.0/12;192.168.0.0/16;169.254.0.0/16";
    ret = prte_mca_base_var_register(
        "prte", "prte", "net", "private_ipv4",
        "Semicolon-delimited list of CIDR notation entries specifying what networks are considered "
        "\"private\" (default value based on RFC1918 and RFC3330)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_3,
        PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_net_private_ipv4);
    if (0 > ret) {
        return ret;
    }

    prte_set_max_sys_limits = NULL;
    ret = prte_mca_base_var_register(
        "prte", "prte", NULL, "set_max_sys_limits",
        "Set the specified system-imposed limits to the specified value, including \"unlimited\"."
        "Supported params: core, filesize, maxmem, openfiles, stacksize, maxchildren",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_3,
        PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_set_max_sys_limits);
    if (0 > ret) {
        return ret;
    }

    prte_abort_delay = 0;
    ret = prte_mca_base_var_register(
        "prte", "prte", NULL, "abort_delay",
        "If nonzero, print out an identifying message when abort operation is invoked (hostname, "
        "PID of the process that called abort) and delay for that many seconds before exiting (a "
        "negative delay value means to never abort).  This allows attaching of a debugger before "
        "quitting the job.",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_abort_delay);
    if (0 > ret) {
        return ret;
    }

    prte_abort_print_stack = false;
    ret = prte_mca_base_var_register("prte", "prte", NULL, "abort_print_stack",
                                     "If nonzero, print out a stack trace when abort is invoked",
                                     PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, PRTE_MCA_BASE_VAR_FLAG_NONE,
    /* If we do not have stack trace
       capability, make this a constant
       MCA variable */
#if PRTE_WANT_PRETTY_PRINT_STACKTRACE
                                     PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
                                     PRTE_MCA_BASE_VAR_SCOPE_READONLY,
#else
                                     PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY, PRTE_INFO_LVL_5,
                                     PRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
#endif
                                     &prte_abort_print_stack);
    if (0 > ret) {
        return ret;
    }

    /* get a clean output channel too - need to do this here because
     * we use it below, and prun and some other tools call this
     * function prior to calling prte_init
     */
    PRTE_CONSTRUCT(&lds, prte_output_stream_t);
    lds.lds_want_stdout = true;
    prte_clean_output = prte_output_open(&lds);
    PRTE_DESTRUCT(&lds);

    prte_help_want_aggregate = true;
    (void) prte_mca_base_var_register(
        "prte", "prte", "base", "help_aggregate",
        "If prte_base_help_aggregate is true, duplicate help messages will be aggregated rather "
        "than displayed individually.  This can be helpful for parallel jobs that experience "
        "multiple identical failures; rather than print out the same help/failure message N times, "
        "display it once with a count of how many processes sent the same message.",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_help_want_aggregate);

    /* LOOK FOR A TMP DIRECTORY BASE */
    /* Several options are provided to cover a range of possibilities:
     *
     * (a) all processes need to use a specified location as the base
     *     for tmp directories
     * (b) daemons on remote nodes need to use a specified location, but
     *     one different from that used by mpirun
     * (c) mpirun needs to use a specified location, but one different
     *     from that used on remote nodes
     */
    prte_tmpdir_base = NULL;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "tmpdir_base",
                                   "Base of the session directory tree to be used by all processes",
                                   PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                   PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                   PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_tmpdir_base);

    prte_local_tmpdir_base = NULL;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "local_tmpdir_base",
                                   "Base of the session directory tree to be used by prun/mpirun",
                                   PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                   PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                   PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_local_tmpdir_base);

    prte_remote_tmpdir_base = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "remote_tmpdir_base",
                                      "Base of the session directory tree on remote nodes, if "
                                      "required to be different from head node",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_remote_tmpdir_base);

    /* if a global tmpdir was specified, then we do not allow specification
     * of the local or remote values to avoid confusion
     */
    if (NULL != prte_tmpdir_base
        && (NULL != prte_local_tmpdir_base || NULL != prte_remote_tmpdir_base)) {
        prte_output(prte_clean_output,
                    "------------------------------------------------------------------\n"
                    "The MCA param prte_tmpdir_base was specified, which sets the base\n"
                    "of the temporary directory tree for all procs. However, values for\n"
                    "the local and/or remote tmpdir base were also given. This can lead\n"
                    "to confusion and is therefore not allowed. Please specify either a\n"
                    "global tmpdir base OR a local/remote tmpdir base value\n"
                    "------------------------------------------------------------------");
        exit(1);
    }

    if (NULL != prte_tmpdir_base) {
        if (NULL != prte_process_info.tmpdir_base) {
            free(prte_process_info.tmpdir_base);
        }
        prte_process_info.tmpdir_base = strdup(prte_tmpdir_base);
    } else if (PRTE_PROC_IS_MASTER && NULL != prte_local_tmpdir_base) {
        /* prun will pickup the value for its own use */
        if (NULL != prte_process_info.tmpdir_base) {
            free(prte_process_info.tmpdir_base);
        }
        prte_process_info.tmpdir_base = strdup(prte_local_tmpdir_base);
    } else if (PRTE_PROC_IS_DAEMON && NULL != prte_remote_tmpdir_base) {
        /* prun will pickup the value and forward it along, but must not
         * use it in its own work. So only a daemon needs to get it, and the
         * daemon will pass it down to its application procs. Note that prun
         * will pass -its- value to any procs local to it
         */
        if (NULL != prte_process_info.tmpdir_base) {
            free(prte_process_info.tmpdir_base);
        }
        prte_process_info.tmpdir_base = strdup(prte_remote_tmpdir_base);
    }

    prte_top_session_dir = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "top_session_dir",
                                      "Top of the session directory tree for applications",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_top_session_dir);

    if (NULL != prte_top_session_dir) {
        if (NULL != prte_process_info.top_session_dir) {
            free(prte_process_info.top_session_dir);
        }
        prte_process_info.top_session_dir = strdup(prte_top_session_dir);
    }

    prte_jobfam_session_dir = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "jobfam_session_dir",
                                      "The jobfamily session directory for applications",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ, &prte_jobfam_session_dir);

    if (NULL != prte_jobfam_session_dir) {
        if (NULL != prte_process_info.jobfam_session_dir) {
            free(prte_process_info.jobfam_session_dir);
        }
        prte_process_info.jobfam_session_dir = strdup(prte_jobfam_session_dir);
    }

    prte_prohibited_session_dirs = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "no_session_dirs",
                                      "Prohibited locations for session directories (multiple "
                                      "locations separated by ',', default=NULL)",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL, &prte_prohibited_session_dirs);

    prte_create_session_dirs = true;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "create_session_dirs",
                                      "Create session directories", PRTE_MCA_BASE_VAR_TYPE_BOOL,
                                      NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL, &prte_create_session_dirs);

    prte_execute_quiet = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "execute_quiet",
                                      "Do not output error and help messages",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL, &prte_execute_quiet);

    prte_report_silent_errors = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "report_silent_errors",
                                      "Report all errors, including silent ones",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL, &prte_report_silent_errors);

    prte_progress_thread_debug_level = -1;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "progress_thread_debug",
                                      "Debug level for PRTE progress threads",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                      &prte_progress_thread_debug_level);

    if (0 <= prte_progress_thread_debug_level) {
        prte_progress_thread_debug = prte_output_open(NULL);
        prte_output_set_verbosity(prte_progress_thread_debug, prte_progress_thread_debug_level);
    }

    prted_debug_failure = PMIX_RANK_INVALID;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "daemon_fail",
        "Have the specified prted fail after init for debugging purposes",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prted_debug_failure);

    prted_debug_failure_delay = 0;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "daemon_fail_delay",
        "Have the specified prted fail after specified number of seconds (default: 0 => no delay)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prted_debug_failure_delay);

    prte_startup_timeout = 0;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "startup_timeout",
                                      "Seconds to wait for startup or job launch before declaring "
                                      "failed_to_start (default: 0 => do not check)",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_startup_timeout);

    /* default hostfile */
    prte_default_hostfile = NULL;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "default_hostfile",
                                   "Name of the default hostfile (relative or absolute path, "
                                   "\"none\" to ignore environmental or default MCA param setting)",
                                   PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                   PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                   PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_default_hostfile);

    if (NULL == prte_default_hostfile) {
        /* nothing was given, so define the default */
        prte_asprintf(&prte_default_hostfile, "%s/prte-default-hostfile",
                      prte_install_dirs.sysconfdir);
        /* flag that nothing was given */
        prte_default_hostfile_given = false;
    } else if (0 == strcmp(prte_default_hostfile, "none")) {
        free(prte_default_hostfile);
        prte_default_hostfile = NULL;
        /* flag that it was given */
        prte_default_hostfile_given = true;
    } else {
        /* flag that it was given */
        prte_default_hostfile_given = true;
    }

    /* default dash-host */
    prte_default_dash_host = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "default_dash_host",
                                      "Default -host setting (specify \"none\" to ignore "
                                      "environmental or default MCA param setting)",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_default_dash_host);
    if (NULL != prte_default_dash_host && 0 == strcmp(prte_default_dash_host, "none")) {
        free(prte_default_dash_host);
        prte_default_dash_host = NULL;
    }

    prte_hostname_cutoff = 1000;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "hostname_cutoff",
        "Pass hostnames to all procs when #nodes is less than cutoff [default:1000]",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_3,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_hostname_cutoff);

    /* which alias to use in MPIR_proctab */
    prte_use_hostname_alias = 1;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "hostname_alias_index",
        "Which alias to use for the debugger proc table [default: 1st alias]",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_use_hostname_alias);

    prte_show_resolved_nodenames = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "show_resolved_nodenames",
        "Display any node names that are resolved to a different name (default: false)",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_show_resolved_nodenames);

    /* allow specification of the launch agent */
    prte_launch_agent = "prted";
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "launch_agent",
        "Command used to start processes on remote nodes (default: prted)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_launch_agent);

    prte_fork_agent_string = NULL;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "fork_agent",
                                   "Command used to fork processes on remote nodes (default: NULL)",
                                   PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                   PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                   PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_fork_agent_string);

    if (NULL != prte_fork_agent_string) {
        prte_fork_agent = prte_argv_split(prte_fork_agent_string, ' ');
    }

    /* whether or not to require RM allocation */
    prte_allocation_required = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "allocation_required",
        "Whether or not an allocation by a resource manager is required [default: no]",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_allocation_required);

    /* whether or not to map stddiag to stderr */
    prte_map_stddiag_to_stderr = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "map_stddiag_to_stderr",
        "Map output from prte_output to stderr of the local process [default: no]",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_map_stddiag_to_stderr);

    /* whether or not to map stddiag to stderr */
    prte_map_stddiag_to_stdout = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "map_stddiag_to_stdout",
        "Map output from prte_output to stdout of the local process [default: no]",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_map_stddiag_to_stdout);
    if (prte_map_stddiag_to_stderr && prte_map_stddiag_to_stdout) {
        prte_output(0,
                    "The options \"prte_map_stddiag_to_stderr\" and \"prte_map_stddiag_to_stdout\" "
                    "are mutually exclusive. They cannot both be set to true.");
        return PRTE_ERROR;
    }

    /* generate new terminal windows to display output from specified ranks */
    prte_xterm = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "xterm",
                                      "Create a new xterm window and display output from the "
                                      "specified ranks there [default: none]",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_xterm);
    if (NULL != prte_xterm) {
        /* if an xterm request is given, we have to leave any ssh
         * sessions attached so the xterm window manager can get
         * back to the controlling terminal
         */
        prte_leave_session_attached = true;
        /* also want to redirect stddiag output from prte_output
         * to stderr from the process so those messages show
         * up in the xterm window instead of being forwarded to mpirun
         */
        prte_map_stddiag_to_stderr = true;
    }

    /* whether or not to report launch progress */
    prte_report_launch_progress = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "report_launch_progress",
        "Output a brief periodic report on launch progress [default: no]",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_report_launch_progress);

    /* tool communication controls */
    prte_report_events_uri = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "report_events",
                                      "URI to which events are to be reported (default: NULL)",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_report_events_uri);
    if (NULL != prte_report_events_uri) {
        prte_report_events = true;
    }

    /* barrier control */
    prte_do_not_barrier = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "do_not_barrier",
                                      "Do not barrier in prte_init", PRTE_MCA_BASE_VAR_TYPE_BOOL,
                                      NULL, 0, PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_do_not_barrier);

    prte_enable_recovery = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "enable_recovery",
                                      "Enable recovery from process failure [Default = disabled]",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_enable_recovery);

    prte_max_restarts = 0;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "max_restarts",
                                      "Max number of times to restart a failed process",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_max_restarts);

    if (!prte_enable_recovery && prte_max_restarts != 0) {
        if (PRTE_PROC_IS_MASTER) {
            prte_output(prte_clean_output,
                        "------------------------------------------------------------------\n"
                        "The MCA param prte_enable_recovery was not set to true, but\n"
                        "a value was provided for the number of restarts:\n\n"
                        "Max restarts: %d\n"
                        "We are enabling process recovery and continuing execution. To avoid\n"
                        "this warning in the future, please set the prte_enable_recovery\n"
                        "param to non-zero.\n"
                        "------------------------------------------------------------------",
                        prte_max_restarts);
        }
        prte_enable_recovery = true;
    }

    prte_abort_non_zero_exit = true;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "abort_on_non_zero_status",
        "Abort the job if any process returns a non-zero exit status - no restart in such cases",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_abort_non_zero_exit);

    prte_allowed_exit_without_sync = false;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "allowed_exit_without_sync",
        "Process exiting without calling finalize will not trigger job termination",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_allowed_exit_without_sync);

    prte_report_child_jobs_separately = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "report_child_jobs_separately",
                                      "Return the exit status of the primary job only",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                      &prte_report_child_jobs_separately);

    prte_stat_history_size = 1;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "stat_history_size",
                                      "Number of stat samples to keep", PRTE_MCA_BASE_VAR_TYPE_INT,
                                      NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_stat_history_size);

    prte_max_vm_size = -1;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "max_vm_size",
                                   "Maximum size of virtual machine - used to subdivide allocation",
                                   PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                                   PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                   &prte_max_vm_size);

    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "set_default_slots",
        "Set the number of slots on nodes that lack such info to the"
        " number of specified objects [a number, \"cores\" (default),"
        " \"packages\", \"hwthreads\" (default if hwthreads_as_cpus is set),"
        " or \"none\" to skip this option]",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_set_slots);

    /* allow specification of the cores to be used by daemons */
    prte_daemon_cores = NULL;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "daemon_cores",
                                      "Restrict the PRTE daemons (including mpirun) to operate on "
                                      "the specified cores (comma-separated list of ranges)",
                                      PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_daemon_cores);

    /* Amount of time to wait for a stack trace to return from the daemons */
    prte_stack_trace_wait_timeout = 30;
    (void)
        prte_mca_base_var_register("prte", "prte", NULL, "timeout_for_stack_trace",
                                   "Seconds to wait for stack traces to return before terminating "
                                   "the job (<= 0 wait forever)",
                                   PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                                   PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                   &prte_stack_trace_wait_timeout);

    /* register the URI of the UNIVERSAL data server */
    prte_data_server_uri = NULL;
    (void) prte_mca_base_var_register(
        "prte", "pmix", NULL, "server_uri",
        "URI of a session-level keyval server for publish/lookup operations",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_3,
        PRTE_MCA_BASE_VAR_SCOPE_ALL, &prte_data_server_uri);

    prte_mca_base_var_register("prte", "prte", NULL, "pmix_verbose",
                               "Verbosity for PRTE-level PMIx code", PRTE_MCA_BASE_VAR_TYPE_INT,
                               NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
                               PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_pmix_verbose_output);

#if PRTE_ENABLE_FT
    prte_mca_base_var_register("prte", "prte", NULL, "enable_ft", "Enable/disable fault tolerance",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_enable_ft);
#endif

    return PRTE_SUCCESS;
}
