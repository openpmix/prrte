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
the ``prun`` command line.  Which app a ``--map-by``, ``--rank-by`` or
``--bind-to`` describes follows one rule:

**The first app segment is where the command line speaks for the job.**  A
directive written there and nowhere else applies to the whole job, however
many apps follow — it is the same thing a single-app command line says, and
it is the only place a qualifier that spans the job (see the next section)
can be written without being taken off an app again.

**A directive written on any later app describes that app, and only that
app.**  Apps that were given none are not agreeing with it — they said
nothing, and what an app that says nothing gets is the ordinary default
resolution.

.. code-block:: sh

   # both apps mapped by core: the only directive is on the first app
   prun --map-by core -n 4 app1 : -n 2 app2

   # app1 mapped by core, app2 mapped by node and ranked by fill
   prun --map-by core -n 4 app1 : --map-by node --rank-by fill -n 2 app2

   # app1 takes the default mapping; only app2 is mapped by node
   prun -n 4 app1 : --map-by node -n 2 app2

   # app1 avoids the head node; app2 can use all nodes
   prun --map-by slot:nolocal -n 8 app1 : --map-by slot -n 2 app2

   # app2 is placed by a rankfile of its own; app1 takes the default mapping
   prun -n 3 app1 : --map-by rankfile:file=/path/to/rfile -n 2 app2

The three classes are decided independently: a ``--map-by`` on the first app
can describe the job while a ``--rank-by`` on the third app describes only
that app.

Any ``--map-by`` qualifier that is valid at the job level is also valid per
app, and every mapping policy is — ``seq``, ``rankfile``, ``ppr:N:obj`` and
``pe-list=…`` included, each with the file or pattern it needs.  So two apps
of one job can be placed by two different mapping components; ``--display
devel-map`` names the component that placed each one.  A few qualifiers
describe the job rather than the app, and are handled as described in the
next section.


Job-Level Directives Written On An App
--------------------------------------

Some qualifiers are properties of the job as a whole.  They may still be
written in a per-app ``--map-by`` string — and on a multi-app command line
there may be nowhere else to write them, since once the apps carry their own
directives there is no job-level one left to hang a qualifier on.  So PRRTE
takes such a qualifier off the app that carried it and applies it to the
job.

What cannot be honored is app contexts that answer the same question in
opposite ways; that is refused, and the job aborts with
``PRTE_JOB_STATE_MAP_FAILED``.  Apps that say nothing are silent, not
dissenting: it is enough that the apps which do give the qualifier agree with
each other, and with any job-level directive.

``OVERSUBSCRIBE`` / ``NOOVERSUBSCRIBE``
    Oversubscription governs whether the job as a whole may exceed node slot
    counts.  Because multiple app contexts share the same nodes, this decision
    has to be the same for all of them.

    .. code-block:: sh

       # legal: the qualifier is written once and applies to the whole job
       prun --map-by slot:oversubscribe -n 14 app1 : --map-by node -n 14 app2

       # refused: the two apps ask for opposite things
       prun --map-by core:oversubscribe -n 4 app1 : \
            --map-by node:nooversubscribe -n 4 app2

``INHERIT`` / ``NOINHERIT``
    These modifiers control whether a spawned child job copies its parent's
    placement policies.  This is a job-level property, promoted to the job in
    the same way, whether it arrives on the command line or in a per-app
    ``info[]`` array on the PMIx spawn path.

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
   prun --map-by slot:nolocal -n 8 app1 : --map-by slot -n 1 app2

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

``PMIX_PPR`` may also be given per app.

When these keys appear in a per-app ``info[]`` array (rather than in the
job-level ``info[]`` array), PRRTE stores them as per-app attributes on the
corresponding ``prte_app_context_t`` and routes them through the same per-app
dispatch path as the command-line case.  When the same keys appear in the
job-level ``info[]`` array, they continue to set the job-level policy as
before.  On this path there is no "first app" rule: the array a key was
written in says what it describes.

``PMIX_MAPPER`` is **not supported**, per job or per app.  A spawn request
carrying it is refused with ``PMIX_ERR_NOT_SUPPORTED``, and it is not listed
in the ``PMIX_QUERY_SPAWN_SUPPORT`` answer.  Naming a mapping component says
nothing ``PMIX_MAPBY`` has not already said — the mapping policy is what
selects the component, since each one claims the policies it implements —
and the two can contradict each other with no answer that could be right.


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
   ``PRTE_ERR_TAKE_NEXT_OPTION`` for those it cannot handle.  A component
   decides whether the app is for it from the *resolved* options —
   ``options->map`` and ``options->mapgiven`` — not from the job-level
   policy, which is why a per-app ``seq``, ``rankfile`` or ``ppr`` reaches
   its own component.  Different app contexts of one job may
   therefore be placed by different components; the component that placed
   each one is recorded on the app and shown by ``--display devel-map``.

#. **Rank assignment** — ``prte_rmaps_base_compute_vpids()`` is called once
   per app context after placement, with the app index and a running vpid
   counter so that global rank values remain contiguous and non-overlapping
   across the whole job.  Per-app ranking controls only the *order* in which
   processes within that app are assigned ranks relative to each other; the
   starting rank for each app is always the first rank not yet assigned by any
   previous app.  The mappers that number their own processes — ``rank_file``,
   ``seq`` and ``lsf`` — are handed that same counter, so a per-app rankfile
   or sequence file numbers *that app's* ranks and PRRTE offsets them into the
   job's numbering.

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

``PRTE_APP_PPR`` (25) holds this app's whole ``N:object`` pattern, in the
same spelling as the job-level ``PRTE_JOB_PPR`` — the object is as much a
part of what the app asked for as the count.  When a per-app ``--map-by``
string contains a ``ppr:N:obj`` specification, that pattern is written to
``PRTE_APP_PPR`` in addition to setting ``PRTE_APP_MAPBY =
PRTE_MAPPING_PPR``, so that the ``ppr`` mapping component can read it through
the standard path.  ``PRTE_APP_PES_PER_PROC`` (24) is unchanged.

Two further attributes are recorded by the rmaps base rather than supplied by
the user, are local to the HNP, and are never packed or sent off-node:
``PRTE_APP_RESOLVED_MAPBY`` / ``RANKBY`` / ``BINDTO`` (35-37), the policies
this app was actually placed with, and ``PRTE_APP_LAST_MAPPER`` (38), the
component that placed it.  ``--display devel-map`` reads all four.

``PRTE_APP_LAST_MAPPER`` is the *only* record PRRTE keeps of which component
mapped anything; ``prte_job_map_t`` carries no mapper name at all.  It is
per-app because with each app's own policy deciding which component claims
it, two apps of one job can be placed by two different mappers — a single
job-level name could only ever be half the answer — and because mapping
happens on the HNP alone, so nothing off that daemon has any use for it.


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
