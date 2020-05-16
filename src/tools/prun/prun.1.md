# NAME

prun - Execute serial and parallel jobs with the PMIx Reference Server.

# SYNOPSIS

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

This will run X copies of `<program>` in your current run-time
environment (if running under a supported resource manager, PSRVR's
`prun` will usually automatically use the corresponding resource manager
process starter, as opposed to, for example, `rsh or `ssh`, which
require the use of a hostfile, or will default to running all X copies
on the localhost), scheduling (by default) in a round-robin fashion by
CPU slot. See the rest of this page for more details.

Please note that prun automatically binds processes. Three binding
patterns are used in the absence of any further directives:

`Bind to core:`

:   when the number of processes is <= 2

`Bind to socket:`

:   when the number of processes is > 2

`Bind to none:`

:   when oversubscribed

If your application uses threads, then you probably want to ensure that
you are either not bound at all (by specifying `--bind-to none`), or
bound to multiple cores using an appropriate binding level or specific
number of processing elements per application process.

# OPTIONS

`prun` will send the name of the directory where it was invoked on the
local node to each of the remote nodes, and attempt to change to that
directory. See the "Current Working Directory" section below for
further details.

`<program>`

:   The program executable. This is identified as the first
    non-recognized argument to prun.

`<args>`

:   Pass these run-time arguments to every new process. These must
    always be the last arguments to `prun`. If an app context file is
    used, `<args>` will be ignored.

`-h, --help`

:   Display help for this command

`-q, --quiet`

:   Suppress informative messages from prun during application
    execution.

`-v, --verbose`

:   Be verbose

`-V, --version`

:   Print version number. If no other arguments are given, this will
    also cause prun to exit.

`-N <num>`

:
    Launch num processes per node on all allocated nodes (synonym for
    npernode).

`-display-map, --display-map`

:   Display a table showing the mapped location of each process prior to
    launch.

`-display-allocation, --display-allocation`

:   Display the detected resource allocation.

`-output-proctable, --output-proctable`

:   Output the debugger proctable after launch.

`-max-vm-size, --max-vm-size <size>`

:   Number of processes to run.

`-novm, --novm`

:   Execute without creating an allocation-spanning virtual machine
    (only start daemons on nodes hosting application procs).

`-hnp, --hnp <arg0>`

:   Specify the URI of the psrvr process, or the name of the file
    (specified as file:filename) that contains that info.

Use one of the following options to specify which hosts (nodes) within
the psrvr to run on.

`-H, -host, --host <host1,host2,...,hostN>`

:   List of hosts on which to invoke processes.

`-hostfile, --hostfile <hostfile>`

:   Provide a hostfile to use.

`-default-hostfile, --default-hostfile <hostfile>`

:   Provide a default hostfile.

`-machinefile, --machinefile <machinefile>`

:   Synonym for `-hostfile`.

`-cpu-set, --cpu-set <list>`

:   Restrict launched processes to the specified logical cpus on each
    node (comma-separated list). Note that the binding options will
    still apply within the specified envelope - e.g., you can elect to
    bind each process to only one cpu within the specified cpu set.

The following options specify the number of processes to launch. Note
that none of the options imply a particular binding policy - e.g.,
requesting N processes for each socket does not imply that the processes
will be bound to the socket.

`-c, -n, --n, -np <#>`

:   Run this many copies of the program on the given nodes. This option
    indicates that the specified file is an executable program and not
    an application context. If no value is provided for the number of
    copies to execute (i.e., neither the "-np" nor its synonyms are
    provided on the command line), prun will automatically execute a
    copy of the program on each process slot (see below for description
    of a "process slot"). This feature, however, can only be used in
    the SPMD model and will return an error (without beginning execution
    of the application) otherwise.

`-map-by,----map-by ppr:N:<object>`

:   Launch N times the number of objects of the specified type on each
    node.

`-npersocket, --npersocket <#persocket>`

:   On each node, launch this many processes times the number of
    processor sockets on the node. The `-npersocket` option also turns
    on the `-bind-to-socket` option. (deprecated in favor of --map-by
    ppr:n:socket)

`-npernode, --npernode <#pernode>`

:   On each node, launch this many processes. (deprecated in favor of
    --map-by ppr:n:node)

`-pernode, --pernode`

:   On each node, launch one process -- equivalent to `-npernode` 1.
    (deprecated in favor of --map-by ppr:1:node)

To map processes:

`-map-by,--map-by <foo>`

:   Map to the specified object, defaults to `socket`. Supported options
    include `slot`, `hwthread`, `core`, `L1cache`, `L2cache`, `L3cache`,
    `socket`, `numa`, `board`, `node`, `sequential`, `distance`, and
    `ppr`. Any object can include modifiers by adding a : and any
    combination of PE=n (bind n processing elements to each proc), SPAN
    (load balance the processes across the allocation), OVERSUBSCRIBE
    (allow more processes on a node than processing elements), and
    NOOVERSUBSCRIBE. This includes PPR, where the pattern would be
    terminated by another colon to separate it from the modifiers.

`-bycore, --bycore`

:   Map processes by core (deprecated in favor of --map-by core)

`-byslot, --byslot`

:   Map and rank processes round-robin by slot.

`-nolocal, --nolocal`

:   Do not run any copies of the launched application on the same node
    as prun is running. This option will override listing the localhost
    with `--host` or any other host-specifying mechanism.

`-nooversubscribe, --nooversubscribe`

:   Do not oversubscribe any nodes; error (without starting any
    processes) if the requested number of processes would cause
    oversubscription. This option implicitly sets "max_slots" equal
    to the "slots" value for each node. (Enabled by default).

`-oversubscribe, --oversubscribe`

:   Nodes are allowed to be oversubscribed, even on a managed system,
    and overloading of processing elements.

`-bynode, --bynode`

:   Launch processes one per node, cycling by node in a round-robin
    fashion. This spreads processes evenly among nodes and assigns ranks
    in a round-robin, "by node" manner.

`-cpu-list, --cpu-list <cpus>`

:   List of processor IDs to bind processes to [default=NULL].

To order processes' ranks:

`--rank-by <foo>`

:   Rank in round-robin fashion according to the specified object,
    defaults to `slot`. Supported options include slot, hwthread, core,
    L1cache, L2cache, L3cache, socket, numa, board, and node.

For process binding:

`--bind-to <foo>`

:   Bind processes to the specified object, defaults to `core`.
    Supported options include slot, hwthread, core, l1cache, l2cache,
    l3cache, socket, numa, board, and none.

`-cpus-per-proc, --cpus-per-proc <#perproc>`

:   Bind each process to the specified number of cpus. (deprecated in
    favor of --map-by <obj>:PE=n)

`-cpus-per-rank, --cpus-per-rank <#perrank>`

:   Alias for `-cpus-per-proc`. (deprecated in favor of --map-by
    <obj>:PE=n)

`-bind-to-core, --bind-to-core`

:   Bind processes to cores (deprecated in favor of --bind-to core)

`-bind-to-socket, --bind-to-socket`

:   Bind processes to processor sockets (deprecated in favor of
    --bind-to socket)

`-report-bindings, --report-bindings`

:   Report any bindings for launched processes.

For rankfiles:

`-rf, --rankfile <rankfile>`

:   Provide a rankfile file.

To manage standard I/O:

`-output-filename, --output-filename <filename>`

:   Redirect the stdout, stderr, and stddiag of all processes to a
    process-unique version of the specified filename. Any directories in
    the filename will automatically be created. Each output file will
    consist of filename.id, where the id will be the processes' rank,
    left-filled with zero's for correct ordering in listings.B oth
    stdout and stderr will be redirected to the file. A relative path
    value will be converted to an absolute path based on the cwd where
    prun is executed. Note that this *will not* work on environments
    where the file system on compute nodes differs from that where prun
    is executed. This option accepts one case-insensitive directive,
    specified after a colon: NOCOPY indicates that the output is not to
    be echoed to the terminal.

`-output-directory, --output-directory <path>`

:   Redirect the stdout, stderr, and stddiag of all processes to a
    process-unique location consisting of
    "<path>/<jobid>/rank.id/stdout[err]", where the id will be
    the processes' rank, left-filled with zero's for correct ordering
    in listings. Any directories in the filename will automatically be
    created. A relative path value will be converted to an absolute path
    based on the cwd where prun is executed. Note that this *will not*
    work on environments where the file system on compute nodes differs
    from that where prun is executed. This option also supports two
    case-insensitive directives, specified in comma-delimited form after
    a colon: NOJOBID (omits the jobid directory layer) and NOCOPY (do
    not copy the output to the terminal).

`-stdin, --stdin <rank> `

:   The rank of the process that is to receive stdin. The default is to
    forward stdin to rank 0, but this option can be used to forward
    stdin to any process. It is also acceptable to specify `none`,
    indicating that no processes are to receive stdin.

`-merge-stderr-to-stdout, --merge-stderr-to-stdout`

:   Merge stderr to stdout for each process.

`-tag-output, --tag-output`

:   Tag each line of output to stdout, stderr, and stddiag with
    `[jobid, MCW_rank]<stdxxx>` indicating the process jobid and
    rank of the process that generated the output, and the channel which
    generated it.

`-timestamp-output, --timestamp-output`

:   Timestamp each line of output to stdout, stderr, and stddiag.

`-xml, --xml`

:   Provide all output to stdout, stderr, and stddiag in an xml format.

`-xml-file, --xml-file <filename>`

:   Provide all output in XML format to the specified file.

`-xterm, --xterm <ranks>`

:   Display the output from the processes identified by their ranks in
    separate xterm windows. The ranks are specified as a comma-separated
    list of ranges, with a -1 indicating all. A separate window will be
    created for each specified process. **Note:** xterm will normally
    terminate the window upon termination of the process running within
    it. However, by adding a "!" to the end of the list of specified
    ranks, the proper options will be provided to ensure that xterm
    keeps the window open *after* the process terminates, thus allowing
    you to see the process' output. Each xterm window will subsequently
    need to be manually closed. **Note:** In some environments, xterm
    may require that the executable be in the user's path, or be
    specified in absolute or relative terms. Thus, it may be necessary
    to specify a local executable as "./foo" instead of just "foo".
    If xterm fails to find the executable, prun will hang, but still
    respond correctly to a ctrl-c. If this happens, please check that
    the executable is being specified correctly and try again.

To manage files and runtime environment:

`-path, --path <path>`

:   <path> that will be used when attempting to locate the requested
    executables. This is used prior to using the local PATH setting.

`--prefix <dir>`

:   Prefix directory that will be used to set the `PATH` and
    `LD_LIBRARY_PATH` on the remote node before invoking the target
    process. See the "Remote Execution" section, below.

`--noprefix`

:   Disable the automatic --prefix behavior

`-s, --preload-binary`

:   Copy the specified executable(s) to remote machines prior to
    starting remote processes. The executables will be copied to the
    session directory and will be deleted upon completion of the job.

`--preload-files <files>`

:   Preload the comma separated list of files to the current working
    directory of the remote machines where processes will be launched
    prior to starting those processes.

`-set-cwd-to-session-dir, --set-cwd-to-session-dir`

:   Set the working directory of the started processes to their session
    directory.

`-wd <dir>`

:   Synonym for `-wdir`.

`-wdir <dir>`

:   Change to the directory <dir> before the user's program executes.
    See the "Current Working Directory" section for notes on relative
    paths. **Note:** If the `-wdir` option appears both on the command
    line and in an application context, the context will take precedence
    over the command line. Thus, if the path to the desired wdir is
    different on the backend nodes, then it must be specified as an
    absolute path that is correct for the backend node.

`-x <env>`

:   Export the specified environment variables to the remote nodes
    before executing the program. Only one environment variable can be
    specified per `-x` option. Existing environment variables can be
    specified or new variable names specified with corresponding values.
    For example: `$ prun -x DISPLAY -x OFILE=/tmp/out ...`

The parser for the `-x` option is not very sophisticated; it does not
even understand quoted values. Users are advised to set variables in the
environment, and then use `-x` to export (not define) them.

Setting MCA parameters:

`-gpmca, --gpmca <key> <value>`

:   Pass global MCA parameters that are applicable to all contexts.
    `<key>` is the parameter name; `<value>` is the parameter value.

`-pmca, --pmca <key> <value>`

:   Send arguments to various MCA modules. See the "MCA" section,
    below.

`-am <arg0>`

:   Aggregate MCA parameter set file list.

`-tune, --tune <tune_file>`

:   Specify a tune file to set arguments for various MCA modules and
    environment variables. See the "Setting MCA parameters and
    environment variables from file" section, below.

For debugging:

`-debug, --debug`

:   Invoke the user-level debugger indicated by the
    `prte_base_user_debugger` MCA parameter.

`--get-stack-traces`

:   When paired with the `--timeout` option, `prun` will obtain and
    print out stack traces from all launched processes that are still
    alive when the timeout expires. Note that obtaining stack traces can
    take a little time and produce a lot of output, especially for large
    process-count jobs.

`-debugger, --debugger <args>`

:   Sequence of debuggers to search for when `--debug` is used (i.e. a
    synonym for `prte_base_user_debugger` MCA parameter).

`--timeout <seconds>`

:   The maximum number of seconds that `prun` will run. After this many
    seconds, `prun` will abort the launched job and exit with a non-zero
    exit status. Using `--timeout` can be also useful when combined
    with the `--get-stack-traces` option.

`-tv, --tv`

:   Launch processes under the TotalView debugger. Deprecated backwards
    compatibility flag. Synonym for `--debug`.

There are also other options:

`--allow-run-as-root`

:   Allow `prun` to run when executed by the root user (`prun` defaults
    to aborting when launched as the root user).

`--app <appfile>`

:   Provide an appfile, ignoring all other command line options.

`-cf, --cartofile <cartofile>`

:   Provide a cartography file.

`-continuous, --continuous`

:   Job is to run until explicitly terminated.

`-disable-recovery, --disable-recovery`

:   Disable recovery (resets all recovery options to off).

`-do-not-launch, --do-not-launch`

:   Perform all necessary operations to prepare to launch the
    application, but do not actually launch it.

`-do-not-resolve, --do-not-resolve`

:   Do not attempt to resolve interfaces.

`-enable-recovery, --enable-recovery`

:   Enable recovery from process failure [Default = disabled].

`-index-argv-by-rank, --index-argv-by-rank`

:   Uniquely index argv[0] for each process using its rank.

`-max-restarts, --max-restarts <num>`

:   Max number of times to restart a failed process.

`--ppr <list>`

:   Comma-separated list of number of processes on a given resource type
    [default: none]. (deprecated in favor of --map-by ppr:<list>)

`-report-child-jobs-separately, --report-child-jobs-separately`

:   Return the exit status of the primary job only.

`-report-events, --report-events <URI>`

:   Report events to a tool listening at the specified URI.

`-report-pid, --report-pid <channel>`

:   Print out prun's PID during startup. The channel must be either a
    '-' to indicate that the pid is to be output to stdout, a '+' to
    indicate that the pid is to be output to stderr, or a filename to
    which the pid is to be written.

`-report-uri, --report-uri <channel>`

:   Print out prun's URI during startup. The channel must be either a
    '-' to indicate that the URI is to be output to stdout, a '+' to
    indicate that the URI is to be output to stderr, or a filename to
    which the URI is to be written.

`-show-progress, --show-progress`

:   Output a brief periodic report on launch progress.

`-terminate, --terminate`

:   Terminate the DVM.

`-use-hwthread-cpus, --use-hwthread-cpus`

:   Use hardware threads as independent cpus.

`-use-regexp, --use-regexp`

:   Use regular expressions for launch.

The following options are useful for developers; they are not generally
useful to most users:

`-d, --debug-devel`

:   Enable debugging. This is not generally useful for most users.

`-display-devel-allocation, --display-devel-allocation`

:   Display a detailed list of the allocation being used by this job.

`-display-devel-map, --display-devel-map`

:   Display a more detailed table showing the mapped location of each
    process prior to launch.

`-display-diffable-map, --display-diffable-map`

:   Display a diffable process map just before launch.

`-display-topo, --display-topo`

:   Display the topology as part of the process map just before launch.

`--report-state-on-timeout`

:   When paired with the `--timeout` command line option, report the
    run-time subsystem state of each process when the timeout expires.

There may be other options listed with `prun --help`.

# DESCRIPTION

One invocation of `prun` starts an application running under PSRVR. If
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
set across all specified programs (e.g. --hostfile), while others are
specific to a single program (e.g. -np).

## Specifying Host Nodes

Host nodes can be identified on the `prun` command line with the `-host`
option or in a hostfile.

For example,

prun -H aa,aa,bb ./a.out

:   launches two processes on node aa and one on bb.

Or, consider the hostfile

```
$ cat myhostfile aa slots=2 bb slots=2 cc slots=2
```

Here, we list both the host names (aa, bb, and cc) but also how many
"slots" there are for each. Slots indicate how many processes can
potentially execute on a node. For best performance, the number of slots
may be chosen to be the number of cores on the node or the number of
processor sockets. If the hostfile does not provide slots information,
PSRVR will attempt to discover the number of cores (or hwthreads, if the
use-hwthreads-as-cpus option is set) and set the number of slots to that
value. This default behavior also occurs when specifying the `-host`
option with a single hostname. Thus, the command

prun -H aa ./a.out

:   launches a number of processes equal to the number of cores on node
    aa.

prun -hostfile myhostfile ./a.out

:   will launch two processes on each of the three nodes.

prun -hostfile myhostfile -host aa ./a.out

:   will launch two processes, both on node aa.

prun -hostfile myhostfile -host dd ./a.out

:   will find no hosts to run on and abort with an error. That is, the
    specified host dd is not in the specified hostfile.

When running under resource managers (e.g., SLURM, Torque, etc.), PSRVR
will obtain both the hostnames and the number of slots directly from the
resource manger.

## Specifying Number of Processes

As we have just seen, the number of processes to run can be set using
the hostfile. Other mechanisms exist.

The number of processes launched can be specified as a multiple of the
number of nodes or processor sockets available. For example,

prun -H aa,bb -npersocket 2 ./a.out

:   launches processes 0-3 on node aa and process 4-7 on node bb, where
    aa and bb are both dual-socket nodes. The `-npersocket` option also
    turns on the `-bind-to-socket` option, which is discussed in a later
    section.

prun -H aa,bb -npernode 2 ./a.out

:   launches processes 0-1 on node aa and processes 2-3 on node bb.

prun -H aa,bb -npernode 1 ./a.out

:   launches one process per host node.

prun -H aa,bb -pernode ./a.out

:   is the same as `-npernode` 1.

Another alternative is to specify the number of processes with the `-np`
option. Consider now the hostfile

```
$ cat myhostfile aa slots=4 bb slots=4 cc slots=4
```

Now,

`prun -hostfile myhostfile -np 6 ./a.out`

:   will launch processes 0-3 on node aa and processes 4-5 on node bb.
    The remaining slots in the hostfile will not be used since the `-np`
    option indicated that only 6 processes should be launched.

## Mapping Processes to Nodes: Using Policies

The examples above illustrate the default mapping of process processes
to nodes. This mapping can also be controlled with various `prun`
options that describe mapping policies.

Consider the same hostfile as above, again with `-np` 6:

node aa node bb node cc

prun 0 1 2 3 4 5

prun --map-by node 0 3 1 4 2 5

prun -nolocal 0 1 2 3 4 5

The `--map-by node` option will load balance the processes across the
available nodes, numbering each process in a round-robin fashion.

The `-nolocal` option prevents any processes from being mapped onto the
local host (in this case node aa). While `prun` typically consumes few
system resources, `-nolocal` can be helpful for launching very large
jobs where `prun` may actually need to use noticeable amounts of memory
and/or processing time.

Just as `-np` can specify fewer processes than there are slots, it can
also oversubscribe the slots. For example, with the same hostfile:

prun -hostfile myhostfile -np 14 ./a.out

:   will launch processes 0-3 on node aa, 4-7 on bb, and 8-11 on cc. It
    will then add the remaining two processes to whichever nodes it
    chooses.

One can also specify limits to oversubscription. For example, with the
same hostfile:

prun -hostfile myhostfile -np 14 -nooversubscribe ./a.out

:   will produce an error since `-nooversubscribe` prevents
    oversubscription.

Limits to oversubscription can also be specified in the hostfile itself:
% cat myhostfile aa slots=4 max_slots=4 bb max_slots=4 cc slots=4

The `max_slots` field specifies such a limit. When it does, the `slots`
value defaults to the limit. Now:

prun -hostfile myhostfile -np 14 ./a.out

:   causes the first 12 processes to be launched as before, but the
    remaining two processes will be forced onto node cc. The other two
    nodes are protected by the hostfile against oversubscription by this
    job.

Using the `--nooversubscribe` option can be helpful since PSRVR
currently does not get "max_slots" values from the resource manager.

Of course, `-np` can also be used with the `-H` or `-host` option. For
example,

prun -H aa,bb -np 8 ./a.out

:   launches 8 processes. Since only two hosts are specified, after the
    first two processes are mapped, one to aa and one to bb, the
    remaining processes oversubscribe the specified hosts.

And here is a MIMD example:

prun -H aa -np 1 hostname : -H bb,cc -np 2 uptime

:   will launch process 0 running `hostname` on node aa and processes 1
    and 2 each running `uptime` on nodes bb and cc, respectively.

## Mapping, Ranking, and Binding: Oh My!

PSRVR employs a three-phase procedure for assigning process locations
and ranks:

**Mapping**

:   Assigns a default location to each process

**Ranking**

:   Assigns a rank value to each process

**Binding**

:   Constrains each process to run on specific processors

The *mapping* step is used to assign a default location to each process
based on the mapper being employed. Mapping by slot, node, and
sequentially results in the assignment of the processes to the node
level. In contrast, mapping by object, allows the mapper to assign the
process to an actual object on each node.

**Note:** the location assigned to the process is independent of where
it will be bound - the assignment is used solely as input to the binding
algorithm.

The mapping of process processes to nodes can be defined not just with
general policies but also, if necessary, using arbitrary mappings that
cannot be described by a simple policy. One can use the "sequential
mapper," which reads the hostfile line by line, assigning processes to
nodes in whatever order the hostfile specifies. Use the `-pmca rmaps
seq` option. For example, using the same hostfile as before:

prun -hostfile myhostfile -pmca rmaps seq ./a.out

will launch three processes, one on each of nodes aa, bb, and cc,
respectively. The slot counts don't matter; one process is launched per
line on whatever node is listed on the line.

Another way to specify arbitrary mappings is with a rankfile, which
gives you detailed control over process binding as well. Rankfiles are
discussed below.

The second phase focuses on the *ranking* of the process within the job.
PSRVR separates this from the mapping procedure to allow more
flexibility in the relative placement of processes. This is best
illustrated by considering the following two cases where we used the
---map-by ppr:2:socket option:

node aa node bb

rank-by core 0 1 ! 2 3 4 5 ! 6 7

rank-by socket 0 2 ! 1 3 4 6 ! 5 7

rank-by socket:span 0 4 ! 1 5 2 6 ! 3 7

Ranking by core and by slot provide the identical result - a simple
progression of ranks across each node. Ranking by socket does a
round-robin ranking within each node until all processes have been
assigned a rank, and then progresses to the next node. Adding the `span`
modifier to the ranking directive causes the ranking algorithm to treat
the entire allocation as a single entity - thus, the MCW ranks are
assigned across all sockets before circling back around to the
beginning.

The *binding* phase actually binds each process to a given set of
processors. This can improve performance if the operating system is
placing processes suboptimally. For example, it might oversubscribe some
multi-core processor sockets, leaving other sockets idle; this can lead
processes to contend unnecessarily for common resources. Or, it might
spread processes out too widely; this can be suboptimal if application
performance is sensitive to interprocess communication costs. Binding
can also keep the operating system from migrating processes excessively,
regardless of how optimally those processes were placed to begin with.

The processors to be used for binding can be identified in terms of
topological groupings - e.g., binding to an l3cache will bind each
process to all processors within the scope of a single L3 cache within
their assigned location. Thus, if a process is assigned by the mapper to
a certain socket, then a `---bind-to l3cache` directive will cause the
process to be bound to the processors that share a single L3 cache
within that socket.

To help balance loads, the binding directive uses a round-robin method
when binding to levels lower than used in the mapper. For example,
consider the case where a job is mapped to the socket level, and then
bound to core. Each socket will have multiple cores, so if multiple
processes are mapped to a given socket, the binding algorithm will
assign each process located to a socket to a unique core in a
round-robin manner.

Alternatively, processes mapped by l2cache and then bound to socket will
simply be bound to all the processors in the socket where they are
located. In this manner, users can exert detailed control over relative
MCW rank location and binding.

Finally, `--report-bindings` can be used to report bindings.

As an example, consider a node with two processor sockets, each
comprising four cores. We run `prun` with `-np 4 --report-bindings` and
the following additional options:

```
% prun ... --map-by core --bind-to core [...] ... binding child
[...,0] to cpus 0001 [...] ... binding child [...,1] to cpus
0002 [...] ... binding child [...,2] to cpus 0004 [...] ...
binding child [...,3] to cpus 0008
```

```
% prun ... --map-by socket --bind-to socket [...] ... binding
child [...,0] to socket 0 cpus 000f [...] ... binding child
[...,1] to socket 1 cpus 00f0 [...] ... binding child [...,2]
to socket 0 cpus 000f [...] ... binding child [...,3] to socket 1
cpus 00f0
```

```
% prun ... --map-by core:PE=2 --bind-to core [...] ... binding
child [...,0] to cpus 0003 [...] ... binding child [...,1] to
cpus 000c [...] ... binding child [...,2] to cpus 0030 [...]
... binding child [...,3] to cpus 00c0
```

```
% prun ... --bind-to none
```

Here, `--report-bindings` shows the binding of each process as a mask.
In the first case, the processes bind to successive cores as indicated
by the masks 0001, 0002, 0004, and 0008. In the second case, processes
bind to all cores on successive sockets as indicated by the masks 000f
and 00f0. The processes cycle through the processor sockets in a
round-robin fashion as many times as are needed. In the third case, the
masks show us that 2 cores have been bound per process. In the fourth
case, binding is turned off and no bindings are reported.

PSRVR's support for process binding depends on the underlying operating
system. Therefore, certain process binding options may not be available
on every system.

Process binding can also be set with MCA parameters. Their usage is less
convenient than that of `prun` options. On the other hand, MCA
parameters can be set not only on the `prun` command line, but
alternatively in a system or user mca-params.conf file or as environment
variables, as described in the MCA section below. Some examples include:

prun option MCA parameter key value

--map-by core rmaps_base_mapping_policy core --map-by socket
rmaps_base_mapping_policy socket --rank-by core
rmaps_base_ranking_policy core --bind-to core
hwloc_base_binding_policy core --bind-to socket
hwloc_base_binding_policy socket --bind-to none
hwloc_base_binding_policy none

## Rankfiles

Rankfiles are text files that specify detailed information about how
individual processes should be mapped to nodes, and to which
processor(s) they should be bound. Each line of a rankfile specifies the
location of one process. The general form of each line in the rankfile
is:

rank <N>=<hostname> slot=<slot list>

For example:

$ cat myrankfile rank 0=aa slot=1:0-2 rank 1=bb slot=0:0,1 rank 2=cc
slot=1-2 $ prun -H aa,bb,cc,dd -rf myrankfile ./a.out

Means that

Rank 0 runs on node aa, bound to logical socket 1, cores 0-2. Rank 1
runs on node bb, bound to logical socket 0, cores 0 and 1. Rank 2 runs
on node cc, bound to logical cores 1 and 2.

Rankfiles can alternatively be used to specify *physical* processor
locations. In this case, the syntax is somewhat different. Sockets are
no longer recognized, and the slot number given must be the number of
the physical PU as most OS's do not assign a unique physical identifier
to each core in the node. Thus, a proper physical rankfile looks
something like the following:

$ cat myphysicalrankfile rank 0=aa slot=1 rank 1=bb slot=8 rank 2=cc
slot=6

This means that

Rank 0 will run on node aa, bound to the core that contains physical PU
1 Rank 1 will run on node bb, bound to the core that contains physical
PU 8 Rank 2 will run on node cc, bound to the core that contains
physical PU 6

Rankfiles are treated as *logical* by default, and the MCA parameter
rmaps_rank_file_physical must be set to 1 to indicate that the
rankfile is to be considered as *physical*.

The hostnames listed above are "absolute," meaning that actual
resolveable hostnames are specified. However, hostnames can also be
specified as "relative," meaning that they are specified in relation
to an externally-specified list of hostnames (e.g., by prun's --host
argument, a hostfile, or a job scheduler).

The "relative" specification is of the form "+n<X>", where X is an
integer specifying the Xth hostname in the set of all available
hostnames, indexed from 0. For example:

$ cat myrankfile rank 0=+n0 slot=1:0-2 rank 1=+n1 slot=0:0,1 rank 2=+n2
slot=1-2 $ prun -H aa,bb,cc,dd -rf myrankfile ./a.out

All socket/core slot locations are be specified as *logical* indexes.
You can use tools such as HWLOC's "lstopo" to find the logical
indexes of socket and cores.

## Application Context or Executable Program?

To distinguish the two different forms, `prun` looks on the command line
for `--app` option. If it is specified, then the file named on the
command line is assumed to be an application context. If it is not
specified, then the file is assumed to be an executable program.

## Locating Files

If no relative or absolute path is specified for a file, prun will first
look for files by searching the directories specified by the `--path`
option. If there is no `--path` option set or if the file is not found
at the `--path` location, then prun will search the user's PATH
environment variable as defined on the source node(s).

If a relative directory is specified, it must be relative to the initial
working directory determined by the specific starter used. For example
when using the rsh or ssh starters, the initial directory is $HOME by
default. Other starters may set the initial directory to the current
working directory from the invocation of `prun`.

## Current Working Directory

The `-wdir` prun option (and its synonym, `-wd`) allows the user to
change to an arbitrary directory before the program is invoked. It can
also be used in application context files to specify working directories
on specific nodes and/or for specific applications.

If the `-wdir` option appears both in a context file and on the command
line, the context file directory will override the command line value.

If the `-wdir` option is specified, prun will attempt to change to the
specified directory on all of the remote nodes. If this fails, `prun`
will abort.

If the `-wdir` option is **not** specified, prun will send the directory
name where `prun` was invoked to each of the remote nodes. The remote
nodes will try to change to that directory. If they are unable (e.g., if
the directory does not exist on that node), then prun will use the
default directory determined by the starter.

All directory changing occurs before the user's program is invoked.

## Standard I/O

PSRVR directs UNIX standard input to /dev/null on all processes except
the rank 0 process. The rank 0 process inherits standard input from
`prun`. **Note:** The node that invoked `prun` need not be the same as
the node where the rank 0 process resides. PSRVR handles the redirection
of `prun`'s standard input to the rank 0 process.

PSRVR directs UNIX standard output and error from remote nodes to the
node that invoked `prun` and prints it on the standard output/error of
`prun`. Local processes inherit the standard output/error of `prun` and
transfer to it directly.

Thus it is possible to redirect standard I/O for applications by using
the typical shell redirection procedure on `prun`.

```
$ prun -np 2 my_app < my_input > my_output
```

Note that in this example *only* the rank 0 process will receive the
stream from `my_input` on stdin. The stdin on all the other nodes will
be tied to /dev/null. However, the stdout from all nodes will be
collected into the `my_output` file.

## Signal Propagation

When prun receives a SIGTERM and SIGINT, it will attempt to kill the
entire job by sending all processes in the job a SIGTERM, waiting a
small number of seconds, then sending all processes in the job a
SIGKILL.

SIGUSR1 and SIGUSR2 signals received by prun are propagated to all
processes in the job.

A SIGTSTOP signal to prun will cause a SIGSTOP signal to be sent to all
of the programs started by prun and likewise a SIGCONT signal to prun
will cause a SIGCONT sent.

Other signals are not currently propagated by prun.

## Process Termination / Signal Handling

During the run of an application, if any process dies abnormally (either
exiting before invoking `PMIx_Finalize`, or dying as the result of a
signal), `prun` will print out an error message and kill the rest of the
application.

## Process Environment

Processes in the application inherit their environment from the PSRVR
daemon upon the node on which they are running. The environment is
typically inherited from the user's shell. On remote nodes, the exact
environment is determined by the boot MCA module used. The `rsh` launch
module, for example, uses either `rsh`/`ssh` to launch the PSRVR daemon
on remote nodes, and typically executes one or more of the user's
shell-setup files before launching the daemon. When running dynamically
linked applications which require the `LD_LIBRARY_PATH` environment
variable to be set, care must be taken to ensure that it is correctly
set when booting PSRVR.

See the "Remote Execution" section for more details.

## Remote Execution

PSRVR requires that the `PATH` environment variable be set to find
executables on remote nodes (this is typically only necessary in `rsh`-
or `ssh`-based environments -- batch/scheduled environments typically
copy the current environment to the execution of remote jobs, so if the
current environment has `PATH` and/or `LD_LIBRARY_PATH` set properly,
the remote nodes will also have it set properly). If PSRVR was compiled
with shared library support, it may also be necessary to have the
`LD_LIBRARY_PATH` environment variable set on remote nodes as well
(especially to find the shared libraries required to run user
applications).

However, it is not always desirable or possible to edit shell startup
files to set `PATH` and/or `LD_LIBRARY_PATH`. The `--prefix` option
is provided for some simple configurations where this is not possible.

The `--prefix` option takes a single argument: the base directory on
the remote node where PSRVR is installed. PSRVR will use this directory
to set the remote `PATH` and `LD_LIBRARY_PATH` before executing any
user applications. This allows running jobs without having
pre-configured the `PATH` and `LD_LIBRARY_PATH` on the remote nodes.

PSRVR adds the basename of the current node's "bindir" (the directory
where PSRVR's executables are installed) to the prefix and uses that to
set the `PATH` on the remote node. Similarly, PSRVR adds the basename of
the current node's "libdir" (the directory where PSRVR's libraries
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

PSRVR will add "/remote/node/directory/bin" to the `PATH` and
"/remote/node/directory/lib64" to the D_LIBRARY_PATH on the remote
node before attempting to execute anything.

The `--prefix` option is not sufficient if the installation paths on
the remote node are different than the local node (e.g., if "/lib" is
used on the local node, but "/lib64" is used on the remote node), or
if the installation paths are something other than a subdirectory under
a common prefix.

Note that executing `prun` via an absolute pathname is equivalent to
specifying `--prefix` without the last subdirectory in the absolute
pathname to `prun`. For example:

```
$ /usr/local/bin/prun ...
```

is equivalent to

```
$ prun --prefix /usr/local
```

## Exported Environment Variables

All environment variables that are named in the form PMIX_\* will
automatically be exported to new processes on the local and remote
nodes. Environmental parameters can also be set/forwarded to the new
processes using the MCA parameter `mca_base_env_list`. While the
syntax of the `-x` option and MCA param allows the definition of new
variables, note that the parser for these options are currently not very
sophisticated - it does not even understand quoted values. Users are
advised to set variables in the environment and use the option to export
them; not to define them.

## Setting MCA Parameters

The `-pmca` switch allows the passing of parameters to various MCA
(Modular Component Architecture) modules. MCA modules have direct impact
on programs because they allow tunable parameters to be set at run time
(such as which BTL communication device driver to use, what parameters
to pass to that BTL, etc.).

The `-pmca` switch takes two arguments: `<key>` and `<value>`. The
`<key>` argument generally specifies which MCA module will receive the
value. For example, the `<key>` "btl" is used to select which BTL to
be used for transporting messages. The `<value>` argument is the value
that is passed. For example:

prun -pmca btl tcp,self -np 1 foo

:   Tells PSRVR to use the "tcp" and "self" BTLs, and to run a
    single copy of "foo" on an allocated node.

prun -pmca btl self -np 1 foo

:   Tells PSRVR to use the "self" BTL, and to run a single copy of
    "foo" on an allocated node.

The `-pmca` switch can be used multiple times to specify different
`<key>` and/or `<value>` arguments. If the same `<key>` is
specified more than once, the `<value>`s are concatenated with a comma
(",") separating them.

Note that the `-pmca` switch is simply a shortcut for setting
environment variables. The same effect may be accomplished by setting
corresponding environment variables before running `prun`. The form of
the environment variables that PSRVR sets is:

```
PMIX_MCA_<key>=<value>
```

Thus, the `-pmca` switch overrides any previously set environment
variables. The `-pmca` settings similarly override MCA parameters set in
the $PRTE_PREFIX/etc/psrvr-mca-params.conf or
$HOME/.psrvr/mca-params.conf file.

Unknown `<key>` arguments are still set as environment variable --
they are not checked (by `prun`) for correctness. Illegal or incorrect
`<value>` arguments may or may not be reported -- it depends on the
specific MCA module.

To find the available component types under the MCA architecture, or to
find the available parameters for a specific component, use the `pinfo`
command. See the *pinfo(1)* man page for detailed information on the
command.

## Setting MCA parameters and environment variables from file.

The `-tune` command line option and its synonym `-pmca
mca_base_envar_file_prefix` allows a user to set mca parameters and
environment variables with the syntax described below. This option
requires a single file or list of files separated by "," to follow.

A valid line in the file may contain zero or many "-x", "-pmca", or
"--pmca" arguments. The following patterns are supported: -pmca var val
-pmca var "val" -x var=val -x var. If any argument is duplicated in
the file, the last value read will be used.

MCA parameters and environment specified on the command line have higher
precedence than variables specified in the file.

## Running as root

The PSRVR team strongly advises against executing `prun` as the root
user. Applications should be run as regular (non-root) users.

Reflecting this advice, prun will refuse to run as root by default. To
override this default, you can add the `--allow-run-as-root` option to
the `prun` command line.

## Exit status

There is no standard definition for what `prun` should return as an exit
status. After considerable discussion, we settled on the following
method for assigning the `prun` exit status (note: in the following
description, the "primary" job is the initial application started by
prun - all jobs that are spawned by that job are designated
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

-   if the cmd line option --report-child-jobs-separately is set, we
    will return -only- the exit status of the primary job. Any non-zero
    exit status in secondary jobs will be reported solely in a summary
    print statement.

By default, PSRVR records and notes that processes exited with non-zero
termination status. This is generally not considered an "abnormal
termination" - i.e., PSRVR will not abort a job if one or more
processes return a non-zero status. Instead, the default behavior simply
reports the number of processes terminating with non-zero status upon
completion of the job.

However, in some cases it can be desirable to have the job abort when
any process terminates with non-zero status. For example, a non-PMIx job
might detect a bad result from a calculation and want to abort, but
doesn't want to generate a core file. Or a PMIx job might continue past
a call to PMIx_Finalize, but indicate that all processes should abort
due to some post-PMIx result.

It is not anticipated that this situation will occur frequently.
However, in the interest of serving the broader community, PSRVR now has
a means for allowing users to direct that jobs be aborted upon any
process exiting with non-zero status. Setting the MCA parameter
"prte_abort_on_non_zero_status" to 1 will cause PSRVR to abort
all processes once any process exits with non-zero status.

Terminations caused in this manner will be reported on the console as an
"abnormal termination", with the first process to so exit identified
along with its exit status.

# RETURN VALUE

`prun` returns 0 if all processes started by `prun` exit after calling
PMIx_Finalize. A non-zero value is returned if an internal error
occurred in prun, or one or more processes exited before calling
PMIx_Finalize. If an internal error occurred in prun, the corresponding
error code is returned. In the event that one or more processes exit
before calling PMIx_Finalize, the return value of the rank of the
process that `prun` first notices died before calling PMIx_Finalize
will be returned. Note that, in general, this will be the first process
that died but is not guaranteed to be so.

If the `--timeout` command line option is used and the timeout
expires before the job completes (thereby forcing `prun` to kill the
job) `prun` will return an exit status equivalent to the value of
`ETIMEDOUT` (which is typically 110 on Linux and OS X systems).
