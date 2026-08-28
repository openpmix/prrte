Data Store Retention: Design
============================

This document describes how the behavior specified in :doc:`spec` is to
be built.  The specification is authoritative about *what* the runtime
does; where this design and that specification disagree about observable
behavior, this document is wrong and must be corrected.  The build order
is in :doc:`impl-plan`.

Everything here lives in ``src/runtime/data_server/`` except the purge
senders, which live in the state machine, and one constant, which lives
in PMIx.

Starting position
-----------------

Two facts about the existing code shape everything below.

**There are two stores, and they hold different things.**  Every daemon
runs ``prte_data_server_init()``.  A prted's store holds *only*
``PMIX_RANGE_LOCAL`` items published by its own local clients; the
master's store holds everything else.  Which one a request reaches is
decided by the range in ``execute()``
(``src/prted/pmix/pmix_server_pub.c``) — one routing decision, no
fallback.

**Each store already knows what it must act on.**  A daemon launches and
reaps its own children, so it sees a process terminate directly; the
master tracks every process's state and holds the job objects, so it
sees process, application, namespace and session terminations.  No rule
in the specification needs a message that does not already exist, with
one exception recorded at the end.

Data structures
---------------

``prte_data_object_t`` (``ds.h``)
    Four new fields, all filled in at publish:

    ``uint32_t app_idx``
        Which application of the owner's job published this.  Taken from
        the publishing process's ``prte_proc_t.app_idx``.
        ``UINT32_MAX`` when the publisher cannot be resolved to a proc
        object — a relayed publish from another DVM, principally — which
        reads as "no application horizon applies".

    ``uint32_t session_id``
        The publisher's session, read from the ``PRTE_JOB_SESSION_ID``
        attribute of its job.  The attribute is set ``PRTE_ATTR_GLOBAL``
        where it is set at all, so it reaches the daemons in the launch
        message and a prted can answer this for its own clients.  Absent
        means the default session, whose end is the DVM's end and
        therefore needs no purge.

    ``time_t last_access``
        Publish time, restamped by every lookup that successfully
        returns one of the item's keys.  Drives both the retention
        timeout and eviction order.

    ``size_t nbytes``
        The item's accounted size (see `Size accounting`_).  Recomputed
        when a ``FIRST_READ`` lookup removes one of its keys, since the
        item then shrinks and its uid's total must follow.

    The class parent stays ``pmix_object_t``.  See `Why no LRU list`_.

``prte_ds_usage_t`` (new, ``ds.h``)
    One record per uid that has published to this store: ``uid`` and
    ``bytes``.  Held on a ``pmix_list_t`` in the store — the number of
    distinct uids publishing to one store is small, and a list keeps the
    lookup, the increment and the decrement in one obvious place.
    Created on first publish by that uid, released when its total
    reaches zero.

``prte_data_store_t`` (``ds.h``)
    Gains the usage list, the sweep timer (``prte_event_t`` plus a
    ``bool`` saying whether it is armed), and the two parameter values.

Size accounting
~~~~~~~~~~~~~~~

An item's size is the sum over its ``info`` list of the key length plus
the value's own size — ``strlen`` for ``PMIX_STRING``, the ``size``
field for ``PMIX_BYTE_OBJECT``, ``sizeof`` the union member otherwise —
plus a fixed per-item constant covering the object itself and its
accessor lists.  It is an accounting figure, not a malloc total: what
matters is that it is monotone in what the publisher stored, so a
publisher cannot evade the cap by choosing a type.

Why no LRU list
~~~~~~~~~~~~~~~

The obvious structure is a per-uid list in least-recently-used order,
which makes eviction O(1).  This design deliberately does not build one.

The store is a ``pmix_pointer_array_t`` and every object is reachable by
its ``index``, which several paths use.  Putting the same object on a
list as well means two memberships to keep in step on every removal
path — publish's duplicate drop, unpublish, each of five purge horizons,
the sweep, eviction, and finalize — and a missed one leaves either a
freed item on a list or a leaked item off the array.  That is a bug
class, not a line of code.

Eviction instead scans the array for the owning uid's oldest
``last_access``, removes it, and repeats until the new item fits.  The
scan is O(n) in the store's size, it runs only when a uid is at its cap,
and n is bounded by that cap divided by the size of the smallest useful
item.  If a store is ever measured spending real time there, the per-uid
list is the answer — but it should be a measurement, not an assumption.

Publish
-------

``ds_publish.c`` gains four things, in this order:

#. **Resolve the publisher.**  Look up the owner's job and proc objects
   to fill ``app_idx`` and ``session_id``.  Both degrade to "unknown"
   rather than failing the publish: a relayed publish has no local proc
   object, and the specification's answer for an item with no
   application is that no application horizon takes it.
#. **Take the identity from the right place.**  ``uid`` and ``gid``
   continue to come from the ``PMIX_USERID`` / ``PMIX_GRPID`` that PMIx
   appends — except for a relayed request, which must present the
   *originating* process's identity.  See `Relayed identity`_.
#. **Apply the cap.**  Compute ``nbytes``; find or create the uid's
   usage record; if ``usage->bytes + nbytes`` exceeds the cap, evict
   that uid's oldest items until it does not.  If ``nbytes`` alone
   exceeds the cap, refuse with ``PMIX_ERR_OUT_OF_RESOURCE`` and store
   nothing.
#. **Stamp ``last_access``** and add to the array, updating the usage
   record.

The cap check belongs **after** the duplicate scan and the directive
scan and **before** ``pmix_pointer_array_add()`` — the same window the
duplicate gate occupies, and for the same reason: everything it reads
(the owner, which ``PMIX_REQUESTOR`` may have replaced, and the uid) is
final only then, and a publish that is going to be refused must leave
the store exactly as it found it.  Eviction is the one thing here that
modifies the store, so it must not run until the publish is certain to
proceed.

``PRTE_PUBLISH_REPLACE`` widens from "an item this process published" to
"an item this uid published", which is the same predicate removal uses.
A colliding item belonging to another uid still refuses the publish,
directive or not.

Lookup
------

Two changes, both small, both easy to put in only one of the two places
that need them.

**Touch on success.**  An item's ``last_access`` is restamped whenever a
lookup returns one of its keys.  That happens in ``ds_lookup.c``, when
answering from the store, *and* in ``ds_publish.c``, when a publish
satisfies a parked request — the same pair of places that already both
implement ``FIRST_READ``.

**Shrink on ``FIRST_READ``.**  Both places already remove the returned
key and drop the object when its ``info`` list empties.  They now also
recompute ``nbytes`` and decrement the owning uid's usage.

Unpublish
---------

``ds_unpublish.c`` changes its ownership test.

Today it compares the requestor's namespace *and* rank against the
item's owner.  It becomes: the requestor's effective uid equals the
item's ``uid``, and — when neither is ``UINT32_MAX`` — the gid equals
the item's ``gid``.

The requestor's uid and gid are not currently unpacked at all: the
directive scan looks only for ``PMIX_REQUESTOR``.  PMIx appends
``PMIX_USERID`` and ``PMIX_GRPID`` to every unpublish, so the values are
present in the buffer and the scan simply has to read them into the
stack ``prte_data_req_t`` that already carries the requestor.

Nothing else about the path changes.  Range is still not consulted —
removal was never a range question — and access permissions are still
not consulted, which is now load-bearing in both directions: an accessor
list must not confer removal, and an owner excluded by its own accessor
list must still be able to remove what it cannot read.

The no-keys form is untouched.  ``PMIx_Unpublish(NULL, ...)`` never
reaches this file: ``pmix_server_unpublish_fn`` turns it into
``PRTE_PMIX_PURGE_PROC_CMD`` naming the caller, and that keeps its
present meaning.

Relayed identity
~~~~~~~~~~~~~~~~

``prte_ds_check_requestor()`` exists because a cross-DVM request arrives
under the relaying daemon's *tool* identity, and every ownership
question would otherwise be answered about the wrong process.  Once
removal is decided by uid, the same is true of the uid: today a relayed
publish is stored with the relaying daemon's uid and gid, because those
are what PMIx appended.

``ds_relay.c`` therefore packs two more directives beside
``PMIX_REQUESTOR``:

.. code-block:: c

   #define PRTE_PUBLISH_REQ_UID  "prte.pub.ruid"   /* PMIX_UINT32 */
   #define PRTE_PUBLISH_REQ_GID  "prte.pub.rgid"   /* PMIX_UINT32 */

They are PRRTE-private keys rather than a second ``PMIX_USERID``,
because PMIx appends its own and the reader must not have to reason
about which of two identically-keyed entries came from where.  They are
honored under exactly the rule ``check_requestor`` applies — **only from
a tool** — since an application process claiming another uid is claiming
another identity.

Purge and the horizons
----------------------

``prte_data_server_expires_by()`` becomes the table in the
specification:

.. code-block:: text

   horizon      removes
   INVALID      everything owned by the target  (explicit unpublish-all)
   PROC         PROC
   APP          PROC, APP
   NSPACE       PROC, APP, NSPACE
   SESSION      PROC, APP, NSPACE, SESSION

``FIRST_READ`` and ``INDEF`` are removed by no horizon.  ``FIRST_READ``
moving out of the "shortest lifetimes" case is the fix for
`issue #2733 <https://github.com/openpmix/prrte/issues/2733>`_.

The values remain a spelled-out ordering rather than a comparison:
``PMIX_PERSIST_INDEF`` is 0 and outlives all of them, and
``PMIX_PERSIST_NSPACE`` will be 5, above ``SESSION``'s 4.

What a purge names
~~~~~~~~~~~~~~~~~~

The command already carries a target ``pmix_proc_t`` and a directive
array.  Two directives are added, each meaningful for one horizon:

.. list-table::
   :header-rows: 1
   :widths: 18 30 52

   * - Horizon
     - Target
     - Additional directive
   * - ``PROC``
     - the process, by name
     - —
   * - ``APP``
     - the namespace, wildcard rank
     - ``prte.purge.appidx`` (``PMIX_UINT32``)
   * - ``NSPACE``
     - the namespace, wildcard rank
     - —
   * - ``SESSION``
     - wildcard namespace and rank
     - ``PMIX_SESSION_ID`` (``PMIX_UINT32``)

``ds_purge`` matches an item when the target admits its owner —
``PMIX_CHECK_PROCID``, which already treats a wildcard rank as "any rank
of this namespace" — *and* the horizon's extra condition holds
(``app_idx`` equal, or ``session_id`` equal), *and* ``expires_by()``
says the persistence goes.  A wildcard namespace with a session id is
the one case where the target admits everything and the session id does
all the work.

The packer and the unpacker change together, in the same commit.  There
is no version negotiation between daemons of one DVM and none is wanted;
see the project rule on mixed-version DVMs.

Who sends what
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - Event
     - Master's store
     - Daemon's own store
   * - a process terminates
     - ``PROC`` purge from ``prte_state_base_track_procs``
     - direct local purge when the daemon reaps the child
   * - an application terminates
     - ``APP`` purge, when the master's per-app termination count
       reaches that app's ``num_procs``
     - *deferred* — see `The one deferred piece`_
   * - a namespace terminates
     - ``NSPACE`` purge — today's job-end purge, with its horizon
       corrected from ``APP``
     - on ``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD``, which the master already
       xcasts to every daemon
   * - a session is torn down
     - ``SESSION`` purge from the session teardown path
     - on a new xcast alongside it — see `Session teardown`_

Per-app termination counting is the one piece of accounting that does
not exist yet.  ``prte_proc_t.app_idx`` already says which application a
process belongs to and ``prte_app_context_t`` already has ``num_procs``
and a ``state`` field with a ``PRTE_APP_STATE_COMPLETED`` value that
nothing currently sets.  The count is a per-app terminated tally
incremented beside the existing ``jdata->num_terminated++`` in
``prte_state_base_track_procs``; when it reaches the app's
``num_procs``, mark the app completed and send the ``APP`` purge.

A bug this uncovers
~~~~~~~~~~~~~~~~~~~

``job_teardown()`` in ``src/mca/state/prted/state_prted.c`` calls
``prte_state_base_notify_data_server()`` when a **prted's own local
procs** of a job have finished — the gate is ``jdata->num_terminated ==
jdata->num_local_procs``.  That function sends the purge to the *global*
store as well as the daemon's own.

So on a multi-node job, the first node to finish its share purges the
whole namespace's ``APP`` and ``PROC`` data out of the master's store
while the rest of the job is still running.  A process on another node
that published with the default persistence can have its data removed
before its application, or even its own process, has ended.

The fix belongs to this work because the horizons are what make it
visible: a daemon's completion is not a lifetime, and the only store it
is entitled to act on at that moment is its own.  The daemon-side call
becomes a purge of its own store only, and the master keeps sole
responsibility for the namespace horizon — which it already reaches
through ``state_dvm.c``.

Session teardown
~~~~~~~~~~~~~~~~

The master reaches session teardown in
``prte_ras_base_teardown_reservation()``.  Where the teardown returns
nodes to the scheduler it also terminates the daemons on them, taking
their stores with them, so nothing is left behind.  Where it does not —
a reservation released back into the default pool with its daemons still
running — those daemons may still hold local-range items published by
procs of that session.

The design therefore sends a session purge to the daemons as well.  If
the implementation finds that no such surviving-daemon case can arise,
the xcast can be dropped; that is a question to settle in code, not to
assume either way here.

The retention timeout
---------------------

One sweep event per store, not one timer per item.

The store arms a repeating ``prte_event_t`` while it holds at least one
item whose persistence is ``INDEF`` or ``FIRST_READ``, and disarms it
when the last one goes.  The sweep walks the array and removes every
such item whose ``last_access`` is more than
``prte_data_server_timeout`` seconds old, decrementing its uid's usage.

The interval is ``timeout / 4``, bounded below at one second and above
at sixty, which is what makes the specification's "no earlier than the
timeout, normally within one sweep interval after" true.  Per-item
timers would be exact, at the cost of an armed libevent timer per
published item and a re-arm on every read; the parked-lookup timeout in
``ds_lookup.c`` is per-request because a request is a one-shot with a
caller waiting on it, which is not this case.

A timeout of zero disables the feature: the timer is never armed and
nothing expires.

PMIx-side work
--------------

One constant, in ``include/pmix_common.h.in``:

.. code-block:: c

   #define PMIX_PERSIST_NSPACE   5   // retain until publishing nspace terminates

Appended rather than slotted between ``APP`` and ``SESSION`` because
these are wire values.  It needs the attribute documentation that goes
with it, and it should be proposed to the Standard, which has the same
gap.  No capability flag: PRRTE requires PMIx 7.0.0 or newer, so a
constant added during the 7.0 series is present in every PMIx that can
build PRRTE.

PRRTE cannot land the default-persistence change until this exists.

MCA parameters
--------------

Registered in ``prte_data_server_init()`` beside the existing
``prte_data_server_verbose``, which fixes their names by its own
convention — ``pmix_mca_base_var_register("prte", "prte", "data", ...)``:

.. code-block:: text

   prte_data_server_timeout    int    300        seconds; 0 disables
   prte_data_server_max_size   size   16777216   bytes per uid; 0 disables

Both are read by every daemon, and both are read once at init: a store
does not re-read them, so changing one mid-run is not a thing a user can
do.

Testing
-------

**Unit** (``test/unit/runtime/test_runtime.c``, which already covers the
range checks and the constructors).  ``expires_by()`` across the full
cross-product of persistence and horizon, including the two that no
horizon takes; the uid/gid removal predicate, including the gid-unknown
degradation and the accessor-list cases in both directions; size
accounting for each value type; eviction order over a synthetic store;
and the sweep's expiry predicate against a stamped ``last_access``.
None of these needs the RML.

**Offline / single node.**  ``FIRST_READ`` end-to-end — publish, read,
second read gets ``NOT_FOUND`` — which the existing suite does not
cover.

**Multi-node** (``contrib/dockerswarm``, the ``test_runtime`` phase).
The two-generation handover from #2733: a publisher exits, a later job
of the same DVM reads what it left.  A ``PERSIST_APP`` item gone at job
end while a ``SESSION`` one is not, which needs two jobs under one
persistent DVM.  The multi-node ``job_teardown`` bug: a job spanning
nodes where one node's procs exit well before another's, asserting that
the early finisher does not take the late one's data.  Expiry, with the
timeout set to a few seconds by MCA parameter — no debug-only knob, and
no reliance on a ``PRTE_ENABLE_DEBUG`` build.

**What the harness cannot show.**  Per-uid cap scoping needs two real
uids and the swarm runs as one, so eviction's uid scoping is unit-tested
and the multi-node case covers only that a single uid's flood evicts its
own oldest.  Say so in the test's comment rather than letting a passing
case imply more than it proves.

The one deferred piece
----------------------

The master does not tell the daemons when an **application** has
terminated, so a daemon cannot honor ``APP`` for a local-range item.
Until that notification exists, a daemon's store treats a local ``APP``
item as ``NSPACE`` — which is what it does today, is never shorter than
the publisher asked for, and affects only local-range publishes carrying
an explicit ``APP`` persistence, since the default is now ``NSPACE``.

This is deliberate and is recorded in ``docs/todo.rst`` rather than left
as a comment in the code.

Traps
-----

* **``PMIX_NEW`` does not zero.**  Every new field needs a line in
  ``construct()`` in ``ds_main.c``.  This file has been bitten before —
  ``proxy`` and ``owner`` were matching requestors against uninitialized
  memory.
* **The answer-buffer contract.**  Returning ``PMIX_SUCCESS`` means the
  handler disposed of ``answer``; anything else means it did not.  The
  new refusal path (``PMIX_ERR_OUT_OF_RESOURCE``) must return the error
  and leave the buffer alone.
* **Every exit answers somebody.**  A caller is parked on a room number;
  a silent return is a hang, not an error.
* **Decide before you remove.**  Eviction modifies the store, so it must
  run only once the publish is certain to proceed — after the duplicate
  scan, not beside it.
* **Two removal paths per item.**  Every place that removes an object
  now also has to decrement its uid's usage.  There are eight of them;
  a helper that does both is the way to avoid missing one.
* **No locking, and none is wanted.**  Everything runs inside the RML
  receive on the progress thread, including the sweep.
* **The wire changes on both sides in one commit.**  A field of the same
  width silently yields wrong values; a different width desynchronizes
  the rest of the buffer.
