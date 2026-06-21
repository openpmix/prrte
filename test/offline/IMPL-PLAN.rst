.. Copyright (c) 2026      Nanook Consulting  All rights reserved.
   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

==================================================================
Offline Mapping Test Harness — Implementation Plan
==================================================================

:Status: Draft / proposed
:Spec: ``test/offline/SPEC.rst``
:Audience: implementer of the harness

Overview
========

This plan turns ``SPEC.rst`` into an ordered build-out of the offline
mapping test harness. The deliverable is a Python 3 (stdlib-only) driver,
``test/offline/run_offline_maps.py``, that drives ``prterun --rtos
donotlaunch --display map`` over a matrix of map/rank/bind directives,
crossed with a set of simulated hwloc topologies, parses each printed
map, and verifies it against per-topology-derived invariants (plus
optional golden snapshots).

The work is split into phases ordered by dependency. Each phase is
independently testable: a later phase never has to revisit an earlier
one. Phase R (do first) relocates the shared topology into
``test/topologies/`` and can land as its own self-contained commit;
phases 0–6 produce a working invariant-checking harness; phase 7 adds
golden snapshots; phase 8 wires it into the build; phase 9 is bring-up
against the current tree.

Current state
=============

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Item
     - Current value
   * - ``test/offline/``
     - contains only ``SPEC.rst`` and this plan
   * - Build wiring for test dirs
     - ``config/prte_config_files.m4`` lists ``test/Makefile``,
       ``test/unit/Makefile``, ``test/unit/rmaps/Makefile``,
       ``test/attachtest/Makefile``
   * - ``test/Makefile.am`` ``SUBDIRS``
     - ``unit attachtest`` (no ``offline``)
   * - Simulated topology in tree
     - ``test/unit/rmaps/test-topo.xml`` today (1 PU/core, 2 pkg, 4 NUMA,
       24 L3, 176 core — but the harness must derive this, never assume);
       relocated to ``test/topologies/`` by Phase R below
   * - References to ``test-topo.xml`` to update on move
     - ``test/unit/rmaps/Makefile.am`` (``EXTRA_DIST``), ``CLAUDE.md``
       (2), ``docs/plans/per_app_mapping/`` (2). No C test loads it at
       runtime — it is referenced only for packaging and in docs.
   * - In-tree ``prterun``
     - symlink to ``prte``, created only at ``make install``; build-tree
       binary is ``src/tools/prte/prte``

Module architecture
===================

A single file, ``run_offline_maps.py``, organized into these units
(classes/functions). Keeping it one file avoids package/import friction
for a test-only script; the units are still cleanly separable.

- ``locate_prterun()`` → path, or ``None``.
- ``discover_topologies(topo_dir)`` → sorted list of ``*.xml`` paths in
  ``test/topologies/`` (overridable by ``--topo``/``--topo-set``).
- ``TopoModel`` — parsed model of one topology XML.

  - ``TopoModel.from_xml(path)`` classmethod.
  - fields: ``counts`` (level→int), ``objects`` (level→list of ``Obj``),
    ``pus_per_core`` (int), ``name`` (basename stem).
  - ``Obj``: ``level``, ``os_index``, ``core_set`` (frozenset of logical
    core ids), ``pu_set``.
  - helpers: ``objects_at(level)``, ``span_of(obj)``,
    ``hwthread_eq_core`` (True when ``pus_per_core == 1``).

- ``ProcEntry`` / ``NodeMap`` / ``ParsedMap`` — parsed ``--display map``
  structure (see Phase 2).
- ``normalize(raw_text)`` → canonical text (strips noise/jobid).
- ``parse_map(raw_text)`` → ``ParsedMap`` or raises ``ParseError``.
- ``RunResult`` — ``argv``, ``rc``, ``stdout``, ``stderr``, ``mapped``
  (bool), ``error_banner`` (str|None).
- ``run_case(prterun, case)`` → ``RunResult``.
- ``Case`` — one test case (see Phase 4).
- ``generate_cases(topo_models)`` → list of ``Case``.
- invariant checkers (Phase 5): each ``check_*(case, pmap, topo)`` →
  list of ``Violation``.
- ``Reporter`` — PASS/FAIL lines, summary, exit-code policy.
- ``main(argv)`` — CLI, orchestration.

Phases
======

Phase R — Relocate the shared topology directory (do first)
-----------------------------------------------------------

**Goal:** ``test-topo.xml`` lives in a new shared ``test/topologies/``
directory that both the offline harness and the C unit tests reference,
with no behavior change to the existing unit tests. This is a
prerequisite for topology discovery (Phase 0) and can be committed on
its own.

Steps:

- ``git mv test/unit/rmaps/test-topo.xml test/topologies/test-topo.xml``
  (preserve history).
- Create ``test/topologies/Makefile.am``: standard copyright header and
  ``EXTRA_DIST = test-topo.xml`` (plus future XMLs). This directory holds
  *only* topology data — no compiled targets.
- Remove ``EXTRA_DIST = test-topo.xml`` from
  ``test/unit/rmaps/Makefile.am`` (the file no longer lives there). The
  C unit tests do not load the file at runtime, so nothing else in that
  Makefile changes.
- Build wiring:

  - add ``test/topologies/Makefile`` to ``config/prte_config_files.m4``;
  - add ``topologies`` to ``SUBDIRS`` in ``test/Makefile.am`` (before
    ``offline``, which Phase 8 adds);
  - re-run ``./autogen.pl`` (required after editing ``*.m4`` /
    ``Makefile.am``).

- Update the remaining references to the old path:

  - ``CLAUDE.md`` — the two ``test/unit/rmaps/test-topo.xml`` mentions in
    the "Testing the mapper without launching" section →
    ``test/topologies/test-topo.xml``;
  - ``docs/plans/per_app_mapping/per-app-mapping.rst`` and
    ``rmaps-tasks.rst`` — the two command examples.

- Verify: ``make dist`` includes ``test/topologies/test-topo.xml``; the
  CLAUDE.md offline command runs unchanged with the new path; the rmaps
  C unit tests (``make -C test/unit/rmaps check``) still pass.

**Done when:** the file is in ``test/topologies/``, all references point
there, ``configure`` regenerates with the new Makefile, and existing
tests/dist are unaffected.

Phase 0 — Scaffolding
---------------------

**Goal:** the directory exists with its support files and a runnable
(empty) driver that correctly skips when prerequisites are absent.

- Create ``test/offline/README.rst`` — the one-liner invocation and a
  couple of worked examples (mirrors the CLAUDE.md offline section).
- Create ``cases/``, ``inputs/``, ``golden/`` directories with a
  ``.gitkeep`` (or a README) so they exist in the tree.
- Create ``run_offline_maps.py`` with the shebang, the copyright header
  (Python ``#`` comment form), ``main()``, argument parsing skeleton, and
  ``locate_prterun()`` implementing the SPEC lookup order
  (``$PRTE_PRTERUN`` → ``$PATH`` → ``$top_builddir/src/tools/prte/prte``
  with a ``prterun`` symlink created beside it). When no prterun and no
  usable topology support is found, exit ``77``.
- Implement ``discover_topologies()``: resolve ``test/topologies/`` via
  ``$(top_srcdir)`` (or relative to the script), glob ``*.xml``, sort by
  name. ``--topo``/``--topo-set`` override the glob. An empty directory
  (no topologies) is a skip (77), not a failure.
- Make the script executable.

**Done when:** ``./run_offline_maps.py`` with no prte available exits
77; with one available it discovers ``test-topo.xml`` from
``test/topologies/`` and (no cases yet) prints the discovered set and
exits 0.

Phase 1 — Invocation and success/error detection
------------------------------------------------

**Goal:** build a ``prterun`` command line from a minimal ``Case`` and
classify the result.

- ``run_case()`` constructs::

      <prterun> --rtos donotlaunch --display map \
                --prtemca hwloc_use_topo_file <topo> \
                -H <hostspec> [directive args] -n <N> hostname [: ...apps]

  capturing stdout+stderr and the exit code.
- Classify per SPEC `Success / failure detection`:

  - ``mapped`` iff exit == 0 **and** exactly one ``JOB MAP`` block is
    present;
  - otherwise capture the framed error banner (the dash-delimited
    paragraph; recognize ``Valid directives:`` / ``not supported`` /
    ``Please check for a typo``).

- Unit-check by hand against a few live invocations: a good map, a bad
  ``--bind-to`` token (exit 213, banner), a default-only modifier
  rejection.

**Done when:** a hardcoded good case is classified ``mapped`` and a
hardcoded bad token is classified as a rejection with its banner.

Phase 2 — Normalization and map parser
--------------------------------------

**Goal:** turn raw map text into ``ParsedMap`` and a stable normalized
string.

- ``normalize()`` removes, in order: the ``libxml`` version warning
  line; the trailing DONOTLAUNCH "headnode architecture" paragraph; the
  ``Total slots allocated <S>`` number; and the ``<jobid>`` token
  (``prterun-<host>-<pid>@<n>`` → fixed placeholder). Keep everything
  else verbatim and trailing-whitespace-trimmed for golden stability.
- ``parse_map()`` extracts:

  - header: ``mapping_policy`` + ``mapping_mods`` (split on ``:``),
    ``ranking_policy``, ``binding_policy``, ``cpu_type``;
  - per node: ``name``, ``num_slots``, ``max_slots``, ``num_procs``, and
    an ordered ``procs`` list;
  - per proc: ``app_idx``, ``rank``, and a parsed ``Bound`` —
    ``N/A`` → unbound; ``<obj>[<os_index>][<child>:L<lo>[-<hi>]]`` →
    structured ``BoundSpec`` with ``obj``, ``os_index``, ``child_level``,
    ``lo``, ``hi``.

- Define ``ParseError`` for malformed/missing blocks; treat as a harness
  bug surfaced loudly (not a silent FAIL of the case).

**Done when:** parsing the four sample outputs already captured in the
spec round-trips into the expected structures, and ``normalize()`` of two
runs of the same case is byte-identical.

Phase 3 — Topology model
------------------------

**Goal:** derive everything the invariants need from any topology XML,
with zero hardcoded dimensions.

- ``TopoModel.from_xml()`` walks the hwloc XML
  (``xml.etree.ElementTree``), recording each ``object`` with its level,
  ``os_index``, and ``cpuset``. From the nesting/cpusets compute, for
  every Package/NUMANode/L3/L2/L1Cache/Core/PU object, the set of logical
  **core** ids and **PU** ids it contains.
- Derive ``pus_per_core`` (== total PU / total Core) and expose
  ``hwthread_eq_core``.
- Provide ``objects_at(level)`` ordered by logical index and
  ``span_of(obj)`` returning the core-id range/set, so the binding
  invariants can compute an object's expected ``Bound`` span for *this*
  topology.
- Cross-check (warn-only): if parsed counts differ from a recorded note
  for a known file, emit one warning and proceed with parsed values.
  (No assertion; the harness must run unchanged on a regenerated file.)

**Done when:** ``TopoModel.from_xml('test/topologies/test-topo.xml')``
reports the actual object counts, ``pus_per_core == 1``, and
``span_of(package[0])`` equals that package's true core range — all read
from the file.

Phase 4 — Case model and generation
-----------------------------------

**Goal:** declaratively enumerate the matrix and the special groups,
crossed with the topology set.

- ``Case`` fields: ``id``, ``group``, ``topo`` (TopoModel),
  ``map_by``, ``rank_by``, ``bind_to``, ``layout`` (name + hostspec),
  ``n``, ``apps`` (for multi-app), ``extra_args``, ``expect``
  (``"map"`` | ``"reject"``), ``expected_banner`` (substring|None).
- Layouts: ``single`` (``node0:8``), ``even``
  (``node0:8,node1:8,node2:8``), ``uneven`` (``node0:8,node1:4``).
  Layout slot counts are topology-independent.
- ``generate_cases()``:

  - **blanket matrix**: 9 map-by × 4 rank-by × 8 bind-to over the
    ``even`` and ``uneven`` layouts at two ``N`` values each
    (one < slots, one forcing wrap), all ``expect="map"``;
  - **groups** (each its own generator): ``oversubscribe``,
    ``span``/``PE=n``, ``ppr``, ``seq``/``rankfile`` (reading inputs
    from ``inputs/``), ``multi-app``, ``negative`` (``expect="reject"``
    with the expected banner substring);
  - cross the entire pattern set with every ``TopoModel`` discovered in
    ``test/topologies/`` (one ``TopoModel`` per ``*.xml``); case ids
    include the topology name (basename stem) so goldens key uniquely.

- Honor ``--filter <glob>`` and ``--topo/--topo-set`` here.

**Done when:** ``generate_cases()`` over the discovered topologies yields
the expected count (``288 × layouts × Ns`` per topology for the matrix,
plus the groups, times the number of ``*.xml`` in ``test/topologies/``)
and ids are unique and stable. Adding a second XML to that directory
doubles the generated cases with no code change.

Phase 5 — Invariant engine
--------------------------

**Goal:** implement every invariant from SPEC `Validation strategy`,
all expressed against ``ParsedMap`` + ``TopoModel``.

- Universal: U1–U7 (policy-string match, total == N, rank set =
  {0..N-1}, per-node count consistency, valid App indices, slot/
  oversubscribe rule, well-formed bound referencing existing objects).
- Mapping shape by map-by: M-node, M-slot, M-<obj> — using
  ``objects_at()`` cycle lengths derived from the topology.
- Ranking by rank-by: R-slot, R-node, R-span, R-fill — as properties of
  the ``rank → (node, placement-slot)`` relation.
- Binding by bind-to: B-none, B-core/B-hwthread (respecting
  ``hwthread_eq_core``), B-<obj> (compare displayed span to
  ``span_of()``), B-locality.
- Each checker returns ``[Violation(case_id, inv_id, message)]``.
- For ``expect="reject"`` cases, the only check is that the run was a
  rejection and (if given) the banner substring matched.

**Done when:** the full matrix runs against the committed topology with
zero unexplained violations; any violation that *is* a real PRRTE bug is
recorded as a follow-up rather than worked around.

Phase 6 — Reporting and CLI
---------------------------

**Goal:** usable developer/CI ergonomics and the SPEC exit-code policy.

- Per-case ``PASS <id>`` / ``FAIL <id> <inv-id>: <detail>``; on FAIL
  echo the exact command line and the normalized map or banner.
- Trailing summary: pass/fail/skip counts and wall time.
- Flags: ``--verbose`` (echo every command + parsed structure),
  ``--filter <glob>``, ``--topo``/``--topo-set``, ``--list`` (print case
  ids without running), and the Phase-7 golden flags.
- Exit ``0`` all-pass, ``1`` any-fail, ``77`` prerequisites missing.

**Done when:** a deliberately injected wrong expectation produces a FAIL
with a copy-pasteable repro, and a clean run exits 0.

Phase 7 — Golden snapshots (Layer 2)
------------------------------------

**Goal:** regression pinning for a curated subset.

- Add ``--golden`` (compare) and ``--update-golden`` modes.
- Golden path: ``golden/<topo-name>/<case-id>.map`` holding normalized
  output. Compare = unified diff; mismatch is a FAIL.
- Curate the golden subset (a few dozen representative cases incl.
  uneven layouts and each group); generate once, **human-review**, commit.
- ``--update-golden`` rewrites them and prints the diff for review;
  document that regeneration requires inspecting the diff before commit.

**Done when:** ``--golden`` passes against freshly generated, reviewed
goldens, and a deliberate placement change shows up as a golden diff.

Phase 8 — Build integration
---------------------------

**Goal:** ``make check`` runs the harness, skipping cleanly where it
cannot.

- Add ``test/offline/Makefile`` to ``config/prte_config_files.m4``.
- Add ``offline`` to ``SUBDIRS`` in ``test/Makefile.am``.
- Create ``test/offline/Makefile.am``:

  - ``TESTS = run_offline_maps.py`` (or a thin ``run-offline.sh`` wrapper
    that locates ``python3`` and execs the driver, for hosts where the
    ``.py`` is not directly executable under Automake);
  - ``AM_TESTS_ENVIRONMENT`` / ``TESTS_ENVIRONMENT`` exporting
    ``top_builddir`` and ``top_srcdir`` so the driver can find both the
    built ``prte`` and the topology files;
  - ``EXTRA_DIST`` = the driver, ``cases/``, ``inputs/``, ``golden/``,
    ``README.rst``, ``SPEC.rst``, this plan. (Topology XMLs are dist'd by
    ``test/topologies/Makefile.am`` from Phase R, not here.)

- Re-run ``./autogen.pl`` (required after editing ``*.m4`` /
  ``Makefile.am``); confirm ``configure`` regenerates and
  ``test/offline/Makefile`` appears.
- Confirm the skip path: on a build without the topology-file MCA
  parameter, the test reports SKIP (77), not FAIL.

**Done when:** ``make check`` from a clean build tree runs the harness
and reports its result through the Automake test summary.

Phase 9 — Bring-up and triage
-----------------------------

**Goal:** the harness is green (or every red is a filed PRRTE issue, not
a harness defect).

- Run the full matrix against the current build; categorize each FAIL as
  (a) harness bug → fix, or (b) genuine mapper/binding discrepancy →
  record with the repro command for a separate fix, and mark the case
  ``xfail`` with a reference rather than deleting it.
- Capture the curated golden set once green.
- Note wall-clock; if the full cross-product is too slow for routine
  ``make check``, gate the blanket matrix behind an env flag
  (e.g. ``PRTE_OFFLINE_FULL=1``) and keep a fast representative subset as
  the default ``TESTS`` run.

**Done when:** ``make check`` is green and the slow/full mode is
documented in ``README.rst``.

Task checklist
==============

Status as implemented: the matrix, the oversubscribe / ppr / multi-app /
negative groups, all invariants, golden snapshots, and the build wiring
are done and green (584 cases default, 1736 with ``--full``).  The
``span``/``PE=n`` and ``seq``/``rankfile`` groups are deferred (see the
note at the end).

Phase R — Relocate shared topology

- [x] ``git mv`` ``test-topo.xml`` → ``test/topologies/``
- [x] ``test/topologies/Makefile.am`` (``EXTRA_DIST``); drop it from
      ``test/unit/rmaps/Makefile.am``
- [x] wire ``test/topologies/Makefile`` (m4 + ``test/Makefile.am``
      SUBDIRS); ``autogen.pl``
- [x] update path in ``CLAUDE.md`` (2) and
      ``docs/plans/per_app_mapping/`` (2)
- [x] verify dist + CLAUDE.md command + rmaps unit tests unaffected

Phase 0 — Scaffolding

- [x] ``README.rst``, ``cases/`` ``inputs/`` ``golden/`` placeholders
- [x] ``run_offline_maps.py`` skeleton + ``locate_prterun()`` +
      ``discover_topologies()`` (glob ``test/topologies/*.xml``) + skip(77)

Phase 1 — Invocation

- [x] ``run_case()`` builds argv and captures rc/stdout/stderr
- [x] success/error classification (one JOB MAP; banner capture)

Phase 2 — Parsing

- [x] ``normalize()`` (libxml warn, DONOTLAUNCH para, slots, jobid)
- [x] ``parse_map()`` → ``ParsedMap`` incl. ``BoundSpec``

Phase 3 — Topology model

- [x] ``TopoModel.from_xml()`` counts, spans, ``pus_per_core``
      (cpuset words are MSW-first; empty fields are zero words)
- [x] ``objects_at()`` / ``spans_at()`` / ``hwthread_eq_core``

Phase 4 — Cases

- [x] ``Case`` + layouts + ``generate_cases()`` matrix (288 combos)
- [x] groups: oversubscribe, ppr, multi-app, negative
- [ ] groups: span/PE=n, seq/rankfile (deferred — see note)
- [x] cross with topology set; ids include topo name

Phase 5 — Invariants

- [x] universal U1–U5, U7 (U6 n/a: default policy oversubscribes)
- [x] mapping M-node / M-slot / M-<obj> (object maps are node-local)
- [x] ranking R-slot / R-node (R-fill / R-span pinned by golden)
- [x] binding B-none / B-core·hwthread / B-<obj> (spans from topology)
- [x] reject-case checking (must-map-by-obj; bind-above-map; bad tokens)

Phase 6 — Reporting/CLI

- [x] PASS/FAIL lines + repro on fail + summary
- [x] ``--verbose`` / ``--filter`` / ``--topo`` / ``--topo-dir`` / ``--list``
- [x] exit-code policy 0/1/77

Phase 7 — Golden

- [x] ``--golden`` compare + ``--update-golden`` (curated subset only)
- [x] curate (16 cases), generate, commit subset

Phase 8 — Build integration

- [x] ``config/prte_config_files.m4`` + ``test/Makefile.am`` SUBDIRS
- [x] ``test/offline/Makefile.am`` (TESTS, env, EXTRA_DIST)
- [x] ``autogen.pl`` + verify configure/Makefile; verify SKIP path

Phase 9 — Bring-up

- [x] triaged matrix; no genuine discrepancies — two surprising-but-real
      behaviors encoded as expected (bind-above-map rejects; object maps
      are node-local without ``:SPAN``)
- [x] captured goldens; default fast (584, ~13s) vs ``--full`` (1736,
      ~40s) documented in ``README.rst``

Deferred groups
---------------

``span``/``PE=n`` and ``seq``/``rankfile`` are not yet generated.  They
carry extra preconditions that need their own calibration before they can
be asserted without flakiness — e.g. ``--map-by core:PE=2`` is *rejected*
unless the mapped object has at least the requested cpus-per-proc, and
``seq``/``rankfile`` need committed input files under ``inputs/`` with
hand-derived expected placements.  They are left as follow-ups rather than
shipped half-calibrated.

Risks and open questions
========================

- **Genuine mapper discrepancies vs. harness bugs.** Bring-up (Phase 9)
  must distinguish them; the plan resolves real PRRTE bugs as filed
  issues + ``xfail``, never by loosening an invariant to hide them.
- **Runtime cost.** 288 × layouts × Ns × topologies plus groups may be
  too slow for default ``make check``; mitigation is the fast-subset
  default with a full-mode env flag (Phase 9).
- **Output-format drift.** All checks run on parsed structure, but the
  parser and ``normalize()`` are the coupling points to PRRTE's display
  format; if that format changes, only those two units (and goldens)
  need updating — by design.
- **``prterun`` discovery in the build tree.** Relies on creating a
  ``prterun`` symlink next to the built ``prte``; confirm Automake test
  env exports ``top_builddir`` and that creating the symlink is benign in
  parallel ``make -j`` test runs (create-if-absent, idempotent).
