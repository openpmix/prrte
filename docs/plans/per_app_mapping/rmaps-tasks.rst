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

- [x] Add keys 26–34 immediately after the ``PRTE_APP_PPR`` line

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

- [x] Add nine pretty-print cases to ``src/util/attr.c``

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

- [x] Add ``app_idx`` field to ``prte_rmaps_options_t``
- [x] Add ``dist_device`` field to ``prte_rmaps_options_t``

T1.3 — Add version 5.0.0 macro (``src/mca/rmaps/rmaps_types.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing ``PRTE_RMAPS_BASE_VERSION_4_0_0`` definition (line 127), add:

.. code-block:: c

   #define PRTE_RMAPS_BASE_VERSION_5_0_0 PRTE_MCA_BASE_VERSION_3_0_0("rmaps", 5, 0, 0)
   /* deprecated alias — out-of-tree components get a version mismatch rather than
    * a silent ABI violation */
   #undef  PRTE_RMAPS_BASE_VERSION_4_0_0
   #define PRTE_RMAPS_BASE_VERSION_4_0_0 PRTE_RMAPS_BASE_VERSION_5_0_0

- [x] Add ``PRTE_RMAPS_BASE_VERSION_5_0_0``
- [x] Redefine ``PRTE_RMAPS_BASE_VERSION_4_0_0`` as a deprecated alias

T1.4 — Add module struct 5.0.0 (``src/mca/rmaps/rmaps.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the existing ``prte_rmaps_base_module_4_0_0_t`` typedef (line 84), add:

.. code-block:: c

   typedef struct prte_rmaps_base_module_4_0_0_t prte_rmaps_base_module_5_0_0_t;

Update the convenience alias on line 86:

.. code-block:: c

   typedef prte_rmaps_base_module_5_0_0_t prte_rmaps_base_module_t;

- [x] Add ``prte_rmaps_base_module_5_0_0_t`` typedef
- [x] Update ``prte_rmaps_base_module_t`` alias

T1.5 — Update all five component files to reference version 5.0.0
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the component struct initializer in each file, change ``PRTE_RMAPS_BASE_VERSION_4_0_0``
to ``PRTE_RMAPS_BASE_VERSION_5_0_0``:

- [x] ``src/mca/rmaps/round_robin/rmaps_rr_component.c`` (line 47)
- [x] ``src/mca/rmaps/ppr/rmaps_ppr_component.c`` (line 37)
- [x] ``src/mca/rmaps/seq/rmaps_seq_component.c``
- [x] ``src/mca/rmaps/rank_file/rmaps_rank_file_component.c``
- [x] ``src/mca/rmaps/lsf/rmaps_lsf_component.c``

T1.6 — Fix ``app_idx`` initialisation in ``prte_rmaps_base_map_job()``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``src/mca/rmaps/base/rmaps_base_map_job.c``: after the ``memset(&options, 0, ...)`` call
(around line 93), add:

.. code-block:: c

   options.app_idx = -1;   /* -1 = map all apps (default) */

Without this the new field is 0 after memset, which would mean "map only app[0]" once
components start honouring it.

- [x] Add explicit ``options.app_idx = -1`` after the memset

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

- [x] Write ``prte_rmaps_base_set_app_mapping_policy(prte_app_context_t *app, char *spec)``
- [x] Verify OVERSUBSCRIBE/NOOVERSUBSCRIBE rejected with ``PRTE_ERR_BAD_PARAM``
- [x] Verify INHERIT/NOINHERIT rejected with ``PRTE_ERR_BAD_PARAM``
- [x] Verify NOLOCAL directive bit stored in ``PRTE_APP_MAPBY``

T2.2 — Implement ``prte_rmaps_base_set_app_ranking_policy()`` (``src/mca/rmaps/base/rmaps_base_frame.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Model on ``prte_rmaps_base_set_ranking_policy()`` (line 756).  Store the parsed
``prte_ranking_policy_t`` as ``PRTE_APP_RANKBY`` (uint16_t) on ``app->attributes``
with ``PRTE_ATTR_GLOBAL``.

- [x] Write ``prte_rmaps_base_set_app_ranking_policy(prte_app_context_t *app, char *spec)``

T2.3 — Implement ``prte_rmaps_base_set_app_binding_policy()`` (``src/mca/rmaps/base/rmaps_base_frame.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Model on the existing binding policy setter.  Store the parsed ``prte_binding_policy_t``
as ``PRTE_APP_BINDTO`` (uint16_t) on ``app->attributes`` with ``PRTE_ATTR_GLOBAL``.

- [x] Write ``prte_rmaps_base_set_app_binding_policy(prte_app_context_t *app, char *spec)``

T2.5 — Use ``PRTE_ATTR_GLOBAL`` for every per-app attribute store
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Across all three ``set_app_*_policy`` functions, every ``prte_set_attribute(&app->attributes,
PRTE_APP_*, ...)`` call must use ``PRTE_ATTR_GLOBAL``, never ``PRTE_ATTR_LOCAL``.  ``LOCAL``
attributes are not packed and are dropped when the spawn request is relayed to the DVM
master, so per-app directives stored as ``LOCAL`` silently never reach ``map_job``.  This
covers ``PRTE_APP_MAPBY``, ``RANKBY``, ``BINDTO``, ``PPR``, ``PES_PER_PROC``, ``HWT_CPUS``,
``CORE_CPUS``, ``CPUSET``, ``MAP_FILE``, and ``BINDING_LIMIT``.

- [x] Audit all three setters; confirm zero ``PRTE_ATTR_LOCAL`` remain in app stores
- [x] Verify with a multi-app MPMD offline run that the attributes reach ``map_job``

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

- [x] Add declarations to ``base.h``

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

1. ``PRTE_APP_MAPBY`` (uint16_t) → ``opts->map`` (masked via ``PRTE_GET_MAPPING_POLICY``).
   Keep the raw value locally for the SPAN directive.  When present, also refresh
   ``opts->maptype``/``mapdepth``/``mapspan``/``ordered`` from the new policy.
2. If ``opts->map == PRTE_MAPPING_PPR``: read ``PRTE_APP_PPR`` (uint16_t) → ``opts->pprn``;
   if absent, fall back to ``PRTE_JOB_PPR`` on ``jdata->attributes``.
3. ``PRTE_APP_PES_PER_PROC`` (uint16_t) → ``opts->cpus_per_rank``
4. ``PRTE_APP_HWT_CPUS`` (bool) → ``opts->use_hwthreads = true``; else
   ``PRTE_APP_CORE_CPUS`` (bool) → ``opts->use_hwthreads = false``
5. ``PRTE_APP_CPUSET`` (string) → ``opts->cpuset``
6. ``PRTE_APP_MAP_FILE`` (string) → store somewhere accessible; see seq/rank_file notes
7. ``PRTE_APP_DIST_DEVICE`` (string) → ``opts->dist_device``
8. ``PRTE_APP_BINDING_LIMIT`` (uint16_t) → ``opts->limit``
9. **Ranking:** if ``PRTE_APP_RANKBY`` present → ``opts->rank`` (masked via
   ``PRTE_GET_RANKING_POLICY``); else if the app supplied ``PRTE_APP_MAPBY`` →
   ``opts->rank = prte_rmaps_base_derive_ranking(rawmap)``; else leave job-level value.
10. **Binding:** if ``PRTE_APP_BINDTO`` present → ``opts->bind`` (masked via
    ``PRTE_GET_BINDING_POLICY``) and ``opts->overload`` from the overload bit; else if the
    app supplied ``PRTE_APP_MAPBY`` →
    ``opts->bind = prte_rmaps_base_derive_binding(rawmap, opts->use_hwthreads)``;
    else leave job-level value.
11. The function must not modify ``jdata->map``.

Also add the two pure helpers ``prte_rmaps_base_derive_ranking(mapping)`` and
``prte_rmaps_base_derive_binding(mapping, use_hwthreads)`` (mirroring the job-level
default logic) used by steps 9–10.

- [x] Implement ``prte_rmaps_base_resolve_app_options()`` with all override steps
- [x] Add ``derive_ranking`` / ``derive_binding`` helpers
- [x] Mask directive bits off ``opts->map``/``rank``/``bind``; route overload to ``opts->overload``
- [x] Default rank/bind from the app's own map when no explicit per-app directive
- [x] Verify function does not write to ``jdata->map``

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

- [x] Add ``any_per_app`` scan

T3.3 — Reject job-level-only directives given per app (no promotion)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The original plan promoted per-app display and INHERIT/NOINHERIT directives to
the job level.  There is no meaningful way to scope a display directive to one
app context, so rather than promote, these are simply rejected:

1. **display:** display directives are job-level.  ``prte_prun_parse_common_cli()``
   (``src/prted/prun_common.c``) errors out when more than one display directive
   is given (``pmix_cmd_line_get_ninsts(results, PRTE_CLI_DISPLAY) > 1`` →
   ``multi-instances`` help + ``PRTE_ERR_BAD_PARAM``), which also catches a
   display attached to a second app in an MPMD line.

2. **INHERIT/NOINHERIT:** ``prte_rmaps_base_set_app_mapping_policy()`` already
   rejects these modifiers in a per-app ``--map-by`` string with
   ``PRTE_ERR_BAD_PARAM``, so a per-app INHERIT directive can never be set and no
   cross-app conflict can arise — no promotion or conflict scan is needed.

- [x] Reject multiple display directives in ``prte_prun_parse_common_cli()``
- [x] INHERIT/NOINHERIT rejected per app by ``set_app_mapping_policy()`` (T2.1)

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

- [x] Guard existing single-dispatch block with ``if (!any_per_app)``
- [x] Add per-app loop in ``else`` branch
- [x] Verify ``app_options.app_idx = n`` is set each iteration
- [x] Verify oversubscription error path reaches ``MAP_FAILED``

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

- [x] Add ``app_idx`` and ``uint32_t *next_vpid`` parameters
- [x] When ``app_idx >= 0``, restrict processing to that app; use/update ``*next_vpid``
- [x] When ``app_idx < 0``, function works as today (backward-compatible path)

T4.2 — Update ``compute_vpids`` call sites (``src/mca/rmaps/base/rmaps_base_map_job.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Grep for all calls: ``grep -n "compute_vpids" src/mca/rmaps/base/rmaps_base_map_job.c``
(currently at lines 1237 and 1323).

- Existing single-dispatch calls: pass ``app_idx = -1`` and a local ``next_vpid = 0``.
- Per-app loop calls: pass ``app_options.app_idx`` and ``&next_vpid`` (declared before the loop).

- [x] Update call at line 1237
- [x] Update call at line 1323
- [x] Declare and initialise ``next_vpid`` before the per-app loop

T4.3 — Update ``compute_vpids`` declaration (``src/mca/rmaps/base/rmaps_private.h``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Update the exported declaration to match the new signature.

- [x] Update declaration in ``rmaps_private.h``

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

- [x] Add ``app_idx`` guard to the app-context loop

T5.2 — ``src/mca/rmaps/ppr/rmaps_ppr.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [x] Add ``app_idx`` guard to the app-context loop
- [x] Remove the per-app ``PRTE_APP_PPR`` read at line 166 — value now comes through
  ``opts->pprn`` set by ``resolve_app_options()``
- [x] Remove the per-app ``PRTE_APP_PES_PER_PROC`` read at line 171 — value now in
  ``opts->cpus_per_rank``

T5.3 — ``src/mca/rmaps/seq/rmaps_seq.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [x] Add ``app_idx`` guard to the app-context loop
- [x] When looking up the seq/rankfile path, check ``PRTE_APP_MAP_FILE`` on the current
  app first; fall back to ``PRTE_JOB_FILE`` on ``jdata->attributes`` if absent

T5.4 — ``src/mca/rmaps/rank_file/rmaps_rank_file.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [x] Add ``app_idx`` guard to the app-context loop
- [x] Same ``PRTE_APP_MAP_FILE`` → ``PRTE_JOB_FILE`` fallback as T5.3

T5.5 — ``src/mca/rmaps/lsf/rmaps_lsf.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [x] Add ``app_idx`` guard to the app-context loop

Phase 6 — Schizo / CLI Wiring
-------------------------------

T6.1 — Per-app args recorded on each app's info[] (``src/prted/prte_app_parse.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No schizo MPMD bookkeeping is needed: ``prte_parse_locals()`` already parses each
app segment (between ``:`` separators) in its own ``create_app()`` call, so each
app's ``--map-by``/``--rank-by``/``--bind-to`` is naturally scoped to that app.
``create_app()`` records them on the app's ``pmix_app_t.info[]`` array as
``PMIX_MAPBY``/``PMIX_RANKBY``/``PMIX_BINDTO`` (already present in the tree).  The
spawn-assembly loops in ``src/prted/prte.c`` and ``src/prted/prun_common.c`` must
convert each ``pmix_app_t.info`` from that app's own ``app->info`` list.

- [x] Per-app ``--map-by``/``--rank-by``/``--bind-to`` recorded per app in ``prte_app_parse.c``
- [x] Spawn assembly converts each app's own ``app->info`` (prte.c / prun_common.c)

T6.2 — Wire PMIx spawn path (``src/mca/schizo/prte/schizo_prte.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the ``PMIx_Spawn`` / server-spawn handler, when processing per-app ``pmix_app_t.info[]``
arrays, map ``PMIX_MAPBY``, ``PMIX_RANKBY``, ``PMIX_BINDTO`` to the new per-app attribute
setter functions.

- [x] Locate the per-app info[] processing in the spawn path
- [x] Map ``PMIX_MAPBY`` → ``prte_rmaps_base_set_app_mapping_policy(app, val)``
- [x] Map ``PMIX_RANKBY`` → ``prte_rmaps_base_set_app_ranking_policy(app, val)``
- [x] Map ``PMIX_BINDTO`` → ``prte_rmaps_base_set_app_binding_policy(app, val)``

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

- [x] Add ``char *prte_schizo_base_normalize_argv(char **argv)`` that renames the
  four deprecated spellings (``--map-by``, ``--rank-by``, ``--bind-to``,
  ``--runtime-options``) in place and returns any ``--personality`` value (a
  pointer into ``argv``, ``NULL`` if none)
- [x] Declare it in ``base.h`` with ``PRTE_EXPORT``

``src/tools/prun/prun.c`` (``prun()``):

- [x] Replace the inline pre-scan loop with ``personality = prte_schizo_base_normalize_argv(pargv);``
- [x] Drop the now-unused ``i`` loop variable from the function's declarations

``src/prted/prte.c`` (``main()``):

- [x] Replace the inline pre-scan loop with ``personality = prte_schizo_base_normalize_argv(pargv);``
  (keep ``i``; it is still used by the environment-scan loop)

- [x] Smoke test: ``prun app1 --rank-by node : app2 --rank-by slot`` is accepted
  (no longer errors with ``multi-instances``)

Phase 7 — Build System
-----------------------

T7.1 — Add the new subdirs to ``test/Makefile.am``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``test/Makefile.am`` already exists (it builds the standalone PMIx client test
programs).  The mapping work only needs the new subdirectories added to its
``SUBDIRS`` line.  As landed — including the offline harness and shared
topology directory added in follow-on work — the line reads:

.. code-block:: makefile

   SUBDIRS = topologies offline unit attachtest

``unit`` carries the ``rmaps`` unit tests (T7.2/T7.3); ``topologies`` holds the
shared hwloc XML fixtures; ``offline`` is the ``prterun --rtos donotlaunch``
golden-map harness (see the Offline harness note in the Completion Checklist).

- [x] Add ``topologies offline unit attachtest`` to ``test/Makefile.am`` ``SUBDIRS``

T7.2 — Create ``test/unit/Makefile.am``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: makefile

   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   SUBDIRS = rmaps

- [x] Create ``test/unit/Makefile.am``

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

- [x] Create ``test/unit/rmaps/Makefile.am``

T7.4 — Add ``test`` to top-level ``SUBDIRS`` (``Makefile.am``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Change line 27:

.. code-block:: makefile

   SUBDIRS = config contrib src include docs

to:

.. code-block:: makefile

   SUBDIRS = config contrib src include docs test

``test`` must follow ``src`` so ``libprrte.la`` is built first.

- [x] Add ``test`` after ``src`` (and before or after ``include docs`` — after ``src`` is the
  hard requirement)

T7.5 — Register Makefiles in ``config/prte_config_files.m4``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the ``AC_CONFIG_FILES`` call, add an entry for every new ``Makefile.am``.
As landed (the offline harness and shared topology directory added their own
entries in follow-on work) the relevant lines are:

.. code-block:: text

   test/Makefile
   test/attachtest/Makefile
   test/topologies/Makefile
   test/offline/Makefile
   test/unit/Makefile
   test/unit/rmaps/Makefile

- [x] Add ``AC_CONFIG_FILES`` entries for ``test/``, ``test/topologies/``,
  ``test/offline/``, ``test/unit/``, and ``test/unit/rmaps/``

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

- [x] Write ``test_rmaps_main.c``

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

- [x] Write ``test_policy_parse.c`` with all cases above

T8.3 — ``test_resolve_options.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cover ``prte_rmaps_base_resolve_app_options()`` and the ``derive_ranking`` /
``derive_binding`` helpers (all exposed non-static for the tests):

- App with no per-app attributes: ``app_options`` equals job-level ``options``
- App with ``PRTE_APP_MAPBY`` set: ``opts->map`` reflects app value, masked of directive bits
- Per-app map with no rank/bind: rank and bind default from the app's own map
- Explicit per-app rank-by overrides the map default
- Explicit bind-to: ``opts->bind`` masked, overload lifted to ``opts->overload``
- ``PRTE_APP_PES_PER_PROC``, ``PRTE_APP_HWT_CPUS``: correct override
- ``derive_ranking`` / ``derive_binding`` map → policy tables

- [x] Write ``test_resolve_options.c`` with the cases above

T8.4 — ``test_dispatch.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cover the per-app resolution the dispatch loop performs.  The placement-level
cases below require a live node pool/topology and are covered end-to-end by the
offline ``prterun --rtos donotlaunch --display map`` method (see AGENTS.md and
the §Verification checklist), not by this pure unit test:

- Two-app job, each app with its own per-app directives, resolves to distinct
  correct options (the regression that the ``LOCAL``/``GLOBAL`` bug caused)
- (offline) single-dispatch vs. per-app path selection
- (offline) ``NOLOCAL`` on app[0] not app[1] on a shared HNP node

- [x] Write ``test_dispatch.c`` (two-app per-app resolution)
- [x] NOLOCAL shared-HNP and single-vs-per-app paths covered by offline verification

T8.5 — ``test_round_robin.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Test the ``round_robin`` component's dispatch guards directly via its exported
module struct (no node pool needed): a non-rr mapping policy and a restarting
job both return ``PRTE_ERR_TAKE_NEXT_OPTION``.  The ``app_idx`` placement modes
(``-1``/``0``/``1``) require a live node pool and are covered by the offline
verification method.

- [x] Write ``test_round_robin.c`` (defer-contract; app_idx placement via offline)

T8.6 — ``test_ppr.c``
~~~~~~~~~~~~~~~~~~~~~~

Same approach for the ``ppr`` component (non-ppr policy and restart → defer).

- [x] Write ``test_ppr.c`` (defer-contract; app_idx placement via offline)

T8.7 — ``test_seq.c``
~~~~~~~~~~~~~~~~~~~~~~

Same approach for the ``seq`` component (non-seq policy and other req_mapper →
defer).  Per-app ``PRTE_APP_MAP_FILE`` selection is covered by the offline
verification method.

- [x] Write ``test_seq.c`` (defer-contract; file selection/app_idx via offline)

T8.8 — ``test_rank_file.c``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Same approach for the ``rank_file`` component (non-byuser policy and other
req_mapper → defer).  Per-app rankfile selection is covered by the offline
verification method.

- [x] Write ``test_rank_file.c`` (defer-contract; file selection/app_idx via offline)

Completion Checklist
---------------------

Before declaring the branch ready for review:

- [x] ``./autogen.pl && ./configure`` completes without errors
- [x] ``make -j$(nproc)`` builds cleanly (no new warnings)
- [x] ``make check`` passes all tests in ``test/unit/rmaps/`` **and** runs the
  offline mapping harness in ``test/offline/`` (``run_offline_maps.py``), which
  drives ``prterun --rtos donotlaunch --display map`` over a directive matrix
  crossed with every topology in ``test/topologies/`` and compares a curated
  subset against the committed golden maps under ``test/offline/golden/``.  See
  ``test/offline/README.rst`` for how to run it by hand and add topologies; the
  per-app MPMD cases live under the ``perapp.*`` golden ids.
- [x] ``grep -rn PRTE_ATTR_LOCAL`` in the three ``set_app_*_policy`` functions returns
  nothing (all per-app stores are ``PRTE_ATTR_GLOBAL``)
- [x] **Offline multi-app check** (no DVM): a per-app ``--map-by``/``--rank-by``/``--bind-to``
  on the *second* app of an MPMD line visibly changes that app's placement/rank/binding::

      prterun --rtos donotlaunch --display map \
          --prtemca hwloc_use_topo_file test/topologies/test-topo.xml \
          -H n0:4,n1:4 \
          --map-by node -n 4 hostname : --map-by slot --rank-by node -n 4 hostname

  Toggle the second app's ``--rank-by slot`` vs ``node`` and confirm the printed ranks
  differ (catches the ``LOCAL``/``GLOBAL`` and masking regressions; a single-app job will
  *not* catch them).
- [x] ``prun app1`` with no per-app directives produces identical output to before
  (regression test)
- [x] ``grep -r "PRTE_RMAPS_BASE_VERSION_4_0_0" src/mca/rmaps/`` returns only the
  deprecated-alias definition in ``rmaps_types.h`` and zero component references
