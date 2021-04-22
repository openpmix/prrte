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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/prte_environ.h"
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

static int define_cli(prte_cmd_line_t *cli);
static int parse_cli(int argc, int start, char **argv, char ***target);
static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv);
static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline);
static int setup_fork(prte_job_t *jdata, prte_app_context_t *context);
static int detect_proxy(char *argv);
static void allow_run_as_root(prte_cmd_line_t *cmd_line);
static int check_sanity(prte_cmd_line_t *cmd_line);
static void job_info(prte_cmd_line_t *cmdline, void *jobinfo);

prte_schizo_base_module_t prte_schizo_prte_module = {.name = "prte",
                                                     .define_cli = define_cli,
                                                     .parse_cli = parse_cli,
                                                     .parse_deprecated_cli = parse_deprecated_cli,
                                                     .parse_env = parse_env,
                                                     .setup_fork = setup_fork,
                                                     .detect_proxy = detect_proxy,
                                                     .allow_run_as_root = allow_run_as_root,
                                                     .check_sanity = check_sanity,
                                                     .job_info = job_info};

static prte_cmd_line_init_t prte_cmd_line_init[] = {
    /* basic options */
    {'h', "help", 0, PRTE_CMD_LINE_TYPE_BOOL, "This help message", PRTE_CMD_LINE_OTYPE_GENERAL},
    {'V', "version", 0, PRTE_CMD_LINE_TYPE_BOOL, "Print version and exit",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'v', "verbose", 0, PRTE_CMD_LINE_TYPE_BOOL, "Be verbose", PRTE_CMD_LINE_OTYPE_GENERAL},
    {'q', "quiet", 0, PRTE_CMD_LINE_TYPE_BOOL, "Suppress helpful messages",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'\0', "parsable", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "When used in conjunction with other parameters, the output is displayed in a "
     "machine-parsable format",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'\0', "parseable", 0, PRTE_CMD_LINE_TYPE_BOOL, "Synonym for --parsable",
     PRTE_CMD_LINE_OTYPE_GENERAL},

    /* prterun options */
    /* Specify the launch agent to be used */
    {'\0', "launch-agent", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Name of daemon executable used to start processes on remote nodes (default: prted)",
     PRTE_CMD_LINE_OTYPE_DVM},
    /* maximum size of VM - typically used to subdivide an allocation */
    {'\0', "max-vm-size", 1, PRTE_CMD_LINE_TYPE_INT, "Number of daemons to start",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "debug-daemons", 0, PRTE_CMD_LINE_TYPE_BOOL, "Debug daemons", PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "debug-daemons-file", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Enable debugging of any PRTE daemons used by this application, storing output in files",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "leave-session-attached", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Do not discard stdout/stderr of remote PRTE daemons", PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "tmpdir", 1, PRTE_CMD_LINE_TYPE_STRING, "Set the root for the session directory tree",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "prefix", 1, PRTE_CMD_LINE_TYPE_STRING, "Prefix to be used to look for RTE executables",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "noprefix", 0, PRTE_CMD_LINE_TYPE_BOOL, "Disable automatic --prefix behavior",
     PRTE_CMD_LINE_OTYPE_DVM},

    /* setup MCA parameters */
    {'\0', "mca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass context-specific MCA parameters; they are considered global if --gmca is not used and "
     "only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    /* setup MCA parameters */
    {'\0', "prtemca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass context-specific PRTE MCA parameters; they are considered global if --gmca is not used "
     "and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "pmixmca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass context-specific PMIx MCA parameters; they are considered global if --gmca is not used "
     "and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "gpmixmca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass global PMIx MCA parameters that are applicable to all contexts (arg0 is the parameter "
     "name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "tune", 1, PRTE_CMD_LINE_TYPE_STRING,
     "File(s) containing MCA params for tuning DVM operations", PRTE_CMD_LINE_OTYPE_DVM},

    /* forward signals */
    {'\0', "forward-signals", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Comma-delimited list of additional signals (names or integers) to forward to "
     "application processes [\"none\" => forward nothing]. Signals provided by "
     "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
     PRTE_CMD_LINE_OTYPE_DVM},

    {'\0', "debug", 0, PRTE_CMD_LINE_TYPE_BOOL, "Top-level PRTE debug switch (default: false)",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "debug-verbose", 1, PRTE_CMD_LINE_TYPE_INT,
     "Verbosity level for PRTE debug messages (default: 1)", PRTE_CMD_LINE_OTYPE_DEBUG},
    {'d', "debug-devel", 0, PRTE_CMD_LINE_TYPE_BOOL, "Enable debugging of PRTE",
     PRTE_CMD_LINE_OTYPE_DEBUG},

    {'\0', "timeout", 1, PRTE_CMD_LINE_TYPE_INT,
     "Timeout the job after the specified number of seconds", PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "report-state-on-timeout", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Report all job and process states upon timeout", PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "get-stack-traces", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Get stack traces of all application procs on timeout", PRTE_CMD_LINE_OTYPE_DEBUG},

    {'\0', "allow-run-as-root", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Allow execution as root (STRONGLY DISCOURAGED)", PRTE_CMD_LINE_OTYPE_DVM}, /* End of list */

    /* Conventional options - for historical compatibility, support
     * both single and multi dash versions */
    /* Number of processes; -c, -n, --n, -np, and --np are all
     synonyms */
    {'c', "np", 1, PRTE_CMD_LINE_TYPE_INT, "Number of processes to run",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'n', "n", 1, PRTE_CMD_LINE_TYPE_INT, "Number of processes to run",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    {'N', NULL, 1, PRTE_CMD_LINE_TYPE_INT, "Number of processes to run per node",
     PRTE_CMD_LINE_OTYPE_GENERAL},
    /* Use an appfile */
    {'\0', "app", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Provide an appfile; ignore all other command line options", PRTE_CMD_LINE_OTYPE_GENERAL},

    /* output options */
    {'\0', "output", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Comma-delimited list of options that control the output generated."
     "Allowed values: tag, timestamp, xml, merge-stderr-to-stdout, dir:DIRNAME",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    /* exit status reporting */
    {'\0', "report-child-jobs-separately", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Return the exit status of the primary job only", PRTE_CMD_LINE_OTYPE_OUTPUT},
    /* select XML output */
    {'\0', "xml", 0, PRTE_CMD_LINE_TYPE_BOOL, "Provide all output in XML format",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    /* tag output */
    {'\0', "tag-output", 0, PRTE_CMD_LINE_TYPE_BOOL, "Tag all output with [job,rank]",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "timestamp-output", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Timestamp all application process output", PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "output-directory", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Redirect output from application processes into filename/job/rank/std[out,err,diag]. A "
     "relative path value will be converted to an absolute path. The directory name may include a "
     "colon followed by a comma-delimited list of optional case-insensitive directives. Supported "
     "directives currently include NOJOBID (do not include a job-id directory level) and NOCOPY "
     "(do not copy the output to the stdout/err streams)",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "output-filename", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Redirect output from application processes into filename.rank. A relative path value will be "
     "converted to an absolute path. The directory name may include a colon followed by a "
     "comma-delimited list of optional case-insensitive directives. Supported directives currently "
     "include NOCOPY (do not copy the output to the stdout/err streams)",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "merge-stderr-to-stdout", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Merge stderr to stdout for each process", PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "xterm", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Create a new xterm window and display output from the specified ranks there",
     PRTE_CMD_LINE_OTYPE_OUTPUT},
    {'\0', "stream-buffering", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Adjust buffering for stdout/stderr [0 unbuffered] [1 line buffered] [2 fully buffered]",
     PRTE_CMD_LINE_OTYPE_LAUNCH},

    /* input options */
    /* select stdin option */
    {'\0', "stdin", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Specify procs to receive stdin [rank, all, none] (default: 0, indicating rank 0)",
     PRTE_CMD_LINE_OTYPE_INPUT},

    /* debugger options */
    {'\0', "output-proctable", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Print the complete proctable to stdout [-], stderr [+], or a file [anything else] after "
     "launch",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "stop-on-exec", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "If supported, stop each process at start of execution", PRTE_CMD_LINE_OTYPE_DEBUG},

    /* launch options */
    /* Preload the binary on the remote machine */
    {'s', "preload-binary", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Preload the binary on the remote machine before starting the remote process.",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    /* Preload files on the remote machine */
    {'\0', "preload-files", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Preload the comma separated list of files to the remote machines current working directory "
     "before starting the remote process.",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    /* Export environment variables; potentially used multiple times,
     so it does not make sense to set into a variable */
    {'x', NULL, 1, PRTE_CMD_LINE_TYPE_STRING,
     "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the "
     "environment variable foo and takes its value from the current environment; \"-x foo=bar\" "
     "exports the environment variable name foo and sets its value to \"bar\" in the started "
     "processes; \"-x foo*\" exports all current environmental variables starting with \"foo\")",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "wdir", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Set the working directory of the started processes", PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "wd", 1, PRTE_CMD_LINE_TYPE_STRING, "Synonym for --wdir", PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "set-cwd-to-session-dir", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Set the working directory of the started processes to their session directory",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "path", 1, PRTE_CMD_LINE_TYPE_STRING,
     "PATH to be used to look for executables to start processes", PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "show-progress", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Output a brief periodic report on launch progress", PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "pset", 1, PRTE_CMD_LINE_TYPE_STRING,
     "User-specified name assigned to the processes in their given application",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    /* Set a hostfile */
    {'\0', "hostfile", 1, PRTE_CMD_LINE_TYPE_STRING, "Provide a hostfile",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "machinefile", 1, PRTE_CMD_LINE_TYPE_STRING, "Provide a hostfile",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'H', "host", 1, PRTE_CMD_LINE_TYPE_STRING, "List of hosts to invoke processes on",
     PRTE_CMD_LINE_OTYPE_LAUNCH},

    /* placement options */
    /* Mapping options */
    {'\0', "map-by", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Mapping Policy for job [slot | hwthread | core (default:np<=2) | l1cache | "
     "l2cache | l3cache | package (default:np>2) | node | seq | dist | ppr |,"
     "rankfile]"
     " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
     "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
     "DEVICE(for dist policy), INHERIT, NOINHERIT, PE-LIST=a,b (comma-delimited "
     "ranges of cpus to use for this job), FILE=<path> for seq and rankfile options",
     PRTE_CMD_LINE_OTYPE_MAPPING},

    /* Ranking options */
    {'\0', "rank-by", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Ranking Policy for job [slot (default:np<=2) | hwthread | core | l1cache "
     "| l2cache | l3cache | package (default:np>2) | node], with modifier :SPAN or :FILL",
     PRTE_CMD_LINE_OTYPE_RANKING},

    /* Binding options */
    {'\0', "bind-to", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Binding policy for job. Allowed values: none, hwthread, core, l1cache, l2cache, "
     "l3cache, package, (\"none\" is the default when oversubscribed, \"core\" is "
     "the default when np<=2, and \"package\" is the default when np>2). Allowed colon-delimited "
     "qualifiers: "
     "overload-allowed, if-supported",
     PRTE_CMD_LINE_OTYPE_BINDING},

    /* rankfile */
    {'\0', "rankfile", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Name of file to specify explicit task mapping", PRTE_CMD_LINE_OTYPE_LAUNCH},

    /* display options */
    {'\0', "display", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Comma-delimited list of options for displaying information about the allocation and job."
     "Allowed values: allocation, bind, map, map-devel, topo",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    /* developer options */
    {'\0', "do-not-launch", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Perform all necessary operations to prepare to launch the application, but do not actually "
     "launch it (usually used to test mapping patterns)",
     PRTE_CMD_LINE_OTYPE_DEVEL},
    {'\0', "display-devel-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display a detailed process map (mostly intended for developers) just before launch",
     PRTE_CMD_LINE_OTYPE_DEVEL},
    {'\0', "display-topo", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display the topology as part of the process map (mostly intended for developers) just before "
     "launch",
     PRTE_CMD_LINE_OTYPE_DEVEL},
    {'\0', "report-bindings", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Whether to report process bindings to stderr", PRTE_CMD_LINE_OTYPE_DEVEL},
    {'\0', "display-devel-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display a detailed list (mostly intended for developers) of the allocation being used by "
     "this job",
     PRTE_CMD_LINE_OTYPE_DEVEL},
    {'\0', "display-map", 0, PRTE_CMD_LINE_TYPE_BOOL, "Display the process map just before launch",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "display-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display the allocation being used by this job", PRTE_CMD_LINE_OTYPE_DEBUG},

#if PRTE_ENABLE_FT
    {'\0', "enable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Enable recovery from process failure [Default = disabled]", PRTE_CMD_LINE_OTYPE_FT},
    {'\0', "max-restarts", 1, PRTE_CMD_LINE_TYPE_INT,
     "Max number of times to restart a failed process", PRTE_CMD_LINE_OTYPE_FT},
    {'\0', "disable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Disable recovery (resets all recovery options to off)", PRTE_CMD_LINE_OTYPE_FT},
    {'\0', "continuous", 0, PRTE_CMD_LINE_TYPE_BOOL, "Job is to run until explicitly terminated",
     PRTE_CMD_LINE_OTYPE_FT},
#endif

    /* mpiexec mandated form launch key parameters */
    {'\0', "initial-errhandler", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Specify the initial error handler that is attached to predefined communicators during the "
     "first MPI call.",
     PRTE_CMD_LINE_OTYPE_LAUNCH},

    /* Display Commumication Protocol : MPI_Init */
    {'\0', "display-comm", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display table of communication methods between ranks during MPI_Init",
     PRTE_CMD_LINE_OTYPE_GENERAL},

    /* Display Commumication Protocol : MPI_Finalize */
    {'\0', "display-comm-finalize", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Display table of communication methods between ranks during MPI_Finalize",
     PRTE_CMD_LINE_OTYPE_GENERAL},

    /* End of list */
    {'\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL}};

static prte_cmd_line_init_t prte_dvm_cmd_line_init[] = {
    /* do not print a "ready" message */
    {'\0', "no-ready-msg", 0, PRTE_CMD_LINE_TYPE_BOOL, "Do not print a DVM ready message",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "daemonize", 0, PRTE_CMD_LINE_TYPE_BOOL, "Daemonize the DVM daemons into the background",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "system-server", 0, PRTE_CMD_LINE_TYPE_BOOL, "Start the DVM as the system server",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "set-sid", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Direct the DVM daemons to separate from the current session", PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "report-pid", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Printout pid on stdout [-], stderr [+], or a file [anything else]", PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "report-uri", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Printout URI on stdout [-], stderr [+], or a file [anything else]", PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "test-suicide", 1, PRTE_CMD_LINE_TYPE_BOOL, "Suicide instead of clean abort after delay",
     PRTE_CMD_LINE_OTYPE_DEBUG},
    {'\0', "default-hostfile", 1, PRTE_CMD_LINE_TYPE_STRING, "Provide a default hostfile",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "singleton", 1, PRTE_CMD_LINE_TYPE_STRING, "ID of the singleton process that started us",
     PRTE_CMD_LINE_OTYPE_DVM},
    {'\0', "keepalive", 1, PRTE_CMD_LINE_TYPE_INT,
     "Pipe to monitor - DVM will terminate upon closure", PRTE_CMD_LINE_OTYPE_DVM},

    /* End of list */
    {'\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL}};

static int define_cli(prte_cmd_line_t *cli)
{
    int rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:prte: define_cli", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRTE_ERR_BAD_PARAM;
    }

    rc = prte_cmd_line_add(cli, prte_cmd_line_init);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    if (PRTE_PROC_IS_MASTER) {
        rc = prte_cmd_line_add(cli, prte_dvm_cmd_line_init);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

static int parse_cli(int argc, int start, char **argv, char ***target)
{
    pmix_status_t rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output, "%s schizo:prte: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    rc = prte_schizo_base_parse_prte(argc, start, argv, target);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prte_schizo_base_parse_pmix(argc, start, argv, target);

    return rc;
}

static int convert_deprecated_cli(char *option, char ***argv, int i)
{
    int rc = PRTE_ERR_NOT_FOUND;
    char **pargs = *argv;
    char *p1, *p2, *tmp, *tmp2, *output;

    /* --display-devel-map  -> --display allocation-devel */
    if (0 == strcmp(option, "--display-devel-map")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "map-devel", true);
    }
    /* --output-proctable  ->  --display map-devel */
    else if (0 == strcmp(option, "--output-proctable")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "map-devel", true);
    }
    /* --display-map  ->  --display map */
    else if (0 == strcmp(option, "--display-map")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "map", true);
    }
    /* --display-topo  ->  --display topo */
    else if (0 == strcmp(option, "--display-topo")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "topo", true);
    }
    /* --report-bindings  ->  --display bind */
    else if (0 == strcmp(option, "--report-bindings")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "bind", true);
    }
    /* --display-allocation  ->  --display allocation */
    else if (0 == strcmp(option, "--display-allocation")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--display", NULL, "allocation", true);
    }
    /* --do-not-launch  ->   --map-by :donotlaunch*/
    else if (0 == strcmp(option, "--do-not-launch")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DONOTLAUNCH", true);
    }
    /* --do-not-resolve  ->   --map-by :donotresolve*/
    else if (0 == strcmp(option, "--do-not-resolve")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "DONOTRESOLVE", true);
    }
    /* --tag-output  ->  "--output tag */
    else if (0 == strcmp(option, "--tag-output")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--output", NULL, "tag", true);
    }
    /* --timestamp-output  ->  --output timestamp */
    else if (0 == strcmp(option, "--timestamp-output")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--output", NULL, "timestamp", true);
    }
    /* --output-directory DIR  ->  --output dir:DIR */
    else if (0 == strcmp(option, "--output-directory")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--output", "dir", pargs[i + 1], true);
    }
    /* --xml  ->  --output xml */
    else if (0 == strcmp(option, "--xml")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--output", NULL, "xml", true);
    }
    /* -N ->   map-by ppr:N:node */
    else if (0 == strcmp(option, "-N")) {
        prte_asprintf(&p2, "ppr:%s:node", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    }
    /* --map-by socket ->  --map-by package */
    else if (0 == strcmp(option, "--map-by")) {
        /* if the option consists solely of qualifiers, then add
         * the "core" default value */
        if (':' == pargs[i + 1][0]) {
            prte_asprintf(&p2, "core%s", pargs[i + 1]);
            free(pargs[i + 1]);
            pargs[i + 1] = p2;
            return PRTE_OPERATION_SUCCEEDED;
        }
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i + 1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i + 1]); // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i + 1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                           tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i + 1]);
            pargs[i + 1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }
    /* --rank-by socket ->  --rank-by package */
    else if (0 == strcmp(option, "--rank-by")) {
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i + 1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i + 1]); // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i + 1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                           tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i + 1]);
            pargs[i + 1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }
    /* --bind-to socket ->  --bind-to package */
    else if (0 == strcmp(option, "--bind-to")) {
        /* check the value of the option for "socket" */
        if (0 == strncasecmp(pargs[i + 1], "socket", strlen("socket"))) {
            p1 = strdup(pargs[i + 1]); // save the original option
            /* replace "socket" with "package" */
            if (NULL == (p2 = strchr(pargs[i + 1], ':'))) {
                /* no modifiers */
                tmp = strdup("package");
            } else {
                *p2 = '\0';
                ++p2;
                prte_asprintf(&tmp, "package:%s", p2);
            }
            prte_asprintf(&p2, "%s %s", option, p1);
            prte_asprintf(&tmp2, "%s %s", option, tmp);
            /* can't just call show_help as we want every instance to be reported */
            output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true, p2,
                                           tmp2);
            fprintf(stderr, "%s\n", output);
            free(output);
            free(p1);
            free(p2);
            free(tmp2);
            free(pargs[i + 1]);
            pargs[i + 1] = tmp;
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        rc = PRTE_OPERATION_SUCCEEDED;
    }

    return rc;
}

static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv)
{
    pmix_status_t rc;

    char *options[] = {"--display-devel-map",
                       "--display-map",
                       "--display-topo",
                       "--display-diff",
                       "--report-bindings",
                       "--display-allocation",
                       "--do-not-launch",
                       "--tag-output",
                       "--timestamp-output",
                       "--xml",
                       "-N",
                       "--map-by",
                       "--rank-by",
                       "--bind-to",
                       "--output-proctable",
                       NULL};

    rc = prte_schizo_base_process_deprecated_cli(cmdline, argc, argv, options,
                                                 convert_deprecated_cli);

    return rc;
}

static void doit(char *tgt, char *src, char **srcenv, char ***dstenv, bool cmdline)
{
    char *param, *p1, *value, *tmp;
    char **env = *dstenv;
    size_t n;

    param = strdup(src);
    p1 = param + strlen(tgt);
    value = strchr(param, '=');
    *value = '\0';
    value++;
    /* check for duplicate in app->env - this
     * would have been placed there by the
     * cmd line processor. By convention, we
     * always let the cmd line override the
     * environment
     */
    if (cmdline) {
        /* check if it is already present */
        for (n = 0; NULL != env[n]; n++) {
            if (0 == strcmp(env[n], p1)) {
                /* this param is already given */
                free(param);
                return;
            }
        }
        prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                            "%s schizo:prte:parse_env adding %s %s to cmd line",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), p1, value);
        if (0 == strcmp(tgt, "PMIX_MCA_")) {
            prte_argv_append_nosize(dstenv, "--pmixmca");
        } else {
            prte_argv_append_nosize(dstenv, "--prtemca");
        }
        prte_argv_append_nosize(dstenv, p1);
        prte_argv_append_nosize(dstenv, value);
    } else {
        /* push it into our environment with a PRTE_MCA_ prefix*/
        if (0 == strcmp(tgt, "PMIX_MCA_")) {
            prte_asprintf(&tmp, "PMIX_MCA_%s", p1);
        } else if (0 != strcmp(tgt, "PRTE_MCA_")) {
            prte_asprintf(&tmp, "PRTE_MCA_%s", p1);
        } else {
            tmp = strdup(param);
        }
        if (environ != srcenv) {
            prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                "%s schizo:prte:parse_env pushing %s=%s into my environment",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tmp, value);
            prte_setenv(tmp, value, true, &environ);
        }
        prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                            "%s schizo:prte:parse_env pushing %s=%s into dest environment",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tmp, value);
        prte_setenv(tmp, value, true, dstenv);
        free(tmp);
    }
    free(param);
}

static char *orte_frameworks[] = {"errmgr", "ess",    "filem", "grpcomm", "iof", "odls",
                                  "oob",    "plm",    "ras",   "rmaps",   "rml", "routed",
                                  "rtc",    "schizo", "state", NULL};

static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline)
{
    int i, j, n;
    char *p1, *p2;
    char **env;
    prte_value_t *pval;
    char **xparams = NULL, **xvals = NULL;
    char *param, *value;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output, "%s schizo:prte: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("PRTE_MCA_", srcenv[i], strlen("PRTE_MCA_"))) {
            doit("PRTE_MCA_", srcenv[i], srcenv, dstenv, cmdline);
        } else if (0 == strncmp("OMPI_MCA_", srcenv[i], strlen("OMPI_MCA_"))) {
            /* if this references one of the old ORTE frameworks, then take it here */
            p1 = srcenv[i] + strlen("OMPI_MCA_");
            for (j = 0; NULL != orte_frameworks[j]; j++) {
                if (0 == strncmp(p1, orte_frameworks[j], strlen(orte_frameworks[j]))) {
                    doit("PRTE_MCA_", srcenv[i], srcenv, dstenv, cmdline);
                    break;
                }
            }
        } else if (0 == strncmp("PMIX_MCA_", srcenv[i], strlen("PMIX_MCA_"))) {
            doit("PMIX_MCA_", srcenv[i], srcenv, dstenv, cmdline);
        }
    }

    if (cmdline) {
        /* if we are looking at the cmd line, then we are done */
        return PRTE_SUCCESS;
    }

    env = *dstenv;

    /* now look for -x options - not allowed to conflict with a -mca option */
    if (NULL != cmd_line && 0 < (j = prte_cmd_line_get_ninsts(cmd_line, "x"))) {
        for (i = 0; i < j; ++i) {
            /* the value is the envar */
            pval = prte_cmd_line_get_param(cmd_line, "x", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* if there is an '=' in it, then they are setting a value */
            if (NULL != (p2 = strchr(p1, '='))) {
                *p2 = '\0';
                ++p2;
            } else {
                p2 = getenv(p1);
                if (NULL == p2) {
                    prte_show_help("help-schizo-base.txt", "missing-envar-param", true, p1);
                    free(p1);
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
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
                free(param);
            }

            /* check if we already processed a conflicting -x version with MCA prefix */
            if (NULL != xparams) {
                for (i = 0; NULL != xparams[i]; i++) {
                    if (0 == strncmp("PRTE_MCA_", p1, strlen("PRTE_MCA_"))
                        || 0 == strncmp("OMPI_MCA_", p1, strlen("OMPI_MCA_"))) {
                        /* this is an error - different values */
                        prte_show_help("help-schizo-base.txt", "duplicate-mca-value", true, p1, p2,
                                       xvals[i]);
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
            }

            /* cache this for later inclusion - do not modify dstenv in this loop */
            prte_argv_append_nosize(&xparams, p1);
            prte_argv_append_nosize(&xvals, p2);
            free(p1);
        }
    }

    /* add the -x values */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            prte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prte_argv_free(xparams);
        prte_argv_free(xvals);
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
    prte_setenv("PRTE_LAUNCHED", "1", true, &app->env);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PRTE_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t)
    {
        if (PRTE_JOB_SET_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_JOB_ADD_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_JOB_UNSET_ENVAR == attr->key) {
            prte_unsetenv(attr->data.data.string, &app->env);
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
                    prte_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
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
                    prte_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PRTE_LIST_FOREACH(attr, &app->attributes, prte_attribute_t)
    {
        if (PRTE_APP_SET_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true, &app->env);
        } else if (PRTE_APP_ADD_ENVAR == attr->key) {
            prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, false, &app->env);
        } else if (PRTE_APP_UNSET_ENVAR == attr->key) {
            prte_unsetenv(attr->data.data.string, &app->env);
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
                    prte_asprintf(&p2, "%s%c%s", attr->data.data.envar.value,
                                  attr->data.data.envar.separator, param);
                    *saveptr = '='; // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
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
                    prte_asprintf(&p2, "%s%c%s", param, attr->data.data.envar.separator,
                                  attr->data.data.envar.value);
                    *saveptr = '='; // restore the current envar setting
                    prte_setenv(attr->data.data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '='; // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prte_setenv(attr->data.data.envar.envar, attr->data.data.envar.value, true,
                            &app->env);
            }
        }
    }

    return PRTE_SUCCESS;
}

static int detect_proxy(char *cmdpath)
{
    char *mybasename;

    prte_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        __FILE__, cmdpath, prte_tool_basename);

    /* we are lowest priority, so we will be checked last */
    if (NULL == cmdpath) {
        /* must use us */
        return 100;
    }

    /* if this isn't a full path, then it is a list
     * of personalities we need to check */
    if (!prte_path_is_absolute(cmdpath)) {
        /* if it contains "prte", then we are available */
        if (NULL != strstr(cmdpath, "prte") || NULL != strstr(cmdpath, "prrte")) {
            return 100;
        }
    }

    /* if it is not a symlink and is in our install path,
     * then it belongs to us */
    mybasename = prte_basename(cmdpath);
    if (0 == strcmp(mybasename, prte_tool_basename)
        && NULL != strstr(cmdpath, prte_install_dirs.prefix)) {
        free(mybasename);
        return 100;
    }
    free(mybasename);

    /* we are always the lowest priority */
    return prte_schizo_prte_component.priority;
}

static void allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    char *r1, *r2;

    if (prte_cmd_line_is_taken(cmd_line, "allow-run-as-root")) {
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

static void job_info(prte_cmd_line_t *cmdline, void *jobinfo)
{
    return;
}

static int check_sanity(prte_cmd_line_t *cmd_line)
{
    prte_value_t *pval;
    int n;
    char **args;
    char *mappers[] = {"slot", "hwthread", "core", "l1cache", "l2cache",  "l3cache", "package",
                       "node", "seq",      "dist", "ppr",     "rankfile", NULL};
    char *rankers[] = {"slot",    "hwthread", "core", "l1cache", "l2cache",
                       "l3cache", "package",  "node", NULL};
    char *binders[] = {"none",    "hwthread", "core",    "l1cache",
                       "l2cache", "l3cache",  "package", NULL};
    bool good = false;
    bool hwtcpus = false;

    if (1 < prte_cmd_line_get_ninsts(cmd_line, "map-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "map-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "rank-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "rank-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "bind-to")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "bind-to");
        return PRTE_ERR_SILENT;
    }

    /* quick check that we have valid directives */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "map-by", 0, 0))) {
        if (NULL != strcasestr(pval->value.data.string, "HWTCPUS")) {
            hwtcpus = true;
        }
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            goto rnk;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n = 0; NULL != mappers[n]; n++) {
            if (0 == strcasecmp(args[0], mappers[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "mapping",
                           args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(args);
    }

rnk:
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "rank-by", 0, 0))) {
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            goto bnd;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n = 0; NULL != rankers[n]; n++) {
            if (0 == strcasecmp(args[0], rankers[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "ranking",
                           args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(args);
    }

bnd:
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "bind-to", 0, 0))) {
        /* if it starts with a ':', then these are just modifiers */
        if (':' == pval->value.data.string[0]) {
            return PRTE_SUCCESS;
        }
        args = prte_argv_split(pval->value.data.string, ':');
        good = false;
        for (n = 0; NULL != binders[n]; n++) {
            if (0 == strcasecmp(args[0], binders[n])) {
                good = true;
                break;
            }
        }
        if (!good) {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "binding",
                           args[0]);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        if (0 == strcasecmp(args[0], "HWTHREAD") && !hwtcpus) {
            /* if we are told to bind-to hwt, then we have to be treating
             * hwt's as the allocatable unit */
            prte_show_help("help-prte-rmaps-base.txt", "invalid-combination", true);
            prte_argv_free(args);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(args);
    }

    return PRTE_SUCCESS;
}
