Data Store Retention: Specification
===================================

Purpose
-------

This document specifies the externally observable retention behavior of
the PRRTE data store — the service behind ``PMIx_Publish``,
``PMIx_Lookup`` and ``PMIx_Unpublish``.  It defines what a publisher may
rely on about how long its data is kept, what a reader may rely on about
finding it, and what a site administrator may set to bound what the
store consumes.  It describes *what* the runtime does, not *how* it does
it; the companion design and implementation plans will describe the
internal structures and the order of work, and where they disagree with
this document about observable behavior, this document is authoritative
and the plans must be corrected.

The immediate driver is `issue #2733
<https://github.com/openpmix/prrte/issues/2733>`_ — an unread
``PMIX_PERSIST_FIRST_READ`` item is deleted when its publisher's job
ends — but the fix for that issue alone would leave the larger problem
in place: in a DVM that outlives the jobs it runs, nothing ever reclaims
a ``PMIX_PERSIST_SESSION`` or ``PMIX_PERSIST_INDEF`` item, and only the
publisher — by then gone — is permitted to remove one.  This
specification therefore covers retention as a whole, and with it the
question that wedged name raises: **who may remove a published item**.

Scope
-----

In scope
~~~~~~~~

* The retention criterion PRRTE applies to each ``PMIX_PERSISTENCE``
  value, and the event that satisfies it.
* An automatic expiration timeout applied to the two persistences that
  otherwise have no bounded criterion.
* A cap on the amount of data a store will hold, and what happens when
  the cap is reached.
* The MCA parameters that control both.
* Who may remove a published item, which this specification widens from
  the publishing process to the publishing *user*.
* A new persistence, ``PMIX_PERSIST_NSPACE``, and the correction to what
  ``PMIX_PERSIST_APP`` means.
* The status codes a publisher or reader observes as a result.
* Where this behavior deliberately departs from the PMIx Standard, and
  why.

Out of scope / non-goals
~~~~~~~~~~~~~~~~~~~~~~~~

* Who may *read* an item — the publisher's access permissions and the
  two range checks.  Those are unchanged by this specification; see
  ``src/runtime/data_server/AGENTS.md``.  Removal *is* in scope, and is
  specified below.
* The duplicate-key rule itself.  A key becomes republishable once the
  item holding it has gone, which is stated below; what counts as a
  duplicate is unchanged.  ``prte.pub.replace`` is touched only insofar
  as widening removal implies widening it — see `Who may remove an
  item`_.
* Which store a request reaches.  That is decided by the range, and is
  unchanged.
* Cross-DVM relay (``prte_pmix_server_uri``).  A relayed request is
  executed by the *remote* store under that store's own retention rules;
  nothing here is negotiated across the boundary.
* Persistent storage.  A store lives in the memory of the daemon holding
  it and does not survive that daemon.

Definitions
-----------

Store
   The set of published items held by one daemon.  Every daemon has one.
   A ``PMIX_RANGE_LOCAL`` publish lives in the store of the daemon that
   relayed it; everything else lives in the DVM master's store.

Item
   Everything one ``PMIx_Publish`` call stored: its keys and values, the
   publishing process's identity, its uid and gid, the range, the
   persistence, and any accessor lists.  An item is the unit of
   retention — it is removed whole, except that a ``FIRST_READ`` item
   loses individual keys as they are read and leaves the store when its
   last key is gone.

Publisher
   The process that made the ``PMIx_Publish`` call.  For a relayed
   request this is the process named in ``PMIX_REQUESTOR``, not the
   relaying daemon.

Application
   One app context — one executable with its own argv, environment and
   process count.  A single ``prun`` may specify several, and in an MPMD
   job they run concurrently.  An application terminates when all of its
   own processes have terminated, which in an MPMD job is not the same
   moment for every app.

Namespace
   The job.  All of a job's applications share one namespace, because
   the namespace is assigned to the job rather than to the app.  A
   namespace terminates when its last application does.

Session
   A PRRTE session object — an allocation.  Every job runs within
   exactly one session; a DVM that was given no allocation runs its jobs
   in the default session, whose lifetime is the DVM's.

Horizon
   The lifetime that has just ended, as reported to the store.  A purge
   naming a horizon removes the items whose retention criterion that
   horizon satisfies.

Retention rules
---------------

An item is removed when the **first** of the following occurs:

#. its persistence criterion is met (the table below);
#. it is removed with ``PMIx_Unpublish`` by a process entitled to remove
   it (`Who may remove an item`_);
#. it is evicted to keep the store within its size cap (`Storage cap`_);
#. the daemon holding the store exits.

The persistence criterion is decided solely by the ``PMIX_PERSISTENCE``
value the publisher gave.  A publisher that gives none gets
``PMIX_PERSIST_NSPACE`` — see `The default persistence`_.

.. list-table::
   :header-rows: 1
   :widths: 22 40 38

   * - Persistence
     - Removed when
     - Notes
   * - ``PMIX_PERSIST_FIRST_READ``
     - the key is returned by a lookup; or, if no lookup ever asks for
       it, when the retention timeout expires
     - The publisher's own departure is **not** a criterion.  An item
       published for a reader that has not started yet survives the
       publisher's job.
   * - ``PMIX_PERSIST_PROC``
     - the publishing process terminates
     - Removed no later than the end of the publisher's job.
   * - ``PMIX_PERSIST_APP``
     - the publisher's **application** terminates — every process of
       that app context, and no further
     - In an MPMD job the publishing app's data goes when *that app*
       ends, while its siblings keep running; in a single-app job it is
       indistinguishable from the end of the namespace.
   * - ``PMIX_PERSIST_NSPACE``
     - the publisher's **namespace** terminates — every application in
       the job
     - New; requires the constant to be added to PMIx (see `A new
       persistence`_).  This is what "until my job is over" needs, what
       nothing available today says, and **PRRTE's default**.
   * - ``PMIX_PERSIST_SESSION``
     - the session (allocation) the publisher was running within is
       torn down
     - For a DVM with no allocation of its own, that is the end of the
       DVM.
   * - ``PMIX_PERSIST_INDEF``
     - the retention timeout expires with no access
     - Retained indefinitely *in use*: each successful lookup restarts
       the clock.  It is an idle timeout, not a lifetime.
   * - ``PMIX_PERSIST_INVALID``
     - treated as ``PMIX_PERSIST_NSPACE``
     - A publisher that gives no persistence gets the default.

Two properties are guaranteed across the whole table:

* **Every item has a bounded lifetime.**  No sequence of publishes can
  leave a store holding data forever, and no item's removal depends on a
  process that has already exited.
* **A publisher's departure removes only what the publisher asked to
  have removed by it.**  ``PROC``, ``APP`` and ``NSPACE`` say "delete
  this when I, my application, or my job goes away"; the others do not,
  and are not removed by those events.
* **Every removal is one the owner authorized** — by calling
  ``PMIx_Unpublish``, or by declaring the criterion at publish.
  ``FIRST_READ`` is the only criterion another process can satisfy on
  the owner's behalf, and it says so on its face (`The one exception`_).

A new persistence
-----------------

``PMIX_PERSIST_APP`` means what it says: the **application**.  In an
MPMD job that is one app context among several, all of them sharing the
job's namespace, and each ending when its own processes do.  Nothing in
the existing set says "retain until my whole job is over" — and that is
the thing a publisher usually means.

This specification therefore requires a new constant:

``PMIX_PERSIST_NSPACE``
    Retain the data until the publishing process's namespace terminates
    — that is, until every application in the job has ended.

Two prerequisites, both outside this document:

* **PMIx must define the constant.**  The next free value is ``5``
  (``INDEF`` 0, ``FIRST_READ`` 1, ``PROC`` 2, ``APP`` 3, ``SESSION`` 4).
  It has to be appended rather than slotted between ``APP`` and
  ``SESSION``, because these are wire values; the numbering has never
  been a ladder anyway — ``INDEF`` is 0 and outlives every one of them —
  so PRRTE spells the ordering out rather than comparing, and one more
  out-of-order value costs nothing.  No capability gate is warranted:
  PRRTE requires PMIx 7.0.0 or newer, so a constant added during the
  7.0 series is present in every PMIx that can build PRRTE.
* **It should be proposed to the Standard**, which has the same gap for
  the same reason.

Why the gap has gone unnoticed is worth recording: MPI requires all of a
job's applications to terminate together, so for an MPI job the app
horizon and the namespace horizon coincide.  That is an MPI requirement,
not a general one, and a non-MPI MPMD job under PRRTE can have one app
outlive another by any amount.

What each horizon takes
~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Lifetime that ended
     - Persistences removed
   * - a process
     - ``PROC``
   * - an application
     - ``PROC``, ``APP``
   * - a namespace
     - ``PROC``, ``APP``, ``NSPACE``
   * - a session
     - ``PROC``, ``APP``, ``NSPACE``, ``SESSION``

``FIRST_READ`` and ``INDEF`` appear in no row: neither is removed by a
lifetime ending.  They go by the read, by the timeout, by an unpublish,
or by eviction.

The default persistence
~~~~~~~~~~~~~~~~~~~~~~~

**A publish that names no persistence gets ``PMIX_PERSIST_NSPACE``.**
The Standard's default is ``APP``, so this is a deliberate departure,
and it is the one that keeps the ``APP`` correction from being a
regression: a publisher that never thought about persistence has always
had its data last until its job ended, and reading ``APP`` correctly
would silently shorten that for every MPMD job.  ``NSPACE`` is what
those publishers have been getting and what they evidently meant.  A
publisher that genuinely wants app lifetime asks for ``APP`` and gets
it.

Who may remove an item
----------------------

**Removal is an ownership question, and published data is owned by the
user that published it.**  This is unchanged in substance from the rule
PRRTE already applies — an owner, and only an owner, may take an item
back.  What changes is how the owner is identified: by the effective uid
that published the data rather than by the process that published it.

Removal is decided by identity alone.  Neither the range the item
carries nor a range named on the unpublish narrows what an owner may
take back, and no access the publisher granted widens it.

The rule
~~~~~~~~

A process may remove an item when it presents the **same effective uid**
as the uid recorded for the item at publish, and the **same effective
gid** when both are known.  Where a gid is not available — the store
reads ``UINT32_MAX``, which is what a PMIx that did not hand one over
yields — the test degrades to uid alone rather than refusing everyone,
which is the same degradation the read rule makes.

Three things follow, and each is deliberate:

* **The publishing process need not still exist.**  A later job of the
  same user — a different namespace entirely — may take back a name its
  predecessor left behind.  That is the whole point: today an item whose
  publisher died before it could unpublish is wedged for the life of the
  DVM, readable by its owner's next job and removable by nobody.
* **Ownership and access are separate questions, in both directions.**
  A publisher that names ``PMIX_ACCESS_USERIDS`` or
  ``PMIX_ACCESS_GRPIDS`` widens who may *see* the data and nothing else:
  a uid on that list may read the item and may not remove it.  The
  converse holds too, and is the sharper case — a publisher whose own
  uid its accessor list excludes cannot look its own data up, and may
  still remove it, because it owns it.  Being able to read an item is
  never a reason to be able to delete it.
* **Nothing crosses a user boundary.**  A process of a different uid
  cannot remove an item, and there is still no administrative override:
  no tool, scheduler, or later job of another user can remove data it
  does not own.  Bounding the store against another user is what the
  per-uid cap and the timeout are for.

The one exception
~~~~~~~~~~~~~~~~~

``PMIX_PERSIST_FIRST_READ`` is the single place where data leaves the
store at the hand of somebody other than its owner: the *reader's*
lookup is what removes it, and that reader may be any process the
publisher's access permissions admit — a different namespace, and if the
publisher named an accessor list, a different uid.

That does not weaken the ownership rule, because the removal was
authorized by the owner at publish.  ``FIRST_READ`` **is** an
instruction to delete on first access; a publisher choosing it has asked
for exactly this, and a publisher that has not chosen it is never
exposed to it.  It is worth stating plainly all the same, since it is
the one case where "only the owner removes an item" is not literally
true:

* the trigger is a read that the publisher's own permissions allowed;
* the criterion is the one the publisher declared at publish;
* no other persistence behaves this way, and no directive on a lookup
  can make one behave this way.

The consequence a publisher should know: ``FIRST_READ`` combined with a
wide accessor list means the *first* admitted reader consumes the item,
whoever that turns out to be.  Where the item is meant for one
particular successor, the accessor list is what makes that so.

Scope of each form
~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Call
     - Removes
   * - ``PMIx_Unpublish(keys, ...)``
     - every item holding one of those keys that the caller's uid (and
       gid) owns, regardless of which process or namespace published it
   * - ``PMIx_Unpublish(NULL, ...)``
     - only what the **calling process itself** published — unchanged

The asymmetry is intentional, and it is not a new restriction — the
no-keys form means "everything this process published" today and keeps
that meaning.  It is a blunt sweep whose scope is implicit: the caller
names nothing, so nothing in the call says what it is about to take.
That is what a process says while tidying up after itself, and a job
that says it at teardown must not take out its own user's other running
jobs.  Reaching across a namespace boundary should require naming the
key being reached for.

In PRRTE the two forms are not even the same operation.  A NULL key list
becomes ``PRTE_PMIX_PURGE_PROC_CMD`` naming the caller
(``pmix_server_pub.c``), which the store executes as "drop every item
this proc owns, whatever its persistence" — the same command the state
machine sends when a lifetime ends, differing only in that a lifecycle
purge names the horizon that was reached.  The uid widening therefore
applies to the ordinary by-key unpublish, and leaves the purge path
alone.

Consequences elsewhere
~~~~~~~~~~~~~~~~~~~~~~

* **``prte.pub.replace`` widens with it.**  The directive lets a
  publisher take back its own prior publication of the keys it is
  publishing instead of being refused ``PMIX_ERR_DUPLICATE_KEY``; it is
  owner-scoped so that it can never be a way to seize a live name.  Once
  a same-uid process may unpublish a key and then publish its own, the
  two-step is available anyway, so the directive is scoped to the same
  test as removal — uid (and gid) rather than process identity.  A
  colliding item belonging to a *different* user still refuses the
  publish, with or without the directive.
* **The relay must carry the publisher's uid.**  Identity for both
  storage and removal is the *originating* process's, never the relaying
  daemon's.  A cross-DVM request already corrects the owning process via
  ``PMIX_REQUESTOR``; under a uid rule the effective uid and gid have to
  cross the same way, or a relayed publish is stored under the relaying
  daemon's identity and a relayed unpublish is tested against it.

Key reuse
~~~~~~~~~

A key is republishable — by anyone the duplicate rule admits — as soon
as the item holding it has been removed, whichever of the four reasons
removed it.  This is what makes a handover between successive
generations of a job work.  There are now two conforming ways to do it,
and they answer different needs: a ``FIRST_READ`` item is consumed by
the successor's own lookup and needs no cleanup call, while same-uid
removal lets a successor reclaim a name whose publisher died before it
could unpublish — including one published ``SESSION`` or ``INDEF``,
which no read consumes.

Retention timeout
-----------------

The two persistences with no other bounded criterion —
``PMIX_PERSIST_INDEF``, and ``PMIX_PERSIST_FIRST_READ`` for a key nobody
reads — are subject to an automatic timeout.

* The timeout is a **whole-item idle timeout**.  The clock starts when
  the item is published and restarts on each lookup that successfully
  returns one of its keys.  A key removed by a ``FIRST_READ`` lookup is
  gone at that instant; what the restart governs is the rest of the item.
* Its value is ``prte_data_server_timeout`` seconds, default **300**
  (five minutes).  Zero disables the timeout, restoring unbounded
  retention for these two persistences.
* An item is removed **no earlier** than its timeout, and normally
  within one sweep interval after it.  A caller may therefore observe an
  item slightly past its nominal expiry; it may not observe one removed
  early.
* Expiry is silent to the publisher.  A reader that arrives afterwards
  gets ``PMIX_ERR_NOT_FOUND``, exactly as if the item had never been
  published.
* The timeout does not apply to ``PROC``, ``APP`` or ``SESSION`` items.
  Those already have a criterion that a running system will reach, and
  cutting them short would break the retention their publishers were
  promised while they are still alive to rely on it.

The five-minute default is chosen for the case the timeout exists to
bound: job A publishes a name for job B to pick up and exits, and the
user then decides not to run job B.  It is a rendezvous window, not a
storage lifetime.  A site whose handovers take longer should raise it.

Storage cap
-----------

Each store enforces a cap on the amount of published data it holds, and
the cap is **applied per publishing uid**.

* The cap is ``prte_data_server_max_size`` bytes **per uid**, default
  **16777216** (16 MiB).  Zero disables the cap.
* Size is accounted as the stored keys and values of an item plus its
  fixed per-item overhead, summed over the items published by that uid.
  Parked lookup requests are not counted.
* When storing a new item would take its publisher's uid over the cap,
  the store evicts **that uid's own items**, least recently used first,
  until the new item fits.  "Least recently used" means the item whose
  last successful lookup is furthest in the past; an item nobody has
  ever looked up is ordered by its publish time.  That is the same clock
  the retention timeout reads, so the two agree about what stale means:
  a key nobody has asked for in an hour is evicted ahead of one being
  read every minute.
* **Eviction never crosses a uid boundary.**  A user who floods the
  store evicts only their own data; no publisher can be made to lose an
  item by anything another user does.  This is what keeps the cap from
  being an abuse primitive in its own right: without the scoping, junk
  published in bulk is a way to push somebody else's rendezvous name out
  of the store.
* Within a uid, eviction ignores persistence: an ``INDEF`` item that
  nobody has touched in an hour goes before an ``APP`` item that uid
  published a second ago.  A user's own data is a user's own problem to
  budget.
* A single publish larger than the whole per-uid cap is refused with
  ``PMIX_ERR_OUT_OF_RESOURCE`` and stores nothing.  No item is evicted
  on behalf of a publish that cannot fit regardless.
* Eviction is the store protecting itself, not a policy anyone asked
  for, so it is **reported**: the first eviction for a given uid in a
  DVM's lifetime emits a ``show_help`` warning naming the cap and the
  parameter that raises it, and each eviction is traced at the store's
  verbosity.
* A reader whose item was evicted gets ``PMIX_ERR_NOT_FOUND``.  There is
  no separate status for "it was here and we dropped it"; a publisher
  cannot distinguish eviction from a reader that never arrived, and is
  not meant to.

The cap is per uid **per store**, not per DVM: a DVM whose daemons each
hold local-range data can hold up to the cap for a given uid on each of
them.  The aggregate a store may hold is therefore the cap times the
number of distinct uids that have published to it, which is bounded in
practice by the accounts able to reach the DVM at all — see the open
question on an aggregate backstop.

Observable status codes
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Situation
     - Status
   * - Lookup of a key that expired, was evicted, was consumed by a
       ``FIRST_READ``, or was never published
     - ``PMIX_ERR_NOT_FOUND``
   * - Lookup of a key that exists but whose accessor list or range
       excludes the requestor
     - ``PMIX_ERR_NO_PERMISSIONS`` (unchanged)
   * - Publish of a key already published to the same set of processes
     - ``PMIX_ERR_DUPLICATE_KEY`` (unchanged)
   * - Publish larger than the caller's whole per-uid cap
     - ``PMIX_ERR_OUT_OF_RESOURCE``
   * - Unpublish naming a key published by a process of a different uid
     - ``PMIX_SUCCESS``, and that item is untouched.  An unpublish
       reports that the request was carried out, not how many items it
       matched; a caller cannot use it to probe for another user's keys.

MCA parameters
--------------

.. list-table::
   :header-rows: 1
   :widths: 32 12 56

   * - Parameter
     - Default
     - Meaning
   * - ``prte_data_server_timeout``
     - ``300``
     - Seconds of idleness after which a ``PMIX_PERSIST_INDEF`` or
       unread ``PMIX_PERSIST_FIRST_READ`` item is removed.  ``0``
       disables the timeout.
   * - ``prte_data_server_max_size``
     - ``16777216``
     - Maximum bytes of published data one store will hold **for one
       uid**.  Reaching it evicts that uid's own items, least recently
       used first.  ``0`` disables the cap.

Both are read by every daemon.  Setting them on the ``prte`` command
line applies them DVM-wide; a per-daemon setting is honored but a store
whose cap differs from its peers' is a configuration accident, not a
feature.

Departures from the PMIx Standard
---------------------------------

Five of the rules above are deliberate departures.  The first three
exist because the Standard's rule, read literally, gives a persistent
DVM no way to bound what it holds; the fourth, because it gives a user
no way to clean up after their own dead job; the fifth, because
correcting a misreading should not silently take retention away from
publishers who never asked for the change.

``PMIX_PERSIST_INDEF`` is not indefinite
    The Standard says the data is retained "until specifically deleted",
    and only the publisher may specifically delete it.  In a DVM that
    outlives the publisher, that is a permanent allocation made by a
    process that no longer exists, and repeating it is a
    denial-of-service that needs no malice — a crash loop that
    republishes under a fresh namespace gets there on its own.  PRRTE
    retains such an item for as long as it is being used and reclaims it
    when it is not.

``PMIX_PERSIST_SESSION`` is tied to the allocation, and is enforced
    The Standard's session is the PMIx session; PRRTE's is the
    allocation the publisher was running within, which is the boundary
    that actually ends.  Today no session-end purge is ever sent, so
    ``SESSION`` behaves as ``INDEF``; this specification requires that
    it be enforced.

``PMIX_PERSIST_FIRST_READ`` outlives its publisher, and then expires
    The Standard's criterion is the first access and nothing else, which
    is why the publisher's job ending must not take the item — that half
    is the Standard being right and PRRTE being wrong (#2733).  But an
    item nobody ever reads would then never go, so the timeout applies.
    A publisher gets the retention it asked for whenever a reader
    arrives within the window; what it does not get is a permanent
    reservation on the strength of a reader that never came.

``PMIx_Unpublish`` is scoped to the user, not to the process
    The Standard defines it as removing "data posted by this process".
    Read strictly, an item outlives every process entitled to remove it
    the moment its publisher exits — and since the publisher is also the
    only one who could have published over the name, the name is then
    unusable as well as unremovable.  That is precisely the state a
    crashed job leaves behind, and it lasts for the life of the DVM.
    PRRTE therefore identifies the owner as the publisher's *user*
    rather than the publisher's process id.  Ownership remains the sole
    basis for removal — no access a publisher granted confers it — and
    the boundary that matters for safety, the one between users, does
    not move.

The default persistence is ``NSPACE``, not ``APP``
    The Standard's default is ``APP``, and ``APP`` means the app
    context.  Applying both literally would shorten the default
    retention of every MPMD publisher that never named a persistence —
    a change nobody asked for, arriving as a side effect of a bug fix.
    PRRTE defaults to ``NSPACE``, which is the lifetime those publishes
    have actually been getting.  ``APP`` remains available and now means
    what it says.

Nothing here weakens a rule the Standard imposes for safety: no process
gains the ability to read or remove another **user's** data, and the
access permission and range rules are untouched.

Where each rule is enforced
---------------------------

Not observable behavior, but recorded here because it is what makes the
rules above cheap enough to be worth specifying.  There are two stores,
and between them they already know almost everything the rules need.

**The daemon's store** holds only ``PMIX_RANGE_LOCAL`` items, published
by its own local clients.

* *A process terminates.*  The daemon launched it and reaps it, so it
  knows directly.  ``PROC`` items are purged locally, with no message
  from anyone.
* *A namespace terminates.*  The master already xcasts
  ``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD`` to every daemon on job completion
  (``state_dvm.c``), so the notice exists; the daemon has only to act on
  it.  Note it is sent only on a persistent DVM — on a one-shot run the
  DVM is going away regardless, so nothing is left unreclaimed.
* *A session terminates.*  A teardown that returns nodes to the
  scheduler terminates the daemons on them, which takes their stores
  with them.  A teardown that does *not* — a reservation released back
  into the default pool, its daemons still running — is the case to
  confirm during design: those daemons may hold local-range items of the
  session that just ended.
* *An application terminates.*  The daemon cannot know: its own
  processes of that app may be done while the app runs on elsewhere.
  This is the one missing notification — see below.

**The master's store** holds everything else.  The master tracks every
process's state and holds the job objects, so process, application,
namespace and session terminations are all known to it directly.  Each
purge is a local call in a path it already runs; none of them needs a
message.

The upshot is that ``PROC`` at true process granularity — the cost that
made job-granularity a deliberate compromise — turns out not to need a
message per terminating process after all.  The store that has to act is
in both cases the one that already knows.

To be implemented later: the app-termination notice
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The single gap is that the master does not tell the daemons when an
*application* has terminated, so a daemon cannot honor ``APP`` for a
local-range item.  Deferred deliberately: it needs a new notification,
it affects only local-range publishes with an explicit ``APP``
persistence — not the default, which is now ``NSPACE`` — and no reported
problem depends on it.  Until it exists, a daemon's store treats a local
``APP`` item as ``NSPACE``, which is what it does today and is never
shorter than the publisher asked for.  The master's store honors ``APP``
properly from the start.

Where the implementation stands
-------------------------------

Kept current as the work lands, so that a reader can tell what this
document still describes as *intended* from what it now describes as
*true*.  The build order is in :doc:`impl-plan`, and what a *user* needs
to know is in :doc:`/how-things-work/publish-lookup`.

Everything specified here is implemented, with one deliberate exception
recorded in ``docs/todo.rst``: no message tells a daemon that an
application has terminated, so a ``PMIX_RANGE_LOCAL`` item published with
an explicit ``APP`` persistence is held until its namespace ends —
later than asked for, never shorter, and not the default.

.. list-table::
   :header-rows: 1
   :widths: 24 12 64

   * - Rule
     - State
     - Notes
   * - ``FIRST_READ`` outlives its publisher
     - done
     - No horizon takes it; removed by the read (issue #2733).
   * - Removal by uid
     - done
     - ``prte_data_server_owns()``; scopes ``prte.pub.replace`` too, and
       the cross-DVM relay carries the originating uid and gid.
   * - ``PROC`` at process granularity
     - done
     - A direct call on each store as the process dies, not a message.
   * - ``APP`` at application granularity
     - done
     - Per-app termination counting at the master; the purge names the
       app index.  A daemon's local-range store still treats a local
       ``APP`` item as ``NSPACE`` — see the deferred notice below.
   * - ``NSPACE``
     - done
     - The new constant, the job-end horizon, and PRRTE's default.
   * - ``SESSION``
     - done
     - Purged at reservation teardown, selected by the session id
       recorded at publish; covered end to end in the swarm's
       ``test_session`` phase.
   * - Retention timeout
     - done
     - ``prte_data_server_timeout``, default 300 s; one idle sweep per
       store, armed only while something it applies to is held.
   * - Per-uid storage cap
     - done
     - ``prte_data_server_max_size``, default 16 MiB per uid; eviction
       confined to that uid's own least-recently-used items.

Decisions taken
---------------

Every choice this specification had to make is settled; they are
recorded here with the alternative each one turned down, so that a
future reader can see what was weighed rather than reopen it.  The
design is in :doc:`datastore-retention` and the build order in
:doc:`impl-plan`.

#. **The timeout is an idle timeout, not a lifetime.**  Each successful
   lookup restarts it, so a rendezvous name in active use is never
   pulled out from under its readers.  Measuring from publish would be
   simpler and would make an item's removal predictable from the publish
   alone; it would also expire names that are demonstrably in service.
   The same clock then orders eviction, so the two agree about what
   stale means.

#. **One timeout, not two.**  A single parameter governs ``INDEF`` and
   unread ``FIRST_READ`` alike.  Five minutes is chosen for the handover
   case; a site whose handovers take longer raises it for both.  A
   second parameter buys a distinction nobody has yet needed.

#. **No per-publish retention override.**  A PRRTE-specific publish
   directive could let one handover ask for an hour without raising the
   parameter DVM-wide, but nothing in the Standard offers it,
   ``PMIX_TIMEOUT`` on a publish already means the time limit on the
   *operation*, and a site-wide parameter answers the same need.  It can
   be added later without changing anything specified here.

#. **Eviction is strict least-recently-used within the uid, and ignores
   persistence.**  Evicting the timeout-bearing classes first would
   spare an item whose publisher is still alive, at the cost of a rule
   with two dimensions rather than one.  Since eviction never crosses a
   uid boundary, what it drops is always the evicting user's own least
   used data, and that user is the one who can judge it.

#. **The cap is counted in bytes.**  A cap on the number of items is
   cheaper to enforce exactly and easier to explain, but bounds nothing
   about memory: one item can hold a megabyte.

#. **No aggregate cap on top of the per-uid one.**  The store's total is
   the cap times the number of distinct uids that publish to it, and
   reaching a DVM at all takes an account.  A second, whole-store cap
   would reintroduce in narrower form the question the per-uid cap
   answers: choosing a victim across uids is choosing whose data to
   drop.

#. **The gid must match too, where both are known.**  This mirrors the
   default read rule and degrades to uid-only exactly as that rule does
   when PMIx hands over no gid.  It means a process whose primary gid
   differs from the publisher's cannot clean up after it even though the
   same person owns both — the conservative reading, and in the common
   case of a user's jobs sharing a primary gid it changes nothing.

#. **``PMIx_Unpublish(NULL, ...)`` keeps its present scope** — everything
   the calling **process** published.  It is a blunt sweep whose scope is
   implicit, and a job that says it while tidying up at teardown must not
   take out its own user's other running jobs.  Reaching across a
   namespace boundary requires naming the key.

#. **The timeout is still needed even with uid-scoped removal.**  Same-uid
   removal lets a successor reclaim a wedged ``SESSION`` or ``INDEF``
   name directly, which is the concrete harm the timeout also addresses.
   But nothing reclaims a *departed* user's data, and no one is obliged
   to come back and clean up, so the store still needs a rule that does
   not depend on anybody returning.

Settled earlier, and recorded in the text above rather than here: the
default persistence is ``PMIX_PERSIST_NSPACE``; ``PROC`` and ``APP`` are
honored at their true granularity, because the store that must act
already knows (`Where each rule is enforced`_); and the daemon-side
``APP`` notice is deferred rather than designed around.
