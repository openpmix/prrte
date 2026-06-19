Node Reservation and Session Targeting
======================================

Overview
--------

PRRTE already owns the data structures needed to partition an allocation
into independently mappable pools.  Each ``prte_job_t`` carries a
``prte_session_t *session`` (``src/runtime/prte_globals.h``), and the mapper
draws a job's candidate nodes exclusively from ``jdata->session->nodes`` (see
``prte_rmaps_base_get_target_nodes`` in
``src/mca/rmaps/base/rmaps_base_support_fns.c``).  The "default" session
created at startup points its ``nodes`` array directly at the global
``prte_node_pool`` (``src/runtime/prte_init.c``), so jobs assigned to the
default session can map onto every node in the allocation.

What is missing is the machinery to (a) place *dynamically* allocated nodes
into a session other than the default, and (b) let a spawn request select
which session(s) — i.e. which allocation(s) — its processes may use.  Today
``prte_ras_base_complete_request`` (``src/mca/ras/base/ras_base_allocate.c``)
unconditionally inserts every newly allocated node into the global pool via
``prte_ras_base_node_insert(&ndlist, NULL)``, making all dynamic resources
visible to every job.

This document specifies the changes required to:

1. Treat the original startup allocation as the default session (no change to
   observable behavior — documented here for completeness).
2. Honor ``PMIX_ALLOC_TARGET`` and ``PMIX_ALLOC_SHARE`` on dynamic
   allocation requests to decide which session receives the new nodes.
3. Honor a new ``PMIX_SPAWN_TARGET`` attribute on spawn requests to select the
   union of allocations a job may map onto.

All three PMIx keys are optional and **must** be compiled out cleanly when the
installed PMIx headers predate them, preserving backward compatibility.

Goals
-----

1. Nodes belong to exactly one session.  Unreserved nodes belong to the
   default session; reserved nodes belong to the session of the namespace (or
   allocation) that reserved them.
2. A job maps only onto nodes owned by the session(s) it targets.  With no
   target specified, behavior is unchanged: the job uses the default session.
3. Reservation decisions are driven entirely by PMIx attributes supplied by
   the requester (tool or application); PRRTE invents no policy of its own
   beyond the defaults specified below.
4. Code that references ``PMIX_ALLOC_TARGET``, ``PMIX_ALLOC_SHARE``, or
   ``PMIX_SPAWN_TARGET`` is guarded so a PMIx that lacks any of these keys
   still builds warning-free and behaves as it does today.

Terminology
-----------

Throughout this document an **allocation** is identified by its
``PMIX_ALLOC_ID`` string, which PRRTE stores as ``session->alloc_refid``.  A
**reservation** is a non-default ``prte_session_t`` whose nodes are withheld
from the default pool and made available only to the owning namespace or to
jobs that explicitly target the allocation.  The **invalid namespace** is the
sentinel (``PMIX_NSPACE_INVALID`` / an empty allocation id) that, when used as
a spawn target, denotes the default session.

A word on jobs and namespaces, since the two are sometimes conflated.  **Every
job is its own namespace**, and a namespace maps to exactly one job: there is
no way to run job A and later run job B under the same namespace.  All app
contexts within a single job share that one namespace.  Two distinct jobs are
always two distinct namespaces, even when they run in the same session.
Consequently, "the namespace's reservation" always means "this one job's
reservation," and any process within a job (i.e. within that namespace) may
spawn new jobs onto resources reserved for its namespace.

A reservation therefore has a **set of owning namespaces**, not a single owner.
The set is seeded with the namespace the reservation is created for, and it
grows by one each time a job is spawned *into* the reservation: a child job
spawned into a reservation becomes an owner of that reservation (so it, in
turn, may spawn further jobs onto those nodes).  Ownership is **not inherited**
transitively — a child owns only the reservation it was spawned into, not any
*other* reservations its parent happens to own.  The scheduler is treated as an
implicit owner of every session.

The Three Allocation Cases
--------------------------

Case 1 — Original allocation detected at DVM startup
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The nodes discovered by the RAS at startup form the default session.  This is
already implemented: ``prte_init.c`` constructs ``prte_default_session`` with
``session_id == UINT32_MAX`` and aliases its ``nodes`` array to
``prte_node_pool``.  Every job that does not target a reservation is assigned
this session and may use any node in it.  **No code change is required**; the
behavior is restated here because it anchors the semantics of the other cases.

Case 2 — Dynamic allocation requested by a tool
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A tool (a PMIx process with no session of its own — e.g. the scheduler or a
``prun``-class client) issues ``PMIx_Allocation_request``.  The new nodes are
routed as follows:

* ``PMIX_ALLOC_TARGET`` given (a target namespace): the new nodes are reserved
  to that namespace.  A ``PMIX_ALLOC_NEW`` directive mints a fresh reservation
  owned by the target namespace; a ``PMIX_ALLOC_EXTEND`` directive adds the
  nodes to the existing reservation named by the request's ``PMIX_ALLOC_ID``,
  provided the target namespace owns it.
* ``PMIX_ALLOC_TARGET`` absent: the new nodes are reserved to the *tool's own*
  namespace, so that processes the tool subsequently launches via
  ``PMIx_Spawn`` land on them by default.
* ``PMIX_ALLOC_TARGET`` absent **and** ``PMIX_ALLOC_SHARE`` present and set
  to ``true``: the new nodes go into the default session and are available to
  all namespaces.

Case 3 — Dynamic allocation requested by an application
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

An application process (one whose namespace owns a ``prte_job_t``) issues the
request:

* Default: the new nodes are reserved to the requesting process's namespace.
  Any process in that namespace may spawn jobs against them.
* ``PMIX_ALLOC_SHARE`` present and set to ``true``: the new nodes go into
  the default session for general use.

Note that ``PMIX_ALLOC_TARGET`` is honored only for tool requesters (Case 2);
an application cannot redirect a reservation to a *different* namespace.  An
application request carrying ``PMIX_ALLOC_TARGET`` is rejected with
``PMIX_ERR_NO_PERMISSIONS``.

Decision summary
~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 20 25 20 35

   * - Requester
     - ``PMIX_ALLOC_TARGET``
     - ``PMIX_ALLOC_SHARE``
     - Destination session
   * - Tool
     - ``<nspace>``
     - any/absent
     - session owned by ``<nspace>``
   * - Tool
     - absent
     - absent or ``false``
     - session owned by the tool's namespace
   * - Tool
     - absent
     - ``true``
     - default session
   * - Application
     - absent
     - absent or ``false``
     - session owned by the app's namespace
   * - Application
     - absent
     - ``true``
     - default session
   * - Application
     - present
     - any
     - rejected (``PMIX_ERR_NO_PERMISSIONS``)

Data Structure Changes
----------------------

Node-to-session backpointer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``prte_node_t`` (``src/runtime/prte_globals.h``) gains a back-reference to its
owning session:

.. code-block:: c

   /* session that owns this node; NULL means the node belongs to the
    * default session (the general, unreserved pool). Not reference-counted
    * to avoid an ownership cycle with prte_session_t->nodes. */
   prte_session_t *session;

The pointer is **not** retained: the session already holds the authoritative
references to its nodes through ``session->nodes``, and a node never outlives
its session.  The constructor in ``src/runtime/prte_globals.c`` initializes the
field to ``NULL``; the destructor clears it without releasing.

This backpointer is what lets the *default* session — whose ``nodes`` array is
the entire global pool — exclude nodes that have been reserved elsewhere.  A
node is considered part of the default session iff ``node->session == NULL``.

Session ownership and lookup
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Because a reservation has a *set* of owning namespaces (see Terminology),
``prte_session_t`` gains an owners list and a reserved flag:

.. code-block:: c

   /* namespaces entitled to spawn onto this session's nodes. Seeded with the
    * namespace the reservation was created for, then extended with each job
    * spawned into the session. Empty for the default session (which everyone
    * may use) and for scheduler-instantiated sessions not owned by a
    * namespace. Stored as an argv-style array of nspace strings. */
   char **owners;

An argv-style array of namespace strings is used rather than an array of
``pmix_nspace_t`` so the ``PMIx_Argv_*`` helpers can manage membership.  The
constructor initializes it to ``NULL``; the destructor frees it with
``PMIx_Argv_free``.

A new session flag is added in ``src/util/attr.h`` alongside
``PRTE_SESSION_FLAG_DYNAMIC``:

.. code-block:: c

   #define PRTE_SESSION_FLAG_RESERVED   0x0002 // nodes withheld from default pool

Two helpers are added in ``src/runtime/prte_globals.{h,c}``:

.. code-block:: c

   /* True if nspace is in session->owners, or session is the default session,
    * or nspace is the scheduler. */
   PRTE_EXPORT bool prte_session_is_owned_by(prte_session_t *session,
                                             const pmix_nspace_t nspace);

   /* Add nspace to session->owners if not already present (no-op for the
    * default session). Called when a reservation is created and each time a
    * job is spawned into the session. */
   PRTE_EXPORT void prte_session_add_owner(prte_session_t *session,
                                           const pmix_nspace_t nspace);

No new "lookup by owner" helper is introduced: because a namespace may own
several reservations, owner is not a unique key.  Existing reservations are
located by their allocation id with the existing
``prte_get_session_object_from_id`` (which matches ``session->alloc_refid``),
and ownership is then checked with ``prte_session_is_owned_by``.

New job attribute for spawn targeting
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A new ``PRTE_JOB_*`` key carries the resolved spawn target list on the
``prte_job_t`` so it survives the pack/unpack to the HNP.  The next free key
after ``PRTE_JOB_DO_NOT_SPAWN = PRTE_JOB_START_KEY + 123`` is used:

.. code-block:: c

   /* Comma-delimited list of PMIX_ALLOC_ID strings naming the allocations
    * (sessions) whose nodes this job may map onto. An empty token denotes the
    * default session. Populated from PMIX_SPAWN_TARGET. */
   #define PRTE_JOB_SPAWN_TARGET   (PRTE_JOB_START_KEY + 124) // char*

``PRTE_JOB_MAX_KEY`` already allows headroom (``+200``); no bump is required.

Two registration steps are easy to overlook and both cause *silent* failures
rather than build errors, so they are called out explicitly:

* **Add a case to** ``prte_attr_key_to_str`` (``src/util/attr.c``).  That
  switch has no ``default`` that names the key; an unregistered key prints as
  ``UNKNOWN``, which is harmless at runtime but masks the attribute in every
  map/state debug dump and makes the feature far harder to diagnose.
* **Set the attribute with the global flag.**  The generic job-attribute pack
  loop in ``prte_dt_packing_fns.c`` forwards only attributes whose ``local``
  field is ``PRTE_ATTR_GLOBAL`` (which is ``false``).  The spawn target must
  therefore be stored with::

     prte_set_attribute(&jdata->attributes, PRTE_JOB_SPAWN_TARGET,
                        PRTE_ATTR_GLOBAL, target_str, PMIX_STRING);

  If it is set ``PRTE_ATTR_LOCAL`` instead, the attribute lives only on the
  process that parsed ``PMIX_SPAWN_TARGET`` and never reaches the HNP, so the
  multi-session resolution in ``plm_base_receive.c`` silently falls back to the
  default/parent session.  Storing it global is what lets the plan claim the
  target "is packed/unpacked with the rest of the job attributes" — no bespoke
  pack/unpack code is needed because the generic loop handles any global
  ``char*`` attribute.

Allocation-Time Handling
------------------------

All reservation routing happens in ``prte_ras_base_complete_request``
(``src/mca/ras/base/ras_base_allocate.c``), which already runs on the progress
thread and already parses ``PMIX_ALLOC_NODE_LIST`` for the ``PMIX_ALLOC_EXTEND``
and ``PMIX_ALLOC_NEW`` directives.  The node-insertion call there is currently:

.. code-block:: c

   ret = prte_ras_base_node_insert(&ndlist, NULL);

The change is to first resolve the destination session, then place the new
nodes into both the global pool (so daemons launch and the nidmap is complete)
and, when reserving, into the destination session with the backpointer set.

Resolving the destination session
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The requester is ``req->tproc`` (recorded in ``pmix_server_alloc_fn``).  A
requester is an **application** iff ``prte_get_job_data_object(req->tproc.nspace)``
returns a non-tool job; otherwise it is a **tool**.  The directive scan in
``prte_ras_base_complete_request`` is extended to read the two new keys:

.. code-block:: c

   char *target = NULL;        /* PMIX_ALLOC_TARGET namespace, if any */
   bool share = false;         /* default: reserve (do not share) */
   bool have_share = false;

   for (n = 0; n < req->ninfo; n++) {
   #if defined(PMIX_ALLOC_TARGET)
       if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_TARGET)) {
           target = req->info[n].value.data.string;
           continue;
       }
   #endif
   #if defined(PMIX_ALLOC_SHARE)
       if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_SHARE)) {
           share = PMIX_INFO_TRUE(&req->info[n]);
           have_share = true;
           continue;
       }
   #endif
       /* ... existing PMIX_ALLOC_NODE_LIST handling ... */
   }

Destination resolution then follows the decision table, with the allocation
*directive* deciding whether a reservation is created or extended:

.. code-block:: c

   prte_session_t *dest;
   prte_job_t *reqjob = prte_get_job_data_object(req->tproc.nspace);
   bool is_tool = (NULL == reqjob) || PRTE_FLAG_TEST(reqjob, PRTE_JOB_FLAG_TOOL);

   /* the namespace the reservation is created for / must be owned by */
   const char *owner_nspace = (NULL != target) ? target : req->tproc.nspace;

   if (have_share && share) {
       dest = prte_default_session;            /* general use */
   } else if (NULL != target && !is_tool) {
       req->pstatus = PMIX_ERR_NO_PERMISSIONS; /* app may not retarget */
       return;
   } else if (PMIX_ALLOC_EXTEND == req->allocdir) {
       /* extend an existing reservation named by its alloc id */
       dest = prte_get_session_object_from_id(/* PMIX_ALLOC_ID from req->info */);
       if (NULL == dest ||
           !prte_session_is_owned_by(dest, req->tproc.nspace)) {
           req->pstatus = (NULL == dest) ? PMIX_ERR_NOT_FOUND
                                         : PMIX_ERR_NO_PERMISSIONS;
           return;
       }
   } else {                                    /* PMIX_ALLOC_NEW */
       dest = create_reservation(owner_nspace);
   }

``create_reservation(nspace)`` allocates a new ``prte_session_t``, assigns a
fresh ``session_id`` and ``alloc_refid``, sets ``flags |=
PRTE_SESSION_FLAG_RESERVED | PRTE_SESSION_FLAG_DYNAMIC``, seeds the owners list
with ``prte_session_add_owner(dest, nspace)``, and registers it with
``prte_set_session_object``.  A ``PMIX_ALLOC_NEW`` request always mints a new
reservation; the same namespace may thus own several, each distinguished by its
allocation id.

When the installed PMIx lacks ``PMIX_ALLOC_TARGET``, the ``target`` guard keeps
it ``NULL`` so ``owner_nspace`` is always the requester's namespace — the
behavior required when the key is unavailable.  Likewise, without
``PMIX_ALLOC_SHARE`` the ``share`` default of ``false`` means a bare
``PMIX_ALLOC_NEW`` reserves to the requester's namespace.

Placing the nodes
~~~~~~~~~~~~~~~~~~

The new nodes are always inserted into the global pool so the DVM can launch
daemons on them and the nidmap stays complete (this is unchanged and is what
drives ``PRTE_JOB_EXTEND_DVM`` / ``PRTE_JOB_STATE_LAUNCH_DAEMONS``).  When
``dest`` is not the default session, the same node objects are additionally
registered with the reservation:

.. code-block:: c

   ret = prte_ras_base_node_insert(&ndlist, NULL);   /* into global pool */
   /* ... existing error handling ... */

   if (dest != prte_default_session) {
       for (each newly inserted node nd) {
           nd->session = dest;                 /* withhold from default pool */
           PMIX_RETAIN(nd);
           pmix_pointer_array_add(dest->nodes, nd);
       }
   }

The reservation holds **references** to the global node objects, not copies.
This keeps live state — ``node->daemon``, ``slots_inuse``, ``topology`` — in a
single place, which the mapper and the launch path both rely on.  (The SLURM
RAS dynamic path in ``ras_slurm_module.c`` currently stores ``prte_node_copy``
results in ``session->nodes``; reconciling that path to references is tracked
as a follow-up below.)

Because reserved nodes carry ``node->session != NULL``, they are automatically
excluded from the default session even though they remain in the global pool
that ``prte_default_session->nodes`` aliases.  The mapper changes below
implement that exclusion.

Reporting the allocation id back to the requester
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

So a tool or application can later target the reservation, the response info
returned through ``req->infocbfunc`` includes the destination's
``alloc_refid`` as ``PMIX_ALLOC_ID`` (and, when newly minted, ``PMIX_ALLOC_REQ_ID``
echoing any user-supplied ``PMIX_ALLOC_REQ_ID``).  This reuses the existing
response array assembly in ``pmix_server_alloc_request_resp``.

Spawn-Time Handling
-------------------

Selecting the target session(s)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A spawn request may carry ``PMIX_SPAWN_TARGET`` whose value is either:

* a single ``PMIX_ALLOC_ID`` string (``PMIX_STRING``), or
* an array of ``PMIX_ALLOC_ID`` strings (``pmix_data_array_t*`` of
  ``PMIX_STRING``).

Each entry names an allocation (session via ``alloc_refid``).  An entry whose
value is the invalid namespace / empty allocation id denotes the default
session.  Resolution rules:

* ``PMIX_SPAWN_TARGET`` absent, or present but naming only the invalid
  namespace: the job uses the **default** session — unchanged behavior.
* One or more valid allocation ids given: the job may map onto the **union** of
  those sessions' nodes.  Including an invalid-namespace entry alongside valid
  ids adds the default pool to the union (the documented way to request
  "reserved plus default").
* A named allocation id that resolves to no session is a fatal error for the
  spawn (``PMIX_ERR_NOT_FOUND``), consistent with the existing handling of an
  unknown ``PRTE_JOB_SESSION_ID``.
* A named allocation id that resolves to a session the requester does not own
  is rejected with ``PMIX_ERR_NO_PERMISSIONS``.  A requester may target only
  the default session (via the invalid namespace) and reservations owned by
  its own namespace; see *Ownership check* below.

Ownership check
~~~~~~~~~~~~~~~~

A spawn may only target allocations the requester is entitled to use.  The
requester is the spawning process recorded as the job's launch proxy
(``PRTE_JOB_LAUNCH_PROXY`` / ``nptr`` in ``plm_base_receive.c``).  A targeted
session ``S`` passes the ownership check iff any of:

* ``S`` is the default session (named by the invalid namespace), which is
  always permitted; or
* the requester's namespace is in ``S->owners`` — i.e. it created the
  reservation or was previously spawned into it; or
* the requester *is* the scheduler, which may target any session.

All three are folded into ``prte_session_is_owned_by(S, requester_nspace)``.

Each entry in the resolved target set is validated against this check on the
HNP, where the authoritative session objects and the requester identity are
both known.  A failing entry rejects the entire spawn with
``PMIX_ERR_NO_PERMISSIONS`` rather than silently dropping the target, so the
caller learns its request was not honored.  This mirrors the allocation-time
rule that an application may not retarget a reservation to a foreign
namespace.

This is handled where the spawn request is unpacked and ``PMIX_SPAWN_TARGET``
is available, in the PMIx server spawn path
(``src/prted/pmix/pmix_server_dyn.c``), under:

.. code-block:: c

   #if defined(PMIX_SPAWN_TARGET)
       /* flatten the single string or string array into a comma-delimited
        * list and stash it as PRTE_JOB_SPAWN_TARGET on the jdata */
   #endif

When the key is unavailable, ``PRTE_JOB_SPAWN_TARGET`` is never set and every
path below falls through to the existing default/parent-session logic.

Carrying the target to the HNP
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The resolved target list rides on the job as ``PRTE_JOB_SPAWN_TARGET`` and is
packed/unpacked with the rest of the job attributes.  On the HNP,
``plm_base_receive.c`` already resolves a single session from
``PRTE_JOB_SESSION_ID`` / ``PRTE_JOB_ALLOC_ID`` / ``PRTE_JOB_REF_ID`` before
falling back to the parent session.  That block is extended so that, when
``PRTE_JOB_SPAWN_TARGET`` is present, it resolves the (possibly multiple)
sessions named there.  The existing single-session attributes continue to work
and are treated as a one-element target set.

Each resolved session is validated against the ownership check described above
before it is admitted to the target set.  Once admitted, the spawned job's
namespace is added as an owner of that reservation with
``prte_session_add_owner(S, jdata->nspace)`` — this is what makes the child a
co-owner of the reservation it was spawned into, so it may in turn spawn
further jobs onto those nodes.  The default session is never modified by this
step (``prte_session_add_owner`` is a no-op for it), and the child gains
ownership of *only* the reservations it was spawned into, never the other
reservations its parent may own — ownership does not propagate transitively.

``jdata->session`` retains its present meaning as the job's *primary* session
(used for accounting, refids, and the ``session->jobs`` linkage).  It is set to
the first valid targeted session, or to the default session when only the
invalid namespace is targeted.  The full target set drives mapping.

Mapping Changes
---------------

``prte_rmaps_base_get_target_nodes``
(``src/mca/rmaps/base/rmaps_base_support_fns.c``) is generalized from "iterate
``jdata->session->nodes``" to "iterate the candidate pool defined by the job's
target session set."  Define the membership predicate:

.. code-block:: c

   /* The session a node belongs to for targeting purposes. */
   static inline prte_session_t *node_owning_session(prte_node_t *nd)
   {
       return (NULL == nd->session) ? prte_default_session : nd->session;
   }

   /* True if nd is usable by a job targeting the given set of sessions. */
   static bool node_in_targets(prte_node_t *nd,
                               prte_session_t **targets, size_t ntargets);

Both node-collection branches in ``get_target_nodes`` change:

* The dash-host / hostfile branch (which matches user-named nodes against the
  session) matches against the global pool but **accepts a match only if the
  node passes** ``node_in_targets``.
* The ``addknown`` branch, which currently walks ``jdata->session->nodes``,
  walks the global ``prte_node_pool`` and includes a node iff it passes
  ``node_in_targets``.

For a job with no spawn target (the overwhelmingly common case) the target set
is ``{ prte_default_session }``, so ``node_in_targets`` accepts exactly the
nodes with ``node->session == NULL`` — i.e. the unreserved pool — and rejects
reserved nodes.  This is the only behavioral change for existing jobs: they no
longer see nodes that some other namespace has reserved, which is the intended
isolation.

For a reserved-session job the target set is ``{ that session }`` (optionally
unioned with the default session when the invalid namespace is also targeted),
and ``node_in_targets`` accepts exactly the reserved nodes (plus the default
pool when requested).

Backward Compatibility
-----------------------

* ``PMIX_ALLOC_TARGET``, ``PMIX_ALLOC_SHARE``, and ``PMIX_SPAWN_TARGET`` are
  referenced only inside ``#if defined(<key>)`` blocks.  These keys are plain
  string ``#define``\ s supplied by PMIx; their mere presence in
  ``pmix_common.h`` is the availability signal, so ``#if defined`` (not the
  logical-macro ``#if FOO`` idiom) is the correct guard here.  Where a future
  PMIx exposes a capability flag for any of these, the guard can be migrated to
  the ``PRTE_CHECK_PMIX_CAP`` mechanism in ``config/prte_setup_pmix.m4`` and a
  configure-defined ``PRTE_HAVE_*`` macro.
* With an older PMIx that lacks all three keys, ``target`` is always ``NULL``,
  ``share`` is never observed, and ``PRTE_JOB_SPAWN_TARGET`` is never set.
  Dynamic allocations then fall to the "reserve to the requester's namespace"
  default — which, combined with the mapping change, still partitions
  resources sensibly — while spawns continue to use the default/parent session
  exactly as today.  No new warnings are introduced under
  ``--enable-devel-check``.

Session Lifecycle
-----------------

A reservation is **not** tied to the lifetime of any single job.  Because every
job is its own namespace, the jobs that run in a reservation come and go while
the reservation persists; a job terminating (even the one that created the
reservation) does not release it.  A reservation lives until one of exactly
three events occurs:

1. **An owner issues** ``PMIX_ALLOC_RELEASE`` for the reservation's allocation
   id.  Any namespace in ``session->owners`` may release it; the scheduler may
   release any reservation.
2. **The DVM tears down.**  All sessions, default and reserved alike, are
   destroyed with the DVM.
3. **The scheduler terminates the allocation** — for example on a timeout
   (``session->timeout``) or an administrative reclaim.  This arrives as a
   scheduler-driven release for the allocation id.

In all three cases the release runs through the existing
``prte_ras_base_release_allocation`` path.  Releasing a reservation must, in
order: clear ``node->session`` on each member node (so any node that survives
in the DVM falls back to the default pool), drop the reservation's retained
references in ``session->nodes``, and only then proceed with any DVM shrink for
nodes that are being returned to the scheduler.  The clear-before-shrink
ordering keeps the reservation bookkeeping from racing the shrink-campaign
accounting (``prte_dvm_launch_fence`` / ``prte_shrink_campaigns``).

Releasing while jobs are still mapped
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Because a reservation outlives the jobs that run in it, a release request may
arrive while one or more jobs are still mapped onto the reservation's nodes —
an owner releasing prematurely, or a scheduler timeout/reclaim firing under a
running job.  The two sub-cases differ:

* **Owner-initiated release** of a reservation whose nodes are *not* being
  returned to the scheduler (the nodes stay in the DVM): no process is killed.
  The nodes simply revert to the default pool when ``node->session`` is
  cleared, and the already-running jobs continue on them; their
  ``jdata->session`` still points at the now-detached session object, which is
  kept alive by the normal ``session->jobs`` references until those jobs
  terminate.  Mapping for *new* jobs no longer sees the reservation.
* **Release that returns nodes to the scheduler** (scheduler reclaim, timeout,
  or an owner release of scheduler-owned nodes): this is the existing DVM
  shrink path, which already terminates the affected jobs and daemons before
  the nodes leave.  The reservation cleanup adds nothing new here beyond the
  clear-before-shrink ordering above; it reuses the established shrink-campaign
  teardown.

Either way the reservation cleanup never *itself* kills a job — termination is
driven only by the pre-existing shrink machinery when nodes physically depart.

Implementation Order
--------------------

1. Add ``prte_node_t::session`` backpointer plus constructor/destructor
   handling; no behavior change yet.
2. Add ``prte_session_t::owners``, ``PRTE_SESSION_FLAG_RESERVED``, and the
   ``prte_session_is_owned_by`` / ``prte_session_add_owner`` helpers.
3. Generalize ``get_target_nodes`` to the target-session-set predicate, with
   the default target set ``{ prte_default_session }`` so existing behavior is
   preserved (reserved-node exclusion becomes active but no node is reserved
   yet).
4. Implement allocation-time routing in ``prte_ras_base_complete_request``
   (Cases 2 and 3), guarded by ``#if defined`` on the two alloc keys.
5. Add ``PRTE_JOB_SPAWN_TARGET`` and the ``PMIX_SPAWN_TARGET`` parsing in the
   spawn path, plus the multi-session resolution in ``plm_base_receive.c``.
6. Return the allocation id to the requester from the alloc response.
7. Wire reservation release/cleanup into the ``PMIX_ALLOC_RELEASE`` path
   (owner-initiated, scheduler-initiated, and DVM teardown), clearing
   ``node->session`` before any shrink — see *Session Lifecycle*.

Each step builds and is testable on its own; steps 1–3 are behavior-preserving
groundwork, and the observable feature lands with steps 4–6.

Open Questions
--------------

1. **SLURM RAS reconciliation.**  ``ras_slurm_module.c`` stores
   ``prte_node_copy`` results in ``session->nodes`` rather than references to
   the global node objects.  The reference-based model here is incompatible
   with copies (the mapper would see daemon-less duplicates).  Recommendation:
   migrate the SLURM dynamic-session path to references for a single,
   consistent model.  A source-code note has been added at the copy site to
   flag the required change.

Resolved decisions
~~~~~~~~~~~~~~~~~~~

* **Default (``PMIX_ALLOC_SHARE`` absent) means "reserve."**  An
  ``ALLOC_SHARE``-absent dynamic request reserves the nodes to the
  requester's (or target's) namespace; only an explicit
  ``PMIX_ALLOC_SHARE == true`` routes nodes to the default pool.
* **``PMIX_SPAWN_TARGET`` is limited by an ownership check.**  A requester may
  target only the default session and reservations its namespace owns (the
  scheduler excepted).  See *Ownership check* above.
* **Ownership is a set, conferred by spawning-into, and not inherited.**  Every
  job is its own namespace.  A reservation's owners are the namespace it was
  created for plus every job spawned into it; a child job owns only the
  reservation it was spawned into, not the parent's other reservations.
* **Reservation teardown has three triggers.**  An owner's
  ``PMIX_ALLOC_RELEASE``, DVM teardown, or scheduler termination of the
  allocation (e.g. timeout).  A job terminating never releases a reservation.
  See *Session Lifecycle*.
