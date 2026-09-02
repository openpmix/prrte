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

What the implementation actually needed
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The sketch above is right about the shape and wrong about the cost.  Four
things had to change, and only the first was anticipated.  Each was found by
killing a relay under a held release on the container harness, and each was
measured rather than reasoned about — the count of daemons released went 4,
then 5, then 7.

**The derived tree did not agree with itself.**  Naming a parent by walking up
past dead ancestors, while computing children by replacing a dead child in
place, are two different promotion rules.  At eight daemons, radix 2, with
daemon 1 dead, the promotion puts daemon 4 in slot 1 — so daemon 3's parent is
4, while a walk-up says 0, and 0's children say 4.  Daemon 3 is in no tree at
all.  Both halves now derive from the position the daemon actually occupies.
``test_release_tree`` in ``test/unit/rml`` brute-forces every failure subset
for radices 2 to 4 and requires the relation to be one tree rooted at 0
covering exactly the living daemons; it is the cheapest guard here by a wide
margin, and it catches this in milliseconds.

**"Is the sender my parent?" is not answerable during a repair.**  Both the
forward and the ack-request paths screened on the sender being this daemon's
parent in the tree the message travelled.  That is sound on the routing tree,
because the repair notice travels that same tree — a daemon has processed the
notice that gave it a new parent before anything that parent relays afterwards
can arrive.  A derived tree has no such ordering: every daemon recomputes
independently when the notice reaches it, so a repaired parent replays to a
child that has not heard, and the child discards the one message that would
have released it.  The screen is now asked only of the routing tree.

**An ack must answer whoever asked**, not whoever the answering daemon
currently calls its parent, for the same reason.  Answering the wrong daemon
costs nothing — it holds no operation under that ack id — while failing to
answer the right one hangs the broadcast.

**Nobody defers.**  The routing tree makes a promoted daemon wait for its own
parent to replay before replaying onward, to preserve operation ordering.
Copying that here strands subtrees: a daemon deferring to a parent that has
already replayed waits for a second replay that is never coming.  It is also
unnecessary — an operation a child is missing is by definition still in
flight, so some daemon is replaying it in the same pass, in order, because the
root cannot retire one until every daemon has acked.

**A forward must be read under the view it was computed in.**  This is the one
that does not follow from the tree being derived, and it is the subtlest.  A
daemon promoted into a dead relay's slot still reads itself as a leaf until the
news reaches it, so it answers the repair by retiring the operation as
complete, having forwarded it nowhere — and the subtree the repair existed to
reach is stranded with nobody holding the payload.  Every forward therefore
carries ``prte_rml_tree_version()``, a monotone count of the departures and
returns the sender has learned of, and a receiver that is behind it **and**
was addressed by a daemon it does not call its parent takes the payload,
delivers it locally, and parks the operation without deriving anything until
the notice lands.

Both conditions are load-bearing.  The version has to be monotone, which is
why it counts events learned rather than the size of the failed set — a
revival clears a bit, so a popcount would go *down*, and a daemon that had
processed the revival would read every peer that had not as being ahead of
it.  But monotone is not agreed: a daemon grown into a DVM is seeded from the
departed set it was launched with, and daemons process a revival at different
moments, so counts legitimately differ.  The "not my parent" test is the
concrete local evidence that the two views actually disagree, and it is what
keeps ordinary traffic — which always arrives from a daemon's parent — from
ever being parked.

Any further topology added here — Bruck above all — inherits all five.  They
are properties of *deriving* a tree rather than being told one, not properties
of this particular tree.

Selection
---------

One MCA parameter naming the release topology, defaulting to the current
behaviour.  The rollup is not selectable — every axis favours a high radix
for it, so there is nothing to choose.

As implemented, the two are separate and only the first is a switch:

``grpcomm_low_radix_release`` (bool, default **true**)
   Whether a fence's release leaves the routing tree at all.  Turned off is
   the whole of the old behaviour: every release travels the routing tree,
   and nothing in the derived-tree path runs.  This is the parameter that
   collapses it all back, which is what makes it the right thing to reach
   for if a release ever misbehaves.

``rml_base_radix2`` (int, default **4**)
   The shape of the release tree.  It selects nothing on its own — with the
   switch off it is not consulted at all.

Both defaults changed once the radix question below was worked through, and
they changed together: 4 is where the optimum sits for every payload past
the crossover, and a fence release is never anywhere near the crossover, so
there was no longer a reason to ship the thing switched off.  The original
reason — "no data to choose a winner" — was answered by a model rather than
by the measurement it was waiting for, and the model is decisive in a way a
single machine's numbers would not have been: the conclusion survives an
eightfold sweep of every constant in it.

Setting the two radices *equal* remains a trap, and PRRTE still says so.  At
equal radices the release tree *is* the routing tree — same parent and same
children for every rank and every failure pattern, which
``test_release_tree_matches_routing`` pins — so that combination gets the
identical fanout carried through more machinery, which can only be slower.
It is no longer what leaving the radix alone gives you, but it is still one
setting away in either direction.  The ``release-radix-noop`` help topic is
emitted once by the master for it, and the run continues.

**The topology must be stamped on the wire and checked, not merely
configured.**  A release is a broadcast and does have an originator, so
unlike the fence's movement id this could in principle instruct rather than
detect — but a daemon configured differently from its peers is a real
deployment error, and the failure it produces without a check is a hang.
Mismatch should produce a named ``show_help``, as the withdrawn movement id
did.

First measurement, and what it does not settle
----------------------------------------------

Taken on ``contrib/dockerswarm``, ten daemons, routing radix 64 (so the
release on the routing tree is one hop to all nine peers) against release
radix 2 (depth 4, three hops).  ``grpcomm_enable_timing`` stamps an absolute
microsecond when the originator starts a broadcast and when each daemon has
the payload; coverage is the span between the two, so this measures the
broadcast alone rather than an end-to-end fence — whose run-to-run spread on
this harness is larger than the whole effect.  Payload from
``scaletest --entropy``, since the default fill is a ramp that deflate
squashes 250:1.  Medians over 10 releases per cell:

.. list-table::
   :header-rows: 1

   * - release payload (on the wire)
     - routing tree (radix 64)
     - release tree (radix 2)
   * - 33 B (a barrier's release)
     - 408 us
     - 854 us
   * - 10.5 KB
     - 1810 us
     - 1770 us
   * - 166 KB
     - 4579 us
     - 6326 us
   * - 658 KB
     - 9949 us
     - 11406 us

**Those numbers were taken against a defect and are superseded** — see the
table below.  The forwards were being sent *routed*, so seven of the nine
release edges were relayed through the controller and the fanout was not
reduced at all.  They are kept here because the correction is the more
interesting number.

Once the release edges are sent direct (``prte_rml_send_payload_direct_cb_nb``,
with the peer registered as a lateral link):

.. list-table::
   :header-rows: 1

   * - release payload (on the wire)
     - routing tree (radix 64)
     - release tree (radix 2)
   * - 33 B (a barrier's release)
     - 476 us
     - 563 us
   * - 10.5 KB
     - 1758 us
     - 2018 us
   * - 166 KB
     - 6381 us
     - 4963 us
   * - 658 KB
     - 7774 us
     - 7786 us

The trustworthy row is the first: no payload, so it is pure depth, and it has
55 samples rather than 10.  The low radix's penalty there fell from **+446 us
to +87 us**, which is the seven relayed edges disappearing at roughly the
50 us a hop this harness shows.  The payload rows became *inconclusive* rather
than negative - a single flat-tree cell at 658 KB ranges from 2362 to 36343 us
over ten samples, so no ordering can be read out of them.

That is still not a case for turning the parameter on.  It is the removal of a
reason it could never have worked, and the measurement environment is
unchanged: every "node" is a container on one host, so the copies the flat
tree makes the controller send contend for nothing, and the ``r*M*beta`` term
the low radix exists to reduce stays close to free.

So this is not evidence against the design.  It is a measurement of an
environment in which the quantity being optimized is approximately zero, and
it is the reading the harness section of ``contrib/dockerswarm/AGENTS.md``
warns to expect.  What it does establish:

* the mechanism works and is measurable — the release really does travel the
  other tree, over direct links, and the instrument resolves it;
* the depth penalty is real, but most of what was measured as depth was
  relaying, and it went away with the routed sends;
* nothing here justifies changing the default, which stays the routing tree.

What would settle it is a machine where a daemon's outbound bandwidth is
shared and finite — a real cluster with one NIC a node.  There the flat
tree's nine copies serialize on that NIC while the low radix's extra hops are
paid against a much larger transfer term.  ``contrib/scaling/cluster-sweep.sh``
is the harness for that; until it has been run, the parameter ships off by
default and this table is the only data.

**And check the edges are direct before believing any of it.**  A release tree
whose edges are sent routed is not a second tree at all - at a high routing
radix every non-child edge is relayed by the controller, so the bytes cross
the very link the tree exists to keep them off and the controller handles them
twice.  ``--prtemca rml_base_verbose 2`` names the macro each send used:
release-tree forwards must appear as ``RML-SEND-PAYLOAD-DIRECT-CB``, and a
plain ``RML-SEND-PAYLOAD-CB`` from a non-controller means the measurement is
of the routing tree wearing a disguise.

One caveat for whoever runs it next: the release is store-and-forward, so a
deeper tree pays depth times the *transfer* time, not merely depth times a
latency.  The crossover is therefore not simply "payload big enough" — it
needs the per-copy bandwidth at the root to be the binding constraint, which
is the thing this harness cannot arrange.

A note on reading the instrument: a fence issues **two** releases per
iteration and they are wildly different sizes — the allgather's carries the
modex, the barrier's is 33 bytes — and both go out on the same tag.  A median
taken over all tag-31 releases is therefore dominated by the 33-byte ones and
says almost nothing about payload.  Pair each release with the size on the
originator's own census line before averaging anything.

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
#. **Where does the release radix come from at scale?**
   **Answered: from the payload, through one ratio - and the answer is a
   step rather than a curve, which is why a constant is the right shape for
   it after all.  The number is 4.**

   Write ``N`` for the daemon count, ``M`` for the bytes on the wire,
   ``alpha`` for a hop's latency - the time from a daemon holding the payload
   to its first byte going back out - ``c`` for the software cost of *one*
   forwarded copy, ``B`` for one link's bandwidth, and ``t`` for
   ``prte_num_worker_threads``.  A daemon at radix ``r`` sends ``r`` copies;
   the tree is ``ceil(log_r N)`` deep; and the last daemon at the bottom
   waits for all of it:

   .. code-block:: text

       T(r) = ceil(log_r N) * ( alpha  +  ceil(r/t) * c  +  r * M/B )

   The two fanout terms are there separately because they parallelise
   differently, and that turns out to be the whole answer.  The **software**
   cost of a copy is spread across the worker pool - the OOB does its
   ``writev`` on ``peer->evbase``, and the pool hands out ``t`` bases in
   rotation - so ``r`` copies cost ``ceil(r/t)`` of them.  The **wire** cost
   is not: ``r`` copies of ``M`` bytes cross one NIC whatever thread issued
   them.  So fanout is nearly free until the wire term per copy overtakes the
   thread-divided software term, and ruinous afterwards.  The crossover is
   just those two being equal:

   .. code-block:: text

       M* = B * c / t

   Below ``M*`` the depth term dominates and a *high* radix is right.  Above
   it every copy is bandwidth, and ``r`` multiplies it while only
   ``log_r N`` divides the depth - so the optimum falls to a small constant
   and **stays there for every larger payload**.  Minimising the expression
   above over ``r`` in ``[2, N-1]``, at ``alpha = 100 us``, ``c = 25 us``
   (measured, below), ``t = 8`` and a 10 GbE link (so ``M* = 3.9 KB``):

   ============  =========  ===========  =============  =============
   payload       N = 128    N = 1024     N = 10000      cost of r=64
   ============  =========  ===========  =============  =============
   barrier       12         32           22             1.5 - 1.7x
   10 KB         12         6            7              ~2x
   100 KB        6          4            5              4.4 - 4.7x
   10 MB         2          4            3              6.4 - 7.1x
   256 MB        2          4            3              6.4 - 7.1x
   ============  =========  ===========  =============  =============

   Three things fall out of that table, and each settles something.

   **The optimum barely depends on N.**  Four orders of magnitude of DVM
   size move it between 2 and 6 once the payload is past ``M*``.  It is not a
   function of scale, so it does not want a rule that reads the scale: the
   question "where does it come from at scale" has the answer "not from the
   scale".  What was right in the original worry - that a fixed radix is
   wrong for a *small* DVM - is real but narrow, and it is handled by the
   clamp rather than by a rule: below ``N = 4`` the flat tree is already
   optimal because ``r`` is capped at ``N-1``, and it is beaten by 1.17x at
   ``N = 4``, 2.4x at 16 and 53x at 1024.

   **The number is 4.**  Across every row past the crossover the optimum is
   2, 3, 4 or 5, and 4 is within a few percent of the best of them
   everywhere.  There is nothing to tune and no table to carry.

   **And the tag-based selection already in the code is exactly right.**
   ``prte_grpcomm_release_topology()`` moves the *fence* release to the low
   radix and leaves the group's alone, on the argument that the operations
   differ in what they carry and a byte threshold cannot express that.  The
   model agrees for a better reason than the one given: a fence release is
   the whole modex, always far past ``M*``, and a group release is small,
   always far below it.  The tag is a reliable proxy for which side of ``M*``
   the payload sits on, and the two regimes want genuinely different radices
   rather than different points on a curve.

   **Why nobody has noticed the default is wrong.**  Radix 64 costs only
   1.5 - 1.7x on a barrier and 4.4 - 7.1x on a real modex release.  The
   operations a DVM performs constantly are the small ones, so the penalty
   that shows up in ordinary use is the small one, and the large one appears
   only in the operation nobody profiles.

   **What ``c`` actually is, measured.**  ``grpcomm_enable_timing``'s
   ``started``-to-``completed`` span prices a release directly, with no fence
   and no client around it.  Taken on ``contrib/dockerswarm``, flat radix (so
   the tree is one level and the span is ``alpha' + (N-1)*c`` by
   construction), a bare barrier's release, 15 independent jobs at each of
   ``N = 2, 4, 6, 8, 10``:

   .. code-block:: text

       median of 15:   T =  179 us  +  129 us * (N-1)
       minimum of 15:  T =  235 us  +   25 us * (N-1)

   The median's slope is the ten containers contending for eight cores; the
   minimum's is the closest this harness comes to an uncontended copy.  So
   ``c`` is about **25 us** and the broadcast's fixed floor about **200 us**,
   in a debug build, on loopback, with the acknowledgement tree's return
   included in both.

   That floor is worth noticing on its own.  The barrier fit recorded
   earlier in this document - ``165 us + 40 us * (N-1)`` end to end - was
   read as a client-to-server-to-daemon round trip that "no topology
   touches".  It is not: **the release broadcast by itself, with no client in
   the picture at all, already costs ~200 us fixed and ~25 us a daemon.**
   Both of that fit's terms are substantially the broadcast's own, which
   makes them ours rather than PMIx's, and makes them things a topology
   change does touch.

   **What is still not measured.**  ``alpha``, and ``c`` and ``B`` on
   anything but a shared-kernel container swarm.  The *ordering* survives a
   wide range of them, which is the reason to publish a number now: sweeping
   ``(alpha, c)`` from ``(150, 40)`` to ``(20, 5)`` microseconds - an eight-
   fold change in both - moves ``r*`` between 3 and 5 for every payload past
   10 KB and leaves radix 64 costing 6.1x to 6.4x the optimum throughout.
   Reaching an optimum of 64 needs a hop's latency to be something like 128
   times the cost of a copy, which is not a machine.  What the constants do
   move is ``M*``, which scales directly with ``c`` and inversely with ``t``:
   between those same two pairs it runs from 6.2 KB down to 780 B.  Where the
   step sits is a measurement; which side of it a modex sits on is not in
   doubt.

   It is also a much cheaper measurement than the radix sweep it replaces.
   Fitting ``T = alpha + (N-1) * c`` to a *flat* release over increasing
   ``N`` gives both constants from one curve, and ``B`` comes from the same
   curve repeated at a second payload - no sweep over radices, and no DVM
   cycle per radix.  ``contrib/scaling/cluster-sweep.sh`` should be asking
   for those three numbers rather than for a ranking of radices, and the
   ``completed`` stamp exists so that it can: the span it closes is measured
   on the master's own clock, which is the only one a real cluster has.

   **The coupling worth recording.**  ``M*`` has ``t`` in the denominator,
   so the worker pool and the release radix are one question, not two.
   Raising ``prte_num_worker_threads`` moves the crossover *down* - more
   threads make software fanout cheaper, so the payload at which bandwidth
   takes over is smaller, and the low radix becomes right sooner.  Any future
   measurement of one of them has to record the other.
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
