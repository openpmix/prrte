# NAME

PRTE - Mapping / Binding / Ranking Processes.

# SYNOPSIS

PRTE employs a three-phase procedure for assigning process locations and ranks:

  1. **mapping**:   Assigns a default location to each process
  2. **ranking**:   Assigns a unique rank value to each process
  3. **binding**:   Constrains each process to run on specific processors

This document describes these three phases with examples.


# QUICK SUMMARY

The two binaries that most influence process layout are `prte` and `prun`.
The `prte` process discovers the allocation, starts the daemons, and defines the default mapping/ranking/binding for all jobs.
The `prun` process defines the specific mapping/ranking/binding for a specific job.
Most of the command line controls are targeted to `prun` since each job has its own unique requirements.

The `prte` process attempts to automatically discover the nodes in the allocation by querying supported resource managers. If a support resource manager is not present then `prte` relies on a hostfile provided by the user. In the absence of such a hostfile it will run all processes on the localhost.

If running under a supported resource manager, the `prte` process will start the daemon processes (`prted`) on the remote nodes using the corresponding resource manager process starter. If no such starter is available then `rsh` or `ssh` is used.

Nodes are scheduled (by default) in a round-robin fassion by CPU slot.

The `prun` process automatically binds processes. Three binding patterns are used in the absence of any further directives:

 * `Bind to core` : when the number of processes is <= 2
 * `Bind to socket` : when the number of processes is > 2
 * `Bind to none` : when oversubscribed

If your application uses threads, then you probably want to ensure that you are either not bound at all (by specifying `--bind-to none`), or bound to multiple cores using an appropriate binding level or specific number of processing elements per application process.


# OPTIONS

Listed here are the subset of command line options related to process mapping/ranking/binding.

Unless otherwise noted, these should be passed to the `prun` process.

## Specifying Host Nodes

Use one of the following options to specify which hosts (nodes) within
the PRRTE DVM environment to run on.

`-H, --host <host1,host2,...,hostN>`

:   List of hosts on which to invoke processes.

`--hostfile <hostfile>`

:   Provide a hostfile to use.

`--default-hostfile <hostfile>`

:   Provide a default hostfile.

`--machinefile <machinefile>`

:   Synonym for `--hostfile`.

## Process Mapping / Ranking / Binding Options

The following options specify the number of processes to launch. Note
that none of the options imply a particular binding policy - e.g.,
requesting `N` processes for each socket does not imply that the processes
will be bound to the socket.

`-c, -n, --n, --np <#>`

:   Run this many copies of the program on the given nodes. This option
    indicates that the specified file is an executable program and not
    an application context. If no value is provided for the number of
    copies to execute (i.e., neither the `-np` nor its synonyms are
    provided on the command line), `prun` will automatically execute a
    copy of the program on each process slot (see below for description
    of a "process slot"). This feature, however, can only be used in
    the SPMD model and will return an error (without beginning execution
    of the application) otherwise.

To map processes across sets of objects:

`--map-by <object>`

:   Map to the specified object. See defaults in Quick Summary. Supported
    options include `slot`, `hwthread`, `core`, `L1cache`, `L2cache`, `L3cache`,
    `socket`, `node`, `seq`, `dist`, and `ppr`. Any object can include modifiers
    by adding a : and any combination of `PE=n` (bind n processing elements to
    each process), `SPAN` (load balance the processes across the allocation),
    `OVERSUBSCRIBE` (allow more processes on a node than processing elements),
    `NOOVERSUBSCRIBE` (`!OVERSUBSCRIBE`), `NOLOCAL` (do not launch processes on
    the same node as `prun`), `HWTCPUS` (use hardware threads as cpu slots),
    `CORECPUS` (use cores as cpu slots), `DEVICE` (for `dist` policy),
    `INHERIT` (??), `NOINHERIT` (`!INHERIT`), `PE-LIST=a,b` (comma-delimited
    ranges of cpus to use for this job).
    `ppr` policy example: `--map-by ppr:N:<object>` will launch `N` times the
    number of objects of the specified type on each node.

To order processes' ranks:

`--rank-by <object>`

:   Rank in round-robin fashion according to the specified object. See defaults
    in Quick Summary.
    Supported options include `slot`, `hwthread`, `core`, `L1cache`, `L2cache`,
    `L3cache`, `socket`, and `node`. Any object can include modifiers by adding
    either `:SPAN` (??) or `:FILL` (??).

To bind processes to sets of objects:

`--bind-to <object>`

:   Bind processes to the specified object. See defaults in Quick Summary.
    Supported options include `none`, `slot`, `hwthread`, `core`, `l1cache`,
    `l2cache`, `l3cache`, and `socket`. Any object can include modifiers
    by adding a : and any combination of `overload-allowed` (if overloading
    of this object is allowed), and `if-supported` (if that object is
    supported on this system).


## Diagnostics

`--display-map`

:   Display a table showing the mapped location of each process prior to
    launch.

`--display-allocation`

:   Display the detected allocation of resources (e.g., nodes, slots)

`--report-bindings`

:   Report bindings for launched processes to `stderr`.

`--display-devel-allocation`

:   Display a detailed list of the allocation being used by this job.

`--display-devel-map`

:   Display a more detailed table showing the mapped location of each
    process prior to launch.

`--display-diffable-map`

:   Display a diffable process map just before launch.

`--display-topo`

:   Display the topology as part of the process map just before launch.

`--do-not-launch`

:   Perform all necessary operations to prepare to launch the
    application, but do not actually launch it. This can be helpful when
    testing mapping patterns.


# DESCRIPTION

PRTE employs a three-phase procedure for assigning process locations and ranks:

  1. **mapping**:   Assigns a default location to each process
  2. **ranking**:   Assigns a unique rank value to each process
  3. **binding**:   Constrains each process to run on specific processors

The first phase of **mapping** is used to assign a default location to each process based on the mapper being employed. Mapping by slot, node, and sequentially results in the assignment of the processes to the node level. In contrast, mapping by object, allows the mapper to assign the process to an actual object on each node.

*Note:* The location assigned to the process is independent of where it will be bound - the assignment is used solely as input to the binding algorithm.

The second phase focuses on the **ranking** of the process within the job's namespace. PRRTE separates this from the mapping procedure to allow more flexibility in the relative placement of processes.

The third phase of *binding* actually binds each process to a given set of processors. This can improve performance if the operating system is placing processes suboptimally. For example, it might oversubscribe some multi-core processor sockets, leaving other sockets idle; this can lead processes to contend unnecessarily for common resources. Or, it might spread processes out too widely; this can be suboptimal if applicatio performance is sensitive to interprocess communication costs. Binding can also keep the operating system from migrating processes excessively, regardless of how optimally those processes were placed to begin with.

PRRTE's support for process binding depends on the underlying operating system. Therefore, certain process binding options may not be available on every system.


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
$ cat myhostfile
aa slots=4
bb slots=4
cc slots=4
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

```
                      node aa      node bb      node cc
prun                  0 1 2 3      4 5
prun --map-by node    0 3          1 4          2 5
prun -nolocal                      0 1 2 3      4 5
```

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


## Mapping, Ranking, and Binding: Fundamentals

The mapping of process processes to nodes can be defined not just with
general policies but also, if necessary, using arbitrary mappings that
cannot be described by a simple policy. One can use the "sequential
mapper," which reads the hostfile line by line, assigning processes to
nodes in whatever order the hostfile specifies. Use the `-pmca rmaps
seq` option.

For example, using the hostfile below:
```
% cat myhostfile
aa slots=4
bb slots=4
cc slots=4
```

The command below will launch three processes, one on each of nodes aa, bb, and cc, respectively. The slot counts don't matter;  one process is launched per line on whatever node is listed on the line.
```
% prte -hostfile myhostfile -prtemca rmaps seq &
% prun ./a.out
```

Or if you have a one-off wrapper run command (e.g., `mpirun`):
```
% mpirun -hostfile myhostfile -prtemca rmaps seq ./a.out
```

*ranking* is best illustrated by considering the following two cases where we used the `--map-by ppr:2:socket` option:

```
                      node aa       node bb
rank-by core          0 1 ! 2 3     4 5 ! 6 7
rank-by socket        0 2 ! 1 3     4 6 ! 5 7
rank-by socket:span   0 4 ! 1 5     2 6 ! 3 7
```

Ranking by core and by slot provide the identical result - a simple
progression of ranks across each node. Ranking by socket does a
round-robin ranking within each node until all processes have been
assigned a rank, and then progresses to the next node. Adding the `span`
modifier to the ranking directive causes the ranking algorithm to treat
the entire allocation as a single entity - thus, the MCW ranks are
assigned across all sockets before circling back around to the
beginning.

The *binding* phase ....

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


(Is the below true? MCA parmaters can be used instead of CLI options?)

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

## Mapping, Ranking, and Binding: Advanced

Cgroups


## Diagnostics

PRRTE provides various diagnostic reports that aid the user in verifying and tuning the mapping /binding / ranking for a specific job.

The `--report-bindings` command line option can be used to report bindings.

As an example, consider a node with two processor sockets, each comprising four cores.  We run `prun` with `-np 4 --report-bindings` and the following additional options:

```
 $ prun ... --map-by core --bind-to core ./a.out
 [...] ... binding child [...,0] to cpus 0001
 [...] ... binding child [...,1] to cpus 0002
 [...] ... binding child [...,2] to cpus 0004
 [...] ... binding child [...,3] to cpus 0008

 $ prun ... --map-by socket --bind-to socket ./a.out
 [...] ... binding child [...,0] to socket 0 cpus 000f
 [...] ... binding child [...,1] to socket 1 cpus 00f0
 [...] ... binding child [...,2] to socket 0 cpus 000f
 [...] ... binding child [...,3] to socket 1 cpus 00f0

 $ prun ... --map-by core:PE=2 --bind-to core ./a.out
 [...] ... binding child [...,0] to cpus 0003
 [...] ... binding child [...,1] to cpus 000c
 [...] ... binding child [...,2] to cpus 0030
 [...] ... binding child [...,3] to cpus 00c0

 $ prun ... --bind-to none ./a.out
```

Here, `--report-bindings` shows the binding of each process as a mask. In the first case, the processes bind to successive cores as indicated by the masks 0001, 0002, 0004, and 0008.  In the second case, processes bind to all cores on successive sockets as indicated by the masks 000f and 00f0. The processes cycle through the processor sockets in a round-robin fashion as many times as are needed.  In the third case, the masks show us that 2 cores have been bound per process.  In the fourth case, binding is turned off and no bindings are reported.


## Rankfiles

Another way to specify arbitrary mappings is with a rankfile, which gives you detailed control over process binding as well.  Rankfiles are discussed below.

--
Rankfiles are text files that specify detailed information about how individual processes should be mapped to nodes, and to which processor(s) they should be bound. Each line of a rankfile specifies the location of one process. The general form of each line in the rankfile is:

```
rank <N>=<hostname> slot=<slot list>
```

For example:
```
$ cat myrankfile
rank 0=aa slot=1:0-2
rank 1=bb slot=0:0,1
rank 2=cc slot=1-2
$ prun -H aa,bb,cc,dd -rf myrankfile ./a.out
```

Means that

```
Rank 0 runs on node aa, bound to logical socket 1, cores 0-2.
Rank 1 runs on node bb, bound to logical socket 0, cores 0 and 1.
Rank 2 runs on node cc, bound to logical cores 1 and 2.
```

Rankfiles can alternatively be used to specify *physical* processor locations. In this case, the syntax is somewhat different. Sockets are no longer recognized, and the slot number given must be the number of the physical PU as most OS's do not assign a unique physical identifier to each core in the node. Thus, a proper physical rankfile looks something like the following:

```
$ cat myphysicalrankfile
rank 0=aa slot=1
rank 1=bb slot=8
rank 2=cc slot=6
```

This means that

```
Rank 0 will run on node aa, bound to the core that contains physical PU 1
Rank 1 will run on node bb, bound to the core that contains physical PU 8
Rank 2 will run on node cc, bound to the core that contains physical PU 6
```

Rankfiles are treated as *logical* by default, and the MCA parameter `rmaps_rank_file_physical` must be set to `1` to indicate that the rankfile is to be considered as *physical*.

The hostnames listed above are "absolute," meaning that actual resolveable hostnames are specified. However, hostnames can also be specified as "relative," meaning that they are specified in relation to an externally-specified list of hostnames (e.g., by prun's `--host` argument, a hostfile, or a job scheduler).

The "relative" specification is of the form "`+n<X>`", where `X` is an integer specifying the Xth hostname in the set of all available hostnames, indexed from 0. For example:

```
$ cat myrankfile
rank 0=+n0 slot=1:0-2
rank 1=+n1 slot=0:0,1
rank 2=+n2 slot=1-2
$ prun -H aa,bb,cc,dd -rf myrankfile ./a.out
```

All socket/core slot locations are be specified as *logical* indexes. You can use tools such as HWLOC's "lstopo" to find the logical indexes of socket and cores.


# Examples

## ...
