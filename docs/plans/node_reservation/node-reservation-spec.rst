Node Reservation and Session Targeting: Specification
=====================================================

Purpose
-------

This document specifies the externally observable behavior of node
reservation and session targeting in PRRTE: the contract a tool,
scheduler, or application may rely on when it asks the DVM to set aside
nodes for later use and to direct spawned jobs onto those nodes.  It
defines *what* the runtime does, not *how* it does it.  The companion
design plan, ``node-reservation.rst``, describes the internal data
structures, code paths, and implementation order; where this
specification and that plan disagree about observable behavior, this
specification is authoritative and the plan must be corrected.

The contract is expressed entirely in terms of PMIx attributes supplied
on allocation and spawn requests and the PMIx status codes returned in
response.  No new command-line options, environment variables, or tool
behaviors are introduced.

Scope
-----

In scope
~~~~~~~~

* The meaning of the ``PMIX_ALLOC_TARGET``, ``PMIX_ALLOC_SHARE``,
  ``PMIX_ALLOC_INHERITANCE``, ``PMIX_ALLOC_WARN_TIMEOUT``, and
  ``PMIX_SPAWN_TARGET`` attributes as interpreted by PRRTE.
* The rules that decide which pool of nodes a dynamically allocated set
  of nodes joins.
* The rules that decide which nodes a spawned job may be mapped onto.
* The ownership model that governs who may target a reservation.
* The events that end a reservation — including the per-reservation
  inheritance disposition — and the observable effect of each.
* The relay of the scheduler's ``PMIX_ALLOC_TIMEOUT_WARNING`` event to the
  requester ahead of an allocation's expiration.
* The behavior guaranteed when the underlying PMIx does not define one or
  more of the attributes above.

Out of scope / non-goals
~~~~~~~~~~~~~~~~~~~~~~~~~~

* PRRTE invents no reservation *policy* of its own.  It does not decide
  on its own to reserve, share, time-out, or reclaim nodes; every such
  decision is driven by an attribute the requester supplied or by the
  scheduler returning nodes.  The sole defaults are those stated below.
* No guarantee is made about the *physical* nodes chosen to satisfy a
  dynamic allocation — node selection remains the scheduler's
  responsibility.  This specification governs only which *session* the
  resulting nodes are placed into.
* Quotas, fair-share, priority, and accounting across reservations are
  not addressed.
* The wire encoding of attributes and the timing of internal state
  transitions are implementation details and are not specified here.

Definitions
-----------

Allocation
   A set of nodes returned by a single resource request, identified by
   its ``PMIX_ALLOC_ID`` string.

Default session
   The general, unreserved pool of nodes.  Every node discovered when the
   DVM starts belongs to the default session, and any job that does not
   target a reservation may map onto it.  The default session is referred
   to in a request by the **invalid namespace** sentinel (an empty
   allocation id / ``PMIX_NSPACE_INVALID``).

Reservation
   A non-default session whose nodes are withheld from the default pool
   and made available only to the namespaces that own it or to jobs that
   explicitly target it by allocation id.

Owner set
   The set of namespaces entitled to spawn jobs onto a reservation's
   nodes.  Reservations have a *set* of owners, not a single owner (see
   `Ownership model`_).  The scheduler is an implicit owner of every
   session.

Namespace / job
   Every job is its own namespace and every namespace maps to exactly one
   job; the two terms are interchangeable in this specification.  "A
   namespace owns a reservation" therefore means "this one job, and any
   process within it, may spawn onto that reservation."

Attribute contract
-------------------

``PMIX_ALLOC_TARGET``
   Supplied on an allocation request to name the namespace a new
   reservation is created for (or whose existing reservation is
   extended).  Honored only when the requester is a **tool**; an
   application that supplies it is rejected (see `Error semantics`_).
   When absent, the reservation is created for the requester's own
   namespace.

``PMIX_ALLOC_SHARE``
   A boolean that governs the allocation's **initial** reservation state
   — its disposition *while the owning namespace lives*.  When absent or
   ``false`` (the default), the nodes are **reserved** for the owning
   namespace.  When ``true``, the nodes are **unreserved** from the
   outset: they join the **default session** and are usable by any member
   of the session.  ``PMIX_ALLOC_SHARE`` is orthogonal to
   ``PMIX_ALLOC_INHERITANCE``: the former describes the allocation while
   its owning namespace lives, the latter what happens when that
   namespace dies.  (This attribute replaces the former
   ``PMIX_ALLOC_RESERVED`` with inverted sense.)

``PMIX_ALLOC_INHERITANCE``
   Supplied on an allocation request to select what becomes of the
   allocation when its **owning namespace** terminates.  Its value is a
   ``pmix_alloc_inheritance_t`` taking one of four dispositions —
   ``PMIX_ALLOC_INHERIT_NONE``, ``PMIX_ALLOC_INHERIT_CHILD``,
   ``PMIX_ALLOC_INHERIT_DEFAULT``, or ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT``
   — whose meanings are defined under `Lifecycle`_.  It is honored on
   ``PMIX_ALLOC_NEW`` and ``PMIX_ALLOC_EXTEND`` requests and ignored on
   ``PMIX_ALLOC_RELEASE``, ``PMIX_ALLOC_REAQUIRE``, and
   ``PMIX_ALLOC_REQ_CANCEL``.  Because the disposition governs behavior at
   the owning namespace's death rather than the reservation's live state,
   it is meaningful even for a shared (unreserved) allocation — there it
   still decides whether the nodes return to the scheduler
   (``NONE``/``CHILD``) or remain in the session.  When absent, the host
   behaves as if ``PMIX_ALLOC_INHERIT_DEFAULT`` had been specified (see
   `Lifecycle`_); the legacy "release to the scheduler on termination"
   behavior must be requested explicitly with ``PMIX_ALLOC_INHERIT_NONE``.
   A non-default value that the host does not support is rejected (see
   `Error semantics`_) rather than silently ignored, so the requester is
   never misled about its allocation's lifetime.

``PMIX_ALLOC_WARN_TIMEOUT``
   A ``uint32_t`` supplied on a ``PMIX_ALLOC_NEW`` or ``PMIX_ALLOC_EXTEND``
   request, giving the number of seconds before the allocation's
   expiration at which the requester wishes to be warned.  It is advisory:
   a scheduler that does not implement it simply ignores it and no warning
   is delivered.  When honored, the scheduler delivers a
   ``PMIX_ALLOC_TIMEOUT_WARNING`` event to the requester as the deadline
   nears; see `Allocation timeout warning`_.

``PMIX_SPAWN_TARGET``
   Supplied on a spawn request to select the allocation(s) whose nodes
   the job may map onto.  Its value is either a single allocation-id
   string or an array of allocation-id strings.  Each entry names a
   reservation by its ``PMIX_ALLOC_ID``; an entry equal to the invalid
   namespace denotes the default session.  When absent, the job uses the
   default session (or its parent's session, exactly as today).

All five attributes are optional.  A request that supplies none of them
behaves exactly as a pre-reservation PRRTE would (see `Backward
compatibility`_).

Allocation behavior
-------------------

When the scheduler returns nodes for a dynamic allocation request,
the nodes are always added to the DVM so daemons can be launched on them.
*Which session they become available to* is decided as follows, by
requester class and the attributes present.

There are three requester/allocation cases:

#. **Startup allocation.**  The nodes discovered when the DVM starts form
   the default session.  No request drives this; it is the baseline.
#. **Tool-requested dynamic allocation.**  A tool (a PMIx client with no
   job of its own — e.g. a scheduler or a ``prun``-class process) issues
   the request.
#. **Application-requested dynamic allocation.**  A process belonging to a
   running job issues the request.

The routing rule is summarized in the decision table below.  "Session
owned by ``<nspace>``" means a reservation whose owner set is seeded with
``<nspace>``.  ``PMIX_ALLOC_SHARE`` governs only the *destination*
(reserved versus shared); ``PMIX_ALLOC_TARGET`` independently designates
the **owning namespace** for inheritance and accounting, which still
applies even when the nodes are shared.

.. list-table::
   :header-rows: 1
   :widths: 18 24 18 40

   * - Requester
     - ``PMIX_ALLOC_TARGET``
     - ``PMIX_ALLOC_SHARE``
     - Destination
   * - Tool
     - ``<nspace>``
     - absent / ``false``
     - reservation owned by ``<nspace>``
   * - Tool
     - absent
     - absent / ``false``
     - reservation owned by the tool's own namespace
   * - Tool
     - absent
     - ``true``
     - default session (shared)
   * - Application
     - absent
     - absent / ``false``
     - reservation owned by the application's namespace
   * - Application
     - absent
     - ``true``
     - default session (shared)
   * - Application
     - present
     - any
     - rejected — ``PMIX_ERR_NO_PERMISSIONS``

New versus extend
~~~~~~~~~~~~~~~~~~

A request carrying the ``PMIX_ALLOC_NEW`` directive always mints a fresh
reservation, even for a namespace that already owns one; the namespace
may thus hold several reservations, each distinguished by its allocation
id.  A request carrying ``PMIX_ALLOC_EXTEND`` adds nodes to the existing
reservation named by the request's ``PMIX_ALLOC_ID``, and succeeds only
when the requester's namespace owns that reservation.

Reporting the result
~~~~~~~~~~~~~~~~~~~~~~

A successful allocation response returns the destination's
``PMIX_ALLOC_ID`` so the requester can later target the reservation when
spawning or extend or release it.  When the request supplied a
``PMIX_ALLOC_REQ_ID``, that id is echoed back in the response.

Spawn targeting behavior
------------------------

A spawn request selects its candidate nodes through ``PMIX_SPAWN_TARGET``:

* **Absent, or naming only the invalid namespace** — the job maps onto
  the default session.  This is the unchanged, pre-reservation behavior.
* **One or more valid allocation ids** — the job may map onto the
  **union** of those reservations' nodes.
* **Valid ids plus the invalid namespace** — the union additionally
  includes the default pool.  This is the supported way to request
  "my reservation, and the general pool too."

Every targeted allocation is subject to the ownership check below.  A
job's candidate node pool is exactly the union of the sessions it is
permitted to target; it can see no other nodes.

A consequence for existing jobs: once any node is reserved, a job that
targets the default session no longer sees reserved nodes.  This
isolation is intended.  A job that targets no reservation behaves
identically to today on an allocation where nothing has been reserved.

Ownership model
---------------

A reservation is owned by a **set** of namespaces:

* The set is seeded with the **owning namespace** — the namespace the
  reservation was created for (the ``PMIX_ALLOC_TARGET`` namespace for a
  tool request, otherwise the requester's own namespace).  A reservation
  has exactly one owning namespace for its lifetime; it is the namespace
  whose termination drives inheritance (see `Lifecycle`_).
* Each time a job is successfully spawned *into* a reservation, that
  job's namespace is added to the owner set as a **child namespace**, and
  may then spawn further jobs onto the same nodes.  This owner set is the
  *authorization* layer — who may map onto and target the reservation.
* Distinct from authorization, a **derived child** is, for lifetime
  purposes, any namespace reachable by following the spawn relationship
  from the owning namespace to arbitrary depth — a descendant at any
  level, not just an immediate child.  Children launched into the
  reservation via ``PMIX_SPAWN_TARGET`` count as derived children.  The
  set of derived children is what the ``CHILD``-flavored inheritance
  rules track (see `Lifecycle`_); it is distinct from the single owning
  namespace that seeded the reservation.
* Ownership is **not** inherited transitively.  A child owns only the
  reservation it was spawned into — never the other reservations its
  parent happens to own.
* The scheduler is an implicit owner of every session and may target or
  release any of them.

A spawn may target a session only if, for **every** entry in
``PMIX_SPAWN_TARGET``, at least one of the following holds: the entry is
the default session; the requester's namespace is in the session's owner
set; or the requester is the scheduler.  If any entry fails this check
the entire spawn is rejected (see `Error semantics`_) — the request is
never silently narrowed to the permitted subset, so the caller always
learns its request was not honored in full.

Error semantics
---------------

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Condition
     - Returned status
   * - Application request carries ``PMIX_ALLOC_TARGET``
     - ``PMIX_ERR_NO_PERMISSIONS``
   * - ``PMIX_ALLOC_EXTEND`` names an allocation the requester does not own
     - ``PMIX_ERR_NO_PERMISSIONS``
   * - ``PMIX_ALLOC_EXTEND`` names an allocation that does not exist
     - ``PMIX_ERR_NOT_FOUND``
   * - ``PMIX_SPAWN_TARGET`` names an allocation that resolves to no session
     - ``PMIX_ERR_NOT_FOUND``
   * - ``PMIX_SPAWN_TARGET`` names a session the requester does not own
     - ``PMIX_ERR_NO_PERMISSIONS``
   * - ``PMIX_ALLOC_INHERITANCE`` carries a non-default value the host
       does not support
     - ``PMIX_ERR_NOT_SUPPORTED``

In every rejection case no partial effect is observable: a rejected
allocation reserves nothing, and a rejected spawn launches no processes.
A non-default ``PMIX_ALLOC_INHERITANCE`` value is rejected rather than
silently ignored so the requester is never misled about its allocation's
lifetime.

Lifecycle
---------

A reservation's lifetime is governed by two independent mechanisms:
**unconditional triggers** that end it regardless of any other state, and
a **namespace-termination disposition** selected at creation by
``PMIX_ALLOC_INHERITANCE`` that decides what its termination does to the
reservation.

Unconditional triggers
~~~~~~~~~~~~~~~~~~~~~~~~

Independently of the inheritance disposition, a reservation ends when any
of these occurs:

#. **An owner releases it** by issuing ``PMIX_ALLOC_RELEASE`` for the
   reservation's allocation id.  Any namespace in the owner set may
   release it; the scheduler may release any reservation.
#. **The DVM tears down.**  All sessions, default and reserved alike, end
   with the DVM.
#. **The scheduler terminates the allocation** — for example on a timeout
   or an administrative reclaim — which arrives as a scheduler-driven
   release for the allocation id.

Namespace-termination disposition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``PMIX_ALLOC_INHERITANCE`` selects, at allocation time, what becomes of
the reservation as its **owning namespace** and **derived children**
(see `Ownership model`_) terminate.  It answers two coupled, orthogonal
questions — *when* the disposition fires (**lifetime**: at the owning
namespace's termination, or only after the last derived child has also
terminated) and *what* it then does (**disposition**: return the nodes to
the scheduler, or merely make them **unreserved** within the session):

``PMIX_ALLOC_INHERIT_NONE``
   When the **owning namespace** terminates, the reservation is released
   and its nodes are returned to the scheduler, even if derived children
   are still running on them.  This is the legacy behavior.
``PMIX_ALLOC_INHERIT_CHILD``
   The reservation outlives its owning namespace and is released — nodes
   returned to the scheduler — only when **all** derived children have
   terminated.
``PMIX_ALLOC_INHERIT_DEFAULT``
   When the **owning namespace** terminates, the nodes are **not**
   returned to the scheduler; they become **unreserved** and remain in
   the session.
``PMIX_ALLOC_INHERIT_CHILD_DEFAULT``
   The nodes become **unreserved** within the session when the **last
   derived child** terminates, rather than being returned to the
   scheduler.

The two axes combine as:

.. list-table::
   :header-rows: 1
   :widths: 34 33 33

   * -
     - **Returned to scheduler**
     - **Unreserved in session**
   * - **At owning-namespace termination**
     - ``PMIX_ALLOC_INHERIT_NONE``
     - ``PMIX_ALLOC_INHERIT_DEFAULT``
   * - **At last-derived-child termination**
     - ``PMIX_ALLOC_INHERIT_CHILD``
     - ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT``

**Returned to the scheduler** runs the same path as an explicit
``PMIX_ALLOC_RELEASE``: the nodes leave the reservation and go back to the
scheduler's general pool (the DVM shrinks for nodes the scheduler owns;
nodes that were carved from the DVM's own startup pool revert to the
default session).  **Unreserved in session** never returns nodes to the
scheduler on this trigger; the named allocation goes away but every node
stays in the DVM, clears its reservation, and joins the default session
for general use.  Such nodes return to the scheduler only when the
session (DVM) itself ends or they are explicitly released — inheritance
alone never hands them back.

Default disposition
~~~~~~~~~~~~~~~~~~~~~

When ``PMIX_ALLOC_INHERITANCE`` is absent, the host behaves as if
``PMIX_ALLOC_INHERIT_DEFAULT`` had been specified: on termination of the
owning namespace the reservation's nodes become unreserved and remain in
the session rather than being returned to the scheduler.  The legacy
"release to the scheduler on termination" behavior is **not** the
default; a requester that wants it must pass ``PMIX_ALLOC_INHERIT_NONE``
explicitly.

Ending a reservation while jobs are still mapped
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Any trigger — an unconditional one or a namespace-termination
disposition — may fire while jobs are still mapped onto the
reservation's nodes (for example, ``PMIX_ALLOC_INHERIT_NONE`` releasing
the reservation while child jobs run on it).  The observable outcome
depends only on whether the nodes leave the DVM:

* **Nodes stay in the DVM** (an owner release whose nodes are not
  returned to the scheduler, or a ``*_DEFAULT`` disposition becoming
  unreserved).  No process is killed.  The nodes revert to the default
  pool, already-running jobs continue undisturbed, and only the mapping of
  *new* jobs stops seeing the reservation.
* **Nodes leave the DVM** (a scheduler reclaim or timeout, a ``NONE`` or
  ``CHILD`` disposition returning nodes to the scheduler, or any release
  of scheduler-owned nodes).  This is the existing DVM-shrink behavior:
  jobs and daemons on the departing nodes are terminated before the nodes
  leave.

Reservation teardown never itself kills a job; process termination occurs
only when nodes physically depart, under the pre-existing shrink
behavior.

Allocation timeout warning
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Scheduler reclaim (unconditional trigger 3) is frequently driven by a
bounded allocation lifetime: when the granted period expires the
scheduler reclaims the nodes, terminating any jobs still running on them.
To let an owner checkpoint, drain work, or request an extension before
that happens, a requester may ask for advance notice when it creates or
extends the allocation.  PRRTE acts purely as the **relay** between the
requester and the scheduler here — it neither sets the timeout nor
generates the warning:

* On a ``PMIX_ALLOC_NEW`` or ``PMIX_ALLOC_EXTEND`` request the requester
  includes ``PMIX_ALLOC_WARN_TIMEOUT`` (``uint32_t`` seconds before
  expiration).  PRRTE forwards it to the scheduler.  Being advisory, a
  scheduler that does not implement it ignores it and no warning follows.

* When the threshold is reached the scheduler issues a
  ``PMIX_ALLOC_TIMEOUT_WARNING`` event.  PRRTE delivers it **only to the
  process that requested the allocation** — it is a directed event, not a
  broadcast — carrying:

  - ``PMIX_ALLOC_ID`` (``char*``) — the affected allocation; always
    present.
  - ``PMIX_ALLOC_REQ_ID`` (``char*``) — the requester's own request id,
    included whenever one was supplied on the original request, so the
    recipient can match the warning by either identifier.
  - ``PMIX_TIME_REMAINING`` (``uint32_t``) — seconds left before
    expiration, in the same units as ``PMIX_ALLOC_WARN_TIMEOUT``.

* The recipient reacts within its event handler — typically by issuing a
  ``PMIX_ALLOC_EXTEND`` (optionally renewing ``PMIX_ALLOC_WARN_TIMEOUT``)
  to lengthen the allocation, or by beginning an orderly drain or
  shutdown — and returns the appropriate event status.  If the allocation
  is not extended, it expires and the scheduler reclaims its nodes through
  the existing shrink path described above, terminating the jobs still
  mapped onto them.

The warning is strictly advance notice; it changes nothing about the
reservation by itself.  Only the subsequent expiration (or an explicit
extend/release) alters the allocation, and that flows through the
unconditional triggers and dispositions already specified.

Backward compatibility
----------------------

Every attribute in this specification is optional, and PRRTE must build
and run against a PMIx that defines none, some, or all of them.

* When ``PMIX_ALLOC_TARGET`` is unavailable, a dynamic allocation is
  always reserved to the requester's own namespace.
* When ``PMIX_ALLOC_SHARE`` is unavailable, the "reserve" default applies
  and nodes are never routed to the default pool by a dynamic request.
* When ``PMIX_ALLOC_INHERITANCE`` is unavailable, every reservation takes
  the default disposition (``PMIX_ALLOC_INHERIT_DEFAULT``): on termination
  of its owning namespace the nodes become unreserved within the session.
  The non-default dispositions — in particular the legacy
  ``PMIX_ALLOC_INHERIT_NONE`` (release to the scheduler) — cannot be
  requested, and a request carrying a non-default value is rejected (see
  `Error semantics`_).
* When ``PMIX_ALLOC_WARN_TIMEOUT`` is unavailable, no advance timeout
  warning is requested; an allocation still expires and is reclaimed by
  the scheduler as before, only without the advisory
  ``PMIX_ALLOC_TIMEOUT_WARNING`` event beforehand.
* When ``PMIX_SPAWN_TARGET`` is unavailable, spawns use the default or
  parent session exactly as they do today.

A DVM whose PMIx lacks all five attributes still partitions resources
sensibly — dynamic allocations reserve to the requesting namespace,
reservations become unreserved within the session when their owning
namespace exits, allocations expire and are reclaimed without an advance
warning, and spawns use the default/parent session — and exhibits no
behavior change for any request beyond that prescribed default
disposition.

Conformance summary
-------------------

A conforming implementation guarantees that:

#. A job's candidate nodes are exactly the union of the sessions it is
   permitted to target, and never include a reserved node it does not
   own.
#. A reservation is created, extended, shared, targeted, or released only
   in response to a requester-supplied attribute or a scheduler action —
   never by PRRTE's own initiative.
#. Every rejection listed in `Error semantics`_ leaves no observable
   partial effect.
#. A reservation ends on an unconditional trigger (owner release, DVM
   teardown, or scheduler termination) or on the namespace-termination
   disposition recorded at its creation — defaulting to
   ``PMIX_ALLOC_INHERIT_DEFAULT`` (unreserve in session) when none was
   supplied — and on no other event.
#. Nodes are returned to the scheduler only under a ``NONE``/``CHILD``
   disposition, an explicit release, or session (DVM) teardown; a
   ``*_DEFAULT`` disposition only ever unreserves nodes within the
   session.
#. A ``PMIX_ALLOC_TIMEOUT_WARNING`` event, when the scheduler honors a
   ``PMIX_ALLOC_WARN_TIMEOUT`` request, is relayed only to the process
   that requested the allocation, and is advisory — it alters nothing
   about the reservation by itself.
#. The defaults and rejections above hold unchanged when the underlying
   PMIx lacks any of the five attributes.
