Rewiring a Returned Daemon: the "Unheal" Path
=============================================

Status: **design draft**.  This document proposes a mechanism, not yet
implemented, for restoring a daemon to the routing tree after it has
disappeared and later returned.  It builds directly on the heal path
described in :doc:`bootstrap_plan` (Step 7b) and reuses the fault
machinery in ``src/rml``.

Problem statement
-----------------

In a bootstrapped DVM the compute nodes come up independently and are not
fanned out by a launcher.  A node can therefore *leave* the DVM without the
DVM being torn down — the node is powered off, loses power, or is rebooted —
and then *return* when it comes back up.  Its daemon boots again with the
**same rank** (rank is derived from the node's fixed position in the
``DVMNodes`` ordering, not assigned at launch).

Today the RML handles only half of this life cycle:

* **Heal (works).**  When the daemon disappears, ``lost_connection`` /
  ``failed_to_connect`` drive ``prte_rml_route_lost`` →
  ``prte_rml_repair_routing_tree``, which promotes the orphaned children to
  their grandparent and drives adoption/failure notices.  The tree closes
  over the hole.

* **Unheal (missing).**  When the daemon returns, nothing re-inserts it.  The
  departure was recorded as **permanent**: ``prte_rml_repair_routing_tree``
  sets the rank in both ``failed_dmns`` *and* ``dead_dmns``
  (``routed_radix.c``), and ``dead_dmns`` is never cleared and is restored
  into ``failed_dmns`` on every ``prte_rml_compute_routing_tree``.  From then
  on ``radix_is_living`` reports the rank dead forever, so ``get_route``, the
  children array, and the ancestor list never point back at it.  The returned
  daemon becomes a zombie the tree ignores; its former children stay attached
  to the grandparent.

The goal of this design is to make the return a first-class event: the
returned daemon rejoins its old slot in the tree, its children drop the
grandparent lifeline and re-home to it, and the DVM converges on the same
tree it would have computed had the daemon never left.

Scope and non-goals
-------------------

* **Bootstrap mode only.**  Unheal is gated on ``prte_bootstrap_setup``.  In a
  launcher-driven or elastic-shrink DVM a departed vpid is genuinely
  permanent (a shrink retires the vpid on purpose; #2491 depends on it), and
  those modes keep their current behavior unchanged.
* **Same rank, same identity.**  We handle a node that returns as *itself*
  (same nspace + rank).  We do **not** reuse a vpid for a different node; that
  invariant is preserved.
* **No change to the heal path's externally observed behavior.**  A daemon
  that leaves and never returns must behave exactly as it does today.

Design principle: revival is the inverse of repair, not a special case of it
----------------------------------------------------------------------------

The tree is a deterministic function of ``(radix, num_daemons, failed_set)``.
Death removes a rank from the live set and everyone recomputes; revival adds
it back and everyone recomputes.  The two are symmetric at the level of the
routing math, but **not** at the level of the notification code:

* ``prte_rml_repair_routing_tree`` and the adoption-inference logic in
  ``rml_fault_handler.c`` assume **depth only ever decreases** — a promotion.
  ``prte_rml_recv_adoption_notice`` explicitly treats an ancestor list that
  *grew* as an unrecoverable invariant violation
  (``rml_fault_handler.c``, the ``report.size > ancestors.size`` branch that
  raises ``PRTE_ERR_UNRECOVERABLE``).

* Revival is exactly the case that grows a daemon's ancestor list: a rank is
  re-inserted **above** the daemons in its former subtree, demoting them one
  level.  Feeding that through the death path would trip the invariant check.

Therefore revival needs its **own** recompute-and-notify entry point,
``prte_rml_revive_routing_tree``, parallel to ``prte_rml_repair_routing_tree``,
plus its own notice tags.  It must not be bolted onto the repair path.

Separating "absent" from "dead"
-------------------------------

The root cause of the missing behavior is that one bitmap
(``dead_dmns``) is asked to mean two different things.  Split them:

===================  ============  ==================================  ==================
Set                  Persistent?   Set by                              Cleared by
===================  ============  ==================================  ==================
``failed_dmns``      no (per       repair / revive / compute restore   compute re-init;
                     recompute)                                        revive
``global_failed``    no            global death xcast                  revive
``dead_dmns``        yes           shrink holes (``nidmap.c``);         **never**
                                   non-bootstrap faults
``absent_dmns``      yes           **bootstrap** faults (new)          revive (new)
(new)
===================  ============  ==================================  ==================

* ``absent_dmns`` is a new persistent bitmap constructed once in
  ``prte_rml_open`` alongside ``dead_dmns`` and, like it, **not** re-initialized
  by ``prte_rml_compute_routing_tree``.
* ``prte_rml_repair_routing_tree`` chooses the target set by mode: in a
  bootstrapped DVM a fault records the rank in ``absent_dmns``; otherwise it
  records it in ``dead_dmns`` exactly as today.  Both are restored into the
  freshly-initialized ``failed_dmns`` at the top of
  ``prte_rml_compute_routing_tree`` (so a grow still routes around an
  absent-but-not-yet-returned daemon).
* Revival clears the rank from ``failed_dmns``, ``global_failed_dmns``, and
  ``absent_dmns``.  ``dead_dmns`` is still never touched — a shrunk-out rank
  can never be revived, which is correct.

This keeps the #2491 fix and all launcher/elastic behavior byte-for-byte
identical (nothing outside bootstrap ever populates ``absent_dmns``), while
giving bootstrap a departure set that *can* be reversed.

The trigger: a returned daemon announces itself to the HNP
----------------------------------------------------------

When the node reboots, its daemon runs the normal bootstrap startup
(:doc:`bootstrap_plan`, Steps 4–7).  It computes a **healthy** tree — its own
``absent_dmns`` is empty, so it sees the full DVM — and connects up its
lifeline.  It has no way to know, on its own, that the rest of the DVM wrote
it off while it was gone.

Rather than infer the return from a stray inbound socket (routing policy does
not belong in the OOB accept path), make it explicit and route it through the
arbiter of global tree state, the **HNP**, mirroring how death is globally
xcast:

#. **Rejoin request (up).**  On bootstrap startup, when
   ``prte_bootstrap_setup`` is set, the daemon sends
   ``PRTE_RML_TAG_DAEMON_RETURNED{rank=self}`` toward the HNP along its
   computed lifeline.  Intermediate hops relay it by destination as usual; a
   relaying hop does not need the source to be "live" in its own tree, so the
   message reaches the HNP even though the sender is still marked absent
   upstream.  (First-boot daemons send this too; the HNP simply finds the rank
   not marked absent and does nothing — see idempotence below.)

#. **HNP validates and broadcasts (global).**  The HNP checks the rank against
   ``absent_dmns``.  If absent, it clears the rank locally and xcasts
   ``PRTE_RML_TAG_DAEMON_REVIVED{rank}`` to the whole DVM, exactly as
   ``send_failures_notice`` xcasts ``PRTE_RML_TAG_DAEMON_DIED`` from the master
   (``rml_fault_handler.c``).  If the rank is not absent (a genuine first boot,
   or a duplicate), the HNP drops the request — the operation is idempotent.

#. **Everyone converges (global).**  Each daemon's
   ``prte_rml_recv_revival_notice`` clears the rank from its failure sets and
   calls ``prte_rml_revive_routing_tree(rank)``.  Because the failed set is now
   globally consistent again, every daemon deterministically recomputes the
   same tree.

Centralizing at the HNP avoids the split-brain that per-subtree local revival
would invite, and reuses the existing global-xcast plumbing.

``prte_rml_revive_routing_tree`` — the recompute and its deltas
-----------------------------------------------------------------

Symmetric to ``prte_rml_repair_routing_tree``:

#. Clear ``rank`` from ``failed_dmns`` / ``global_failed_dmns`` /
   ``absent_dmns`` (the recv handler does this before calling in, matching how
   ``repair`` sets the bit before recomputing).
#. Snapshot ``prev_ancestors`` / ``prev_parent`` / ``prev_children`` into a
   ``prte_rml_recovery_status_t``.
#. Re-derive ancestors, promotion/**demotion**, and children.  Most of this is
   the existing helpers run against the updated (smaller) failed set:

   * ``prte_rml_update_ancestors`` already walks to the next *living* ancestor;
     with ``rank`` now living again it will re-appear in the lists of the
     daemons below it, **growing** their ancestor arrays.  This is the case the
     current code deliberately rejects, so ``update_ancestors`` needs an
     audited pass to confirm it produces the right list when depth increases
     (it may need a companion to the promotion path — a "demotion" fixup —
     analogous to ``handle_promotion``).
   * The daemon that had adopted ``rank``'s orphans (``rank``'s parent) loses
     them from its child list; ``rank`` regains them.  ``update_descendants``
     recomputes children from the live set, so the child arrays fall out
     correctly once the failed bit is cleared; the work is producing the
     *delta* for the notices.

#. Fill in ``parent_changed`` / ``children_changed`` / a new ``demoted`` flag
   (mirroring ``promoted``) and notify the components:
   ``prte_rml_fault_handler``, ``prte_grpcomm.fault_handler``,
   ``prte_filem.fault_handler``, ``prte_relm.fault_handler``.

Recovery-status and component impact
------------------------------------

The existing ``prte_rml_recovery_status_t`` is close but assumes promotion.
Two additions:

* Add a ``bool demoted`` flag (a daemon gained an ancestor / its subtree
  shrank), the mirror of ``promoted``.  Handlers that "treat all children as
  new when promoted" need the analogous rule: **treat the re-homing children
  as new when a neighbor is revived**, because a child that briefly had the
  grandparent as parent must discard that lineage.
* The RML's own reaction (``rml_fault_handler.c``) needs revival analogues of
  its two notices:

  - **Re-home notice (down)** — the inverse of the adoption notice.  ``rank``'s
    parent tells the affected promoted children "your ancestor list has
    changed; ``rank`` is back above you," so they drop the grandparent lifeline
    and re-open their lifeline to ``rank`` (or the closest revived ancestor in
    their path).  The receive handler is the inverse of
    ``prte_rml_recv_adoption_notice`` and must accept a *grown* ancestor list
    rather than rejecting it.
  - **Rejoin/rollup (up)** — RELM re-drives any messages that were in flight
    across the re-homing so nothing is lost, exactly as it re-drives across a
    heal.

``prte_grpcomm`` and ``prte_filem`` already receive the recovery status on
every tree change; they must tolerate a change whose net effect is a daemon
*appearing*.  The audit here is: does any collective/xcast accounting assume
membership only shrinks?  ``prte_rml_get_num_contributors`` counts live
children, so a revived child correctly re-enters the count once the failed bit
clears — but in-progress collectives that already excluded ``rank`` need the
same "save state between local and global scope" discipline the death path
uses (``rml_types.h`` documents this contract on the status struct).

Bringing the returned daemon up to date
----------------------------------------

The routing tree is only half the problem.  While it was gone the returned
daemon missed everything: jobs launched, other faults, nidmap growth from
elastic grows.  It boots with a stale world view.  Re-inserting it into the
tree without resynchronizing its state would let it route correctly but act on
stale data.

This is the same problem the **elastic grow** path already solves — "admit a
daemon into a running DVM and hand it the current state" — with one twist: the
vpid is the returned daemon's own, not a newly minted one.  Reuse that
machinery:

* The HNP, on processing the rejoin, drives the returned daemon through the
  grow-style wireup so it receives the current nidmap (with any holes) and the
  active job/proc data, rather than the boot-time snapshot.
* Because the returned rank is an existing hole rather than an extension of the
  vpid span, ``num_daemons`` does **not** change; only the returned rank's
  ``dead``/``absent`` state and the tree change.  The nidmap-hole bookkeeping
  in ``nidmap.c`` must not re-mark the returning rank as dead when it repopulates
  ``daemons->procs`` — clearing ``absent_dmns`` for the rank must precede, or be
  reconciled with, that scan.

Concurrency and correctness concerns
------------------------------------

* **Incarnation / stale-message hazard.**  The returned daemon is a *new
  process* wearing the *old rank*.  Messages queued to the old incarnation, or
  late death/adoption notices still in flight, could be mis-delivered to the
  new one.  **Decided:** tag each daemon with a **boot epoch** — a
  monotonically increasing incarnation counter — and carry it in the wire
  header (``prte_oob_tcp_hdr.h``) so a hop can drop a message addressed to a
  stale incarnation of a rank.  This is safe to add: the header is not an ABI
  (see the RML ``AGENTS.md`` — it is exchanged only among daemons of one DVM,
  which all run the same PRRTE build), so there is no cross-version concern,
  only the requirement that every daemon agree, which a single build
  guarantees.  The epoch is the daemon's **boot timestamp**, captured once at
  startup (a wall-clock time at ``prte_init``); no persisted on-disk counter is
  needed.  A reboot yields a later timestamp, so the returned incarnation
  always outranks the one the DVM wrote off.  (The one degenerate case — a
  reboot fast enough, or with a reset clock, to reproduce the prior timestamp —
  is bounded by timestamp resolution; use at least millisecond granularity, and
  the HNP can reject a ``DAEMON_RETURNED`` whose epoch is not strictly greater
  than the recorded one, forcing a retry.)  The epoch is announced in the
  ``DAEMON_RETURNED`` request so the HNP propagates it in the
  ``DAEMON_REVIVED`` xcast; peers record the current epoch per rank and reject
  header-stamped traffic from an older one.  See Stage 6.
* **Revive/again-die races.**  A node that flaps (returns, dies again before
  the revival xcast completes) must converge.  Because both death and revival
  are HNP-arbitrated global xcasts over the same rank, ordering them at the HNP
  (serialize per-rank; the last event wins) keeps every daemon consistent.  The
  recv handlers must be idempotent (clearing an already-clear bit, or setting an
  already-set one, is a no-op that produces an empty delta and no notices — the
  ``status.failed_ranks.size == 0`` early return in ``repair`` already models
  this).
* **Stale OOB peer object.**  The peer object for ``rank`` on its neighbors is
  in a closed/failed state from the original loss.  Revival must reset it (or
  drop it so the next send re-synthesizes the URI via
  ``prte_ess_base_bootstrap_peer_uri`` and reconnects, as the heal path already
  does for adopted parents).

Staged implementation plan
--------------------------

The stages are independently reviewable and ordered so the tree keeps building
and behaving at each step.

**Stage 1 — Split the departure sets.**  Add ``absent_dmns`` to
``prte_rml_base`` (construct in ``prte_rml_open``, restore into ``failed_dmns``
in ``prte_rml_compute_routing_tree``).  Route bootstrap faults to it instead of
``dead_dmns``.  No behavior change yet (an absent daemon still never returns);
this only reclassifies where the mark lives.  Verify launched/elastic behavior
is untouched (nothing populates ``absent_dmns`` outside bootstrap).

**Stage 2 — Revival recompute.**  Implement
``prte_rml_revive_routing_tree(rank)`` and the ``demoted`` status flag; audit
``update_ancestors`` for growing ancestor lists and add a demotion fixup if
needed.  Unit-exercise it by directly clearing a bit and calling it on a small
synthetic tree.

**Stage 3 — Global protocol.**  Add ``PRTE_RML_TAG_DAEMON_RETURNED`` and
``PRTE_RML_TAG_DAEMON_REVIVED``, the HNP validate-and-xcast, and
``prte_rml_recv_revival_notice``.  At this point a returned daemon that already
holds current state rejoins the tree and children re-home.

**Stage 4 — Re-home and RELM.**  Add the re-home (inverse-adoption) notice and
its receiver that accepts a grown ancestor list, and extend the RELM fault
handler to re-drive across a revival.

**Stage 5 — State resync.**  Wire the returned daemon through the grow-style
state handoff so it comes back with the current nidmap and job data; reconcile
with the ``nidmap.c`` hole scan.

**Stage 6 — Incarnation guard.**  Add the boot-epoch field to the wire header
(``prte_oob_tcp_hdr_t``), stamp outgoing messages with the sender's epoch,
carry the returned daemon's epoch through the ``DAEMON_RETURNED`` /
``DAEMON_REVIVED`` exchange, and drop inbound traffic stamped with a stale
epoch for a rank.  This closes the stale-message window that the return of a
same-rank/new-process daemon opens.

Testing
-------

The Docker multi-node bootstrap harness (``contrib/dockerswarm/``) already
drives launcher-less formation.  Extend it:

#. Form a bootstrapped DVM of enough nodes to have a non-trivial interior
   (radix small enough that some daemon has both a parent and children).
#. Kill an interior node's daemon (or ``docker stop`` the node); confirm the
   heal — children promote to the grandparent — via ``rml_base_verbose``.
#. Restart the node; confirm the unheal — the ``DAEMON_RETURNED`` /
   ``DAEMON_REVIVED`` exchange, the children re-homing to the returned rank,
   and the tree matching a never-failed run.
#. Launch a job across the DVM *after* the unheal to confirm the returned
   daemon carries current state and participates in collectives.
#. Flap test: kill and restart in quick succession to exercise the
   race/idempotence handling.

Resolved decisions
------------------

#. **Incarnation identity — boot-epoch in the wire header.**  Adopt a boot-epoch
   incarnation counter carried in ``prte_oob_tcp_hdr_t`` (Stage 6) rather than
   trying to close the stale-message window with xcast ordering and peer reset
   alone.  There is no ABI cost: every daemon of a DVM runs the same PRRTE
   build, so the header can change freely as long as all daemons agree.  The
   epoch value is the daemon's **boot timestamp** (captured at ``prte_init``),
   not a persisted counter — a reboot always produces a later value, so no
   on-disk state is required.
#. **Bootstrap-only — no launched/elastic re-launch.**  Unheal stays gated on
   ``prte_bootstrap_setup``.  Extending it to a launched or elastic DVM would
   require re-launching the returned daemon into its **original** vpid (an
   existing hole), and the bulk launchers cannot do that: SLURM, PALS, and
   similar RMs assign vpids sequentially over the node set they are handed and
   offer no way to force a *particular* vpid one-at-a-time, so there is no
   portable re-launch-into-hole primitive to build on.  Only bootstrap, where a
   returned node re-derives its own rank from static configuration and re-runs
   its own daemon, provides the returned-with-original-rank precondition the
   mechanism needs.  The RML core (the ``absent_dmns`` split,
   ``prte_rml_revive_routing_tree``, the revival protocol, re-home notices, the
   boot-epoch guard) is not itself launcher-specific, so this could be revisited
   if a launcher ever gains per-vpid placement — but it is out of scope now.

Open questions
--------------

#. **Trigger source.**  Route the rejoin through the HNP (this proposal) or let
   the returned daemon's *parent* arbitrate locally and forward up?  HNP-central
   is simpler and race-safe but adds one hop of latency to the rejoin.
#. **Partial returns.**  If several daemons in one subtree are absent and only
   some return, the recompute must handle a partially-repopulated path; confirm
   ``update_ancestors`` walks correctly when the returned rank is itself below
   another still-absent ancestor.
