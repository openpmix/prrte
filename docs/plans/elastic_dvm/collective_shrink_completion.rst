.. _dvm-collective-shrink-completion-label:

Collective Shrink Completion — Repair the Routing Tree Once per Campaign
=======================================================================

This document plans the **optimization** tracked in
`openpmix/prrte#2492 <https://github.com/openpmix/prrte/issues/2492>`_: draining
a shrink campaign as a single collective event rather than one daemon at a time.
It is a *revision* of the shrink path described in
:ref:`dvm-shrink-campaign-label`; that document remains authoritative for the
fence mechanism, the held-job arrays, the second hold point at ``LAUNCH_APPS``,
and the campaign type.  Only the pieces this revision changes are restated here.

This is an **optimization, not a correctness fix**.  The per-daemon completion
path shipped in :ref:`dvm-shrink-campaign-label` is correct; it was deliberately
kept simple and the collective scheme was deferred.  Nothing here changes the
externally observable contract in :ref:`elastic-dvm-spec-label` — the same
``PMIX_DVM_IS_READY`` / ``PMIX_ERR_DVM_MOD`` events fire for the same requests;
only *when and how many times* the internal routing-tree repair runs changes.

The state machine is single-threaded on the progress thread, so no locking is
required anywhere in this plan.

Background — the cost being removed
-----------------------------------

The shipped design drains a campaign **one daemon at a time**.  Every targeted
daemon exits on its own in response to ``PRTE_DAEMON_SHRINK_CMD``
(``src/prted/prted_comm.c``, the ``PRTE_DAEMON_SHRINK_CMD`` case), and the HNP
discovers each departure independently through the errmgr comm-failure path
(``src/mca/errmgr/dvm/errmgr_dvm.c``, ``proc_errors()``), decrementing the
campaign's ``pending`` count and the shared fence once per death.

Each of those independent departures drives a **separate** routing-tree repair.
A daemon that exits triggers ``prte_rml_route_lost()``
(``src/rml/routed_radix.c``), which calls
``prte_rml_repair_routing_tree(&failed_ranks, /*global=*/false)`` with a
**single** rank; that in turn runs ``handle_promotion()`` and
``update_descendants()`` for that one rank.  Shrinking ``m`` daemons that sit
along one branch of the radix routing tree therefore triggers up to ``m``
sequential promotions/descendant rewrites.  Review of PR #2472 flagged this as
potentially expensive for a large single-branch shrink (unprofiled).

Crucially, ``prte_rml_repair_routing_tree()`` **already accepts a rank array**
(``pmix_data_array_t *failed_ranks``) and performs a single promotion/descendant
pass for the whole set.  The optimization is to feed it the whole campaign at
once instead of one rank at a time.

Design overview
---------------

Repair the tree **once per campaign**:

#. The HNP broadcasts ``PRTE_DAEMON_SHRINK_CMD`` via the reliable xcast, exactly
   as today.  The broadcast payload already carries the full target-rank list,
   so **every** daemon learns the complete set of departing ranks from the
   broadcast itself — no separate failure notice is needed to inform survivors.
#. Each targeted daemon records that it is leaving and does its local
   processing, but **does not exit yet** (today it self-exits).
#. The HNP hooks the **broadcast's completion**.  The reliable xcast in
   ``src/mca/grpcomm/direct/grpcomm_direct_xcast.c`` already tracks completion
   via ACKs flowing up the tree: when ``finish_op()`` runs on the master, every
   daemon in the DVM has received the op.  A per-op completion callback is added
   (or the shrink op special-cased) to fire a handler at that point.
#. That handler reports **all** of the campaign's targets as failed in a single
   batch via ``prte_rml_repair_routing_tree(failed_ranks, /*global=*/false)`` —
   one promotion/descendant pass for the whole set — and performs the HNP-side
   teardown bookkeeping (``num_daemons``, node state, fence) for the batch.
#. Each doomed daemon exits once its lifeline disconnects as a consequence of
   the rewire, rather than self-exiting — but only because processing the shrink
   command put it into a **new leaving mode** that converts lifeline loss into
   termination (normally lifeline loss triggers *recovery*, not exit).  Because
   that mode rides in the broadcast itself and reaches each doomed daemon through
   its own lifeline, there is no race between learning "you are leaving" and the
   lifeline failing (see Step 3's design decision).  The completion event
   (``PMIX_DVM_IS_READY``) fires from this single batch point rather than from
   the last individual departure.

Why this is **not** the rejected per-daemon ACK
-----------------------------------------------

:ref:`dvm-shrink-campaign-label` rejected an earlier design in which each daemon
sent a ``PRTE_PLM_SHRINK_ACK_CMD`` announcing its *intent* to leave, and the HNP
decremented the campaign on receipt of that ACK.  That was wrong because the ACK
arrived while the daemon was still a live participant — acting on it could
release held jobs into a DVM that still believed the departing daemon present —
and because two decrement paths (ACK plus errmgr fallback) double-counted.

The collective scheme is **not** that design.  The authoritative HNP-side
teardown — route removal, ``num_daemons``, node state, fence — still happens at
the batch-repair callback, and held jobs are released only *after* it.  The
signal is not a daemon announcing intent; it is the xcast-completion fact that
every daemon has received the shrink order, at which point the HNP itself
performs the teardown.  The invariant "act once teardown has occurred, not on
intent" is preserved.  Because completion collapses to a single event per
campaign, the per-rank ``PMIX_RANK_INVALID`` idempotency stamping and the
double-count analysis that the per-death path required are retired: there is now
exactly one teardown event per campaign, so there is nothing to make idempotent.

Required revisions
------------------

Step 1 — Add an xcast completion callback (grpcomm/direct)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Today ``prte_grpcomm.xcast(tag, msg)`` is fire-and-forget
(``src/mca/grpcomm/grpcomm.h``, ``prte_grpcomm_base_module_xcast_fn_t``).  The
reliable xcast already *knows* when the whole DVM has received an op — on the
master, ``finish_op()`` in ``grpcomm_direct_xcast.c`` runs when the last child
ACK arrives, and ``op->sig.op_id`` is complete for the entire subtree, which for
the master is the entire DVM.

Add a mechanism to run a caller-supplied callback at that point.  Two options,
in preference order:

* **Per-op completion callback (preferred).**  Extend the xcast entry so the
  caller may pass a completion function and an opaque ``cbdata``, cache them on
  the ``op_t``, and invoke them from ``finish_op()`` **only on the master**
  (``PRTE_PROC_IS_MASTER``) — the point at which whole-DVM receipt is known.
  This is a general facility, useful beyond shrink.
* **Special-case the shrink op.**  If a full callback API is deemed too broad
  for this change, have ``finish_op()`` on the master recognize the shrink op
  and call the shrink-completion handler directly.  Cheaper to write, less
  reusable; a follow-up would still likely generalize it.

Whichever is chosen, the callback fires on the progress thread inside
``finish_op()``, so it may touch state-machine globals directly.

.. note::

   ``finish_op()`` also runs on non-master daemons (it ACKs to the parent).  The
   completion callback must fire **only** where ``PRTE_PROC_IS_MASTER`` is true,
   because only there does op completion mean *every* daemon received the op.  A
   non-master's ``finish_op()`` means only its own subtree completed.

Step 2 — HNP shrink-completion handler (new)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register the Step-1 callback when the shrink xcast is issued in
``src/mca/ras/base/ras_base_allocate.c`` (the ``PMIX_ALLOC_RELEASE`` branch of
``prte_ras_base_complete_request()`` and the reservation-teardown xcast in
``prte_ras_base_teardown_reservation()`` — both send ``PRTE_DAEMON_SHRINK_CMD``).
Carry the ``prte_shrink_campaign_t *`` as the callback ``cbdata`` so the handler
has the target list and requester in hand.

The handler, running once per campaign on the master, must do everything the
per-death errmgr path currently does across ``m`` invocations — but for the
whole batch, and exactly once:

#. **Batch routing-tree repair.**  Build a ``pmix_data_array_t`` from
   ``camp->targets`` and call
   ``prte_rml_repair_routing_tree(&failed, /*global=*/false)`` **once**.  This is
   the single promotion/descendant pass that replaces the per-daemon repairs.
#. **Per-target HNP bookkeeping.**  For each target rank, apply the same
   teardown the comm-failure block applies today (``errmgr_dvm.c`` lines
   269-274): unset ``PRTE_PROC_FLAG_ALIVE``, set the proc state, and decrement
   ``prte_process_info.num_daemons``.  This bookkeeping currently *rides on the
   comm-failure event*; when the loss is declared proactively it must be done
   here instead.  **This is the highest-risk part of the change** — see
   *Validation* below.
#. **Fence and completion.**  Decrement ``prte_dvm_launch_fence`` by
   ``camp->pending`` (all at once), invoke
   ``prte_ras_base_shrink_complete(camp)`` to give the RAS modules their release
   hook, emit ``PMIX_DVM_IS_READY`` to the requester via
   ``prte_plm_base_dvm_mod_notify()`` when ``camp->have_requester``, remove the
   campaign from ``prte_shrink_campaigns``, and call
   ``prte_plm_base_fence_release()`` when ``prte_dvm_launch_fence`` reaches zero.
   These are the same calls the errmgr path makes; they simply move here and run
   once for the batch.

Step 3 — Daemon side: record-and-wait instead of self-exit
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In ``src/prted/prted_comm.c``, the ``PRTE_DAEMON_SHRINK_CMD`` case currently, for
a daemon that finds its own rank in the target list, fires the
``PMIX_EVENT_JOB_END`` notification and immediately activates
``PRTE_JOB_STATE_DAEMONS_TERMINATED`` (self-exit).  Revise so that:

* **Every** daemon (target or survivor) uses the target list carried in the
  broadcast to repair its *own* routing tree locally —
  ``prte_rml_repair_routing_tree(targets, /*global=*/false)`` — so survivors
  drop the departing ranks from their children/parent sets.  Because the list
  travels in the broadcast, no global failure propagation is needed; this is why
  Step 2 uses ``global=false``.
* A daemon that finds **its own** rank among the targets records that it is
  leaving by entering **leaving mode** — a flag set as it *processes the
  ``PRTE_DAEMON_SHRINK_CMD`` itself*, not in response to any separate order (see
  the design decision below) — fires its ``JOB_END`` notification, and then
  **waits for its lifeline to disconnect** rather than self-exiting.  Entering
  this mode is what changes the daemon's response to lifeline loss from
  *recover* to *terminate*; see the warning below.

.. warning::

   **This code path does not exist today and must be created as part of this
   effort.**  A daemon that loses its lifeline (its parent) normally attempts
   **recovery** — it promotes and reconnects to an ancestor
   (``prte_rml_route_lost()`` and the promotion path in ``routed_radix.c``); it
   does **not** exit.  The collective scheme needs a doomed daemon to
   *terminate* on lifeline loss instead, and that new behavior must be gated
   strictly on the leaving mode set above: **termination on lifeline failure may
   occur only when this daemon has processed the shrink command naming its own
   rank.**  A daemon that loses its lifeline for any other reason (a genuine
   fault) must still take the normal recover/promote path — never this exit.
   The complementary half — actually *dropping* the doomed daemon's lifeline —
   comes from the survivor-side rewire (driven off the same broadcast) closing
   the connection to each departing child; for a single-branch shrink this
   cascades from the top of the branch down.  Prove both halves on the testbed
   before removing the daemon self-exit; a bounded self-exit fallback is the safe
   interim so a daemon that never sees its lifeline drop still terminates.

Design Decision — Leaving mode rides in the shrink command (race-free by construction)
""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""

A tempting but wrong shape is to make "enter leaving mode" a *second*, separate
order the HNP sends after the shrink command.  That opens a race: a doomed
daemon's lifeline could fail (because a survivor already rewired and dropped it)
**before** the separate leaving-mode order arrived, and the daemon — not yet in
leaving mode — would take the recovery path and try to promote instead of
exiting.  The fix is to **not** have a second order at all: leaving mode is set
by the daemon as it processes ``PRTE_DAEMON_SHRINK_CMD``, so the "you are
leaving" fact travels in the *same* broadcast that will ultimately tear the
lifeline down.

This is race-free by construction because the shrink broadcast propagates **down
the routing tree, which is the very set of lifelines**:

* A doomed daemon receives ``PRTE_DAEMON_SHRINK_CMD`` *through its own lifeline*
  (its parent forwards the xcast to it over exactly the connection whose loss
  will later trigger termination).
* The reliable xcast **forwards to children before processing locally** for
  ``PRTE_RML_TAG_DAEMON`` — the tag the shrink command uses.  In
  ``grpcomm_direct_xcast.c`` (``prte_grpcomm_direct_xcast_recv()``), only
  ``PRTE_RML_TAG_WIREUP`` and ``PRTE_RML_TAG_DAEMON_DIED`` are in the
  ``process_first`` set; every other tag runs ``forward_op()`` then
  ``process_msg()``.  So a parent hands the command to each child **before** it
  runs its own handler and rewires/drops that child.
* TCP in-order delivery on the lifeline then guarantees the doomed daemon reads
  the command (and enters leaving mode) *before* it can observe that same
  connection failing.  For a whole branch the cascade is inductive: each doomed
  daemon forwards the command down to its own children before its later death
  drops their lifelines, so every daemon on the branch is in leaving mode before
  its lifeline dies.

Two consequences for the implementation:

#. The shrink command **must stay on** ``PRTE_RML_TAG_DAEMON`` (forward-first).
   It must *not* be moved into the ``process_first`` set, or a parent could drop
   a child before forwarding the command to it — reopening the race.
#. The only remaining way a doomed daemon can see its lifeline fail before
   entering leaving mode is a genuine, independent fault that races the
   broadcast.  That is precisely the case the leaving-mode gate handles
   correctly: not yet in leaving mode ⇒ recover/promote (correct — it really is
   a fault), and the reliable xcast re-propagates the command through the
   repaired tree, so the daemon still enters leaving mode and exits once the
   command reaches it.

Step 4 — Remove the per-death completion logic from the errmgr
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Once completion is driven by Step 2, the shrink-campaign block in
``proc_errors()`` (``errmgr_dvm.c`` lines 286-323 — the ``PMIX_LIST_FOREACH_SAFE``
over ``prte_shrink_campaigns`` with the ``PMIX_RANK_INVALID`` stamping, the
per-death ``pending``/fence decrement, ``prte_ras_base_shrink_complete()``, and
``prte_plm_base_dvm_mod_notify()``) is removed.  The comm-failure event for a
doomed daemon must then be **harmless**: because the daemon was already stamped
gone and removed from routing in Step 2, a later comm-failure for the same rank
should fall through the general daemon-loss handling without re-aborting the DVM.

.. warning::

   This reverses the current *ordering of cause and effect*.  Today the daemon
   dies first, and the comm-failure event does the teardown.  In the collective
   scheme the HNP does the teardown first, and the daemon dies afterward.  The
   general daemon-loss handling below the removed block (``errmgr_dvm.c`` from
   line 324 on) must tolerate a comm-failure for a rank the HNP has *already*
   torn down — otherwise the doomed daemons' eventual deaths will trip DVM abort
   logic.  Auditing that fall-through is mandatory, not optional.

Interaction with the grow rollback path
----------------------------------------

The comm-failure block already special-cases grow targets via
``prte_plm_base_grow_target_failed()`` (``errmgr_dvm.c`` line 283), which returns
early.  That check must remain **ahead of** any shrink handling and is
unaffected: a rank cannot be simultaneously a grow target and a shrink target,
and the grow path still relies on the real comm-failure event.  Only the shrink
branch moves to the collective callback.

Design invariants preserved
----------------------------

* **Teardown before release.**  Held jobs are released only after the HNP-side
  teardown (routes, ``num_daemons``, node state, fence) has run — now in the
  Step-2 batch handler rather than the per-death path.
* **Per-campaign completion event.**  ``PMIX_DVM_IS_READY`` still fires once per
  request, when *this* campaign drains; other concurrent campaigns keeping the
  shared fence nonzero do not delay it.  Held-job release still waits for the
  global fence to reach zero.
* **Clean exit and crash are indistinguishable.**  A daemon that crashes mid
  shrink is still handled: its rank is in the batch, so it is torn down by
  Step 2 regardless of whether it would have exited cleanly.
* **Failure path unchanged.**  ``PMIX_ERR_DVM_MOD`` is still emitted only on the
  xcast-failure cleanup in :ref:`dvm-shrink-campaign-label` Step 1; once the
  shrink command is on the wire, every departure is a success for the campaign.

Implementation status
---------------------

This plan has been implemented and validated on the ten-node Docker testbed.
The change spans ``grpcomm/grpcomm.h``, ``grpcomm/direct/`` (the ``xcast_nb``
entry point plus the completion FIFO), ``ras/base/ras_base_allocate.c`` (the
collective completion handler), ``prted/prted_comm.c`` and ``rml/routed_radix.c``
(the daemon leaving mode), ``errmgr/dvm/errmgr_dvm.c`` (the already-departed
guard), and ``runtime/prte_globals.{h,c}``.  It builds warning-free under
``--enable-devel-check`` (``-Werror`` plus the full picky set).

Two implementation choices settled the open questions the plan had flagged:

* The completion hook is the **general** ``xcast_nb`` facility (open question #2's
  preferred option), not a shrink special case.
* The daemon departs on a **bounded timer** with a lifeline-loss fast path (the
  plan's endorsed fallback); the "survivor actively closes the connection" half
  of open question #1 was *not* built — the timer covers it.

One scope reduction is worth recording: **survivors are not batched.**  A survivor
that loses several targets still repairs once per departure, exactly as before.
Batching the survivors would require them to repair from the broadcast target
list *mid-broadcast*, which races the reliable xcast's own ACK bookkeeping on the
same tree — the interaction the plan flags as needing validation.  The HNP-side
repair — the cost issue #2492 actually names — is collapsed to one pass; the
survivor-side batching is left as a follow-up.

Validation results
------------------

The collective / whole-branch failure path is far less exercised than individual
daemon deaths, so it was exercised on the in-repo Docker testbed
(``contrib/dockerswarm/``), sized to **ten** nodes and driven with
``--prtemca rml_radix 2`` to force a real multi-level tree
(``0 → 1,2   1 → 3,4   2 → 5,6   3 → 7,8``) rather than the default flat fan-out.

#. **Single-branch multi-daemon shrink** (subtree ``{3,7,8}``): completed with a
   single ``PMIX_DVM_IS_READY``; the HNP survived and ``prun`` still worked.  With
   ``routed_base_verbose`` on the flat tree the collapse is visible directly — a
   **single** repair pass takes children ``4,5,6 → INVALID`` in one shot, and
   every one of the departing daemons' comm-failures is absorbed by the
   already-departed guard (``errmgr_base_verbose`` shows the "ignoring it" line),
   driving **zero** per-death repairs.
#. **Multi-branch shrink** (ranks ``4`` and ``6``, one leaf under each of the
   HNP's two children): one completion event, correct survivors, ``prun`` works.
#. **Crash during shrink** (``pkill -9`` a target's ``prted`` inside the
   departure window): the campaign still drained to a single completion event and
   the HNP survived — confirming clean exit and crash are indistinguishable.
#. **Fence under load** (forty rapid ``prun`` launches spanning a shrink): all
   forty succeeded and the DVM stayed healthy, so the fence raised during a shrink
   does not wedge concurrent traffic.

The **in-flight-job remap-onto-survivors** path (a job held at the ``LAUNCH_APPS``
hold point during a shrink, then remapped) could **not** be exercised in this
harness: a plain ``prun`` maps only onto the head node's base pool, not the
reservation the grown/shrunk nodes belong to, so a normal job is never held for a
reservation-node shrink; and the ``elastic`` tool cannot connect while concurrent
``prun`` sessions litter ``$TMPDIR`` with rendezvous files (it fails
``PMIX_ERR_UNREACH`` — "multiple possible servers").  The hold/remap machinery is
inherited unchanged from :ref:`dvm-shrink-campaign-label`; this plan only moves
*when* the fence releases, which the tests above confirm fires correctly.  A
reservation-targeted in-flight shrink remains to be validated.

Two bugs surfaced and were fixed during validation, both worth noting because
they are easy to reintroduce:

* The completion callback was **lost across the master's relay-to-self**: the
  op created in ``xcast_nb`` is discarded by ``begin_xcast`` and the master
  rebuilds a fresh op on receipt, so the callback has to be carried in the FIFO
  and re-attached when the master relays its own broadcast back.
* The FIFO was first declared with ``PMIX_LIST_STATIC_INIT``, whose sentinel
  ``next``/``prev`` are ``NULL``; appending to such a list corrupts memory and
  silently wedged normal launches.  The list must be ``PMIX_CONSTRUCT``-ed — it
  now lives in the xcast-ops object and is constructed in ``xcast_con``.

See ``contrib/dockerswarm/README.md`` for the elastic-mode flag, the cleanup
loop between runs, and the known re-grow flake (#2491).

Open questions
--------------

#. **Terminate-on-lifeline mode — resolved (with a fallback).**  Implemented: a
   target sets ``prte_dvm_leaving`` as it processes the shrink command naming its
   own rank, and departs on a bounded timer or, sooner, when its lifeline drops
   (``prte_rml_route_lost`` exits early only while ``prte_dvm_leaving`` is set, so
   a genuine unrelated fault still recovers).  The race is closed by construction
   as the design decision argues.  The half that was **not** built is the
   survivor *actively closing* the connection to each departing child; the timer
   makes that unnecessary for correctness, at the cost of the doomed daemons
   lingering a second or two after completion.  Building the active close would
   let the fast path fire deterministically and retire the timer.
#. **General callback vs. shrink special case — resolved.**  Implemented as the
   general ``xcast_nb`` facility, usable beyond shrink.
#. **Comm-failure fall-through — resolved.**  A daemon comm-failure is ignored
   when the daemon is already not-alive **and** its recorded state is at or past
   ``PRTE_PROC_STATE_TERMINATED`` (the state the completion handler stamps).  The
   state test is what keeps the guard from swallowing a ``FAILED_TO_START`` daemon
   (never alive, but its state is still below ``TERMINATED`` at that point).
#. **Survivor-side batching — open follow-up.**  See *Implementation status*:
   survivors still repair once per departure.  Batching them means repairing from
   the broadcast list mid-broadcast, which must be reconciled with the reliable
   xcast's ACK bookkeeping on the same tree.
#. **Profiling.**  The original concern was unprofiled.  Worth measuring the
   per-daemon vs. batch repair cost on a large single-branch shrink to confirm
   the optimization earns its complexity — especially since only the HNP side is
   batched so far.

Summary of files changed
------------------------

.. list-table::
   :widths: 40 60
   :header-rows: 1

   * - File
     - Change
   * - ``src/mca/grpcomm/grpcomm.h``
     - Extend the xcast module interface with an optional per-op completion
       callback + ``cbdata`` (Step 1, preferred option).
   * - ``src/mca/grpcomm/direct/grpcomm_direct_xcast.c``
     - Cache the completion callback on ``op_t``; invoke it from ``finish_op()``
       **only** when ``PRTE_PROC_IS_MASTER``.
   * - ``src/mca/ras/base/ras_base_allocate.c``
     - Register the completion callback (carrying the ``prte_shrink_campaign_t``)
       on both ``PRTE_DAEMON_SHRINK_CMD`` xcasts.  Add the Step-2 handler:
       batch ``prte_rml_repair_routing_tree()``, per-target HNP bookkeeping,
       fence decrement, ``prte_ras_base_shrink_complete()``, ``dvm_mod_notify``,
       campaign removal, ``prte_plm_base_fence_release()``.
   * - ``src/prted/prted_comm.c``
     - ``PRTE_DAEMON_SHRINK_CMD`` handler: every daemon repairs its own tree from
       the broadcast target list; a targeted daemon records-and-waits for
       lifeline loss instead of self-exiting (with a bounded fallback).
   * - ``src/mca/errmgr/dvm/errmgr_dvm.c``
     - Remove the per-death shrink-campaign block (lines 286-323) including the
       ``PMIX_RANK_INVALID`` stamping.  Ensure the general daemon-loss handling
       tolerates a comm-failure for a rank already torn down by Step 2.
   * - ``src/rml/routed_radix.c``
     - No change expected — ``prte_rml_repair_routing_tree()`` already accepts a
       rank array and does one pass.  Listed for reference.
   * - ``contrib/dockerswarm/``
     - Testbed grown to ten nodes to exercise single-branch multi-daemon shrink
       (already done alongside this plan).
