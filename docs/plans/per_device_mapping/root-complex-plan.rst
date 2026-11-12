Root complexes: device groups and companion NICs
================================================

Two extensions to ``--map-by device=``, both prompted by Open MPI's
btl/ofi NIC selection rework (`open-mpi/ompi#14800
<https://github.com/open-mpi/ompi/pull/14800>`_).  That PR measured, on
EFA, what happens when one process drives NICs under more than one PCI
root complex:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - NICs per process (64 KB RDMA reads, 64 outstanding)
     - Bandwidth
     - Retransmitted
   * - 2, one root complex
     - 22379 MB/s
     - 0.00%
   * - 2, two root complexes
     - 1940 MB/s
     - 32.95%
   * - 4, one root complex
     - 23881 MB/s
     - 0.00%
   * - 8, two root complexes
     - 859 MB/s
     - 36.09%

Its response was to group the node's NICs by root complex and hand each
process NICs from one complex only.  This document covers what PRRTE
should take from that:

* **Part 1** — an ``ndev`` group should not span root complexes when it can
  be avoided.  A correction to existing behavior.
* **Part 2** — a process mapped by GPU should be able to be given the NICs
  under its GPU's root complex.  A new qualifier.
* **Part 3** — PMIx ``pnet`` support for the fabrics whose NIC
  assignments are currently delivered but acted on by nothing: RoCE on
  NVIDIA, Broadcom and Intel adapters, EFA, and Slingshot.  A PMIx
  change, independent of the other two.

Read it alongside :doc:`per-device-mapping` and :doc:`impl-plan`, and
`src/mca/rmaps/base/AGENTS.md
<https://github.com/openpmix/prrte/blob/master/src/mca/rmaps/base/AGENTS.md>`_,
whose rules are assumed here.

.. note:: The numbers above are from one fabric.  Nobody has measured the
   effect on InfiniBand, RoCE or Slingshot, so nothing here refuses a
   placement on the strength of it; both parts are *preferences*.  For
   GPUs with ``ndev=2`` there is an independent reason in the same
   direction: GPU peer-to-peer traffic between two root complexes crosses
   the CPU interconnect, or is not offered at all.


What a root complex is, here
----------------------------

hwloc represents each PCI root bus as a ``Bridge`` object whose upstream
side is the host (``bridge_type="0-1"`` in the XML, upstream type
``HWLOC_OBJ_BRIDGE_HOST`` in the API).  Every device under that bridge
shares a root complex.  This is the definition the OMPI PR uses (it climbs
from the device's PCI function to the last bridge before a non-I/O
parent), and on the platforms in hand it is one host bridge per root
complex.

The key is reachable today **without a PMIx change**: every
``pmix_hwloc_device_t`` carries ``obj``, the device's position in the I/O
tree, borrowed from the same topology as ``locality``.  Walking up from
``obj`` while the parent is a ``Bridge`` or ``PCIDev`` ends at the host
bridge.  A device with no PCI ancestor has no host bridge and is treated as
a complex of its own.

The CPU-side locality PRRTE already computes **cannot** see this.  Two host
bridges routinely share a cpuset: in ``test/topologies/test-topo.xml``
``ib0`` and ``eth1`` sit under different host bridges with identical
cpusets, so a group holding both passes ``binding_fits()`` and the
degenerate-locality check and nothing reports it.  So the key has to come
from the I/O tree, not from ``locality``.


Part 1: keep an ``ndev`` group under one root complex
-----------------------------------------------------

Current behavior
~~~~~~~~~~~~~~~~

``prte_rmaps_base_devices_begin()`` forms group *n* from devices
``n*per .. n*per+per-1`` of PMIx's list, which is in PCI bus id order.
Bus order keeps a root complex's devices contiguous, but nothing aligns
group boundaries to the runs.  ``test/topologies/turin-4gpu.xml``:

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Bus id
     - Device
     - Host bridge (document order)
   * - ``0000:03:00.0``
     - ``mlx5_0``
     - HB3 (with GPU ``renderD128``)
   * - ``0000:13:00.0``
     - ``mlx5_3``
     - HB5 (with GPU ``renderD129``)
   * - ``0000:51:00.0``
     - ``eno6``
     - HB1
   * - ``0000:61:00.0``
     - ``mlx5_1``
     - HB8
   * - ``0000:61:00.1``
     - ``mlx5_2``
     - HB8
   * - ``0000:93:00.0``
     - ``mlx5_4``
     - HB13 (with GPU ``card3``)
   * - ``0000:e3:00.0``
     - ``mlx5_5``
     - HB16 (with GPU ``card4``)

``--map-by device=nic:ndev=2`` gives {``mlx5_0``, ``mlx5_3``},
{``eno6``, ``mlx5_1``}, {``mlx5_2``, ``mlx5_4``}.  All three groups span
two root complexes, and the one pair on the machine that shares a complex
(the two ports of the card at ``61:00``) is split between two processes.

The rule
~~~~~~~~

When ``ndev > 1`` and ``interleave`` was **not** given:

1. Partition the device list by host bridge, keeping bus order within
   each partition and ordering partitions by their first device.
2. Cut each partition into whole groups of ``per``, in order.
3. Pool what is left over from every partition, in bus order, and cut
   that pool into groups of ``per``.  These are the groups that have to
   span complexes, and they come **after** every single-complex group, so
   a job that does not need them never gets them.

Turin, ``device=nic:ndev=2``: {``mlx5_1``, ``mlx5_2``}, then
{``mlx5_0``, ``mlx5_3``}, {``eno6``, ``mlx5_4``}; ``mlx5_5`` is left
over, as it is today.

**The group count does not change.**  With partitions of sizes
``s1 .. sk`` and ``p`` devices per group, the single-complex groups number
``sum(floor(si / p))`` and the pool holds ``L = sum(si mod p)`` devices,
so the total is ``sum(floor(si / p)) + floor(L / p) = floor(N / p)`` -
which is what ``prte_rmaps_base_devices_total()`` already computes.  The
pre-count stays as it is, and no job that fits
today is refused for want of devices.

**Where nothing can be kept together, nothing changes.**  When no complex
holds ``per`` devices, every device goes to the pool, and the pool in bus
order is today's list.  This is the case for the GPUs on both Turin
topologies (one GPU per complex), so the existing ``ndev`` golden maps
and the ``ndev=2`` worked example in ``cli-map-by.rst`` are unaffected.

Why ``interleave`` turns it off
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ndev=2:interleave`` is documented to give each process one device from
each package: the user is asking for groups that span.  Applying the
complex rule first and then interleaving would answer neither request.
With ``ndev`` of one the rule has nothing to do, so the only combination
where the two meet is the one where the user has said what they want.

Interaction with the binding check — a behavior change to accept
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A group's locality is the common ancestor of its members' localities.
A single-complex group usually has a **finer** locality than the spanning
group it replaces.  ``binding_fits()`` refuses a binding coarser than a
group's locality, so a ``--bind-to`` that every spanning group admitted can
be refused for a single-complex one.  On Turin, ``device=nic:ndev=2
--bind-to package`` would now be refused for {``mlx5_1``, ``mlx5_2``},
whose locality is one 16-core chunk rather than a package.

This is the same answer ``ndev=1`` already gives for those two devices,
and the old acceptance depended on the group spanning, which is the defect
being fixed.  So the recommendation is to accept it and document it, not to
loosen the check.  It is the one place Part 1 can make a command that
worked stop working, so the release note should name it.

Where the code goes
~~~~~~~~~~~~~~~~~~~

* ``src/hwloc/``: ``prte_hwloc_base_host_bridge(hwloc_obj_t obj)``,
  returning the host bridge above an I/O object or ``NULL``.  It belongs
  beside the other topology helpers rather than in rmaps, since Part 2
  and anything else that asks "which complex" needs the same answer.
* ``rmaps_base_devices.c``: a ``group_by_complex()`` step in
  ``devices_begin()`` between the interleave reorder and the grouping
  loop.  It reorders ``dc->devs`` the way ``interleave_devices()`` does —
  shallow copy and ``memcpy`` back, each entry moved exactly once, so
  string ownership moves with it — and the existing grouping loop then
  runs unchanged.  Allocation failure leaves the list in bus order, which
  is a valid (today's) answer, matching ``interleave_devices()``.
* No change to ``prte_rmaps_options_t``, the module struct, attributes or
  the wire, so no rmaps version bump and no PMIx change.

Tests
~~~~~

* ``test/unit/rmaps/test_devices.c``, on ``turin-4gpu.xml``:
  ``device=nic:ndev=2`` yields the three groups above in that order,
  ``mlx5_5`` unassigned; ``device=gpu:ndev=2`` yields bus order unchanged;
  ``device=nic:ndev=2:interleave`` yields today's interleaved order; on
  ``test-topo.xml``, two NICs under two host bridges with one cpuset yield
  one pooled group.
* ``test/offline``: a golden for ``--map-by device=nic:ndev=2`` on
  ``turin-4gpu-nvml``, plus a case for the refused ``--bind-to package``.
  Every existing golden must be byte-identical.

Docs
~~~~

* ``cli-map-by.rst``, ``NDEV``: replace "a group is a contiguous run of
  that order" with the rule above, and the binding consequence.
* ``src/mca/rmaps/base/AGENTS.md``: the rule, why ``interleave`` turns it
  off, and that the key comes from ``obj`` rather than ``locality``.


Part 2: give a GPU-mapped process the NICs on its GPU's root complex
--------------------------------------------------------------------

The problem
~~~~~~~~~~~

``--map-by device=gpu`` tells each process which GPU it has.  Nothing tells
it which NIC to use with that GPU, so each communication library guesses.
The OMPI PR guesses by assuming ``gpu = local_rank * ngpus / local_size``
and assigns contiguous blocks of local ranks to each root complex so that
the guess comes out right.  That guess is correct only for one placement.
PRRTE does not need to guess: it placed the GPU.

On ``turin-4gpu.xml`` every GPU shares a host bridge with exactly one
InfiniBand HCA (``renderD128``–``mlx5_0``, ``renderD129``–``mlx5_3``,
``card3``–``mlx5_4``, ``card4``–``mlx5_5``), which is the common shape on
GPU nodes and is already in the tree as test material.

What already works, verified against PMIx master
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The delivery path does not need to change.  ``PRTE_PROC_DEVICE_ID`` is
already an array of ``pmix_device_t`` delivered as ``PMIX_DEVICE_ID``, and
both PMIx consumers already select their own devices out of a mixed array:

* ``pgpu`` (``pmix_pgpu_base_get_visible_devices()``) keeps only devices
  whose ``vendor`` matches the component's, which a NIC never has.
* ``pnet`` (``pmix_pnet_base_get_assigned_devices()``) keeps only devices
  whose PCI ``(vendor, class)`` matches the component's list, which a GPU's
  class ``03xx`` never does.

So adding NICs to a GPU process's array sets ``CUDA_VISIBLE_DEVICES`` from
the GPUs and ``NCCL_IB_HCA`` / ``UCX_NET_DEVICES`` from the NICs, with no
PMIx change.  Part 2 is a PRRTE-only change.

Two gaps in what acts on the assignment:

* ``pnet/nvd`` matches class ``0x207`` (InfiniBand) only, so a RoCE
  adapter (class ``0x200``) gets ``PMIX_DEVICE_ID`` but no variable, and
  EFA and Slingshot have no ``pnet`` component at all.  Part 3 closes
  this.
* btl/ofi chooses NICs from the local rank alone and reads no assignment
  from PMIx.  On EFA Part 3 makes that moot: ``FI_EFA_IFACE`` is applied
  inside libfabric's provider, so btl/ofi and mtl/ofi are only ever shown
  the assigned NICs.  Elsewhere on the ofi path it remains a suggestion
  for that project.

Proposed design
~~~~~~~~~~~~~~~

``--map-by device=gpu:nic[=N]`` — valid only when the device class is GPU
(by class or by naming one GPU), refused by name otherwise.  It does not
change placement or binding: the process is placed and bound against its
GPU group exactly as without it.  It only adds NICs to what
``prte_rmaps_base_devices_record()`` publishes.

Per node, in ``devices_begin()`` when the qualifier is present, enumerate
the NIC class (``PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS``) a
second time and keep it on the context.  For each GPU group, the candidate
NICs are those under the host bridge of any of the group's GPUs.  Within
the candidates, a PCI function carrying an OpenFabrics device ranks ahead
of one that does not, then bus order.

When several groups share a complex (two GPUs under one host bridge),
the complex's candidates are dealt so that the groups get distinct NICs
while there are enough and share once there are not.  That is the OMPI
PR's division of a complex's NICs among the processes on it, done with the
real assignment rather than a guess.  Processes that share one GPU
(``shared``) share its NICs: the choice is made per group, which is what
``devices_record()`` publishes.

Both mappers go through ``devices_begin()`` / ``devices_record()``, but
``ppr`` refuses device qualifiers in its parser (``ndev`` included), so in
practice the qualifier applies to ``--map-by device=``.

The qualifier is a new field at the end of ``prte_rmaps_options_t``, with
a new ``PRTE_JOB_MAP_NIC`` / ``PRTE_APP_MAP_NIC`` pair at the next free
offsets, parsed by both ``--map-by`` parsers and added to
``prte_cli_mapquals``.  Only the base reads the field, and it is appended,
so the rmaps interface version does not change - the same reasoning under
which ``ndev``, ``shared`` and ``interleave`` were added.

Decisions
~~~~~~~~~

Settled before any code was written:

1. **Spelling: ``:nic[=N]``** on the existing directive.  ``nic`` is
   already the class word for these devices.  Refused by name for a
   non-GPU class.  ``n`` and ``nd`` stop being unique abbreviations, which
   the vocabulary matcher already reports as ambiguous.
2. **Bare ``nic`` is one per process.**  It is predictable and fits the
   common one-HCA-per-GPU shape; ``nic=N`` asks for more.  The OMPI PR's
   "even share of the complex" rule is not the default.
3. **No NIC under the GPU's complex: assign none, and say so through
   show_help so it aggregates.**  The OMPI PR does not address this case:
   it groups only the complexes that *have* NICs and hands every process
   one of them, never consulting where its GPU is, so a process whose
   GPU's complex carries no NIC is given a NIC from another one.  Naming a
   far NIC in ``UCX_NET_DEVICES`` overrides a library that would otherwise
   have chosen sensibly, so for PRRTE a bad assignment is worse than none.
   The process keeps its GPU assignment either way.

   Clusters are mostly homogeneous, so a node that trips this usually
   means every node does, and a message per node would be thousands of
   lines.  ``devices_begin()`` therefore calls
   ``prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt",
   "rmaps:no-nic-near-gpu", ...)`` once per node that has the problem.
   PMIx suppresses duplicates by the (nspace, help file, topic) tuple, and
   a call on the HNP goes through that path (verified in
   ``src/util/pmix_show_help.c``), so the user sees the first node's
   message in full and then one "N more processes have sent help message
   help-prte-rmaps-base.txt / rmaps:no-nic-near-gpu (job ...)" line.  This
   is the pattern ``rmaps:degenerate-device-locality`` already follows.
   The topic's text names the first node and says the same may hold for
   other nodes, since the summary line counts calls rather than naming
   nodes.
4. **Ethernet is a candidate, ranked last.**  On a RoCE-only node it is the
   right answer; on a node with a management port it is chosen only when
   nothing RDMA-capable shares the complex.

Ranking within a complex
~~~~~~~~~~~~~~~~~~~~~~~~

The candidates for a GPU are ordered by:

1. whether the NIC's PCI function carries an OpenFabrics device (decision
   4);
2. the depth of the deepest common ancestor of the NIC and the GPU in the
   I/O tree, deepest first — a NIC behind the same PCIe switch as the GPU
   ahead of one that only shares the host bridge.  This comes from the
   review of the OMPI PR, which named it as the next step for nodes such
   as ``p5en`` where GPUs and NICs share switches below the root complex;
3. the faster PCIe link.  Added after the user's HPE Cray EX235n topology
   (``test/topologies/slingshot.xml``) showed a 1 GbE management port and
   the Slingshot NIC under the same host bridge as an A100, neither an
   OpenFabrics device and both one root port down: without it, only bus
   order kept the management port off the GPU;
4. how many groups already hold the NIC, fewest first;
5. bus order.

With ``ndev`` greater than one, a group's candidates are the union over its
GPUs' complexes, and depth is measured against the nearest of the group's
GPUs.


Part 3: ``pnet`` support for RoCE, EFA and Slingshot
----------------------------------------------------

Part 2 makes NIC assignments common, and an assignment nothing acts on is
only reportable.  The question is whether the topology carries enough to
name each device to the software that will use it, without guessing.
Verified against libfabric and hwloc master (November 2026):

.. list-table::
   :header-rows: 1
   :widths: 14 22 30 34

   * - Fabric
     - Variable
     - What it accepts
     - What hwloc gives us
   * - RoCE (NVIDIA / Mellanox)
     - ``NCCL_IB_HCA``, ``UCX_NET_DEVICES``
     - The verbs device name, exactly as for InfiniBand (``mlx5_1``,
       ``mlx5_1:*``)
     - An OpenFabrics OS device on a class ``0x200`` function of vendor
       ``0x15b3`` / ``0x10de``.  Same selector as InfiniBand.
   * - RoCE (Broadcom)
     - ``NCCL_IB_HCA``, ``UCX_NET_DEVICES``
     - The verbs device name (``bnxt_re0``, or ``rocep6s0f0`` where
       rdma-core's PCI-path naming is in use); both libraries drive any
       IB/RoCE verbs device, not only NVIDIA's
     - An OpenFabrics OS device on a class ``0x200`` function of vendor
       ``0x14e4``.  ``bnxt_re`` is RoCE only.
   * - RoCE or iWARP (Intel E810, X722)
     - ``NCCL_IB_HCA``, ``UCX_NET_DEVICES``; ``PSM3_NIC`` to confirm
     - The verbs device name (``irdma0``) — but only in RoCE mode; see
       below
     - An OpenFabrics OS device on a class ``0x200`` function of vendor
       ``0x8086``.  Nothing that says which protocol it runs.
   * - Any of the verbs devices above, through libfabric
     - ``FI_VERBS_DEVICE_NAME``
     - A comma-separated list of **prefixes**: each entry is compared with
       ``strncasecmp`` over its own length (``vrb_get_matching_info()`` in
       ``prov/verbs/src/verbs_info.c``)
     - The verbs device names of the node, from which a colliding prefix
       can be detected
   * - EFA (Amazon)
     - ``FI_EFA_IFACE``
     - A comma-separated list of verbs device names, matched exactly
       (``efa_env_allows_nic()`` in ``prov/efa/src/efa_env.h``, compared
       against ``ibv_device->name``)
     - EFA registers under ``/sys/class/infiniband``, so hwloc reports it as
       an OpenFabrics OS device whose name *is* the verbs device name
       (``rdmap16s27``).  Vendor ``0x1d0f``.
   * - Slingshot (HPE)
     - ``FI_CXI_DEVICE_NAME``
     - A comma-separated list of ``cxiN``, validated by the provider
       (``cxip_env_validate_device_name()``)
     - hwloc 2.14.0 and later add a network OS device named ``cxiN`` from
       ``/sys/class/cxi``, beside the ``hsnN`` interface on the same PCI
       function.  Older hwloc has only ``hsnN``.

So yes for all three, with two things to settle in PMIx first.

**A match needs a device type as well as PCI ids.**  ``pmix_pnet_pcimatch_t``
is ``(vendor, devclass)``.  That cannot tell an RDMA function from a plain
Ethernet one of the same vendor and class: an NVIDIA ConnectX port with RDMA
disabled, or Amazon's ENA beside its EFA (both vendor ``0x1d0f``).  Writing
an interface name like ``eth0`` into ``NCCL_IB_HCA`` or ``FI_EFA_IFACE`` is
the "another fabric's device in your variable" case ``pnet`` exists to
prevent.  Add a ``pmix_device_type_t types`` field (zero meaning any) that
the device's type must intersect; ``nvd``'s RoCE entries and ``efa`` require
``PMIX_DEVTYPE_OPENFABRICS``.  With the type doing that work, ``efa`` can
leave ``devclass`` at zero and need not depend on EFA's PCI class.  This
changes a struct the components are built against, so ``pnet``'s interface
version bumps.

**A Slingshot function has two network OS devices, and the enumerator picks
one by iteration order.**  ``osdev_preferred()`` already takes the
OpenFabrics device over the interface because ``ib0`` is accepted by no
fabric variable.  ``hsn0`` versus ``cxi0`` is the same problem, and the
provider *rejects* anything not of the form ``cxiN``, so a wrong choice is
not a silent narrowing but a provider that fails to initialize.  Prefer the
``cxi`` device.  It is identified by what hwloc records only for it — the
``CassiniVersion`` / ``NID`` infos from ``/sys/class/cxi`` — rather than by
its name.  Under hwloc older than 2.14 there is no ``cxi`` device, the
selector is ``hsnN``, and ``pnet/cxi`` must set nothing rather than derive
``cxi0`` from ``hsn0``: the two usually correspond, but nothing guarantees
it, and the plan's rule against guessing an ordinal applies.

**Intel's ``irdma`` may be iWARP, and nothing in the topology says so.**
An E810 runs RoCEv2 or iWARP depending on how the driver was loaded, and an
X722 runs iWARP only.  Neither UCX nor NCCL's IB transport drives iWARP.
NCCL then falls back to sockets, but a ``UCX_NET_DEVICES`` naming only an
iWARP device leaves UCX no device for any transport, and it fails rather
than falls back.  The kernel says which it is in
``/sys/class/infiniband/<dev>/node_type`` (``1: CA`` versus ``4: RNIC``).
hwloc records the GUIDs, port states, LIDs and GIDs, but not that file, so
the topology the HNP holds cannot tell them apart.

``setup_fork`` runs on the node that forks the process, so the component
can read that one file for each device it is about to name, on the machine
it describes.  An ``RNIC`` is left out of ``UCX_NET_DEVICES`` and
``NCCL_IB_HCA``.  A file that cannot be read means the device is left out
too: on Linux the file always exists, and elsewhere none of these devices
does.  The long-term fix is for hwloc to record the node type as an info
attribute, which would let the HNP know it as well.  Propose that upstream;
do not wait for it.

**``FI_VERBS_DEVICE_NAME`` cannot be written exactly.**  The provider
treats each entry as a prefix, so ``mlx5_1`` also selects ``mlx5_10``
through ``mlx5_19`` on a node with that many devices.  No spelling of the
entry anchors it.  The component therefore writes it only when no other
OpenFabrics device on the local node has an assigned name as a proper
prefix, and otherwise leaves it unset and says why at verbosity.  On the
common node, with fewer than ten devices of one driver, it is always
written.

**One component owns each variable.**  Two components writing
``UCX_NET_DEVICES`` for one process would overwrite each other, and
whichever ran last would win.  So the IB/RoCE vendors are entries in one
component's table, not one component each.  That component is ``nvd``
today.  Its name says NVIDIA, which stops being true; renaming it is
cosmetic and can be done separately.

The components
~~~~~~~~~~~~~~

* ``nvd`` (the IB/RoCE verbs component): add
  ``{0x15b3, 0x200, OPENFABRICS}``, ``{0x10de, 0x200, OPENFABRICS}``,
  ``{0x14e4, 0x200, OPENFABRICS}`` and ``{0x8086, 0x200, OPENFABRICS}`` to
  ``mymatch``.  The class keeps Omni-Path's ``0x208`` with ``opa``.
  ``setup_fork`` drops any device whose ``node_type`` is not ``CA``, then
  writes ``NCCL_IB_HCA`` and ``UCX_NET_DEVICES`` as now, and
  ``FI_VERBS_DEVICE_NAME`` under the prefix rule.
* ``efa`` (new): match ``{0x1d0f, 0, OPENFABRICS}``; write the joined
  selectors to ``FI_EFA_IFACE``.  ``component_open`` gates on the vendor,
  as ``nvd`` and ``opa`` do.
* ``cxi`` (new): match the HPE/Cray vendor id with ``NETWORK``; write
  ``FI_CXI_DEVICE_NAME`` only when every selector is of the form ``cxiN``,
  and say once at verbosity why it declined otherwise.

Like ``nvd``, the components overwrite a variable that is already set.  A
process mapped against a device made the more specific request.

Not needed: the variables take device *names*, which the topology has.  No
ordinals are involved, so nothing here repeats the ``HFI_UNIT`` problem.

Still to confirm before landing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* **Test material.**  Neither this tree nor hwloc's test suite carries a
  topology with an EFA or a Slingshot device.  The unit tests can use
  hand-edited copies of ``turin-4gpu.xml`` (vendor ids and OS devices
  changed), but those prove only that PMIx reads what it was written to
  read.  A real ``lstopo --of xml`` from a ``p5`` / ``p5en`` instance and
  from a Slingshot node should be obtained and checked in before the
  components are called done.
* **Slingshot's PCI vendor id.**  The Cassini parts are expected to report
  Cray's ``0x17db``.  Confirm it from a real topology.
* **PSM3 on E810.**  PSM3 is Intel's provider for E810 RoCE as well as for
  Omni-Path, and ``opa`` already writes ``PSM3_NIC`` for class ``0x208``.
  Whether ``PSM3_NIC`` takes the ``irdma`` device name on an E810, and
  what PSM3 does with one in iWARP mode, needs confirming from PSM3's
  source before an ``{0x8086, 0x200}`` entry joins ``opa``'s table.
* **Topologies for Broadcom and Intel adapters.**  Same need as EFA and
  Slingshot.  An Intel one should say which mode its ``irdma`` was in.
* **Scrubbing.**  A topology XML carries the host name, MAC addresses,
  GUIDs and, for Slingshot, the NID.  Replace them before anything is
  checked in.

Part 3 as landed
~~~~~~~~~~~~~~~~

The user's HPE Cray EX235n topology (``test/topologies/slingshot.xml``,
read by hwloc 2.10) settled several of the items above and changed three
decisions.

* **Slingshot's vendor id is confirmed**: Cassini reports ``[17db:0501]``,
  class ``0x0200``.
* **pnet/cxi reads sysfs when the topology has no cxi device.**  hwloc 2.10
  records only ``hsnN``; the node itself names the matching device under
  ``/sys/class/net/<hsnN>/device/cxi/`` - which is where hwloc 2.14 finds it
  too.  Exactly one ``cxiN`` entry there is used; none, several or an
  unreadable directory declines.  The name is never derived from the
  ``hsn`` number, so the rule against guessing still holds.  The root is the
  ``pnet_cxi_sysfs_root`` parameter.
* **The node-type check is Intel's only.**  Only ``irdma`` can be iWARP, so
  only Intel's adapters are asked.  Asking NVIDIA's and Broadcom's made their
  naming depend on a file that changes nothing about them, and dropped them
  wherever it was absent - a topology loaded on a machine without the
  hardware, which is how this path is tested.
* **A 3D controller is a GPU without OS devices.**  The node's A100s (class
  ``0x0302``) carry none: that hwloc had no vendor backend and the node no
  render nodes, so ``--map-by device=gpu`` said there was no GPU.  PMIx now
  reports such a function as a GPU named by its PCI bus id, with no vendor
  identity, so PRRTE refuses it as unnameable instead - which names the fix.

A Broadcom node (``test/topologies/mi350-bcm57608.xml``, read by hwloc
2.15) confirmed the RoCE rule on real hardware and found one defect
elsewhere.

* **Broadcom RoCE is named as planned.**  Each BCM57608 port
  (``[14e4:1760]``, class ``0x0200``) carries an OpenFabrics device, and a
  process mapped against it gets ``NCCL_IB_HCA``, ``UCX_NET_DEVICES`` and
  ``FI_VERBS_DEVICE_NAME``.  The names are rdma-core's PCI-path names
  (``rocep6s0f0``), not ``bnxt_reN``; nothing here depends on the name,
  and those names cannot prefix one another, so ``FI_VERBS_DEVICE_NAME`` is
  always written.  A BCM57508 on the same node with no OpenFabrics device
  is plain Ethernet and is named to nothing.
* **The grouping and pairing hold.**  Each host bridge holds one MI350
  and one dual-port adapter: ``device=gpu:nic`` gives each GPU port 0,
  ``nic=2`` both ports, and ``device=nic:ndev=2`` keeps each card's ports
  together.
* **PMIx's ``pgpu/amd`` did not open on this node.**  It asked only for an
  AMD function in the display class ``0x03``, and an MI350 reports
  ``0x1200``, a processing accelerator, so no process was given
  ``ROCR_VISIBLE_DEVICES``.  PRRTE's map was unaffected: the device
  enumeration counts the GPUs by their RSMI devices.  The fix is in PMIx
  (openpmix/openpmix#4442).
  This is a separate PMIx change, and PRRTE's offline harness mirrors it in a
  PRRTE change that lands after it.  Class ``0x0380`` is left alone; those
  parts get render nodes from their drivers.
* **opa's ``PSM3_NIC`` named several HFIs with commas**, which PSM3 matches
  as one ``fnmatch`` pattern and so matched nothing.  It now writes a single
  plain name or nothing.  An E810 entry was not added to ``opa``: whether
  PSM3's verbs layer drives an iWARP ``irdma`` is unresolved.

Separately, and not part of this work: hwloc master (3.0, unreleased) turns
the OS device ``type`` attribute into a ``types`` bitmask.  PMIx's
``pmix_hwloc.c`` reads ``attr->osdev.type`` throughout and has no
``HWLOC_API_VERSION`` guard for it, so it will need adapting before it can
build against hwloc 3.0.


Build order
-----------

Part 1 first and alone: it is a correction, it touches only
``rmaps_base_devices.c`` and a new ``src/hwloc`` helper, and its proof is
that every existing golden stays byte-identical while the new ones pin the
change.  Part 2 builds on the helper Part 1 adds.  Parts 1 and 2 are one
PRRTE PR each, and neither needs PMIx.

Part 3 is an openpmix PR and does not depend on the other two: it acts on
any NIC assignment, including today's ``--map-by device=nic``.  It can land
in parallel, and should land before Part 2 is announced, so that the first
users of ``:nic`` on EFA or Slingshot get a variable and not just a key.

Verification for both follows :doc:`impl-plan`'s checklist:
warning-free ``--enable-debug`` build from the top of an out-of-tree build,
``make check``, ``make -C test/offline check-offline``, a live
``prte``/``prun``/``pterm`` smoke test with one failure case leaving the DVM
up, and a ``-W`` docs build.  Part 2 adds the end-to-end check that a
launched process on ``turin-4gpu-nvml`` sees ``CUDA_VISIBLE_DEVICES`` and
``NCCL_IB_HCA`` naming the GPU and the HCA under one host bridge.


Task checklist
--------------

**Part 1**

- [x] ``prte_hwloc_base_host_bridge()`` in ``src/hwloc``
- [x] ``group_by_complex()`` in ``rmaps_base_devices.c``, skipped under
      ``interleave`` and for ``ndev`` of one
- [x] ``test_devices.c`` cases on ``turin-4gpu.xml`` and ``test-topo.xml``
- [x] Offline golden for ``device=nic:ndev=2``; refusal case for
      ``--bind-to package``; existing goldens unchanged
- [x] ``cli-map-by.rst`` ``NDEV`` text; ``rmaps/base/AGENTS.md``

**Part 2**

- [x] Qualifier vocabulary, both parsers, job/app attributes, options field
      (appended, so no rmaps version bump)
- [x] Second enumeration and per-complex dealing in ``devices_begin()``;
      NICs appended in ``devices_record()``
- [x] Candidate ranking: OpenFabrics first, deepest common ancestor, bus
      order
- [x] Refusal for a non-GPU class; the ``rmaps:no-nic-near-gpu`` topic,
      shown per node under the job's nspace so show_help aggregates it
- [x] Unit, offline and end-to-end envar tests
- [x] ``cli-map-by.rst``, ``help-mapby.txt``, placement examples

**Part 3** (openpmix)

- [x] ``types`` field in ``pmix_pnet_pcimatch_t``, honored by
      ``pmix_pnet_base_get_assigned_devices()``; pnet version bump
- [x] ``osdev_preferred()``: the ``cxi`` OS device over ``hsnN``
- [x] ``nvd``: NVIDIA, Broadcom and Intel RoCE entries; ``node_type``
      check; ``FI_VERBS_DEVICE_NAME`` with the prefix-collision rule
- [x] ``pnet/efa``: ``FI_EFA_IFACE``
- [x] ``pnet/cxi``: ``FI_CXI_DEVICE_NAME``, declining non-``cxiN`` selectors
- [x] Unit tests on hand-edited topologies; the real Slingshot topology
      scrubbed and checked in to both trees
- [x] ``pnet/cxi``: the cxi device found in sysfs under hwloc < 2.14
- [x] ``nvd``: the node-type check limited to Intel's adapters
- [x] PMIx: a class ``0x0302`` function is a GPU without OS devices; PRRTE's
      harness mirrors it once that lands
- [x] A real Broadcom topology (``mi350-bcm57608.xml``)
- [ ] Real EFA, Intel and AMD Pensando topologies (and a Slingshot one from
      hwloc 2.14 or later)
- [ ] Propose a node-type info attribute to hwloc
- [x] ``pnet`` AGENTS.md
- [x] the PRRTE ``cli-map-by.rst`` list of variables set, with the harness
      change once the PMIx side has landed
