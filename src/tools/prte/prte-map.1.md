# NAME

PRTE: Mapping, Ranking, and Binding

# SYNOPSIS

PRTE employs a three-phase procedure for assigning process locations and ranks:

  1. **mapping**:   Assigns a default location to each process
  2. **ranking**:   Assigns a unique rank value to each process
  3. **binding**:   Constrains each process to run on specific processors

This document describes these three phases with examples. Unless
otherwise noted, this behavior is shared by `prun`, `prterun`, and `prte`.

# QUICK SUMMARY

The two binaries that most influence process layout are `prte` and `prun`.
The `prte` process discovers the allocation, starts the daemons, and defines the
default mapping/ranking/binding for all jobs.
The `prun` process defines the specific mapping/ranking/binding for a specific
job. Most of the command line controls are targeted to `prun` since each job has
its own unique requirements.

`prterun` is just a wrapper around `prte` for a single job PRTE DVM. It is
doing the job of both `prte` and `prun`, and, as such, accepts the sum all of
their command line arguments. Any example that uses `prun` can substitute the
use of `prterun` except where otherwise noted.

The `prte` process attempts to automatically discover the nodes in the
allocation by querying supported resource managers. If a support resource
manager is not present then `prte` relies on a hostfile provided by the user.
In the absence of such a hostfile it will run all processes on the localhost.

If running under a supported resource manager, the `prte` process will start the
daemon processes (`prted`) on the remote nodes using the corresponding resource
manager process starter. If no such starter is available then `rsh` or `ssh`
is used.

PRTE automatically maps processes in a round-robin fashion by CPU slot
in one of two ways in the absence of any further directives:

`Map by core:`

:   when the number of total processes in the job is <= 2

`Map by package:`

:   when the number of total processes in the job is > 2

PRTE automatically binds processes. Three binding patterns are used in the
absence of any further directives:

`Bind to core:`

:   when the number of total processes in the job is <= 2

`Bind to package:`

:   when the number of total processes in the job is > 2

`Bind to none:`

:   when oversubscribed

If your application uses threads, then you probably want to ensure that
you are either not bound at all (by specifying `--bind-to none`), or
bound to multiple cores using an appropriate binding level or specific
number of processing elements per application process.

PRTE automatically ranks processes starting from 0. Two ranking patterns are
used in the absence of any further directives:

`Rank by slot:`

:   when the number of total processes in the job is <= 2

`Rank by package:`

:   when the number of total processes in the job is > 2

# OPTIONS

Listed here are the subset of command line options that will be used in the
process mapping/ranking/binding discussion in this manual page.

## Specifying Host Nodes

Use one of the following options to specify which hosts (nodes) within
the PRTE DVM environment to run on.

`--host <host1,host2,...,hostN>` or `--host <host1:X,host2:Y,...,hostN:Z>`

:   List of hosts on which to invoke processes. After each hostname a
    colon (`:`) followed by a positive integer can be used to specify the
    number of slots on that host (`:X`, `:Y`, and `:Z`). The default is `1`.

`--hostfile <hostfile>`

:   Provide a hostfile to use.

`--machinefile <machinefile>`

:   Synonym for `-hostfile`.

`--default-hostfile <hostfile>`

:   Provide a default hostfile to use.


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
    options include `slot`, `hwthread`, `core`, `l1cache`, `l2cache`, `l3cache`,
    `package`, `node`, `seq`, `dist`, `ppr`, and `rankfile`.

Any object can include qualifier by adding a colon (`:`) and any combination
of one or more of the following to the `--map-by` option:

 - `PE=n` bind `n` processing elements to each process
 - `SPAN` load balance the processes across the allocation
 - `OVERSUBSCRIBE` allow more processes on a node than processing elements
 - `NOOVERSUBSCRIBE` means `!OVERSUBSCRIBE`
 - `NOLOCAL` do not launch processes on the same node as `prun`
 - `HWTCPUS` use hardware threads as CPU slots
 - `CORECPUS` use cores as CPU slots (default)
 - `DEVICE=dev` device specifier for the `dist` policy
 - `INHERIT`
 - `NOINHERIT` means `!INHERIT`
 - `PE-LIST=a,b` comma-delimited ranges of cpus to use for this job processed as an unordered pool of CPUs
 - `FILE=%s` (path to file containing sequential or rankfile entries).

`ppr` policy example: `--map-by ppr:N:<object>` will launch `N` times the
number of objects of the specified type on each node.

To order processes' ranks:

`--rank-by <object>`

:   Rank in round-robin fashion according to the specified object. See defaults
    in Quick Summary.
    Supported options include `slot`, `hwthread`, `core`, `l1cache`, `l2cache`,
    `l3cache`, `package`, and `node`.

Any object can include qualifiers by adding a colon (`:`) and any combination
of one or more of the following to the `--rank-by` option:

 - `SPAN`
 - `FILL`

To bind processes to sets of objects:

`--bind-to <object>`

:   Bind processes to the specified object. See defaults in Quick Summary.
    Supported options include `none`, `hwthread`, `core`, `l1cache`,
    `l2cache`, `l3cache`, and `package`.

Any object can include qualifiers by adding a colon (`:`) and any combination
of one or more of the following to the `--bind-to` option:

 - `overload-allowed` allows for binding more than one process in relation to a CPU
 - `if-supported` if that object is supported on this system

## Diagnostics

`--map-by :DISPLAY`

:   Display a table showing the mapped location of each process prior to
    launch.

`--map-by :DISPLAYALLOC`

:   Display the detected allocation of resources (e.g., nodes, slots)

`--bind-to :REPORT`

:   Report bindings for launched processes to `stderr`.

# DESCRIPTION

PRTE employs a three-phase procedure for assigning process locations and ranks:

  1. **mapping**:   Assigns a default location to each process
  2. **ranking**:   Assigns a unique rank value to each process
  3. **binding**:   Constrains each process to run on specific processors

The first phase of **mapping** is used to assign a default location to each
process based on the mapper being employed. Mapping by slot, node, and
sequentially results in the assignment of the processes to the node level. In
contrast, mapping by object, allows the mapper to assign the process to an
actual object on each node.

*Note:* The location assigned to the process is independent of where it will be
bound - the assignment is used solely as input to the binding algorithm.

The second phase focuses on the **ranking** of the process within the job's
namespace. PRTE separates this from the mapping procedure to allow more
flexibility in the relative placement of processes.

The third phase of **binding** actually binds each process to a given set of
processors. This can improve performance if the operating system is placing
processes sub-optimally. For example, it might oversubscribe some multi-core
processor sockets, leaving other sockets idle; this can lead processes to
contend unnecessarily for common resources. Or, it might spread processes out
too widely; this can be suboptimal if application performance is sensitive to
interprocess communication costs. Binding can also keep the operating system
from migrating processes excessively, regardless of how optimally those
processes were placed to begin with.

PRTE's support for process binding depends on the underlying operating system.
Therefore, certain process binding options may not be available on every system.


## Specifying Host Nodes

Host nodes can be identified on the command line with the `--host` option or
in a hostfile.

For example, assuming no other resource manager or scheduler is involved,

`prte --host aa,aa,bb ./a.out`

:   launches two processes on node `aa` and one on `bb`.

`prun --host aa ./a.out`

:   launches one process on node `aa`.

`prun --host aa:5 ./a.out`

:   launches five processes on node `aa`.

Or, consider the hostfile

```
$ cat myhostfile
aa slots=2
bb slots=2
cc slots=2
```

Here, we list both the host names (`aa`, `bb`, and `cc`) but also how
many "slots" there are for each. Slots indicate how many processes can
potentially execute on a node. For best performance, the number of
slots may be chosen to be the number of cores on the node or the
number of processor sockets.

If the hostfile does not provide slots information, the PRTE DVM will attempt
to discover the number of cores (or hwthreads, if the `:HWTCPUS` qualifier to
the `--map-by` option is set) and set the number of slots to that value.

Examples using the hostfile above with and without the `--host` option

`prun --hostfile myhostfile ./a.out`

:   will launch two processes on each of the three nodes.

`prun --hostfile myhostfile --host aa ./a.out`

:   will launch two processes, both on node `aa`.

`prun --hostfile myhostfile --host dd ./a.out`

:   will find no hosts to run on and abort with an error. That is, the
    specified host `dd` is not in the specified hostfile.

When running under resource managers (e.g., SLURM, Torque, etc.), PRTE will
obtain both the hostnames and the number of slots directly from the resource
manger. The behavior of `--host` in that environment will behave the same as
if a hostfile was provided (since it is provided by the resource manager).


## Specifying Number of Processes

As we have just seen, the number of processes to run can be set using
the hostfile. Other mechanisms exist.

The number of processes launched can be specified as a multiple of the
number of nodes or processor sockets available. Consider the hostfile below for
the examples that follow.

```
$ cat myhostfile
aa
bb
```

For example,

`prun --hostfile myhostfile --map-by ppr:2:package ./a.out`

:   launches processes 0-3 on node `aa` and process 4-7 on node `bb`, where
    `aa` and `bb` are both dual-package nodes. The `--map-by ppr:2:package`
    option also turns on the `--bind-to package` option, which is discussed
    in a later section.

`prun --hostfile myhostfile --map-by ppr:2:node ./a.out`

:   launches processes 0-1 on node `aa` and processes 2-3 on node `bb`.

`prun --hostfile myhostfile --map-by ppr:1:node ./a.out`

:   launches one process per host node.

Another alternative is to specify the number of processes with the `--np`
option. Consider now the hostfile

```
$ cat myhostfile
aa slots=4
bb slots=4
cc slots=4
```

Now,

`prun --hostfile myhostfile --np 6 ./a.out`

:   will launch processes 0-3 on node `aa` and processes 4-5 on node `bb`.
    The remaining slots in the hostfile will not be used since the `-np`
    option indicated that only 6 processes should be launched.

## Mapping Processes to Nodes: Using Policies

The examples above illustrate the default mapping of process processes
to nodes. This mapping can also be controlled with various `prun`/`prterun`
options that describe mapping policies.

```
$ cat myhostfile
aa slots=4
bb slots=4
cc slots=4
```

Consider the hostfile above, with `--np 6`:

```
                              node aa      node bb      node cc
prun                          0 1 2 3      4 5
prun --map-by node            0 1          2 3          4 5
prun --map-by node:NOLOCAL                 0 1 2        3 4 5
```

The `--map-by node` option will load balance the processes across the
available nodes, numbering each process in a round-robin fashion.

The `:NOLOCAL` qualifier to `--map-by` prevents any processes from being mapped
onto the local host (in this case node `aa`). While `prun` typically consumes
few system resources, the `:NOLOCAL` qualifier can be helpful for launching
very large jobs where `prun` may actually need to use noticeable amounts of
memory and/or processing time.

Just as `--np` can specify fewer processes than there are slots, it can
also oversubscribe the slots. For example, with the same hostfile:

`prun --hostfile myhostfile --np 14 ./a.out`

:   will produce an error since the default `:NOOVERSUBSCRIBE` qualifier to
    `--map-by` prevents oversubscription.

To oversubscribe the nodes you can use the `:OVERSUBSCRIBE` qualifier to
`--map-by`:

`prun --hostfile myhostfile --np 14 --map-by :OVERSUBSCRIBE ./a.out`

:   will launch processes 0-5 on node `aa`, 6-9 on `bb`, and 10-13 on `cc`.

<!--
// JJH TODO -- this does not work see https://github.com/openpmix/prrte/issues/770

Limits to oversubscription can also be specified in the hostfile itself:
```
% cat myhostfile
aa slots=4 max_slots=4
bb         max_slots=8
cc slots=4
```

The `max_slots` field specifies such a limit. When it does, the `slots`
value defaults to the limit. Now:

`prun --hostfile myhostfile --np 14 --map-by :OVERSUBSCRIBE ./a.out`

:   causes the first 12 processes to be launched as before, but the
    remaining two processes will be forced onto node cc. The other two
    nodes are protected by the hostfile against oversubscription by this
    job.

Using the `:NOOVERSUBSCRIBE` qualifier to `--map-by` option can be helpful
since the PRTE DVM currently does not get "max_slots" values from the
resource manager.
-->

Of course, `--np` can also be used with the `--host` option. For
example,

`prun --host aa,bb --np 8 ./a.out`

:   will produce an error since the default `:NOOVERSUBSCRIBE` qualifier to
    `--map-by` prevents oversubscription.

`prun --host aa,bb --np 8 --map-by :OVERSUBSCRIBE ./a.out`

:   launches 8 processes. Since only two hosts are specified, after the
    first two processes are mapped, one to `aa` and one to `bb`, the
    remaining processes oversubscribe the specified hosts evenly.

`prun --host aa:2,bb:6 --np 8 ./a.out`

:   launches 8 processes. Processes 0-1 on node `aa` since it has 2 slots and
    processes 2-7 on node `bb` since it has 6 slots.

And here is a MIMD example:

`prun --host aa --np 1 hostname : --host bb,cc --np 2 uptime`

:   will launch process 0 running `hostname` on node `aa` and processes 1
    and 2 each running `uptime` on nodes `bb` and `cc`, respectively.

## Mapping, Ranking, and Binding: Fundamentals

The mapping of process processes to nodes can be defined not just with
general policies but also, if necessary, using arbitrary mappings that
cannot be described by a simple policy. One can use the "sequential
mapper," which reads the hostfile line by line, assigning processes to
nodes in whatever order the hostfile specifies. Use the
`--prtemca rmaps seq` option.

For example, using the hostfile below:
```
% cat myhostfile
aa slots=4
bb slots=4
cc slots=4
```

The command below will launch three processes, one on each of nodes `aa`, `bb`,
and `cc`, respectively. The slot counts don't matter; one process is launched
per line on whatever node is listed on the line.
```
% prun --hostfile myhostfile --prtemca rmaps seq ./a.out
```

The *ranking* phase is best illustrated by considering the following hostfile
and test cases we used the `--map-by ppr:2:package` option:

```
% cat myhostfile
aa
bb
```

```
                         node aa       node bb
--rank-by core           0 1 ! 2 3     4 5 ! 6 7
--rank-by package        0 2 ! 1 3     4 6 ! 5 7
--rank-by package:SPAN   0 4 ! 1 5     2 6 ! 3 7
```

Ranking by core and by slot provide the identical result - a simple
progression of ranks across each node. Ranking by package does a
round-robin ranking within each node until all processes have been
assigned a rank, and then progresses to the next node. Adding the `:SPAN`
qualifier to the ranking directive causes the ranking algorithm to treat
the entire allocation as a single entity - thus, the process ranks are
assigned across all sockets before circling back around to the
beginning.

The *binding* phase restricts the process to a subset of the CPU resources
on the node.

The processors to be used for binding can be identified in terms of
topological groupings - e.g., binding to an l3cache will bind each
process to all processors within the scope of a single L3 cache within
their assigned location. Thus, if a process is assigned by the mapper to
a certain package, then a `--bind-to l3cache` directive will cause the
process to be bound to the processors that share a single L3 cache
within that package.

To help balance loads, the binding directive uses a round-robin method
when binding to levels lower than used in the mapper. For example,
consider the case where a job is mapped to the package level, and then
bound to core. Each package will have multiple cores, so if multiple
processes are mapped to a given package, the binding algorithm will
assign each process located to a package to a unique core in a
round-robin manner.

Alternatively, processes mapped by l2cache and then bound to package will
simply be bound to all the processors in the package where they are
located. In this manner, users can exert detailed control over relative
process location and binding.

Process mapping/ranking/binding can also be set with MCA parameters. Their
usage is less convenient than that of the command line options. On the other
hand, MCA parameters can be set not only on the `prun` command line, but
alternatively in a system or user `mca-params.conf` file or as environment
variables, as described in the MCA section below. Some examples include:

```
prun option          MCA parameter key           value
--map-by core        rmaps_base_mapping_policy   core
--map-by package     rmaps_base_mapping_policy   package
--rank-by core       rmaps_base_ranking_policy   core
--bind-to core       hwloc_base_binding_policy   core
--bind-to package    hwloc_base_binding_policy   package
--bind-to none       hwloc_base_binding_policy   none
```

## Difference between overloading and oversubscription

This section explores the difference between these two options. Users are often
confused by the difference between these two scenarios. As such this section
provides a number of scenarios to help illustrate the differences.

 - `--map-by :OVERSUBSCRIBE` allow more processes on a node than processing elements
 - `--bind-to <object>:overload-allowed` allows for binding more than one process in relation to a CPU

The important thing to remember with _oversubscribing_ is that it can be
defined separately from the actual number of CPUs on a node. This allows the
mapper to place more or fewer processes per node than CPUs. By default, PRTE
uses cores to determine slots in the absence of such information provided in
the hostfile or by the resource manager (except in the case of the `--host`
as described in the "Specifying Host Nodes" section).

The important thing to remember with _overloading_ is that it is defined as
binding more processes than CPUs. By default, PRTE uses cores as a means of
counting the number of CPUs. However, the user can adjust this. For example
when using the `:HWTCPUS` qualifier to the `--map-by` option PRTE will use
hardware threads as a means of counting the number of CPUs.

For the following examples consider a node with:
 - Two processor packages,
 - Ten cores per package, and
 - Eight hardware threads per core.

Consider the node from above with the hostfile below:

```
$ cat myhostfile
node01 slots=32
node02 slots=32
```

The "slots" tells PRTE that it can place up to 32 processes before
_oversubscribing_ the node.

If we run the following:
```
prun --np 34 --hostfile myhostfile --map-by core --bind-to core hostname
```

It will return an error at the binding time indicating an _overloading_ scenario.

The mapping mechanism assigns 32 processes to `node01` matching the "slots"
specification in the hostfile. The binding mechanism will bind the first 20
processes to unique cores leaving it with 12 processes that it cannot bind
without overloading one of the cores (putting more than one process on the
core).

Using the `overload-allowed` qualifier to the `--bind-to core` option tells
PRTE that it may assign more than one process to a core.

If we run the following:
```
prun --np 34 --hostfile myhostfile --map-by core --bind-to core:overload-allowed hostname
```

This will run correctly placing 32 processes on `node01`, and 2 processes on
`node02`. On `node01` two processes are bound to cores 0-11 accounting for
the overloading of those cores.

Alternatively, we could use hardware threads to give binding a lower level
CPU to bind to without overloading.

If we run the following:
```
prun --np 34 --hostfile myhostfile --map-by core:HWTCPUS --bind-to hwthread hostname
```

This will run correctly placing 32 processes on `node01`, and 2 processes on
`node02`. On `node01` two processes are mapped to cores 0-11 but bound to
different hardware threads on those cores (the logical first and second
hardware thread) thus no hardware threads are overloaded at binding time.

In both of the examples above the node is not oversubscribed at mapping time
because the hostfile set the oversubscription limit to "slots=32" for each
node. It is only after we exceed that limit that PRTE will throw an
oversubscription error.

Consider next if we ran the following:
```
prun --np 66 --hostfile myhostfile --map-by core:HWTCPUS --bind-to hwthread hostname
```

This will return an error at mapping time indicating an oversubscription
scenario. The mapping mechanism will assign all of the available slots
(64 across 2 nodes) and be left two processes to map. The only way to map
those processes is to exceed the number of available slots putting the job
into an oversubscription scenario.

You can force PRTE to oversubscribe the nodes by using the `:OVERSUBSCRIBE`
qualifier to the `--map-by` option as seen in the example below:
```
prun --np 66 --hostfile myhostfile --map-by core:HWTCPUS:OVERSUBSCRIBE --bind-to hwthread hostname
```

This will run correctly placing 34 processes on `node01` and 32 on `node02`.
Each process is bound to a unique hardware thread.

### Overloading vs Oversubscription: Package Example

Let's extend these examples by considering the package level.
Consider the same node as before, but with the hostfile below:
```
$ cat myhostfile
node01 slots=22
node02 slots=22
```

The lowest level CPUs are 'cores' and we have 20 total (10 per package).

If we run:
```
prun --np 20 --hostfile myhostfile --map-by package --bind-to package:REPORT hostname
```

Then 10 processes are mapped to each package, and bound at the package level.
This is not overloading since we have 10 CPUs (cores) available in the package
at the hardware level.

However, if we run:
```
prun --np 21 --hostfile myhostfile --map-by package --bind-to package:REPORT hostname
```

Then 11 processes are mapped to the first package and 10 to the second package.
At binding time we have an overloading scenario because there are only
10 CPUs (cores) available in the package at the hardware level. So the first
package is overloaded.

### Overloading vs Oversubscription: Hardware Threads Example

Similarly, if we consider hardware threads.

Consider the same node as before, but with the hostfile below:
```
$ cat myhostfile
node01 slots=165
node02 slots=165
```

The lowest level CPUs are 'hwthreads' (because we are going to use the
`:HWTCPUS` qualifier) and we have 160 total (80 per package).

If we re-run (from the package example) and add the `:HWTCPUS` qualifier:
```
prun --np 21 --hostfile myhostfile --map-by package:HWTCPUS --bind-to package:REPORT hostname
```

Without the `:HWTCPUS` qualifier this would be overloading (as we saw
previously). The mapper places 11 processes on the first package and 10 to the
second package. The processes are still bound to the package level. However,
with the `:HWTCPUS` qualifier, it is not overloading since we have
80 CPUs (hwthreads) available in the package at the hardware level.

Alternatively, if we run:
```
prun --np 161 --hostfile myhostfile --map-by package:HWTCPUS --bind-to package:REPORT hostname
```

Then 81 processes are mapped to the first package and 80 to the second package.
At binding time we have an overloading scenario because there are only
80 CPUs (hwthreads) available in the package at the hardware level.
So the first package is overloaded.


## Diagnostics

PRTE provides various diagnostic reports that aid the user in verifying and
tuning the mapping/ranking/binding for a specific job.

The `:REPORT` qualifier to `--bind-to` command line option can be used to
report process bindings.

As an example, consider a node with:
 - Two processor packages,
 - Four cores per package, and
 - Eight hardware threads per core.

In each of the examples below the binding is reported in a human readable
format.

```
$ prun --np 4 --map-by core --bind-to core:REPORT ./a.out
[node01:103137] MCW rank 0 bound to package[0][core:0]
[node01:103137] MCW rank 1 bound to package[0][core:1]
[node01:103137] MCW rank 2 bound to package[0][core:2]
[node01:103137] MCW rank 3 bound to package[0][core:3]
```

The example above processes bind to successive cores on the first package.

```
$ prun --np 4 --map-by package --bind-to package:REPORT ./a.out
[node01:103115] MCW rank 0 bound to package[0][core:0-9]
[node01:103115] MCW rank 1 bound to package[1][core:10-19]
[node01:103115] MCW rank 2 bound to package[0][core:0-9]
[node01:103115] MCW rank 3 bound to package[1][core:10-19]
```

The example above processes bind to all cores on successive packages.
The processes cycle though the packages in a round-robin fashion as many times
as are needed.

```
$ prun --np 4 --map-by package:PE=2 --bind-to core:REPORT ./a.out
[node01:103328] MCW rank 0 bound to package[0][core:0-1]
[node01:103328] MCW rank 1 bound to package[1][core:10-11]
[node01:103328] MCW rank 2 bound to package[0][core:2-3]
[node01:103328] MCW rank 3 bound to package[1][core:12-13]
```

The example above shows us that 2 cores have been bound per process.
The `:PE=2` qualifier states that 2 processing elements underneath the package
(which would be cores in this case) are mapped to each process.
The processes cycle though the packages in a round-robin fashion as many times
as are needed.


```
$ prun --np 4 --map-by core:PE=2:HWTCPUS --bind-to :REPORT  hostname
[node01:103506] MCW rank 0 bound to package[0][hwt:0-1]
[node01:103506] MCW rank 1 bound to package[0][hwt:8-9]
[node01:103506] MCW rank 2 bound to package[0][hwt:16-17]
[node01:103506] MCW rank 3 bound to package[0][hwt:24-25]
```

The example above shows us that 2 hardware threads have been bound per process.
In this case `prun` is mapping by hardware threads since we used the `:HWTCPUS`
qualifier. Without that qualifier this command would return an error since
by default `prun` will not map to resources smaller than a core.
The `:PE=2` qualifier states that 2 processing elements underneath the core
(which would be hardware threads in this case) are mapped to each process.
The processes cycle though the cores in a round-robin fashion as many times
as are needed.

```
$ prun --np 4 --bind-to none:REPORT  hostname
[node01:107126] MCW rank 0 is not bound (or bound to all available processors)
[node01:107126] MCW rank 1 is not bound (or bound to all available processors)
[node01:107126] MCW rank 2 is not bound (or bound to all available processors)
[node01:107126] MCW rank 3 is not bound (or bound to all available processors)
```

The example above binding is turned off.

## Rankfiles

Another way to specify arbitrary mappings is with a rankfile, which gives you
detailed control over process binding as well.

Rankfiles are text files that specify detailed information about how individual
processes should be mapped to nodes, and to which processor(s) they should be
bound. Each line of a rankfile specifies the location of one process. The
general form of each line in the rankfile is:

```
rank <N>=<hostname> slot=<slot list>
```

For example:
```
$ cat myrankfile
rank 0=c712f6n01 slot=10-12
rank 1=c712f6n02 slot=0,1,4
rank 2=c712f6n03 slot=1-2
$ prun --host aa,bb,cc,dd --map-by rankfile:FILE=myrankfile ./a.out
```

Means that

```
Rank 0 runs on node aa, bound to logical cores 10-12.
Rank 1 runs on node bb, bound to logical cores 0, 1, and 4.
Rank 2 runs on node cc, bound to logical cores 1 and 2.
```

For example:
```
$ cat myrankfile
rank 0=aa slot=1:0-2
rank 1=bb slot=0:0,1,4
rank 2=cc slot=1-2
$ prun --host aa,bb,cc,dd --map-by rankfile:FILE=myrankfile ./a.out
```

Means that

```
Rank 0 runs on node aa, bound to logical package 1, cores 10-12 (the 0th through 2nd cores on that package).
Rank 1 runs on node bb, bound to logical package 0, cores 0, 1, and 4.
Rank 2 runs on node cc, bound to logical cores 1 and 2.
```

The hostnames listed above are "absolute," meaning that actual resolvable
hostnames are specified. However, hostnames can also be specified as
"relative," meaning that they are specified in relation to an
externally-specified list of hostnames (e.g., by `prun`'s `--host` argument,
a hostfile, or a job scheduler).

The "relative" specification is of the form "`+n<X>`", where `X` is an integer
specifying the Xth hostname in the set of all available hostnames, indexed
from 0. For example:

```
$ cat myrankfile
rank 0=+n0 slot=10-12
rank 1=+n1 slot=0,1,4
rank 2=+n2 slot=1-2
$ prun --host aa,bb,cc,dd --map-by rankfile:FILE=myrankfile ./a.out
```

All package/core slot locations are be specified as *logical* indexes. You can
use tools such as HWLOC's "lstopo" to find the logical indexes of packages and
cores.

## Deprecated Options

These deprecated options will be removed in a future release.

`--bind-to-core`

:   **(Deprecated: Use `--bind-to core`)**
    Bind processes to cores

`-bind-to-socket, --bind-to-socket`

:   **(Deprecated: Use `--bind-to package`)**
    Bind processes to processor sockets

`--bycore`

:   **(Deprecated: Use `--map-by core`)**
    Map processes by core

`-bynode, --bynode`

:   **(Deprecated: Use `--map-by node`)**
    Launch processes one per node, cycling by node in a round-robin
    fashion. This spreads processes evenly among nodes and assigns ranks
    in a round-robin, "by node" manner.

`--byslot`

:   **(Deprecated: Use `--map-by slot`)**
    Map and rank processes round-robin by slot.

`--cpus-per-proc <#perproc>`

:   **(Deprecated: Use `--map-by <obj>:PE=<#perproc>`)**
    Bind each process to the specified number of cpus.

`--cpus-per-rank <#perrank>`

:   **(Deprecated: Use `--map-by <obj>:PE=<#perrank>`)**
    Alias for `--cpus-per-proc`.

`--display-allocation`

:   **(Deprecated: Use `--map-by :DISPLAYALLOC`)**
    Display the detected resource allocation.

`--display-devel-map`

:   **(Deprecated: Use `--map-by :DISPLAYDEVEL`)**
    Display a detailed process map (mostly intended for developers) just
    before launch.

`--display-map`

:   **(Deprecated: Use `--map-by :DISPLAY`)**
    Display a table showing the mapped location of each process prior to
    launch.

`--display-topo`

:   **(Deprecated: Use `--map-by :DISPLAYTOPO`)**
    Display the topology as part of the process map (mostly intended for
    developers) just before launch.

`--do-not-launch`

:   **(Deprecated: Use `--map-by :DONOTLAUNCH`)**
    Perform all necessary operations to prepare to launch the application,
    but do not actually launch it (usually used to test mapping patterns).

`--do-not-resolve`

:   **(Deprecated: Use `--map-by :DONOTRESOLVE`)**
    Do not attempt to resolve interfaces - usually used to determine proposed
    process placement/binding prior to obtaining an allocation.

`-N <num>`

:   **(Deprecated: Use `--map-by prr:<num>:node`)**
    Launch `num` processes per node on all allocated nodes.

`--nolocal`

:   **(Deprecated: Use `--map-by :NOLOCAL`)**
    Do not run any copies of the launched application on the same node
    as `prun` is running. This option will override listing the `localhost`
    with `--host` or any other host-specifying mechanism.

`--nooversubscribe`

:   **(Deprecated: Use `--map-by :NOOVERSUBSCRIBE`)**
    Do not oversubscribe any nodes; error (without starting any
    processes) if the requested number of processes would cause
    oversubscription. This option implicitly sets "max_slots" equal
    to the "slots" value for each node. (Enabled by default).

`--npernode <#pernode>`

:   **(Deprecated: Use `--map-by ppr:<#pernode>:node`)**
    On each node, launch this many processes.

`--npersocket <#persocket>`

:   **(Deprecated: Use `--map-by ppr:<#perpackage>:package`)**
    On each node, launch this many processes times the number of
    processor sockets on the node. The `--npersocket` option also turns
    on the `--bind-to socket` option. The term `socket` has been globally
    replaced with `package`.

`--oversubscribe`

:   **(Deprecated: Use `--map-by :OVERSUBSCRIBE`)**
    Nodes are allowed to be oversubscribed, even on a managed system,
    and overloading of processing elements.

`--pernode`

:   **(Deprecated: Use `--map-by ppr:1:node`)**
    On each node, launch one process.

`--ppr`

:   **(Deprecated: Use `--map-by ppr:<list>`)**
    Comma-separated list of number of processes on a given resource type
    [default: none].

`--rankfile <FILENAME>`

:   **(Deprecated: Use `--map-by rankfile:FILE=<FILENAME>`)**
    Use a rankfile for mapping/ranking/binding

`--report-bindings`

:   **(Deprecated: Use `--bind-to :REPORT`)**
    Report any bindings for launched processes.

`--tag-output`

:   **(Deprecated: Use `--map-by :TAGOUTPUT`)**
    Tag all output with [job,rank]

`--timestamp-output`

:   **(Deprecated: Use `--map-by :TIMESTAMPOUTPUT`)**
    Timestamp all application process output

`--use-hwthread-cpus`

:   **(Deprecated: Use `--map-by :HWTCPUS`)**
    Use hardware threads as independent cpus.

`--xml`

:   **(Deprecated: Use `--map-by :XMLOUTPUT`)**
    Provide all output in XML format
