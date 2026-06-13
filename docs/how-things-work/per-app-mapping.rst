.. _per-app-mapping-label:

Per-App-Context Mapping
=======================

By default, every application context (``prte_app_context_t``) within a job
is placed using the same mapping, ranking, and binding policy — the one
specified at the job level via ``--map-by``, ``--rank-by``, and ``--bind-to``.
Per-app-context mapping allows each application context in a multi-program
multiple-data (MPMD) job to carry its own independent set of placement
directives.


When to Use It
--------------

Per-app-context mapping is useful when different components of a coupled
application have meaningfully different hardware affinity requirements.  For
example:

* A compute kernel that should be mapped by core and bound tightly to those
  cores.
* A communication or I/O helper that should be mapped by node and left
  unbound.
* A utility process that must not run on the head node (``NOLOCAL``), while
  the rest of the job can use all nodes.

Without per-app mapping, satisfying these requirements would require launching
multiple separate jobs with separate ``prun`` invocations, losing the ability
to use shared memory and direct PMIx communication between the components.


Command-Line Syntax
-------------------

Per-app directives are specified using the standard MPMD separator (``:``) on
the ``prun`` command line.  Each ``--map-by``, ``--rank-by``, and ``--bind-to``
option that appears after a ``:`` separator applies only to the application
context that follows it.  Options that appear before the first ``:`` separator
continue to apply at the job level.

.. code-block:: sh

   # app1 mapped by core, app2 mapped by node and ranked by fill
   prun -n 4 app1 --map-by core : -n 2 app2 --map-by node --rank-by fill

   # app1 avoids the head node; app2 can use all nodes
   prun -n 8 app1 --map-by slot:nolocal : -n 2 app2 --map-by slot

   # app1 uses a rankfile for precise placement; app2 uses default slot mapping
   prun -n 3 app1 --map-by rankfile:file=/path/to/rfile : -n 2 app2

Any ``--map-by`` qualifier that is valid at the job level is also valid per
app, with the following exceptions (described in the next section).


Job-Level-Only Directives
-------------------------

Some directives are properties of the job as a whole and cannot be applied
per app context:

``OVERSUBSCRIBE`` / ``NOOVERSUBSCRIBE``
    Oversubscription governs whether the job as a whole may exceed node slot
    counts.  Because multiple app contexts share the same nodes, this decision
    must be consistent across all apps.  Specifying ``OVERSUBSCRIBE`` or
    ``NOOVERSUBSCRIBE`` in a per-app ``--map-by`` string is an error and will
    cause the job to abort with ``PRTE_JOB_STATE_MAP_FAILED``.

``INHERIT`` / ``NOINHERIT``
    These modifiers control whether a spawned child job copies its parent's
    placement policies.  This is a job-level property.  If the PMIx spawn path
    supplies ``INHERIT`` or ``NOINHERIT`` in per-app ``info[]`` arrays, PRRTE
    will attempt to promote the directive to the job level.  If different app
    contexts carry conflicting directives (one ``INHERIT`` and another
    ``NOINHERIT``), the job will abort with ``PRTE_JOB_STATE_MAP_FAILED``.

``--display-map`` / ``--display-devel-map``
    The job map is displayed once after all app contexts have been placed.
    A display-map directive found on any individual app context is promoted to
    the job level automatically; displaying a partial mid-loop map is not
    supported.


Per-App ``NOLOCAL``
-------------------

The ``NOLOCAL`` modifier (``PRTE_MAPPING_NO_USE_LOCAL``) prevents an app's
processes from being placed on the head node (HNP).  Unlike the job-level-only
directives above, ``NOLOCAL`` *is* permitted per app context and takes effect
only for the app that carries it.

This means one app in a job can avoid the head node while other apps in the
same job can use it:

.. code-block:: sh

   # app1 will not run on the head node; app2 may
   prun -n 8 app1 --map-by slot:nolocal : -n 1 app2 --map-by slot

Internally, ``NOLOCAL`` is stored as a directive bit within the
``PRTE_APP_MAPBY`` attribute on the ``prte_app_context_t``.  The node-list
construction performed by ``prte_rmaps_base_get_target_nodes()`` reads this
bit for each app independently, so the exclusion of the head node does not
affect subsequent app contexts that do not carry the bit.


PMIx Spawn Path
---------------

Per-app placement directives can also be supplied via the ``PMIx_Spawn`` API
using the per-app ``info[]`` array on each ``pmix_app_t``.  The relevant PMIx
keys are:

* ``PMIX_MAPBY`` — equivalent to ``--map-by``
* ``PMIX_RANKBY`` — equivalent to ``--rank-by``
* ``PMIX_BINDTO`` — equivalent to ``--bind-to``

When these keys appear in a per-app ``info[]`` array (rather than in the
job-level ``info[]`` array), PRRTE stores them as per-app attributes on the
corresponding ``prte_app_context_t`` and routes them through the same per-app
dispatch path as the command-line case.  When the same keys appear in the
job-level ``info[]`` array, they continue to set the job-level policy as
before.


Inheritance and Fallback
------------------------

An app context that carries no per-app directives inherits the job-level
policy without modification.  Partial overrides are supported: if an app
specifies only ``--map-by``, it inherits the job-level ``--rank-by`` and
``--bind-to``.

The inheritance chain for each field is:

#. Per-app attribute on ``prte_app_context_t`` (highest priority)
#. Job-level value from ``jdata->map`` / ``jdata->attributes``
#. PRRTE system default

This resolution is performed by ``prte_rmaps_base_resolve_app_options()``
immediately before each app context is dispatched to a mapping component.


How the Dispatch Works
----------------------

The standard single-dispatch path (in which one mapping component processes
all app contexts in a single ``map_job()`` call) is preserved unchanged for
jobs that carry no per-app directives.

When at least one app context carries a per-app ``PRTE_APP_MAPBY``,
``PRTE_APP_RANKBY``, or ``PRTE_APP_BINDTO`` attribute, ``prte_rmaps_base_map_job()``
switches to a per-app loop:

#. **Resolve options** — ``prte_rmaps_base_resolve_app_options()`` builds a
   per-app copy of the ``prte_rmaps_options_t`` struct, starting from the
   job-level defaults and overriding with any per-app attributes.  The field
   ``app_options.app_idx`` is set to the index of the current app context.

#. **Select component** — the same component selection loop is used as in the
   single-dispatch path.  Each component's ``map_job()`` is called with
   ``app_options``.  Because ``app_options.app_idx >= 0``, each component skips
   any app context whose index does not match, returning
   ``PRTE_ERR_TAKE_NEXT_OPTION`` for those it cannot handle.

#. **Rank assignment** — ``prte_rmaps_base_compute_vpids()`` is called once
   per app context after placement, with the app index and a running vpid
   counter so that global rank values remain contiguous and non-overlapping
   across the whole job.  Per-app ranking controls only the *order* in which
   processes within that app are assigned ranks relative to each other; the
   starting rank for each app is always the first rank not yet assigned by any
   previous app.

#. **Binding** — no structural changes are required.  Because
   ``prte_rmaps_base_setup_proc()`` is called from within each component's
   inner loop with the current ``opts`` in scope, per-app binding is
   automatically derived from the ``opts->bind`` value set by
   ``prte_rmaps_base_resolve_app_options()``.

The complete job map is the union of nodes used by all app contexts.
``prte_rmaps_base_display_map()`` is called once at the end, after all app
contexts have been placed, and displays this complete map.


Attribute Storage
-----------------

Per-app directives are stored as attributes on ``prte_app_context_t`` using
the following keys (defined in ``src/util/attr.h``):

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - Attribute key
     - PMIx type
     - Meaning
   * - ``PRTE_APP_MAPBY`` (26)
     - ``PMIX_UINT16``
     - Parsed mapping policy enum value; directive bits (e.g., ``NOLOCAL``) are
       encoded in the upper bits using ``PRTE_SET_MAPPING_DIRECTIVE``
   * - ``PRTE_APP_RANKBY`` (27)
     - ``PMIX_UINT16``
     - Parsed ranking policy enum value
   * - ``PRTE_APP_BINDTO`` (28)
     - ``PMIX_UINT16``
     - Parsed binding policy enum value
   * - ``PRTE_APP_MAP_FILE`` (29)
     - ``PMIX_STRING``
     - Path to the sequential or rankfile for this app; takes precedence over
       the job-level ``PRTE_JOB_FILE`` in the ``seq`` and ``rank_file``
       components
   * - ``PRTE_APP_DIST_DEVICE`` (30)
     - ``PMIX_STRING``
     - Device name for distance-based mapping (e.g., ``mlx5_0``)
   * - ``PRTE_APP_HWT_CPUS`` (31)
     - ``PMIX_BOOL``
     - Use hardware threads as CPUs for this app
   * - ``PRTE_APP_CORE_CPUS`` (32)
     - ``PMIX_BOOL``
     - Use cores as CPUs for this app
   * - ``PRTE_APP_CPUSET`` (33)
     - ``PMIX_STRING``
     - Comma-delimited CPU ranges for ``PE-LIST`` mapping
   * - ``PRTE_APP_BINDING_LIMIT`` (34)
     - ``PMIX_UINT16``
     - Maximum number of processes to bind to a single target object before
       moving to the next

The existing ``PRTE_APP_PPR`` (25) and ``PRTE_APP_PES_PER_PROC`` (24)
attributes are unchanged.  When a per-app ``--map-by`` string contains a
``ppr:N:obj`` specification, the parsed N value is written to ``PRTE_APP_PPR``
in addition to setting ``PRTE_APP_MAPBY = PRTE_MAPPING_PPR``, so that the
``ppr`` mapping component can read it through the standard path.


Framework Version
-----------------

The addition of ``app_idx`` to ``prte_rmaps_options_t`` is a breaking interface
change for any mapping component.  All components must now honour the
``options->app_idx`` field: when it is ``>= 0``, the component must process
only the app context at that index.  The rmaps framework version was therefore
incremented from ``4.0.0`` to ``5.0.0``
(``PRTE_RMAPS_BASE_VERSION_5_0_0``).  Out-of-tree components built against
the older headers will produce a version mismatch at load time rather than
silently exhibiting incorrect behavior.
