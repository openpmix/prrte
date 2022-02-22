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
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
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

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/pmix_argv.h"
#include "src/util/output.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_environ.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

static bool passed_thru = false;
static int prte_progress_thread_debug_level = -1;
static char *prte_tmpdir_base = NULL;
static char *prte_local_tmpdir_base = NULL;
static char *prte_remote_tmpdir_base = NULL;
static char *prte_top_session_dir = NULL;
static char *prte_jobfam_session_dir = NULL;

char *prte_signal_string = NULL;
char *prte_stacktrace_output_filename = NULL;
char *prte_net_private_ipv4 = NULL;
char *pmix_if_include = NULL;
char *pmix_if_exclude = NULL;
char *prte_set_max_sys_limits = NULL;
int prte_pmix_verbose_output = 0;
char *prte_progress_thread_cpus = NULL;
bool prte_bind_progress_thread_reqd = false;

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
                pmix_asprintf(&string, "%d", signals[j]);
            } else {
                char *tmp;
                pmix_asprintf(&tmp, "%s,%d", string, signals[j]);
                free(string);
                string = tmp;
            }
        }

        prte_signal_string = string;
        ret = pmix_mca_base_var_register(
            "prte", "prte", NULL, "signal",
            "Comma-delimited list of integer signal numbers to PRTE to attempt to intercept.  Upon "
            "receipt of the intercepted signal, PRTE will display a stack trace and abort.  PRTE "
            "will *not* replace signals if handlers are already installed by the time MPI_INIT is "
            "invoked.  Optionally append \":complain\" to any signal number in the comma-delimited "
            "list to make PRTE complain if it detects another signal handler (and therefore does "
            "not insert its own).",
            PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_signal_string);
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
    ret = pmix_mca_base_var_register(
        "prte", "prte", NULL, "stacktrace_output",
        "Specifies where the stack trace output stream goes.  "
        "Accepts one of the following: none (disabled), stderr (default), stdout, file[:filename]. "
        "  "
        "If 'filename' is not specified, a default filename of 'stacktrace' is used.  "
        "The 'filename' is appended with either '.PID' or '.RANK.PID', if RANK is available.  "
        "The 'filename' can be an absolute path or a relative path to the current working "
        "directory.",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_stacktrace_output_filename);
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
    ret = pmix_mca_base_var_register(
        "prte", "prte", "net", "private_ipv4",
        "Semicolon-delimited list of CIDR notation entries specifying what networks are considered "
        "\"private\" (default value based on RFC1918 and RFC3330)",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_net_private_ipv4);
    if (0 > ret) {
        return ret;
    }

    pmix_if_include = NULL;
    ret = pmix_mca_base_var_register("prte", "prte", NULL, "if_include",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks to use for PRTE "
                                    "bootstrap communication (e.g., \"eth0,192.168.0.0/16\").  Mutually exclusive with "
                                     "pmix_if_exclude.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_if_include);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "include",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "if_include",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    pmix_if_exclude = NULL;
    ret = pmix_mca_base_var_register("prte", "prte", NULL, "if_exclude",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks to NOT use for PRTE "
                                     "bootstrap communication -- all devices not matching these specifications will be used "
                                     "(e.g., \"eth0,192.168.0.0/16\").  If set to a non-default value, it is mutually exclusive "
                                        "with pmix_if_include.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_if_exclude);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "exclude",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "if_exclude",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* if_include and if_exclude need to be mutually exclusive */
    if (NULL != pmix_if_include && NULL != pmix_if_exclude) {
        /* Return ERR_NOT_AVAILABLE so that a warning message about
         "open" failing is not printed */
        prte_show_help("help-oob-tcp.txt", "include-exclude", true,
                       pmix_if_include, pmix_if_exclude);
        return PRTE_ERR_NOT_AVAILABLE;
    }

    prte_set_max_sys_limits = NULL;
    ret = pmix_mca_base_var_register(
        "prte", "prte", NULL, "set_max_sys_limits",
        "Set the specified system-imposed limits to the specified value, including \"unlimited\"."
        "Supported params: core, filesize, maxmem, openfiles, stacksize, maxchildren",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_set_max_sys_limits);
    if (0 > ret) {
        return ret;
    }

    /* get a clean output channel too - need to do this here because
     * we use it below, and prun and some other tools call this
     * function prior to calling prte_init
     */
    PMIX_CONSTRUCT(&lds, prte_output_stream_t);
    lds.lds_want_stdout = true;
    prte_clean_output = prte_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    prte_help_want_aggregate = true;
    (void) pmix_mca_base_var_register(
        "prte", "prte", "base", "help_aggregate",
        "If prte_base_help_aggregate is true, duplicate help messages will be aggregated rather "
        "than displayed individually.  This can be helpful for parallel jobs that experience "
        "multiple identical failures; rather than print out the same help/failure message N times, "
        "display it once with a count of how many processes sent the same message.",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_help_want_aggregate);

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
        pmix_mca_base_var_register("prte", "prte", NULL, "tmpdir_base",
                                   "Base of the session directory tree to be used by all processes",
                                   PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_tmpdir_base);

    prte_local_tmpdir_base = NULL;
    (void)
        pmix_mca_base_var_register("prte", "prte", NULL, "local_tmpdir_base",
                                   "Base of the session directory tree to be used by prun/mpirun",
                                   PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_local_tmpdir_base);

    prte_remote_tmpdir_base = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "remote_tmpdir_base",
                                      "Base of the session directory tree on remote nodes, if "
                                      "required to be different from head node",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_remote_tmpdir_base);

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
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "top_session_dir",
                                      "Top of the session directory tree for applications",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_top_session_dir);

    if (NULL != prte_top_session_dir) {
        if (NULL != prte_process_info.top_session_dir) {
            free(prte_process_info.top_session_dir);
        }
        prte_process_info.top_session_dir = strdup(prte_top_session_dir);
    }

    prte_jobfam_session_dir = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "jobfam_session_dir",
                                      "The jobfamily session directory for applications",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_jobfam_session_dir);

    if (NULL != prte_jobfam_session_dir) {
        if (NULL != prte_process_info.jobfam_session_dir) {
            free(prte_process_info.jobfam_session_dir);
        }
        prte_process_info.jobfam_session_dir = strdup(prte_jobfam_session_dir);
    }

    prte_prohibited_session_dirs = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "no_session_dirs",
                                      "Prohibited locations for session directories (multiple "
                                      "locations separated by ',', default=NULL)",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_prohibited_session_dirs);

    prte_create_session_dirs = true;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "create_session_dirs",
                                      "Create session directories",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_create_session_dirs);

    prte_add_pid_to_session_dirname = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "add_pid_to_session_dirname",
                                      "Add pid to the DVM top-level session directory name",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_add_pid_to_session_dirname);

    prte_execute_quiet = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "execute_quiet",
                                      "Do not output error and help messages",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_execute_quiet);

    prte_report_silent_errors = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "report_silent_errors",
                                      "Report all errors, including silent ones",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_report_silent_errors);

    prte_progress_thread_debug_level = -1;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "progress_thread_debug",
                                      "Debug level for PRTE progress threads",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &prte_progress_thread_debug_level);

    if (0 <= prte_progress_thread_debug_level) {
        prte_progress_thread_debug = prte_output_open(NULL);
        prte_output_set_verbosity(prte_progress_thread_debug, prte_progress_thread_debug_level);
    }

    prted_debug_failure = PMIX_RANK_INVALID;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "daemon_fail",
        "Have the specified prted fail after init for debugging purposes",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prted_debug_failure);

    prted_debug_failure_delay = 0;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "daemon_fail_delay",
        "Have the specified prted fail after specified number of seconds [default: 0 => no delay]",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prted_debug_failure_delay);

    /* default hostfile */
    prte_default_hostfile = NULL;
    (void)
        pmix_mca_base_var_register("prte", "prte", NULL, "default_hostfile",
                                   "Name of the default hostfile (relative or absolute path, "
                                   "\"none\" to ignore environmental or default MCA param setting)",
                                   PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_default_hostfile);

    if (NULL == prte_default_hostfile) {
        /* nothing was given, so define the default */
        pmix_asprintf(&prte_default_hostfile, "%s/prte-default-hostfile",
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
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "default_dash_host",
                                      "Default -host setting (specify \"none\" to ignore "
                                      "environmental or default MCA param setting)",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_default_dash_host);
    if (NULL != prte_default_dash_host && 0 == strcmp(prte_default_dash_host, "none")) {
        free(prte_default_dash_host);
        prte_default_dash_host = NULL;
    }

    prte_hostname_cutoff = 1000;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "hostname_cutoff",
        "Pass hostnames to all procs when #nodes is less than cutoff [default:1000]",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prte_hostname_cutoff);

    prte_show_resolved_nodenames = false;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "show_resolved_nodenames",
        "Display any node names that are resolved to a different name [default: false]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_show_resolved_nodenames);

    prte_do_not_resolve = true;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "do_not_resolve",
                                      "Do not attempt to resolve hostnames "
                                      "[defaults to true]",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_do_not_resolve);

    /* allow specification of the launch agent */
    prte_launch_agent = "prted";
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "launch_agent",
        "Command used to start processes on remote nodes [default: prted]",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_launch_agent);

    prte_fork_agent_string = NULL;
    (void)
        pmix_mca_base_var_register("prte", "prte", NULL, "fork_agent",
                                   "Command used to fork processes on remote nodes [default: NULL]",
                                   PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_fork_agent_string);

    /* whether or not to require RM allocation */
    prte_allocation_required = false;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "allocation_required",
        "Whether or not an allocation by a resource manager is required [default: no]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_allocation_required);

    /* whether or not to map stddiag to stderr */
    prte_map_stddiag_to_stderr = false;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "map_stddiag_to_stderr",
        "Map output from prte_output to stderr of the local process [default: no]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_map_stddiag_to_stderr);

    /* whether or not to map stddiag to stderr */
    prte_map_stddiag_to_stdout = false;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "map_stddiag_to_stdout",
        "Map output from prte_output to stdout of the local process [default: no]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_map_stddiag_to_stdout);
    if (prte_map_stddiag_to_stderr && prte_map_stddiag_to_stdout) {
        prte_output(0,
                    "The options \"prte_map_stddiag_to_stderr\" and \"prte_map_stddiag_to_stdout\" "
                    "are mutually exclusive. They cannot both be set to true.");
        return PRTE_ERROR;
    }

    /* generate new terminal windows to display output from specified ranks */
    prte_xterm = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "xterm",
                                      "Create a new xterm window and display output from the "
                                      "specified ranks there [default: none]",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_xterm);
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
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "report_launch_progress",
        "Output a brief periodic report on launch progress [default: no]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_report_launch_progress);

    /* tool communication controls */
    prte_report_events_uri = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "report_events",
                                      "URI to which events are to be reported [default: NULL]",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_report_events_uri);
    if (NULL != prte_report_events_uri) {
        prte_report_events = true;
    }

    /* barrier control */
    prte_do_not_barrier = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "do_not_barrier",
                                      "Do not barrier in prte_init",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_do_not_barrier);

    prte_enable_recovery = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "enable_recovery",
                                      "Enable recovery from process failure [Default = disabled]",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_enable_recovery);

    prte_max_restarts = 0;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "max_restarts",
                                      "Max number of times to restart a failed process",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &prte_max_restarts);

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
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "abort_on_non_zero_status",
        "Abort the job if any process returns a non-zero exit status - no restart in such cases",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_abort_non_zero_exit);

    prte_allowed_exit_without_sync = false;
    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "allowed_exit_without_sync",
        "Process exiting without calling finalize will not trigger job termination",
        PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_allowed_exit_without_sync);

    prte_report_child_jobs_separately = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "report_child_jobs_separately",
                                      "Return the exit status of the primary job only",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_report_child_jobs_separately);

    prte_stat_history_size = 1;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "stat_history_size",
                                      "Number of stat samples to keep",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &prte_stat_history_size);

    prte_max_vm_size = -1;
    (void)
        pmix_mca_base_var_register("prte", "prte", NULL, "max_vm_size",
                                   "Maximum size of virtual machine - used to subdivide allocation",
                                   PMIX_MCA_BASE_VAR_TYPE_INT,
                                   &prte_max_vm_size);

    (void) pmix_mca_base_var_register(
        "prte", "prte", NULL, "set_default_slots",
        "Set the number of slots on nodes that lack such info to the"
        " number of specified objects [a number, \"cores\" (default),"
        " \"packages\", \"hwthreads\" (default if hwthreads_as_cpus is set),"
        " or \"none\" to skip this option]",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_set_slots);

    /* allow specification of the cores to be used by daemons */
    prte_daemon_cores = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "daemon_cores",
                                      "Restrict the PRTE daemons (including mpirun) to operate on "
                                      "the specified cores (comma-separated list of ranges)",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_daemon_cores);

    /* Amount of time to wait for a stack trace to return from the daemons */
    prte_stack_trace_wait_timeout = 30;
    (void)
        pmix_mca_base_var_register("prte", "prte", NULL, "timeout_for_stack_trace",
                                   "Seconds to wait for stack traces to return before terminating "
                                   "the job (<= 0 wait forever)",
                                   PMIX_MCA_BASE_VAR_TYPE_INT,
                                   &prte_stack_trace_wait_timeout);

    /* register the URI of the UNIVERSAL data server */
    prte_data_server_uri = NULL;
    (void) pmix_mca_base_var_register(
        "prte", "pmix", NULL, "server_uri",
        "URI of a session-level keyval server for publish/lookup operations",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_data_server_uri);

    pmix_mca_base_var_register("prte", "prte", NULL, "pmix_verbose",
                               "Verbosity for PRTE-level PMIx code",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_pmix_verbose_output);

    (void) pmix_mca_base_var_register("prte", "prte", NULL, "progress_thread_cpus",
                                      "Comma-delimited list of ranges of CPUs to which"
                                      "the internal PRRTE progress thread is to be bound",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &prte_progress_thread_cpus);

    (void) pmix_mca_base_var_register("prte", "prte", NULL, "bind_progress_thread_reqd",
                                      "Whether binding of internal PRRTE progress thread is required",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_bind_progress_thread_reqd);

#if PRTE_ENABLE_FT
    pmix_mca_base_var_register("prte", "prte", NULL, "enable_ft", "Enable/disable fault tolerance",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_enable_ft);
#endif

    return PRTE_SUCCESS;
}
