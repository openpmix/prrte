.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Examples
========

Listed here are the subset of command line options that will be used
in the process mapping/ranking/binding examples below.

Specifying Host Nodes
---------------------

Use one of the following options to specify which hosts (nodes) within
the PRRTE DVM environment to run on.

.. code::

    --host <host1,host2,...,hostN>

    # or

    --host <host1:X,host2:Y,...,hostN:Z>

* List of hosts on which to invoke processes. After each hostname a
  colon (``:``) followed by a positive integer can be used to specify
  the number of slots on that host (``:X``, ``:Y``, and ``:Z``). The
  default is ``1``.

.. code::

    --hostfile <hostfile>

* Provide a hostfile to use.

Process Mapping / Ranking / Binding Options
-------------------------------------------

* ``-c #``, ``-n #``, ``--n #``, ``--np <#>``: Run this many copies of
  the program on the given nodes. This option indicates that the
  specified file is an executable program and not an application
  context. If no value is provided for the number of copies to execute
  (i.e., neither the ``-np`` nor its synonyms are provided on the
  command line), ``prun`` will automatically execute a copy of the
  program on each process slot (see below for description of a
  "process slot"). This feature, however, can only be used in the SPMD
  model and will return an error (without beginning execution of the
  application) otherwise.

  .. note:: These options specify the number of processes to launch.
            None of the options imply a particular binding policy
            |mdash| e.g., requesting ``N`` processes for each package
            does not imply that the processes will be bound to the
            package.

* ``--mapby <object>``: Map to the specified object. Supported
  objects include:

  * ``slot``
  * ``hwthread``
  * ``core`` (default)
  * ``l1cache``
  * ``l2cache``
  * ``l3cache``
  * ``numa``
  * ``package``
  * ``node``
  * ``seq``
  * ``ppr``
  * ``rankfile``
  * ``pe-list``
  * ``device=<class|name>``

  Any object can include qualifiers by adding a colon (``:``) and any
  colon-delimited combination of one or more of the following to the
  ``--mapby`` options:

  * ``PE=n`` bind ``n`` processing elements to each process (can not
    be used in combination with rankfile or pe-list directives). Note
    that this qualifier is distinct from the ``pe-list=a,b`` mapping
    directive, which names the CPUs to be used.

  * ``SPAN`` load balance the processes across the allocation (cannot
    be used in combination with ``slot``, ``node``, ``seq``, ``ppr``,
    ``rankfile``, or ``pe-list`` directives)

  * ``OVERSUBSCRIBE`` allow more processes on a node than processing
    elements

  * ``NOOVERSUBSCRIBE`` means ``!OVERSUBSCRIBE``

  * ``NOLOCAL`` do not launch processes on the same node as ``prun``

  * ``HWTCPUS`` use hardware threads as CPU slots

  * ``CORECPUS`` use cores as CPU slots (default)

  * ``INHERIT`` indicates that a child job (i.e., one spawned from
    within an application) shall inherit the placement policies of the
    parent job that spawned it.

  * ``NOINHERIT`` means ``!INHERIT``

  * ``FILE=<path>`` (path to file containing sequential or rankfile
    entries).

  * ``INTERLEAVE[=<level>]`` only applies to the ``device`` directive;
    reorders the device list so consecutive procs land on different
    objects of the given level (default ``package``)

  * ``SHARED[=true|false]`` only applies to the ``device`` directive;
    permits several procs to be assigned the same device (default
    ``false``)

  * ``NDEV=<n>`` only applies to the ``device`` directive; assigns ``n``
    devices to each proc rather than one. The proc is then local to
    whatever contains all of them |mdash| two GPUs on different NUMA
    domains make their package the locality |mdash| so a binding
    refused with one device per proc may be legitimate with several

  * ``ORDERED`` only applies to the PE-LIST option to indicate that
    procs are to be bound to each of the specified CPUs in the order
    in which they are assigned (i.e., the first proc on a node shall
    be bound to the first CPU in the list, the second proc shall be
    bound to the second CPU, etc.)

  ``ppr`` policy example: ``--mapby ppr:N:<object>`` will launch
  ``N`` times the number of objects of the specified type on each
  node.

  .. note:: Directives and qualifiers are case-insensitive and can be
            shortened to the minimum number of characters to uniquely
            identify them. Thus, ``L1CACHE`` can be given as
            ``l1cache`` or simply as ``L1``. A shortening that fits
            more than one of them is refused rather than guessed at.

* ``--rankby <object>``: This assigns ranks in round-robin fashion
  according to the specified object. The default follows the mapping
  pattern. Supported rankby objects include:

  * ``slot``
  * ``node``
  * ``fill``
  * ``span``

  There are no qualifiers for the ``--rankby`` directive.

* ``--bindto <object>``: This binds processes to the specified
  object. See defaults in Quick Summary.  Supported bindto objects
  include:

  * ``none``
  * ``hwthread``
  * ``core``
  * ``l1cache``
  * ``l2cache``
  * ``l3cache``
  * ``numa``
  * ``package``

  Any object can include qualifiers by adding a colon (``:``) and any
  colon-delimited combination of one or more of the following to the
  ``--bindto`` options:

  * ``overload-allowed`` allows for binding more than one process in
    relation to a CPU

  * ``if-supported`` if binding to that object is supported on this
    system.

Specifying Host Nodes
---------------------

Host nodes can be identified on the command line with the ``--host``
option or in a hostfile.

For example, assuming no other resource manager or scheduler is
involved:

.. code::

    prun --host aa,aa,bb ./a.out

This launches two processes on node ``aa`` and one on ``bb``.

.. code::

    prun --host aa ./a.out

This launches one process on node ``aa``.

.. code::

    prun --host aa:5 ./a.out

This launches five processes on node ``aa``.

Or, consider the hostfile:

.. code::

    $ cat myhostfile
    aa slots=2
    bb slots=2
    cc slots=2

Here, we list both the host names (``aa``, ``bb``, and ``cc``) but
also how many "slots" there are for each. Slots indicate how many
processes can potentially execute on a node. For best performance, the
number of slots may be chosen to be the number of cores on the node or
the number of processor sockets.

If the hostfile does not provide slots information, the PRRTE DVM will
attempt to discover the number of cores (or hwthreads, if the
``:HWTCPUS`` qualifier to the ``--mapby`` option is set) and set the
number of slots to that value.

Examples using the hostfile above with and without the ``--host``
option:

.. code::

    prun --hostfile myhostfile ./a.out

This will launch two processes on each of the three nodes.

.. code::

    prun --hostfile myhostfile --host aa ./a.out

This will launch two processes, both on node ``aa``.

.. code::

    prun --hostfile myhostfile --host dd ./a.out

This will find no hosts to run on and abort with an error. That is, the
specified host ``dd`` is not in the specified hostfile.

When running under resource managers (e.g., SLURM, Torque, etc.), PRTE
will obtain both the hostnames and the number of slots directly from
the resource manger. The behavior of ``--host`` in that environment
will behave the same as if a hostfile was provided (since it is
provided by the resource manager).


Specifying Number of Processes
------------------------------

As we have just seen, the number of processes to run can be set using
the hostfile. Other mechanisms exist.

The number of processes launched can be specified as a multiple of the
number of nodes or processor sockets available. Consider the hostfile
below for the examples that follow.

.. code::

   $ cat myhostfile
   aa
   bb

For example:

.. code::

   prun --hostfile myhostfile --mapby ppr:2:package ./a.out

This launches processes 0-3 on node ``aa`` and process 4-7 on node
``bb``, where ``aa`` and ``bb`` are both dual-package nodes. The
``--mapby ppr:2:package`` option also turns on the ``--bindto
package`` option, which is discussed in a later section.

.. code::

   prun --hostfile myhostfile --mapby ppr:2:node ./a.out

This launches processes 0-1 on node ``aa`` and processes 2-3 on node
``bb``.

.. code::

   prun --hostfile myhostfile --mapby ppr:1:node ./a.out

This launches one process per host node.

Another alternative is to specify the number of processes with the
``--np`` option. Consider now the hostfile:

.. code::

   $ cat myhostfile
   aa slots=4
   bb slots=4
   cc slots=4

With this hostfile:

.. code::

   prun --hostfile myhostfile --np 6 ./a.out

This will launch processes 0-3 on node ``aa`` and processes 4-5 on
node ``bb``.  The remaining slots in the hostfile will not be used
since the ``--np`` option indicated that only 6 processes should be
launched.


Mapping Processes to Devices
----------------------------

Consider a two-socket node with eight NUMA domains and four GPUs, where
the GPUs are attached to NUMA domains 1 and 2 on the first socket but 6
and 7 on the second. That asymmetry is the case device mapping exists
for: no ``--mapby numa`` or ``--mapby package`` expression selects those
four domains, because they are not at the same position within each
socket.

.. code::

   $ prun -n 4 --mapby device=gpu --bindto core ./a.out

places one process per GPU, in PCI bus order, each bound to the first
available core in the CPUs local to its own GPU --- on the machine
described above, cores 16, 32, 96 and 112.

Binding may be to any object at or below the device's locality:

.. code::

   $ prun -n 1 --mapby device=gpu --bindto numa ./a.out

binds the process to the whole NUMA domain its GPU is attached to,
while ``--bindto l3cache`` binds it to one L3 cache within that domain
and ``--bindto core`` to a single core. Asking for ``--bindto package``
on this machine is an error: a package contains the GPU's NUMA domain
and three others, so binding there would place the process on CPUs the
GPU is not local to.

A device is assigned to a process rather than subdivided between processes,
so by default each device takes one process: asking for more processes than
there are devices is an error. Where sharing the devices is intended, say so
with the ``shared`` qualifier:

.. code::

   $ prun -n 8 --mapby device=gpu:shared --bindto core ./a.out

On the four-GPU machine above that runs two processes per GPU. Note the
processes still get separate cores: sharing a device and overloading a CPU
are different resources and different decisions, which is why they have
different qualifiers --- ``shared`` here, and ``overload-allowed`` on
``--bindto`` for the CPUs.

Where a job wants its processes spread across sockets rather than filling
the first, add the ``interleave`` qualifier:

.. code::

   $ prun -n 2 --mapby device=gpu:interleave --bindto core ./a.out

This reorders the device list so that consecutive processes land on
different packages --- on the machine above, cores 16 and 96 rather than
16 and 32. The level may be given explicitly (``interleave=numa``, for
instance); it defaults to ``package``.

Where a process needs more than one device, ``ndev`` says how many:

.. code::

   $ prun -n 2 --mapby device=gpu:ndev=2 --bindto package ./a.out

gives each of the two processes two GPUs. A process holding devices in
different NUMA domains is local to neither of them alone, so its locality
becomes whatever contains them both --- here the package, which is why
binding to a package is legal in this case and an error without ``ndev``.
A finer binding still lands beside the devices: with ``--bindto core``
the first process is bound to core 16, in the NUMA domain of its first
GPU, and not to core 0, which is in the same package but local to neither
of them.

Because the devices are handed out in groups taken in order from the device
list, ``interleave`` composes with ``ndev``: the interleaving decides the
order, and the grouping then takes contiguous runs of it.

Mapping by a GPU requires one thing of the machine that the other
device classes do not: that the GPUs can be named to the vendor's
runtime. hwloc learns a GPU's vendor identity --- an NVIDIA
``GPU-<uuid>`` and its AMD and Intel equivalents --- only from that
vendor's backend (NVML, RSMI, Level Zero), which has to be enabled when
hwloc is built. Without it, the GPUs are still discovered and processes
are still placed correctly beside them, but no process can be told
which GPU it was given in terms the library it links will accept. PRRTE
refuses the request in that case. Mapping and then quietly telling the
process nothing looks identical to a working job --- the map is right,
and the only symptom is that every process on the node uses the same
GPU.

The hwloc that decides this is the one PRRTE was built against, since
each daemon discovers its own node; installing another hwloc alongside
does not change it. Network, fabric and block devices are unaffected,
being named by identifiers hwloc always has.

Where the identity is available, each process is also handed its GPUs
in the environment variable its vendor's runtime reads:

.. list-table::
   :header-rows: 1

   * - Variable
     - Vendor

   * - ``CUDA_VISIBLE_DEVICES``
     - NVIDIA

   * - ``ROCR_VISIBLE_DEVICES``
     - AMD

   * - ``ZE_AFFINITY_MASK``
     - Intel

named, for NVIDIA and AMD, by the vendor's own identifier rather than
by an index; Intel is the exception, and is described below. Only
processes actually mapped against a device are given one, and a value
already in the environment is replaced --- ``--mapby device=`` is the
more specific request, and because the identifiers name devices
absolutely they compose correctly with a set a resource manager has
already narrowed.

PRRTE never sets the vendor's device *ordering* variable
(``CUDA_DEVICE_ORDER`` and its equivalents). The identifiers do not
depend on it, which is precisely why they are used, and changing it
would renumber every device for the rest of the process's life.

Intel is the exception to "identity rather than index".
``ZE_AFFINITY_MASK`` has no identifier form --- it takes Level Zero
device ordinals --- but the ordinals are not guessed. hwloc's Level
Zero backend records the driver and device index that ``zeDeviceGet``
returned for each device, so the value is read from that enumeration
rather than predicted, and it is read on the node that will run the
process.

An ordinal is not a complete statement on its own: a Level Zero driver
reads ``ZE_FLAT_DEVICE_HIERARCHY`` first and then interprets the mask
against the devices that model exposes, so the same ordinals name a
card under ``COMPOSITE`` and a tile under ``FLAT``. The model is
therefore stated alongside the mask whenever the process's environment
does not already name one. If it does name one and it disagrees,
nothing is set and a message says so --- overriding a deliberate
choice would change how many devices the program sees, and writing a
mask that will be read under a different model would silently hand it
half the hardware it was assigned.

A process mapped against a network device is handed it the same way,
in the environment variables the fabric libraries read:

.. list-table::
   :header-rows: 1

   * - Variable
     - Adapters

   * - ``NCCL_IB_HCA``
     - Mellanox / NVIDIA InfiniBand adapters

   * - ``UCX_NET_DEVICES``
     - Mellanox / NVIDIA InfiniBand adapters

   * - ``PSM3_NIC``
     - Intel Omni-Path adapters

named as those libraries name the device --- ``mlx5_0`` --- which is
the name hwloc gave it. Unlike a GPU's vendor identity that name is
always there, so the identity check above does not apply to this class.

No variable that names an adapter by its unit *number* is set ---
``HFI_UNIT`` and ``FI_OPX_HFI_SELECT`` among them. A unit number is
meaningful only against the enumeration it came from, which is the
driver's rather than one PRRTE performed, and a wrong number in these
variables does not fail --- it quietly puts the process on a different
adapter. Nothing is set at all for an adapter whose fabric has no
support behind it, rather than guessing at a variable it might read.

The assignment can also be read directly, whether or not a device
variable was set, from the process's own job data under
``PMIX_DEVICE_ID``.

The reverse ratio --- several processes on each device rather than several
devices for each process --- is a ``ppr`` pattern, spelled the same way as
every other:

.. code::

   $ prun --mapby ppr:2:device=gpu --bindto core ./a.out

places two processes on each GPU, each bound to its own core within that
GPU's locality. As with any ``ppr`` pattern the process count follows from
the pattern when ``-n`` is not given: four GPUs yield eight processes.

Naming a single device rather than a class places every process near
that one device, which suits a job whose performance depends on one
particular fabric interface:

.. code::

   $ prun -n 8 --mapby device=mlx5_0 --bindto core ./a.out

Other classes are selected the same way: ``device=network`` for the
node's network interfaces and ``device=block`` for block devices.
``device=nic``, ``device=fabric`` and ``device=openfabrics`` all mean
``device=network``: a card that presents both an OpenFabrics device and
a network interface is one device under any of them.


Mapping Processes to Nodes Using Policies
-----------------------------------------

The examples above illustrate the default mapping of process processes
to nodes. This mapping can also be controlled with various
``prun`` / ``prterun`` options that describe mapping policies.

.. code::

   $ cat myhostfile
   aa slots=4
   bb slots=4
   cc slots=4

Consider the hostfile above, with ``--np 6``:

.. list-table::
   :header-rows: 1

   * - Command
     - Ranks on ``aa``
     - Ranks on ``bb``
     - Ranks on ``cc``

   * - ``prun``
     - 0 1 2 3
     - 4 5
     -

   * - ``prun --mapby node``
     - 0 3
     - 1 4
     - 2 5

   * - ``prun --mapby node:NOLOCAL``
     -
     - 0 2 4
     - 1 3 5

The ``--mapby node`` option will load balance the processes across
the available nodes, numbering each process by node in a round-robin
fashion.

The ``:NOLOCAL`` qualifier to ``--mapby`` prevents any processes from
being mapped onto the local host (in this case node ``aa``). While
``prun`` typically consumes few system resources, the ``:NOLOCAL``
qualifier can be helpful for launching very large jobs where ``prun``
may actually need to use noticeable amounts of memory and/or
processing time.

Just as ``--np`` can specify fewer processes than there are slots, it
can also oversubscribe the slots. For example, with the same hostfile:

.. code::

   prun --hostfile myhostfile --np 14 ./a.out

This will produce an error since the default ``:NOOVERSUBSCRIBE``
qualifier to ``--mapby`` prevents oversubscription.

To oversubscribe the nodes you can use the ``:OVERSUBSCRIBE``
qualifier to ``--mapby``:

.. code::

   prun --hostfile myhostfile --np 14 --mapby :OVERSUBSCRIBE ./a.out

This will launch processes 0-5 on node ``aa``, 6-9 on ``bb``, and
10-13 on ``cc``.

Limits to oversubscription can also be specified in the hostfile
itself with the ``max_slots`` field:

.. code::

    $ cat myhostfile
    aa slots=4 max_slots=4
    bb         max_slots=8
    cc slots=4

The ``max_slots`` field specifies such a limit. When it does, the
``slots`` value defaults to the limit. Now:

.. code::

   prun --hostfile myhostfile --np 14 --mapby :OVERSUBSCRIBE ./a.out

This causes the first 12 processes to be launched as before, but the
remaining two processes will be forced onto node cc. The other two
nodes are protected by the hostfile against oversubscription by this
job.

Using the ``:NOOVERSUBSCRIBE`` qualifier to ``--mapby`` option can be
helpful since the PRTE DVM currently does not get ``max_slots`` values
from the resource manager.

Of course, ``--np`` can also be used with the ``--host`` option. For
example,

.. code::

   prun --host aa,bb --np 8 ./a.out

This will produce an error since the default ``:NOOVERSUBSCRIBE``
qualifier to ``--mapby`` prevents oversubscription.

.. code::

   prun --host aa,bb --np 8 --mapby :OVERSUBSCRIBE ./a.out

This launches 8 processes. Since only two hosts are specified, after
the first two processes are mapped, one to ``aa`` and one to ``bb``,
the remaining processes oversubscribe the specified hosts evenly.

.. code::

   prun --host aa:2,bb:6 --np 8 ./a.out

This launches 8 processes. Processes 0-1 on node ``aa`` since it has 2
slots and processes 2-7 on node ``bb`` since it has 6 slots.

And here is a MIMD example:

.. code::

   prun --host aa --np 1 hostname : --host bb,cc --np 2 uptime

This will launch process 0 running ``hostname`` on node ``aa`` and
processes 1 and 2 each running ``uptime`` on nodes ``bb`` and ``cc``,
respectively.


Per-App-Context Mapping Example
-------------------------------

In an MPMD job, each application context separated by ``:`` may carry
its own ``--mapby``, ``--rankby``, and ``--bindto`` directives, given
ahead of that app's executable. Using the same hostfile:

.. code::

   $ cat myhostfile
   aa slots=4
   bb slots=4
   cc slots=4

   prun --hostfile myhostfile \
       --mapby core --bindto core -n 6 app1 \
       : \
       --mapby node --rankby fill --bindto none -n 2 app2

This will:

* Map ``app1``'s 6 processes by core, binding each to its own core
  (processes 0-5).

* Map ``app2``'s 2 processes by node in round-robin fashion, leaving
  them unbound (processes 6-7).

Note that process ranks are globally contiguous across both apps:
``app1`` receives ranks 0-5 and ``app2`` receives ranks 6-7. That holds
for the mappers that number their own processes too: a ``rankfile`` or
``seq`` file given to an app numbers *that* app's ranks, and PRRTE
offsets them into the job's numbering.

A directive is per-app only when it is written on an app other than the
first. Written on the first app and nowhere else it describes the whole
job, however many apps follow:

.. code::

   prun --hostfile myhostfile --mapby core -n 6 app1 : -n 2 app2

maps both apps by core, while

.. code::

   prun --hostfile myhostfile -n 6 app1 : --mapby core -n 2 app2

maps only ``app2`` by core and leaves ``app1`` to the default rules.

The ``:NOLOCAL`` qualifier may also be applied per app context:

.. code::

   prun --hostfile myhostfile \
       --mapby slot:nolocal -n 6 app1 \
       : \
       --mapby slot -n 1 app2

Here ``app1`` avoids the head node (whichever node ``prun`` is running
on) while ``app2`` may run on any node including the head node.
