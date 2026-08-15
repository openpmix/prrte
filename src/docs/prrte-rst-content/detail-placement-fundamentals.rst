.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Fundamentals
============

The mapping of processes to nodes can be defined not just with general
policies but also, if necessary, using arbitrary mappings that cannot
be described by a simple policy. Supported directives, given on the
command line via the ``--mapby`` option, include:

* ``SEQ``: (often accompanied by the ``file=<path>`` qualifier)
  assigns one process to each node specified in the file. The
  sequential file is to contain an entry for each desired process, one
  per line of the file.

* ``RANKFILE``: (often accompanied by the ``file=<path>`` qualifier)
  assigns one process to the node/resource specified in each entry of
  the file, one per line of the file.

* ``DEVICE=<class|name>``: assigns one process to each device in the
  node's topology, in PCI bus order.

Mapping by device is unlike the other directives in one respect worth
understanding, because it governs what binding can then do.

Every other mapping target is an object the user names directly --- a
core, a NUMA domain, a package --- and binding descends within it. A
device is not such an object: it hangs off the I/O side of the topology
and has no CPUs of its own. What a process is actually placed against
is the device's **locality**, meaning the nearest object in the topology
that both contains the device and has CPUs. On one machine that may be
a NUMA domain, on another a whole package, depending on where the
hardware attaches the device.

Two consequences follow, and both are deliberate:

#. Binding descends from the locality, not from the device. So
   ``--mapby device=gpu --bindto core`` binds each process to one core
   within the CPUs local to its GPU.

#. Asking to bind to an object larger than the locality is an error,
   not a silent widening. Such a binding is not "near the device" at
   all, which is the whole of what was asked for. Note this cannot be
   known from the command line alone: whether ``--bindto package`` is
   legal depends on where that machine attaches its devices.

PRRTE cannot restrict a process to a device the way it restricts one to a
set of CPUs --- no such mechanism exists --- so the assignment is only
useful if the process can find out about it. Each process is therefore told
which devices it was mapped against, as the ``PMIX_DEVICE_ID`` key of its own
proc info:

.. code:: c

   PMIx_Get(&myproc, PMIX_DEVICE_ID, NULL, 0, &value);

The value is **always** a ``pmix_data_array_t`` of ``pmix_device_t``, even
when it holds a single device. A process given two devices and a process
given one are the same kind of answer differing in length, so there is no
separate single-device form to special-case --- and a reader that had one
would be exercising it almost always and the array path almost never.

Each entry carries the device's UUID, its OS name and its type. The UUID
rather than an index is what identifies it, because a runtime's own device
numbering need not match the topology's --- CUDA, for example, orders devices
by speed rather than by bus by default --- so an index would name a different
device than the one PRRTE chose. The same UUID appears in the
``PMIX_DEVICE_DISTANCES`` a process can query, which is what lets the two be
matched up.

``--display map`` reports it too, as a ``Device:`` field on each process
line.

Where every device on a node is equally close to every CPU --- which
happens when they all hang off one PCI complex rather than off
individual NUMA domains --- the job still runs and each process is
still assigned its own device, but a warning is printed: the binding
cannot be made any more specific than it would have been without the
directive.

For example, using the hostfile below:

.. code::

   $ cat myhostfile
   aa slots=4
   bb slots=4
   cc slots=4

The command below will launch three processes, one on each of nodes
``aa``, ``bb``, and ``cc``, respectively. The slot counts don't
matter; one process is launched per line on whatever node is listed on
the line.

.. code::

   $ prun --hostfile myhostfile --mapby seq ./a.out

Impact of the ranking option is best illustrated by considering the
following hostfile and test cases where each node contains two
packages (each package with two cores). Using the ``--mapby
ppr:2:package`` option, we map two processes onto each package and
utilize the ``--rankby`` option as show below:

.. code::

   $ cat myhostfile
   aa
   bb

.. list-table::
   :header-rows: 1

   * - Command
     - Ranks on ``aa``
     - Ranks on ``bb``

   * - ``--rankby core``
     - 0 1 ! 2 3
     - 4 5 ! 6 7

   * - ``--rankby package``
     - 0 2 ! 1 3
     - 4 6 ! 5 7

   * - ``--rankby package:SPAN``
     - 0 4 ! 1 5
     - 2 6 ! 3 7

Ranking by slot provides the identical result as ranking by core in
this case |mdash| a simple progression of ranks across each
node. Ranking by package does a round-robin ranking across packages
within each node until all processes have been assigned a rank, and
then progresses to the next node.  Adding the ``:SPAN`` qualifier to
the ranking directive causes the ranking algorithm to treat the entire
allocation as a single entity |mdash| thus, the process ranks are
assigned across all packages before circling back around to the
beginning.

The binding operation restricts the process to a subset of the CPU
resources on the node.

The processors to be used for binding can be identified in terms of
topological groupings |mdash| e.g., binding to an l3cache will bind
each process to all processors within the scope of a single L3 cache
within their assigned location. Thus, if a process is assigned by the
mapper to a certain package, then a ``--bindto l3cache`` directive
will cause the process to be bound to the processors that share a
single L3 cache within that package.

To help balance loads, the binding directive uses a round-robin method,
binding a process to the first available specified object type within
the object where the process was mapped. For example, consider the case
where a job is mapped to the package level, and then bound to core. Each
package will have multiple cores, so if multiple processes are mapped to
a given package, the binding algorithm will assign each process located
to a package to a unique core in a round-robin manner.

Binding can only be done to the mapped object or to a resource located
within that object.

An object is considered completely consumed when the number of
processes bound to it equals the number of CPUs within it. Unbound
processes are not considered in this computation. Additional
processes cannot be mapped to consumed objects unless the
OVERLOAD qualifier is provided via the "--bindto" command
line option.

Default process mapping/ranking/binding policies can also be set with MCA
parameters, overridden by the command line options when provided. MCA
parameters can be set on the ``prte`` command line when starting the
DVM (or in the ``prterun`` command line for a single-execution job), but
also in a system or user ``mca-params.conf`` file or as environment
variables, as described in the MCA section below. Some examples include:

.. list-table::
   :header-rows: 1

   * - ``prun`` option
     - MCA parameter key
     - Value

   * - ``--mapby core``
     - ``mapby``
     - ``core``

   * - ``--mapby package``
     - ``mapby``
     - ``package``

   * - ``--rankby core``
     - ``rankby``
     - ``core``

   * - ``--bindto core``
     - ``bindto``
     - ``core```

   * - ``--bindto package``
     - ``bindto``
     - ``package``

   * - ``--bindto none``
     - ``bindto``
     - ``none``
