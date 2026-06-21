Per-App-Context Mapping Policies
=================================

Overview
--------

Today every ``prte_job_t`` carries a single ``prte_job_map_t`` (mapping/ranking/binding
policy triple) and a single resolved ``prte_rmaps_options_t`` that is passed unchanged
to whichever rmaps component wins selection.  All app contexts in the job are mapped
by that one component under that one policy.

This document specifies the changes required to allow each ``prte_app_context_t`` to
carry its own mapping, ranking, and binding directives so that different apps within
the same job can be mapped by different components under different policies.

Goals
-----

1. Every directive expressible at job level via ``--map-by``, ``--rank-by``, and
   ``--bind-to`` must be expressible at app-context level.
2. When an app carries no per-app directives, it inherits the job-level policy
   unchanged — no behaviour change for existing usage.
3. The rmaps component selected for each app context is determined by that app's
   resolved mapping policy, not by the job's.
4. The existing component interface (``map_job(prte_job_t *, prte_rmaps_options_t *)``)
   is preserved with a minimal, backward-compatible extension.
5. Global rank assignment (vpid computation) remains a single coordinated pass
   across all apps after all apps have been placed.

Attributes Required on ``prte_app_context_t``
---------------------------------------------

Add the following new attribute keys to ``src/util/attr.h`` in the
``PRTE_APP_*`` range (next available keys after ``PRTE_APP_PPR = 25``):

.. code-block:: c

   /* Mapping policy string for this app, same syntax as --map-by.
    * When present overrides the job-level mapping policy for this app. */
   #define PRTE_APP_MAPBY              26  // char* - e.g. "core:pe=2:oversubscribe"

   /* Ranking policy string for this app, same syntax as --rank-by. */
   #define PRTE_APP_RANKBY             27  // char* - e.g. "fill"

   /* Binding policy string for this app, same syntax as --bind-to. */
   #define PRTE_APP_BINDTO             28  // char* - e.g. "core"

   /* File to use for sequential or rankfile mapping for this app.
    * Distinct from PRTE_APP_HOSTFILE which lists nodes; this is the
    * ordering/affinity file consumed by the seq and rank_file components. */
   #define PRTE_APP_MAP_FILE           29  // char* - path to seq or rankfile

   /* Device name for dist mapping for this app. */
   #define PRTE_APP_DIST_DEVICE        30  // char* - e.g. "mlx5_0"

   /* Use hwthreads as CPUs for this app. */
   #define PRTE_APP_HWT_CPUS           31  // bool

   /* Use cores as CPUs for this app (explicit, not relying on absence of HWT). */
   #define PRTE_APP_CORE_CPUS          32  // bool

   /* PE-list (CPU set) for this app, same syntax as pe-list= modifier. */
   #define PRTE_APP_CPUSET             33  // char* - comma-delimited CPU ranges

   /* Max procs to bind to a target object before moving to the next. */
   #define PRTE_APP_BINDING_LIMIT      34  // uint16_t

``PRTE_APP_MAX_KEY`` must be raised to accommodate the new keys.

Relationship to existing attributes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The already-defined ``PRTE_APP_PES_PER_PROC`` (24) and ``PRTE_APP_PPR`` (25) are
preserved as-is; their semantics are unchanged.

``PRTE_APP_MAPBY`` supersedes ``PRTE_APP_PPR`` and ``PRTE_APP_PES_PER_PROC`` when
present — the full ``PRTE_APP_MAPBY`` string is the canonical per-app mapping
directive and is parsed by the same machinery as the job-level ``--map-by``
string (see §Parsing below).

Changes to ``prte_rmaps_options_t`` (``src/mca/rmaps/rmaps_types.h``)
-----------------------------------------------------------------------

Add one field:

.. code-block:: c

   typedef struct {
       /* ... existing fields unchanged ... */

       /* When >= 0, the component must map only the app context at this index
        * within jdata->apps and must skip all others.
        * When < 0 (default, set to -1), the component maps all app contexts
        * as it does today. */
       int app_idx;

   } prte_rmaps_options_t;

The new field is initialized to ``-1`` (map all apps) in ``prte_rmaps_base_map_job``
via the existing ``memset(&options, 0, ...)`` plus an explicit assignment immediately
after:

.. code-block:: c

   memset(&options, 0, sizeof(prte_rmaps_options_t));
   options.app_idx = -1;   /* map all apps by default */

New Parsing Functions (``src/mca/rmaps/base/rmaps_base_frame.c``)
------------------------------------------------------------------

The existing ``prte_rmaps_base_set_mapping_policy(prte_job_t *jdata, char *spec)``
and ``check_modifiers()`` store their results into ``jdata->map->mapping`` and
``jdata->attributes``.  They must not be changed.

Add three new functions with identical parsing logic but storing results into
``app->attributes``:

.. code-block:: c

   /* Parse a --map-by style string and store the result on app->attributes.
    * All mapping policy values, modifiers, and options that check_modifiers()
    * handles at the job level are handled here at the app level.  PPR pattern
    * is stored as PRTE_APP_PPR; pe count as PRTE_APP_PES_PER_PROC; cpuset as
    * PRTE_APP_CPUSET; file as PRTE_APP_MAP_FILE; hwthread/core CPU mode as
    * PRTE_APP_HWT_CPUS / PRTE_APP_CORE_CPUS.  The parsed mapping policy enum
    * value is stored as PRTE_APP_MAPBY (as a uint16_t, not the original string,
    * once resolved) — see §Resolution below for how this is read back. */
   int prte_rmaps_base_set_app_mapping_policy(prte_app_context_t *app, char *spec);

   /* Parse a --rank-by style string and store the result as PRTE_APP_RANKBY
    * (uint16_t ranking policy) on app->attributes.  Accepts the same ranking
    * objects as the job-level --rank-by: SLOT, NODE, FILL, and SPAN.  The
    * PRTE_RANKING_GIVEN directive bit is set so the resolve step knows the
    * value was supplied explicitly (and must not be re-derived from the app's
    * mapping policy).  An unrecognized object returns PRTE_ERR_SILENT after a
    * diagnostic. */
   int prte_rmaps_base_set_app_ranking_policy(prte_app_context_t *app, char *spec);

   /* Parse a --bind-to style string and store the result as PRTE_APP_BINDTO
    * (uint16_t binding policy) on app->attributes.  Accepts the same binding
    * objects as the job-level --bind-to: NONE, HWTHREAD, CORE, L1CACHE,
    * L2CACHE, L3CACHE, NUMA, and PACKAGE.  The ":"-delimited modifiers
    * if-supported, overload-allowed, no-overload, and LIMIT=N are parsed and
    * recorded: if-supported/overload directives become directive bits within
    * the PRTE_APP_BINDTO uint16_t (PRTE_BIND_IF_SUPPORTED,
    * PRTE_BIND_ALLOW_OVERLOAD / PRTE_BIND_OVERLOAD_GIVEN), while LIMIT=N is
    * stored separately as PRTE_APP_BINDING_LIMIT (uint16_t).  An unrecognized
    * object or modifier returns PRTE_ERR_BAD_PARAM (or PRTE_ERR_SILENT for a
    * malformed LIMIT value) after a diagnostic. */
   int prte_rmaps_base_set_app_binding_policy(prte_app_context_t *app, char *spec);

These are declared in ``src/mca/rmaps/base/base.h``.

Both functions mirror the job-level ``prte_rmaps_base_set_ranking_policy()`` and
the binding-policy parser exactly, differing only in that they write their
result onto ``app->attributes`` rather than ``jdata->map``.  Every ranking
object and binding object/modifier expressible at the job level is therefore
expressible per app, satisfying Goal 1 for ``--rank-by`` and ``--bind-to`` to
the same degree as ``--map-by``.

Attribute storage convention
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To avoid storing both a raw string and a parsed integer for the same concept,
the new attributes store the **parsed policy value** (uint16_t), not the
original string.  The string attribute keys defined in §Attributes are used
only for the schizo/CLI layer to record the unparsed directive before the
base layer processes it; once parsed, the uint16_t value replaces the string
in the attribute.

Alternatively — and this is the recommended approach for simplicity — the
string attributes are **only** used by the schizo/CLI parsing layer; the base
layer's new ``prte_rmaps_base_set_app_*`` functions accept the string, parse it,
and store the **result** as additional attributes:

.. list-table::
   :header-rows: 1
   :widths: 30 35 20

   * - Parsed result
     - Attribute
     - Type
   * - Mapping policy enum
     - ``PRTE_APP_MAPBY``
     - ``PMIX_UINT16``
   * - Ranking policy enum
     - ``PRTE_APP_RANKBY``
     - ``PMIX_UINT16``
   * - Binding policy enum
     - ``PRTE_APP_BINDTO``
     - ``PMIX_UINT16``
   * - PPR pattern string
     - ``PRTE_APP_PPR``
     - ``PMIX_STRING``
   * - CPUs per rank
     - ``PRTE_APP_PES_PER_PROC``
     - ``PMIX_UINT16``
   * - CPU set string
     - ``PRTE_APP_CPUSET``
     - ``PMIX_STRING``
   * - Map/rankfile path
     - ``PRTE_APP_MAP_FILE``
     - ``PMIX_STRING``
   * - Use hwthreads
     - ``PRTE_APP_HWT_CPUS``
     - ``PMIX_BOOL``
   * - Use cores
     - ``PRTE_APP_CORE_CPUS``
     - ``PMIX_BOOL``
   * - Binding limit
     - ``PRTE_APP_BINDING_LIMIT``
     - ``PMIX_UINT16``

These attributes must be stored with ``PRTE_ATTR_GLOBAL``, never ``PRTE_ATTR_LOCAL``
....................................................................................

This is a correctness requirement, not a stylistic one, and it is easy to get
wrong.  The per-app directives are set on the app context while the spawn
request is being processed, but the request is then serialized and relayed to
the DVM master before ``prte_rmaps_base_map_job()`` runs.  Only ``GLOBAL``
attributes are packed; ``LOCAL`` attributes are silently dropped during that
transfer.

If the ``prte_rmaps_base_set_app_*`` helpers store these attributes as
``PRTE_ATTR_LOCAL``, they vanish before mapping: the ``any_per_app`` scan
(see below) finds nothing, the per-app dispatch path is never taken, and every
app is mapped, ranked, and bound by the job-level policy regardless of its own
directives — with no error reported.  The single-app case can appear to "work"
only because its directive coincides with the job-level policy, which masks the
defect.

Store every ``PRTE_APP_*`` attribute listed above with ``PRTE_ATTR_GLOBAL``,
matching the convention already used for ``PRTE_APP_PPR`` and
``PRTE_APP_PES_PER_PROC`` in the spawn handler.

Changes to ``prte_rmaps_base_map_job()`` (``src/mca/rmaps/base/rmaps_base_map_job.c``)
---------------------------------------------------------------------------------------

This is the primary structural change.

Step 1 — resolve job-level defaults (unchanged)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The existing inheritance, policy-resolution, and process-count logic runs as
now and populates ``jdata->map->mapping``, ``jdata->map->ranking``,
``jdata->map->binding``, and the job-level ``options`` struct.  This path is
unchanged and provides the fallback for apps that carry no per-app directives.

Step 2 — check whether any app has per-app directives
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After job-level resolution, scan the apps array:

.. code-block:: c

   bool any_per_app = false;
   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) continue;
       if (prte_get_attribute(&app->attributes, PRTE_APP_MAPBY, NULL, PMIX_UINT16) ||
           prte_get_attribute(&app->attributes, PRTE_APP_RANKBY, NULL, PMIX_UINT16) ||
           prte_get_attribute(&app->attributes, PRTE_APP_BINDTO, NULL, PMIX_UINT16)) {
           any_per_app = true;
           break;
       }
   }

If ``any_per_app`` is false, the existing single-dispatch path runs unchanged.

Step 3 — per-app dispatch loop (new path)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``any_per_app`` is true, replace the single component-dispatch block with
a loop over app contexts:

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) continue;

       /* Build a per-app copy of options starting from the job-level defaults */
       prte_rmaps_options_t app_options = options;   /* shallow copy */
       app_options.app_idx = n;

       /* Override with app-level directives where present */
       rc = prte_rmaps_base_resolve_app_options(jdata, app, &app_options);
       if (PRTE_SUCCESS != rc) {
           PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
           goto cleanup;
       }

       /* Compute process count for this app (if not already set) */
       rc = prte_rmaps_base_compute_nprocs(jdata, app, &app_options);
       if (PRTE_SUCCESS != rc) {
           PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
           goto cleanup;
       }

       /* Select and invoke the component appropriate for this app's policy */
       did_map = false;
       PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules,
                         prte_rmaps_base_selected_module_t) {
           rc = mod->module->map_job(jdata, &app_options);
           if (PRTE_SUCCESS == rc) {
               did_map = true;
               break;
           }
           if (PRTE_ERR_RESOURCE_BUSY == rc) {
               /* oversubscription detected */
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

New helper: ``prte_rmaps_base_resolve_app_options()``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Extract into a separate static (or base-exported) function the logic for
building ``app_options`` from the job-level ``options`` plus any per-app
overrides:

.. code-block:: c

   static int prte_rmaps_base_resolve_app_options(prte_job_t *jdata,
                                                  prte_app_context_t *app,
                                                  prte_rmaps_options_t *opts)

This function:

1. Reads ``PRTE_APP_MAPBY`` (uint16_t) from ``app->attributes``; if present,
   stores the **masked** policy (``PRTE_GET_MAPPING_POLICY``) into ``opts->map``
   and refreshes the fields that are derived from the mapping policy —
   ``opts->maptype``, ``opts->mapdepth``, ``opts->mapspan``, ``opts->ordered``
   — exactly as the job-level path does after it resolves the job map.  The
   raw value (with its directive bits) is kept locally so the rank/bind
   defaults in steps 4–5 can read its ``SPAN`` directive.

2. If ``opts->map`` is ``PRTE_MAPPING_PPR``, reads ``PRTE_APP_PPR`` from
   ``app->attributes``; if absent falls back to ``PRTE_JOB_PPR`` on ``jdata``.

3. Reads ``PRTE_APP_PES_PER_PROC``, ``PRTE_APP_HWT_CPUS``, ``PRTE_APP_CORE_CPUS``,
   ``PRTE_APP_CPUSET``, ``PRTE_APP_MAP_FILE``, ``PRTE_APP_DIST_DEVICE``,
   ``PRTE_APP_BINDING_LIMIT`` and overrides the corresponding ``opts`` fields.

4. **Ranking.**  If ``PRTE_APP_RANKBY`` is present, stores the masked policy
   (``PRTE_GET_RANKING_POLICY``) into ``opts->rank``.  Otherwise, if the app
   supplied its own ``PRTE_APP_MAPBY`` (step 1), derives the ranking default
   from **that app's** mapping policy — mirroring the NULL-spec path of
   ``prte_rmaps_base_set_ranking_policy()`` (by-node map → by-node rank,
   by-slot map → by-slot rank, object map → by-fill, ``SPAN`` → by-span).  If
   the app changed neither, the job-level ranking carried in ``opts`` stands.

5. **Binding.**  If ``PRTE_APP_BINDTO`` is present, stores the masked policy
   (``PRTE_GET_BINDING_POLICY``) into ``opts->bind`` and lifts the overload
   directive into ``opts->overload``.  Otherwise, if the app supplied its own
   ``PRTE_APP_MAPBY``, derives the binding default from that app's mapping
   policy — bind to the mapped object (numa/package/cache/core/hwthread), or to
   core (hwthread when hwthreads are in use) for object-less mappings such as
   by-node and by-slot.

The crucial point for steps 4–5: when an app overrides its mapping policy but
gives no explicit ranking or binding, the defaults must follow **that app's**
mapping, not the job-level mapping.  Inheriting ``opts->rank``/``opts->bind``
unchanged would silently rank and bind the app as if it had been mapped by the
job-wide policy.

Two small pure helpers, ``prte_rmaps_base_derive_ranking(mapping)`` and
``prte_rmaps_base_derive_binding(mapping, use_hwthreads)``, encode the
map → rank and map → bind defaults and are reused for both the explicit and
defaulted cases.

Masking note: the ``PRTE_APP_MAPBY``/``RANKBY``/``BINDTO`` attributes carry the
policy value with its high-bit directive flags (``GIVEN``, overload,
``IS_SET``) attached.  ``opts->map``/``rank``/``bind`` are compared against the
bare ``PRTE_MAPPING_*``/``PRTE_RANK_BY_*``/``PRTE_BIND_TO_*`` enums elsewhere,
so the resolver must mask off the directive bits (and route the overload bit to
``opts->overload``) rather than assigning the raw attribute value.

The function must be idempotent and must not modify ``jdata->map`` — any
per-app mapping policy lives only in ``opts`` and in ``app->attributes``.

Binding-default and display refinements
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Follow-on work hardened the binding-default derivation and the map display so
they behave correctly across SMT/no-SMT topologies and per-app MPMD lines.
These refine — but do not change — the resolver contract above:

* **Oversubscribe permission no longer suppresses binding.**  The binding
  default is derived from the mapping policy in every case (job-level and
  per-app ``resolve_app_options()`` alike).  The mere *permission* to
  oversubscribe (``rmaps_default_mapping_policy=:oversubscribe``) is not the
  same as a node *being* oversubscribed; only the round_robin/ppr mappers,
  which know the real per-node placement, reset an un-``GIVEN`` binding to
  ``BIND_TO_NONE`` for a node they find genuinely oversubscribed or overloaded.
  An explicit ``--bind-to`` (the policy's ``GIVEN`` bit) is never overridden.

* **"core" survives on no-SMT nodes.**  A core that holds a single hwthread is
  still a core: the core→hwthread rewrite in the ``--map-by core`` /
  ``--bind-to core`` / ``corecpus`` paths now keys off the *absence* of core
  objects (``prte_hwloc_base_has_cores()``), not off
  ``prte_hwloc_base_core_cpus()``.  The displayed policy reflects the user's
  "core" terminology, and ``--map-by core:pe=2`` errors consistently on SMT and
  no-SMT nodes instead of being silently rewritten.

* **bind-to hwthread within a mapped object works on SMT.**  A process mapped by
  core (or any object) and bound to hwthread binds to one hwthread of its
  object, one process per core, on topologies whose cores hold more than one
  PU.

* **Per-app policy lines in the map display.**  Each app's effective
  (resolved) mapping/ranking/binding policy is recorded during per-app dispatch
  as job-local attributes.  When two or more apps resolve to different
  policies, ``--display map`` (and ``map-devel``) emits an ``App N: Mapping
  policy …`` line per app instead of a single misleading job-level line; a
  single-policy job keeps the existing one-line format.  These per-app display
  attributes are never packed or sent off-node.

Every component must be updated to honour ``options->app_idx``.

Contract
~~~~~~~~

When ``options->app_idx >= 0``, the component processes **only** the app context
at that index in ``jdata->apps`` and returns ``PRTE_ERR_TAKE_NEXT_OPTION`` for any
app it cannot handle.  When ``options->app_idx < 0``, the component processes all
app contexts as today.

Required change in each component
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the top-level loop of each ``map_job`` function, replace:

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) continue;
       ...
   }

with:

.. code-block:: c

   for (n = 0; n < jdata->apps->size; n++) {
       app = pmix_pointer_array_get_item(jdata->apps, n);
       if (NULL == app) continue;
       /* honour per-app dispatch */
       if (options->app_idx >= 0 && n != options->app_idx) continue;
       ...
   }

This is the only mandatory change to each component.  All five components
require this change: ``round_robin``, ``ppr``, ``seq``, ``rank_file``, ``lsf``.

Component selection logic in the per-app path
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The mapping policy in ``options->map`` determines which component accepts the
app.  Each component's ``map_job`` already returns ``PRTE_ERR_TAKE_NEXT_OPTION``
when the policy is not one it handles.  No changes to per-component policy
checking are required beyond the ``app_idx`` guard above.

The existing component-selection priority ordering (determined by each
component's query priority) is preserved.

Changes to Ranking
------------------

``prte_rmaps_base_compute_vpids()`` in ``rmaps_base_ranking.c`` runs after all
apps have been placed.  It currently reads ``jdata->map->ranking`` to determine
the single global ranking strategy.

With per-app ranking the function signature becomes:

.. code-block:: c

   int prte_rmaps_base_compute_vpids(prte_job_t *jdata,
                                     prte_rmaps_options_t *options,
                                     int app_idx,
                                     uint32_t *next_vpid);

1. ``app_idx`` selects the app context to rank.  When ``app_idx < 0`` the
   function ranks all apps in one pass exactly as it does today (the
   job-level path passes ``-1``).

2. When called from the per-app loop it is invoked once per app with the
   app's resolved ``opts->rank`` (which honours that app's ``PRTE_APP_RANKBY``)
   and that app's index.

3. ``next_vpid`` carries the running global rank counter **between** per-app
   calls: each invocation begins assigning at ``*next_vpid`` and updates it to
   the first unassigned rank on return.  Global rank assignment is therefore
   still monotonically increasing across apps in app-index order, so
   ``pptr->name.rank`` values remain contiguous and non-overlapping across the
   whole job.

4. Per-app ranking controls only the **order** in which processes within
   that app are assigned their ranks relative to each other (by SLOT, NODE,
   FILL, or SPAN as that app requested); the starting rank for each app is the
   first unassigned rank after all previous apps.

Changes to Binding
------------------

No structural changes are required.  ``prte_rmaps_base_bind_proc()`` already
takes the per-call ``options`` struct.  Because ``prte_rmaps_base_setup_proc()``
is called from within each component's inner loop with the current ``options``
in scope, per-app binding is automatically derived from ``opts->bind`` which
was set by ``prte_rmaps_base_resolve_app_options()``.

The full binding directive is carried per app:

- ``opts->bind`` receives the app's binding object and the if-supported /
  overload directive bits decoded from ``PRTE_APP_BINDTO``.
- ``opts->limit`` receives the app's ``PRTE_APP_BINDING_LIMIT`` (the
  ``LIMIT=N`` modifier), defaulting to the job-level value when the app does
  not set one.
- ``opts->cpus_per_rank`` (``PRTE_APP_PES_PER_PROC``), ``opts->use_hwthreads``
  (``PRTE_APP_HWT_CPUS`` / ``PRTE_APP_CORE_CPUS``), and ``opts->cpuset``
  (``PRTE_APP_CPUSET``) are likewise resolved per app and feed the binder.

Thus an app may bind to a different object, with different overload/limit
behaviour, than its siblings in the same job.

Command-line / PMIx-spawn wiring
--------------------------------

Per-app directives reach the app context through the PMIx spawn machinery, not
through a schizo-only path.  The expected command-line representation is:

.. code-block:: sh

   prun app1 --map-by core : app2 --map-by node --rank-by fill

where ``:`` is the MPMD separator between app contexts.

The flow, end to end:

1. **Per-app parse** — ``src/prted/prte_app_parse.c`` splits the command line at
   each ``:`` (``prte_parse_locals()``) and parses each app segment in its own
   ``create_app()`` call.  Each segment's ``--map-by``/``--rank-by``/``--bind-to``
   is recorded on that app's ``pmix_app_t.info[]`` array as ``PMIX_MAPBY`` /
   ``PMIX_RANKBY`` / ``PMIX_BINDTO``.  Because each app is parsed independently,
   directives are already correctly scoped to their app context; no MPMD-aware
   schizo bookkeeping is required.

2. **Spawn** — the tool builds the ``pmix_app_t`` array (one entry per app, each
   carrying its own ``info[]``) and calls ``PMIx_Spawn``.  Whichever spawn
   assembly path is used (``src/prted/prte.c`` for the proxy HNP,
   ``src/prted/prun_common.c`` for the tool), each app's ``info`` must be
   converted from its own ``app->info`` list — not from the job-level info — so
   the per-app keys are preserved.

3. **Server-side translation** — ``src/prted/pmix/pmix_server_dyn.c``
   (``prte_pmix_xfer_app()``) walks each app's ``info[]`` and converts the
   ``PMIX_MAPBY`` / ``PMIX_RANKBY`` / ``PMIX_BINDTO`` keys into the
   ``PRTE_APP_*`` attributes by calling:

   .. code-block:: c

      prte_rmaps_base_set_app_mapping_policy(app, info->value.data.string);
      prte_rmaps_base_set_app_ranking_policy(app, info->value.data.string);
      prte_rmaps_base_set_app_binding_policy(app, info->value.data.string);

   These helpers parse the string and store the result with
   ``PRTE_ATTR_GLOBAL`` (see §"These attributes must be stored with
   ``PRTE_ATTR_GLOBAL``").  This is the same path used by a third-party caller
   of ``PMIx_Spawn`` that supplies ``PMIX_MAPBY`` etc. in a per-app ``info[]``
   array, so the CLI and the programmatic spawn API share one implementation.

4. **Job-level directives** — options given before any ``:`` apply to the whole
   job and continue to flow through the existing job-level
   ``prte_rmaps_base_set_mapping_policy()`` / ``set_ranking_policy()`` /
   ``set_binding_policy()`` functions, which store onto ``jdata->map``.  An app
   that carries no per-app directive inherits these.

No changes to ``src/mca/schizo/prte/`` are required for per-app map/rank/bind:
the option definitions already exist, and the per-app association happens in
``prte_app_parse.c``.

Tool-level argv pre-scan guards
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Before schizo runs, the tool launchers themselves walk the raw argv to
normalise option spellings.  ``prun`` (``src/tools/prun/prun.c``) and the ``prte``
HNP launcher (``src/prted/prte.c``) both rename ``--rank-by`` → ``--rankby`` and
``--bind-to`` → ``--bindto``, and both currently **reject a second occurrence** of
either option with the ``multi-instances`` help message.

Because a per-app MPMD command line repeats ``--rank-by``/``--bind-to`` once per
app context, this guard must be removed.  ``--rank-by`` and ``--bind-to`` are made
to behave like ``--map-by``, which is already renamed unconditionally with no
such guard.  Detecting an erroneous duplicate (two job-level ``--rank-by`` with
no intervening MPMD separator) is left to the schizo MPMD parser, which has the
app-context boundaries the flat argv pre-scan lacks.

Both launchers carry an identical copy of this pre-scan loop, so the shared
logic is factored into a single helper rather than relaxed twice:

.. code-block:: c

   /* src/mca/schizo/base/schizo_base_stubs.c */
   char *prte_schizo_base_normalize_argv(char **argv);

It renames all four deprecated option spellings (``--map-by``, ``--rank-by``,
``--bind-to``, ``--runtime-options``) in place and returns any ``--personality``
value found (a pointer into ``argv``, ``NULL`` if none).  ``prun`` and ``prte``
each replace their inline loop with a single call to it.

Migration Notes
---------------

Existing per-app PPR and PES_PER_PROC
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``PRTE_APP_PPR`` (25) and ``PRTE_APP_PES_PER_PROC`` (24) are already
stored on ``prte_app_context_t`` and checked in the ppr component.
Their handling is absorbed into the general ``PRTE_APP_MAPBY`` path:

- A standalone ``PRTE_APP_PPR`` without ``PRTE_APP_MAPBY`` continues to be
  read by the ppr component as today (backward compatible).
- ``PRTE_APP_MAPBY`` containing a ``ppr:N:obj`` spec stores into ``PRTE_APP_PPR``
  in addition to setting ``PRTE_APP_MAPBY = PRTE_MAPPING_PPR``.

``PRTE_JOB_FILE`` versus ``PRTE_APP_MAP_FILE``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The existing ``PRTE_JOB_FILE`` attribute stores the rankfile/seq file path on
the job.  The new ``PRTE_APP_MAP_FILE`` (29) stores it on an individual app
context.  The seq and rank_file components must be updated to check
``PRTE_APP_MAP_FILE`` on the current app before falling back to ``PRTE_JOB_FILE``
on the job.

Files Modified
--------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - File
     - Change
   * - ``src/util/attr.h``
     - Add ``PRTE_APP_MAPBY`` through ``PRTE_APP_BINDING_LIMIT`` (keys 26–34); raise ``PRTE_APP_MAX_KEY``
   * - ``src/mca/rmaps/rmaps_types.h``
     - Add ``app_idx`` field to ``prte_rmaps_options_t``
   * - ``src/mca/rmaps/base/base.h``
     - Declare new parsing and resolution functions
   * - ``src/mca/rmaps/base/rmaps_base_frame.c``
     - Add ``prte_rmaps_base_set_app_mapping_policy()``, ``prte_rmaps_base_set_app_ranking_policy()``, ``prte_rmaps_base_set_app_binding_policy()`` — storing every ``PRTE_APP_*`` attribute with ``PRTE_ATTR_GLOBAL`` (not ``LOCAL``)
   * - ``src/mca/rmaps/base/rmaps_base_map_job.c``
     - Add per-app detection, per-app dispatch loop, per-app ``compute_vpids`` calls, and ``prte_rmaps_base_resolve_app_options()`` (with ``prte_rmaps_base_derive_ranking()`` / ``derive_binding()`` defaulting, directive-bit masking, and map-derived field refresh)
   * - ``src/mca/rmaps/base/rmaps_base_ranking.c``
     - Add ``app_idx`` parameter to ``prte_rmaps_base_compute_vpids()``
   * - ``src/mca/rmaps/round_robin/rmaps_rr.c``
     - Add ``app_idx`` guard in app-context loop
   * - ``src/mca/rmaps/ppr/rmaps_ppr.c``
     - Add ``app_idx`` guard; remove duplicate per-app PPR/PES override (now handled centrally)
   * - ``src/mca/rmaps/seq/rmaps_seq.c``
     - Add ``app_idx`` guard; check ``PRTE_APP_MAP_FILE`` before ``PRTE_JOB_FILE``
   * - ``src/mca/rmaps/rank_file/rmaps_rank_file.c``
     - Add ``app_idx`` guard; check ``PRTE_APP_MAP_FILE`` before ``PRTE_JOB_FILE``
   * - ``src/mca/rmaps/lsf/rmaps_lsf.c``
     - Add ``app_idx`` guard
   * - ``src/prted/prte_app_parse.c``
     - Record per-app ``--map-by``/``--rank-by``/``--bind-to`` as ``PMIX_MAPBY``/``PMIX_RANKBY``/``PMIX_BINDTO`` on each app's ``info[]`` (already present)
   * - ``src/prted/pmix/pmix_server_dyn.c``
     - Translate per-app ``PMIX_MAPBY``/``RANKBY``/``BINDTO`` info into ``PRTE_APP_*`` attributes via the ``set_app_*_policy`` helpers (already present)
   * - ``src/prted/prte.c`` / ``src/prted/prun_common.c``
     - Ensure each ``pmix_app_t.info`` is built from that app's ``app->info`` (per-app), not the job-level info
   * - ``src/mca/schizo/base/schizo_base_stubs.c`` (and ``base.h``)
     - Add shared ``prte_schizo_base_normalize_argv()`` helper (no ``multi-instances`` guard) used by both tool launchers
   * - ``src/tools/prun/prun.c``
     - Replace inline argv pre-scan loop with a call to ``prte_schizo_base_normalize_argv()``
   * - ``src/prted/prte.c``
     - Same replacement (the HNP launcher's argv pre-scan was a copy of ``prun``'s)
   * - ``src/mca/rmaps/rmaps_types.h``
     - Rename version macro to ``PRTE_RMAPS_BASE_VERSION_5_0_0``; retain ``4_0_0`` as deprecated alias
   * - ``src/mca/rmaps/rmaps.h``
     - Rename module struct/typedef to ``prte_rmaps_base_module_5_0_0_t``
   * - ``src/mca/rmaps/round_robin/rmaps_rr_component.c``
     - Reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``src/mca/rmaps/ppr/rmaps_ppr_component.c``
     - Reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``src/mca/rmaps/seq/rmaps_seq_component.c``
     - Reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``src/mca/rmaps/rank_file/rmaps_rank_file_component.c``
     - Reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``src/mca/rmaps/lsf/rmaps_lsf_component.c``
     - Reference ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``test/unit/rmaps/`` (new directory)
     - Unit test suite: eight ``.c`` files + ``Makefile.am``
   * - ``test/unit/Makefile.am`` (new)
     - ``SUBDIRS = rmaps``
   * - ``test/Makefile.am`` (new)
     - ``SUBDIRS = unit``
   * - ``Makefile.am``
     - Add ``test`` to ``SUBDIRS`` (after ``src``)
   * - ``config/prte_config_files.m4``
     - Add ``test/Makefile``, ``test/unit/Makefile``, ``test/unit/rmaps/Makefile`` to ``AC_CONFIG_FILES``

Resolved Design Decisions
--------------------------

Oversubscription is job-level only
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``OVERSUBSCRIBE`` and ``NOOVERSUBSCRIBE`` are not per-app-context directives.
They govern whether the job as a whole is permitted to exceed node slot counts,
and that decision must be consistent across all apps sharing the same nodes.

Consequence: if a ``--map-by`` string supplied for an individual app context
includes an ``OVERSUBSCRIBE`` or ``NOOVERSUBSCRIBE`` modifier,
``prte_rmaps_base_set_app_mapping_policy()`` must treat it as an error, emit a
diagnostic via ``pmix_show_help()``, and return ``PRTE_ERR_BAD_PARAM``.  The
mapping event aborts with ``PRTE_JOB_STATE_MAP_FAILED``.

The ``PRTE_APP_MAPBY`` attribute must therefore never carry an oversubscription
modifier; those modifiers remain valid only in the job-level ``--map-by``
string where they are stored on ``jdata->map->mapping`` as today.

Display map is job-level only
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``prte_rmaps_base_display_map()`` is called once after all app contexts have
been mapped, and it displays the complete job map.  It is not meaningful to
display a partial map mid-loop.

``PRTE_JOB_DISPLAY_MAP`` and ``PRTE_JOB_DISPLAY_DEVEL_MAP`` remain job-level
attributes.  There are no per-app-context display-map attributes.

If a caller (e.g. schizo, PMIx spawn) sets a display-map directive on an
individual app context, ``prte_rmaps_base_map_job()`` must promote it to the
job level: after resolving all per-app options but before entering the
dispatch loop, scan the apps array and, if any app carries such a directive,
set ``PRTE_JOB_DISPLAY_MAP`` on ``jdata->attributes``.  The per-app copy of the
directive is then ignored.

Spawn inheritance is job-level only
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``INHERIT`` and ``NOINHERIT`` control whether a spawned child job copies its
parent's mapping/ranking/binding policies.  This is a property of the job as
a whole — a child either inherits from its parent or it does not.

Consequently, ``INHERIT`` and ``NOINHERIT`` modifiers are not permitted in a
per-app ``--map-by`` string.  ``prte_rmaps_base_set_app_mapping_policy()`` must
reject them with ``PRTE_ERR_BAD_PARAM`` and a diagnostic, aborting the mapping
event, exactly as it does for oversubscription modifiers.

If the CLI or PMIx spawn path presents ``INHERIT``/``NOINHERIT`` at the app level
(e.g., via per-app ``info[]`` keys), ``prte_rmaps_base_map_job()`` must scan all
apps before entering the dispatch loop and promote the directive to the job
level:

- If all apps that carry the directive agree (all ``INHERIT`` or all ``NOINHERIT``),
  apply it to ``jdata->attributes`` as ``PRTE_JOB_INHERIT`` or ``PRTE_JOB_NOINHERIT``.
- If any two apps carry conflicting directives, emit a diagnostic and abort
  with ``PRTE_JOB_STATE_MAP_FAILED``.

``NOLOCAL`` may be applied per app context
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``NOLOCAL`` (``PRTE_MAPPING_NO_USE_LOCAL``) directive prevents placement of an
app's processes on the HNP node.  It is meaningful on a per-app basis — one
app in a job may need to avoid the head node while another does not.

``NOLOCAL`` is therefore a valid modifier in a per-app ``--map-by`` string.
``prte_rmaps_base_set_app_mapping_policy()`` stores it as a directive bit within
the ``PRTE_APP_MAPBY`` uint16_t attribute (using ``PRTE_SET_MAPPING_DIRECTIVE``).

``prte_rmaps_base_resolve_app_options()`` propagates the bit into ``opts->map``
for the current app.  ``prte_rmaps_base_get_target_nodes()`` already tests
``PRTE_MAPPING_NO_USE_LOCAL`` in the mapping policy it receives; no further
changes to that function are required.

All ``--rank-by`` objects are valid per app
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Ranking has no job-wide-consistency requirement analogous to oversubscription
or inheritance: it only fixes the order in which an app's own processes receive
their global ranks.  Every ``--rank-by`` object — ``SLOT``, ``NODE``, ``FILL``,
``SPAN`` — is therefore accepted per app with no forbidden modifiers.

``prte_rmaps_base_set_app_ranking_policy()`` sets the ``PRTE_RANKING_GIVEN``
directive bit when it stores ``PRTE_APP_RANKBY``.  This is what lets
``prte_rmaps_base_resolve_app_options()`` distinguish an app that explicitly
requested a ranking from one that should fall back to the default derived from
its mapping policy (by-node mapping → by-node ranking, etc.).  An app that sets
``PRTE_APP_MAPBY`` but not ``PRTE_APP_RANKBY`` gets a ranking derived from its
own per-app mapping policy, not from the job-level mapping policy.

All ``--bind-to`` objects and modifiers are valid per app
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Binding is intrinsically a per-process property, so every ``--bind-to`` object
(``NONE``, ``HWTHREAD``, ``CORE``, ``L1CACHE``, ``L2CACHE``, ``L3CACHE``,
``NUMA``, ``PACKAGE``) and every binding modifier (``if-supported``,
``overload-allowed``, ``no-overload``, ``LIMIT=N``) is accepted per app with no
forbidden modifiers.

The ``no-overload`` modifier records ``PRTE_BIND_OVERLOAD_GIVEN`` without
``PRTE_BIND_ALLOW_OVERLOAD`` so that an app can explicitly forbid overload even
when the job-level default would have permitted it; ``resolve_app_options()``
copies these bits into ``opts->bind`` for the app, overriding the job-level
binding directive in its entirety rather than merging bit-by-bit.

Framework Version Increment
----------------------------

The ``app_idx`` field added to ``prte_rmaps_options_t`` changes the contract
between the base layer and every component: components are now required to
honour ``options->app_idx`` and skip apps that do not match.  This is a
breaking interface change for any out-of-tree component built against the
previous headers.

The framework version must be incremented from **4.0.0** to **5.0.0**:

.. code-block:: c

   /* src/mca/rmaps/rmaps_types.h */
   #define PRTE_RMAPS_BASE_VERSION_5_0_0 PRTE_MCA_BASE_VERSION_3_0_0("rmaps", 5, 0, 0)

The old macro ``PRTE_RMAPS_BASE_VERSION_4_0_0`` should be retained as a
deprecated alias pointing at the new value so that out-of-tree components
that have not been updated produce a link-time or runtime mismatch rather
than a silent ABI violation.

All five in-tree component files must be updated to reference
``PRTE_RMAPS_BASE_VERSION_5_0_0`` in their component struct:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Component file
     - Change
   * - ``round_robin/rmaps_rr_component.c``
     - ``PRTE_RMAPS_BASE_VERSION_4_0_0`` → ``PRTE_RMAPS_BASE_VERSION_5_0_0``
   * - ``ppr/rmaps_ppr_component.c``
     - same
   * - ``seq/rmaps_seq_component.c``
     - same
   * - ``rank_file/rmaps_rank_file_component.c``
     - same
   * - ``lsf/rmaps_lsf_component.c``
     - same

The module struct typedef and convenience alias in ``rmaps.h`` must also be
updated:

.. code-block:: c

   /* src/mca/rmaps/rmaps.h */
   struct prte_rmaps_base_module_5_0_0_t { ... };
   typedef struct prte_rmaps_base_module_5_0_0_t prte_rmaps_base_module_5_0_0_t;
   typedef prte_rmaps_base_module_5_0_0_t prte_rmaps_base_module_t;

Unit Tests
----------

PRRTE currently has no unit test suite for the rmaps framework.  This work
introduces non-trivial new logic in ``prte_rmaps_base_map_job()`` and
``prte_rmaps_base_resolve_app_options()`` that must be verified independently
of a live DVM.  A new unit test tree is required.

Offline end-to-end verification (no launch)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In addition to the unit tests below, the **whole** per-app path — CLI parse →
``pmix_app_t.info[]`` → ``PRTE_APP_*`` attributes → ``map_job`` dispatch →
placement/ranking/binding — can and must be exercised end to end without
launching anything:

.. code-block:: sh

   prterun --rtos donotlaunch --display map \
           --prtemca hwloc_use_topo_file test/topologies/test-topo.xml \
           -H node0:N,node1:M,node2:L \
           --map-by node -n 4 hostname : --map-by slot --rank-by node -n 4 hostname

``--rtos donotlaunch`` runs the mapper/ranker/binder and prints the map without
forking any process; ``--prtemca hwloc_use_topo_file`` supplies a simulated
node topology (so binding resolves against real objects); ``-H`` declares the
simulated nodes (slot counts only need to be ≥ the procs placed on each node).
The printed map shows each process's app index, rank, and bound object.  See
the "Testing the mapper without launching" section of ``AGENTS.md`` for the
full description.

This offline check is what catches the class of failure described in
§"These attributes must be stored with ``PRTE_ATTR_GLOBAL``": a per-app
directive that parses correctly but is silently dropped before mapping shows up
immediately here as an app whose placement/rank/binding does not change when
its per-app policy changes.  Verification must include a **multi-app** MPMD
case — a single-app job can pass even when per-app attributes are being lost,
because its lone directive coincides with the job-level policy.

This manual check is automated by the ``test/offline/`` harness
(``run_offline_maps.py``), which ``make check`` runs.  It drives the same
``prterun --rtos donotlaunch --display map`` command over a
``--map-by``/``--rank-by``/``--bind-to`` matrix crossed with every hwloc XML in
``test/topologies/`` (currently ``test-topo.xml`` and the SMT ``test-topo2.xml``),
verifies each map against topology-derived invariants, and pins the per-app
(``perapp.*``), ppr, and multi-app cases to golden snapshots under
``test/offline/golden/``.  The multi-app per-app cases live there as
``perapp.<topo>.map.node-slot``, ``perapp.<topo>.rank.node-slot``,
``perapp.<topo>.bind.numa-core``, and ``perapp.<topo>.map.three``.  See
``test/offline/README.rst`` to run it by hand or add a topology.

Location
~~~~~~~~~

.. code-block:: text

   test/unit/rmaps/
       Makefile.am
       test_rmaps_main.c          — harness: init/finalize, run all suites
       test_resolve_options.c     — prte_rmaps_base_resolve_app_options()
       test_policy_parse.c        — prte_rmaps_base_set_app_mapping_policy() etc.
       test_dispatch.c            — per-app dispatch loop in map_job
       test_round_robin.c         — round_robin component with app_idx
       test_ppr.c                 — ppr component with app_idx
       test_seq.c                 — seq component with app_idx
       test_rank_file.c           — rank_file component with app_idx

``test/unit/rmaps/`` is added to the ``SUBDIRS`` list in ``test/unit/Makefile.am``
(creating that file if it does not yet exist) and wired into the top-level
``make check`` target.

Test harness requirements
~~~~~~~~~~~~~~~~~~~~~~~~~~

The tests link against the PRRTE static libraries but do not require a running
DVM.  They use the same minimal-init pattern as the PMIx unit tests: call
``prte_init()`` in server-tool mode, bypassing daemon launch.  A lightweight
stub replaces ``PRTE_ACTIVATE_JOB_STATE`` for the mapping-failed path so tests
can assert the failure code without triggering the state machine.

Coverage required per test file
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**``test_policy_parse.c``** — ``prte_rmaps_base_set_app_mapping_policy()``,
``prte_rmaps_base_set_app_ranking_policy()``, ``prte_rmaps_base_set_app_binding_policy()``:

*Mapping* (``set_app_mapping_policy``):

- Valid single-word policies (``core``, ``node``, ``slot``, ``ppr:2:core``, ``hwthread``, etc.) store the correct uint16_t in ``app->attributes``.
- All valid modifiers (``NOLOCAL``, ``PE=N``, ``ORDERED``, ``HWTCPUS``, ``CORECPUS``, ``FILE=path``) parse and store correctly.
- Forbidden modifiers (``OVERSUBSCRIBE``, ``NOOVERSUBSCRIBE``, ``INHERIT``, ``NOINHERIT``) return ``PRTE_ERR_BAD_PARAM``.
- Malformed strings (missing value after ``=``, unknown keyword, conflicting ``HWTCPUS``/``CORECPUS``) return appropriate error codes.

*Ranking* (``set_app_ranking_policy``):

- Each object (``slot``, ``node``, ``fill``, ``span``) stores the matching ``PRTE_RANK_BY_*`` value in ``PRTE_APP_RANKBY`` with ``PRTE_RANKING_GIVEN`` set.
- An unrecognized object returns ``PRTE_ERR_SILENT`` and leaves ``app->attributes`` unchanged.

*Binding* (``set_app_binding_policy``):

- Each object (``none``, ``hwthread``, ``core``, ``l1cache``, ``l2cache``, ``l3cache``, ``numa``, ``package``) stores the matching ``PRTE_BIND_TO_*`` value in ``PRTE_APP_BINDTO``.
- Modifiers ``if-supported`` and ``overload-allowed`` set ``PRTE_BIND_IF_SUPPORTED`` / ``PRTE_BIND_ALLOW_OVERLOAD`` (with ``PRTE_BIND_OVERLOAD_GIVEN``); ``no-overload`` sets ``PRTE_BIND_OVERLOAD_GIVEN`` without the allow bit.
- ``LIMIT=N`` stores ``N`` as ``PRTE_APP_BINDING_LIMIT``; a non-numeric ``LIMIT`` value returns ``PRTE_ERR_SILENT``.
- An unrecognized object or modifier returns ``PRTE_ERR_BAD_PARAM``.

**``test_resolve_options.c``** — ``prte_rmaps_base_resolve_app_options()``:

- App with no per-app attributes: ``app_options`` is identical to the job-level ``options``.
- App with ``PRTE_APP_MAPBY`` set: ``opts->map`` reflects the app value, not the job value.
- App with ``PRTE_APP_NOLOCAL`` directive bit set: ``opts->map`` carries ``PRTE_MAPPING_NO_USE_LOCAL``.
- App with ``PRTE_APP_PES_PER_PROC`` / ``PRTE_APP_HWT_CPUS`` / ``PRTE_APP_CPUSET``: correct override.
- Fallback chain: ``PRTE_APP_PPR`` absent → ``PRTE_JOB_PPR`` used.

**``test_dispatch.c``** — the per-app detection and dispatch loop in ``prte_rmaps_base_map_job()``:

- Job with no per-app directives: single-dispatch path taken (verified by mock component call count = 1).
- Job with at least one app carrying ``PRTE_APP_MAPBY``: per-app path taken; mock component called once per app.
- ``OVERSUBSCRIBE`` in a per-app ``--map-by`` string: mapping aborts with ``PRTE_JOB_STATE_MAP_FAILED``.
- Conflicting ``INHERIT``/``NOINHERIT`` across apps: mapping aborts.
- Any app with display-map directive: ``PRTE_JOB_DISPLAY_MAP`` promoted to job level.
- **``NOLOCAL`` on app[0], not on app[1], shared HNP node** (see below).

**``test_round_robin.c``**, **``test_ppr.c``**, **``test_seq.c``**, **``test_rank_file.c``**:

Each file tests its component with the ``app_idx`` field in both modes:

- ``app_idx = -1``: component maps all apps (baseline, existing behaviour).
- ``app_idx = 0`` on a two-app job: only app[0] is mapped; app[1] procs remain unplaced.
- ``app_idx = 1`` on a two-app job: only app[1] is mapped; app[0] procs remain unplaced.
- ``app_idx`` set to an index with ``NULL`` in the apps array: component skips gracefully.

Dedicated test: ``NOLOCAL`` on app[0] with shared HNP node
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Purpose.**  Verify that excluding the HNP node from app[0]'s target list
via ``NOLOCAL`` leaves no persistent side-effect on the ``prte_node_t`` that
would prevent app[1] (which carries no ``NOLOCAL``) from placing processes on
that same node.

**Setup.**  Construct a synthetic allocation of three nodes: the HNP node
(``node0``, rank 0) and two worker nodes (``node1``, ``node2``).  Create a job with
two app contexts:

- ``app[0]``: 4 processes, ``PRTE_APP_MAPBY = PRTE_MAPPING_BYSLOT`` with
  ``PRTE_MAPPING_NO_USE_LOCAL`` set in the directive bits.
- ``app[1]``: 2 processes, ``PRTE_APP_MAPBY = PRTE_MAPPING_BYSLOT``, no
  ``NOLOCAL``.

**Execution.**  Run the per-app dispatch loop (the same code path exercised by
``prte_rmaps_base_map_job()`` when ``any_per_app`` is true).

**Assertions.**

1. None of app[0]'s four processes are assigned to ``node0``.  All four land on
   ``node1`` and ``node2``.
2. At least one of app[1]'s two processes is assigned to ``node0``, confirming
   that the HNP node was not permanently marked as excluded or had its slot
   count incorrectly zeroed by app[0]'s mapping pass.
3. ``jdata->map->nodes`` contains all three nodes (the job map is the union of
   nodes used by any app).
4. ``node0``'s available slot count after both apps have been mapped reflects
   only the slots consumed by app[1]'s processes, not a spurious reduction
   from app[0].

**What this catches.**  If ``prte_rmaps_base_get_target_nodes()`` constructs its
target list by removing nodes in-place from a shared list, or if it sets a
persistent flag on ``prte_node_t`` (e.g., modifying ``node->slots_inuse`` or a
"do not use" flag without resetting it), app[1]'s target list will be missing
``node0`` and assertion 2 will fail.  The test thereby confirms that node list
construction is stateless with respect to per-app ``NOLOCAL`` decisions.

``Makefile.am`` for the test suite
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: makefile

   # test/unit/rmaps/Makefile.am
   #
   # Copyright (c) 2026      Nanook Consulting  All rights reserved.
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

``make check`` Integration
--------------------------

PRRTE currently has no ``make check`` target.  This section specifies everything
required to wire the new unit test suite into the Automake check framework so
that ``make check`` builds and runs the tests from the top of the source tree.

1. Add ``test/unit/`` to the build tree
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``test/unit/Makefile.am``:

.. code-block:: makefile

   # test/unit/Makefile.am
   #
   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   SUBDIRS = rmaps

Create ``test/Makefile.am``:

.. code-block:: makefile

   # test/Makefile.am
   #
   # $COPYRIGHT$
   # Additional copyrights may follow
   # $HEADER$

   SUBDIRS = unit

2. Wire ``test/`` into the top-level build
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In ``Makefile.am``, add ``test`` to ``SUBDIRS``:

.. code-block:: makefile

   # Makefile.am  (excerpt — existing SUBDIRS line)
   SUBDIRS = config contrib src include docs test

The ``test`` entry must come after ``src`` so that ``libprrte.la`` is built before
the test programs that link against it.

3. Register ``test/`` Makefiles in the configure system
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In ``config/prte_config_files.m4``, extend the ``AC_CONFIG_FILES`` call:

.. code-block:: bash

   AC_DEFUN([PRTE_CONFIG_FILES],[
       AC_CONFIG_FILES([
           src/Makefile
           ...existing entries...
           test/Makefile
           test/unit/Makefile
           test/unit/rmaps/Makefile
       ])
   ])

4. Automake check mechanics
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Automake's ``make check`` target automatically builds everything listed in
``check_PROGRAMS`` and then runs everything listed in ``TESTS``.  Because
``TESTS = test_rmaps`` is set in ``test/unit/rmaps/Makefile.am``, running
``make check`` from the top of the build tree will:

1. Build ``test_rmaps`` (and its dependency ``libprrte.la`` if not already built).
2. Execute ``./test_rmaps``.
3. Report pass/fail based on the exit code.

No additional Automake variables or test-driver configuration are required for
a simple binary-exit-code test.  If a TAP-based driver is preferred in future,
``AM_TESTS_ENVIRONMENT`` and ``LOG_DRIVER`` can be added at that point without
changing the test source files.

5. ``autogen.pl`` / ``configure`` impact
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Adding ``test/Makefile.am``, ``test/unit/Makefile.am``, and
``test/unit/rmaps/Makefile.am`` to the source tree and listing them in
``config/prte_config_files.m4`` is sufficient.  No new ``configure.ac`` macros
are needed.  Developers must re-run ``./autogen.pl && ./configure`` after
pulling these files for the first time.

6. Isolation from normal builds
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The test programs are listed under ``check_PROGRAMS``, not ``bin_PROGRAMS`` or
``noinst_PROGRAMS``.  Automake only builds ``check_PROGRAMS`` when ``make check``
is explicitly invoked; a plain ``make`` or ``make install`` does not build them.
This keeps the normal build fast and does not install test binaries.

7. Developer workflow
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: sh

   # After configure:
   make -j$(nproc)          # normal build, does not compile tests
   make check               # build and run all unit tests
   make check -C test/unit/rmaps   # run only the rmaps suite

Open Questions
--------------

None.
