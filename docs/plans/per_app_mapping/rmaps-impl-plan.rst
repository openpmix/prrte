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
2. Add ``prte_rmaps_base_compute_nprocs()`` call per app (extract from existing
   process-count logic if not already a helper, or call the existing macro inline).
3. Add the ``any_per_app`` scan.
4. Replace the single-dispatch block with the per-app loop when ``any_per_app`` is true.
5. Handle the pre-loop promotion rules: display-map directive on any app → promote to
   job level; conflicting ``INHERIT``/``NOINHERIT`` → abort.

File:

- ``src/mca/rmaps/base/rmaps_base_map_job.c``

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
5. **Schizo MPMD parsing** — the ``:`` MPMD separator logic in schizo must correctly
   associate each ``--map-by``/``--rank-by``/``--bind-to`` with its app context index, not
   blindly with the last-seen app.
