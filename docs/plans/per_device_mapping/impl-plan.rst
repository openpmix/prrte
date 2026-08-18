Device mapping: implementation plan
===================================

This is the build order for the design in
:doc:`per-device-mapping`.  That document says *what* ``--map-by
device=<class>`` means and why; this one says what to change, in what order,
and how each step is proved before the next one starts.  Where the two
disagree the design document wins — except for the two places noted below as
**refinements**, which correct it.

Read it alongside `src/mca/rmaps/AGENTS.md
<https://github.com/openpmix/prrte/blob/master/src/mca/rmaps/AGENTS.md>`_ and
`src/hwloc/AGENTS.md
<https://github.com/openpmix/prrte/blob/master/src/hwloc/AGENTS.md>`_; the
rules in those files are assumed here rather than repeated.


Two refinements to the design
-----------------------------

Both came out of reading the topologies already in the tree.

**1. The GPU filter is a union, not a priority ladder.**  The design says:
prefer backend-tagged OS devices (``cuda0``, ``rsmi0``, ``ze0``,
``HWLOC_OBJ_OSDEV_COPROC``) and *fall back* to the DRM render-node rule if
the topology has none.  Topology-wide fallback is wrong on a mixed-vendor
node: one CUDA GPU with its backend loaded and one AMD GPU without would make
rule 1 fire and the AMD card disappear.  Apply both rules **per PCI function**
and take the union — a function is a GPU if it carries a backend/COPROC OS
device **or** it is PCI class ``03xx`` carrying a ``renderD*`` OS device.
The two rules agree on every topology in hand, so this costs nothing now and
removes a whole class of future bug report.

**2. prte_rmaps_options_t changes, so the rmaps version bumps.**  The
design does not mention it.  Precedent is
``docs/plans/per_app_mapping/rmaps-impl-plan.rst``, which bumped 4.0.0 → 5.0.0
for exactly this reason (it added ``app_idx`` and ``dist_device`` to that
struct).  Add ``PRTE_RMAPS_BASE_VERSION_6_0_0`` and redefine the ``5_0_0``
alias to it, following the pattern already in ``rmaps_types.h``.


Current state
-------------

Facts the plan depends on, all verified against the tree at
``topic/cluster-scaling-sweep``:

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - Item
     - Current value
   * - Last used ``PRTE_PROC_*`` attribute offset
     - ``PRTE_PROC_NBEATS`` = ``+14``; next free is **+15**
       (``+2``/``+3``/``+4`` are holes — do not reuse)
   * - ``PRTE_JOB_DIST_DEVICE`` / ``PRTE_APP_DIST_DEVICE``
     - job key ``+78`` / app key ``30``; both dead, rename in place
   * - rmaps framework version
     - ``PRTE_RMAPS_BASE_VERSION_5_0_0`` (``4_0_0`` aliased to it)
   * - ``prte_rmaps_base.device``
     - declared, initialized to ``NULL``, never read or written
   * - ``mappers[]`` / ``mapquals[]``
     - ``schizo_base_frame.c`` ~line 725 / ~line 740
   * - Job-level ``--map-by`` parser
     - ``prte_rmaps_base_set_mapping_policy()``, ``rmaps_base_frame.c`` ~800
   * - Per-app ``--map-by`` parser
     - ``prte_rmaps_base_set_app_mapping_policy()``, same file ~1135
   * - Qualifier chain order
     - SPAN, OVERSUB, NOOVER, NOLOCAL, ORDERED, PE, **INHERIT**, NOINHERIT,
       HWTCPUS, CORECPUS, QFILE
   * - ``test/topologies/``
     - ``test-topo.xml``, ``test-topo2.xml``; ``EXTRA_DIST`` lists them by
       name; ``test/offline`` auto-discovers every ``*.xml`` there
   * - PMIx floor
     - ``pmix_min_version = 7.0.0`` in ``VERSION``

**The test material already exists.**  This is the single biggest
risk-reducer in the plan and it was not obvious: both shipped topologies
already carry I/O devices, and ``test-topo2.xml`` carries precisely the
shapes this feature has to get right.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Topology
     - What it pins
   * - ``test-topo2.xml``
     - One AMD GPU at ``0000:63:00.0`` (class ``0380``) exposing **three**
       OS devices — ``renderD128``, ``card0`` **and** ``rsmi0`` — so it
       tests dedup-by-PCI-function *and* the backend-tagged path in one
       object.  Plus ``card1`` at ``0000:02:00.0``: class ``0300``, no
       render node, no backend — **the decoy, already in the tree**.
       ``device=gpu`` on this topology must return **exactly one** device.
       It also has one OFA device (``mlx5_0``) and one network device
       (``enp67s0``) on *different* NUMA groups.
   * - ``test-topo.xml``
     - Every device sits under the **same** ``Group`` (cpus 0-43) of a
       four-group machine — the degenerate-locality case, free.  Four of its
       OS devices (``eth0``, ``sda``, ``sdb``, ``sr0``) have **no PCI
       ancestor**, which is the "sort last, by osname" rule.

So the enumerator can be written and fully unit-tested before the
requester's topology is added to the tree at all, and before any PRRTE
plumbing exists.


Dependency graph
----------------

.. code-block:: text

   PR A  remove dead dist remnants        (prrte)   -- independent
   PR B  parameterize byobj               (prrte)   -- independent, no behavior change
   PR C  device enumerator + cap flag     (openpmix)
                    |
                    v
   PR D  wrapper + directive + mapper     (prrte)   needs C, B
                    |
                    v
   PR E  interleave qualifier             (prrte)   needs D
                    |
                    v
   PR F  report the assignment            (prrte)   needs D
                    |
                    v
   PR G  requester's topology + swarm     (prrte)   needs D, E

A and B can start immediately and in parallel with C.  D is the only place
the feature becomes visible to a user, and it deliberately lands the
directive **and** the mapper together — a directive that parses and does
nothing is the exact failure this whole feature is repairing, and shipping
one for even a single release would repeat it.


Phase A — remove the dead ``dist`` remnants
--------------------------------------------

Standalone PR, no dependencies, reviewable on its own.  The project asks for
incidental cleanup as its own commit; this one also shrinks the diff of
every phase after it.

Delete, in one commit:

* ``PRTE_CLI_DIST`` from ``src/util/prte_cmd_line.h``
* ``prte_rmaps_base.device`` (field, initializer in ``rmaps_base_frame.c``,
  and the ``device`` entry in ``base.h``'s struct)
* ``case PRTE_MAPPING_BYDIST`` from the policy switch in
  ``rmaps_base_map_job.c``
* the ``"MINDIST"`` arm in ``rmaps_base_print_fns.c``
* ``dist`` and ``DEVICE=dev (for dist policy)`` from the ``mapby`` MCA
  parameter help string in ``rmaps_base_frame.c``

Keep — they are reused in phase D, renamed rather than deleted:
``PRTE_MAPPING_BYDIST`` (the value ``10``),
``PRTE_JOB_DIST_DEVICE`` / ``PRTE_APP_DIST_DEVICE``, and
``options->dist_device`` with its ``resolve_app_options()`` plumbing and its
``free()`` at ``cleanup``.

**Commit message** must say why: the MCA parameter has been advertising a
policy no command line can reach, which is what sent the reporter of
OMPI #14169 looking for it.  That is the bug being fixed here, independent
of whether device mapping ever lands.

*Verify:* top-level ``make`` warning-free with ``--enable-debug``;
``make check``; ``prte_info --all | grep -i dist`` no longer offers it.


Phase B — parameterize ``byobj`` over its object enumerator
------------------------------------------------------------

Standalone PR.  Pure refactor, **no behavior change**, and that is the
point: it is provable by the existing offline harness, so the risky
restructuring lands separately from the new feature and cannot be confused
with it.

In ``src/mca/rmaps/round_robin/rmaps_rr_mappers.c``, replace the two direct
calls in ``prte_rmaps_rr_byobj()``:

.. code-block:: c

   /* today */
   nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                              options->maptype);
   obj   = prte_hwloc_base_get_obj_by_type(node->topology->topo,
                                           options->maptype, j);

with a small vtable supplied by the caller:

.. code-block:: c

   typedef struct {
       /* how many placement targets does this node offer? */
       unsigned    (*count)(prte_node_t *node, prte_rmaps_options_t *opts,
                            void *ctx);
       /* the j-th target, or NULL */
       hwloc_obj_t (*item)(prte_node_t *node, prte_rmaps_options_t *opts,
                           void *ctx, unsigned j);
       /* per-node setup/teardown; NULL when not needed */
       int         (*begin)(prte_node_t *node, prte_rmaps_options_t *opts,
                            void **ctx);
       void        (*end)(void *ctx);
       const char *name;      /* for diagnostics: "core", "device=gpu", ... */
   } prte_rmaps_target_enum_t;

``prte_rmaps_rr_byobj()`` becomes a thin wrapper that calls the shared body
with the hwloc-type enumerator.  Everything else in that function is
untouched — in particular the ``redo:`` loop, the ``nodefull`` guard around
``check_avail`` (whose absence segfaulted the HNP on a ``max_slots``
hostfile), and the oversubscribe second pass.  Do **not** take the
opportunity to tidy those.

The ``name`` field replaces ``hwloc_obj_type_string(options->maptype)`` in
the two verbose messages and in the ``rmaps:mapping-target-not-found``
show_help call, so the device case gets a message that names the device spec
rather than an hwloc type.

*Verify:* ``make check``; then the decisive one —
``make -C test/offline check-offline`` must produce **byte-identical** golden
maps to the pre-refactor run across all ~1200 cases.  Capture the goldens
before the change and diff.  Anything that differs is a bug in the refactor,
not a golden to update.


Phase C — the PMIx device enumerator
-------------------------------------

openpmix PR.  Gates phase D.

New in ``src/hwloc/pmix_hwloc.[ch]``:

.. code-block:: c

   /* devs is allocated here and freed by PMIx_Device_free();
    * ordering is PCI bus id ascending, no-PCI devices last by osname */
   PMIX_EXPORT pmix_status_t
   pmix_hwloc_get_devices(pmix_topology_t *topo,
                          pmix_device_type_t type,
                          const char *devid,      /* NULL = all of type */
                          pmix_device_t **devs,
                          size_t *ndevs,
                          pmix_cpuset_t **localities);  /* parallel array */

Behavior:

* **Dedup by parent PCIDev.**  Today PMIx emits one entry per OS device
  in hwloc traversal order.  ``test-topo2.xml``'s single GPU with three OS
  devices is the test.
* **Order by PCI bus id** (domain, bus, device, function).
* **GPU selection by the union rule** (refinement 1 above), replacing the
  current ``strncasecmp(device->name, "card", 4)`` skip.  That skip drops the
  auxiliary DRM card node, which happens to give the right answer on the
  topologies at hand and gives *no GPU at all* on a machine whose GPU has a
  ``card*`` node and no render node.
* **Stop unconditionally skipping** ``HWLOC_OBJ_OSDEV_COPROC`` — that is
  where ``cuda0`` and ``ze0`` live.
* **Locality** = nearest ancestor with a non-NULL cpuset
  (``hwloc_get_non_io_ancestor_obj()``).  Return the ``Machine`` root rather
  than failing when that is all there is; the caller decides what a
  node-wide locality means.
* **UUID grammar unchanged** — ``gpu://<host>::<osname>``,
  ``fab://<NodeGUID>::<SysImageGUID>``, ``ipv4://<mac>``.  This is the whole
  reason the enumerator is here: the name PRRTE reports as a process's
  assigned device and the name PMIx reports through
  ``PMIX_DEVICE_DISTANCES`` must be the same string.

Add ``PMIX_CAP_DEVICE_ENUM`` (or whatever the maintainer prefers) to
``pmix_version.h.in``.

*Verify:* PMIx's own unit tests against both PRRTE topologies (copy them, or
point at the PRRTE tree); ``make check`` in PMIx.


Phase D — wrapper, directive, mapper, bind ceiling
---------------------------------------------------

The main PR.  Needs C and B.  Four commits, landing together.

**D1 — configure gate.**  In ``config/prte_setup_pmix.m4``, add a
``PRTE_CHECK_PMIX_CAP([DEVICE_ENUM], …)`` block defining
``PRTE_PMIX_DEVICE_ENUM`` to 0/1, following the ``IOF_FILE_PATTERN``
pattern.  Bump ``pmix_min_version`` only if the maintainer prefers a hard
floor to a capability gate.

.. warning::

   This edits an ``m4`` file, so ``make`` alone will not pick it up and may
   wedge the tree in a half-regenerated state.  Run ``./autogen.pl``, then
   re-run ``configure`` from the build directory with the same options
   (``./config.status --config`` recovers them).  There is no shortcut.

**D2 — the PRRTE wrapper**, in ``src/hwloc/hwloc-internal.h`` and
``hwloc_base_util.c``:

.. code-block:: c

   typedef struct {
       pmix_object_t       super;
       hwloc_obj_t         locality;  /* borrowed - do not release */
       char               *osname;
       char               *uuid;
       char               *busid;
       pmix_device_type_t  type;
   } prte_hwloc_device_t;

   PRTE_EXPORT int prte_hwloc_base_get_devices(hwloc_topology_t topo,
                                               pmix_device_type_t type,
                                               const char *name,
                                               pmix_list_t *devs);
   PRTE_EXPORT pmix_device_type_t prte_hwloc_base_device_type(const char *spec);

Rules this file already enforces and that apply here: take the topology as
an argument and never consult ``prte_hwloc_topology``; an empty result is
not an error; ``PMIX_NEW`` does not zero, so the constructor sets every
field the destructor frees.

**D3 — grammar and plumbing:**

* ``src/util/prte_cmd_line.h``: ``PRTE_CLI_DEVICE`` = ``"device="``.
* ``schizo_base_frame.c``: add ``PRTE_CLI_DEVICE`` to ``mappers[]``.
* ``rmaps_types.h``: rename ``PRTE_MAPPING_BYDIST`` →
  ``PRTE_MAPPING_BYDEVICE`` (value ``10`` unchanged, which keeps it
  ``<= PRTE_MAPPING_RR`` so round_robin still claims it); rename
  ``options->dist_device`` → ``options->map_device``; add
  ``PRTE_RMAPS_BASE_VERSION_6_0_0`` and alias ``5_0_0`` to it.
* ``attr.[ch]``: rename the two ``DIST_DEVICE`` keys to ``MAP_DEVICE``,
  values unchanged, **and their strings in** ``prte_attr_key_to_str()`` — a
  key with no name renders as ``UNKNOWN-KEY: <n>`` in every attribute
  diagnostic, and ``test/unit/util`` requires the names to be unique.
* ``rmaps_base_frame.c``: both parsers.  Read the value with
  ``qualifier_value()``.  **Refuse a bare** ``--map-by gpu`` — no synonym.
  Update the ``mapby`` MCA help string.
* ``rmaps_base_map_job.c``: ``case PRTE_MAPPING_BYDEVICE`` leaves
  ``mapdepth = PRTE_BIND_TO_NONE`` and ``maptype = HWLOC_OBJ_MACHINE``, so
  the job-level bind-upwards check does not fire on a basis not yet known.

**D4 — the mapper**, in ``rmaps_rr_mappers.c``: the device enumerator
instance for phase B's vtable, plus the per-node bind ceiling.  Its
``begin()`` builds the node's device list, ``end()`` frees it — the list is
per node, and must **not** become file-static: ``rank_file`` and ``lsf``
both carry static state that outlived a job and handed a stale value to the
next one.

The ceiling check runs in ``begin()``, once the list exists and before any
proc is placed:

.. code-block:: c

   /* binding coarser than the device's locality cannot be honored: the
    * request is "near this device", and an object that strictly contains
    * the locality is not near it. Compare cpusets, not PRTE_BIND_TO_*
    * levels - the locality is frequently an hwloc Group, which has no
    * position in that ladder at all. */
   if (PRTE_BIND_TO_NONE != options->bind) {
       /* first candidate object of the binding type that covers the
        * locality; if its cpuset strictly contains the locality's, refuse */
   }

Failure paths use ``prte_show_help`` + ``PRTE_ERR_SILENT`` and must reach
``jdata->exit_code``; a node offering zero devices of the requested class is
a hard error via ``rmaps:mapping-target-not-found`` naming the spec, exactly
as a missing hwloc object type already is.

New help topics in ``help-prte-rmaps-base.txt``:
``rmaps:no-such-device-class``, ``rmaps:bind-above-device``,
``rmaps:degenerate-device-locality`` (a warning, not an error — see the
design document).

.. warning::

   After touching any ``help-*.txt``::

      rm src/util/prte_show_help_content.*
      make

   An ordinary ``make`` does **not** regenerate it, and the binary keeps
   serving the old (or missing) message while the ``.txt`` looks right.

**Tests in this PR:**

* ``test/unit/hwloc/test_hwloc.c`` — the enumerator against both existing
  topologies: ``device=gpu`` on ``test-topo2`` returns exactly one device
  (not two, not three); its osname/uuid/busid; ``device=openfabrics``
  returns one and ``device=network`` one, on different groups;
  ``device=mlx5_0`` by name returns one; a name that does not exist returns
  none; on ``test-topo``, every device shares a locality (the degenerate
  case) and the four PCI-less OS devices sort last.
* ``test/unit/rmaps/test_policy_parse.c`` + ``test_job_policy.c`` —
  ``device=gpu`` parses identically at job and app level; ``dev=gpu`` works;
  the value is read after the ``=`` and not at a fixed offset; a bare
  ``--map-by gpu`` is **refused**.
* ``test/unit/rmaps/test_dispatch.c`` — round_robin claims
  ``PRTE_MAPPING_BYDEVICE``; the specialized mappers defer.

*Verify:* warning-free debug build; ``make check``;
``make -C test/offline check-offline`` still byte-identical for every
pre-existing case; live smoke test (``prte --daemonize`` →
``prun -n 2 --map-by device=network --bind-to core hostname`` → ``pterm``).


Phase D2 — the user documentation
----------------------------------

Lands **with** phase D, not after it.  ``--map-by device=`` is a user-visible
option, and the project requires documentation with a user-visible change;
a release in which the directive works but nothing documents it is the same
shape of defect this whole plan started from — the ``dist`` policy that the
MCA parameter advertised and no command line could reach.

Five files, and they are not interchangeable.  Each is read by someone in a
different situation, so the same fact has to be stated five times at five
levels of detail rather than written once and cross-referenced.

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - File
     - What it needs
   * - ``src/mca/rmaps/base/help-mapby.txt``
     - The ``[map-by]`` topic — what ``prterun --help map-by`` prints.  Add
       ``device=<class|name>`` to the directive list, with the classes and
       the "or a device name" form.  This is the one the reporter of
       #14169 checked and found silent about ``dist``.
   * - ``src/mca/rmaps/base/help-placement.txt``
     - Two topics, and they are separate audiences.  ``[placement-examples]``
       carries both the option list its examples draw on and the examples
       themselves; ``[placement-fundamentals]`` carries the model, and is
       where a device's *locality* has to be explained, since that concept
       exists nowhere else in placement.  ``[placement]`` needs nothing: it
       is conceptual (mapping vs ranking vs binding, slots, PEs) and
       enumerates no directives — easy to assume otherwise, since its prose
       resembles the examples topic's option list.
   * - ``src/docs/prrte-rst-content/cli-map-by.rst``
     - The rendered ``--map-by`` reference, whose bullet list of directives
       (``SLOT``, ``HWTHREAD``, … ``PE-LIST=a,b``) is the canonical
       enumeration.  Add ``DEVICE=<class|name>``, and note in the qualifier
       list that a binding coarser than the device's locality is refused.
   * - ``src/docs/prrte-rst-content/detail-placement-fundamentals.rst``
     - Where the *model* is explained.  Mapping by a device is the first
       policy whose target is not an hwloc object the user can name, so say
       what it maps to: the nearest ancestor of the device that has a
       cpuset, and why binding descends from there.
   * - ``src/docs/prrte-rst-content/detail-placement-examples.rst``
     - Worked examples with output.  The reporter's machine is the example
       worth using — GPUs on NUMA 1 and 2 of one socket and 6 and 7 of the
       other is exactly the shape no other directive can express, and it
       shows why the feature exists rather than just how to spell it.

Three things the text must say, because each is a decision a reader would
otherwise take for a bug:

* there is no bare ``--map-by gpu``; the class is the directive's value;
* a binding coarser than the device's locality is **refused**, not silently
  widened;
* on a machine where every device is equally close to every cpu the job
  still runs, with a warning, and each process still gets its own device.

Remember the ``show_help`` golden rule for the two ``.txt`` files: the
generated content is not rebuilt by an ordinary ``make``.  In a VPATH build
the generated file lives in the **build** tree, so it is
``rm <builddir>/src/util/prte_show_help_content.*`` that matters — deleting
the source tree's copy does nothing, because there isn't one.

Then ``make`` in ``docs/`` (Sphinx runs with ``-W``) and the help/option
cross-check::

   python3 src/util/prte-convert-help.py --root . --check-only \
       --cppflags="$(pkg-config --cflags-only-I pmix)"


Phase E — the ``interleave`` qualifier
---------------------------------------

Needs D.

* ``prte_cmd_line.h``: ``PRTE_CLI_INTERLEAVE`` = ``"interleave"``.
* ``schizo_base_frame.c``: add to ``mapquals[]``.
* ``rmaps_base_frame.c``: an ``interleave`` arm **appended at the end of
  both qualifier chains**, after ``INHERIT``.

.. warning::

   ``pmix_check_cli_option()`` is ``strncasecmp`` over
   ``min(strlen(given), strlen(defined))`` and has **no view of the other
   options** — it answers one comparison at a time, and the first arm of the
   caller's ``if``/``else if`` chain that prefix-matches wins.  ``interleave``
   and ``inherit`` share a first letter, and ``:i`` means ``INHERIT``
   today.  Placing the new arm earlier silently changes the meaning of a
   working command line: no error, no warning, a different mapping.  Append
   it, and add the regression test below.

* The reordering itself lives beside the enumerator, not in the mapper:
  group the device list by the ``<level>`` object containing each device's
  locality, then take one device per group in turn, dropping a group when it
  is exhausted.  Default ``level=package``.  Refuse ``node`` as a level —
  that is what ``SPAN`` expresses — via ``invalid-value``.  Refuse
  ``interleave`` on a non-``device=`` map by name, as the per-app
  ``--bind-to`` parser already refuses ``report``.

**Tests:** in ``test/unit/hwloc`` (the reordering is pure):
``interleave=package`` on the requester's topology yields GPU0, GPU2, GPU1,
GPU3; ``interleave=numa`` there yields the unchanged PCI-bus order (graceful
degradation); an uneven split drops the exhausted group.  In
``test/unit/rmaps``: the default is ``package``; ``=numa`` is honored;
``node`` is refused; a non-device map is refused with its own message; and
**:i and :in still resolve to INHERIT**.  That last assertion is
the entire guard against the ordering hazard and is invisible to every other
kind of test.

Note the uneven-split and requester's-topology cases need phase G's XML, so
either land E after G or embed a cut-down topology in the test file.  hwloc's
synthetic generator cannot produce I/O devices at all, so there is no third
option — say so in the test file's header, since every other topology there
is synthetic.


Phase F — report the assignment
--------------------------------

Needs D.  Small, and separable because nothing in the placement depends on
it.

* ``attr.h``: ``PRTE_PROC_DEVICE_ID = PRTE_PROC_START_KEY + 15`` (next free;
  ``+2``/``+3``/``+4`` are holes and stay holes), holding the assigned
  device's UUID.  Add its name to ``prte_attr_key_to_str()``.
* ``rmaps_rr_mappers.c``: set it on the proc as it is placed.  It must be
  ``PRTE_ATTR_GLOBAL`` — a spawn request is always packed, so a local
  attribute never reaches the daemon that forks the process.
* ``pmix_server_register_fns.c``: publish it per proc as ``PMIX_DEVICE_ID``.
  The process reads it with ``PMIx_Get(&myproc, PMIX_DEVICE_ID, …)`` and can
  correlate it against ``PMIX_DEVICE_DISTANCES`` for the full distance
  vector.
* ``prte_dt_print_fns.c``: add it to the ``--display map`` proc line beside
  ``Bound:``.  A placement the user cannot see is a placement they will not
  trust, and this is the first thing they will check.

A UUID, never an ordinal: CUDA's default ``CUDA_DEVICE_ORDER`` is
``FASTEST_FIRST``, not ``PCI_BUS_ID``, so an index PRRTE hands out is not
the index the runtime will use.

*Verify:* ``make check``; live smoke test reading the key back from a
process; ``--display map`` shows the device.


Phase G — the requester's topology, the harness, the swarm
-----------------------------------------------------------

Needs D and E.  This is the confirmation phase: nothing new is designed
here, the requested table is simply run.

* Add the issue's ``hwloc_topology.xml`` to ``test/topologies/`` as
  ``turin-4gpu.xml``, and to ``EXTRA_DIST`` in that directory's
  ``Makefile.am``.

  .. note::

     ``test/offline`` auto-discovers **every** ``*.xml`` in that directory,
     so adding a 256-PU topology multiplies the harness by another full
     matrix.  Measure ``make -C test/offline check-offline`` before and
     after.  If the cost is unacceptable, produce a reduced-core variant
     with the same Group/NUMA/GPU structure for the harness and keep the
     full file for the unit test — but measure first rather than
     pre-emptively trimming, and never trim in a way that changes the
     device-to-NUMA relationships, which are the whole point of the file.

* Offline cases for every row of the requester's table, in both orderings
  (plain and ``:interleave``), with golden maps.  Teach the harness's
  invariant checker the new directive, including that ``--bind-to package``
  under it is an expected *rejection* — the harness already models that for
  ``must-map-by-obj``.
* ``contrib/dockerswarm/run-tests.sh``, ``test_rmaps()``: the containers
  have no GPUs, so what runs there is what only a live multi-node DVM can
  show — that the HNP maps against each node's *own* topology.  Two cases:
  a job asking for a device class no node has must fail the job and **leave
  the DVM standing**; and ``--map-by device=network`` must place procs
  against the containers' actual interfaces.  GPU cases stay offline.
* Docs: see `Phase D2 — the user documentation`_, which lands with the
  directive rather than here.


Phase H — vendor identity and the visibility envars
-----------------------------------------------------

Needs F.  This is the phase that turns "which device did I get" from a
string the process must interpret into something the vendor runtime acts
on, and it is where the identity of a device — as opposed to its position
— starts to matter.  Everything before this phase is positional, which is
why none of it noticed the two defects below.

The trigger was the reporter rebuilding hwloc against CUDA/NVML on
`OMPI #14169 <https://github.com/open-mpi/ompi/issues/14169>`_ and
attaching the resulting topology.  It settles the question that had the
envar work blocked.

What the NVML topology settles
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each GPU function now carries three OS devices and a usable identity::

    PCIDev 0000:06:00.0  (GH100 [H200 SXM 141GB])
      +-- cuda0       subtype=CUDA    Backend=CUDA
      +-- opencl0d0   subtype=OpenCL  Backend=OpenCL
      +-- nvml0       subtype=NVML    NVIDIAUUID=GPU-46f77619-68bf-...

``GPU-<uuid>`` is order-independent, so PRRTE can name a device exactly
without pinning ``CUDA_DEVICE_ORDER`` — which is the thing that killed the
index route, CUDA's default being the unspecified ``FASTEST_FIRST``
heuristic.  See the ``CUDA_VISIBLE_DEVICES`` reasoning in
:doc:`per-device-mapping`.

Two properties of that topology matter to the code and neither is
obvious:

* The enumerator dedups all three OS devices to **one** device keyed on
  the PCI function, and names it by the first vendor compute node it
  meets — ``cuda0``.  The UUID lives on the *sibling* ``nvml0``.  So
  anything wanting the vendor id walks from the chosen device to its PCI
  ancestor and scans that function's other OS devices.  It is not on the
  device PMIx hands back.
* Mapping never needed any of this.  The original DRM-only topology and
  the NVML one produce identical placement — cores 16/32/96/112 for
  ``-n 4 --map-by device=gpu --bind-to core`` — differing only in the
  reported device name (``renderD128`` vs ``cuda0``).  NVML is an *envar*
  prerequisite, not a mapping one.

The hwloc that decides this is the one **PRRTE is built against**, not
whichever one the user ran by hand: ``prted`` probes each node itself.  A
distro hwloc without the NVML backend puts us back to a bus id.

Decision: no identity, no device mapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the topology PRRTE holds for a node carries no vendor identity for its
devices, ``--map-by device=`` is **refused with a diagnostic** naming the
cause and suggesting an hwloc rebuilt with the vendor backend.  We do not
map and then silently decline to set anything: an assignment nobody can
act on is worse than a clear refusal, and it is indistinguishable from a
working run until performance is measured.

Where the check lands is the easy part.  A node whose hwloc lacked NVML
has a *different set of OS devices*, so it becomes its own recorded
topology rather than a diff (see below).  "Has vendor identity" is
therefore a property of each ``prte_topology_t``, and the mapper can test
it on the HNP without any of the plumbing in H2.

H1 — the UUID is stamped with the wrong hostname
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``build_device_uuid()`` in PMIx's ``src/hwloc/pmix_hwloc.c`` builds a GPU
uuid as::

    pmix_asprintf(uuid, "gpu://%s::%s", pmix_globals.hostname, osdev->name);

``pmix_globals.hostname`` is the hostname of *the process doing the
enumeration*.  The mapper runs on the HNP, so every device uuid PRRTE
records reads ``gpu://<HNP-hostname>::cuda0`` — for every node in the job.

That breaks the premise ``rmaps_base_devices.c`` states out loud: the uuid
travels rather than an ordinal precisely because it is "the same string
PMIx reports for that device through ``PMIX_DEVICE_DISTANCES``, so the
process can correlate the two."  A rank computing its distances locally
gets ``gpu://<its own host>::cuda0`` and will not match.  The correlation
works only on the HNP's own node.

This is wrong today, independently of anything else in this phase, and it
is where the work starts.  The fix is to make the hostname an input to the
enumeration rather than an ambient global — a device belongs to the node
whose topology it came from, not to whoever is reading that topology.
Every producer of the uuid must move together, since the whole point of
``build_device_uuid()`` being one function is that the grammar cannot vary
between paths.

H2 — per-node identity: the UUIDs are in the diff
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Verified**, not assumed.  Taking the reporter's NVML topology and a copy
differing only in ``HostName`` and the four ``NVIDIAUUID`` values,
``hwloc-diff`` (2.12.2) reports::

    Found 5 differences ... exit=0
    <diff obj_depth="0"  obj_index="0"  obj_attr_name="HostName"   .../>
    <diff obj_depth="-6" obj_index="7"  obj_attr_name="NVIDIAUUID" .../>
    <diff obj_depth="-6" obj_index="12" obj_attr_name="NVIDIAUUID" .../>
    <diff obj_depth="-6" obj_index="21" obj_attr_name="NVIDIAUUID" .../>
    <diff obj_depth="-6" obj_index="26" obj_attr_name="NVIDIAUUID" .../>

``hwloc_diff_trees()`` does descend ``io_first_child`` and does emit an
``OBJ_ATTR_INFO`` entry for a changed info value, so the delta is fully
expressible — exit 0, no ``TOO_COMPLEX``.  ``obj_depth=-6`` is
``HWLOC_TYPE_DEPTH_OS_DEVICE`` and ``obj_index`` the OS device's logical
index; ``hwloc_get_obj_by_depth()`` resolves special depths through
``slevels``, so an entry maps straight back to a device.

The consequence for PRRTE is that the identity is thrown away.
``prte_plm_base_daemon_callback()`` treats a return of 0 as "we already
have this topology": the node ``PMIX_RETAIN``\ s the *recorded* topology,
its own is destructed, and the delta goes into ``node->topodiff`` — which
is written in exactly one place, destroyed in two, and **read nowhere**.
So the mapper reads the first-reporting node's UUIDs for every node.

Positionally that is still a correct map — the structure genuinely is
identical, which is exactly why the offline harness cannot see it — but
the moment the UUID is the thing acted on it is silently wrong everywhere
except one node.

The cases split cleanly, and better than feared:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Nodes differ in
     - What happens
   * - Which devices they have (structure)
     - Different OS device children, ``TOO_COMPLEX``, separate
       ``prte_topology_t``.  Already correct.
   * - Whether the devices carry identity at all
     - An absent info attribute changes an object's **info count**, which
       ``hwloc_diff_trees()`` also calls ``TOO_COMPLEX`` — verified with
       ``hwloc-diff`` against a copy of the NVML topology with the
       ``NVIDIAUUID`` attributes stripped ("Found 5 differences, including
       4 too complex ones").  So this is structural too, and such nodes
       never share a topology either.
   * - Only the identity **values** (serial numbers)
     - Expressible diff, topology shared, values lost.  The only case in
       which a shared topology misreports anything.

What that means for the plumbing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**No inventory transport was built, because it would have had no
consumer.**  The two rows above are what settled it, and they are worth
stating as one sentence: *whether* a device can be named is a property of
the topology, and only its *value* is a property of the node.

Follow both halves to where they are read:

* The HNP needs only the first.  Its one decision is whether to accept
  ``--map-by device=<gpu class>`` at all, and it can take that from the
  recorded topology — which, per the table, cannot disagree with the node
  on that question.
* The value is read where it is used, which is the daemon: the envar in
  H3 is set at fork time, and the daemon has its own topology.  Shipping
  values to the HNP so they could be shipped back out again adds a
  round trip, a wire format and a cache to arrive at the answer the
  daemon already had.

The earlier draft of this phase called for
``PMIx_server_collect_inventory`` plumbing on the strength of "nodes can
have different GPUs".  They can — and that case was already correct,
because different GPUs mean a different topology.  The inventory API is
still the right door for anything the *topology* cannot answer (a
component querying NVML directly, say, on a node whose hwloc lacks the
backend), and ``pgpu``'s ``collect_inventory`` stubs are still where that
would go.  It is not the right door for reading an attribute out of a
topology PRRTE is already holding.

What H2 landed instead
~~~~~~~~~~~~~~~~~~~~~~

* ``pmix_hwloc_device_t`` gains ``vendor_id``, harvested by
  ``pmix_hwloc_get_devices()`` from the vendor-key table
  (``NVIDIAUUID`` / ``AMDUUID`` / ``LevelZeroUUID``).  Harvested across
  the whole PCI **function**, not off the OS device that names it: with
  CUDA and NVML both loaded the function is named ``cuda0`` while the
  uuid is on ``nvml0``, so a per-device read would report "no identity"
  on exactly the machines that have one.
* ``prte_rmaps_base_devices_begin()`` refuses a GPU-class request when
  any device on the node has no ``vendor_id``
  (``rmaps:device-not-nameable``), naming the cause and the fix.  GPU
  classes only — a fabric or network device is named by its own GUIDs.
* ``test/topologies`` gains ``turin-4gpu-nvml.xml``: the same machine as
  ``turin-4gpu.xml``, read by an hwloc built with CUDA/NVML.  The pair is
  what makes both arms of the decision testable, and the device cases in
  the offline harness moved onto the one that can be named.  Placement is
  byte-identical between the two, which is the check that the refusal
  changed no mapping.

H3 — where the abstraction lives
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

PMIx's ``pgpu`` framework, and it needs two things it does not have.

**Inventory is the right channel for identity.**
``PMIx_server_collect_inventory()`` / ``PMIx_server_deliver_inventory()``
already exist and already route to ``pmix_pgpu.collect_inventory``; the
``nvd``, ``amd`` and ``intel`` bodies are stubs (*"search the topology for
nvd GPUs"*, returning success).  PRRTE calls neither API anywhere.  So
each daemon reports what is actually on *its* node, the HNP records it
per node, and the mapper assigns from that list.  This is preferable to
mining ``node->topodiff`` even though the diff is free and already
recorded: the diff route works only while the delta happens to stay
expressible, and it cannot carry anything hwloc did not put in the
topology in the first place.

Note H2 has already settled where the *value* comes from: the daemon's
own topology, which a ``pgpu`` component reads as
``pmix_globals.topology`` and is running on anyway.  Nothing has to be
shipped to it.

**The envar cannot ride ``setup_application``.**  That path is
per-namespace: ``allocate()`` returns a blob, ``setup_local()`` unpacks it
into ``ns->envars``, and ``pmix_pgpu_base_setup_fork()`` replays *that same
list* for every local process.  ``CUDA_VISIBLE_DEVICES`` is per-rank, so
the existing path would hand every rank on a node the same value.

The hook needed is a module-level ``setup_fork``.  ``pmix_pgpu_module_t``
has no such entry; the API-level function already receives the
``pmix_proc_t *``.  Add it to the module struct, call the active modules
from ``pmix_pgpu_base_setup_fork()`` after the namespace envars, and let
the component build its own value.  That puts every vendor-specific fact —
which variable, which identifier form, what to do when the vendor is
absent — inside the component, and it runs on the **daemon**, where the
local topology is the node's own.  For the envar specifically that
sidesteps both H1 and H2; it does not fix the identity PRRTE *reports*,
which is why H1 and H2 still have to happen.

No new field on ``pmix_device_t`` is required: from ``osname``
(``cuda0``) the component walks to the PCI function and reads
``NVIDIAUUID`` off the sibling ``nvml0`` — which is what H2's
``vendor_id`` already does, so the component asks the enumerator rather
than walking the topology itself.

What H3 landed, and two things it found on the way
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``pmix_pgpu_module_t`` gains ``setup_fork``, dispatched from
  ``pmix_pgpu_base_setup_fork()`` after the namespace-wide envars.  The
  framework interface version goes to 2.0.0, since a component built
  against 1.0.0 leaves the new entry NULL.
* ``pmix_pgpu_base_set_visible_devices()`` in the base does the work all
  vendors share — fetch the proc's ``PMIX_DEVICE_ID``, resolve each
  device against the **local** topology, keep the ones belonging to the
  caller's vendor, join their identities — so a component is three lines.
  ``pmix_hwloc_device_t`` gains ``vendor`` alongside ``vendor_id`` for
  that filter, because a node may carry cards from two vendors and each
  wants its own variable.
* ``nvd`` sets ``CUDA_VISIBLE_DEVICES``, ``amd`` sets
  ``ROCR_VISIBLE_DEVICES``, ``intel`` deliberately sets nothing.

**``PMIx_server_setup_fork`` never called ``pmix_pgpu.setup_fork``.**  It
called the ``pnet`` and ``pmdl`` hooks and not this one, so the whole pgpu
environment path was dead code: ``allocate()`` harvested the vendor's
envars, ``setup_local()`` cached them, and nothing ever replayed the cache
into a child.  Adding the call repairs that independently of anything in
this phase.

**The vendor components were not built.**  ``amd``, ``intel`` and ``nvd``
gated themselves off in ``configure.m4`` unless ``--enable-test-build``
was given, on the grounds that no vendor-runtime detection existed.  That
asked the question in the wrong place: none of them links anything, and
the machine that builds PMIx is routinely not the machine that runs it, so
a cluster with GPUs got nothing unless somebody had configured a test
build.  They build unconditionally now and decline at **run** time in
``component_open``, which is where the question can actually be answered.

Verified end to end rather than only in unit tests, by handing both PRRTE
and the daemon's PMIx server the NVML topology and launching::

    $ prterun --map-by device=gpu --bind-to none -n 4 \
          printenv CUDA_VISIBLE_DEVICES
    GPU-46f77619-68bf-8f9d-7cfb-61fa9c4ca692
    GPU-32559d2f-bd80-6360-09ab-97af8614d541
    GPU-f62c37c9-47b3-35d9-91d2-b403ca0ef7ca
    GPU-81addb77-2337-6f9f-b0dd-696aa82969d1

with the three negatives confirmed the same way: a job not mapped by
device gets nothing, ``CUDA_DEVICE_ORDER`` is never set, and the same
machine's DRM-only topology is refused.

Multi-node: the case fake hardware can still make
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The question a single node cannot answer is whether each node's processes
are told about *that* node's devices, and the containers have no GPUs.
Fake them per node instead: give every container its own copy of the NVML
topology at the **same path**, with the UUIDs rewritten to carry the
node's number, and point both layers at that path —
``PRTE_MCA_hwloc_use_topo_file`` for what the daemon reports and the
mapper places against, ``PMIX_MCA_pmix_hwloc_topo_file`` for the PMIx
server that resolves an assignment to a vendor identity at fork time.
Both are forwarded from the HNP's environment to every daemon
(``plm_base_launch_support.c`` turns them into ``--prtemca`` /
``--pmixmca`` on the daemon command line), so one export reaches the
whole DVM while the *content* stays per node.

That produces exactly the shape the identity is at risk in, and the run
confirms it is the shape: four nodes reporting hardware that differs only
in serial numbers, so the HNP collapses them —

.. code-block:: text

   TOPOLOGY ALREADY RECORDED IN POSN 0 - SOME DIFFS FOUND   (x3)

— and is left holding one topology carrying node1's identities.  Sixteen
processes across those four nodes nevertheless come back with sixteen
distinct GPUs, each belonging to the node its process is running on::

   node1 GPU-n1-46f77619-…    node3 GPU-n3-46f77619-…
   node2 GPU-n2-46f77619-…    node4 GPU-n4-46f77619-…

which is the H2 conclusion demonstrated rather than argued: the head node
never needed the values, because they are read where they are used.

Vendors, and what each one can be told
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 16 26 58

   * - Vendor
     - Variable
     - Identity available
   * - NVIDIA
     - ``CUDA_VISIBLE_DEVICES``
     - ``GPU-<uuid>`` from ``NVIDIAUUID`` (NVML backend).
       Order-independent; never set ``CUDA_DEVICE_ORDER``.
   * - AMD
     - ``ROCR_VISIBLE_DEVICES``
     - Same shape, from ``AMDUUID`` (RSMI backend).
   * - Intel
     - ``ZE_AFFINITY_MASK``
     - Index-based, no uuid form — but the index is *read*, not guessed:
       hwloc's Level Zero backend records the driver and device index
       ``zeDeviceGet`` returned.  Needs ``ZE_FLAT_DEVICE_HIERARCHY``
       stated with it, since the same ordinals name a card under
       ``COMPOSITE`` and a tile under ``FLAT``.  (Written here as "needs
       its own decision; do not guess one" — the decision is recorded in
       `Phases as landed`_.)

Two properties of the values are worth stating because they decide the
failure behaviour.  A wrong ``CUDA_VISIBLE_DEVICES`` does not error — "if
an invalid index is encountered, only devices with indices that appear
before the invalid index in the list are visible" — so it silently shrinks
the visible set, which is why guessing is refused outright.  And because
UUIDs are order-independent, naming a subset composes correctly with a set
a resource manager has already filtered (Slurm's ``--gpus-per-task``
leaves ``CUDA_VISIBLE_DEVICES`` set); an index-based value would not.

H4 — the same for network devices
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The GPU work left the network classes half-finished in two ways, and this
phase closed both.

**The vocabulary was split where users expect one word.**
``device=network`` meant ``PMIX_DEVTYPE_NETWORK``, ``device=openfabrics``
meant ``PMIX_DEVTYPE_OPENFABRICS``, and ``device=nic`` meant the union.
But one HCA presents itself as *both* — an OpenFabrics OS device
(``mlx5_0``) and a network interface (``ib0``) on one PCI function — so
the two narrow spellings returned the same hardware under different names
and different counts, and whichever the user did not type looked like a
wrong answer.  ``network``, ``nic``, ``fabric`` and ``openfabrics`` are
now synonyms for the union, which the PCI-function dedup collapses to one
entry per card.  The cost is that there is no longer a spelling for
"ethernet only"; naming the interface (``device=eno6``) answers that
question exactly, and it is a narrower question than the directive is for.

**A NIC assignment could not be acted on.**  The GPU path ends by naming
the device to the vendor's runtime; the network path ended at
``PMIX_DEVICE_ID`` and nothing else.  PMIx's ``pnet`` framework now
mirrors ``pgpu``: a module-level ``setup_fork``, base fan-out after the
namespace envar cache is replayed, and
``pmix_pnet_base_get_assigned_devices()`` / ``_set_assigned_devices()``
as the shared implementation.  Two things were needed underneath it.

A NIC's **selector** is its OS device name — that is what every fabric
library's variable accepts, and unlike a GPU's vendor identity it can
never be missing.  Which of a function's two OS devices supplies that
name therefore stopped being a coin flip: ``osdev_preferred()`` now takes
the OpenFabrics device over the network interface, because ``ib0`` is
accepted by none of those variables.

And a NIC has **no vendor attribute** to filter on the way a GPU does, so
``pmix_hwloc_device_t`` now carries the PCI ``vendor``/``class`` ids and a
component selects with the same pair it hands ``pmix_hwloc_check_vendor()``
in its own ``component_open``.  On a node with two fabrics, naming
somebody else's NIC in your variable is worse than naming none.

.. list-table::
   :header-rows: 1
   :widths: 20 30 50

   * - Fabric
     - Variable
     - Form
   * - Mellanox / NVIDIA
     - ``NCCL_IB_HCA``
     - The device name (``mlx5_0``); NCCL matches an HCA by name.
   * - Mellanox / NVIDIA
     - ``UCX_NET_DEVICES``
     - ``mlx5_0:*`` — UCX names a device *port* and matches each entry as
       a glob, so this is "this card, whatever ports it has".  The ``:``
       is load-bearing: ``mlx5_1*`` would also match ``mlx5_10``.
   * - Intel Omni-Path
     - ``PSM3_NIC``
     - The device name; PSM3 selects by NIC name.
   * - Intel Omni-Path
     - ``HFI_UNIT``, ``FI_OPX_HFI_SELECT``
     - **Deliberately not set.**  Both take a unit *ordinal*, which is
       meaningful only against the driver's enumeration rather than one
       PMIx performed.  ``hfi1_0``'s trailing digit looks like it and
       usually is, but a wrong value here does not fail — it quietly puts
       the process on another adapter.

The ``nvd`` and ``opa`` components also lost their ``configure.m4``
files, for the reason ``pgpu``'s vendor components did: ``nvd`` was
hardwired off ("no real NVIDIA-transport detection exists yet") and
``opa``'s said "always succeed".  Neither links anything, and whether a
component has work to do is a property of the machine the *daemon* runs
on — which only ``component_open`` is in a position to know.

Build order
~~~~~~~~~~~

H1 first and alone: it is a live defect and needs none of the rest.  Then
H2, so per-node identity is real.  Then H3, which is only worth doing once
the identity it would publish is correct.  H4 last: it is the same shape
as H3 applied to a second framework, and it reuses the ``setup_fork``
plumbing H3 proved.


Phases as landed
----------------

Two things came out differently from the plan and are recorded here so the
document matches the tree.

**No capability macro.**  Phase D1 called for ``PRTE_CHECK_PMIX_CAP`` to
define a 0/1 macro with the C code conditional on it.  PMIx master *is*
7.0.0 and PRRTE's floor is 7.0.0, so that gate could never fail.  configure
errors out with a diagnostic instead, following the ``CLI_QUAL_VALUE``
precedent, and no conditional path exists in the C at all.

**A proc attribute cannot carry the device id.**  Phase F assumed marking
``PRTE_PROC_DEVICE_ID`` as ``PRTE_ATTR_GLOBAL`` would deliver it to the
daemon that forks the proc.  It does not: **no proc attribute list goes on
the wire**, deliberately - it was 512 KB of a 1.9 MB launch message at
1000 nodes x 128 ppn, carrying a list that has been empty in every job ever
launched.  ``prte_proc_pack`` carries a debug-build guard that says so, and
it is what caught this.  The id therefore travels as its own packed field,
and only for a job whose mapping policy is ``BYDEVICE``, so every other job
pays nothing.  The attribute stays ``PRTE_ATTR_LOCAL``.

That guard is worth knowing about before adding anything to a proc: it only
fires in a debug build, and its output goes to stdout, so on a release build
the attribute would simply never arrive and the daemon would read a default.

**Intel GPUs are told after all.**  The vendor table above says
``ZE_AFFINITY_MASK`` "needs its own decision; do not guess one", on the
premise that PMIx would have to reproduce the Level Zero runtime's device
ordering.  That premise was wrong in one specific way: PMIx does not have
to reproduce the ordering, because hwloc's Level Zero backend *recorded*
it — each root device carries the driver and device index ``zeDeviceGet``
handed back for it.  So the ordinal is read from the enumeration rather
than predicted, and read on the node that will fork the process.  What the
topology cannot record is the model that enumeration ran under, so
``ZE_FLAT_DEVICE_HIERARCHY`` is written alongside the mask when the child's
environment does not already name a model, and the assignment is dropped
with a message when it names a conflicting one.  Nothing here is guessed;
the rule the table was protecting is intact.


Verification checklist
----------------------

Per phase, in this order — the project's "did I break it?" list, with the
items this work actually touches:

1. Warning-free build from the **top** of an out-of-tree build directory
   configured ``--enable-debug`` (which implies ``--enable-devel-check``, so
   warnings are errors).  Never compile a single file by hand.
2. ``make check``.
3. ``make -C test/offline check-offline`` — for **every** phase, not just
   the mapper ones.  Phase B's whole proof is that this output does not
   change.
4. Live smoke test for D and F: ``prte --daemonize`` → ``prun`` → ``pterm``,
   including a failure path (a bad device spec must exit non-zero and leave
   the DVM running).
5. ``contrib/dockerswarm/run-tests.sh linux`` for G.
6. ``make`` in ``docs/`` — Sphinx runs with ``-W``, so a docs warning is a
   build failure.
7. After any ``help-*.txt`` edit: ``rm src/util/prte_show_help_content.*``
   then ``make``.
8. After any ``m4``/``configure.ac`` edit (phase D1 only): ``./autogen.pl``
   plus a fresh ``configure``.
9. ``git status`` after a clean build — nothing generated left untracked.

Commits are signed off (``git commit -s``), one logical change each, prose
messages saying *why*.  Branches go to a personal fork, never to ``origin``.


Risks and open items
--------------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Risk
     - Handling
   * - Phase B changes placement by accident
     - The golden-map diff is the gate.  Land B alone so a later placement
       bug cannot be blamed on it, or hidden by it.
   * - PMIx cap flag is unavailable
     - ``--map-by device=`` is refused **with a diagnostic**, never silently
       absent.  A knob that does nothing is worse than no knob.
   * - ``:i`` silently becomes ``INTERLEAVE``
     - Chain ordering + the explicit regression test in phase E.
   * - Adding the big topology slows the offline harness
     - Measure; reduced-core variant only if needed (phase G).
   * - Mixed-vendor node hides a GPU
     - The union rule (refinement 1).  **No topology in hand tests this** —
       one CUDA GPU with a backend beside an AMD GPU without.  Worth
       constructing by hand if a mixed node is ever available.
   * - ``prte_uniform_nodes`` maps against another node's topology
     - Documented, not detected.  Nothing else in the tree detects it
       either.
   * - Every node is told the first node's device UUIDs
     - Real, and invisible to the offline harness because the placement
       stays correct — only the identity is wrong.  Phase H2.
   * - The reported uuid names the HNP, not the node
     - Real today.  Phase H1, and it lands first because it is a defect
       rather than a missing feature.

Still undecided, and neither blocks phase C:

1. **More processes than devices.**  ``byobj``'s ``redo:`` loop wraps and
   puts a second process on each object in turn — two per GPU for ``-n 8``
   on a four-GPU node.  Almost certainly right, but it is a policy choice
   and belongs in the docs explicitly rather than inherited by accident.
2. **A per-app MPMD swarm case.**  The unit test covers the two parsers
   agreeing; an MPMD line with a different device class per app is the shape
   that would catch a plumbing error in ``resolve_app_options()``.


Task checklist
--------------

**Phase A — dist removal**

- [ ] Delete ``PRTE_CLI_DIST``, ``prte_rmaps_base.device``, the ``BYDIST``
      switch case, the ``"MINDIST"`` printer arm
- [ ] Strip ``dist`` / ``DEVICE=dev`` from the ``mapby`` help string
- [ ] Build, ``make check``, ``check-offline``

**Phase B — enumerator vtable**

- [ ] Capture pre-change offline goldens
- [ ] Add ``prte_rmaps_target_enum_t``; split ``byobj`` into body + wrapper
- [ ] Route the two verbose messages and ``mapping-target-not-found``
      through ``->name``
- [ ] Goldens byte-identical

**Phase C — PMIx (openpmix)**

- [ ] ``pmix_hwloc_get_devices()`` with dedup, PCI ordering, localities
- [ ] Union GPU rule; drop the ``card*`` skip; stop skipping ``COPROC``
- [ ] ``PMIX_CAP_DEVICE_ENUM``
- [ ] PMIx unit tests against both PRRTE topologies

**Phase D — directive and mapper**

- [ ] ``PRTE_CHECK_PMIX_CAP([DEVICE_ENUM])`` + ``autogen.pl`` + reconfigure
- [ ] ``prte_hwloc_device_t`` + ``prte_hwloc_base_get_devices()``
- [ ] ``PRTE_CLI_DEVICE``; ``mappers[]``; both ``--map-by`` parsers; bare
      ``gpu`` refused
- [ ] Rename ``BYDIST``→``BYDEVICE``, ``dist_device``→``map_device``, the
      two attribute keys **and their strings**; rmaps version → 6.0.0
- [ ] Device enumerator instance + per-node bind ceiling
- [ ] Three new help topics; regenerate show_help content
- [ ] Unit tests: hwloc enumerator, both parsers, dispatch gate
- [ ] Build / check / check-offline / smoke test

**Phase D2 — user documentation**

- [ ] ``help-mapby.txt`` ``[map-by]`` directive list
- [ ] ``help-placement.txt``: ``[placement-examples]`` (option list +
      worked example) and ``[placement-fundamentals]`` (the model)
- [ ] ``cli-map-by.rst`` directive + qualifier lists
- [ ] ``detail-placement-fundamentals.rst`` — what a device's locality is
- [ ] ``detail-placement-examples.rst`` — the reporter's machine, worked
- [ ] no bare ``gpu`` / refused coarse binding / degenerate case all stated
- [ ] regenerate show_help content in the **build** tree; docs build ``-W``;
      ``prte-convert-help.py --check-only``

**Phase E — interleave**

- [ ] ``PRTE_CLI_INTERLEAVE``; ``mapquals[]``; arm appended **after**
      ``inherit`` in both chains
- [ ] Grouping + round-robin reorder; default ``package``; ``node`` refused;
      non-device map refused by name
- [ ] Tests incl. **:i → INHERIT** regression

**Phase F — reporting**

- [ ] ``PRTE_PROC_DEVICE_ID`` (+15), ``PRTE_ATTR_GLOBAL``, key string
- [ ] Set in the mapper; publish as ``PMIX_DEVICE_ID``; show in
      ``--display map``

**Phase G — confirmation**

- [ ] ``turin-4gpu.xml`` + ``EXTRA_DIST``; measure harness cost
- [ ] Offline cases for the full requested table, both orderings
- [ ] Swarm: no-such-device fails the job and leaves the DVM up;
      ``device=network`` places correctly
- [ ] (docs moved to phase D2)
- [ ] Reply on OMPI #14169 with the resulting table

**Phase H — vendor identity and the envars**

- [x] H1: ``build_device_uuid()`` takes the owning node's hostname instead
      of reading ``pmix_globals.hostname``; every producer moves together
- [x] H1: PMIx and PRRTE unit tests enumerating one topology under two
      node names
- [x] H2: ``pmix_hwloc_device_t.vendor_id``, harvested across the PCI
      function from the vendor-key table
- [x] H2: **inventory transport NOT built** — identity *presence* is
      structural and only its *value* is per-node, so the HNP needs
      nothing shipped to it; see "What that means for the plumbing"
- [x] Refuse ``--map-by device=<gpu class>`` with a diagnostic when the
      devices carry no vendor identity; ``rmaps:device-not-nameable``;
      show_help content regenerated
- [x] ``turin-4gpu-nvml.xml`` added; offline device cases moved onto it;
      refusal case added for ``turin-4gpu``; stale device goldens removed
- [x] ``check-offline`` now runs ``--golden`` (the snapshots had drifted
      two changes behind the code, unread)
- [x] H3: ``setup_fork`` added to ``pmix_pgpu_module_t``; called from
      ``pmix_pgpu_base_setup_fork()`` after the namespace envars; pgpu
      framework version 1.0.0 → 2.0.0
- [x] H3: ``PMIx_server_setup_fork`` now calls ``pmix_pgpu.setup_fork``
      at all — it never did, so the whole pgpu envar path was dead
- [x] H3: ``nvd`` sets ``CUDA_VISIBLE_DEVICES`` from ``NVIDIAUUID``;
      ``amd`` from ``AMDUUID``; ``intel`` left deliberately unimplemented
- [x] H3: the vendor components build unconditionally instead of only
      under ``--enable-test-build``; detection moves to ``component_open``
- [x] H3: end-to-end launch confirms the value, and the three negatives
- [x] Multi-node confirmation that ranks on different nodes receive
      *their own* node's identities — dockerswarm, per-node topology
      files, 16 procs over 4 nodes, 16 distinct GPUs, and the HNP
      verified to be sharing one topology while it happens
