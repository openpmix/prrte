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
#include "src/util/pmix_keyval_parse.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"

#include "schizo_hydra.h"
#include "src/mca/schizo/base/base.h"

static int define_cli(prte_cmd_line_t *cli);
static int check_help(prte_cmd_line_t *cli, char **argv);
static int parse_cli(int argc, int start, char **argv, char ***target);
static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv);
static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline);
static int detect_proxy(char *argv);
static void allow_run_as_root(prte_cmd_line_t *cmd_line);
static void job_info(prte_cmd_line_t *cmdline, void *jobinfo);
static int check_sanity(prte_cmd_line_t *cmd_line);

prte_schizo_base_module_t prte_schizo_hydra_module = {
    .name = "hydra",
    .define_cli = define_cli,
    .check_help = check_help,
    .parse_cli = parse_cli,
    .parse_deprecated_cli = parse_deprecated_cli,
    .parse_env = parse_env,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info,
    .check_sanity = check_sanity
};

static prte_cmd_line_init_t hydra_cmd_line_init[] = {
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
     "Pass context-specific HYDRA MCA parameters; they are considered global if --gmca is not used "
     "and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
     PRTE_CMD_LINE_OTYPE_LAUNCH},
    {'\0', "gomca", 2, PRTE_CMD_LINE_TYPE_STRING,
     "Pass global HYDRA MCA parameters that are applicable to all contexts (arg0 is the parameter "
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

    {'\0', "debug", 0, PRTE_CMD_LINE_TYPE_BOOL,
     "Top-level PRTE debug switch (default: false) "
     "This CLI option will be deprecated starting in Open MPI v5",
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
        "Comma-delimited list of options that control how output is generated."
        "Allowed values: tag, timestamp, xml, merge-stderr-to-stdout, dir=DIRNAME, file=filename."
        " The dir option redirects output from application processes into DIRNAME/job/rank/std[out,err,diag]."
        " The file option redirects output from application processes into filename.rank.[out,err,diag]."
        " If merge is specified, the dir and file options will put both stdout and stderr into a"
        " file with the \"out\" suffix. In both cases, "
        "the provided name will be converted to an absolute path. Supported qualifiers include NOCOPY"
        " (do not copy the output to the stdout/err streams).",
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
     "Process binding",
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
                        "%s schizo:hydra: define_cli", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRTE_ERR_BAD_PARAM;
    }

    rc = prte_cmd_line_add(cli, hydra_cmd_line_init);
    return rc;
}

typedef struct {
    char *option;
    char *description;
} options_t;

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
    "\n\nHydra specific options (treated as global):\n"
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
    "\n  Other Hydra options:\n"
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

static int check_help(prte_cmd_line_t *cli, char **argv)
{
    size_t n;
    char *option;

    if (pmix_cmd_line_is_taken(cli, "help")) {
        fprintf(stdout, "\nUsage: %s [global opts] [local opts for exec1] [exec1] [exec1 args] :"
                        "[local opts for exec2] [exec2] [exec2 args] : ...\n%s",
                prte_tool_basename, genhelp);
        /* If someone asks for help, that should be all we do */
        return PRTE_ERR_SILENT;
    } else {
        /* check if an option was given that has a value of "--help" as
         * that indicates a request for option-specific help */
        option = NULL;
        for (n=1; NULL != argv[n]; n++) {
            if (0 == strcmp(argv[n], "--help") ||
                0 == strcmp(argv[n], "-help")) {
                /* the argv before this one must be the option they
                 * are seeking help about */
                option = argv[n-1];
                break;
            }
        }
        if (NULL != option) {
            /* see if more detailed help message is available
             * for this option */
            for (n=0; NULL != opthelp[n].option; n++) {
                if (0 == strcmp(opthelp[n].option, option)) {
                    fprintf(stdout, "\n%s", opthelp[n].description);
                    break;
                }
            }
            return PRTE_ERR_SILENT;
        }
    }

    return PRTE_SUCCESS;
}

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

static int convert_deprecated_cli(char *option, char ***argv, int i)
{
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
}

static int parse_deprecated_cli(prte_cmd_line_t *cmdline, int *argc, char ***argv)
{
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
}

static int parse_cli(int argc, int start, char **argv, char ***target)
{
    int i;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:hydra: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    for (i = 0; i < (argc - start); i++) {
         if (0 == strcmp("--map-by", argv[i])) {
            /* if they set "inherit", then make this the default for prte */
            if (NULL != strcasestr(argv[i + 1], "inherit")
                && NULL == strcasestr(argv[i + 1], "noinherit")) {
                if (NULL == target) {
                    /* push it into our environment */
                    pmix_setenv("PRTE_MCA_rmaps_default_inherit", "1", true, &environ);
                    pmix_setenv("PRTE_MCA_rmaps_default_mapping_policy", argv[i + 1], true,
                                &environ);
                } else {
                    pmix_argv_append_nosize(target, "--prtemca");
                    pmix_argv_append_nosize(target, "rmaps_default_inherit");
                    pmix_argv_append_nosize(target, "1");
                    pmix_argv_append_nosize(target, "--prtemca");
                    pmix_argv_append_nosize(target, "rmaps_default_mapping_policy");
                    pmix_argv_append_nosize(target, argv[i + 1]);
                }
            }
             break;
        }
    }

    return PRTE_SUCCESS;
}

static int parse_env(prte_cmd_line_t *cmd_line, char **srcenv, char ***dstenv, bool cmdline)
{
    char *p1, *p2;
    prte_value_t *pval;
    int i, j;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:hydra: parse_env",
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
            pmix_setenv(p1, p2, true, dstenv);
            free(p1);
            free(p2);
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

    /* COMMAND-LINE OVERRRIDES ALL */
    /* this is a list of personalities we need to check -
     * if it contains "hydra" or "mpich", then we are available */
    if (NULL != personalities) {
        if (NULL != strstr(personalities, "hydra") ||
            NULL != strstr(personalities, "mpich")) {
            return 100;
        }
        return 0;
    }

    /* if we were told the proxy, then use it */
    if (NULL != (evar = getenv("PRTE_MCA_schizo_proxy"))) {
        if (0 == strcmp(evar, "hydra") ||
            0 == strcmp(evar, "mpich")) {
            return 100;
        } else {
            return 0;
        }
    }

    /* if neither of those were true, then it cannot be us */
    return 0;
}

static void allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    /* hydra always allows run-as-root */
    return;
}

static void job_info(prte_cmd_line_t *cmdline, void *jobinfo)
{
}

static int check_sanity(prte_cmd_line_t *cmd_line)
{
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

    return PRTE_SUCCESS;
}
