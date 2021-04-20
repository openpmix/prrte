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

#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif

#include "src/util/argv.h"
#include "src/util/keyval_parse.h"
#include "src/util/name_fns.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/prte_environ.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"

#include "schizo_ompi.h"
#include "src/mca/schizo/base/base.h"

static int define_cli(prte_cmd_line_t *cli);
static int parse_cli(int argc, int start, char **argv, char ***target);
static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv);
static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline);
static int detect_proxy(char *argv);
static void allow_run_as_root(prte_cmd_line_t *cmd_line);
static void job_info(prte_cmd_line_t *cmdline, void *jobinfo);
static int check_sanity(prte_cmd_line_t *cmd_line);

prte_schizo_base_module_t prte_schizo_ompi_module = {.name = "ompi",
                                                     .define_cli = define_cli,
                                                     .parse_cli = parse_cli,
                                                     .parse_deprecated_cli = parse_deprecated_cli,
                                                     .parse_env = parse_env,
                                                     .detect_proxy = detect_proxy,
                                                     .allow_run_as_root = allow_run_as_root,
                                                     .job_info = job_info,
                                                     .check_sanity = check_sanity};

static prte_cmd_line_init_t ompi_cmd_line_init[] = {
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

    /* mpirun options */
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
    {'\0', "omca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass context-specific OMPI MCA parameters; they are considered global if --gmca is not used "
     "and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "gomca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass global OMPI MCA parameters that are applicable to all contexts (arg0 is the parameter "
     "name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
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
    /* fwd mpirun port */
    {'\0', "fwd-mpirun-port", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Forward mpirun port to compute node daemons so all will use it", PRTE_CMD_LINE_OTYPE_DVM},

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
    {'\0', "default-hostfile", 1, PRTE_CMD_LINE_TYPE_STRING, "Provide a default hostfile",
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
    {'\0', "with-ft", 1, PRTE_CMD_LINE_TYPE_STRING,
     "Specify the type(s) of error handling that the application will use.",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
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

static int define_cli(prte_cmd_line_t *cli)
{
    int rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: define_cli", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRTE_ERR_BAD_PARAM;
    }

    rc = prte_cmd_line_add(cli, ompi_cmd_line_init);
    return rc;
}

static int convert_deprecated_cli(char *option, char ***argv, int i)
{
    char **pargs, *p2, *modifier;
    int rc = PRTE_SUCCESS;

    pargs = *argv;

    /* --nolocal -> --map-by :nolocal */
    if (0 == strcmp(option, "--nolocal")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "NOLOCAL", true);
    }
    /* --oversubscribe -> --map-by :OVERSUBSCRIBE
     * --nooversubscribe -> --map-by :NOOVERSUBSCRIBE
     */
    else if (0 == strcmp(option, "--oversubscribe") || 0 == strcmp(option, "--nooversubscribe")) {
        if (0 == strcmp(option, "--nooversubscribe")) {
            prte_show_help("help-schizo-base.txt", "deprecated-inform", true, option,
                           "This is the default behavior so does not need to be specified");
            modifier = "NOOVERSUBSCRIBE";
        } else {
            modifier = "OVERSUBSCRIBE";
        }
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, modifier, true);
    }
    /* --use-hwthread-cpus -> --bind-to hwthread */
    else if (0 == strcmp(option, "--use-hwthread-cpus")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "hwthread", NULL, true);
    }
    /* --cpu-set and --cpu-list -> --map-by pe-list:X
     */
    else if (0 == strcmp(option, "--cpu-set") || 0 == strcmp(option, "--cpu-list")) {
        prte_asprintf(&p2, "PE-LIST=%s", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2, true);
        free(p2);
    }
    /* --bind-to-core and --bind-to-socket -> --bind-to X */
    else if (0 == strcmp(option, "--bind-to-core")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "core", NULL, true);
    } else if (0 == strcmp(option, "--bind-to-socket")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "socket", NULL, true);
    }
    /* --bynode -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bynode")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "node", NULL, true);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    /* --bycore -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bycore")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "core", NULL, true);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    /* --byslot -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--byslot")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "slot", NULL, true);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    /* --cpus-per-proc/rank X -> --map-by :pe=X */
    else if (0 == strcmp(option, "--cpus-per-proc") || 0 == strcmp(option, "--cpus-per-rank")) {
        prte_asprintf(&p2, "pe=%s", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2, true);
        free(p2);
    }
    /* -N ->   map-by ppr:N:node */
    else if (0 == strcmp(option, "-N")) {
        prte_asprintf(&p2, "ppr:%s:node", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    }
    /* --npernode X and --npersocket X -> --map-by ppr:X:node/socket */
    else if (0 == strcmp(option, "--npernode")) {
        prte_asprintf(&p2, "ppr:%s:node", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    } else if (0 == strcmp(option, "--pernode")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "ppr:1:node", NULL, true);
    } else if (0 == strcmp(option, "--npersocket")) {
        prte_asprintf(&p2, "ppr:%s:socket", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    }
    /* --ppr X -> --map-by ppr:X */
    else if (0 == strcmp(option, "--ppr")) {
        /* if they didn't specify a complete pattern, then this is an error */
        if (NULL == strchr(pargs[i + 1], ':')) {
            prte_show_help("help-schizo-base.txt", "bad-ppr", true, pargs[i + 1], true);
            return PRTE_ERR_BAD_PARAM;
        }
        prte_asprintf(&p2, "ppr:%s", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL, true);
        free(p2);
    }
    /* --am[ca] X -> --tune X */
    else if (0 == strcmp(option, "--amca") || 0 == strcmp(option, "--am")) {
        rc = prte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL, true);
    }
    /* --tune X -> aggregate */
    else if (0 == strcmp(option, "--tune")) {
        rc = prte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL, true);
    }
    /* --rankfile X -> map-by rankfile:file=X */
    else if (0 == strcmp(option, "--rankfile")) {
        prte_asprintf(&p2, "file=%s", pargs[i + 1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", "rankfile", p2, false);
        free(p2);
        rc = PRTE_ERR_SILENT;
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
    /* --display-devel-map  -> --display allocation-devel */
    else if (0 == strcmp(option, "--display-devel-map")) {
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

    return rc;
}

static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv)
{
    pmix_status_t rc;

    char *options[] = {"--nolocal",
                       "--oversubscribe",
                       "--nooversubscribe",
                       "--use-hwthread-cpus",
                       "--cpu-set",
                       "--cpu-list",
                       "--bind-to-core",
                       "--bind-to-socket",
                       "--bynode",
                       "--bycore",
                       "--byslot",
                       "--cpus-per-proc",
                       "--cpus-per-rank",
                       "-N",
                       "--npernode",
                       "--pernode",
                       "--npersocket",
                       "--ppr",
                       "--amca",
                       "--am",
                       "--rankfile",
                       "--display-devel-map",
                       "--display-map",
                       "--display-topo",
                       "--display-diff",
                       "--report-bindings",
                       "--display-allocation",
                       "--tag-output",
                       "--timestamp-output",
                       "--xml",
                       "--output-proctable",
                       NULL};

    rc = prte_schizo_base_process_deprecated_cli(cmdline, argc, argv, options,
                                                 convert_deprecated_cli);

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
        prte_argv_append_nosize(c1, p1);
        prte_argv_append_nosize(c2, p2);
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

    tokens = prte_argv_split(env_list, (int) sep);
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

    prte_argv_free(tokens);
    return rc;
}

static int process_tune_files(char *filename, char ***dstenv, char sep)
{
    FILE *fp;
    char **tmp, **opts, *line, *param, *p1, *p2;
    int i, n, rc = PRTE_SUCCESS;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;

    tmp = prte_argv_split(filename, sep);
    if (NULL == tmp) {
        return PRTE_SUCCESS;
    }

    /* Iterate through all the files passed in -- it is an ERROR if
     * a given param appears more than once with different values */

    for (i = 0; NULL != tmp[i]; i++) {
        fp = fopen(tmp[i], "r");
        if (NULL == fp) {
            /* if the file given wasn't absolute, check in the default location */
            if (prte_path_is_absolute(tmp[i])) {
                prte_show_help("help-schizo-base.txt", "missing-param-file", true, tmp[i], p1);
                prte_argv_free(tmp);
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
                return PRTE_ERR_NOT_FOUND;
            }
            p1 = prte_os_path(false, DEFAULT_PARAM_FILE_PATH, tmp[i], NULL);
            fp = fopen(p1, "r");
            if (NULL == fp) {
                prte_show_help("help-schizo-base.txt", "missing-param-file-def", true, tmp[i],
                               DEFAULT_PARAM_FILE_PATH);
                prte_argv_free(tmp);
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
                free(p1);
                return PRTE_ERR_NOT_FOUND;
            }
            free(p1);
        }
        while (NULL != (line = prte_schizo_base_getline(fp))) {
            if ('\0' == line[0])
                continue; /* skip empty lines */
            opts = prte_argv_split_with_empty(line, ' ');
            if (NULL == opts) {
                prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                free(line);
                prte_argv_free(tmp);
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
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
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
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
                            prte_argv_free(tmp);
                            prte_argv_free(opts);
                            prte_argv_free(cache);
                            prte_argv_free(cachevals);
                            prte_argv_free(xparams);
                            prte_argv_free(xvals);
                            fclose(fp);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        p2 = prte_schizo_base_strip_quotes(opts[n + 3]);
                        prte_asprintf(&param, "%s=%s", p1, p2);
                        free(p1);
                        free(p2);
                        p1 = param;
                        ++n; // need an extra step
                    }
                    rc = process_envar(p1, &xparams, &xvals);
                    free(p1);
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    ++n; // skip over the envar option
                } else if (0 == strcmp(opts[n], "--mca")) {
                    if (NULL == opts[n + 1] || NULL == opts[n + 2]) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        free(line);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
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
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    n += 2; // skip over the MCA option
                } else if (0
                           == strncmp(opts[n], "mca_base_env_list", strlen("mca_base_env_list"))) {
                    /* find the equal sign */
                    p1 = strchr(opts[n], '=');
                    if (NULL == p1) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        free(line);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    ++p1;
                    rc = process_env_list(p1, &xparams, &xvals, ';');
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                } else {
                    rc = process_token(opts[n], &cache, &cachevals);
                    if (PRTE_SUCCESS != rc) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i],
                                       line);
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                }
            }
            free(line);
        }
        fclose(fp);
    }

    prte_argv_free(tmp);

    if (NULL != cache) {
        /* add the results into dstenv */
        for (i = 0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
        prte_argv_free(cache);
        prte_argv_free(cachevals);
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
    if (0 == strncmp("opal_", p1, strlen("opal_")) || 0 == strncmp("ompi_", p1, strlen("ompi_"))) {
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

static int parse_cli(int argc, int start, char **argv, char ***target)
{
    char *p1, *p2;
    int i;
    pmix_status_t rc;
    bool takeus;
    char *param = NULL;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output, "%s schizo:ompi: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    for (i = 0; i < (argc - start); ++i) {
        if (0 == strcmp("--omca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);
            if (NULL == target) {
                /* push it into our environment */
                asprintf(&param, "OMPI_MCA_%s", p1);
                prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                    "%s schizo:ompi:parse_cli pushing %s into environment",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                prte_setenv(param, p2, true, &environ);
            } else {
                prte_argv_append_nosize(target, "--omca");
                prte_argv_append_nosize(target, p1);
                prte_argv_append_nosize(target, p2);
            }
            free(p1);
            free(p2);
            i += 2;
            continue;
        }
        if (0 == strcmp("--mca", argv[i])) {
            if (NULL == argv[i + 1] || NULL == argv[i + 2]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            p2 = prte_schizo_base_strip_quotes(argv[i + 2]);

            /* this is a generic MCA designation, so see if the parameter it
             * refers to belongs to one of our frameworks */
            takeus = check_generic(p1);
            if (takeus) {
                if (NULL == target) {
                    /* push it into our environment */
                    prte_asprintf(&param, "OMPI_MCA_%s", p1);
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:ompi:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, p2, true, &environ);
                } else {
                    prte_argv_append_nosize(target, "--omca");
                    prte_argv_append_nosize(target, p1);
                    prte_argv_append_nosize(target, p2);
                }
                free(p1);
                free(p2);
                i += 2;
                continue;
            }
        }
        if (0 == strcmp("--map-by", argv[i])) {
            /* if they set "inherit", then make this the default for prte */
            if (NULL != strcasestr(argv[i + 1], "inherit")
                && NULL == strcasestr(argv[i + 1], "noinherit")) {
                if (NULL == target) {
                    /* push it into our environment */
                    prte_setenv("PRTE_MCA_rmaps_default_inherit", "1", true, &environ);
                    prte_setenv("PRTE_MCA_rmaps_default_mapping_policy", argv[i + 1], true,
                                &environ);
                } else {
                    prte_argv_append_nosize(target, "--prtemca");
                    prte_argv_append_nosize(target, "rmaps_default_inherit");
                    prte_argv_append_nosize(target, "1");
                    prte_argv_append_nosize(target, "--prtemca");
                    prte_argv_append_nosize(target, "rmaps_default_mapping_policy");
                    prte_argv_append_nosize(target, argv[i + 1]);
                }
            }
        }

#if PRTE_ENABLE_FT
        if (0 == strcmp("--with-ft", argv[i]) || 0 == strcmp("-with-ft", argv[i])) {
            if (NULL == argv[i + 1]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = prte_schizo_base_strip_quotes(argv[i + 1]);
            if (0 != strcmp("no", p1) && 0 != strcmp("false", p1) && 0 != strcmp("0", p1)) {
                if (NULL == target) {
                    /* push it into our environment */
                    prte_asprintf(&param, "PRTE_MCA_prte_enable_ft");
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:ompi:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, "true", true, &environ);
                    // prte_enable_ft = true;
                    prte_enable_recovery = true;
                    prte_asprintf(&param, "OMPI_MCA_mpi_ft_enable");
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                        "%s schizo:ompi:parse_cli pushing %s into environment",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, "true", true, &environ);
                } else {
                    prte_argv_append_nosize(target, "--prtemca");
                    prte_argv_append_nosize(target, "prte_enable_ft");
                    prte_argv_append_nosize(target, "true");
                    prte_argv_append_nosize(target, "--enable-recovery");
                    prte_argv_append_nosize(target, "--mca");
                    prte_argv_append_nosize(target, "mpi_ft_enable");
                    prte_argv_append_nosize(target, "true");
                }
            }
            free(p1);
        }
#endif
    }

    rc = prte_schizo_base_parse_prte(argc, start, argv, target);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prte_schizo_base_parse_pmix(argc, start, argv, target);

    return rc;
}

static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline)
{
    char *p1, *p2;
    char *env_set_flag;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;
    char **envlist = NULL, **envtgt = NULL;
    prte_value_t *pval;
    int i, j, rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output, "%s schizo:ompi: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if they are filling out a cmd line, then we don't
     * have anything to contribute */
    if (cmdline) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    /* Begin by examining the environment as the cmd line trumps all */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (NULL != env_set_flag) {
        rc = process_env_list(env_set_flag, &xparams, &xvals, ';');
        if (PRTE_SUCCESS != rc) {
            prte_argv_free(xparams);
            prte_argv_free(xvals);
            return rc;
        }
    }
    /* process the resulting cache into the dstenv */
    if (NULL != xparams) {
        for (i = 0; NULL != xparams[i]; i++) {
            prte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prte_argv_free(xparams);
        xparams = NULL;
        prte_argv_free(xvals);
        xvals = NULL;
    }

    /* now process any tune file specification - the tune file processor
     * will police itself for duplicate values */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "tune", 0, 0))) {
        p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
        rc = process_tune_files(p1, dstenv, ',');
        free(p1);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "initial-errhandler", 0, 0))) {
        p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
        rc = check_cache(&cache, &cachevals, "mpi_initial_errhandler", p1);
        free(p1);
        if (PRTE_SUCCESS != rc) {
            prte_argv_free(cache);
            prte_argv_free(cachevals);
            return rc;
        }
    }

    if (prte_cmd_line_is_taken(cmd_line, "display-comm")
        && prte_cmd_line_is_taken(cmd_line, "display-comm-finalize")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_init,mpi_finalize", true, dstenv);
    } else if (prte_cmd_line_is_taken(cmd_line, "display-comm")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_init", true, dstenv);
    } else if (prte_cmd_line_is_taken(cmd_line, "display-comm-finalize")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_finalize", true, dstenv);
    }

    /* now look for any "--mca" options - note that it is an error
     * for the same MCA param to be given more than once if the
     * values differ */
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "omca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "omca", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "omca", i, 1);
            p2 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "gomca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "gomca", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "gomca", i, 1);
            p2 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "mca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "mca", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "mca", i, 1);
            p2 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* check if this is one of ours */
            if (check_generic(p1)) {
                /* treat mca_base_env_list as a special case */
                if (0 == strcmp(p1, "mca_base_env_list")) {
                    prte_argv_append_nosize(&envlist, p2);
                    free(p1);
                    free(p2);
                    continue;
                }
                rc = check_cache(&cache, &cachevals, p1, p2);
                free(p1);
                free(p2);
                if (PRTE_SUCCESS != rc) {
                    prte_argv_free(cache);
                    prte_argv_free(cachevals);
                    prte_argv_free(envlist);
                    return rc;
                }
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "gmca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "gmca", i, 0);
            p1 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* check if this is one of ours */
            if (!check_generic(p1)) {
                free(p1);
                continue;
            }
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "gmca", i, 1);
            p2 = prte_schizo_base_strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(envlist);
                return rc;
            }
        }
    }

    /* if we got any env lists, process them here */
    if (NULL != envlist) {
        for (i = 0; NULL != envlist[i]; i++) {
            envtgt = prte_argv_split(envlist[i], ';');
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
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(envtgt);
                        prte_argv_free(envlist);
                        return rc;
                    }
                } else {
                    *p2 = '\0';
                    rc = check_cache(&xparams, &xvals, envtgt[j], p2 + 1);
                    if (PRTE_SUCCESS != rc) {
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(envtgt);
                        prte_argv_free(envlist);
                        return rc;
                    }
                }
            }
            prte_argv_free(envtgt);
        }
    }
    prte_argv_free(envlist);

    /* now look for -x options - not allowed to conflict with a -mca option */
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "x"))) {
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
                    free(p1);
                    continue;
                }
            }
            /* not allowed to duplicate anything from an MCA param on the cmd line */
            rc = check_cache_noadd(&cache, &cachevals, p1, p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                free(p1);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
                return rc;
            }
            /* cache this for later inclusion */
            prte_argv_append_nosize(&xparams, p1);
            prte_argv_append_nosize(&xvals, p2);
            free(p1);
        }
    }

    /* process the resulting cache into the dstenv */
    if (NULL != cache) {
        for (i = 0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
    }
    prte_argv_free(cache);
    prte_argv_free(cachevals);

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

static int detect_proxy(char *cmdpath)
{
    char *evar;
    char *inipath = NULL;

    prte_output_verbose(2, prte_schizo_base_framework.framework_output,
                        "%s[%s]: detect proxy with %s (%s)", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        __FILE__, cmdpath, prte_tool_basename);

    if (NULL == cmdpath) {
        return 0;
    }
    /* if this isn't a full path, then it is a list
     * of personalities we need to check */
    if (!prte_path_is_absolute(cmdpath)) {
        /* if it contains "ompi", then we are available */
        if (NULL != strstr(cmdpath, "ompi")) {
            return 100;
        }
        return 0;
    }

    /* look for the OMPIHOME envar to tell us where OMPI
     * was installed */
    evar = getenv("OMPIHOME");
    if (NULL != evar) {
        inipath = prte_os_path(false, evar, "ompi.ini", NULL);
        if (prte_schizo_base_check_ini(cmdpath, inipath)) {
            free(inipath);
            return 100;
        }
        free(inipath);

        inipath = prte_os_path(false, evar, "open-mpi.ini", NULL);
        if (prte_schizo_base_check_ini(cmdpath, inipath)) {
            free(inipath);
            return 100;
        }
        free(inipath);

        /* if the executable is in the OMPIHOME path, then
         * it belongs to us - however, we exclude explicit
         * calls to "prte" as that is intended to start
         * the DVM */
        if (NULL != strstr(cmdpath, evar) && 0 != strcmp(prte_tool_basename, "prte")) {
            return 100;
        }
    }

    /* we may not have an ini file, or perhaps they don't
     * have OMPIHOME set - but it still could be an MPI
     * proxy, so let's check */
    /* if the basename of the cmd was "mpirun" or "mpiexec",
     * we default to us */
    if (prte_schizo_base.test_proxy_launch || 0 == strcmp(prte_tool_basename, "mpirun")
        || 0 == strcmp(prte_tool_basename, "mpiexec")
        || 0 == strcmp(prte_tool_basename, "oshrun")) {
        return prte_schizo_ompi_component.priority;
    }

    /* if none of those were true, then it cannot be us */
    return 0;
}

static void allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    /* we always run last */
    char *r1, *r2;

    if (prte_cmd_line_is_taken(cmd_line, "allow-run-as-root")) {
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

static void job_info(prte_cmd_line_t *cmdline, void *jobinfo)
{
    prte_value_t *pval;
    uint16_t u16;
    pmix_status_t rc;

    if (NULL != (pval = prte_cmd_line_get_param(cmdline, "stream-buffering", 0, 0))) {
        u16 = pval->value.data.integer;
        if (0 != u16 && 1 != u16 && 2 != u16) {
            /* bad value */
            prte_show_help("help-schizo-base.txt", "bad-stream-buffering-value", true,
                           pval->value.data.integer);
            return;
        }
        PMIX_INFO_LIST_ADD(rc, jobinfo, "OMPI_STREAM_BUFFERING", &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
}

static int check_sanity(prte_cmd_line_t *cmd_line)
{
    return PRTE_SUCCESS;
}
