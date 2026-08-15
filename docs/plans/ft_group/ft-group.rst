Degraded-mode survival of a group construct on daemon loss
==========================================================

Tracking issue: `#2510 <https://github.com/openpmix/prrte/issues/2510>`_.

Context
-------

A PMIx group collective can lose a participant two ways, and PRRTE handled
them asymmetrically.

**Client death** is handled inside the PMIx server library.
``grp_ft_collective()`` gates it: when ``PMIX_GROUP_FT_COLLECTIVE`` was
requested the construct completes on the survivors and each is told who was
lost via ``PMIX_GROUP_MEMBER_FAILED``; otherwise ``abort_construct()`` fires
``PMIX_GROUP_CONSTRUCT_ABORT``.

**Daemon death** — a whole ``prted`` and its entire subtree of clients — is
handled by ``prte_grpcomm_direct_group_fault_handler``, which aborted every
affected operation *regardless* of the flag, because the flag was neither
parsed nor stored at this layer. ``PMIX_GROUP_FT_COLLECTIVE`` appeared exactly
once in the whole tree: in a comment explaining that it was not honored.
Closing that asymmetry is the issue.

The hard part is not the policy — it is re-converging an up-tree rollup over a
routing tree that just changed shape. ``coll->nexpected`` was computed once, at
tracker creation, and is invalidated by the fault.

Reading the code turned up four further problems the fix depends on:

* **An interior relay death silently hung a construct.** A tracker whose
  participants all survive but whose relaying child died was not "affected" by
  the handler's test, which only inspected ``coll->dmns``, so it was never
  aborted — and its stale ``nexpected`` never completed.
* ``nreported`` **was an identity-free counter**, safe only while each child
  contributes exactly once. Recovery makes duplicates normal, and two messages
  from one child plus zero from another satisfy ``nreported == nexpected`` just
  as well as one from each.
* **Nothing bounded a group operation in time.** ``coll->timeout`` was parsed,
  max-merged and forwarded to the parent — and never armed. The unconditional
  abort was the de-facto liveness guarantee.
* ``create_dmns`` **failure was a silent hang path.** Recovery must not depend
  on resolving a process whose node is being torn down.

Corrections to earlier premises
-------------------------------

Three assumptions that an earlier draft of this plan got wrong, recorded
because each one changes the design:

#. **A global notice fires the handler twice on each daemon.**
   ``prte_rml_repair_routing_tree(.., global=true)`` recurses into the LOCAL
   pass and then falls through to the GLOBAL pass. The GLOBAL pass carries the
   *full* rank list and always reports no topology change, because the topology
   work is skipped — so the tree is already repaired and
   ``prte_rml_get_num_contributors()`` is immediately usable there.

#. **The xcast ordering argument must not be stated as "process before
   forward".** ``PRTE_RML_TAG_DAEMON_DIED`` *is* in xcast's ``process_first``
   set, but ``process_msg()`` only *queues* the local delivery, and
   ``forward_op()`` then reads the children synchronously. The ordering this
   design needs still holds, for a different reason: a daemon makes its own
   delivery event active before it can read any reply from a child, and
   libevent dispatches active events before the next I/O poll. The design
   handles a newer stamp anyway rather than assuming it away.

#. ``prte_grpcomm.xcast()`` **copies its buffer.** Three callers dropped the
   pointer afterwards and leaked the entire release payload on every group and
   fence operation.

Desired behavior
----------------

Evaluated at the controller, per in-flight operation:

.. list-table::
   :header-rows: 1
   :widths: 14 24 16 46

   * - Operation
     - Participating daemon lost?
     - ``FT_COLLECTIVE``
     - Result
   * - construct
     - no (only relays changed)
     - —
     - **completes normally** (fixes the hang above)
   * - construct
     - yes
     - set
     - **completes on survivors**, reduced membership, one
       ``PMIX_GROUP_MEMBER_FAILED`` per departed process
   * - construct
     - yes
     - unset
     - aborts ``PMIX_GROUP_CONSTRUCT_ABORT`` (prior behavior)
   * - destruct
     - yes
     - —
     - completes ``PMIX_SUCCESS`` (prior behavior)
   * - any
     - surviving membership empty
     - —
     - aborts ``PMIX_GROUP_CONSTRUCT_ABORT``
   * - bootstrap
     - any
     - —
     - aborts (see `Out of scope`_)

Design: one global epoch, not a per-link round
----------------------------------------------

The instinct is to copy ``prte_grpcomm_direct_xcast_fault_handler`` — ack
rounds chosen by the parent and echoed by the child. **That model does not
transfer.** In xcast the payload flows *down*, so the parent always holds the
operation and can re-poll. In a group rollup the payload flows *up*, so the
parent is exactly the party that may hold nothing: a pass-through daemon's
tracker is created lazily, on the first arriving contribution. A parent-driven
re-poll cannot reach a promoted orphan whose new parent has no tracker, and a
round cannot invalidate a contribution the parent has *already absorbed* — that
invalidation has to travel up, and rounds travel down.

Instead, use the synchronization the RML already provides: the **GLOBAL fault
scope**. It is DVM-wide, ordered identically on every daemon, and arrives after
the tree is repaired. That gives a simultaneous restart for free — no new RML
tag, no re-poll protocol, no promoted-orphan special case, and the topology
deltas are not needed at all.

**The mechanism.** One ``uint32_t group_epoch`` per daemon, advanced in the
GLOBAL-scope group fault handler on **every** daemon. On advance, every
in-flight non-bootstrap tracker:

#. resets — clears ``reported_slots``/``self_reported``/``converged``, releases
   and reopens ``grpinfo``/``endpts``, resets the sticky ``status``;
#. recomputes ``nexpected`` from the repaired tree. ``coll->dmns`` stays the
   **full pre-fault set**: ``prte_rml_get_num_contributors()`` skips
   ``prte_rml_base.failed_dmns`` internally, so the count is automatically
   fault-aware while ``dmns`` remains the record of who was *supposed* to take
   part;
#. re-injects its own saved contribution to itself, stamped with the new epoch.
   A daemon with nothing of its own — a pure relay — simply calls
   ``check_complete()``, which fires immediately if ``nexpected`` is now zero.
   That is the interior-relay fix.

::

    HNP xcast DAEMON_DIED
             |
             v
    each daemon: repair_routing_tree(global)
             |
             +--> LOCAL pass: tree repaired, xcast handler re-forwards
             |
             v
    GLOBAL pass: group fault handler, on EVERY daemon
             |
             +-- master && operation cannot survive?
             |     (bootstrap, or construct + lost participant + no FT)
             |        -> abort_group_op(); mark aborting; skip the reset
             |
             v
    advance_group_epoch(E+1)
             |
             +--> reset tracker, recompute nexpected
             +--> re-inject my_contribution to self @ epoch E+1
                       |
                       v
             grp_recv: per-slot dedup, drop stale epochs
                       |
              +--------+---------+
              |                  |
          non-HNP              HNP
        roll up to          FT gate: departed set,
        parent @ E+1        reduced membership
              |                  |
              +------------------+
                       |
                       v
             xcast GROUP_RELEASE + departed list
                       |
                       v
             grp_release: complete clients,
             then GROUP_MEMBER_FAILED events

**Epoch stamping.** The epoch is *not* part of the signature. ``group()``
builds the contribution body, saves ``coll->my_contribution`` as a copy of it,
then sends ``[epoch][body]``. The rollup to the parent does the same. Replay is
``[new epoch][my_contribution]``. ``grp_recv`` unpacks the epoch first:

* ``stamp < group_epoch`` — stale; drop before any merge.
* ``stamp > group_epoch`` — we are behind. Call ``advance_group_epoch(stamp)``,
  which is exactly what the fault handler would have done a moment later and is
  idempotent with it, then process normally. No hold queue, no dropped
  contribution. ``PRTE_ERROR_LOG`` it so the ordering assumption stays
  testable.

**Per-slot accounting** replaces the bare counter.
``prte_rml_get_subtree_index()`` returns an index computed by the same radix
math ``get_num_contributors`` uses, so the two agree by construction.
``grp_recv`` already receives ``sender``. A contribution whose slot bit is
already set is dropped **whole**, before the info-list accumulation, which
appends with no key de-duplication. Completion becomes "every expected slot
present", idempotent under replay. This applies to the **non-bootstrap** branch
only — bootstrap contributions go straight to the controller from arbitrary
daemons, so a subtree index is meaningless for them.

**The FT policy lives at controller completion, not in the fault handler.**
That is the one point with full information, it runs exactly once, and it
covers the case where the controller had no tracker when the fault landed. The
fault handler's job is mechanical re-convergence plus one optimization:
aborting immediately what provably cannot survive.

Commits
-------

One branch, one PR against ``master``.

Drive-by fixes (1–5)
~~~~~~~~~~~~~~~~~~~~

Each stands alone and is independently revertable.

#. The wildcard-preservation writes in ``get_tracker`` index the *incoming*
   loop variable into the *tracker's* array. Out-of-bounds write whenever the
   arrays differ in length or order.
#. ``get_tracker``'s creation path never copies ``bootstrap``, ``follower`` or
   ``final_order`` into the accumulated signature. A single-leader bootstrap
   therefore has ``nleaders == 0`` and can never complete; the release describes
   the operation wrongly; and a final order supplied by whichever daemon happens
   to create the tracker is silently discarded.
#. The signature destructor never freed ``final_order``.
#. Three release buffers leak after ``xcast``, which copies.
#. ``find_delete_tracker`` matched on ``groupID`` only while ``get_tracker``
   matches ``groupID`` **and** operation.

Rollup accounting (6)
~~~~~~~~~~~~~~~~~~~~~

No behavior change; every part is correct on its own merits. Per-slot reporting
bitmap; a ``converged`` latch so a straggler cannot drive a second release
broadcast or a second rollup; a bounded memo of released operations so a
straggler cannot recreate a tracker nothing will delete; ``check_complete()``
factored out so a fault handler can re-test a tracker with no message in hand;
and ``coll->timeout`` finally armed on the controller — only when a participant
actually asked for one, so a tree with no timeout directive behaves as before.
Three dead tracker fields go with the struct surgery.

The FT feature (7)
~~~~~~~~~~~~~~~~~~

``ft_collective`` on the signature, parsed under ``#if
PRTE_PMIX_HAVE_GROUP_FT`` but packed and unpacked **unguarded** so the wire
format is uniform regardless of the capability. Sticky-OR on merge, which means
"any **surviving** participant asked for it" — a participant that requested it
and died before rolling up is not visible, and that deviation is documented at
the code. It is also a deliberate superset of the PMIx server's first-match-wins
rule.

Then: the saved contribution, the epoch machinery, the fault-handler rewrite,
``create_dmns`` tolerance for a torn-down node, the controller's completion
gate, the departed list carried in the release, and the
``PMIX_GROUP_MEMBER_FAILED`` events. The events use ``PMIX_RANGE_CUSTOM`` over
the group's local members rather than ``PMIX_RANGE_LOCAL``, which would reach
every client on the node including unrelated jobs, and they carry the
mandatory ``prte.notify.donotloop`` marker — without it PMIx hands the event
back to our own upcall, which thread-shifts onto the progress thread we are
blocking on, and the daemon deadlocks.

Tests and docs (8–9)
~~~~~~~~~~~~~~~~~~~~

``test/unit/grpcomm`` gains the new constructor defaults and drives
``prte_grpcomm_direct_group_member_departed()`` against a synthetic failed-daemon
set — the only genuinely unit-testable part, and where policy bugs will live.

``contrib/dockerswarm`` is where the feature is actually proven. ``groupcon``
gains ``--ft``, ``--delay`` and a ``PMIX_GROUP_MEMBER_FAILED`` handler, keeping
its positional interface so existing cases are unaffected. Four new cases, all
gated on ``pmix_cap PMIX_CAP_GROUP_FT``: an FT construct surviving the loss of a
participating daemon; the same loss without ``--ft`` aborting cleanly; the loss
of a relay-only daemon not stalling anything; and further constructs still
working afterwards. ``--delay`` is load-bearing — without the stagger the
construct is over before the kill lands and every assertion passes without
testing anything.

Out of scope
------------

* **Bootstrap operations.** They have no resolved daemon set, and ``nleaders``
  is a count with no identities, so the controller cannot work out how much of
  one died. Keep the unconditional abort; recording leader identities is the
  enabling change for a follow-up.
* **Revival/unheal during an in-flight operation.** Aborted.
  ``PRTE_RML_TAG_DAEMON_REVIVED`` is deliberately forward-first in the xcast, so
  it cannot give the parent-before-child ordering an epoch advance needs. This
  path previously did nothing at all, i.e. hung, so aborting is an improvement
  rather than a fix.
* ``prte_grpcomm_direct_fence_fault_handler`` **has no scope guard at all**, so
  it activates ``PRTE_JOB_STATE_COMM_FAILED`` twice per fault *and once on every
  revival* — killing a job on a legitimate unheal. A real bug, but not this
  one; report it separately rather than widening this work.
* The inherent race where the controller completes a construct microseconds
  before learning a member's daemon died. That member is an ordinary
  post-construct member failure, not a departed entry.

Verification
------------

.. code-block:: sh

   mkdir -p build/debug && cd build/debug
   ../../configure --enable-debug --with-pmix=<pmix> --with-hwloc=<hwloc> \
       --with-libevent=<libevent>
   make -j$(nproc)      # --enable-debug implies warnings-as-errors
   make check

``grep PMIX_CAP_GROUP_FT <pmix>/include/pmix_version.h`` before trusting any
result — without it the feature compiles out and a green build proves nothing.
Build both with and without a capable PMIx if time allows, or the ``#else`` arm
of the completion gate is never compiled.

Then a single-host smoke test to prove the ordinary path is untouched
(``prte --daemonize`` → ``prun -n 4 hostname`` → ``pterm``), and the harness,
which is the only layer that proves the feature — a daemon that merely
*receives* the release does not exist on one host:

.. code-block:: sh

   cd contrib/dockerswarm
   ./build.sh
   docker compose up -d
   ./run-tests.sh linux

``make -C test/offline check-offline`` is not needed; nothing here touches the
mapper.
