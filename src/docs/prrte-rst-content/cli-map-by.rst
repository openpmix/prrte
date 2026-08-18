.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

.. note:: PRRTE accepts both the new "--mapby" and the older
          deprecated "--map-by" cmd line options. For simplicity, the
          following description will refer to the new "--mapby" form.

Processes are mapped based on one of the following directives as
applied at the job level:

* ``SLOT`` assigns procs to each node up to the number of available
  slots on that node before moving to the next node in the
  allocation

* ``HWTHREAD`` assigns a proc to each hardware thread on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``CORE`` (default) assigns a proc to each core on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``L1CACHE`` assigns a proc to each L1 cache on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``L2CACHE`` assigns a proc to each L2 cache on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``L3CACHE`` assigns a proc to each L3 cache on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``NUMA`` assigns a proc to each NUMA region on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``PACKAGE`` assigns a proc to each package on a node in a
  round-robin manner up to the number of available slots on that
  node before moving to the next node in the allocation

* ``NODE`` assigns processes in a round-robin fashion to all nodes
  in the allocation, with the number assigned to each node capped
  by the number of available slots on that node

* ``SEQ`` (often accompanied by the file=<path> qualifier) assigns
  one process to each node specified in the file. The sequential
  file is to contain an entry for each desired process, one per
  line of the file.

* ``PPR:N``:resource maps N procs to each instance of the specified
  resource type in the allocation. The resource may be an hwloc object
  (``ppr:2:package``) or a class of device (``ppr:2:device=gpu``), which
  places N procs on each such device

* ``RANKFILE`` (often accompanied by the file=<path> qualifier) assigns
  one process to the node/resource specified in each entry of the
  file, one per line of the file.

* ``PE-LIST=a,b`` assigns procs to each node in the allocation based on
  the ORDERED qualifier. The list is comprised of comma-delimited
  ranges of CPUs to use for this job. If the ORDERED qualifier is not
  provided, then each node will be assigned procs up to the number of
  available slots, capped by the availability of the specified CPUs.
  If ORDERED is given, then one proc will be assigned to each of the
  specified CPUs, if available, capped by the number of slots on each
  node and the total number of specified processes. Providing the
  OVERLOAD qualifier to the "bind-to" option removes the check on
  availability of the CPU in both cases.

* ``DEVICE=<class|name>`` assigns one proc to each device in the node's
  topology, in PCI bus order, placing it on the CPUs local to that
  device. The value is either a class of device -- ``gpu``, ``network``,
  or ``block`` -- or the name or UUID of one particular device such as
  ``mlx5_0``, in which case every proc is placed near that one device.
  ``nic``, ``fabric`` and ``openfabrics`` are accepted as spellings of
  ``network`` and mean exactly the same set: one entry per card, whether
  the node presents it as an OpenFabrics device (``mlx5_0``), a network
  interface (``ib0``), or both.

  Note there is no bare ``--mapby gpu``: the class is the *value* of the
  ``device`` directive, which is what allows other classes of device to
  be supported later without adding a directive for each.

  A device is assigned to a process rather than subdivided between them,
  so each device takes one process and asking for more processes than
  there are devices is an error unless ``SHARED`` is given.

  Binding descends from the device's *locality*: the nearest object in
  the topology that both contains the device and has CPUs. Asking to
  bind to an object larger than that locality is an error rather than a
  silent widening, since such a binding is not "near the device" at all.
  Whether a given ``--bindto`` is legal therefore depends on the machine,
  not on the command line alone. Where every device on a node is equally
  close to every CPU, the job runs and each proc is still assigned its
  own device, but a warning reports that the binding could not be made
  any more specific.

  Mapping by a **GPU** additionally requires that the GPUs can be named
  to the vendor's runtime. hwloc records a GPU's vendor identity -- an
  NVIDIA ``GPU-<uuid>`` and its AMD and Intel equivalents -- only when it
  is built with that vendor's backend (NVML, RSMI, or Level Zero). Built
  without them, hwloc still reports the GPUs and PRRTE can still place
  processes next to them, but no process can be told which GPU it got in
  terms the library it links will accept. PRRTE refuses the request in
  that case rather than mapping and saying nothing, because the two are
  indistinguishable while the job runs: the placement looks correct and
  the only symptom is that every process on the node ends up using the
  same GPU.

  The hwloc that decides this is the one **PRRTE was built against**,
  since each daemon discovers its own node -- not whatever hwloc happens
  to be installed alongside it. The other device classes are unaffected:
  a network or fabric device is named by its own GUIDs or MAC address,
  which hwloc always has.

  Given that identity, each process is also handed its GPUs in the
  variable its vendor's runtime reads -- ``CUDA_VISIBLE_DEVICES`` for
  NVIDIA, ``ROCR_VISIBLE_DEVICES`` for AMD -- naming them by the vendor's
  own identifier. Only processes actually mapped against a device get
  this, and a variable already set in the environment is replaced, since
  ``--map-by device=`` is the more specific request. PRRTE never sets the
  vendor's device *ordering* variable (``CUDA_DEVICE_ORDER`` and its
  equivalents): the identifiers do not depend on the ordering, which is
  the reason for using them, and changing it would renumber devices for
  the rest of the process's life. Intel GPUs are deliberately not given a
  variable: ``ZE_AFFINITY_MASK`` takes device indices, in an order PRRTE
  has no way to reproduce, and a wrong value there silently hides devices
  rather than failing.

  A process mapped against a **network** device is handed it the same
  way, in the variables the fabric libraries read: ``NCCL_IB_HCA`` and
  ``UCX_NET_DEVICES`` for a Mellanox or NVIDIA InfiniBand adapter, and
  ``PSM3_NIC`` for an Intel Omni-Path adapter. The device is named by the
  name those libraries accept -- ``mlx5_0`` -- which is the name hwloc
  gave it, so unlike the GPU case there is no identity that can be
  missing. Nothing is set for an adapter whose fabric has no component
  behind it, and no variable that takes a device *unit number* is set at
  all (``HFI_UNIT``, ``FI_OPX_HFI_SELECT``): a unit number is meaningful
  only against the enumeration it came from, and a wrong one there does
  not fail, it quietly puts the process on another adapter.

  The assignment is also readable directly, whether or not a device
  variable was set, as the ``PMIX_DEVICE_ID`` key of the process's own
  job data.

Any directive can include qualifiers by adding a colon (``:``) and any
combination of one or more of the following (delimited by colons) to
the ``--mapby`` option (except where noted):

* ``PE=n`` bind n CPUs to each process (can not be used in combination
  with rankfile or pe-list directives)

* ``SPAN`` load balance the processes across the allocation by treating
  the allocation as a single "super-node" (can not be used in
  combination with ``slot``, ``node``, ``seq``, ``ppr``, ``rankfile``, or
  ``pe-list`` directives)

* ``OVERSUBSCRIBE`` allow more processes on a node than processing elements

* ``NOOVERSUBSCRIBE`` means ``!OVERSUBSCRIBE``

* ``NOLOCAL`` do not launch processes on the same node as ``prun``

* ``HWTCPUS`` use hardware threads as CPU slots

* ``CORECPUS`` use cores as CPU slots (default)

* ``INHERIT`` indicates that a child job (i.e., one spawned from within
  an application) shall inherit the placement policies of the parent job
  that spawned it.

* ``NOINHERIT`` means ```!INHERIT``

* ``FILE=<path>`` (path to file containing sequential or rankfile entries).

* ``INTERLEAVE[=<level>]`` only applies to the ``DEVICE`` directive. It
  reorders the device list so that consecutive processes land on different
  objects of the given level --- ``package`` (the default), ``numa``,
  ``l3cache``, ``l2cache`` or ``l1cache``. On a node with two GPUs per
  socket, ``device=gpu:interleave`` places the first two processes on
  different sockets rather than filling the first. ``node`` is not an
  accepted level: interleaving across nodes is what ``SPAN`` already
  expresses. A level that does not divide the devices into groups leaves
  the order unchanged, so the qualifier is safe to leave in a default
  mapping policy.

* ``SHARED[=true|false]`` only applies to the ``DEVICE`` directive. It
  permits several processes to be assigned the same device. The default is
  ``false``: a device is assigned to a process rather than subdivided
  between them, so a job asking for more processes than there are devices
  is an error unless this is given. This is a separate question from the
  ``overload-allowed`` qualifier to ``--bindto``, which concerns running
  more processes than there are CPUs.

* ``NDEV=<n>`` only applies to the ``DEVICE`` directive. It assigns *n*
  devices to each process rather than one.

  This changes what a process is placed against, and that is worth
  understanding. A process holding two GPUs attached to different NUMA
  domains is local to neither of them alone --- it is local to whatever
  contains them both. So the locality of a process becomes the **common
  ancestor** of its devices' localities, and binding descends from there. On
  a node with two GPUs per socket, ``ndev=2`` therefore makes each process
  package-local, which means ``--bindto package`` is legitimate in that case
  and remains an error without ``ndev``.

  Devices are handed out in groups taken in order from the device list, so a
  group is a contiguous run of that order and the ``INTERLEAVE`` qualifier
  composes with this one.

* ``ORDERED`` only applies to the ``PE-LIST`` option to indicate that
  procs are to be bound to each of the specified CPUs in the order in
  which they are assigned (i.e., the first proc on a node shall be
  bound to the first CPU in the list, the second proc shall be bound
  to the second CPU, etc.)

.. note:: Directives and qualifiers are case-insensitive and can be
          shortened to the minimum number of characters to uniquely
          identify them. Thus, ``L1CACHE`` can be given as ``l1cache`` or
          simply as ``L1``.

The type of CPU (core vs hwthread) used in the mapping algorithm
is determined as follows:

* by user directive on the command line via the HWTCPUS qualifier to
  the ``--mapby`` directive

* by setting the ``rmaps_default_mapping_policy`` MCA parameter to
  include the ``HWTCPUS`` qualifier. This parameter sets the default
  value for a PRRTE DVM |mdash| qualifiers are carried across to DVM jobs
  started via ``prun`` unless overridden by the user's command line

* defaults to CORE in topologies where core CPUs are defined, and to
  hwthreads otherwise.

If your application uses threads, then you probably want to ensure that
you are either not bound at all (by specifying ``--bind-to none``), or
bound to multiple cores using an appropriate binding level or specific
number of processing elements per application process via the ``PE=#``
qualifier to the ``--mapby`` command line directive.

A more detailed description of the mapping, ranking, and binding
procedure can be obtained via the ``--help placement`` option.
