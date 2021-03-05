/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_SCHIZO_OMPI_H_
#define _MCA_SCHIZO_OMPI_H_

#include "prte_config.h"

#include "types.h"

#include "src/mca/base/base.h"
#include "src/mca/schizo/schizo.h"


BEGIN_C_DECLS

typedef struct {
    prte_schizo_base_component_t super;
    int priority;
} prte_schizo_ompi_component_t;

PRTE_MODULE_EXPORT extern prte_schizo_ompi_component_t prte_schizo_ompi_component;
extern prte_schizo_base_module_t prte_schizo_ompi_module;

static prte_cmd_line_init_t ompi_cmd_line_init[] = {
    /* basic options */
    { 'h', "help", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "This help message",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'V', "version", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Print version and exit",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'v', "verbose", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Be verbose", PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'q', "quiet", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Suppress helpful messages", PRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parsable", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "When used in conjunction with other parameters, the output is displayed in a machine-parsable format",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    { '\0', "parseable", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Synonym for --parsable",
        PRTE_CMD_LINE_OTYPE_GENERAL },

    /* mpirun options */
    { '\0', "singleton", 1, PRTE_CMD_LINE_TYPE_STRING,
        "ID of the singleton process that started us",
        PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "keepalive", 1, PRTE_CMD_LINE_TYPE_INT,
        "Pipe to monitor - DVM will terminate upon closure",
        PRTE_CMD_LINE_OTYPE_DVM },
    /* Specify the launch agent to be used */
    { '\0', "launch-agent", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Name of daemon executable used to start processes on remote nodes (default: prted)",
        PRTE_CMD_LINE_OTYPE_DVM },
    /* maximum size of VM - typically used to subdivide an allocation */
    { '\0', "max-vm-size", 1, PRTE_CMD_LINE_TYPE_INT,
        "Number of daemons to start",
        PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "debug-daemons", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Debug daemons",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-daemons-file", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Enable debugging of any PRTE daemons used by this application, storing output in files",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "leave-session-attached", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Do not discard stdout/stderr of remote PRTE daemons",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "tmpdir", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Set the root for the session directory tree",
        PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "prefix", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Prefix to be used to look for RTE executables",
        PRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "noprefix", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Disable automatic --prefix behavior",
        PRTE_CMD_LINE_OTYPE_DVM },

    /* setup MCA parameters */
    { '\0', "omca", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Pass context-specific OMPI MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gomca", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Pass global OMPI MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "mca", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Pass context-specific MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* setup MCA parameters */
    { '\0', "prtemca", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Pass context-specific PRTE MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "tune", 1, PRTE_CMD_LINE_TYPE_STRING,
        "File(s) containing MCA params for tuning DVM operations",
        PRTE_CMD_LINE_OTYPE_DVM },

    /* forward signals */
    { '\0', "forward-signals", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Comma-delimited list of additional signals (names or integers) to forward to "
        "application processes [\"none\" => forward nothing]. Signals provided by "
        "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
        PRTE_CMD_LINE_OTYPE_DVM},

    { '\0', "debug", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Top-level PRTE debug switch (default: false)",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-verbose", 1, PRTE_CMD_LINE_TYPE_INT,
        "Verbosity level for PRTE debug messages (default: 1)",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { 'd', "debug-devel", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Enable debugging of PRTE",
        PRTE_CMD_LINE_OTYPE_DEBUG },

    { '\0', "timeout", 1, PRTE_CMD_LINE_TYPE_INT,
        "Timeout the job after the specified number of seconds",
        PRTE_CMD_LINE_OTYPE_DEBUG },
#if PMIX_NUMERIC_VERSION >= 0x00040000
    { '\0', "report-state-on-timeout", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Report all job and process states upon timeout",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "get-stack-traces", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Get stack traces of all application procs on timeout",
        PRTE_CMD_LINE_OTYPE_DEBUG },
#endif


    { '\0', "allow-run-as-root", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Allow execution as root (STRONGLY DISCOURAGED)",
        PRTE_CMD_LINE_OTYPE_DVM },    /* End of list */
    /* fwd mpirun port */
    { '\0', "fwd-mpirun-port", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Forward mpirun port to compute node daemons so all will use it",
        PRTE_CMD_LINE_OTYPE_DVM },

    /* Conventional options - for historical compatibility, support
     * both single and multi dash versions */
    /* Number of processes; -c, -n, --n, -np, and --np are all
     synonyms */
    { 'c', "np", 1, PRTE_CMD_LINE_TYPE_INT,
        "Number of processes to run",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'n', "n", 1, PRTE_CMD_LINE_TYPE_INT,
        "Number of processes to run",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    { 'N', NULL, 1, PRTE_CMD_LINE_TYPE_INT,
        "Number of processes to run per node",
        PRTE_CMD_LINE_OTYPE_GENERAL },
    /* Use an appfile */
    { '\0',  "app", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Provide an appfile; ignore all other command line options",
        PRTE_CMD_LINE_OTYPE_GENERAL },

    /* output options */
    /* exit status reporting */
    { '\0', "report-child-jobs-separately", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Return the exit status of the primary job only",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    /* select XML output */
    { '\0', "xml", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Provide all output in XML format",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    /* tag output */
    { '\0', "tag-output", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Tag all output with [job,rank]",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "timestamp-output", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Timestamp all application process output",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-directory", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Redirect output from application processes into filename/job/rank/std[out,err,diag]. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOJOBID (do not include a job-id directory level) and NOCOPY (do not copy the output to the stdout/err streams)",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "output-filename", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Redirect output from application processes into filename.rank. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOCOPY (do not copy the output to the stdout/err streams)",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "merge-stderr-to-stdout", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Merge stderr to stdout for each process",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "xterm", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Create a new xterm window and display output from the specified ranks there",
        PRTE_CMD_LINE_OTYPE_OUTPUT },
    { '\0', "stream-buffering", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Adjust buffering for stdout/stderr [0 unbuffered] [1 line buffered] [2 fully buffered]",
        PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* input options */
    /* select stdin option */
    { '\0', "stdin", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Specify procs to receive stdin [rank, all, none] (default: 0, indicating rank 0)",
        PRTE_CMD_LINE_OTYPE_INPUT },

    /* debugger options */
    { '\0', "output-proctable", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Print the complete proctable to stdout [-], stderr [+], or a file [anything else] after launch",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "stop-on-exec", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "If supported, stop each process at start of execution",
        PRTE_CMD_LINE_OTYPE_DEBUG },

    /* launch options */
    /* request that argv[0] be indexed */
    { '\0', "index-argv-by-rank", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Uniquely index argv[0] for each process using its rank",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload the binary on the remote machine */
    { 's', "preload-binary", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Preload the binary on the remote machine before starting the remote process.",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Preload files on the remote machine */
    { '\0', "preload-files", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Preload the comma separated list of files to the remote machines current working directory before starting the remote process.",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Export environment variables; potentially used multiple times,
     so it does not make sense to set into a variable */
    { 'x', NULL, 1, PRTE_CMD_LINE_TYPE_STRING,
        "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the environment variable foo and takes its value from the current environment; \"-x foo=bar\" exports the environment variable name foo and sets its value to \"bar\" in the started processes; \"-x foo*\" exports all current environmental variables starting with \"foo\")",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wdir", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Set the working directory of the started processes",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "wd", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Synonym for --wdir",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "set-cwd-to-session-dir", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Set the working directory of the started processes to their session directory",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "path", 1, PRTE_CMD_LINE_TYPE_STRING,
        "PATH to be used to look for executables to start processes",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "show-progress", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Output a brief periodic report on launch progress",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "pset", 1, PRTE_CMD_LINE_TYPE_STRING,
        "User-specified name assigned to the processes in their given application",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    /* Set a hostfile */
    { '\0', "hostfile", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Provide a hostfile",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "machinefile", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Provide a hostfile",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "default-hostfile", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Provide a default hostfile",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { 'H', "host", 1, PRTE_CMD_LINE_TYPE_STRING,
        "List of hosts to invoke processes on",
        PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* placement options */
    /* Mapping options */
    { '\0', "map-by", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Mapping Policy for job [slot | hwthread | core (default:np<=2) | l1cache | "
        "l2cache | l3cache | package (default:np>2) | node | seq | dist | ppr |,"
        "rankfile]"
        " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
        "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
        "DEVICE(for dist policy), INHERIT, NOINHERIT, PE-LIST=a,b (comma-delimited "
        "ranges of cpus to use for this job), FILE=<path> for seq and rankfile options",
        PRTE_CMD_LINE_OTYPE_MAPPING },

    /* Ranking options */
    { '\0', "rank-by", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Ranking Policy for job [slot (default:np<=2) | hwthread | core | l1cache "
        "| l2cache | l3cache | package (default:np>2) | node], with modifier :SPAN or :FILL",
        PRTE_CMD_LINE_OTYPE_RANKING },

    /* Binding options */
    { '\0', "bind-to", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Binding policy for job. Allowed values: none, hwthread, core, l1cache, l2cache, "
        "l3cache, package, (\"none\" is the default when oversubscribed, \"core\" is "
        "the default when np<=2, and \"package\" is the default when np>2). Allowed colon-delimited qualifiers: "
        "overload-allowed, if-supported",
        PRTE_CMD_LINE_OTYPE_BINDING },

    /* rankfile */
    { '\0', "rankfile", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Name of file to specify explicit task mapping",
        PRTE_CMD_LINE_OTYPE_LAUNCH },


    /* developer options */
    { '\0', "do-not-resolve", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Do not attempt to resolve interfaces - usually used to determine proposed process placement/binding prior to obtaining an allocation",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "do-not-launch", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Perform all necessary operations to prepare to launch the application, but do not actually launch it (usually used to test mapping patterns)",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display a detailed process map (mostly intended for developers) just before launch",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-topo", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display the topology as part of the process map (mostly intended for developers) just before launch",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-diffable-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display a diffable process map (mostly intended for developers) just before launch",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "report-bindings", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Whether to report process bindings to stderr",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-devel-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display a detailed list (mostly intended for developers) of the allocation being used by this job",
        PRTE_CMD_LINE_OTYPE_DEVEL },
    { '\0', "display-map", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display the process map just before launch",
        PRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "display-allocation", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display the allocation being used by this job",
        PRTE_CMD_LINE_OTYPE_DEBUG },

#if PRTE_ENABLE_FT
    { '\0', "enable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Enable recovery from process failure [Default = disabled]",
        PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "max-restarts", 1, PRTE_CMD_LINE_TYPE_INT,
        "Max number of times to restart a failed process",
        PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "disable-recovery", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Disable recovery (resets all recovery options to off)",
        PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "continuous", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Job is to run until explicitly terminated",
        PRTE_CMD_LINE_OTYPE_FT },
    { '\0', "with-ft", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Specify the type(s) of error handling that the application will use.",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
#endif

    /* mpiexec mandated form launch key parameters */
    { '\0', "initial-errhandler", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Specify the initial error handler that is attached to predefined communicators during the first MPI call.",
        PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "with-ft", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Specify the type(s) of error handling that the application will use.",
        PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Display Commumication Protocol : MPI_Init */
    { '\0', "display-comm", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display table of communication methods between ranks during MPI_Init",
        PRTE_CMD_LINE_OTYPE_GENERAL },

    /* Display Commumication Protocol : MPI_Finalize */
    { '\0', "display-comm-finalize", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Display table of communication methods between ranks during MPI_Finalize",
        PRTE_CMD_LINE_OTYPE_GENERAL },

    /* End of list */
    { '\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL }
};

END_C_DECLS

#endif /* MCA_SCHIZO_OMPI_H_ */

