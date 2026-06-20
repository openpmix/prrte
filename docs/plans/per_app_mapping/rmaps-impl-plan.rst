Per-App-Context Mapping: Implementation Plan
=============================================

Overview
--------

This plan describes the implementation of per-app-context mapping, ranking, and binding
policies for PRRTE as specified in ``per-app-mapping.rst``.  The work adds nine new
``PRTE_APP_*`` attribute keys, extends ``prte_rmaps_options_t`` with an ``app_idx`` field,
replaces the single-dispatch path in ``prte_rmaps_base_map_job()`` with a per-app loop
when any app carries per-app directives, and bumps the rmaps framework version from
4.0.0 to 5.0.0.

Current State
-------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Item
     - Current value
   * - Last ``PRTE_APP_*`` key
     - ``PRTE_APP_PPR = 25``
   * - ``PRTE_APP_MAX_KEY``
     - ``100`` (no change required)
   * - ``prte_rmaps_options_t`` fields
     - no ``app_idx``; no ``dist_device``
   * - ``compute_vpids`` signature
     - ``(prte_job_t *, prte_rmaps_options_t *)``
   * - rmaps framework version
     - ``PRTE_RMAPS_BASE_VERSION_4_0_0``
   * - Module struct
     - ``prte_rmaps_base_module_4_0_0_t``
   * - ``test/`` in top-level ``SUBDIRS``
     - no
   * - ``config/prte_config_files.m4`` entries for test/
     - none

Phases
------

Phase 1 — Data model (no logic change)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** All new attributes and the ``app_idx`` field exist; the codebase compiles
cleanly with no behaviour change.

Files:

- ``src/util/attr.h`` — add keys 26–34
- ``src/mca/rmaps/rmaps_types.h`` — add ``app_idx`` to ``prte_rmaps_options_t``; add
  ``PRTE_RMAPS_BASE_VERSION_5_0_0`` macro and keep ``4_0_0`` as deprecated alias
- ``src/mca/rmaps/rmaps.h`` — add ``prte_rmaps_base_module_5_0_0_t`` struct/typedef;
  keep ``prte_rmaps_base_module_t`` typedef pointing at the new struct

No functional change.  All existing code still compiles because the new ``app_idx`` field
is initialised to ``-1`` (map all apps) and no component reads it yet.

Phase 2 — Parsing functions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** The three new ``prte_rmaps_base_set_app_*_policy()`` functions exist,
parse correctly, and store results into ``app->attributes``.  The forbidden-modifier
guards (``OVERSUBSCRIBE``, ``NOOVERSUBSCRIBE``, ``INHERIT``, ``NOINHERIT``) are enforced.
``NOLOCAL`` is stored as a directive bit in ``PRTE_APP_MAPBY``.

Files:

- ``src/mca/rmaps/base/rmaps_base_frame.c`` — add the three parsing functions
- ``src/mca/rmaps/base/base.h`` — declare them

These functions mirror the existing ``prte_rmaps_base_set_mapping_policy()`` /
``check_modifiers()`` logic but write into ``app->attributes`` instead of ``jdata->map``.

Phase 3 — ``prte_rmaps_base_map_job()`` per-app dispatch
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** When at least one app carries ``PRTE_APP_MAPBY``, ``PRTE_APP_RANKBY``, or
``PRTE_APP_BINDTO``, the function enters a per-app loop instead of the single-dispatch
path.  Otherwise existing behaviour is unchanged.

Sub-steps (in order):

1. Add ``prte_rmaps_base_resolve_app_options()`` as a static helper — builds an
   ``app_options`` struct from the job-level ``options`` plus per-app attribute overrides.
   It must: (a) mask the ``PRTE_APP_MAPBY``/``RANKBY``/``BINDTO`` directive bits off when
   writing ``opts->map``/``rank``/``bind`` (route overload to ``opts->overload``);
   (b) refresh the map-derived fields (``maptype``/``mapdepth``/``mapspan``/``ordered``)
   when the app overrides its map; and (c) **default ranking and binding from the app's
   own map** when no explicit per-app ``--rank-by``/``--bind-to`` is given — via small
   pure helpers ``prte_rmaps_base_derive_ranking()`` / ``derive_binding()`` — rather than
   inheriting the job-level ranking/binding.
2. Add ``prte_rmaps_base_compute_nprocs()`` call per app (extract from existing
   process-count logic if not already a helper, or call the existing macro inline).
3. Add the ``any_per_app`` scan.
4. Replace the single-dispatch block with the per-app loop when ``any_per_app`` is true.
5. Handle the pre-loop promotion rules: display-map directive on any app → promote to
   job level; conflicting ``INHERIT``/``NOINHERIT`` → abort.

File:

- ``src/mca/rmaps/base/rmaps_base_map_job.c``

Phase 3a — per-app attribute plumbing (CLI → attributes)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** The per-app ``PRTE_APP_*`` attributes actually reach ``map_job``.  This
plumbing largely already exists, but two things are required for it to work:

1. The ``prte_rmaps_base_set_app_*_policy()`` helpers must store every ``PRTE_APP_*``
   attribute with **``PRTE_ATTR_GLOBAL``**, not ``PRTE_ATTR_LOCAL``.  ``LOCAL``
   attributes are not packed and are silently dropped when the spawn request is relayed
   to the DVM master, so per-app directives stored as ``LOCAL`` never reach ``map_job``
   and ``any_per_app`` is always false.  This is the single most important correctness
   fix in the whole feature and produces no error when wrong — only silent fallback to
   the job-level policy.
2. The spawn-assembly loops (``src/prted/prte.c`` and ``src/prted/prun_common.c``) must
   convert each ``pmix_app_t.info`` from that app's own ``app->info`` list, not from the
   job-level info.

Files:

- ``src/mca/rmaps/base/rmaps_base_frame.c`` (``LOCAL`` → ``GLOBAL`` in the three setters)
- ``src/prted/prte_app_parse.c``, ``src/prted/pmix/pmix_server_dyn.c`` (already present)
- ``src/prted/prte.c``, ``src/prted/prun_common.c`` (per-app info conversion)

Phase 4 — Ranking with ``app_idx``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** ``prte_rmaps_base_compute_vpids()`` accepts an ``app_idx`` parameter so the
per-app loop can call it once per app.

Files:

- ``src/mca/rmaps/base/rmaps_base_ranking.c`` — add ``app_idx`` parameter; when ≥ 0
  process only the matching app context; global rank counter is passed between calls
  so vpids remain contiguous
- ``src/mca/rmaps/base/base.h`` — update declaration
- ``src/mca/rmaps/base/rmaps_base_map_job.c`` — update all call sites

Phase 5 — Component ``app_idx`` guards
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** Each of the five components skips app contexts that do not match
``options->app_idx`` when that field is ≥ 0.

Files (one change each — add the ``app_idx`` guard to the inner app-context loop):

- ``src/mca/rmaps/round_robin/rmaps_rr.c``
- ``src/mca/rmaps/ppr/rmaps_ppr.c`` — also remove the duplicate per-app PPR/PES
  override that is now handled centrally by ``prte_rmaps_base_resolve_app_options()``
- ``src/mca/rmaps/seq/rmaps_seq.c`` — also check ``PRTE_APP_MAP_FILE`` before
  ``PRTE_JOB_FILE``
- ``src/mca/rmaps/rank_file/rmaps_rank_file.c`` — same ``MAP_FILE`` fallback
- ``src/mca/rmaps/lsf/rmaps_lsf.c``

Also update each component file to reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``.

Phase 6 — Schizo / CLI wiring
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** MPMD per-app ``--map-by``, ``--rank-by``, ``--bind-to`` arguments call the new
``prte_rmaps_base_set_app_*`` functions instead of the job-level variants.  PMIx spawn
path maps ``PMIX_MAPBY``/``PMIX_RANKBY``/``PMIX_BINDTO`` in per-app ``info[]`` arrays to the
new attributes.

Files:

- ``src/mca/schizo/prte/schizo_prte.c``
- ``src/mca/schizo/base/schizo_base_stubs.c`` (and ``base.h``)
- ``src/tools/prun/prun.c``
- ``src/prted/prte.c``

Relax the tool-level argv pre-scan guards
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Before schizo ever sees the command line, both ``prun`` (``src/tools/prun/prun.c``,
``prte_prun()``) and the ``prte`` HNP launcher (``src/prted/prte.c``, ``main()``) walk
the raw argv to normalise option spellings (``--rank-by`` → ``--rankby``,
``--bind-to`` → ``--bindto``).  In doing so they currently track ``rankby_found`` /
``bindto_found`` booleans and **reject a second occurrence** of ``--rank-by`` or
``--bind-to`` with the ``help-schizo-base.txt`` ``multi-instances`` error.

This guard makes per-app ranking and binding impossible: an MPMD line such as

.. code-block:: sh

   prun app1 --rank-by node : app2 --rank-by slot

legitimately repeats ``--rank-by`` once per app context and trips the guard before
the per-app machinery is ever reached.  Note ``--map-by`` already has **no** such
guard — it is renamed unconditionally on every occurrence — which is exactly why
per-app ``--map-by`` already works while ``--rank-by``/``--bind-to`` do not.

The fix is to make ``--rank-by`` and ``--bind-to`` behave like ``--map-by`` in this
pre-scan: drop the ``rankby_found`` / ``bindto_found`` tracking and the
``multi-instances`` rejection, and rename every occurrence unconditionally.
Distinguishing a legitimate per-app repeat from an erroneous duplicate
job-level option is the schizo MPMD parser's job (it associates each option with
its app context by ``:`` separator), not the argv pre-scan's — the pre-scan
cannot tell the two apart and must not try.

The two pre-scan loops in ``prun`` and ``prte`` are exact copies of each other, so
rather than relaxing each in place the shared logic is factored into one helper,
``prte_schizo_base_normalize_argv()`` (``src/mca/schizo/base/schizo_base_stubs.c``,
declared in ``src/mca/schizo/base/base.h``).  It normalises all four deprecated
spellings (``--map-by``, ``--rank-by``, ``--bind-to``, ``--runtime-options``) in
place and returns any ``--personality`` value found.  Both tools replace their
inline loop with a single call to it.

Phase 7 — Build system wiring
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Goal:** ``make check`` builds and runs the unit test suite.

Files:

- ``Makefile.am`` — add ``test`` to ``SUBDIRS`` after ``src``
- ``config/prte_config_files.m4`` — add ``test/Makefile``, ``test/unit/Makefile``,
  ``test/unit/rmaps/Makefile`` to ``AC_CONFIG_FILES``
- ``test/Makefile.am`` — new, ``SUBDIRS = unit``
- ``test/unit/Makefile.am`` — new, ``SUBDIRS = rmaps``
- ``test/unit/rmaps/Makefile.am`` — new (see spec for contents)

Phase 8 — Unit tests
~~~~~~~~~~~~~~~~~~~~~

**Goal:** Eight test ``.c`` files exercise the new code paths; ``make check`` passes.

New files under ``test/unit/rmaps/``:

- ``test_rmaps_main.c``
- ``test_policy_parse.c``
- ``test_resolve_options.c``
- ``test_dispatch.c``
- ``test_round_robin.c``
- ``test_ppr.c``
- ``test_seq.c``
- ``test_rank_file.c``

Dependency Graph
----------------

.. code-block:: text

   Phase 1 (data model)
     └── Phase 2 (parsing functions)
           └── Phase 3 (map_job dispatch)
                 ├── Phase 4 (ranking app_idx)   ← depends on Phase 3 call sites
                 └── Phase 5 (component guards)  ← depends on Phase 1 for app_idx field
   Phase 6 (schizo) ← depends on Phase 2
   Phase 7 (build)  ← independent, can be done any time
   Phase 8 (tests)  ← depends on all prior phases

Phases 1–5 must be done in order.  Phase 6 can proceed in parallel with Phases 3–5
once Phase 2 is done.  Phase 7 can be done any time before Phase 8.

Key Design Decisions (from spec)
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Decision
     - Consequence
   * - ``OVERSUBSCRIBE``/``NOOVERSUBSCRIBE`` are job-level only
     - ``set_app_mapping_policy()`` must reject them with ``PRTE_ERR_BAD_PARAM``
   * - ``INHERIT``/``NOINHERIT`` are job-level only
     - Same rejection; map_job promotes them if consistent, aborts if conflicting
   * - ``NOLOCAL`` is per-app
     - Stored as directive bit in ``PRTE_APP_MAPBY`` uint16_t
   * - ``display_map`` is job-level only
     - Pre-loop promotion: any app with display-map attr → set on jdata
   * - ``PRTE_APP_MAP_FILE`` vs ``PRTE_JOB_FILE``
     - seq/rank_file check app attr first, fall back to job attr
   * - Existing ``PRTE_APP_PPR``/``PRTE_APP_PES_PER_PROC``
     - Backward-compatible: still read by ppr component; ``PRTE_APP_MAPBY`` with ``ppr:`` also writes ``PRTE_APP_PPR``
   * - Global vpid assignment
     - Monotonically increasing across apps; per-app ranking controls intra-app order only
   * - ``app_idx`` init
     - ``memset`` sets to 0; must add explicit ``options.app_idx = -1`` after memset in map_job

Attribute Mapping: ``prte_rmaps_options_t`` Field ↔ ``PRTE_APP_*`` Key
------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 35 20

   * - ``opts`` field
     - New attribute key
     - PMIx type
   * - ``opts->map``
     - ``PRTE_APP_MAPBY`` (26)
     - ``PMIX_UINT16``
   * - ``opts->rank``
     - ``PRTE_APP_RANKBY`` (27)
     - ``PMIX_UINT16``
   * - ``opts->bind``
     - ``PRTE_APP_BINDTO`` (28)
     - ``PMIX_UINT16``
   * - ``opts->cpus_per_rank``
     - ``PRTE_APP_PES_PER_PROC`` (24)
     - ``PMIX_UINT16``
   * - ``opts->cpuset``
     - ``PRTE_APP_CPUSET`` (33)
     - ``PMIX_STRING``
   * - map/rankfile path
     - ``PRTE_APP_MAP_FILE`` (29)
     - ``PMIX_STRING``
   * - dist device name
     - ``PRTE_APP_DIST_DEVICE`` (30)
     - ``PMIX_STRING``
   * - ``opts->use_hwthreads``
     - ``PRTE_APP_HWT_CPUS`` (31)
     - ``PMIX_BOOL``
   * - (core cpus flag)
     - ``PRTE_APP_CORE_CPUS`` (32)
     - ``PMIX_BOOL``
   * - ``opts->limit``
     - ``PRTE_APP_BINDING_LIMIT`` (34)
     - ``PMIX_UINT16``

Note: ``prte_rmaps_options_t`` does not currently have a ``dist_device`` string field.
``resolve_app_options()`` will need to read ``PRTE_APP_DIST_DEVICE`` and store it on the
options struct.  Either add a ``char *dist_device`` field to ``prte_rmaps_options_t`` in
Phase 1, or handle it by reading the attribute directly inside the dist component if
one exists.  Given no ``dist`` component exists in the current tree, this attribute can
be defined but left wired-up-later; ``resolve_app_options()`` reads it and sets a new
``opts->dist_device`` field.

Risk Areas
----------

1. **``compute_vpids`` signature change** — every call site must be updated.  Search for
   all callers before changing the signature.
2. **``app_idx`` init to ``-1`` vs zero** — ``memset`` zeroes the struct, so ``app_idx = 0``
   without the explicit assignment would silently map only app[0].  The explicit
   ``options.app_idx = -1`` assignment is mandatory and easy to miss.
3. **Node list statefulness with ``NOLOCAL``** — the dedicated unit test (see spec
   §"NOLOCAL on app[0] with shared HNP node") must pass before the branch is
   considered correct.  If ``prte_rmaps_base_get_target_nodes()`` modifies shared
   node state, the per-app loop will produce incorrect results for subsequent apps.
4. **ppr component duplicate logic** — the ppr component currently reads
   ``PRTE_APP_PPR`` and ``PRTE_APP_PES_PER_PROC`` directly (rmaps_ppr.c lines 166, 171).
   In Phase 5 this must be removed and the values must come through
   ``resolve_app_options()`` to avoid double-application.
5. **Per-app MPMD parsing** — the ``:`` separator logic must associate each
   ``--map-by``/``--rank-by``/``--bind-to`` with its app context.  This already happens in
   ``src/prted/prte_app_parse.c`` (each app segment is parsed in its own ``create_app()``
   call), so no schizo MPMD bookkeeping is needed — but the spawn-assembly loops must
   convert each ``pmix_app_t.info`` from that app's own ``app->info``.
6. **``PRTE_ATTR_LOCAL`` vs ``GLOBAL`` (highest-risk, silent failure)** — the
   ``set_app_*_policy`` helpers must store every ``PRTE_APP_*`` attribute as
   ``PRTE_ATTR_GLOBAL``.  ``LOCAL`` attributes are dropped when the spawn request is
   packed and relayed to the DVM master, so per-app directives stored as ``LOCAL`` never
   reach ``map_job``; ``any_per_app`` stays false and every app falls back to the
   job-level policy with **no error**.  A single-app test masks this (its directive
   equals the job policy); always verify with a multi-app MPMD case.
7. **Directive-bit masking in ``resolve_app_options``** — the ``PRTE_APP_*`` attributes
   carry directive bits (``GIVEN``, overload, ``IS_SET``) in their high bits.  Assigning
   the raw value to ``opts->map``/``rank``/``bind`` breaks the bare-enum comparisons in
   the components and ``compute_vpids``.  Mask with ``PRTE_GET_*_POLICY`` and route the
   overload bit to ``opts->overload``.

Verification
------------

Use the offline ``prterun --rtos donotlaunch --display map`` technique (see the spec
§"Offline end-to-end verification" and ``AGENTS.md``) to confirm placement, ranks, and
bindings for both single-app and **multi-app MPMD** cases before relying on the unit
tests.  The multi-app case is what catches risks 6 and 7.
