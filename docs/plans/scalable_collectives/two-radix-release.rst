Two trees: a low-radix release beside the high-radix rollup
===========================================================

Status
------

Design, not yet implemented.  It is the first of the alternative collection
topologies that ``grpcomm`` will offer behind an MCA parameter, with the
existing single tree remaining the default because it is the one we know
works.  We will not have data to pick a winner before release, so the others
ship for evaluation rather than as a recommendation.

Why this one first
------------------

Of everything the withdrawn lateral work explored, this is the largest gain
per unit of new risk, and the arithmetic is already recorded under
"Separating the barrier from the modex".

**The gather and the broadcast are asymmetric, and that is the whole
opportunity.**  A broadcasting daemon receives one copy of ``D`` and sends
``r`` copies, so its egress is ``r*D``.  A gathering daemon receives ``r``
messages and sends **one** aggregate, so fanout costs it nothing at all.
Every axis therefore favours a *high* radix for the rollup — smaller
per-node egress, fewer hops, ``d`` times fewer total byte-hops — and only the
release wants a low one.  Writing the cost of the pair as
``(1 + r_release) * D/B``:

====================================  ==============  ==========
configuration                         cost            vs today
====================================  ==============  ==========
radix 64 both (today)                 ``65 * D/B``      1x
gather 64, release 3                  ``4 * D/B``     **16x**
Bruck allgather                       ``1 * D/B``       65x
====================================  ==============  ==========

So a two-radix tree captures 16x of an available 65x **with no exchange
schedule**.  Its ceiling is the gather's own ``D/B``, irreducible because the
controller must receive the entire modex; that is why gather-then-broadcast
can never reach an allgather, and why Bruck remains worth another 4x on top.

**And the transport already exists.**  A radix-3 release edge is a *sibling*
edge in a radix-64 routing tree, so this needs direct sends to non-children —
which is Piece 1, and Piece 1 survived the tree-only reset.
``prte_rml_send_buffer_direct_nb``, ``PRTE_RML_SEND_DIRECT``,
``prte_rml_base.lateral_links`` and ``prte_rml_is_lateral_only`` are all on
master and currently unused.

The decision that shapes everything else
----------------------------------------

**Each tree carries its own reliable-message recovery.**  There are two trees
in the system with two dedicated purposes — rollup and fan-out — and neither
can account for the other's coverage.

This is not a detail of implementation, it is the thing that makes the design
tractable, and the reason is visible in what an ``op_t`` already holds
(``src/grpcomm/grpcomm_xcast.c``):

* ``nexpected`` is "# children at time of (re)start" — a statement about one
  topology's shape.
* ``ack_id_up`` / ``ack_id_down`` "track which acks are valid by order of
  faults reported from HNP" — an ordering over one topology's repairs.
* ``replay_pending_parent`` exists because "our completion information for
  older ops is invalid when our subtree grows" — a promotion in one tree.

You cannot account for coverage of a message that traversed tree *B* by
rolling up acks on tree *A*.  A release whose reliability is accounted on the
wrong topology is worse than one with no reliability at all, because it
reports success it has not established.

Shape
-----

**Parameterize the existing op machinery by topology; do not copy it.**  The
reliability logic — hold the payload until completion is confirmed, count
child acks, replay on repair — is correct and hard-won, and it is the same
logic for both trees.  What differs is only *which tree* it asks for children
and *which* repairs it reacts to.  So an op gains a topology, and:

* ``forward_op`` asks that topology for its children rather than
  ``prte_rml_base`` directly.
* acks roll up that topology.
* ``nexpected`` is that topology's child count.
* a repair in that topology triggers that topology's replay.

**The op id must stop being one global space.**  It is documented today as
"HNP's assigned collective ID, globally unique", and two topologies
allocating from it will alias.  Either the topology becomes part of the key
or each gets its own counter; the key is cleaner, and matches the direction
the fence has already taken with ``(signature, generation, step)``.

**Each topology needs its own RML tag and receive path.**  This is not
optional tidiness.  A daemon receiving a release on a sibling edge is not in
any of this daemon's routing subtrees, and any handler that screens on
``prte_rml_get_subtree_index()`` will discard it — which is exactly what the
deleted ``PRTE_RML_TAG_XCAST_BULK`` and ``_FENCE_EXCHANGE`` existed to avoid.

Deriving the second tree
------------------------

Every daemon must compute the same low-radix tree independently, from the
same inputs — the daemon set and the release radix — exactly as the routing
tree is derived today.  No exchange, no agreement protocol, no state to keep
in step.  That matters beyond convenience: it is the same "every participant
must reach the same answer independently" rule that a fence lives under,
because a fence has no originator to settle a disagreement.

The failed-daemon set is an input, and it must be **the same input on every
daemon**.  The withdrawn work learned this the expensive way and it is
recorded under Piece 2: deriving a participant list from the failed set makes
daemons disagree in the window around a fault, and the collective deadlocks.
The release tree is derived from the daemon set and repaired on the recovery
epoch, which every daemon already adopts in step.

Fault model
-----------

A daemon that dies is a relay in *both* trees, and each has to repair
independently.

* In the **rollup** tree nothing changes: the routing tree repairs as it does
  today, and the fence's existing recovery restart re-offers contributions.
* In the **release** tree the dead daemon orphans its low-radix subtree.  That
  subtree's members must be re-parented by the same deterministic derivation —
  they recompute the tree from the surviving daemon set and reach the same
  answer — and the release replayed to them, which is what the op's held
  payload is for.

Note the asymmetry that makes this tractable: a release is idempotent at the
receiver.  A daemon that receives the same release twice retires a tracker
that is already gone, which ``fence_release`` already treats as "not
involved" rather than an error.  Replaying too much is safe; replaying too
little hangs.  Prefer the former.

What is *not* covered by the fence's existing fault handler: it ends a fence
that lost a **participant**, and knows nothing about losing a **relay on a
second topology**.  That is this design's own responsibility.

Selection
---------

One MCA parameter naming the release topology, defaulting to the current
behaviour.  The rollup is not selectable — every axis favours a high radix
for it, so there is nothing to choose.

A second parameter gives the release radix, so the 16x above can be explored
rather than asserted; the cost model says 3 but the model's constants are the
ones we do not have.

**The topology must be stamped on the wire and checked, not merely
configured.**  A release is a broadcast and does have an originator, so
unlike the fence's movement id this could in principle instruct rather than
detect — but a daemon configured differently from its peers is a real
deployment error, and the failure it produces without a check is a hang.
Mismatch should produce a named ``show_help``, as the withdrawn movement id
did.

Verification
------------

* **Unit** — the low-radix tree derivation: same answer on every daemon for
  the same inputs, including non-power-of-two daemon counts and the vpid
  holes an elastic shrink leaves.
* **Multi-node** — a large modex over a DVM with the release at a low radix,
  every daemon holding identical bytes; and the fault case, where a relay in
  the release tree dies mid-release and its orphaned subtree is still
  released.  ``contrib/dockerswarm``'s ``grpcomm_release_delay_ms`` can widen
  the window a fault has to land in.
* **A/B** — the same sweep with the release topology set both ways.  Expect
  it to show nothing on the container swarm: the fence is fixed-cost
  dominated below roughly 140 KB of total modex, and a real job's modex is
  far under that.  That is not a reason to skip the measurement, but it is a
  reason not to read a null result as a verdict.

Open questions
--------------

#. **Does the release still reach every daemon, or only participants?**
   **Answered: every daemon, and not merely because it is simpler.**

   To be clear about what the alternative was, since it is easy to misread:
   a participants-only release is still a *relayed tree*, not a message to
   each participant.  Only the node set differs — daemons hosting
   participants rather than every daemon — so the traffic is ``(P-1)*D``
   against ``(N-1)*D``.  For a job-wide fence ``P`` is ``N`` and there is no
   difference at all; the saving exists only for a **subset** fence on a
   large DVM, where today the payload reaches daemons with no stake in it.

   What rules it out is the round number.  ``fence_release`` records the
   generation *before* it looks for a tracker, deliberately, so **every**
   daemon records **every** signature's round whether it participated or
   not.  That is what makes the bootstrap rule safe: a daemon present since
   the DVM started may stamp 0 for a signature it has no entry for, because
   a DVM-wide release means "no entry" really does imply "round 0 has not
   happened yet".

   Send the release only to participants and that implication fails.  A
   daemon outside the set records nothing; the job later grows onto it, so
   it acquires participants; it has no entry and it is *not* ``joined_late``,
   having been here since the start; so it stamps 0 while every other
   participant is at ``k``, and its contribution is dropped as ancient.  A
   hang — precisely the failure the joiner flag exists to prevent, reached
   through a different door.  And unlike a joiner, this daemon cannot detect
   its own situation: "I have been a participant throughout" is not
   something it can know locally.

   If the subset saving is ever wanted, the way to have both is to split the
   release: a small DVM-wide notice that round ``k`` of signature ``S`` is
   done, and the *payload* over participants only.  The notice is O(1)
   against the payload's ``D``, so the saving survives and the invariant
   holds.  Not worth building until a subset-fence workload asks for it.
#. **Where does the release radix come from at scale?**  A fixed 3 is the
   cost model's answer for a large DVM and is plainly wrong for a small one,
   where the extra depth buys nothing.  A rule of thumb wants data we do not
   have.
#. **Does the group collective's release want the same treatment?**
   **Answered: it gets the mechanism for free, and should keep the tree's
   radix.**

   There is nothing to consolidate — the consolidation already happened when
   ``grpcomm`` came out of MCA.  ``prte_grpcomm_release_bcast`` is a single
   seam that every release goes through: two sites in the fence (the normal
   release and ``abort_fence_op``'s) and two in the group.  Implementing the
   low-radix release inside that seam means both operations get it, and the
   two cannot drift apart into different methods for the same job.

   What should stay per-operation is the **radix**, not the mechanism.  A
   group release is small, and a low radix only pays where the ``r*M*beta``
   term is real; where it is nil, a low radix buys nothing and costs depth —
   the same reason the operations table says a barrier and a tiny broadcast
   both want a *high* radix.  Since the seam already takes a tag, selecting
   on it is free, and it is the lesson Piece 5 recorded: the launch message
   was given a tag of its own precisely so the choice could be made by
   operation rather than by a byte threshold.

   So: one mechanism, chosen by tag.  ``PRTE_RML_TAG_FENCE_RELEASE`` takes
   the low radix when it is enabled; ``PRTE_RML_TAG_GROUP_RELEASE`` keeps the
   tree unless someone measures a reason to change it.
