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
 * Copyright (c) 2007-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <signal.h>
#include <stdio.h>

#include "src/mca/base/prrte_mca_base_var.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/util/printf.h"
#include "src/util/prrte_environ.h"

#include "src/util/proc_info.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/dss/dss.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"

static bool passed_thru = false;
static int prrte_progress_thread_debug_level = -1;
static char *prrte_xml_file = NULL;
static char *prrte_fork_agent_string = NULL;
static char *prrte_tmpdir_base = NULL;
static char *prrte_local_tmpdir_base = NULL;
static char *prrte_remote_tmpdir_base = NULL;
static char *prrte_top_session_dir = NULL;
static char *prrte_jobfam_session_dir = NULL;

char *prrte_signal_string = NULL;
char *prrte_stacktrace_output_filename = NULL;
char *prrte_net_private_ipv4 = NULL;
char *prrte_set_max_sys_limits = NULL;
int prrte_abort_delay = 0;
bool prrte_abort_print_stack = false;
int prrte_pmix_verbose_output = 0;

int prrte_max_thread_in_progress = 1;

int prrte_register_params(void)
{
    int id, ret;
    prrte_output_stream_t lds;
    char *string = NULL;

    /* only go thru this once - mpirun calls it twice, which causes
     * any error messages to show up twice
     */
    if (passed_thru) {
        return PRRTE_SUCCESS;
    }
    passed_thru = true;

    /*
     * This string is going to be used in prrte/util/stacktrace.c
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
            -1
        };
        for (j = 0 ; signals[j] != -1 ; ++j) {
            if (j == 0) {
                prrte_asprintf(&string, "%d", signals[j]);
            } else {
                char *tmp;
                prrte_asprintf(&tmp, "%s,%d", string, signals[j]);
                free(string);
                string = tmp;
            }
        }

        prrte_signal_string = string;
        ret = prrte_mca_base_var_register ("prrte", "prrte", NULL, "signal",
                                     "Comma-delimited list of integer signal numbers to PRRTE to attempt to intercept.  Upon receipt of the intercepted signal, PRRTE will display a stack trace and abort.  PRRTE will *not* replace signals if handlers are already installed by the time MPI_INIT is invoked.  Optionally append \":complain\" to any signal number in the comma-delimited list to make PRRTE complain if it detects another signal handler (and therefore does not insert its own).",
                                     PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                     PRRTE_INFO_LVL_3, PRRTE_MCA_BASE_VAR_SCOPE_LOCAL,
                                     &prrte_signal_string);
        free (string);
        if (0 > ret) {
            return ret;
        }
    }

    /*
     * Where should the stack trace output be directed
     * This string is going to be used in prrte/util/stacktrace.c
     */
    string = strdup("stderr");
    prrte_stacktrace_output_filename = string;
    ret = prrte_mca_base_var_register ("prrte", "prrte", NULL, "stacktrace_output",
                                 "Specifies where the stack trace output stream goes.  "
                                 "Accepts one of the following: none (disabled), stderr (default), stdout, file[:filename].   "
                                 "If 'filename' is not specified, a default filename of 'stacktrace' is used.  "
                                 "The 'filename' is appended with either '.PID' or '.RANK.PID', if RANK is available.  "
                                 "The 'filename' can be an absolute path or a relative path to the current working directory.",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_LOCAL,
                                 &prrte_stacktrace_output_filename);
    free (string);
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
    prrte_net_private_ipv4 = "10.0.0.0/8;172.16.0.0/12;192.168.0.0/16;169.254.0.0/16";
    ret = prrte_mca_base_var_register ("prrte", "prrte", "net", "private_ipv4",
                                 "Semicolon-delimited list of CIDR notation entries specifying what networks are considered \"private\" (default value based on RFC1918 and RFC3330)",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_3, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prrte_net_private_ipv4);
    if (0 > ret) {
        return ret;
    }

    prrte_set_max_sys_limits = NULL;
    ret = prrte_mca_base_var_register ("prrte", "prrte", NULL, "set_max_sys_limits",
                                 "Set the specified system-imposed limits to the specified value, including \"unlimited\"."
                                 "Supported params: core, filesize, maxmem, openfiles, stacksize, maxchildren",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_3, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prrte_set_max_sys_limits);
    if (0 > ret) {
        return ret;
    }

    prrte_abort_delay = 0;
    ret = prrte_mca_base_var_register("prrte", "prrte", NULL, "abort_delay",
                                "If nonzero, print out an identifying message when abort operation is invoked (hostname, PID of the process that called abort) and delay for that many seconds before exiting (a negative delay value means to never abort).  This allows attaching of a debugger before quitting the job.",
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                 PRRTE_INFO_LVL_5,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                 &prrte_abort_delay);
    if (0 > ret) {
        return ret;
    }

    prrte_abort_print_stack = false;
    ret = prrte_mca_base_var_register("prrte", "prrte", NULL, "abort_print_stack",
                                 "If nonzero, print out a stack trace when abort is invoked",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                /* If we do not have stack trace
                                   capability, make this a constant
                                   MCA variable */
#if PRRTE_WANT_PRETTY_PRINT_STACKTRACE
                                 0,
                                 PRRTE_INFO_LVL_5,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
#else
                                 PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY,
                                 PRRTE_INFO_LVL_5,
                                 PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
#endif
                                 &prrte_abort_print_stack);
    if (0 > ret) {
        return ret;
    }

    /* register the envar-forwarding params */
    (void)prrte_mca_base_var_register ("prrte", "mca", "base", "env_list",
                                 "Set SHELL env variables",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_env_list);

    prrte_mca_base_env_list_sep = PRRTE_MCA_BASE_ENV_LIST_SEP_DEFAULT;
    (void)prrte_mca_base_var_register ("prrte", "mca", "base", "env_list_delimiter",
                                 "Set SHELL env variables delimiter. Default: semicolon ';'",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_env_list_sep);

    /* Set OMPI_MCA_mca_base_env_list variable, it might not be set before
     * if mca variable was taken from amca conf file. Need to set it
     * here because mca_base_var_process_env_list is called from schizo_ompi.c
     * only when this env variable was set.
     */
    if (NULL != prrte_mca_base_env_list) {
        char *name = NULL;
        (void) prrte_mca_base_var_env_name ("prrte_mca_base_env_list", &name);
        if (NULL != name) {
            prrte_setenv(name, prrte_mca_base_env_list, false, &environ);
            free(name);
        }
    }

    /* Register internal MCA variable mca_base_env_list_internal. It can be set only during
     * parsing of amca conf file and contains SHELL env variables specified via -x there.
     * Its format is the same as for mca_base_env_list.
     */
    (void)prrte_mca_base_var_register ("prrte", "mca", "base", "env_list_internal",
            "Store SHELL env variables from amca conf file",
            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRRTE_INFO_LVL_3,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_env_list_internal);

    /* dss has parameters */
    ret = prrte_dss_register_vars ();
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    /* get a clean output channel too - need to do this here because
     * we use it below, and prun and some other tools call this
     * function prior to calling prrte_init
     */
    PRRTE_CONSTRUCT(&lds, prrte_output_stream_t);
    lds.lds_want_stdout = true;
    prrte_clean_output = prrte_output_open(&lds);
    PRRTE_DESTRUCT(&lds);

    prrte_help_want_aggregate = true;
    (void) prrte_mca_base_var_register ("prrte", "prrte", "base", "help_aggregate",
                                  "If prrte_base_help_aggregate is true, duplicate help messages will be aggregated rather than displayed individually.  This can be helpful for parallel jobs that experience multiple identical failures; rather than print out the same help/failure message N times, display it once with a count of how many processes sent the same message.",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_help_want_aggregate);

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
    prrte_tmpdir_base = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "tmpdir_base",
                                  "Base of the session directory tree to be used by all processes",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_tmpdir_base);

    prrte_local_tmpdir_base = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "local_tmpdir_base",
                                  "Base of the session directory tree to be used by prun/mpirun",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_local_tmpdir_base);

    prrte_remote_tmpdir_base = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "remote_tmpdir_base",
                                  "Base of the session directory tree on remote nodes, if required to be different from head node",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_remote_tmpdir_base);

    /* if a global tmpdir was specified, then we do not allow specification
     * of the local or remote values to avoid confusion
     */
    if (NULL != prrte_tmpdir_base &&
        (NULL != prrte_local_tmpdir_base || NULL != prrte_remote_tmpdir_base)) {
        prrte_output(prrte_clean_output,
                    "------------------------------------------------------------------\n"
                    "The MCA param prrte_tmpdir_base was specified, which sets the base\n"
                    "of the temporary directory tree for all procs. However, values for\n"
                    "the local and/or remote tmpdir base were also given. This can lead\n"
                    "to confusion and is therefore not allowed. Please specify either a\n"
                    "global tmpdir base OR a local/remote tmpdir base value\n"
                    "------------------------------------------------------------------");
        exit(1);
    }

    if (NULL != prrte_tmpdir_base) {
        if (NULL != prrte_process_info.tmpdir_base) {
            free(prrte_process_info.tmpdir_base);
        }
        prrte_process_info.tmpdir_base = strdup (prrte_tmpdir_base);
    } else if (PRRTE_PROC_IS_MASTER && NULL != prrte_local_tmpdir_base) {
        /* prun will pickup the value for its own use */
        if (NULL != prrte_process_info.tmpdir_base) {
            free(prrte_process_info.tmpdir_base);
        }
        prrte_process_info.tmpdir_base = strdup (prrte_local_tmpdir_base);
    } else if (PRRTE_PROC_IS_DAEMON && NULL != prrte_remote_tmpdir_base) {
        /* prun will pickup the value and forward it along, but must not
         * use it in its own work. So only a daemon needs to get it, and the
         * daemon will pass it down to its application procs. Note that prun
         * will pass -its- value to any procs local to it
         */
        if (NULL != prrte_process_info.tmpdir_base) {
            free(prrte_process_info.tmpdir_base);
        }
        prrte_process_info.tmpdir_base = strdup (prrte_remote_tmpdir_base);
    }

    prrte_top_session_dir = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "top_session_dir",
                                  "Top of the session directory tree for applications",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_top_session_dir);

    if (NULL != prrte_top_session_dir) {
         if (NULL != prrte_process_info.top_session_dir) {
            free(prrte_process_info.top_session_dir);
        }
        prrte_process_info.top_session_dir = strdup(prrte_top_session_dir);
    }

    prrte_jobfam_session_dir = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "jobfam_session_dir",
                                  "The jobfamily session directory for applications",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                  &prrte_jobfam_session_dir);

    if (NULL != prrte_jobfam_session_dir) {
        if (NULL != prrte_process_info.jobfam_session_dir) {
            free(prrte_process_info.jobfam_session_dir);
        }
        prrte_process_info.jobfam_session_dir = strdup(prrte_jobfam_session_dir);
    }

    prrte_prohibited_session_dirs = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "no_session_dirs",
                                  "Prohibited locations for session directories (multiple locations separated by ',', default=NULL)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_prohibited_session_dirs);

    prrte_create_session_dirs = true;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "create_session_dirs",
                                  "Create session directories",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_create_session_dirs);

    prrte_execute_quiet = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "execute_quiet",
                                  "Do not output error and help messages",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_execute_quiet);

    prrte_report_silent_errors = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "report_silent_errors",
                                  "Report all errors, including silent ones",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_report_silent_errors);

    prrte_debug_flag = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "debug",
                                  "Top-level PRRTE debug switch (default: false)",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_debug_flag);

    prrte_debug_verbosity = -1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "debug_verbose",
                                  "Verbosity level for PRRTE debug messages (default: 1)",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_debug_verbosity);

    prrte_debug_daemons_file_flag = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "debug_daemons_file",
                                  "Whether want stdout/stderr of daemons to go to a file or not",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_debug_daemons_file_flag);
    /* If --debug-daemons-file was specified, that also implies
       --debug-daemons */
    if (prrte_debug_daemons_file_flag) {
        prrte_debug_daemons_flag = true;

        /* value can't change */
        (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "debug_daemons",
                                      "Whether to debug the PRRTE daemons or not",
                                      PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                      PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                      &prrte_debug_daemons_flag);
    } else {
        prrte_debug_daemons_flag = false;

        (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "debug_daemons",
                                      "Whether to debug the PRRTE daemons or not",
                                      PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                      PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                      &prrte_debug_daemons_flag);
    }

    prrte_progress_thread_debug_level = -1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "progress_thread_debug",
                                  "Debug level for PRRTE progress threads",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_progress_thread_debug_level);

    if (0 <= prrte_progress_thread_debug_level) {
        prrte_progress_thread_debug = prrte_output_open(NULL);
        prrte_output_set_verbosity(prrte_progress_thread_debug,
                                  prrte_progress_thread_debug_level);
    }

    /* do we want session output left open? */
    prrte_leave_session_attached = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "leave_session_attached",
                                  "Whether applications and/or daemons should leave their sessions "
                                  "attached so that any output can be received - this allows X forwarding "
                                  "without all the attendant debugging output",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_leave_session_attached);

    /* if any debug level is set, ensure we output debug level dumps */
    if (prrte_debug_flag || prrte_debug_daemons_flag || prrte_leave_session_attached) {
        prrte_devel_level_output = true;
    }

    prrte_do_not_launch = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "do_not_launch",
                                  "Perform all necessary operations to prepare to launch the application, but do not actually launch it",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_do_not_launch);

    prted_debug_failure = PRRTE_VPID_INVALID;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "daemon_fail",
                                  "Have the specified prted fail after init for debugging purposes",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prted_debug_failure);

    prted_debug_failure_delay = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "daemon_fail_delay",
                                  "Have the specified prted fail after specified number of seconds (default: 0 => no delay)",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prted_debug_failure_delay);

    prrte_startup_timeout = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "startup_timeout",
                                  "Seconds to wait for startup or job launch before declaring failed_to_start (default: 0 => do not check)",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_startup_timeout);

    /* default hostfile */
    prrte_default_hostfile = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "default_hostfile",
                                  "Name of the default hostfile (relative or absolute path, \"none\" to ignore environmental or default MCA param setting)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_default_hostfile);

    if (NULL == prrte_default_hostfile) {
        /* nothing was given, so define the default */
        prrte_asprintf(&prrte_default_hostfile, "%s/prrte-default-hostfile", prrte_install_dirs.sysconfdir);
        /* flag that nothing was given */
        prrte_default_hostfile_given = false;
    } else if (0 == strcmp(prrte_default_hostfile, "none")) {
        free (prrte_default_hostfile);
        prrte_default_hostfile = NULL;
        /* flag that it was given */
        prrte_default_hostfile_given = true;
    } else {
        /* flag that it was given */
        prrte_default_hostfile_given = true;
    }

    /* default dash-host */
    prrte_default_dash_host = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "default_dash_host",
                                  "Default -host setting (specify \"none\" to ignore environmental or default MCA param setting)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_default_dash_host);
    if (NULL != prrte_default_dash_host &&
        0 == strcmp(prrte_default_dash_host, "none")) {
        free(prrte_default_dash_host);
        prrte_default_dash_host = NULL;
    }

    prrte_hostname_cutoff = 1000;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "hostname_cutoff",
                                  "Pass hostnames to all procs when #nodes is less than cutoff [default:1000]",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_3, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_hostname_cutoff);

    /* which alias to use in MPIR_proctab */
    prrte_use_hostname_alias = 1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "hostname_alias_index",
                                  "Which alias to use for the debugger proc table [default: 1st alias]",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_use_hostname_alias);

    prrte_xml_output = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "xml_output",
                                  "Display all output in XML format (default: false)",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_xml_output);

    /* whether to tag output */
    /* if we requested xml output, be sure to tag the output as well */
    prrte_tag_output = prrte_xml_output;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "tag_output",
                                  "Tag all output with [job,rank] (default: false)",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_tag_output);
    if (prrte_xml_output) {
        prrte_tag_output = true;
    }


    prrte_xml_file = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "xml_file",
                                  "Provide all output in XML format to the specified file",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_xml_file);
    if (NULL != prrte_xml_file) {
        if (PRRTE_PROC_IS_MASTER && NULL == prrte_xml_fp) {
            /* only the HNP opens this file! Make sure it only happens once */
            prrte_xml_fp = fopen(prrte_xml_file, "w");
            if (NULL == prrte_xml_fp) {
                prrte_output(0, "Could not open specified xml output file: %s", prrte_xml_file);
                return PRRTE_ERROR;
            }
        }
        /* ensure we set the flags to tag output */
        prrte_xml_output = true;
        prrte_tag_output = true;
    } else {
        /* default to stdout */
        prrte_xml_fp = stdout;
    }

    /* whether to timestamp output */
    prrte_timestamp_output = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "timestamp_output",
                                  "Timestamp all application process output (default: false)",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_timestamp_output);

    prrte_show_resolved_nodenames = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "show_resolved_nodenames",
                                  "Display any node names that are resolved to a different name (default: false)",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_show_resolved_nodenames);

    /* allow specification of the launch agent */
    prrte_launch_agent = "prted";
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "launch_agent",
                                  "Command used to start processes on remote nodes (default: prted)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_launch_agent);

    prrte_fork_agent_string = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "fork_agent",
                                  "Command used to fork processes on remote nodes (default: NULL)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_fork_agent_string);

    if (NULL != prrte_fork_agent_string) {
        prrte_fork_agent = prrte_argv_split(prrte_fork_agent_string, ' ');
    }

    /* whether or not to require RM allocation */
    prrte_allocation_required = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "allocation_required",
                                  "Whether or not an allocation by a resource manager is required [default: no]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_allocation_required);

    /* whether or not to map stddiag to stderr */
    prrte_map_stddiag_to_stderr = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "map_stddiag_to_stderr",
                                  "Map output from prrte_output to stderr of the local process [default: no]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_map_stddiag_to_stderr);

    /* whether or not to map stddiag to stderr */
    prrte_map_stddiag_to_stdout = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "map_stddiag_to_stdout",
                                  "Map output from prrte_output to stdout of the local process [default: no]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_map_stddiag_to_stdout);
    if( prrte_map_stddiag_to_stderr && prrte_map_stddiag_to_stdout ) {
        prrte_output(0, "The options \"prrte_map_stddiag_to_stderr\" and \"prrte_map_stddiag_to_stdout\" are mutually exclusive. They cannot both be set to true.");
        return PRRTE_ERROR;
    }

    /* generate new terminal windows to display output from specified ranks */
    prrte_xterm = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "xterm",
                                  "Create a new xterm window and display output from the specified ranks there [default: none]",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_xterm);
    if (NULL != prrte_xterm) {
        /* if an xterm request is given, we have to leave any ssh
         * sessions attached so the xterm window manager can get
         * back to the controlling terminal
         */
        prrte_leave_session_attached = true;
        /* also want to redirect stddiag output from prrte_output
         * to stderr from the process so those messages show
         * up in the xterm window instead of being forwarded to mpirun
         */
        prrte_map_stddiag_to_stderr = true;
    }

    /* whether or not to report launch progress */
    prrte_report_launch_progress = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "report_launch_progress",
                                  "Output a brief periodic report on launch progress [default: no]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_report_launch_progress);

    /* cluster hardware info detected by prrte only */
    prrte_local_cpu_type = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "cpu_type",
                                  "cpu type detected in node",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_local_cpu_type);

    prrte_local_cpu_model = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "cpu_model",
                                  "cpu model detected in node",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_local_cpu_model);

    /* tool communication controls */
    prrte_report_events_uri = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "report_events",
                                  "URI to which events are to be reported (default: NULL)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_report_events_uri);
    if (NULL != prrte_report_events_uri) {
        prrte_report_events = true;
    }

    /* barrier control */
    prrte_do_not_barrier = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "do_not_barrier",
                                  "Do not barrier in prrte_init",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_do_not_barrier);

    prrte_enable_recovery = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "enable_recovery",
                                  "Enable recovery from process failure [Default = disabled]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_enable_recovery);

    prrte_max_restarts = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "max_restarts",
                                  "Max number of times to restart a failed process",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_max_restarts);

    if (!prrte_enable_recovery && prrte_max_restarts != 0) {
        if (PRRTE_PROC_IS_MASTER) {
            prrte_output(prrte_clean_output,
                        "------------------------------------------------------------------\n"
                        "The MCA param prrte_enable_recovery was not set to true, but\n"
                        "a value was provided for the number of restarts:\n\n"
                        "Max restarts: %d\n"
                        "We are enabling process recovery and continuing execution. To avoid\n"
                        "this warning in the future, please set the prrte_enable_recovery\n"
                        "param to non-zero.\n"
                        "------------------------------------------------------------------",
                        prrte_max_restarts);
        }
        prrte_enable_recovery = true;
    }

    prrte_abort_non_zero_exit = true;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "abort_on_non_zero_status",
                                  "Abort the job if any process returns a non-zero exit status - no restart in such cases",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_abort_non_zero_exit);

    prrte_allowed_exit_without_sync = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "allowed_exit_without_sync",
                                  "Process exiting without calling finalize will not trigger job termination",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_allowed_exit_without_sync);

    prrte_report_child_jobs_separately = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "report_child_jobs_separately",
                                  "Return the exit status of the primary job only",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_report_child_jobs_separately);

    prrte_stat_history_size = 1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "stat_history_size",
                                  "Number of stat samples to keep",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_stat_history_size);

    prrte_max_vm_size = -1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "max_vm_size",
                                  "Maximum size of virtual machine - used to subdivide allocation",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_max_vm_size);

    if (prrte_hwloc_use_hwthreads_as_cpus) {
        prrte_set_slots = "hwthreads";
    } else {
        prrte_set_slots = "cores";
    }
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "set_default_slots",
                                  "Set the number of slots on nodes that lack such info to the"
                                  " number of specified objects [a number, \"cores\" (default),"
                                  " \"numas\", \"sockets\", \"hwthreads\" (default if hwthreads_as_cpus is set),"
                                  " or \"none\" to skip this option]",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_set_slots);

    /* should we display the allocation after determining it? */
    prrte_display_allocation = false;
    id = prrte_mca_base_var_register ("prrte", "prrte", NULL, "display_alloc",
                                "Whether to display the allocation after it is determined",
                                PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &prrte_display_allocation);
    /* register a synonym for old name -- should we remove this now? */
    prrte_mca_base_var_register_synonym (id, "prrte", "ras", "base", "display_alloc", PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* should we display a detailed (developer-quality) version of the allocation after determining it? */
    prrte_devel_level_output = false;
    id = prrte_mca_base_var_register ("prrte", "prrte", NULL, "display_devel_alloc",
                                "Whether to display a developer-detail allocation after it is determined",
                                PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &prrte_devel_level_output);
    /* register a synonym for old name -- should we remove this now? */
    prrte_mca_base_var_register_synonym (id, "prrte", "ras", "base", "display_devel_alloc", PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    if (prrte_devel_level_output) {
        prrte_display_allocation = true;
    }

    /* should we treat any -host directives as "soft" - i.e., desired
     * but not required
     */
    prrte_soft_locations = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "soft_locations",
                                  "Treat -host directives as desired, but not required",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_soft_locations);

    /* allow specification of the cores to be used by daemons */
    prrte_daemon_cores = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "daemon_cores",
                                  "Restrict the PRRTE daemons (including mpirun) to operate on the specified cores (comma-separated list of ranges)",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_5, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_daemon_cores);

    /* Amount of time to wait for a stack trace to return from the daemons */
    prrte_stack_trace_wait_timeout = 30;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "timeout_for_stack_trace",
                                  "Seconds to wait for stack traces to return before terminating "
                                  "the job (<= 0 wait forever)",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_stack_trace_wait_timeout);

    /* register the URI of the UNIVERSAL data server */
    prrte_data_server_uri = NULL;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "server_uri",
                                  "URI of a session-level keyval server for publish/lookup operations",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                  PRRTE_INFO_LVL_3, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_data_server_uri);

    prrte_mca_base_var_register("prrte", "prrte", NULL, "pmix_verbose",
                          "Verbosity for PRRTE-level PMIx code",
                          PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                          PRRTE_INFO_LVL_5,
                          PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                          &prrte_pmix_verbose_output);

    return PRRTE_SUCCESS;
}
