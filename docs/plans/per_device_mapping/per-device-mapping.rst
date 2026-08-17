Mapping and binding by device
=============================

Status
------

.. note::

   **Plan only — nothing described here is implemented.**  This document
   works out what ``--map-by device=gpu`` should mean, checks the proposed
   answer against the real topology the requester supplied, and lists the
   work.  Where the proposal deviates from what the requester asked for, the
   deviation is called out rather than quietly resolved; see
   `Deviations from the requester's table`_ and `Decisions still to make`_.

   The build order — phases, PR boundaries, and what proves each step —
   is in :doc:`impl-plan`.

Source: `Open MPI issue #14169
<https://github.com/open-mpi/ompi/issues/14169>`_, "``--map-by dist`` not
working (anymore) / ``--map-by device=gpu`` wanted".


The request
-----------

A user on a 2-socket AMD Turin node (8 NUMA domains, 4 H200 GPUs, InfiniBand)
wants processes placed against GPUs rather than against a CPU object that
happens to be near one.  Their motivating observation is that the GPU-to-NUMA
assignment on this machine is *not* uniform across the two sockets:

.. code-block:: text

   $ nvidia-smi topo -m
         GPU0 GPU1 GPU2 GPU3   CPU Affinity      NUMA Affinity
   GPU0   X    NV6  NV6  NV6   16-31,144-159     1
   GPU1  NV6   X    NV6  NV6   32-47,160-175     2
   GPU2  NV6  NV6   X    NV6   96-111,224-239    6
   GPU3  NV6  NV6  NV6   X     112-127,240-255   7

On socket 0 the GPUs hang off NUMA domains 1 and 2 (of 0-3); on socket 1 they
hang off 6 and 7 (of 4-7).  No single ``--map-by numa`` or ``--map-by
package`` expression reaches the four right domains, and ``--cpu-set`` would
have to be hand-written per machine.

The table they asked for, for a **single node** and hwloc logical core
numbering:

.. code-block:: text

   -n 1 --map-by gpu --bind-to core         -> rank 0: core 16
   -n 1 --map-by gpu --bind-to l3cache      -> rank 0: cores 16-23
   -n 1 --map-by gpu --bind-to numa         -> rank 0: cores 16-31
   -n 1 --map-by gpu --bind-to package      -> error; above mapping level (?)
                                               or cores 0-63
   -n 1 --map-by gpu:PE=2 --bind-to core    -> error or core 16 or core 0???
   -n 2 --map-by gpu --bind-to core         -> rank 0: core 16
                                               rank 1: core 32
   -n 2 --map-by gpu:span --bind-to core    -> rank 0: core 16
                                               rank 1: core 96
   -n 2 --map-by gpu:span --bind-to numa    -> rank 0: cores 16-31
                                               rank 1: cores 96-111
   -n 2 --map-by gpu:span --bind-to package -> rank 0: cores 0-63 (or error)
                                               rank 1: cores 64-128

They also ask, in the discussion, for the general form — ``--map-by
device=gpu``, ``device=hca``, ``device=pci/<type>`` — rather than a
GPU-specific directive, and note that the *old* ``--map-by dist:span --mca
rmaps_dist_device hfi1_0`` served the NIC half of the same need before it was
removed.

Two things the maintainer established in the thread and which this plan
takes as given:

* PRRTE can compute the placement, and can **tell** a process which device it
  is nearest.  It cannot **enforce** an assignment: there is no
  ``sched_setaffinity`` for a GPU.  ``CUDA_VISIBLE_DEVICES`` /
  ``ROCR_VISIBLE_DEVICES`` / ``ZE_AFFINITY_MASK`` are vendor-specific
  conventions, not a mechanism PRRTE owns.
* On many machines every GPU hangs off one PCI complex per package, so every
  process on a package is equidistant from every GPU on it and ``--map-by
  gpu`` degenerates into ``--map-by package``.  That is very likely why
  ``dist`` was removed.  It is **not** true of the machine in this issue, and
  the plan must not assume it either way.


What is in the tree today
-------------------------

``dist`` is a ghost.  Every piece of it survives except the parts that would
make it work:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Remnant
     - State
   * - ``PRTE_MAPPING_BYDIST`` (10) in ``rmaps_types.h``
     - Defined.  No parser ever produces it.
   * - ``rmaps_base_map_job.c`` policy switch
     - Has a ``case PRTE_MAPPING_BYDIST`` that sets ``mapdepth =
       PRTE_BIND_TO_NONE``, ``maptype = HWLOC_OBJ_MACHINE`` — i.e. treats it
       as by-slot.
   * - ``rmaps_base_print_fns.c``
     - Renders it as ``"MINDIST"``.
   * - ``PRTE_APP_DIST_DEVICE`` (30), ``PRTE_JOB_DIST_DEVICE`` (job key 78)
     - Attribute keys with names in ``attr.c``.  Nothing sets them.
   * - ``prte_rmaps_options_t.dist_device``
     - Read from the attribute by ``resolve_app_options()``, copied into the
       per-app options, freed at ``cleanup``.  No mapper reads it.
   * - ``prte_rmaps_base.device``
     - Initialized to ``NULL`` in ``rmaps_base_frame.c`` and **never read or
       written** anywhere in the tree.
   * - ``PRTE_CLI_DIST`` (``"dist"``) in ``prte_cmd_line.h``
     - Defined.  Not in ``mappers[]`` in ``schizo_base_frame.c``, so a
       command line carrying it is refused at the front door; and not in
       either ``--map-by`` parser, so it could not be honored if it got
       through.
   * - The ``mapby`` MCA parameter's help string
     - Still advertises ``dist`` and ``DEVICE=dev (for dist policy)``.  This
       is what the requester found with ``prte_info`` and reasonably read as
       a promise.

So the directive is documented, half-plumbed, and unreachable.  Whatever this
plan lands, **the advertised-but-dead spellings have to go with it** — either
implemented or removed from the help string.  A knob that does nothing is
worse than no knob (see ``plm``'s ``node_regex_threshold``, retired for the
same reason).

What *is* already in place and does not need building:

* PRRTE keeps I/O objects in every topology it holds.  ``topology_set_flags()``
  in ``src/hwloc/hwloc_base_util.c`` asks hwloc for
  ``HWLOC_TYPE_FILTER_KEEP_IMPORTANT``, and both callers pass ``io = true``
  — the sensing path and the ``hwloc_use_topo_file`` XML path.
* Every daemon ships its own topology to the HNP (``prted.c`` packs
  ``prte_hwloc_topology`` as ``PMIX_TOPO``), and ``prte_homo_nodes`` defaults
  to **false**, so the HNP holds a per-node topology *with devices in it*.
  Under ``--prtemca prte_uniform_nodes 1`` only daemon rank 1 reports and
  every other node inherits its topology — a real caveat for a cluster whose
  nodes differ in device count, and one this plan must document rather than
  fix.
* PMIx already owns a device vocabulary and a device enumerator:
  ``pmix_device_type_t`` (``PMIX_DEVTYPE_GPU``, ``NETWORK``,
  ``OPENFABRICS``, ``COPROC``, ``BLOCK``, ``DMA``), ``pmix_device_t``
  (``uuid``, ``osname``, ``type``), ``PMIX_DEVICE_DISTANCES``,
  ``PMIX_DEVICE_ID``, ``PMIX_QUERY_DEVICES``, and
  ``pmix_hwloc_compute_distances()``, which takes an **arbitrary** topology.
  This plan builds on that rather than beside it; see `Device discovery`_.


Evidence: the topology the requester supplied
---------------------------------------------

The requester attached ``hwloc_topology.xml`` from the machine.  Reading it
settles several design questions that would otherwise be guesswork.  Every
OS device, with the PCI function it hangs off, its PCI class, and the CPU
locality of its nearest non-I/O ancestor:

.. code-block:: text

   busid            class  osdev_type  osdev        numa  cpus
   0000:03:00.0     0207   net/ofa     ib3x/mlx5_0    1   16-31,144-159
   0000:06:00.0     0302   gpu         card1          1   16-31,144-159
                                       renderD128
   0000:13:00.0     0207   net/ofa     ib2x/mlx5_3    2   32-47,160-175
   0000:16:00.0     0302   gpu         card2          2   32-47,160-175
                                       renderD129
   0000:51:00.0     0200   net         eno6           0   0-15,128-143
   0000:53:00.0     0300   gpu         card0          0   0-15,128-143
   0000:61:00.0     0200   net/ofa     eno4np0/mlx5_1 3   48-63,176-191
   0000:61:00.1     0200   net/ofa     eno5np1/mlx5_2 3   48-63,176-191
   0000:71:00.0     0108   block       nvme0n1        0   0-15,128-143
   0000:72:00.0     0108   block       nvme1c1n1      0   0-15,128-143
   0000:93:00.0     0207   net/ofa     ib1x/mlx5_4    6   96-111,224-239
   0000:96:00.0     0302   gpu         card3          6   96-111,224-239
                                       renderD130
   0000:e3:00.0     0207   net/ofa     ib0/mlx5_5     7   112-127,240-255
   0000:e6:00.0     0302   gpu         card4          7   112-127,240-255
                                       renderD131

Five things follow, and each of them is a design constraint:

**1. PCI bus order reproduces the requester's table exactly.**  Sorting the
class-``03xx`` functions by bus id gives ``06:00.0`` → NUMA 1, ``16:00.0`` →
NUMA 2, ``96:00.0`` → NUMA 6, ``e6:00.0`` → NUMA 7 — precisely GPU0..GPU3 of
``nvidia-smi topo -m``.  So **order devices by PCI bus id**, not by hwloc
traversal order and emphatically not by OS device name (``card10`` sorts
before ``card2``, and the names differ between nodes).

**2. The unit of "a device" is the PCI function, not the OS device.**
``card1`` and ``renderD128`` are two ``OSDev`` children of the *same*
``PCIDev`` (gp_index 919).  Counting OS devices gives nine GPUs where there
are four.  Likewise ``eno4np0`` and ``mlx5_1`` are one card function seen two
ways.  Dedup by parent ``PCIDev``.

**3. There is a decoy, and it is the interesting case.**  ``card0`` at
``0000:53:00.0`` is the ASPEED BMC display adapter (vendor ``1a03``, class
``0300``).  hwloc reports it as ``osdev_type=1`` — ``HWLOC_OBJ_OSDEV_GPU`` —
exactly like the H200s.  A naive "enumerate GPU OS devices" finds five GPU
PCI functions on this machine, one of which cannot run anything, and it sits
on NUMA 0, which would shift every subsequent assignment by one and break
every row of the requester's table.

Two discriminators are visible in this file:

* PCI class: the H200s are ``0302`` (3D controller), the ASPEED is ``0300``
  (VGA compatible controller).  **This does not generalize** — a consumer
  NVIDIA card in a workstation is ``0300`` too.
* Render node: each H200 function carries a ``renderD*`` OS device; the
  ASPEED carries only ``card0``.  A DRM render node is what makes a display
  device usable for compute, so this is the *semantically* right test, and it
  is the one to use for the DRM fallback path.

Neither is needed at all when hwloc was built with its GPU backends (CUDA,
NVML, RSMI, LevelZero, OpenCL): those produce unambiguous ``cuda0`` /
``nvml0`` / ``rsmi0`` OS devices carrying a ``Backend`` info key.  This
particular ``lstopo`` was run without them, which is exactly why the
fallback matters — **the fallback is the common case, not the exotic one.**

**4. The device's locality is a real, sub-package object here.**  Each GPU's
nearest non-I/O ancestor is an hwloc ``Group`` whose cpuset is exactly one
NUMA domain's.  ``--map-by device=gpu --bind-to numa`` therefore *means*
something on this machine, and the "every process on a package is
equidistant" degeneracy the maintainer described does not apply.  The plan
must handle both shapes; see `Degenerate topologies`_.

**5. The device type vocabulary has to be finer than "NIC".**  This node has
six OpenFabrics functions (``mlx5_0`` … ``mlx5_5``) on NUMA 1, 2, 3, 3, 6, 7,
*plus* a plain Ethernet controller (``eno6``) on NUMA 0 and no OFA device at
all.  ``device=openfabrics`` gives six devices; ``device=network`` gives
seven, one of which is the management NIC.  PMIx's existing
``PMIX_DEVTYPE_*`` split already draws this line correctly — use it.


Design
------

Grammar
~~~~~~~

Add one mapping directive::

   --map-by device=<spec>[:<qualifiers>]

``<spec>`` is either a **device class** or a **device name**:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - ``<spec>``
     - Meaning
   * - ``gpu``
     - every compute GPU on the node (``PMIX_DEVTYPE_GPU`` ∪
       ``PMIX_DEVTYPE_COPROC``, filtered — see below)
   * - ``network``
     - every network interface (``PMIX_DEVTYPE_NETWORK``)
   * - ``openfabrics`` (``hca``, ``ofa``)
     - every OpenFabrics device (``PMIX_DEVTYPE_OPENFABRICS``)
   * - ``nic``
     - ``network`` ∪ ``openfabrics``, deduped by PCI function
   * - ``block``
     - every block device (``PMIX_DEVTYPE_BLOCK``)
   * - anything else
     - a device **name** or **UUID** — matched against each device's
       ``osname`` and ``uuid`` (``mlx5_0``, ``renderD128``,
       ``gpu://node07::renderD128``).  The device list is then that one
       device, and the map is "put every process near this device."

The name form is what brings ``dist`` back.  ``--map-by
device=mlx5_0`` is ``--map-by dist --mca rmaps_dist_device mlx5_0`` with one
directive instead of two, and it needs no second policy value, no
``DEVICE=`` qualifier, and no MCA parameter.  ``PRTE_CLI_DIST`` and the
``DEVICE=`` qualifier text are then deleted, not resurrected.

**There is deliberately no bare --map-by gpu form.**  The requester's
examples mostly use that shorthand, but ``device=`` is the directive and the
device class is its *value* — which is exactly what lets a new class be
supported later by adding a value rather than a directive.  Promoting one
class to a top-level directive would make ``gpu`` a mapping policy
permanently, and would leave every subsequent device type facing a choice
between an inconsistent second spelling and a bare name of its own.  One
directive, an open-ended set of values, is both the consistent shape and the
extensible one; the requester's own follow-up comment asks for the general
form for the same reason.

So every ``--map-by gpu`` in the table above is written ``--map-by
device=gpu`` from here on.  Prefix matching keeps it short in practice —
``--map-by dev=gpu`` is accepted.

Qualifiers: the existing ``--map-by`` qualifiers all apply unchanged
(``PE=n``, ``SPAN``, ``OVERSUBSCRIBE``, ``NOLOCAL``, ``HWTCPUS``, …), and one
is added — see `The interleave qualifier`_.

Note the grammar rules that bite here.  ``pmix_check_cli_option()`` is
``strncasecmp`` over ``min(strlen(given), strlen(defined))`` — a **prefix**
match — so ``--map-by dev=gpu`` must work.  It strips anything from ``=``
onward before comparing, so the ``=value`` form needs no special handling in
the match itself; but the value must then be read with ``qualifier_value()``,
never by indexing past the full spelling.  ``pe-list=`` is the existing
precedent for a directive carrying an ``=value``; follow it exactly.

.. warning::

   ``pmix_check_cli_option()`` does **not** detect ambiguity.  It has no view
   of the other options; it answers one comparison at a time, and the first
   arm of the caller's ``if``/``else if`` chain that prefix-matches wins.  A
   new qualifier is therefore not merely "added" — its **position in the
   chain** is part of its semantics.  See the ordering rule under `The
   interleave qualifier`_.

The interleave qualifier
~~~~~~~~~~~~~~~~~~~~~~~~

**Decided: yes, a package-interleaved device order is wanted, spelled**
``interleave[=<level>]``, **defaulting to** ``level=package``.

It reorders the device list so that consecutive processes land on different
objects of ``<level>``: group the devices by the ``<level>`` object
containing each one's locality, then take one device from each group in
turn, dropping a group when it is exhausted.  Nothing else changes —
placement, binding and ranking all consume the reordered list exactly as
they consume the plain one.

On the requester's machine the GPUs group by package as ``{GPU0, GPU1}`` and
``{GPU2, GPU3}``, so the interleaved order is GPU0, GPU2, GPU1, GPU3 and::

   -n 2 --map-by device=gpu:interleave --bind-to core  -> cores 16, 96
   -n 4 --map-by device=gpu:interleave --bind-to core  -> cores 16, 96, 32, 112

which is the row the requester wanted from ``:span``.

Four properties make this worth having as a qualifier rather than a separate
policy:

**It is a pure reordering.**  It touches the device list and nothing else, so
it composes with every other qualifier for free — including ``SPAN``, which
concatenates the per-node lists *after* each has been interleaved.

**Every level degrades gracefully to the plain order.**  Interleaving never
invents an ordering; it only redistributes across groups.  On the
requester's machine ``interleave=numa`` puts each GPU in its own group and
the round-robin reproduces the PCI-bus order exactly; ``interleave=package``
on a single-package node is likewise a no-op.  So a level that does not
partition the devices is harmless, not an error, and the qualifier is safe to
put in a site's default ``mapby`` parameter.

**Uneven groups need no special case.**  Three GPUs on package 0 and one on
package 1 gives ``G0, G3, G1, G2`` — the exhausted group simply drops out.

``<level>`` **is restricted to objects within a node** (``package``,
``numa``, ``l3cache``, …).  ``node`` is deliberately **not** an accepted
level: interleaving across nodes is what ``SPAN`` already expresses, and
allowing both spellings would give one behavior two names that compose into
nonsense.  The two qualifiers stay orthogonal — ``SPAN`` owns the cross-node
dimension, ``interleave`` owns the within-node one.

Why ``package`` is the right default: it is the only level that does anything
interesting on a conventional two-socket node, and it is the level whose
crossing costs the most.  The parameter still earns its place — a node with
several GPUs per NUMA domain wants ``interleave=numa``, and the requester's
own Cascade Lake AP example (two NUMA domains per socket) is a case where
``package`` and ``numa`` differ.

.. warning::

   **interleave must be tested *after* inherit in the qualifier
   chain.**  They share a first letter, and because the matcher does not
   detect ambiguity, whichever is tested first claims ``:i`` and ``:in``.
   ``:i`` means ``INHERIT`` today.  Placing ``interleave`` earlier would
   silently change the meaning of a working command line — no error, no
   warning, a different mapping.  Append it at the end of the chain, and add
   a test that pins ``:i`` to ``INHERIT``.  (``:inh`` and ``:int`` are
   unambiguous either way.)

**Scope for the first implementation:** accept ``interleave`` only with
``device=``, and refuse it by name elsewhere — ``mapquals[]`` in
``schizo_base_frame.c`` is global, so it will be accepted by the front door
for every ``--map-by`` directive and must be rejected by the parser with a
message that says *why*, as the per-app ``--bind-to`` parser already does for
``report``.  Note the generalization is nearly free once ``byobj`` is
parameterized over its object enumerator (see `Placement`_): interleaving is
a reordering of whatever list the enumerator produced, so
``--map-by numa:interleave=package`` would work the same way.  That is
follow-on work, not part of this change.

Device discovery
~~~~~~~~~~~~~~~~

**Decided: the enumerator goes in PMIx.**  PRRTE consumes it and grows no
enumerator of its own.

PMIx already walks a topology building ``uuid``/``osname``/``type`` triples
for ``PMIX_DEVICE_DISTANCES`` (``pmix_hwloc.c``), owns the UUID grammar
(``gpu://<host>::<osname>``, ``fab://<NodeGUID>::<SysImageGUID>``,
``ipv4://<mac>``), and already accepts an arbitrary ``pmix_topology_t``.  If
PRRTE builds its own list, the name PRRTE tells a process it was assigned and
the name PMIx reports to that same process through ``PMIX_DEVICE_DISTANCES``
can disagree — which makes the assignment uncorrelatable and therefore
useless.  One enumerator, in PMIx.

What PMIx needs added, all of it fixing gaps PMIx has anyway:

* An enumeration entry point — ``pmix_hwloc_get_devices(topo, type, devid,
  &devs, &ndevs)`` — returning devices *with their locality cpuset*, not
  distances from a given cpuset.
* Dedup by parent ``PCIDev`` and ordering by PCI bus id.  PMIx currently
  emits one entry per OS device in hwloc traversal order.
* Replace the current ``strncasecmp(device->name, "card", 4)`` skip — which
  drops the auxiliary DRM card node — with the rules under `The device
  filter`_ below.  That skip happens to produce the right answer on this
  topology and produces *no* GPU at all on a machine whose GPU has a
  ``card*`` node and no render node.
* Stop unconditionally skipping ``HWLOC_OBJ_OSDEV_COPROC``.  That is where
  ``cuda0``/``ze0`` live, i.e. the unambiguous case.

This means a new PMIx capability flag and a floor bump.  PRRTE's floor is
``pmix_min_version = 7.0.0``, so a flag added *after* 7.0 is a real gate
(unlike one added during the 7.0 development series, which every PMIx a
PRRTE build can talk to already defines).  Guard the feature with
``PRTE_CHECK_PMIX_CAP`` and have ``--map-by device=`` be *refused with a
diagnostic* on a PMIx that lacks it — not silently absent.  Do **not** carry
a local copy of the enumerator in PRRTE as a bridge; that is two maintenance
paths for one answer.

The PRRTE-side wrapper is then thin, in ``src/hwloc`` (the directory's rule
is "a pure function of a topology plus a string", which this is):

.. code-block:: c

   typedef struct {
       pmix_object_t       super;
       hwloc_obj_t         locality;  /* nearest non-I/O ancestor - borrowed */
       char               *osname;    /* "renderD128", "mlx5_0"             */
       char               *uuid;      /* "gpu://node07::renderD128"          */
       char               *busid;     /* "0000:06:00.0" - the sort key       */
       pmix_device_type_t  type;
   } prte_hwloc_device_t;

   /* devs is filled in PCI-bus order; empty list is not an error */
   int prte_hwloc_base_get_devices(hwloc_topology_t topo,
                                   pmix_device_type_t type,
                                   const char *name,   /* NULL = all of type */
                                   pmix_list_t *devs);

.. _the device filter:

**The device filter**, in priority order, for ``device=gpu``:

1. If the topology contains any OS device with a vendor backend
   (``Backend`` info of ``CUDA``, ``NVML``, ``RSMI``, ``LevelZero``,
   ``OpenCL``), or of type ``HWLOC_OBJ_OSDEV_COPROC``: those are the GPUs.
   Dedup by parent ``PCIDev``; a function with both a ``cuda0`` and a
   ``renderD*`` node is one device.
2. Otherwise, DRM devices: a ``PCIDev`` of class ``03xx`` that carries a
   ``renderD*`` OS device child.  This is what excludes the ASPEED BMC
   adapter on the requester's machine.
3. Otherwise, no GPUs on this node.  That is a hard error for a job that
   asked to map by one — see `Placement`_.

For ``network``/``openfabrics``/``block`` no filtering beyond the type and
the PCI-function dedup is needed.

Ordering
~~~~~~~~

By PCI bus id (domain, bus, device, function), ascending.  Devices with no
PCI ancestor sort last, by ``osname``.  This is deterministic, identical on
every node with the same hardware, stable across reboots, and — as shown
above — matches ``nvidia-smi``'s enumeration on the requester's machine.

.. warning::

   The **index** PRRTE assigns is a topology index and is not necessarily
   the runtime's device index.  CUDA's default ``CUDA_DEVICE_ORDER`` is
   ``FASTEST_FIRST``, not ``PCI_BUS_ID``.  This is why the assignment must be
   reported to the process as a **name/UUID**, never as a bare ordinal.  See
   `Reporting the assignment`_.

Locality
~~~~~~~~

A device's locality is the cpuset of its nearest ancestor that has one —
``hwloc_get_non_io_ancestor_obj()``, or equivalently climbing ``->parent``
until ``->cpuset`` is non-NULL.  On the requester's machine that ancestor is
a ``Group`` covering exactly one NUMA domain.  On other machines it may be a
``Package``, or the ``Machine`` itself.

The locality object is what the mapper hands to
``prte_rmaps_base_setup_proc()`` as the proc's ``obj``, and therefore what
``bind_generic()`` narrows binding to.  Nothing else in the mapper needs to
know a device was involved.

Placement
~~~~~~~~~

``PRTE_MAPPING_BYDEVICE`` takes the value ``10``, reusing the slot
``PRTE_MAPPING_BYDIST`` occupies today.  That keeps it ``<=
PRTE_MAPPING_RR`` (16), which is what makes ``round_robin`` claim it —
``prte_rmaps_rr_map()``'s gate is ``PRTE_MAPPING_RR <
PRTE_GET_MAPPING_POLICY(options->map)`` → defer.  No new component, and no
change to any other mapper's gate.

The algorithm is ``prte_rmaps_rr_byobj()`` with one substitution.  ``byobj``
is already exactly "walk the objects of one type on each node, one proc per
object, wrapping until the node is full or the procs run out"; the only thing
device mapping changes is **where the object list comes from**:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - ``byobj``
     - ``bydevice``
   * - ``prte_hwloc_base_get_nbobjs_by_type(topo, options->maptype)``
     - length of the device list for this node
   * - ``prte_hwloc_base_get_obj_by_type(topo, options->maptype, j)``
     - ``devs[j]->locality``
   * - zero objects → ``rmaps:mapping-target-not-found``, ``PRTE_ERR_SILENT``
     - zero devices → the same, naming the device spec

Everything else — ``get_cpuset``, ``check_support``, ``get_ncpus``,
``check_avail`` (including its node-removal contract and the ``nodefull``
guard around ``redo:``), ``setup_proc``, ``check_oversubscribed``, the
``span``/non-``span`` split, the second-pass overflow handling — is unchanged
and must stay unchanged.

The ``span``/non-``span`` split needs no device-specific handling and gives
the right answer for free.  Non-``span`` works a node's devices until the
node is full before moving on, so a job front-loads onto the first nodes.
``SPAN`` gathers every device of the requested class in the allocation into
one super-node — node *N*'s devices in PCI-bus order, then node *N+1*'s —
and assigns one process per device across the whole list, wrapping when the
processes outnumber the devices.  For device mapping that is exactly the
useful distinction: "fill this node's GPUs first" versus "one process per
GPU across the whole allocation, then a second round".

The cleanest implementation is therefore **not** a copy of ``byobj`` but a
generalization of it: give ``byobj`` an object-enumerator argument (a small
struct of "how many on this node" and "give me the *j*-th"), with the
existing hwloc-type enumerator and a new device enumerator as its two
instances.  A copy would be ~200 lines duplicating the two subtlest loops in
the mapper (``redo:``/``check_avail`` and the oversubscribe second pass),
which is how those bugs come back.

Because the device list is per node and rebuilt each pass, it must be cached
for the duration of the map rather than recomputed inside the ``j`` loop —
but it must **not** be cached across jobs.  ``rank_file`` and ``lsf`` both
carry file-static state that outlives a job, and both have had a stale value
handed to the next job as its process count.  Build the list into the
per-node scratch and free it when the node is done.

Binding, and the bind ceiling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Binding needs no new code.  ``bind_generic()`` intersects
``options->target`` (which ``check_avail`` computed from the proc's ``obj``,
i.e. the device locality) with each candidate object of type ``options->hwb``
and takes the first with free cpus.  With the locality object as ``obj``:

* ``--bind-to core`` → the first free ``Core`` inside the device's cpuset;
* ``--bind-to l3cache`` → the first ``L3Cache`` inside it;
* ``--bind-to numa`` → the ``NUMANode`` inside it.

``set_proc_cpuset()`` writes ``trg_obj->cpuset ∩ node->jobcache``, so a
DVM-wide ``--cpu-set`` still applies, as it does everywhere else.

The one genuinely new rule is **the bind ceiling**.  PRRTE refuses to bind
above where it mapped, and the job-level check for that
(``options.mapdepth > options.bind``) compares two ``prte_binding_policy_t``
values fixed before any node is looked at.  Neither operand works here.  A
device's locality is not known until the node is known, and it differs
between machines — NUMA-sized on the requester's node, package-sized on a
machine where the GPUs hang off one complex per package.  And the locality
object is frequently a ``Group``, which has no position in the
``PRTE_BIND_TO_*`` ladder at all, so there is no value ``mapdepth`` could be
set to that would make the fixed comparison mean the right thing.

The check therefore moves, and the place for it is **the device mapper,
immediately after the node's device list is built and before any process is
placed on that node**:

* Leave ``options.mapdepth = PRTE_BIND_TO_NONE`` in the job-level switch, so
  the fixed check does not fire on a basis that is not yet known.
* If binding is in force, compare **cpusets**, not levels: the request cannot
  be honored when the binding object's cpuset is a strict superset of the
  device locality's.  Report ``bind-upwards`` and fail the job.

That point is the earliest at which the answer is knowable, and it fails the
job before a partial map exists — a mapper that discovers half way through a
node that it cannot bind has already consumed slots and added the node to
``jdata->map->nodes``.

The obvious alternative — putting the test in ``bind_generic()``, which is
the one function holding both the locale and ``options->hwb`` — was
considered and rejected.  It would generalize to every mapper, but
``bind_generic()`` currently returns ``PRTE_SUCCESS`` and silently declines
to bind whenever binding is not *required*, and turning it into a function
that can fail a job changes behavior for the colocation path (which reaches
binding without going through ``get_target_nodes`` and can carry an
unexpected locale) and for every existing object map, where the job-level
check has already settled the question and the new one could only ever
disagree with it.  One caller needs this rule; give it to that caller.

Under this rule, on the requester's machine:

* ``--bind-to package`` is an **error** (package 0's cpuset ⊃ NUMA 1's).
  That is the behavior they leaned toward, and it is the behavior consistent
  with the rest of PRRTE.
* ``--bind-to numa``, ``l3cache``, ``core``, ``hwthread`` are all accepted.
* On a machine where the GPU's locality *is* the package, ``--bind-to
  package`` is accepted, because there it is not above the map.

``PE=n`` needs no special handling and answers the requester's open question:
``--map-by device=gpu:PE=2 --bind-to core`` gives rank 0 two cores inside
GPU0's locality — cores 16 and 17 — through the existing ``bind_multiple()``
path.  It is neither an error nor core 0.

Reporting the assignment
~~~~~~~~~~~~~~~~~~~~~~~~

PRRTE cannot enforce a GPU assignment, but the assignment is worthless if the
process cannot learn it.  Record it on the proc and publish it:

* A new proc attribute (next free key in the ``PRTE_PROC_*`` band) holding
  the device's **UUID and osname** — not an index, for the
  ``CUDA_DEVICE_ORDER`` reason above.
* Published to the process through the PMIx server registration as
  ``PMIX_DEVICE_ID``, which PMIx documents as exactly this: "system-wide UUID
  or node-local OS name of a particular device".  The process retrieves it
  with ``PMIx_Get(&myproc, PMIX_DEVICE_ID, ...)``, and can then correlate it
  against ``PMIX_DEVICE_DISTANCES`` for the full distance vector — which is
  the "closest device plus distances to all others" the maintainer described
  in the issue.
* Shown by ``--display map`` on the proc line, beside ``Bound:``.  A user who
  types ``--map-by device=gpu`` will check it with ``--display map`` first,
  and a placement they cannot see is a placement they will not trust.

Setting ``CUDA_VISIBLE_DEVICES`` and its siblings is deliberately **out of
scope** — see `Out of scope`_.

Degenerate topologies
~~~~~~~~~~~~~~~~~~~~~

If every device of the requested class resolves to the same locality object —
the "all GPUs hang off one PCI complex per package" shape, and the ``Machine``
root case where a device has no locality at all — then the *binding* half of
the request is vacuous: every process is equidistant from every device.

**Proceed, do not refuse.**  The device *assignment* is still meaningful and
is half of what was asked for: four processes on a node with four
equidistant GPUs still each get a distinct GPU, reported through
``PMIX_DEVICE_ID``, and that is a result no other directive produces.
Emit a warning through ``prte_show_help`` naming the locality object the
devices share, so the user is not left believing they got a locality-aware
placement they did not get.

Heterogeneous nodes
~~~~~~~~~~~~~~~~~~~

Device lists are built per node from that node's own topology, so a DVM whose
nodes carry different device counts maps correctly by construction — *except*
under ``prte_uniform_nodes`` (``--prtemca prte_uniform_nodes 1``), where only
daemon rank 1 reports a topology and every other node inherits it.  A
device-mapped job under that flag on a non-uniform cluster will place
against a topology that is not the node's.  Document it; do not try to detect
it (nothing else in the tree does either).

A node with **zero** devices of the requested class is a hard error, not a
skipped node — the same rule ``byobj`` already applies to a node with no
object of the mapping type.  Quietly dropping the node shrinks the
allocation the user gave us without saying so.


Where the code goes
-------------------

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - File
     - Work
   * - **openpmix** ``src/hwloc/pmix_hwloc.[ch]``
     - The device enumerator: dedup by PCI function, PCI-bus ordering,
       backend/render-node filtering, COPROC no longer skipped.  New
       capability flag.
   * - ``src/hwloc/hwloc-internal.h``, ``hwloc_base_util.c``
     - ``prte_hwloc_device_t``, ``prte_hwloc_base_get_devices()`` — the thin
       wrapper over the PMIx call, plus the locality climb.
   * - ``src/mca/rmaps/rmaps_types.h``
     - ``PRTE_MAPPING_BYDEVICE`` replaces ``PRTE_MAPPING_BYDIST`` (value 10).
       Rename ``options->dist_device`` → ``options->map_device``.
   * - ``src/util/prte_cmd_line.h``
     - ``PRTE_CLI_DEVICE`` (``"device="``) and ``PRTE_CLI_INTERLEAVE``
       (``"interleave"``).  Delete ``PRTE_CLI_DIST``.
   * - ``src/mca/schizo/base/schizo_base_frame.c``
     - Add ``device=`` to ``mappers[]`` and ``interleave`` to ``mapquals[]``.
       **The front door** — a name missing here never reaches a parser.
   * - ``src/mca/rmaps/base/rmaps_base_frame.c``
     - Both ``--map-by`` parsers (job-level ``prte_rmaps_base_set_mapping_policy``
       and per-app ``prte_rmaps_base_set_app_mapping_policy``) — they must
       agree, which is the recurring bug in this file.  Record the spec on
       ``PRTE_JOB_DIST_DEVICE`` / ``PRTE_APP_DIST_DEVICE`` (renamed).  Add the
       ``interleave`` arm **at the end of** each qualifier chain, after
       ``inherit``.  Fix the ``mapby`` MCA help string.  Delete
       ``prte_rmaps_base.device``.
   * - ``src/mca/rmaps/base/rmaps_base_map_job.c``
     - The policy switch case; leave ``mapdepth = PRTE_BIND_TO_NONE``.
       ``resolve_app_options()`` already threads the string — rename only.
   * - ``src/mca/rmaps/base/rmaps_base_print_fns.c``
     - ``"MINDIST"`` → ``"BYDEVICE"``.
   * - ``src/mca/rmaps/round_robin/rmaps_rr_mappers.c``
     - Generalize ``byobj`` over its object enumerator; add the device
       enumerator, the interleave reordering, and the per-node bind-ceiling
       check.
   * - ``src/mca/rmaps/base/help-prte-rmaps-base.txt``
     - New topics: no devices of that class on a node; binding above the
       device locality; the degenerate-locality warning.  **Then**
       ``rm src/util/prte_show_help_content.* && make``.
   * - ``src/util/attr.[ch]``
     - Rename the two ``DIST_DEVICE`` keys; add the per-proc assigned-device
       key.
   * - ``src/prted/pmix/pmix_server_register_fns.c``
     - Publish ``PMIX_DEVICE_ID`` per proc.
   * - ``src/runtime/data_type_support/prte_dt_print_fns.c``
     - Show the assigned device on the ``--display map`` proc line.
   * - ``docs/``
     - ``--map-by`` reference and the ``prterun --help map-by`` text, which
       today does not mention ``dist`` at all even though the MCA parameter
       does.

Note that ``--bind-to`` needs **no** change: the ceiling check is in the
mapper, and no new binding target is introduced.


Deviations from the requester's table
-------------------------------------

Every row is answerable, but none of them is spelled the way it is written in
the issue, and one is answered differently from either of the requester's
guesses.  Stated here rather than resolved silently.

**The directive is device=gpu, not gpu.**  Every row of the table
uses the bare shorthand; it is not accepted, deliberately — see the grammar
discussion under `Grammar`_.  ``--map-by dev=gpu`` is the short form in
practice.  This is the one deviation that touches every line the requester
wrote, so it is worth saying plainly in the issue reply rather than leaving
them to infer it from an example.

``:span`` **will not spread across sockets.**  The requester expects

.. code-block:: text

   -n 2 --map-by device=gpu --bind-to core       -> cores 16, 32  (GPU0, GPU1)
   -n 2 --map-by device=gpu:span --bind-to core  -> cores 16, 96  (GPU0, GPU2)

i.e. ``span`` meaning "spread across packages".  That is not what ``SPAN``
means.  ``SPAN`` gathers **all objects of the mapped type into one
super-node** — every object of node *N*, followed by every object of node
*N+1*, and so on — and then assigns one process to each object in turn,
wrapping when the objects run out and the processes have not.  Applied to
``device=gpu``, it collects every GPU in the allocation into a single ordered
list and round-robins the processes over it.

The qualifier therefore already does the right thing for device mapping; it
simply does not do the thing this row wants.  Within a node the super-node
order is still PCI-bus order, so ``-n 2`` takes the first two GPUs — GPU0 and
GPU1, cores **16 and 32**.  On one node the result coincides with the
non-``span`` result, because there is only one node's worth of objects to
concatenate; the two diverge as soon as there is a second node, where
non-``span`` fills node A's GPUs before touching node B's and ``span``
alternates between them.

Getting cores 16 and 96 means ordering the device list so that consecutive
processes land on different *packages* — an interleave, not a concatenation.
That is a distinct behavior and it gets its own name: **the requester's rows
are produced by** ``:interleave``, not ``:span``.  See `The interleave
qualifier`_.  So this is a spelling deviation rather than a missing
capability — the behavior they asked for is in scope and planned; the
qualifier they guessed at is already taken by something else.

Note that the requester's other stated rationale — the Cascade Lake AP case,
where the NIC hangs off one particular NUMA domain and processes must reach
*it* — is served by the plain ordering, not by an interleaved one.  Both
orderings are wanted, which is why this is a qualifier and not a change to
the default.

``--bind-to package`` **is an error, not "cores 0-63".**  The requester
offered both.  Erroring is what the rest of PRRTE does when asked to bind
above the map, and silently handing back the whole package is precisely the
locality loss they filed the issue about.

``--map-by device=gpu:PE=2 --bind-to core`` **gives rank 0 cores 16-17.**  The
requester guessed "error or core 16 or core 0".  ``PE=n`` already means "n
cpus per process", and there is no reason for it to mean anything else here;
their worry that a GPU has nothing "beneath" it does not arise, because
binding descends from the device's *locality*, not from the device.


Testing
-------

**Add the requester's topology to the tree.**  ``hwloc_topology.xml`` from
the issue goes into ``test/topologies/`` (as, say, ``turin-4gpu.xml``).  It is
the only topology available with a non-uniform GPU-to-NUMA mapping *and* a
decoy display adapter, and both properties are load-bearing.  Note the
existing topologies there carry no I/O objects at all, so this is also the
first one that exercises the I/O half of the topology import.

Four layers, each answering something the others cannot:

**1. Unit — test/unit/hwloc/test_hwloc.c.**  The enumerator is a pure
function of a topology plus a string, which is this directory's whole design
rule.  Drive it from the embedded XML: that ``device=gpu`` finds four devices
and not five or nine; that they come out in PCI-bus order; that their
localities are NUMA 1, 2, 6, 7; that ``device=openfabrics`` finds six and
``device=network`` seven; that ``device=mlx5_0`` finds exactly one; that a
name that does not exist finds none.  This is where the ASPEED decoy is
pinned.

The interleave reordering is pure too, and belongs here beside the
enumerator: that ``interleave=package`` on this topology yields GPU0, GPU2,
GPU1, GPU3; that ``interleave=numa`` yields the unchanged PCI-bus order (the
graceful-degradation property); and that an uneven split drops the exhausted
group rather than repeating or skipping.  The uneven case needs a
hand-written XML — hwloc's synthetic generator cannot produce I/O devices at
all, which is worth stating in the file, since every other topology in that
test is synthetic.

**2. Unit — test/unit/rmaps/.**  ``test_policy_parse.c`` and
``test_job_policy.c``: that ``device=gpu`` parses at both levels and to the
same result, that the abbreviated ``dev=gpu`` works, that the value is read
after the ``=`` and not at a fixed offset, and that a bare ``--map-by gpu``
is **refused** — the shorthand the issue is written in must not quietly
become a second spelling.
For the new qualifier: that ``:interleave`` defaults to ``package``, that
``:interleave=numa`` is honored, that ``node`` as a level is refused, that
``interleave`` on a non-``device=`` map is refused *with its own message*
rather than as an unknown qualifier — and, the regression that matters,
**that :i and :in still resolve to INHERIT**.  That last one is
the whole guard against the chain-ordering hazard, and it is invisible to
every other kind of test.  ``test_dispatch.c``: that ``round_robin`` claims
the policy and the specialized mappers defer on it.

**3. Offline harness — make -C test/offline check-offline.**  This is the
cheapest place to check the *whole* requested table, because it drives
``prterun --rtos donotlaunch --display map --prtemca hwloc_use_topo_file``
against exactly this kind of XML.  Every row above becomes a case with a
golden map, in both orderings — the plain one and ``:interleave``, which is
the pair the requester's table is really asking about.  The harness's
invariant checker will need to learn the new directive, including that
``--bind-to package`` under it is an expected *rejection*, which the harness
already models for ``must-map-by-obj``.

**4. Multi-node — contrib/dockerswarm, test_rmaps().**  What the
first three cannot show: that the HNP maps against each node's *own*
topology.  The containers have no GPUs, so the case that runs there is the
negative one and the ``device=network`` one — a job asking for a device class
no node has must fail the job and **leave the DVM standing**, and a
``device=network`` map must place procs against the container's actual
interfaces.  The GPU cases stay offline, against the XML.


Decisions already taken
-----------------------

* **The enumerator goes in PMIx**, with a capability flag and a floor bump.
  PRRTE grows none of its own.  A second enumerator would let PRRTE's "you
  were assigned device X" and PMIx's ``PMIX_DEVICE_DISTANCES`` answer to the
  same process disagree about that device's name, which makes the assignment
  uncorrelatable.  The cost — ``--map-by device=`` refused against an older
  PMIx — is accepted.
* **The bind ceiling lives in the device mapper**, per node, right after the
  device list is built, comparing cpusets rather than ``PRTE_BIND_TO_*``
  levels.  ``bind_generic()`` was the alternative and was rejected; see
  `Binding, and the bind ceiling`_.
* **A package-interleaved device order is wanted**, spelled
  ``interleave[=<level>]`` with ``level=package`` as the default, restricted
  to within-node levels, and tested *after* ``inherit`` in the qualifier
  chain.  See `The interleave qualifier`_.
* ``device=<class>`` **is the only spelling** — no bare ``--map-by gpu``.
  The device class is a *value*, so a future class is a new value rather
  than a new directive, and no later device type has to choose between an
  inconsistent second spelling and a bare name of its own.
* **A device is assigned, not shared**, and sharing has its own qualifier.
  Left to ``byobj``'s wrap, ``-n 8`` on a four-GPU node would have put two
  processes on each GPU by inheritance rather than by decision.  A device is
  assigned to a process rather than subdivided between them — which is not
  true of a core — so more processes than devices is an error, and
  ``--map-by device=<class>:shared`` is what permits it.  ``shared``
  defaults to false, pending user feedback.

  It deliberately does **not** ride on ``--bind-to``'s
  ``overload-allowed``.  That qualifier is about running more processes
  than there are CPUs; this one is about handing one device to several
  processes.  Different resource, different decision — and answering both
  with one word would leave neither sayable on its own, so a job that wants
  to share GPUs while still refusing to overload its cores could not say
  so.  ``shared`` is tested *after* ``span`` in the qualifier chain for the
  same reason ``interleave`` is tested after ``inherit``: ``:s`` has meant
  ``SPAN`` for as long as there has been one.


Decisions still to make
-----------------------

1. **Does device= need a per-app spelling test in the swarm?**  Per-app
   and job-level ``--map-by`` parsers disagreeing is the single most
   repeated bug in ``rmaps_base_frame.c``; the unit test covers it, but an
   MPMD line with a different device class per app is the shape that would
   catch a plumbing error in ``resolve_app_options()``.


Out of scope
------------

* **Setting CUDA_VISIBLE_DEVICES / ROCR_VISIBLE_DEVICES /
  ZE_AFFINITY_MASK.**  These are vendor conventions with different
  semantics (AMD's own documentation warns about the interaction between
  ``ROCR_VISIBLE_DEVICES`` and ``CUDA_VISIBLE_DEVICES``), and injecting them
  makes PRRTE responsible for a vendor's device-ordering rules.  PRRTE
  reports the assignment; a site's launch wrapper, or the MPI library, can
  act on it.  If this is later judged in scope, it belongs behind an explicit
  opt-in and in the framework that already handles vendor-specific syntax,
  not in the mapper.

  .. note::

     **This has since been judged in scope**, on the terms this bullet set:
     in ``pgpu``, the framework that already owns vendor-specific syntax,
     never in the mapper.  See *Phase H* of :doc:`impl-plan`.  What changed
     is that the requester rebuilt hwloc against CUDA/NVML and supplied a
     topology carrying ``NVIDIAUUID``, which makes the sound route below
     available; the paragraph at the end of this bullet describes the
     *original* topology and is kept because it is still what a distro
     hwloc produces.

     One decision here was reversed rather than refined.  Where this bullet
     concludes "otherwise set nothing, and say so", the ruling is now to
     **refuse the mapping request outright** when no vendor identity is
     available.  Mapping by device and then declining to act on it is
     indistinguishable from a working run until someone measures.

  Four constraints from `NVIDIA's
  documentation
  <https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/environment-variables.html#cuda-visible-devices>`_
  settle most of the design and are worth not rediscovering:

  1. The accepted values are an integer index, ``GPU-<uuid>`` (full or an
     abbreviated unique prefix), or ``MIG-<uuid>/<gi>/<ci>``.  **There is no
     PCI bus id form.**  That rules out the obvious route: the bus id is the
     one handle hwloc always has, and it is not a legal value.
  2. ``GPU-<uuid>`` is *order-independent*, so using it means PRRTE never
     has to touch the user's ``CUDA_DEVICE_ORDER``.
  3. The index route would require pinning ``CUDA_DEVICE_ORDER=PCI_BUS_ID``,
     since the default ``FASTEST_FIRST`` is an unspecified heuristic.  That
     overrides a possibly-deliberate user setting and renumbers devices for
     the rest of their program — too much to take on someone's behalf.
  4. A wrong value fails *silently*: "if an invalid index is encountered,
     only devices with indices that appear before the invalid index in the
     list are visible."  A guess truncates the visible set rather than
     erroring, so guessing is not a safe option.

  The rule that falls out: emit the comma-joined ``GPU-<uuid>`` of the
  assigned devices **only** when hwloc supplies ``NVIDIAUUID`` for all of
  them.  Never set ``CUDA_DEVICE_ORDER``.  An unset variable leaves CUDA
  behaving normally; a wrong one does not.

  Note where that left the requester of the originating issue *before* they
  rebuilt hwloc, since it is still where a distro hwloc leaves everyone
  else.  hwloc sets ``NVIDIAUUID`` only in its **NVML** backend
  (``src/topology-nvml.c``); a DRM/PCI-only hwloc reports
  ``card*``/``renderD*`` and a bus id and nothing else.  The GPU OS devices
  in their original topology carry *no* info attributes at all, so the
  sound route was unavailable on exactly the machine that asked for the
  feature.  The deciding hwloc is the one **PRRTE is built against** —
  ``prted`` probes each node itself — so a site wanting the envars must
  build PRRTE against an hwloc with the vendor backend, not merely run one
  by hand.  The same shape applies to
  AMD (``ROCR_VISIBLE_DEVICES`` takes the same index-or-uuid values, and the
  RSMI backend supplies ``AMDUUID``), so any future work here is one rule
  with a per-vendor info key, not a per-vendor design.
* **Deriving the device count from the resource manager** (SLURM's
  ``--gpus-per-task``).  The requester raises it as an "even more ideally".
  It is a ``ras``-side question — what the allocation granted — and is a
  separate piece of work with its own scheduler-specific parsing.
* **Enforcing exclusivity.**  There is no mechanism.  Two jobs mapping by
  device on the same node will both be told they own GPU0.
* **Distance-weighted placement.**  The old ``dist`` policy nominally
  ordered by hwloc distance.  This plan orders by PCI bus and binds within
  the device's locality, which is what the requester actually asked for and
  what the topology supports.  Nothing here forecloses a distance-ordered
  variant later.
