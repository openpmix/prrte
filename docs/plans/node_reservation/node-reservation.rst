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
3. Honor ``PMIX_ALLOC_INHERITANCE`` to decide what becomes of a reservation
   when its owning namespace terminates.
4. Relay the scheduler's allocation-timeout warning
   (``PMIX_ALLOC_WARN_TIMEOUT`` request, ``PMIX_ALLOC_TIMEOUT_WARNING`` event)
   to the requester.
5. Honor a new ``PMIX_SPAWN_TARGET`` attribute on spawn requests to select the
   union of allocations a job may map onto.

The observable contract for all of the above is specified in
``node-reservation-spec.rst``; this document describes the implementation that
realizes it.  All of these PMIx keys are optional and **must** be compiled out
cleanly when the installed PMIx headers predate them, preserving backward
compatibility.

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
4. Code that references ``PMIX_ALLOC_TARGET``, ``PMIX_ALLOC_SHARE``,
   ``PMIX_ALLOC_INHERITANCE``, ``PMIX_ALLOC_WARN_TIMEOUT``, or
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

Owning namespace and inheritance disposition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``owners`` list is the *authorization* set (who may map onto and target
the reservation).  The inheritance rules in ``node-reservation-spec.rst`` also
require a distinguished **owning namespace** — the single namespace the
reservation was created for, whose termination drives the ``NONE`` and
``DEFAULT`` dispositions — and the recorded disposition itself.
``prte_session_t`` gains:

.. code-block:: c

   /* The single namespace the reservation was created for (PMIX_ALLOC_TARGET,
    * else the requester). Empty for the default session. owners[] additionally
    * accumulates every namespace spawned into the reservation. */
   pmix_nspace_t owner;

   /* Retained reference to the owning namespace's job object, so its children
    * subtree stays walkable for CHILD-flavored drain even after the owning
    * namespace terminates. NULL for the default session. Released at teardown. */
   prte_job_t *owner_job;

   /* Disposition recorded at creation, governing teardown when the owning
    * namespace (NONE/DEFAULT) or the last derived child (CHILD/CHILD_DEFAULT)
    * terminates. Stored as the uint8_t underlying pmix_alloc_inheritance_t so
    * the struct compiles against a PMIx that lacks the type; defaults to the
    * value of PMIX_ALLOC_INHERIT_DEFAULT. */
   uint8_t inheritance;

The ``inheritance`` field is typed ``uint8_t`` rather than
``pmix_alloc_inheritance_t`` so the struct compiles even when the installed
PMIx predates the type; the four disposition constants are only referenced
inside ``#if defined`` guards.  The constructor sets ``owner`` empty,
``owner_job`` to ``NULL``, and ``inheritance`` to the default-disposition value
(``PMIX_ALLOC_INHERIT_DEFAULT`` when defined, else the equivalent "unreserve on
owning-namespace termination" behavior, which is what an inheritance-unaware
build performs unconditionally).  The destructor releases ``owner_job`` if still
set, so DVM teardown (which runs the session destructor directly) also balances
the reference.

For the ``CHILD``-flavored dispositions, teardown is deferred until the owning
namespace and **all of its derived children** — the full transitive spawn
subtree rooted at the owning namespace, per the spec — have terminated.  The
owner set is *not* sufficient for this: a descendant may run in some other
session (not spawned into this reservation) yet still counts as a derived child
that holds the reservation open.  The drain condition must therefore be
computed from the job genealogy, not from ``owners``.

PRRTE already records that genealogy on ``prte_job_t``:

* ``prte_job_t::children`` — a list of the job's **immediate** child jobs,
  populated in ``plm_base_receive.c`` when a spawn's parent is resolved.
* ``prte_job_t::launcher`` — the nspace of the **root** launcher of the tree.
  All descendants of an original spawn inherit the same ``launcher`` value (the
  spawn code copies the parent's ``launcher``, or sets it to the parent when the
  parent is itself an original spawn).

The transitive subtree under an owning namespace *O* is obtained by recursively
walking ``children`` from *O*'s job object.  PRRTE already does exactly this
kind of genealogy traversal on the termination path: ``prte_dump_aborted_procs``
(``src/runtime/prte_quit.c``) resolves a job's ``launcher`` and walks the
launcher's ``children`` to report a failed descendant.  Because that tree-walk
cost is already paid when jobs terminate, the drain condition reuses the
**existing parentage tracking** rather than introducing parallel bookkeeping: at
each ``PRTE_JOB_STATE_TERMINATED``, traverse the owning namespace's ``children``
subtree and ask whether any job in it is still running.  A per-reservation
descendant *counter* was considered and rejected — it would duplicate state the
genealogy already holds, and add spawn-time bookkeeping for no traversal saving
the termination path does not already incur.

The walk must root at *O*'s own job object (``launcher`` names only the *root*
of a tree, so it cannot root the subtree of an owning namespace that sits
mid-genealogy).  Since a ``CHILD``-flavored reservation outlives *O*, that job
object must stay queryable after *O* terminates: ``create_reservation`` takes a
reference on it (``PMIX_RETAIN``) and stores it as ``session->owner_job``,
released during teardown.  Each job retains its own ``children``, so retaining
the subtree root keeps the whole subtree reachable.  This mirrors how
``session->jobs`` already retains job objects for the session's lifetime and
introduces no new tracking machinery.

When the owning namespace is a **tool** (no ``prte_job_t``), ``owner_job`` is
``NULL`` and there is no single root job.  The tool's derived children are the
jobs it spawned directly into the reservation — present in ``session->jobs`` —
so the drain walk roots at those and descends through their ``children``.  This
covers descendants the tool's children later spawn into other sessions, since
those are reached through the child jobs' own ``children`` links.

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
``prte_ras_base_complete_request`` is extended to read the new keys:

.. code-block:: c

   char *target = NULL;        /* PMIX_ALLOC_TARGET namespace, if any */
   bool share = false;         /* default: reserve (do not share) */
   bool have_share = false;
   uint8_t inherit = PRTE_INHERIT_DEFAULT_VALUE;  /* see below */
   bool have_inherit = false;

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
   #if defined(PMIX_ALLOC_INHERITANCE)
       if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_INHERITANCE)) {
           inherit = req->info[n].value.data.inheritance;
           have_inherit = true;
           continue;
       }
   #endif
       /* PMIX_ALLOC_WARN_TIMEOUT is not consumed here: it is left in
        * req->info to be forwarded verbatim to the scheduler; see
        * "Allocation timeout warning" below. */
       /* ... existing PMIX_ALLOC_NODE_LIST handling ... */
   }

``PRTE_INHERIT_DEFAULT_VALUE`` is a small internal macro defined as
``PMIX_ALLOC_INHERIT_DEFAULT`` when that constant is available and as the
equivalent literal otherwise, so the default disposition is the same whether
or not the running PMIx defines the type.  A non-default ``inherit`` value that
this build cannot honor (for example, because ``PMIX_ALLOC_INHERITANCE`` is
undefined and the value arrived over the wire from a newer peer) is rejected
with ``PMIX_ERR_NOT_SUPPORTED`` rather than silently dropped, matching the
spec's error contract.

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

``create_reservation(nspace, inherit)`` allocates a new ``prte_session_t``,
assigns a fresh ``session_id`` and ``alloc_refid``, sets ``flags |=
PRTE_SESSION_FLAG_RESERVED | PRTE_SESSION_FLAG_DYNAMIC``, records the owning
namespace (``PMIX_LOAD_NSPACE(dest->owner, nspace)``) and the disposition
(``dest->inheritance = inherit``), takes and stores a retained reference to the
owning namespace's job object when one exists (``dest->owner_job`` via
``PMIX_RETAIN``; it is ``NULL`` for a tool requester that has no job), seeds the
owners list with ``prte_session_add_owner(dest, nspace)``, and registers it with
``prte_set_session_object``.  A ``PMIX_ALLOC_NEW`` request always mints a new
reservation; the same namespace may thus own several, each distinguished by its
allocation id.  On a ``PMIX_ALLOC_EXTEND`` that carried an inheritance value,
``dest->inheritance`` is updated to the new value (the spec permits inheritance
on ``EXTEND``); when the request omits it, the existing disposition is left
untouched.

When the installed PMIx lacks ``PMIX_ALLOC_TARGET``, the ``target`` guard keeps
it ``NULL`` so ``owner_nspace`` is always the requester's namespace — the
behavior required when the key is unavailable.  Likewise, without
``PMIX_ALLOC_SHARE`` the ``share`` default of ``false`` means a bare
``PMIX_ALLOC_NEW`` reserves to the requester's namespace.  Without
``PMIX_ALLOC_INHERITANCE`` the ``inherit`` default keeps every reservation at
``PMIX_ALLOC_INHERIT_DEFAULT`` semantics — unreserve into the session when the
owning namespace terminates — which is exactly the disposition the spec
prescribes for an absent attribute.

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

Allocation timeout warning
--------------------------

The spec requires PRRTE to act as the relay for the scheduler's
allocation-timeout warning; PRRTE neither sets the timeout nor generates the
warning.  Two directions are involved:

* **Request (outbound).**  ``PMIX_ALLOC_WARN_TIMEOUT`` (``uint32_t`` seconds)
  arriving on a ``PMIX_ALLOC_NEW`` or ``PMIX_ALLOC_EXTEND`` is not consumed by
  the routing logic above; it is left in the info array that the RAS forwards
  to the host/scheduler so the scheduler can honor it.  In a DVM with no
  backing scheduler, there is no timeout source and the attribute is simply
  never acted upon — the advisory contract permits this.

* **Event (inbound).**  When the scheduler fires ``PMIX_ALLOC_TIMEOUT_WARNING``,
  it is delivered to the HNP through the existing PMIx event channel.  PRRTE
  relays it **only to the process that requested the allocation** — a directed
  notification, not a DVM-wide broadcast — by setting the event range to that
  process (the requester recorded as the reservation's creator, ``tproc`` on
  the originating request).  The relayed payload carries the same
  ``PMIX_ALLOC_ID``, the user's ``PMIX_ALLOC_REQ_ID`` when one was supplied, and
  ``PMIX_TIME_REMAINING`` (``uint32_t`` seconds) unchanged from the scheduler.

The whole path is guarded by ``#if defined(PMIX_ALLOC_TIMEOUT_WARNING)`` (and
the companion key/event guards); a PMIx that predates the event registers no
handler and relays nothing, while allocation expiry still flows through the
ordinary scheduler-reclaim/shrink path (see *Session Lifecycle*).  The warning
changes no reservation state by itself: only a subsequent ``PMIX_ALLOC_EXTEND``
or the actual expiry alters the allocation.

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

* ``PMIX_ALLOC_TARGET``, ``PMIX_ALLOC_SHARE``, ``PMIX_ALLOC_INHERITANCE``,
  ``PMIX_ALLOC_WARN_TIMEOUT``, ``PMIX_ALLOC_TIMEOUT_WARNING``, and
  ``PMIX_SPAWN_TARGET`` are referenced only inside ``#if defined(<key>)``
  blocks.  These keys are plain string ``#define``\ s supplied by PMIx (and the
  inheritance type ``pmix_alloc_inheritance_t`` likewise); their mere presence
  in ``pmix_common.h`` is the availability signal, so ``#if defined`` (not the
  logical-macro ``#if FOO`` idiom) is the correct guard here.  The
  ``session->inheritance`` field is typed ``uint8_t`` precisely so the struct
  needs no guard.  Where a future PMIx exposes a capability flag for any of
  these, the guard can be migrated to the ``PRTE_CHECK_PMIX_CAP`` mechanism in
  ``config/prte_setup_pmix.m4`` and a configure-defined ``PRTE_HAVE_*`` macro.
* With an older PMIx that lacks these keys, ``target`` is always ``NULL``,
  ``share`` is never observed, ``inherit`` stays at the default-disposition
  value, no timeout warning is relayed, and ``PRTE_JOB_SPAWN_TARGET`` is never
  set.  Dynamic allocations then fall to the "reserve to the requester's
  namespace" default, every reservation behaves as ``PMIX_ALLOC_INHERIT_DEFAULT``
  (unreserve into the session when the owning namespace exits), and — combined
  with the mapping change — resources are still partitioned sensibly, while
  spawns continue to use the default/parent session exactly as today.  No new
  warnings are introduced under ``--enable-devel-check``.

Session Lifecycle
-----------------

A reservation's lifetime is governed by two mechanisms, matching the spec:
**unconditional triggers** that end it regardless of any other state, and the
**namespace-termination disposition** recorded in ``session->inheritance`` at
creation.

Unconditional triggers
~~~~~~~~~~~~~~~~~~~~~~~~

Independently of the disposition, a reservation ends when:

1. **An owner issues** ``PMIX_ALLOC_RELEASE`` for the reservation's allocation
   id.  Any namespace in ``session->owners`` may release it; the scheduler may
   release any reservation.
2. **The DVM tears down.**  All sessions, default and reserved alike, are
   destroyed with the DVM.
3. **The scheduler terminates the allocation** — for example on a timeout or an
   administrative reclaim.  This arrives as a scheduler-driven release for the
   allocation id (and may be preceded by the timeout warning relayed above).

Namespace-termination disposition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The other end of a reservation's life is driven by namespace termination per
``session->inheritance``.  PRRTE already observes job (namespace) termination
in the state machine / errmgr at ``PRTE_JOB_STATE_TERMINATED``.  That site is
extended: when namespace *N* terminates, walk the registered reservations and,
for each one whose disposition fires, run the corresponding teardown.

* ``PMIX_ALLOC_INHERIT_NONE`` — when ``N`` equals ``session->owner``: **release**
  (return nodes to the scheduler), even if entries of ``session->owners`` are
  still running.
* ``PMIX_ALLOC_INHERIT_DEFAULT`` (also the default when no value was recorded) —
  when ``N`` equals ``session->owner``: **unreserve** (clear ``node->session``
  and drop the reservation's references, leaving the nodes in the DVM default
  pool; do not shrink, do not return them to the scheduler).
* ``PMIX_ALLOC_INHERIT_CHILD`` — when ``N`` is the owning namespace or any
  derived child of it, and after ``N`` exits no job in the owning namespace's
  transitive spawn subtree remains running: **release**.
* ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT`` — same drain condition as ``CHILD`` but
  the teardown is **unreserve** rather than release.

The ``CHILD`` drain condition is evaluated against the **job genealogy**, not
the owner set: a derived child is any transitive spawn descendant of the owning
namespace, even one running in another session.  As described under *Owning
namespace and inheritance disposition*, this is computed by traversing the
owning namespace's ``prte_job_t::children`` subtree — the same parentage walk
``prte_dump_aborted_procs`` already performs on the termination path — so no
counter or parallel bookkeeping is added.  The subtree root stays reachable
after the owning namespace exits because the reservation holds a retained
reference to it in ``session->owner_job``.  The owner set continues to serve
only *authorization* (who may map onto and target the reservation); it is
deliberately **not** reused for the lifetime computation.

Release versus unreserve
~~~~~~~~~~~~~~~~~~~~~~~~~~

Both teardowns share a prefix and differ only in the tail:

* **Release** runs the existing ``prte_ras_base_release_allocation`` path.  In
  order: clear ``node->session`` on each member node (so any node that survives
  in the DVM falls back to the default pool), drop the reservation's retained
  references in ``session->nodes``, and only then proceed with any DVM shrink
  for nodes being returned to the scheduler.  The clear-before-shrink ordering
  keeps the reservation bookkeeping from racing the shrink-campaign accounting
  (``prte_dvm_launch_fence`` / ``prte_shrink_campaigns``).
* **Unreserve** performs the same clear-and-drop prefix but **stops there**: it
  never shrinks and never returns nodes to the scheduler.  The nodes remain in
  the DVM as part of the default pool, available to any job, until the DVM (the
  PMIx "session") itself ends.  The reservation object is deregistered with
  ``prte_set_session_object`` once its nodes and owners are cleared.

Both tails also drop the reservation's retained owning-job reference
(``PMIX_RELEASE(session->owner_job)``) as the session object is torn down,
balancing the ``PMIX_RETAIN`` taken in ``create_reservation``.

Teardown while jobs are still mapped
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A trigger or disposition may fire while jobs are still mapped onto the
reservation's nodes — an owner releasing prematurely, a scheduler
timeout/reclaim under a running job, or ``PMIX_ALLOC_INHERIT_NONE`` firing while
spawned children still run.  The outcome depends only on whether the nodes
leave the DVM:

* **Nodes stay in the DVM** (an owner release or a ``*_DEFAULT`` unreserve whose
  nodes are not returned to the scheduler): no process is killed.  The nodes
  revert to the default pool when ``node->session`` is cleared, and
  already-running jobs continue on them; their ``jdata->session`` still points
  at the now-detached session object, kept alive by the normal ``session->jobs``
  references until those jobs terminate.  Mapping for *new* jobs no longer sees
  the reservation.
* **Nodes leave the DVM** (scheduler reclaim or timeout, or a ``NONE``/``CHILD``
  release of scheduler-owned nodes): this is the existing DVM shrink path, which
  already terminates the affected jobs and daemons before the nodes leave.  The
  reservation cleanup adds nothing beyond the clear-before-shrink ordering
  above; it reuses the established shrink-campaign teardown.

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
4. Add ``prte_session_t::owner`` and ``::inheritance``, with the constructor
   defaulting ``inheritance`` to the ``PMIX_ALLOC_INHERIT_DEFAULT`` value; still
   no behavior change.
5. Implement allocation-time routing in ``prte_ras_base_complete_request``
   (Cases 2 and 3), guarded by ``#if defined`` on the alloc keys, recording
   ``owner``, ``owner_job`` (retained), and ``inheritance`` on each new
   reservation.
6. Add ``PRTE_JOB_SPAWN_TARGET`` and the ``PMIX_SPAWN_TARGET`` parsing in the
   spawn path, plus the multi-session resolution in ``plm_base_receive.c``.
7. Return the allocation id to the requester from the alloc response.
8. Wire reservation release/cleanup into the ``PMIX_ALLOC_RELEASE`` path
   (owner-initiated, scheduler-initiated, and DVM teardown), clearing
   ``node->session`` before any shrink — see *Session Lifecycle*.
9. Implement the strict ``CHILD`` drain as a recursive walk of the owning
   namespace's ``prte_job_t::children`` subtree, rooted at ``session->owner_job``
   (or, for a tool owner, at the reservation's ``session->jobs``), reusing the
   parentage tracking ``prte_dump_aborted_procs`` already traverses.  The
   retained ``owner_job`` from step 5 keeps that subtree reachable after the
   owning namespace exits.  No counter is introduced.
10. Drive the namespace-termination disposition from
    ``PRTE_JOB_STATE_TERMINATED``: on each namespace exit, apply
    ``NONE``/``DEFAULT`` (owner-keyed) and ``CHILD``/``CHILD_DEFAULT``
    (transitive-descendant drain, via a recursive ``prte_job_t::children`` walk
    rooted at the owning namespace) teardowns, distinguishing release from
    unreserve.  ``DEFAULT`` is in force for every reservation by default, so
    this step also makes the absent-attribute behavior correct.
11. Relay the allocation-timeout warning: forward ``PMIX_ALLOC_WARN_TIMEOUT`` to
    the scheduler and relay ``PMIX_ALLOC_TIMEOUT_WARNING`` back to the
    requesting process, guarded by ``#if defined``.

Each step builds and is testable on its own; steps 1–4 are behavior-preserving
groundwork, and the observable feature lands with steps 5–11.

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
* **Reservation teardown has unconditional triggers plus an inheritance
  disposition.**  The unconditional triggers are an owner's
  ``PMIX_ALLOC_RELEASE``, DVM teardown, and scheduler termination of the
  allocation (e.g. timeout).  In addition, namespace termination drives teardown
  per ``session->inheritance``: ``NONE``/``DEFAULT`` fire when the owning
  namespace exits, ``CHILD``/``CHILD_DEFAULT`` when the last derived child
  exits; the ``*_DEFAULT`` variants unreserve into the session rather than
  returning nodes to the scheduler.  See *Session Lifecycle*.
* **``CHILD`` tracking is strict transitive-descendant tracking, via the
  existing parentage system.**  A derived child is *any* transitive spawn
  descendant of the owning namespace, per the spec and the PMIx inheritance
  overview — including descendants that run in another session.  The drain
  condition is computed from the job genealogy (a recursive walk of
  ``prte_job_t::children``), not from the reservation's owner set.  This reuses
  the parentage tracking that ``prte_dump_aborted_procs`` already traverses on
  the termination path, so the tree-walk cost is already borne; a per-reservation
  descendant counter was considered and rejected.
* **The owning namespace's job object is retained on the reservation.**  Because
  a ``CHILD``/``CHILD_DEFAULT`` reservation outlives its owning namespace *O*
  while descendants run, ``create_reservation`` takes a reference on *O*'s
  ``prte_job_t`` (``PMIX_RETAIN``) and stores it on the session; the teardown
  (release or unreserve) drops it.  This keeps *O*'s ``children`` subtree — each
  job retains its own children — walkable after *O* terminates, so the drain
  test always has a root.  It mirrors how ``session->jobs`` already retains and
  releases job objects for the session's lifetime, and adds no new tracking
  machinery.  ``prte_session_t`` therefore also carries a
  ``prte_job_t *owner_job`` alongside the ``owner`` nspace.
* **The default disposition is ``PMIX_ALLOC_INHERIT_DEFAULT``.**  When
  ``PMIX_ALLOC_INHERITANCE`` is absent, a reservation becomes unreserved within
  the session when its owning namespace terminates; the legacy "release to the
  scheduler on termination" behavior requires an explicit
  ``PMIX_ALLOC_INHERIT_NONE``.  This matches the PMIx inheritance overview, and
  supersedes the earlier "a job terminating never releases a reservation"
  decision.
* **The allocation-timeout warning is relayed, not invented.**  PRRTE forwards
  ``PMIX_ALLOC_WARN_TIMEOUT`` to the scheduler and relays the scheduler's
  ``PMIX_ALLOC_TIMEOUT_WARNING`` event only to the requesting process.  See
  *Allocation timeout warning*.
