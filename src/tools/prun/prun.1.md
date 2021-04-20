# NAME

prun - Execute serial and parallel jobs with the PMIx Reference Runtime (PRTE).

# SYNOPSIS

`prun` requires a running `prte` Distributed Virtual Machine (DVM) to be running
at the time of the call. See prte(1) for more information.

Single Process Multiple Data (SPMD) Model:

```
prun [ options ] <program> [ <args> ]
```

Multiple Instruction Multiple Data (MIMD) Model:

```
prun [ global_options ] \
     [ local_options1 ] <program1> [ <args1> ] : \
     [ local_options2 ] <program2> [ <args2> ] : \
     ... : \
     [ local_optionsN ] <programN> [ <argsN> ]
```

Note that in both models, invoking `prun` via an absolute path name is
equivalent to specifying the `--prefix` option with a `<dir>` value
equivalent to the directory where `prun` resides, minus its last
subdirectory. For example:

```
$ /usr/local/bin/prun ...
```

is equivalent to

```
$ prun --prefix /usr/local
```

# QUICK SUMMARY

If you are simply looking for how to run an application, you probably
want to use a command line of the following form:

```
$ prun [ -np X ] [ --hostfile <filename> ] <program>
```

This will run `X` copies of `<program>` in your current run-time environment
over the set of hosts specified by `<filename>`, scheduling (by default)
in a round-robin fashion by CPU slot. If running under a supported resource
manager a hostfile is usually not required unless the caller wishes to further
restrict the set of resources used for that job.

Please note that PRTE automatically binds processes. See prte-map(1) for
defaults for the mapping, ranking, and binding of processes.

If your application uses threads, then you probably want to ensure that
you are either not bound at all (by specifying `--bind-to none`), or
bound to multiple cores using an appropriate binding level or specific
number of processing elements per application process.

Default ranking is by `slot` if number of processes <= 2, otherwise default to
ranking by `package` (formally known as "socket").

See prte-map(1) for more details on mapping, ranking, and binding options.


# OPTIONS

This section includes many commonly used options. There may be other options
listed with `prun --help`.

`prun` will send the name of the directory where it was invoked on the
local node to each of the remote nodes, and attempt to change to that
directory. See the "Current Working Directory" section below for
further details.

`<program>`

:   The program executable. This is identified as the first
    non-recognized argument to `prun`.

`<args>`

:   Pass these run-time arguments to every new process. These must
    always be the last arguments to `prun` after the `<program>`.
    If an app context file is used, `<args>` will be ignored.

`-h, --help`

:   Display help for this command

`-q, --quiet`

:   Suppress informative messages from `prun` during application
    execution.

`-v, --verbose`

:   Be verbose

`-V, --version`

:   Print version number. If no other arguments are given, this will
    also cause `prun` to exit.


## Specifying Number of Processes

The following options specify the number of processes to launch. Note
that none of the options imply a particular binding policy - e.g.,
requesting N processes for each socket does not imply that the processes
will be bound to the package.

Additional options and details are presented in prte-map(1). Below are a few
of the commonly used options.

`-c, -n, --n, --np <#>`

:   Run this many copies of the program on the given nodes. This option
    indicates that the specified file is an executable program and not
    an application context. If no value is provided for the number of
    copies to execute (i.e., neither the `--np` nor its synonyms are
    provided on the command line), `prun` will automatically execute a
    copy of the program on each process slot (see below for description
    of a "process slot"). This feature, however, can only be used in
    the SPMD model and will return an error (without beginning execution
    of the application) otherwise.

## I/O Management

To manage standard I/O:

`--output-filename <filename>`

:   Redirect the `stdout`, `stderr`, and `stddiag` of all processes to a
    process-unique version of the specified filename ("filename.id"). Any
    directories in the filename will automatically be created. Each output file
    will consist of "filename.id", where the `id` will be the processes' rank,
    left-filled with zero's for correct ordering in listings. Both
    `stdout` and `stderr` will be redirected to the file. A relative path
    value will be converted to an absolute path based on the current working
    directory where `prun` is executed. Note that this *will not* work in
    environments where the file system on compute nodes differs from that where
    `prun` is executed. This option accepts one case-insensitive directive,
    specified after a colon (`:`): `NOCOPY` indicates that the output is not to
    be echoed to the terminal.

`--output-directory <path>`

:   Redirect the `stdout`, `stderr`, and `stddiag` of all processes to a
    process-unique location consisting of
    "<path>/<jobid>/rank.id/std[out,err,diag]", where the `id` will be
    the processes' rank, left-filled with zero's for correct ordering
    in listings. Any directories in the filename will automatically be
    created. A relative path value will be converted to an absolute path
    based on the current working directory where `prun` is executed. Note that
    this *will not* work on environments where the file system on compute nodes
    differs from that where `prun` is executed. This option also supports two
    case-insensitive directives, specified in comma-delimited form after
    a colon (`:`): `NOJOBID` (omits the jobid directory layer) and `NOCOPY` (do
    not copy the output to the terminal).

`--stdin <rank>`

:   The rank of the process that is to receive `stdin`. The default is to
    forward `stdin` to rank 0, but this option can be used to forward
    `stdin` to any process. It is also acceptable to specify `none`,
    indicating that no processes are to receive `stdin`.

`--merge-stderr-to-stdout`

:   Merge `stderr` to `stdout` for each process.

`--map-by :TAGOUTPUT`

:   Tag each line of output to `stdout`, `stderr`, and `stddiag` with
    `[jobid, MCW_rank]<stdxxx>` indicating the jobid and rank of the process
    that generated the output, and the channel which generated it.

`--map-by :TIMESTAMPOUTPUT`

:   Timestamp each line of output to `stdout`, `stderr`, and `stddiag`.

`--map-by :XMLOUTPUT`

:   Provide all output to `stdout`, `stderr`, and `stddiag` in an xml format.

`--xterm <ranks>`

:   Display the output from the processes identified by their ranks in
    separate `xterm` windows. The ranks are specified as a comma-separated
    list of ranges, with a `-1` indicating all. A separate window will be
    created for each specified process. **Note:** `xterm` will normally
    terminate the window upon termination of the process running within
    it. However, by adding a "!" to the end of the list of specified
    ranks, the proper options will be provided to ensure that `xterm`
    keeps the window open *after* the process terminates, thus allowing
    you to see the process' output. Each `xterm` window will subsequently
    need to be manually closed. **Note:** In some environments, `xterm`
    may require that the executable be in the user's path, or be
    specified in absolute or relative terms. Thus, it may be necessary
    to specify a local executable as "./foo" instead of just "foo".
    If `xterm` fails to find the executable, `prun` will hang, but still
    respond correctly to a ctrl-c. If this happens, please check that
    the executable is being specified correctly and try again.

## File and Environment Management

To manage files and runtime environment:

`--path <path>`

:   `<path>` that will be used when attempting to locate the requested
    executables. This is used prior to using the local `PATH` setting.

`--prefix <dir>`

:   Prefix directory that will be used to set the `PATH` and
    `LD_LIBRARY_PATH` on the remote node before invoking the target
    process. See the "Remote Execution" section, below.

`--noprefix`

:   Disable the automatic `--prefix` behavior

`-s | --preload-binary`

:   Copy the specified executable(s) to remote machines prior to
    starting remote processes. The executables will be copied to the
    session directory and will be deleted upon completion of the job.

`--preload-files <files>`

:   Preload the comma separated list of files to the current working
    directory of the remote machines where processes will be launched
    prior to starting those processes.

`--set-cwd-to-session-dir`

:   Set the working directory of the started processes to their session
    directory.

`--wdir <dir>`

:   Change to the directory `<dir>` before the user's program executes.
    See the "Current Working Directory" section for notes on relative
    paths. **Note:** If the `--wdir` option appears both on the command
    line and in an application context, the context will take precedence
    over the command line. Thus, if the path to the desired working directory is
    different on the backend nodes, then it must be specified as an
    absolute path that is correct for the backend node.

`--wd <dir>`

:   Synonym for `--wdir`.

`-x <env>`

:   Export the specified environment variables to the remote nodes
    before executing the program. Only one environment variable can be
    specified per `-x` option. Existing environment variables can be
    specified or new variable names specified with corresponding values.
    If multiple `-x` options with the same variable name (regardless of value)
    are provided then the last one listed on the command line will take
    precedence, and the others will be ignored. The exception to this
    is for PRTE_MCA_ prefixed environment variables which will report
    an error in that scenario if any of the values differ.
    For example: `$ prun -x DISPLAY -x OFILE=/tmp/out ...`

The parser for the `-x` option is not very sophisticated; it does not
even understand quoted values. Users are advised to set variables in the
environment, and then use `-x` to export (not define) them.

## MCA Parameters

Setting MCA parameters take a few different forms depending the target
project for the parameter. For example, MCA parameters targeting OpenPMIx
will contain the string `pmix` in their name, and MCA parameters targeting
PRTE will contain the string `prte` in their name. See the "MCA" section,
below, for finer details on the MCA.

`--gpmixmca <key> <value>`

:   Pass global PMIx MCA parameters that are applicable to all application
    contexts. `<key>` is the parameter name; `<value>` is the parameter value.

`--mca <key> <value>`

:   Send arguments to various MCA modules. See the "MCA" section,
    below.

`--pmixmca <key> <value>`

:   Send arguments to various PMIx MCA modules. See the "MCA" section,
    below.

`--prtemca <key> <value>`

:   Send arguments to various PRTE MCA modules. See the "MCA" section,
    below.

`--pmixam <arg0>`

:   Aggregate PMIx MCA parameter set file list. The `arg0` argument is a
    comma-separated list of tuning files. Each file containing MCA parameter
    sets for this application context.

## Debugging Options

`--get-stack-traces`

:   When paired with the `--timeout` option, `prun` will obtain and
    print out stack traces from all launched processes that are still
    alive when the timeout expires. Note that obtaining stack traces can
    take a little time and produce a lot of output, especially for large
    process-count jobs.

`--timeout <seconds>`

:   The maximum number of seconds that `prun` will run. After this many
    seconds, `prun` will abort the launched job and exit with a non-zero
    exit status. Using `--timeout` can be also useful when combined
    with the `--get-stack-traces` option.


## Other Options

There are also other options:

`--allow-run-as-root`

:   Allow `prun` to run when executed by the root user (`prun` defaults
    to aborting when launched as the root user).

`--app <appfile>`

:   Provide an `appfile`, ignoring all other command line options.

`--continuous`

:   Job is to run until explicitly terminated.

`--dvm-uri`

:   Specify the URI of the DVM master, or the name of the file (specified as
    file:filename) that contains that info.

`--enable-recovery`

:   Enable recovery from process failure [Default = disabled].

`--disable-recovery`

:   Disable recovery (resets all recovery options to off).

`--map-by :DONOTLAUNCH`

:   Perform all necessary operations to prepare to launch the
    application, but do not actually launch it.

`--index-argv-by-rank`

:   Uniquely index `argv[0]` for each process using its rank.

`--max-restarts <num>`

:   Max number of times to restart a failed process.

`--pid`

:   PID of the daemon to which we should connect.

`--report-child-jobs-separately`

:   Return the exit status of the primary job only.

`--show-progress`

:   Output a brief periodic report on launch progress.

`--terminate`

:   Terminate the DVM.

The following options are useful for developers; they are not generally
useful to most users:

`--map-by :DISPLAYALLOC`

:   Display a detailed list of the allocation being used by this job.

`--map-by :DISPLAYDEVEL`

:   Display a more detailed table showing the mapped location of each
    process prior to launch.

`--map-by :DISPLAYTOPO`

:   Display the topology as part of the process map just before launch.

`--report-state-on-timeout`

:   When paired with the `--timeout` command line option, report the
    run-time subsystem state of each process when the timeout expires.

# DESCRIPTION

One invocation of `prun` starts an application running under the PRTE DVM. If
the application is single process multiple data (SPMD), the application
can be specified on the `prun` command line.

If the application is multiple instruction multiple data (MIMD),
comprising of multiple programs, the set of programs and argument can be
specified in one of two ways: Extended Command Line Arguments, and
Application Context.

An application context describes the MIMD program set including all
arguments in a separate file. This file essentially contains multiple
`prun` command lines, less the command name itself. The ability to
specify different options for different instantiations of a program is
another reason to use an application context.

Extended command line arguments allow for the description of the
application layout on the command line using colons (`:`) to separate
the specification of programs and arguments. Some options are globally
set across all specified programs (e.g. `--hostfile`), while others are
specific to a single program (e.g. `--np`).

## Specifying Host Nodes

Host nodes can be identified on the `prun` command line with the `--host`
option or in a hostfile. See prte-map(1) for more details.


## Application Context or Executable Program?

To distinguish the two different forms, `prun` looks on the command line
for `--app` option. If it is specified, then the file named on the
command line is assumed to be an application context. If it is not
specified, then the file is assumed to be an executable program.

## Locating Files

If no relative or absolute path is specified for a file, `prun` will first
look for files by searching the directories specified by the `--path`
option. If there is no `--path` option set or if the file is not found
at the `--path` location, then `prun` will search the user's PATH
environment variable as defined on the source node(s).

If a relative directory is specified, it must be relative to the initial
working directory determined by the specific starter used. For example
when using the rsh or ssh starters, the initial directory is `$HOME` by
default. Other starters may set the initial directory to the current
working directory from the invocation of `prun`.

## Current Working Directory

The `--wdir` prun option (and its synonym, `--wd`) allows the user to
change to an arbitrary directory before the program is invoked. It can
also be used in application context files to specify working directories
on specific nodes and/or for specific applications.

If the `--wdir` option appears both in a context file and on the command
line, the context file directory will override the command line value.

If the `--wdir` option is specified, `prun` will attempt to change to the
specified directory on all of the remote nodes. If this fails, `prun`
will abort.

If the `--wdir` option is **not** specified, `prun` will send the directory
name where `prun` was invoked to each of the remote nodes. The remote
nodes will try to change to that directory. If they are unable (e.g., if
the directory does not exist on that node), then `prun` will use the
default directory determined by the starter.

All directory changing occurs before the user's program is invoked.

## Standard I/O

The PRTE DVM directs UNIX standard input to `/dev/null` on all processes except
the rank 0 process. The rank 0 process inherits standard input from
`prun`. **Note:** The node that invoked `prun` need not be the same as
the node where the rank 0 process resides. PRTE DVM handles the redirection
of `prun`'s standard input to the rank 0 process.

The PRTE DVM directs UNIX standard output and error from remote nodes to the
node that invoked `prun` and prints it on the standard output/error of
`prun`. Local processes inherit the standard output/error of `prun` and
transfer to it directly.

Thus it is possible to redirect standard I/O for applications by using
the typical shell redirection procedure on `prun`.

```
$ prun --np 2 my_app < my_input > my_output
```

Note that in this example *only* the rank 0 process will receive the
stream from `my_input` on stdin. The stdin on all the other nodes will
be tied to `/dev/null`. However, the stdout from all nodes will be
collected into the `my_output` file.

## Signal Propagation

When `prun` receives a `SIGTERM` and `SIGINT`, it will attempt to kill the
entire job by sending all processes in the job a `SIGTERM`, waiting a
small number of seconds, then sending all processes in the job a
`SIGKILL`.

`SIGUSR1` and `SIGUSR2` signals received by `prun` are propagated to all
processes in the job.

A `SIGTSTOP` signal to `prun` will cause a `SIGSTOP` signal to be sent to all
of the programs started by `prun` and likewise a `SIGCONT` signal to `prun`
will cause a `SIGCONT` sent.

Other signals are not currently propagated by `prun`.

## Process Termination / Signal Handling

During the run of an application, if any process dies abnormally (either
exiting before invoking `PMIx_Finalize`, or dying as the result of a
signal), `prun` will print out an error message and kill the rest of the
application.

## Process Environment

Processes in the application inherit their environment from the PRTE DVM
daemon upon the node on which they are running. The environment is
typically inherited from the user's shell. On remote nodes, the exact
environment is determined by the boot MCA module used. The `rsh` launch
module, for example, uses either `rsh`/`ssh` to launch the PRTE DVM daemon
on remote nodes, and typically executes one or more of the user's
shell-setup files before launching the daemon. When running dynamically
linked applications which require the `LD_LIBRARY_PATH` environment
variable to be set, care must be taken to ensure that it is correctly
set when booting PRTE DVM.

See the "Remote Execution" section for more details.

## Remote Execution

The PRTE DVM requires that the `PATH` environment variable be set to find
executables on remote nodes. This is typically only necessary in `rsh`-
or `ssh`-based environments. Batch and scheduled environments typically
copy the current environment to the execution of remote jobs, so if the
current environment has `PATH` and/or `LD_LIBRARY_PATH` set properly,
the remote nodes will also have it set properly. If the PRTE DVM was compiled
with shared library support, it may also be necessary to have the
`LD_LIBRARY_PATH` environment variable set on remote nodes as well
(especially to find the shared libraries required to run user
applications).

However, it is not always desirable or possible to edit shell startup
files to set `PATH` and/or `LD_LIBRARY_PATH`. The `--prefix` option
is provided for some simple configurations where this is not possible.

The `--prefix` option takes a single argument: the base directory on
the remote node where PRTE DVM is installed. The PRTE DVM will use this directory
to set the remote `PATH` and `LD_LIBRARY_PATH` before executing any
user applications. This allows running jobs without having
pre-configured the `PATH` and `LD_LIBRARY_PATH` on the remote nodes.

The PRTE DVM adds the basename of the current node's "bindir" (the directory
where the PRTE DVM's executables are installed) to the prefix and uses that to
set the `PATH` on the remote node. Similarly, PRTE DVM adds the basename of
the current node's "libdir" (the directory where the PRTE DVM's libraries
are installed) to the prefix and uses that to set the
`LD_LIBRARY_PATH` on the remote node. For example:

Local bindir:

:   /local/node/directory/bin

Local libdir:

:   /local/node/directory/lib64

If the following command line is used:

```
$ prun --prefix /remote/node/directory
```

The PRTE DVM will add "/remote/node/directory/bin" to the `PATH` and
"/remote/node/directory/lib64" to the `LD_LIBRARY_PATH` on the remote
node before attempting to execute anything.

The `--prefix` option is not sufficient if the installation paths on
the remote node are different than the local node (e.g., if "/lib" is
used on the local node, but "/lib64" is used on the remote node), or
if the installation paths are something other than a subdirectory under
a common prefix.

Note that executing `prun` via an absolute pathname is equivalent to
specifying `--prefix` without the last subdirectory in the absolute
pathname to `prun`.

For example:

```
$ /usr/local/bin/prun ...
```

is equivalent to

```
$ prun --prefix /usr/local ...
```

## Exported Environment Variables

All environment variables that are named in the form `PMIX_\*` will
automatically be exported to new processes on the local and remote
nodes. Environmental parameters can also be set/forwarded to the new
processes using the MCA parameter `mca_base_env_list`. While the
syntax of the `-x` option and MCA param allows the definition of new
variables, note that the parser for these options are currently not very
sophisticated - it does not even understand quoted values. Users are
advised to set variables in the environment and use the option to export
them; not to define them.

## Setting MCA Parameters

The `--mca` / `--pmixmca` / `--prtemca` switches (referenced here as
"`--mca` switches" for brevity) allow the passing of parameters
to various MCA (Modular Component Architecture) modules. MCA modules have
direct impact on programs because they allow tunable parameters to be set at
run time.

The `-mca` switch takes two arguments: `<key>` and `<value>`. The
`<key>` argument generally specifies which MCA module will receive the
value. For example, the `<key>` "rmaps" is used to select which RMAPS to
be used for mapping processes to nodes. The `<value>` argument is the value
that is passed. For example:

`prun -prtemca rmaps seq -np 1 foo`

:   Tells PRTE to use the "seq" RMAPS component, and to run a
    single copy of "a.out" on an allocated node.

The `-mca` switch can be used multiple times to specify different
`<key>` and/or `<value>` arguments. If the same `<key>` is
specified more than once, the `<value>`s are concatenated with a comma
(",") separating them.

Note that the `-mca` switch is simply a shortcut for setting
environment variables. The same effect may be accomplished by setting
corresponding environment variables before running `prun`. The form of
the environment variables depends on the type of the `--mca` switch.

`--mca`

:    `PRTE_MCA_<key>=<value>`

`--pmixmca`

:    `PMIX_MCA_<key>=<value>`

`--prtemca`

:    `PRTE_MCA_<key>=<value>``

Thus, the `-mca` switch overrides any previously set environment
variables. The `-mca` settings similarly override MCA parameters set in
the `$PRTE_PREFIX/etc/prte-mca-params.conf` or
`$HOME/.prte/mca-params.conf` file.

Unknown `<key>` arguments are still set as environment variable --
they are not checked (by `prun`) for correctness. Illegal or incorrect
`<value>` arguments may or may not be reported -- it depends on the
specific MCA module.

To find the available component types under the MCA architecture, or to
find the available parameters for a specific component, use the `pinfo`
command. See the *pinfo(1)* man page for detailed information on the
command.

## Running as root

The PRTE team strongly advises against executing `prun` as the root
user. Applications should be run as regular (non-root) users.

Reflecting this advice, `prun` will refuse to run as root by default. To
override this default, you can add the `--allow-run-as-root` option to
the `prun` command line.

# RETURN VALUE

There is no standard definition for what `prun` should return as an exit
status. After considerable discussion, we settled on the following
method for assigning the `prun` exit status (note: in the following
description, the "primary" job is the initial application started by
`prun` - all jobs that are spawned by that job are designated
"secondary" jobs):

-   if all processes in the primary job normally terminate with exit
    status 0, we return 0

-   if one or more processes in the primary job normally terminate with
    non-zero exit status, we return the exit status of the process with
    the lowest rank to have a non-zero status

-   if all processes in the primary job normally terminate with exit
    status 0, and one or more processes in a secondary job normally
    terminate with non-zero exit status, we (a) return the exit status
    of the process with the lowest rank in the lowest jobid to have a
    non-zero status, and (b) output a message summarizing the exit
    status of the primary and all secondary jobs.

-   if the cmd line option `--report-child-jobs-separately` is set, we
    will return -only- the exit status of the primary job. Any non-zero
    exit status in secondary jobs will be reported solely in a summary
    print statement.

By default, the job will abort when any process terminates with non-zero
status. The MCA parameter `prte_abort_on_non_zero_status` can be set to
`false` (or `0`) to cause the PRTE DVM to not abort a job if one or more
processes return a non-zero status. In that situation the PRTE DVM records
and notes that processes exited with non-zero termination status to
report the approprate exit status of `prun` (per bullet points above).

If the `--timeout` command line option is used and the timeout
expires before the job completes (thereby forcing `prun` to kill the
job) `prun` will return an exit status equivalent to the value of
`ETIMEDOUT` (which is typically 110 on Linux and OS X systems).
