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
proc info, which it can retrieve with:

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
utilize the ``--rankby`` option as shown below (a ``!`` separates the
two packages of a node):

.. code::

   $ cat myhostfile
   aa
   bb

.. list-table::
   :header-rows: 1

   * - Command
     - Ranks on ``aa``
     - Ranks on ``bb``

   * - ``--rankby slot``
     - 0 1 ! 2 3
     - 4 5 ! 6 7

   * - ``--rankby node``
     - 0 2 ! 4 6
     - 1 3 ! 5 7

   * - ``--rankby span``
     - 0 4 ! 1 5
     - 2 6 ! 3 7

Ranking by slot ranks the processes in the order in which the mapper
placed them |mdash| a simple progression of ranks across each node;
ranking by fill gives the identical result in this case. Ranking by
node assigns ranks round-robin across the nodes, so consecutive ranks
land on different nodes. Ranking by span treats the entire allocation
as a single entity |mdash| thus, the process ranks are assigned across
all packages of all nodes before circling back around to the
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
``OVERLOAD`` qualifier is provided via the ``--bindto`` command
line option.

Per-app-context mapping
-----------------------

By default every application context in a job shares the same mapping,
ranking, and binding policy. In a multi-program multiple-data (MPMD)
job, each application context separated by ``:`` on the ``prun``
command line may carry its own independent ``--mapby``, ``--rankby``,
and ``--bindto`` directives.

Which app such a directive describes follows one rule: the *first* app
segment is where the command line speaks for the job. A directive
written there and nowhere else applies to the whole job, however many
apps follow. Written on any later app it describes that app alone, and
the apps that were given none take the ordinary defaults |mdash| an app
that says nothing is not agreeing with one that did.

.. code::

   prun --mapby core --bindto core -n 4 app1 \
       : --mapby node --rankby fill --bindto none -n 2 app2

This launches ``app1`` mapped by core (processes 0-3) and ``app2``
mapped by node (processes 4-5) within the same job, allowing
shared-memory and direct PMIx communication between the two apps.

.. code::

   prun -n 4 app1 : --mapby node -n 2 app2

Here only ``app2`` is mapped by node; ``app1`` is placed by the default
rules.

Every mapping policy may be given per app |mdash| ``seq``, ``rankfile``,
``ppr:N:obj`` and ``pe-list=...`` included |mdash| so two apps of one
job may be placed by two different mapping components. ``--display
map-devel`` names the component that placed each app.

Qualifiers that describe the whole job |mdash| the following govern the
job as a whole, so wherever they are written they are applied to the
job:

* ``OVERSUBSCRIBE`` / ``NOOVERSUBSCRIBE``: oversubscription applies to
  all apps sharing the same nodes.

* ``INHERIT`` / ``NOINHERIT``: whether a spawned child job inherits the
  parent's policies is a job-wide property.

Apps that say nothing about these are silent, not dissenting. Only
apps that answer the same question in *opposite* ways are refused, and
that aborts the job with an error.

The ``NOLOCAL`` qualifier *is* per app context. It prevents that app's
processes from running on the head node without affecting other apps
in the same job:

.. code::

   prun --mapby slot:nolocal -n 8 app1 : --mapby slot -n 1 app2

Here ``app1`` avoids the head node; ``app2`` may run anywhere.

Per-app directives can also be supplied via the ``PMIx_Spawn`` API by
placing ``PMIX_MAPBY``, ``PMIX_RANKBY``, ``PMIX_BINDTO`` and
``PMIX_PPR`` keys in the per-app ``info[]`` array on the corresponding
``pmix_app_t``. On that path there is no "first app" rule: the array a
key was written in says what it describes.

Note that ``PMIX_MAPPER`` is *not* supported, per job or per app, and a
spawn request carrying it is refused. Naming a mapping component says
nothing that ``PMIX_MAPBY`` has not already said |mdash| the mapping
policy is what selects the component |mdash| and the two can
contradict each other. Describe the placement you want with
``PMIX_MAPBY`` and let PRRTE choose the component that performs it.

Global rank assignment remains contiguous across all apps regardless
of per-app ranking directives: ranks are assigned in app-context order
starting from 0, so the first app receives ranks 0..N-1 and each
subsequent app continues from the next unassigned rank. The per-app
``--rankby`` directive controls only the ordering of ranks within that
app. The same holds for the mappers that number their own processes: a
``rankfile`` or ``seq`` file given to an app numbers *that* app's
ranks, and PRRTE offsets them into the job's numbering.

Default policies
----------------

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

   * - ``--rankby fill``
     - ``rankby``
     - ``fill``

   * - ``--bindto core``
     - ``bindto``
     - ``core``

   * - ``--bindto package``
     - ``bindto``
     - ``package``

   * - ``--bindto none``
     - ``bindto``
     - ``none``
