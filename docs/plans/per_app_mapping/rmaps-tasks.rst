Per-App-Context Mapping: Implementation Tasks
=============================================

Tasks are ordered by dependency.  Complete each phase before starting the next.
Mark tasks ``[x]`` as they are done.

Phase 1 — Data Model
---------------------

T1.1 — Add new ``PRTE_APP_*`` attribute keys (``src/util/attr.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After ``PRTE_APP_PPR = 25`` (line 70), add:

.. code-block:: c

   #define PRTE_APP_MAPBY              26  /* uint16_t mapping policy enum */
   #define PRTE_APP_RANKBY             27  /* uint16_t ranking policy enum */
   #define PRTE_APP_BINDTO             28  /* uint16_t binding policy enum */
   #define PRTE_APP_MAP_FILE           29  /* char* path to seq or rankfile */
   #define PRTE_APP_DIST_DEVICE        30  /* char* dist device name */
   #define PRTE_APP_HWT_CPUS           31  /* bool: use hwthreads as CPUs */
   #define PRTE_APP_CORE_CPUS          32  /* bool: use cores as CPUs */
   #define PRTE_APP_CPUSET             33  /* char* comma-delimited CPU ranges */
   #define PRTE_APP_BINDING_LIMIT      34  /* uint16_t max procs per binding target */

``PRTE_APP_MAX_KEY`` is already 100; no change needed.

- [ ] Add keys 26–34 immediately after the ``PRTE_APP_PPR`` line

T1.1b — Add pretty-print cases to ``src/util/attr.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the ``case PRTE_APP_PPR:`` block (currently the last APP case, around line 281),
add:

.. code-block:: c

           case PRTE_APP_MAPBY:
               return "PRTE_APP_MAPBY";
           case PRTE_APP_RANKBY:
               return "PRTE_APP_RANKBY";
           case PRTE_APP_BINDTO:
               return "PRTE_APP_BINDTO";
           case PRTE_APP_MAP_FILE:
               return "PRTE_APP_MAP_FILE";
           case PRTE_APP_DIST_DEVICE:
               return "PRTE_APP_DIST_DEVICE";
           case PRTE_APP_HWT_CPUS:
               return "PRTE_APP_HWT_CPUS";
           case PRTE_APP_CORE_CPUS:
               return "PRTE_APP_CORE_CPUS";
           case PRTE_APP_CPUSET:
               return "PRTE_APP_CPUSET";
           case PRTE_APP_BINDING_LIMIT:
               return "PRTE_APP_BINDING_LIMIT";

- [ ] Add nine pretty-print cases to ``src/util/attr.c``

T1.2 — Add ``app_idx`` to ``prte_rmaps_options_t`` (``src/mca/rmaps/rmaps_types.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the ``prte_rmaps_options_t`` struct (ends around line 120), add after the last field:

.. code-block:: c

       /* When >= 0, map only the app context at this index in jdata->apps.
        * When < 0 (default -1), map all app contexts as today. */
       int app_idx;

Also add a ``dist_device`` string field (needed by ``resolve_app_options``):

.. code-block:: c

       char *dist_device;  /* device name for dist mapping, from PRTE_APP_DIST_DEVICE */

- [ ] Add ``app_idx`` field to ``prte_rmaps_options_t``
- [ ] Add ``dist_device`` field to ``prte_rmaps_options_t``

T1.3 — Add version 5.0.0 macro (``src/mca/rmaps/rmaps_types.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing ``PRTE_RMAPS_BASE_VERSION_4_0_0`` definition (line 127), add:

.. code-block:: c

   #define PRTE_RMAPS_BASE_VERSION_5_0_0 PRTE_MCA_BASE_VERSION_3_0_0("rmaps", 5, 0, 0)
   /* deprecated alias — out-of-tree components get a version mismatch rather than
    * a silent ABI violation */
   #undef  PRTE_RMAPS_BASE_VERSION_4_0_0
   #define PRTE_RMAPS_BASE_VERSION_4_0_0 PRTE_RMAPS_BASE_VERSION_5_0_0

- [ ] Add ``PRTE_RMAPS_BASE_VERSION_5_0_0``
- [ ] Redefine ``PRTE_RMAPS_BASE_VERSION_4_0_0`` as a deprecated alias

T1.4 — Add module struct 5.0.0 (``src/mca/rmaps/rmaps.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing ``prte_rmaps_base_module_4_0_0_t`` typedef (line 84), add:

.. code-block:: c

   typedef struct prte_rmaps_base_module_4_0_0_t prte_rmaps_base_module_5_0_0_t;

Update the convenience alias on line 86:

.. code-block:: c

   typedef prte_rmaps_base_module_5_0_0_t prte_rmaps_base_module_t;

- [ ] Add ``prte_rmaps_base_module_5_0_0_t`` typedef
- [ ] Update ``prte_rmaps_base_module_t`` alias

T1.5 — Update all five component files to reference version 5.0.0
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the component struct initializer in each file, change ``PRTE_RMAPS_BASE_VERSION_4_0_0``
to ``PRTE_RMAPS_BASE_VERSION_5_0_0``:

- [ ] ``src/mca/rmaps/round_robin/rmaps_rr_component.c`` (line 47)
- [ ] ``src/mca/rmaps/ppr/rmaps_ppr_component.c`` (line 37)
- [ ] ``src/mca/rmaps/seq/rmaps_seq_component.c``
- [ ] ``src/mca/rmaps/rank_file/rmaps_rank_file_component.c``
- [ ] ``src/mca/rmaps/lsf/rmaps_lsf_component.c``

T1.6 — Fix ``app_idx`` initialisation in ``prte_rmaps_base_map_job()``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``src/mca/rmaps/base/rmaps_base_map_job.c``: after the ``memset(&options, 0, ...)`` call
(around line 93), add:

.. code-block:: c

   options.app_idx = -1;   /* -1 = map all apps (default) */

Without this the new field is 0 after memset, which would mean "map only app[0]" once
components start honouring it.

- [ ] Add explicit ``options.app_idx = -1`` after the memset

Phase 2 — Parsing Functions
----------------------------

T2.1 — Implement ``prte_rmaps_base_set_app_mapping_policy()`` (``src/mca/rmaps/base/rmaps_base_frame.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Model this on ``prte_rmaps_base_set_mapping_policy()`` (line 448) and the existing
``check_modifiers()`` (line 179).  Key differences:

- Store results into ``app->attributes`` instead of ``jdata->map`` / ``jdata->attributes``.
- Reject ``OVERSUBSCRIBE``, ``NOOVERSUBSCRIBE``, ``INHERIT``, ``NOINHERIT`` — call
  ``pmix_show_help()`` and return ``PRTE_ERR_BAD_PARAM``.
- Accept ``NOLOCAL`` — store the ``PRTE_MAPPING_NO_USE_LOCAL`` directive bit inside
  the ``PRTE_APP_MAPBY`` uint16_t value via ``PRTE_SET_MAPPING_DIRECTIVE``.
- Parse ``ppr:N:obj`` → store ``PRTE_APP_PPR`` (uint16_t N) plus ``PRTE_APP_MAPBY = PRTE_MAPPING_PPR``.
- Parse ``pe=N`` modifier → store ``PRTE_APP_PES_PER_PROC`` (uint16_t N).
- Parse ``hwtcpus`` modifier → store ``PRTE_APP_HWT_CPUS`` (bool true).
- Parse ``corecpus`` modifier → store ``PRTE_APP_CORE_CPUS`` (bool true).
- Parse ``file=path`` modifier → store ``PRTE_APP_MAP_FILE`` (string).
- Parse ``pe-list=ranges`` modifier → store ``PRTE_APP_CPUSET`` (string).
- The final parsed mapping policy enum value is stored as ``PRTE_APP_MAPBY`` (uint16_t)
  via ``prte_set_attribute(&app->attributes, PRTE_APP_MAPBY, PRTE_ATTR_LOCAL, &val, PMIX_UINT16)``.

- [ ] Write ``prte_rmaps_base_set_app_mapping_policy(prte_app_context_t *app, char *spec)``
- [ ] Verify OVERSUBSCRIBE/NOOVERSUBSCRIBE rejected with ``PRTE_ERR_BAD_PARAM``
- [ ] Verify INHERIT/NOINHERIT rejected with ``PRTE_ERR_BAD_PARAM``
- [ ] Verify NOLOCAL directive bit stored in ``PRTE_APP_MAPBY``

T2.2 — Implement ``prte_rmaps_base_set_app_ranking_policy()`` (``src/mca/rmaps/base/rmaps_base_frame.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Model on ``prte_rmaps_base_set_ranking_policy()`` (line 756).  Store the parsed
``prte_ranking_policy_t`` as ``PRTE_APP_RANKBY`` (uint16_t) on ``app->attributes``.

- [ ] Write ``prte_rmaps_base_set_app_ranking_policy(prte_app_context_t *app, char *spec)``

T2.3 — Implement ``prte_rmaps_base_set_app_binding_policy()`` (``src/mca/rmaps/base/rmaps_base_frame.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Model on the existing binding policy setter.  Store the parsed ``prte_binding_policy_t``
as ``PRTE_APP_BINDTO`` (uint16_t) on ``app->attributes``.

- [ ] Write ``prte_rmaps_base_set_app_binding_policy(prte_app_context_t *app, char *spec)``

T2.4 — Declare new functions (``src/mca/rmaps/base/base.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing ``prte_rmaps_base_set_mapping_policy`` declaration (line 131), add:

.. code-block:: c

   PRTE_EXPORT int prte_rmaps_base_set_app_mapping_policy(prte_app_context_t *app,
                                                           char *spec);
   PRTE_EXPORT int prte_rmaps_base_set_app_ranking_policy(prte_app_context_t *app,
                                                           char *spec);
   PRTE_EXPORT int prte_rmaps_base_set_app_binding_policy(prte_app_context_t *app,
                                                           char *spec);

- [ ] Add declarations to ``base.h``

Phase 3 — Per-App Dispatch in ``prte_rmaps_base_map_job()``
------------------------------------------------------------

T3.1 — Implement ``prte_rmaps_base_resolve_app_options()`` (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Add as a ``static int`` function before ``prte_rmaps_base_map_job()``.  Signature:

.. code-block:: c

   static int prte_rmaps_base_resolve_app_options(prte_job_t *jdata,
                                                  prte_app_context_t *app,
                                                  prte_rmaps_options_t *opts)

Logic (all reads from ``app->attributes``; fall through to existing ``opts`` value if
attribute is absent):

1. ``PRTE_APP_MAPBY`` (uint16_t) → ``opts->map``
2. If ``opts->map == PRTE_MAPPING_PPR``: read ``PRTE_APP_PPR`` (uint16_t) → ``opts->pprn``;
   if absent, fall back to ``PRTE_JOB_PPR`` on ``jdata->attributes``.
3. ``PRTE_APP_PES_PER_PROC`` (uint16_t) → ``opts->cpus_per_rank``
4. ``PRTE_APP_HWT_CPUS`` (bool) → ``opts->use_hwthreads``
5. ``PRTE_APP_CPUSET`` (string) → ``opts->cpuset``
6. ``PRTE_APP_MAP_FILE`` (string) → store somewhere accessible; see seq/rank_file notes
7. ``PRTE_APP_DIST_DEVICE`` (string) → ``opts->dist_device``
8. ``PRTE_APP_BINDING_LIMIT`` (uint16_t) → ``opts->limit``
9. ``PRTE_APP_RANKBY`` (uint16_t) → ``opts->rank``
10. ``PRTE_APP_BINDTO`` (uint16_t) → ``opts->bind``
11. The function must not modify ``jdata->map``.

- [ ] Implement ``prte_rmaps_base_resolve_app_options()`` with all ten override steps
- [ ] Verify function does not write to ``jdata->map``

T3.2 — Add ``any_per_app`` scan (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing job-level policy-resolution block (before the single-dispatch
section around line 929), add:

.. code-block:: c

   bool any_per_app = false;
   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) {
           continue;
       }
       if (prte_get_attribute(&app->attributes, PRTE_APP_MAPBY, NULL, PMIX_UINT16) ||
           prte_get_attribute(&app->attributes, PRTE_APP_RANKBY, NULL, PMIX_UINT16) ||
           prte_get_attribute(&app->attributes, PRTE_APP_BINDTO, NULL, PMIX_UINT16)) {
           any_per_app = true;
           break;
       }
   }

- [ ] Add ``any_per_app`` scan

T3.3 — Add pre-loop promotion pass (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``any_per_app`` is true, before entering the per-app loop:

1. **display-map promotion:** scan all apps; if any carries a display-map directive,
   set ``PRTE_JOB_DISPLAY_MAP`` on ``jdata->attributes``.

2. **INHERIT/NOINHERIT promotion:** scan all apps for per-app info-array INHERIT/NOINHERIT
   directives (from PMIx spawn path).  If all agree → promote to job level.  If
   conflicting → ``pmix_show_help()`` + ``PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED)`` + goto cleanup.

- [ ] Implement display-map promotion
- [ ] Implement INHERIT/NOINHERIT conflict detection and promotion

T3.4 — Implement per-app dispatch loop (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Wrap the existing single-dispatch block (around lines 929–954) in an
``if (!any_per_app)`` guard so it runs unchanged for jobs with no per-app directives.

Add an ``else`` branch with:

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) {
           continue;
       }

       prte_rmaps_options_t app_options = options;   /* shallow copy of job defaults */
       app_options.app_idx = n;

       rc = prte_rmaps_base_resolve_app_options(jdata, app, &app_options);
       if (PRTE_SUCCESS != rc) {
           PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
           goto cleanup;
       }

       /* Process count for this app (reuse/extract existing nprocs logic) */
       rc = prte_rmaps_base_compute_nprocs(jdata, app, &app_options);
       if (PRTE_SUCCESS != rc) {
           PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
           goto cleanup;
       }

       did_map = false;
       PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules,
                         prte_rmaps_base_selected_module_t) {
           rc = mod->module->map_job(jdata, &app_options);
           if (PRTE_SUCCESS == rc) {
               did_map = true;
               break;
           }
           if (PRTE_ERR_RESOURCE_BUSY == rc) {
               PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
               goto cleanup;
           }
           /* PRTE_ERR_TAKE_NEXT_OPTION → try next component */
       }
       if (!did_map) {
           pmix_show_help("help-prte-rmaps-base.txt", "failed-map", true, ...);
           PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
           goto cleanup;
       }
   }

Note: ``prte_rmaps_base_compute_nprocs`` — check whether this already exists as a
function or whether the nprocs logic needs to be extracted from the existing
single-dispatch path.  Extract if necessary.

- [ ] Guard existing single-dispatch block with ``if (!any_per_app)``
- [ ] Add per-app loop in ``else`` branch
- [ ] Verify ``app_options.app_idx = n`` is set each iteration
- [ ] Verify oversubscription error path reaches ``MAP_FAILED``

Phase 4 — Ranking with ``app_idx``
------------------------------------

T4.1 — Add ``app_idx`` parameter to ``prte_rmaps_base_compute_vpids()`` (``src/mca/rmaps/base/rmaps_base_ranking.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Current signature (line 109):

.. code-block:: c

   int prte_rmaps_base_compute_vpids(prte_job_t *jdata, prte_rmaps_options_t *options)

New signature:

.. code-block:: c

   int prte_rmaps_base_compute_vpids(prte_job_t *jdata,
                                     prte_rmaps_options_t *options,
                                     int app_idx)

When ``app_idx >= 0``, process only the app context at that index.  The global
rank counter (``vpid``) must be passed in (or stored on ``jdata``) so that successive
per-app calls produce contiguous, non-overlapping vpid ranges.

Implementation options (choose one):

- **Option A:** Add a ``uint32_t next_vpid`` field to ``prte_job_t`` or ``jdata->map`` that
  persists between per-app calls; initialise to 0 before the per-app loop.
- **Option B:** Accept a ``uint32_t *next_vpid`` pointer parameter alongside ``app_idx``
  so the caller tracks it.

Option B is cleaner (no new persistent state on jdata).  Update signature accordingly.

- [ ] Add ``app_idx`` and ``uint32_t *next_vpid`` parameters
- [ ] When ``app_idx >= 0``, restrict processing to that app; use/update ``*next_vpid``
- [ ] When ``app_idx < 0``, function works as today (backward-compatible path)

T4.2 — Update ``compute_vpids`` call sites (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Grep for all calls: ``grep -n "compute_vpids" src/mca/rmaps/base/rmaps_base_map_job.c``
(currently at lines 1237 and 1323).

- Existing single-dispatch calls: pass ``app_idx = -1`` and a local ``next_vpid = 0``.
- Per-app loop calls: pass ``app_options.app_idx`` and ``&next_vpid`` (declared before the loop).

- [ ] Update call at line 1237
- [ ] Update call at line 1323
- [ ] Declare and initialise ``next_vpid`` before the per-app loop

T4.3 — Update ``compute_vpids`` declaration (``src/mca/rmaps/base/base.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Update the exported declaration to match the new signature.

- [ ] Update declaration in ``base.h``

Phase 5 — Component ``app_idx`` Guards
----------------------------------------

For each component, the change is:

**Before:**

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) {
           continue;
       }
       ...

**After:**

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) {
           continue;
       }
       if (options->app_idx >= 0 && (int)n != options->app_idx) {
           continue;
       }
       ...

T5.1 — ``src/mca/rmaps/round_robin/rmaps_rr.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] Add ``app_idx`` guard to the app-context loop

T5.2 — ``src/mca/rmaps/ppr/rmaps_ppr.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] Add ``app_idx`` guard to the app-context loop
- [ ] Remove the per-app ``PRTE_APP_PPR`` read at line 166 — value now comes through
  ``opts->pprn`` set by ``resolve_app_options()``
- [ ] Remove the per-app ``PRTE_APP_PES_PER_PROC`` read at line 171 — value now in
  ``opts->cpus_per_rank``

T5.3 — ``src/mca/rmaps/seq/rmaps_seq.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] Add ``app_idx`` guard to the app-context loop
- [ ] When looking up the seq/rankfile path, check ``PRTE_APP_MAP_FILE`` on the current
  app first; fall back to ``PRTE_JOB_FILE`` on ``jdata->attributes`` if absent

T5.4 — ``src/mca/rmaps/rank_file/rmaps_rank_file.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] Add ``app_idx`` guard to the app-context loop
- [ ] Same ``PRTE_APP_MAP_FILE`` → ``PRTE_JOB_FILE`` fallback as T5.3

T5.5 — ``src/mca/rmaps/lsf/rmaps_lsf.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] Add ``app_idx`` guard to the app-context loop

Phase 6 — Schizo / CLI Wiring
-------------------------------

T6.1 — Wire MPMD per-app args in ``src/mca/schizo/prte/schizo_prte.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find the MPMD app-context separator logic.  When ``--map-by``, ``--rank-by``, or
``--bind-to`` is encountered after a ``:`` separator (i.e., associated with a specific
``prte_app_context_t``), call the new per-app functions instead of the job-level ones:

.. code-block:: c

   /* per-app context */
   prte_rmaps_base_set_app_mapping_policy(app, optval);
   prte_rmaps_base_set_app_ranking_policy(app, optval);
   prte_rmaps_base_set_app_binding_policy(app, optval);

When these options appear before any ``:`` separator, continue calling the existing
job-level functions (no change to that path).

- [ ] Identify the MPMD separator handling code in ``schizo_prte.c``
- [ ] Route per-app ``--map-by`` to ``prte_rmaps_base_set_app_mapping_policy()``
- [ ] Route per-app ``--rank-by`` to ``prte_rmaps_base_set_app_ranking_policy()``
- [ ] Route per-app ``--bind-to`` to ``prte_rmaps_base_set_app_binding_policy()``

T6.2 — Wire PMIx spawn path (``src/mca/schizo/prte/schizo_prte.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the ``PMIx_Spawn`` / server-spawn handler, when processing per-app ``pmix_app_t.info[]``
arrays, map ``PMIX_MAPBY``, ``PMIX_RANKBY``, ``PMIX_BINDTO`` to the new per-app attribute
setter functions.

- [ ] Locate the per-app info[] processing in the spawn path
- [ ] Map ``PMIX_MAPBY`` → ``prte_rmaps_base_set_app_mapping_policy(app, val)``
- [ ] Map ``PMIX_RANKBY`` → ``prte_rmaps_base_set_app_ranking_policy(app, val)``
- [ ] Map ``PMIX_BINDTO`` → ``prte_rmaps_base_set_app_binding_policy(app, val)``

T6.3 — Factor the argv pre-scan into a shared helper and relax the guards
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Both ``prun`` and the ``prte`` HNP launcher pre-scan the raw argv and reject a
second ``--rank-by`` / ``--bind-to`` with the ``multi-instances`` error, which blocks
per-app MPMD lines like ``prun app1 --rank-by node : app2 --rank-by slot``.
``--map-by`` has no such guard.  The two pre-scan loops are exact copies, so the
fix is to factor them into one helper and relax it there once.  Make ``--rank-by``
and ``--bind-to`` behave like ``--map-by``: no ``found`` tracking, no
``multi-instances`` rejection, rename every occurrence unconditionally.  Per-app
vs. duplicate-job-level association is the schizo MPMD parser's responsibility, not
the pre-scan's.

``src/mca/schizo/base/schizo_base_stubs.c`` and ``src/mca/schizo/base/base.h``:

- [ ] Add ``char *prte_schizo_base_normalize_argv(char **argv)`` that renames the
  four deprecated spellings (``--map-by``, ``--rank-by``, ``--bind-to``,
  ``--runtime-options``) in place and returns any ``--personality`` value (a
  pointer into ``argv``, ``NULL`` if none)
- [ ] Declare it in ``base.h`` with ``PRTE_EXPORT``

``src/tools/prun/prun.c`` (``prun()``):

- [ ] Replace the inline pre-scan loop with ``personality = prte_schizo_base_normalize_argv(pargv);``
- [ ] Drop the now-unused ``i`` loop variable from the function's declarations

``src/prted/prte.c`` (``main()``):

- [ ] Replace the inline pre-scan loop with ``personality = prte_schizo_base_normalize_argv(pargv);``
  (keep ``i``; it is still used by the environment-scan loop)

- [ ] Smoke test: ``prun app1 --rank-by node : app2 --rank-by slot`` is accepted
  (no longer errors with ``multi-instances``)

Phase 7 — Build System
-----------------------

T7.1 — Create ``test/Makefile.am``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: makefile

   # Copyright (c) 2026      Nanook Consulting  All rights reserved.
   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   SUBDIRS = unit

- [ ] Create ``test/Makefile.am``

T7.2 — Create ``test/unit/Makefile.am``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: makefile

   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   SUBDIRS = rmaps

- [ ] Create ``test/unit/Makefile.am``

T7.3 — Create ``test/unit/rmaps/Makefile.am``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: makefile

   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   AM_CPPFLAGS = \
       -I$(top_srcdir)/src \
       -I$(top_srcdir)/include \
       -I$(top_srcdir)

   check_PROGRAMS = test_rmaps

   test_rmaps_SOURCES = \
       test_rmaps_main.c      \
       test_policy_parse.c    \
       test_resolve_options.c \
       test_dispatch.c        \
       test_round_robin.c     \
       test_ppr.c             \
       test_seq.c             \
       test_rank_file.c

   test_rmaps_LDADD = $(top_builddir)/src/libprrte.la

   TESTS = test_rmaps

- [ ] Create ``test/unit/rmaps/Makefile.am``

T7.4 — Add ``test`` to top-level ``SUBDIRS`` (``Makefile.am``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Change line 27:

.. code-block:: makefile

   SUBDIRS = config contrib src include docs

to:

.. code-block:: makefile

   SUBDIRS = config contrib src include docs test

``test`` must follow ``src`` so ``libprrte.la`` is built first.

- [ ] Add ``test`` after ``src`` (and before or after ``include docs`` — after ``src`` is the
  hard requirement)

T7.5 — Register Makefiles in ``config/prte_config_files.m4``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the ``AC_CONFIG_FILES`` call, add:

.. code-block:: text

   test/Makefile
   test/unit/Makefile
   test/unit/rmaps/Makefile

- [ ] Add three new ``AC_CONFIG_FILES`` entries

Phase 8 — Unit Tests
---------------------

All test files go in ``test/unit/rmaps/``.

T8.1 — ``test_rmaps_main.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Harness: ``main()`` that calls ``prte_init()`` in server-tool mode, runs each suite,
calls ``prte_finalize()``, and returns the aggregate pass/fail exit code.

Declare and call:

.. code-block:: c

   extern int test_policy_parse(void);
   extern int test_resolve_options(void);
   extern int test_dispatch(void);
   extern int test_round_robin(void);
   extern int test_ppr(void);
   extern int test_seq(void);
   extern int test_rank_file(void);

- [ ] Write ``test_rmaps_main.c``

T8.2 — ``test_policy_parse.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cover ``prte_rmaps_base_set_app_mapping_policy()``, ``_ranking_policy()``, ``_binding_policy()``:

- Valid single-word policies store correct uint16_t in ``app->attributes``
- ``pe=N`` modifier → ``PRTE_APP_PES_PER_PROC = N``
- ``hwtcpus`` → ``PRTE_APP_HWT_CPUS = true``
- ``corecpus`` → ``PRTE_APP_CORE_CPUS = true``
- ``file=path`` → ``PRTE_APP_MAP_FILE = "path"``
- ``NOLOCAL`` → ``PRTE_APP_MAPBY`` has ``PRTE_MAPPING_NO_USE_LOCAL`` bit set
- ``ppr:2:core`` → ``PRTE_APP_PPR = 2``, ``PRTE_APP_MAPBY = PRTE_MAPPING_PPR``
- ``OVERSUBSCRIBE`` → returns ``PRTE_ERR_BAD_PARAM``
- ``NOOVERSUBSCRIBE`` → returns ``PRTE_ERR_BAD_PARAM``
- ``INHERIT`` → returns ``PRTE_ERR_BAD_PARAM``
- ``NOINHERIT`` → returns ``PRTE_ERR_BAD_PARAM``
- Malformed string → appropriate error code

- [ ] Write ``test_policy_parse.c`` with all cases above

T8.3 — ``test_resolve_options.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cover ``prte_rmaps_base_resolve_app_options()``:

- App with no per-app attributes: ``app_options`` equals job-level ``options``
- App with ``PRTE_APP_MAPBY`` set: ``opts->map`` reflects app value
- ``PRTE_APP_NOLOCAL`` directive bit: ``opts->map`` carries ``PRTE_MAPPING_NO_USE_LOCAL``
- ``PRTE_APP_PES_PER_PROC``, ``PRTE_APP_HWT_CPUS``, ``PRTE_APP_CPUSET``: correct override
- ``PRTE_APP_PPR`` absent → falls back to ``PRTE_JOB_PPR`` on jdata
- Function does not modify ``jdata->map`` in any test case

- [ ] Write ``test_resolve_options.c`` with all cases above

T8.4 — ``test_dispatch.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cover the per-app detection and dispatch loop:

- Job with no per-app directives: single-dispatch path (mock component call count = 1)
- Job with at least one app carrying ``PRTE_APP_MAPBY``: per-app path; mock called once
  per app
- ``OVERSUBSCRIBE`` in per-app ``--map-by``: mapping aborts with ``MAP_FAILED``
- Conflicting ``INHERIT``/``NOINHERIT`` across apps: mapping aborts
- Any app with display-map directive: ``PRTE_JOB_DISPLAY_MAP`` promoted to job level
- **``NOLOCAL`` on app[0], not on app[1], shared HNP node** (see spec §"Dedicated test"):

  - None of app[0]'s processes land on ``node0``
  - At least one of app[1]'s processes lands on ``node0``
  - ``jdata->map->nodes`` contains all three nodes
  - ``node0`` slot count reflects only app[1]'s consumption

- [ ] Write ``test_dispatch.c`` with all cases above including the NOLOCAL shared-HNP test

T8.5 — ``test_round_robin.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Test ``round_robin`` component with ``app_idx``:

- ``app_idx = -1``: maps all apps (baseline)
- ``app_idx = 0`` on two-app job: only app[0] placed; app[1] procs unplaced
- ``app_idx = 1`` on two-app job: only app[1] placed; app[0] procs unplaced
- ``app_idx`` pointing to NULL slot: component skips gracefully

- [ ] Write ``test_round_robin.c``

T8.6 — ``test_ppr.c``
~~~~~~~~~~~~~~~~~~~~~~

Same four cases as T8.5 but for the ``ppr`` component.

- [ ] Write ``test_ppr.c``

T8.7 — ``test_seq.c``
~~~~~~~~~~~~~~~~~~~~~~

Same four cases plus:

- ``PRTE_APP_MAP_FILE`` present: component uses app-level file
- ``PRTE_APP_MAP_FILE`` absent, ``PRTE_JOB_FILE`` present: component falls back to job file

- [ ] Write ``test_seq.c``

T8.8 — ``test_rank_file.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Same as T8.7 but for the ``rank_file`` component.

- [ ] Write ``test_rank_file.c``

Completion Checklist
---------------------

Before declaring the branch ready for review:

- [ ] ``./autogen.pl && ./configure`` completes without errors
- [ ] ``make -j$(nproc)`` builds cleanly (no new warnings)
- [ ] ``make check`` passes all tests in ``test/unit/rmaps/``
- [ ] ``prun app1 --map-by core : app2 --map-by node --rank-by fill`` produces a correct
  two-app MPMD job (smoke test against a live DVM)
- [ ] ``prun app1`` with no per-app directives produces identical output to before
  (regression test)
- [ ] ``grep -r "PRTE_RMAPS_BASE_VERSION_4_0_0" src/mca/rmaps/`` returns only the
  deprecated-alias definition in ``rmaps_types.h`` and zero component references
