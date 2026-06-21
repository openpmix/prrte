.. Copyright (c) 2026      Nanook Consulting  All rights reserved.
   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

==================================================================
Offline Mapping Test Harness — Specification
==================================================================

:Status: Draft / proposed
:Location: ``test/offline/``
:Audience: PRRTE contributors working on ``src/mca/rmaps`` and ``src/hwloc``

Purpose
=======

PRRTE can compute and print a complete job map — process-to-node
placement, ranks, and CPU bindings — **without launching any daemons or
application processes**, by combining the ``donotlaunch`` runtime option
with ``--display map`` and a *simulated* node topology loaded from an
hwloc XML file (see the "Testing the mapper without launching" section of
``CLAUDE.md``).

This document specifies a test harness, to live in ``test/offline/``,
that exercises that capability systematically: it drives ``prterun`` over
the full matrix of ``--map-by`` / ``--rank-by`` / ``--bind-to``
combinations against a set of one or more simulated hwloc topologies
(initially ``test/topologies/test-topo.xml``), parses each printed map,
and checks it for correctness.

The harness is **test-only**: it is not compiled, not installed, and is
not part of the shipped product. It complements the compiled C unit
tests under ``test/unit/rmaps/`` (which exercise the dispatch/guard logic
reachable without a topology) by covering the end-to-end placement,
ranking, and binding results that those unit tests deliberately defer to
"the offline ``donotlaunch`` method."

Goals and non-goals
====================

Goals
-----

- Verify, for every supported map/rank/bind combination, that the
  produced map is **internally consistent and semantically correct**
  with respect to the requested policy.
- Run with no special privileges, no allocation, and no network — on a
  developer laptop or in CI — in seconds.
- Be deterministic: the same inputs against the same topology file
  must always produce the same verdicts.
- Produce clear, greppable PASS/FAIL output with enough context to
  localize a regression to a specific case and invariant.
- Detect regressions in ``src/mca/rmaps`` and ``src/hwloc`` binding code
  before any integration test is attempted.

Non-goals
---------

- The harness does **not** reimplement the mapper. It does not compute a
  byte-for-byte "expected" placement for every case from first
  principles (that would merely duplicate the code under test). Instead
  it checks *invariants* that any correct mapping must satisfy, plus an
  optional set of human-reviewed golden snapshots for regression
  pinning (see `Validation strategy`_).
- It does not test launch, I/O forwarding, collectives, or any path that
  requires live daemons.
- It does not test resource-manager allocation discovery (``ras``); the
  node pool is always supplied explicitly with ``-H``.

Background: what the harness drives
===================================

The single command form the harness builds and runs is::

    prterun --rtos donotlaunch --display map \
            --prtemca hwloc_use_topo_file <TOPO> \
            -H <node-spec> \
            [--map-by M] [--rank-by R] [--bind-to B] \
            -n <N> hostname

- ``--rtos donotlaunch`` runs the mapper/ranker/binder, prints the map,
  and exits. Nothing is forked or exec'd. (``--do-not-launch`` is a
  deprecated alias.)
- ``--display map`` prints the resulting map. ``--display map-devel``
  prints more detail and may be used by the harness for diagnostics.
  (``--display-map`` is a deprecated alias.)
- ``--prtemca hwloc_use_topo_file <TOPO>`` binds against the simulated
  topology rather than the head node's own. The harness always passes
  this so that results are independent of the machine running the test.
- ``-H node0:N,node1:M,...`` declares the simulated nodes and their slot
  counts. All simulated nodes share the one topology from the XML file.
- ``hostname`` is an arbitrary placeholder executable; it is never run.

Locating ``prterun``
--------------------

``prterun`` is installed as a symlink to ``prte``; the personality is
selected from ``argv[0]``. The built binary in a source tree is
``src/tools/prte/prte`` and the ``prterun`` name only appears after
``make install``. The harness must therefore locate an executable named
``prterun`` in this order:

1. ``$PRTE_PRTERUN`` if set (explicit override).
2. ``prterun`` on ``$PATH``.
3. ``$top_builddir/src/tools/prte/prterun`` — creating that symlink to
   the freshly built ``prte`` if it is absent — when invoked from the
   build tree.

If none is found, the harness exits with the "skip" status (see
`Exit status and CI integration`_) rather than failing.

Topologies
==========

The harness is **topology-agnostic**. It runs against a *set* of one or
more hwloc XML topology files and makes **no assumption about the shape
of any of them** — every dimension it needs is derived by parsing each
file at runtime. This is deliberate: we expect to add topologies over
time (SMT vs. non-SMT, symmetric vs. asymmetric packages, varying cache
groupings), and the same case matrix must run unchanged against each.

Topology set
------------

- Topology XML files live in a **shared** ``test/topologies/`` directory
  so that both the offline harness and the C unit tests draw from one
  canonical location. (``test-topo.xml`` is relocated there from
  ``test/unit/rmaps/`` as part of this work; see the implementation
  plan.)
- **The default topology set is every ``*.xml`` file found in
  ``test/topologies/``.** The harness discovers them by scanning that
  directory (sorted by name for determinism) and runs its full pattern
  set against each one in turn — so dropping a new topology XML into
  ``test/topologies/`` automatically extends coverage with no edit to
  the harness, the build, or any manifest. The files are referenced in
  place, not copied.
- A ``--topo <file>`` / ``--topo-set <list>`` option overrides discovery
  so a developer can point the whole pattern set at one new topology
  while iterating.
- Each topology file is passed to ``prterun`` via
  ``--prtemca hwloc_use_topo_file <file>``, so results never depend on
  the machine running the test.

Per-topology model the harness derives
--------------------------------------

For each topology file, before running any case, the harness parses the
XML (``xml.etree.ElementTree``) and builds a model containing, at
minimum:

- the count of objects at every level present (Machine, Package,
  NUMANode, L3/L2/L1 Cache, Core, PU);
- for each object, the set of child cores / PUs it contains (its span),
  recording that these may be **uneven** across siblings (e.g. a cache
  level need not divide the core count evenly) and **need not be nested
  uniformly** (e.g. NUMA-per-package may be >1);
- the **PUs-per-core ratio**, from which the harness decides *per
  topology* whether ``hwthread`` and ``core`` are distinguishable:

  - if every core has one PU (no SMT), ``--map-by hwthread`` ==
    ``--map-by core`` and ``--bind-to hwthread`` == ``--bind-to core``
    **for that topology**, and the harness treats the pair as
    expected-equal rather than asserting a distinction the topology
    cannot express;
  - if cores have multiple PUs, the harness asserts the hwthread/core
    distinction normally.

- which directive levels are even *meaningful* for the topology: a level
  with a single object across the whole pool (e.g. one package per node)
  makes ``--map-by package`` degenerate; the harness records this so the
  distribution invariants scale to the actual object count rather than a
  hardcoded one.

All invariants (see `Validation strategy`_) are expressed in terms of
this derived model — object counts, spans, and the PU/core ratio — never
in terms of literal numbers. A topology can therefore be added to the
set with no change to the invariant logic. There is **no** hardcoded
dimension table anywhere in the harness.

The supported directive vocabulary
==================================

Empirically (from ``prterun``'s own "Valid directives" diagnostics
against this build), the accepted tokens are:

``--map-by``
    ``slot``, ``hwthread``, ``core``, ``l1cache``, ``l2cache``,
    ``l3cache``, ``numa``, ``package``, ``node``, ``seq``, ``ppr``,
    ``rankfile``, ``pe-list=`` — plus colon-separated modifiers such as
    ``PE=n``, ``SPAN``, ``OVERSUBSCRIBE``, ``NOOVERSUBSCRIBE``,
    ``pe-list=...``.

``--rank-by``
    ``slot``, ``node``, ``fill``, ``span`` — **only these four.**
    (``--rank-by core`` and other object levels are rejected.)

``--bind-to``
    ``none``, ``hwthread``, ``core``, ``l1cache``, ``l2cache``,
    ``l3cache``, ``numa``, ``package``.

The **core matrix** the harness must cover is the Cartesian product of
the *topology-level* mappers with the rank and bind vocabularies:

- ``--map-by`` ∈ {slot, node, package, numa, l3cache, l2cache, l1cache,
  core, hwthread} — 9 values
- ``--rank-by`` ∈ {slot, node, fill, span} — 4 values
- ``--bind-to`` ∈ {none, hwthread, core, l1cache, l2cache, l3cache,
  numa, package} — 8 values

= **288 combinations**, each run against several node layouts and
process counts (see `Case generation`_).

The directive families that require extra arguments — ``seq``,
``rankfile``, ``ppr``, ``pe-list=`` — and the modifiers
(``OVERSUBSCRIBE``, ``SPAN``, ``PE=n``) are covered by separate,
purpose-built case groups rather than the blanket matrix, because each
needs its own inputs and its own invariants.

Output grammar
==============

A successful ``--display map`` run prints a block of this shape (jobid,
hostname, and slot totals vary per run and per environment)::

    ========================   JOB MAP   ========================
    Data for JOB <jobid> offset 0 Total slots allocated <S>
        Mapping policy: <MAP>[:<mods>]  Ranking policy: <RANK> Binding policy: <BIND>
        Cpu set: N/A  PPR: N/A  Cpus-per-rank: N/A  Cpu Type: <CORE|HWTHREAD>

    Data for node: <name>	Num slots: <ns>	Max slots: <ms>	Num procs: <np>
            Process jobid: <jobid> App: <a> Process rank: <r> Bound: <bound>
            ...

    [DONOTLAUNCH headnode-architecture warning]
    =============================================================

``<bound>`` takes one of these forms:

- ``N/A`` — process is unbound (``--bind-to none``).
- ``package[<i>][core:L<x>]`` — bound to a single core.
- ``package[<i>][core:L<x>-<y>]`` — bound to a contiguous core range
  (e.g. a whole package or NUMA node or cache).

Generally: ``<obj>[<os_index>][<child>:L<range>]`` where ``<obj>`` is the
enclosing object, ``<child>`` the child granularity, and ``<range>`` one
index or an inclusive ``Lx-Ly`` span (logical indices).

The parser must:

- Normalize away non-deterministic noise before any comparison:

  - the ``Warning: program compiled against libxml ...`` line,
  - the entire trailing DONOTLAUNCH "headnode architecture" warning
    paragraph,
  - the ``<jobid>`` token (e.g. ``prterun-host-12345@1``) — replace with
    a fixed placeholder,
  - the ``hostname`` host component and PID in the jobid,
  - the ``Total slots allocated <S>`` number (it reflects the head
    node's probed topology and is irrelevant to placement).

- Extract a structured representation:

  - header: mapping policy + modifiers, ranking policy, binding policy,
    cpu type;
  - per node: name, num slots, max slots, num procs, and an ordered list
    of ``(app_idx, rank, bound)`` tuples.

Success / failure detection
===========================

A run is a **successful map** when **both**:

- the process exit status is ``0``, **and**
- the output contains exactly one ``JOB MAP`` block.

A run is a **mapper rejection / error** when the exit status is nonzero
(observed: ``213``) **or** the output contains a known error banner.
Error output is framed by a line of dashes and carries one of:

- ``Valid directives:`` (bad/unsupported token),
- ``not supported as a default value`` (modifier allowed only per-job),
- ``Please check for a typo``,
- other framed diagnostic paragraphs with no ``JOB MAP`` block.

The harness must **not** treat a nonzero exit alone as the only signal,
nor a zero exit alone as success: it must confirm the ``JOB MAP`` block
is present and well formed. (Both conditions have been observed
independently; relying on exit status alone is insufficient.)

Each case in the matrix declares whether it is *expected to succeed* or
*expected to be rejected*. A mismatch (a case that should map but errors,
or that should be rejected but maps) is a FAIL.

Validation strategy
===================

Two complementary layers. Layer 1 is mandatory; Layer 2 is recommended.

Layer 1 — Invariant checking (primary, oracle-free)
---------------------------------------------------

For every successful map, the harness checks invariants that any correct
mapping must satisfy. These do not require an independent oracle and so
do not duplicate the mapper.

**Universal invariants (every successful case):**

U1. Header ``Mapping policy`` matches the requested ``--map-by`` (modulo
    modifiers); ``Ranking policy`` matches ``--rank-by``;
    ``Binding policy`` matches ``--bind-to``.
U2. Total processes summed over all nodes equals ``-n N``.
U3. The multiset of ranks equals exactly ``{0, 1, ..., N-1}`` — no
    duplicates, no gaps.
U4. Each node's printed ``Num procs`` equals the number of process lines
    listed under it.
U5. Every ``App:`` index is valid for the job (``0`` for a single-app
    job; the declared set for multi-app cases).
U6. No node carries more processes than its ``Num slots`` **unless** the
    effective policy includes ``OVERSUBSCRIBE``; when oversubscribed,
    processes still respect ``Max slots`` if a nonzero max is set.
U7. Every ``Bound`` field is well formed for the requested bind level
    (see B-invariants), and references core/object indices that exist in
    the topology.

**Mapping (placement-shape) invariants — by ``--map-by``:**

M-node. Processes are distributed round-robin across nodes: per-node
    counts differ by at most one, and walking the mapping order cycles
    through the nodes.
M-slot. Processes fill each node's slots (block distribution) before
    moving to the next node, until ``N`` is placed (subject to
    oversubscribe when ``N`` exceeds total slots).
M-<obj> (package, numa, l3cache, l2cache, l1cache, core, hwthread).
    Processes are distributed round-robin across objects of that level,
    using the per-object child sets parsed from the topology. The
    placement cycle length matches the count of that object level
    available in the node pool. Consecutive processes on a node advance
    to the next object of the requested level (wrapping as needed).

**Ranking invariants — by ``--rank-by``:**

R-slot. Ranks follow mapping order (the order processes were placed).
R-node. Ranks stride across nodes: successive ranks land on successive
    nodes (round-robin by node), independent of how many procs share a
    node.
R-span. Ranking treats all mapping objects across the whole node pool as
    one contiguous numbering space.
R-fill. Ranks fill one mapping object completely before advancing to the
    next.

Each ranking invariant is expressed as a property of the
``rank → (node, placement-slot)`` relation derived from the parsed map,
not as a hardcoded expected sequence.

**Binding invariants — by ``--bind-to``:**

B-none. Every ``Bound`` is ``N/A``.
B-core / B-hwthread. Each process is bound to a single granularity unit
    (one ``L<x>``, no range). On a topology with one PU per core the two
    levels are treated as equivalent (per the derived PU/core ratio); on
    an SMT topology the hwthread binding targets a single PU within a
    core while the core binding spans the core's PUs.
B-<obj> (package, numa, lXcache). Each process is bound to the full core
    span of one object of that level; the displayed range equals that
    object's child-core set **as parsed from the topology model**, not a
    hardcoded span. (For the committed topology, for example, this
    happens to be package 0 ⇒ ``core:L0-87`` and package 1 ⇒
    ``core:L88-175``, but the harness computes the expected span from the
    XML for whichever topology is under test.)
B-locality. A process's binding target lies within the object it was
    mapped onto when bind level is finer than or equal to map level
    (no process is bound outside the resource it was assigned).

Any violated invariant is reported as ``FAIL <case-id> <invariant-id>:
<message>`` with the offending node/rank quoted.

Layer 2 — Golden snapshots (regression pinning, recommended)
------------------------------------------------------------

For a curated, human-reviewed subset of cases (a few dozen that exercise
representative map/rank/bind shapes, multi-node uneven layouts, and the
special families), the harness stores the *normalized* map output as a
golden file under ``test/offline/golden/<case-id>.map``. On each run it
diffs the freshly normalized output against the golden file.

- Golden files are produced once, **inspected by a human for
  correctness**, and committed. They pin behavior so that an
  unintended change in placement is caught even if it still satisfies
  the invariants.
- A ``--update-golden`` (or ``UPDATE_GOLDEN=1``) mode regenerates them;
  regeneration requires explicit human review of the diff before commit.
- Goldens are keyed by case id **and** topology name, so each topology
  in the set has its own reference output. If a topology file changes,
  its goldens must be regenerated and re-reviewed.

Invariants catch *semantic* breakage; goldens catch *silent drift*.
Neither alone is sufficient, which is why both layers exist.

Case generation
===============

The harness enumerates cases from a small declarative description rather
than hardcoding 288+ command lines.

Node layouts (each combination is run against several):

- ``single``: ``-H node0:8`` — single node, all objects local.
- ``even``: ``-H node0:8,node1:8,node2:8`` — three equal nodes.
- ``uneven``: ``-H node0:8,node1:4`` — exercises by-node vs by-slot rank
  differences that are only visible across nodes of differing size.

Process counts: a small set chosen relative to the layout, including

- ``N`` < total slots (no oversubscription),
- ``N`` == total slots (exact fill),
- ``N`` that forces round-robin wrap (e.g. ``N`` = 2 × node count + 1),
- a deliberately oversubscribed ``N`` (paired with an
  ``OVERSUBSCRIBE`` modifier) in the dedicated oversubscribe group.

The blanket matrix (288 combos) is run primarily against the ``even``
and ``uneven`` layouts with a couple of ``N`` values each. Slot counts
in ``-H`` are independent of the topology (they bound oversubscription,
not the hardware) and must be **at least** the per-node process count
for the non-oversubscribed cases.

**The full set of cases is crossed with the topology set:** every
case (matrix and groups) is run once per topology file in the active
set, and its invariants are evaluated against that topology's derived
model. Adding a topology to the set therefore multiplies coverage with
no change to the case definitions. Cases that are degenerate for a given
topology (e.g. ``--map-by package`` when that topology has a single
package per node) are still run, but their distribution invariants scale
to the derived object count rather than being skipped.

Dedicated case groups (outside the blanket matrix):

- **oversubscribe** — ``--map-by <obj>:OVERSUBSCRIBE`` with ``N`` >
  slots; verify U6's oversubscribe branch and even spillover.
- **span / PE=n** — ``--map-by package:SPAN``, ``--map-by core:PE=2``,
  etc.; verify the corresponding distribution and binding width.
- **ppr** — e.g. ``--map-by ppr:2:package``; verify exactly that many
  procs per stated object and correct total.
- **seq / rankfile** — supply a small sequence/rank file (committed
  under ``test/offline/inputs/``) and verify the explicit placement.
- **multi-app** — two app contexts in one invocation
  (``-n 2 hostname : -n 3 hostname``, optionally with per-app
  ``--map-by``); verify per-app ``App:`` indices, per-app placement, and
  global rank continuity. This directly exercises the per-app mapping
  work tracked under ``docs/plans/per_app_mapping/``.
- **negative** — intentionally invalid tokens/modifiers (bad ``bind-to``
  value, ``--rank-by core``, default-only modifier); verify the run is
  rejected with the expected banner and no ``JOB MAP``.

Output and reporting
====================

- One line per case: ``PASS <case-id>`` or ``FAIL <case-id>
  <invariant-id>: <detail>``.
- On any FAIL, the harness prints the exact ``prterun`` command line and
  the normalized map (or the error banner) so the failure can be
  reproduced by copy/paste.
- A trailing summary: counts of pass / fail / skipped, and total wall
  time.
- A ``--verbose`` mode echoes every command and its parsed structure; a
  ``--filter <glob>`` selects a subset of case-ids for focused
  debugging.

Exit status and CI integration
==============================

- Exit ``0`` if all selected cases pass.
- Exit ``1`` if any case fails.
- Exit ``77`` (the Automake "skip" convention) if prerequisites are
  missing — no ``prterun`` found, or the build lacks hwloc topology-file
  support — so that ``make check`` records a SKIP rather than an error
  on configurations that cannot run it.

Build integration (optional, recommended): add ``test/offline`` to the
``test`` subdir wiring and register the driver as a ``TESTS`` entry
guarded by the skip behavior above. The driver locates the freshly built
``prterun`` via ``$(top_builddir)`` as described in `Locating prterun`_.
The driver discovers topologies by scanning the shared
``test/topologies/`` directory (located via ``$(top_srcdir)``), so each
XML lives in exactly one place in the tree and the build does not have to
enumerate them.

Implementation notes
====================

- **Language: Python 3, standard library only.** The harness is
  fundamentally text generation + parsing + structural assertions, for
  which Python is clear and robust; it ships nowhere and adds no
  build/runtime dependency to PRRTE (it is invoked only when a developer
  or CI runs the offline tests). No third-party packages.
- Each topology XML is parsed with ``xml.etree.ElementTree`` to build the
  per-topology model (object counts, per-object core/PU spans, PU/core
  ratio); no external XML dependency.
- All invariants operate on the **parsed map structure and the derived
  topology model**, never on raw string matching or hardcoded
  dimensions, so neither a display-format tweak (whitespace, ordering)
  nor a new/regenerated topology produces a false failure. Only the
  normalization rules touch raw text.

Proposed directory layout
=========================

::

    test/topologies/             # shared hwloc XML topologies (offline + unit tests)
        test-topo.xml            # relocated here from test/unit/rmaps/
        *.xml                    # every file here is auto-discovered and tested

    test/offline/
        SPEC.rst                 # this document
        README.rst               # how to run (one-liner + examples)
        run_offline_maps.py      # the driver (to be implemented)
        cases/                   # declarative case definitions (matrix + groups)
        inputs/                  # seq / rankfile inputs for those groups
        golden/                  # normalized reference maps, keyed by case + topology

The topology set is **not** maintained in any list: the driver runs its
pattern set against every ``*.xml`` in ``test/topologies/`` (sorted for
determinism). Adding a topology is a one-step operation — drop the XML
into that directory. The files are **not** copied into ``test/offline/``;
they are referenced in place. Because the pattern set is crossed with the
topology set, golden files are keyed by both the case id and the topology
name (the XML's basename stem).

Future extensions
=================

- Drop an SMT topology (≥2 PUs per core) into ``test/topologies/`` to
  exercise the ``hwthread`` vs. ``core`` distinction the harness already
  derives but the initial topology cannot express.
- Drop an asymmetric-package / asymmetric-NUMA topology into
  ``test/topologies/`` to stress non-uniform distribution.
- Heterogeneous node pools (different topologies per node) once the
  offline path supports per-node topology files.
- ``--display map-devel`` / ``--display allocation`` parsing for deeper
  binding-detail assertions.
