Restructuring the DVM collectives around operations
===================================================

Context
-------

PRRTE's DVM-wide collectives all run on the RML radix routing tree, and until
recently they did so through an MCA framework with exactly one component.
Two problems had accumulated.

**The abstraction was on the wrong axis.** ``grpcomm`` was a framework whose
``direct`` component returned priority 5 and declared itself "always
available"; the historical ``bmg`` component had long since been deleted, and
selection was single-winner. More importantly, the choice that actually
matters — which algorithm moves the data — is a *per-operation,
per-message-size* decision, and a component cannot express that. A component
is chosen once, for the whole interface, for the life of the process.

**One implementation serves wildly different operations.** ``xcast`` carries
both the launch message and a two-byte shutdown command, on the same tag.
``fence`` serves both a zero-byte barrier and the full modex. The single tree
algorithm is near-optimal for one end of each pair and badly wrong for the
other.

Status
------

.. warning::

   **The lateral movements have been removed.**  ``grpcomm`` moves every
   broadcast and every fence on the routing tree, and only on the routing
   tree.  ``scatter_allgather``, ``rd_allgather``, the Bruck exchange
   schedule (``grpcomm_exchange.c``), the two lateral RML tags, the four
   ``grpcomm_*_movement`` MCA parameters, and the framing each of them
   required — the partial-payload gate, the out-of-order op hold, the
   early-chunk parking, the degrade-to-tree fallback, the fence's movement
   interlock and its per-participant deadline — are all gone.

   The rest of this document is kept deliberately.  It is the record of what
   was built, what was measured, and what the reasoning was, and anyone
   reintroducing a lateral movement should read it before starting rather
   than rediscover it.  Read the sections below as history, not as a
   description of the code.

   **Why.**  A fence over a lateral exchange could not survive consecutive
   fences: measured on ten daemons at radix 2, five fences back to back, the
   exchange hung or failed on three attempts out of three while the rollup
   completed cleanly on three out of three.  A generation in the fence
   signature closes that, but the mechanism it closes exists only because a
   release travels the tree while exchange blocks travel across it — two
   routes, and a participant legally starting fence *N+1* before a peer has
   finished *N*.  On one route the window does not exist.  The two other
   things the movements bought were also weaker than they looked by the time
   they were withdrawn: the launch message, their main beneficiary, had
   shrunk by roughly 3.5x (PRRTE #2628), so a one-proc launch is a few
   hundred bytes wrapped in a 16 KB participant list, and the fence's own
   win was
   never confirmed on hardware where ``alpha`` and ``beta`` mean what the
   cost model assumes.

   What survives, and was worth the exercise: ``grpcomm`` is no longer an
   MCA framework, the launch message is a great deal smaller, and the
   framing/movement separation is documented well enough to be rebuilt.


Every step was verifiable on its own, and the multi-node suite has not
regressed at any point.  Both second movements are implemented and exercised
across a real multi-node DVM, and both are **opt-in**: an unconfigured DVM
moves every broadcast and every fence exactly as it did before this work.

.. list-table::
   :header-rows: 1
   :widths: 18 62 20

   * - Commit
     - What
     - State
   * - ``404e553273``
     - ``grpcomm`` collapsed out of MCA into ``src/grpcomm/`` (net −772 lines),
       following the precedent of ``src/rml`` (itself once three frameworks)
     - done
   * - ``f46e0d4371``
     - RML lateral links: the direct send and the fault gate
     - done
   * - ``25416cf534``
     - Broadcast framing/movement split; ``tree_whole`` the only movement
     - done
   * - ``2e2a3c77ef``
     - The Bruck allgather exchange schedule
     - done
   * - ``aa41998d39``
     - The scatter's chunk partition
     - done
   * - —
     - The bulk transport itself: ``scatter_allgather``, the
       ``payload_complete`` gate, the op-order hold, and the degrade-to-tree
       fault path
     - done, **opt-in**
   * - —
     - Fence framing/movement split, ``rd_allgather``, and the movement
       interlock (Piece 3)
     - done, **opt-in**
   * - —
     - Selecting a fence's movement from ``PMIX_COLLECT_DATA`` (Piece 4)
     - done — **no PMIx change was needed**

Both halves of the bulk movement's *arithmetic* went in before any of its
transport, and are tested exhaustively without one.  That ordering was chosen
because an error in either would surface later as what looks like a transport
or corruption bug rather than an arithmetic one.

**Both second movements are now the default**, each chosen per operation:

* The **broadcast** selects by tag.  The launch message — split onto
  ``PRTE_RML_TAG_DAEMON_LAUNCH`` for exactly this purpose — scatters;
  everything else takes the tree.  The byte threshold survives only as the
  opt-in ``size`` selection.
* The **fence** selects on ``PMIX_COLLECT_DATA``.  A modex gets the exchange
  because the release fanout, not the gather, is what dominates it; a barrier
  keeps the rollup because a high-radix tree beats a dissemination exchange at
  *any* scale.

Neither default rests on a measured constant, because neither has one left to
rest on: both discriminators are categorical.  That is what made it reasonable
to turn them on rather than leave them behind a parameter — and turning them on
is the point, because a movement nobody selects is a movement nobody tests, and
the failure modes here (a broadcast that misparses, a fence that cannot
converge) are the kind that surface at scale and under fault rather than in a
unit test.

``tree`` on either parameter reverts to the previous behaviour in one flag.

The operations
--------------

.. list-table::
   :header-rows: 1
   :widths: 26 14 10 14 36

   * - Operation
     - Pattern
     - Payload
     - Ordering
     - Wanted
   * - shutdown / job-ctrl / notification / monitor commands
     - one-to-all
     - ~0
     - some
     - **unchanged**
   * - ``DAEMON_DIED`` / ``DAEMON_REVIVED``
     - one-to-all
     - ~0
     - **critical**
     - **unchanged**
   * - launch message
     - one-to-all
     - large
     - no
     - scatter + allgather
   * - ``WIREUP``
     - one-to-all
     - large
     - **critical**
     - **unchanged** — ``process_first``
   * - ``FILEM`` chunks
     - one-to-all
     - 16 KB each
     - no
     - **unchanged** — see below
   * - barrier (``PMIx_Fence``, no collect)
     - all-to-all
     - 0
     - no
     - **unchanged**
   * - modex (``PMIx_Fence``, collect)
     - all-to-all
     - large
     - no
     - direct allgather
   * - ``PMIx_Group_construct`` / ``_destruct``
     - all-to-all
     - medium
     - no
     - rides the allgather

Two of the "unchanged" rows are worth as much as the two changes:

* **The barrier needs no new algorithm.** Its cost is ``2*d*alpha``, and a
  high radix crushes ``d`` — at radix 64 a 4096-daemon DVM is depth 2. A
  dissemination barrier would be ``log2(N)*alpha = 12*alpha``, i.e.
  *worse*. The only win available is constant-factor: not dragging a zero-byte
  collective through the compression attempt, op-id sequencing and ACK rollup
  that a bulk broadcast needs.
* **Tiny broadcasts need none either**, for the same reason. A high radix is
  right when the ``r*M*beta`` term is nil.

"Gather then broadcast" is one operation, not two
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The modex today is *gather to the HNP* (``D*beta``, near-optimal) followed
by *broadcast from the HNP* (``d*r*D*beta``). The second half is the
entire cost. A direct allgather is ``D*beta`` in total. So this is **one
primitive**, and ``PRTE_RML_TAG_FENCE_RELEASE`` / ``PRTE_RML_TAG_GROUP_RELEASE``
should stop existing as broadcasts at all.

Cost model
----------

For ``N`` daemons, ``M`` bytes broadcast or ``D = N*n`` bytes
gathered, radix ``r``, depth ``d = ceil(log_r N)``, per-message latency ``alpha``
and ``beta`` seconds per byte (so ``alpha`` is latency and ``beta`` is inverse
bandwidth throughout):

.. list-table::
   :header-rows: 1

   * - Algorithm
     - Latency
     - Bandwidth
   * - broadcast, tree (today)
     - ``d*alpha``
     - ``d*r*M*beta``
   * - broadcast, scatter + RD-allgather
     - ``(d + log2 N)*alpha``
     - ``2*M*beta``
   * - allgather, gather+bcast (today)
     - ``2*d*alpha``
     - ``(1 + d*r)*D*beta``
   * - allgather, ring
     - ``(N-1)*alpha``
     - ``D*beta``
   * - allgather, recursive doubling / Bruck
     - ``log2(N)*alpha``
     - ``D*beta``

At ``N = 4096``, ``r = 64``, ``beta = 1 ns/B``,
``alpha = 50 us``, a 32 MB modex costs roughly 4.1 s on the tree and
33 ms via an RD-allgather. The launch message sees the same
``d*r -> 2`` improvement.

Two caveats belong with those numbers. ``xcast`` compresses its payload once
(``PMIx_Data_compress`` in ``xcast_nb``), so the ``d*r`` copies are of the
*compressed* bucket — a constant-factor discount on the tree's term, not a
change of shape. And ``alpha`` for the RML is a progress-thread hop, not
a raw TCP round trip, so latency terms are likely worse in practice than the
table suggests.

Why recursive doubling rather than a ring
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Recursive doubling (Bruck, for non-power-of-two ``N``) dominates the ring:
the same bandwidth term with ``log2 N`` steps instead of ``N-1``.
The ring's only genuine advantages are two lateral links instead of
``log2 N``, and contiguous data needing no final rotation — both cheap to
give up once the link machinery exists.

The decisive point is that the *large broadcast* is scatter-plus-allgather, so
**one movement strategy serves both primitives**. Keep the ring in reserve as
a fallback movement if the link count proves a problem at scale.

Lateral links cannot be avoided
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This looks avoidable and is not, so it is worth recording. The
``r*M*beta`` fanout is per-node-per-level. Pipelining hides the *depth*
but not the fanout. Dropping to radix 2 helps only if the *routing* tree is
radix 2, since a broadcast forwards to ``prte_rml_base.children``. And
scatter-then-reassemble requires the children to exchange directly with each
other. Every bandwidth-optimal collective needs non-tree edges.

Design
------

Piece 1 — lateral links in the RML *(landed:* ``f46e0d4371`` *)*
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The enabling mechanism for everything else. ``prte_oob_base_send_nb``
(``src/rml/oob/oob_base_stubs.c``) resolves a *next hop* via
``prte_rml_get_route``, so at radix 64 a daemon reaching any non-child goes
through the HNP. But the same function already fetches a peer's
``PMIX_PROC_URI`` from the modex — distributed to every daemon by
``process_wireup()`` — and builds a peer from it. If the hop *were* the
target, the existing code connects directly.

#. **Direct send.** A ``direct`` flag on ``prte_rml_send_t`` plus
   ``prte_rml_send_buffer_direct_nb()`` / ``PRTE_RML_SEND_DIRECT``; when set,
   ``prte_oob_base_send_nb`` uses ``msg->dst`` as the hop. Fall back to a
   routed send on ``PRTE_ERR_ADDRESSEE_UNKNOWN``.

#. **A lateral-link registry** on ``prte_rml_base`` — the ranks this daemon
   holds a non-tree link to, with a registrant callback.

#. **Lateral-link loss must not trigger tree repair.** ``prte_rml_route_lost``,
   ``prte_mca_oob_tcp_component_lost_connection`` and ``..._failed_to_connect``
   must consult the registry first: report to the registrant, drop the peer,
   **no promotion, no ancestor walk, no** ``COMM_FAILED``. This is the
   highest-risk item in the whole plan — "unreachable peer read as a lost
   lifeline" has bitten this code before. It lands with its own test, before
   anything uses it.

Items 1 to 3 landed.  Three more were deliberately deferred until a collective
actually opens a lateral link, because until then there is nothing to exercise
them against:

#. **Its own connect bound.** ``prte_connect_max_time`` exists so the tree can
   heal past a dead ancestor; a lateral link has no such fallback. Give
   registered links ``prte_lateral_connect_max_time`` and report a timeout to
   the registrant rather than abandoning silently.

#. **Pre-warm.** The Bruck partner set (``rank`` XOR ``2^k``) is fixed
   and computable, as is a ring. Warm it from ``vm_ready()`` using the existing
   ``PRTE_RML_TAG_WARMUP_CONNECTION``.

#. **Idle teardown**, so a long-lived DVM running many transient subset
   collectives does not accumulate sockets. Note the descriptor budget:
   ``log2 N`` per daemon, 12 at ``N = 4096``.

Piece 2 — broadcast: framing versus movement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**The framing is shared and stays as it is.** It is the hardest code in the
subsystem and none of it is about data movement: op-id assignment by the HNP,
the ``XCAST_ACK`` rollup with its ``is_request`` re-poll, the ``process_first``
set, late-joiner catch-up (``op_id_completed_at_promotion``), the promotion
replay hold (``replay_pending_parent``), the ``pending_completions`` FIFO, and
the fault-handler reactions to parent/children changes. One implementation,
movement-agnostic.

**Movement is pluggable**, chosen by the originator from measured payload size
and stamped on the wire. A broadcast has a single originator, so there is **no
agreement problem**:

``tree_whole``
   Today's behaviour: forward the whole payload to each routing-tree child.
   Tiny payloads.

``scatter_allgather``
   The root scatters chunks down the tree, then an RD/Bruck allgather over
   lateral links reassembles. Large payloads.

Two invariants govern the split.

*Ordering-critical traffic keeps* ``tree_whole``. The ``process_first`` set and
the forward-before-process rule exist to keep ``DAEMON_DIED`` /
``DAEMON_REVIVED`` ordered against everything else. Those are exactly the tiny
messages, so the constraint and the regime split agree rather than conflict.
Make it explicit: a payload whose delivery order is a correctness invariant may
not use lateral movement.

*Op-id order survives mixed movement.* A daemon processes ops in op-id order.
A bulk op on lateral movement can complete out of order relative to a tiny op
behind it, so ``process_msg`` must hold an out-of-order op until its
predecessors are processed — a generalisation of the existing
``replay_pending_parent`` hold. This is the main design risk on the broadcast
side.

*ACK semantics.* A daemon ACKs "my subtree has the payload". Under
``scatter_allgather`` completion is not subtree-shaped, so a daemon ACKs once
it holds the whole payload *and* its children have ACKed. The rollup itself is
unchanged.

**The tag should be the discriminator, not the size.** An earlier draft of
this document argued the opposite — that selection should read a measured
size, because otherwise "every future large payload needs a new tag to get the
benefit". That reasoning treats PRRTE as though it carried arbitrary traffic.
It does not: it carries a known, small set of things, and which of them a
message is *is* the operation. A new tag for a new bulk payload is not a cost
to be avoided, it is the declaration that makes the choice reasoned rather
than guessed.

The conflation is narrower than it looks, which is what makes this practical.
``PRTE_RML_TAG_DAEMON`` is the only overloaded tag, and within it exactly
**one** call site is large — ``plm_base_launch_support.c`` broadcasting
``jdata->launch_msg``. Everything else on that tag is a command: job-control,
halt/terminate, the allocation messages. Every other broadcast tag is already
single-purpose:

.. list-table::
   :header-rows: 1
   :widths: 34 12 54

   * - Tag
     - Size
     - Wanted
   * - ``PRTE_RML_TAG_DAEMON`` (launch message)
     - large
     - **its own tag** — then: bulk
   * - ``PRTE_RML_TAG_DAEMON`` (all other sites)
     - ~0
     - tree
   * - ``PRTE_RML_TAG_FILEM_BASE``
     - large
     - bulk; already its own tag
   * - ``PRTE_RML_TAG_WIREUP``
     - large
     - tree — ``process_first``, see below
   * - ``DAEMON_DIED`` / ``DAEMON_REVIVED``
     - ~0
     - tree — ordering-critical
   * - ``NOTIFICATION`` / ``MONITOR_REQUEST`` / ``IOF_PROXY``
     - ~0
     - tree

So splitting one tag off removes the need for a byte threshold entirely, and
the remaining selection is categorical: a small table of tag → movement, with
no number in it to be wrong about.

**Done.** ``PRTE_RML_TAG_DAEMON_LAUNCH`` carries the launch message, delivered
to the same handler on the same command stream — ``prte_daemon_recv`` ignores
the tag it arrives on, so nothing about the receiving side changed. Selection
is ``grpcomm_bcast_movement`` = ``tag`` (the default) / ``size`` / ``tree`` /
``bulk``, and ``bulk_tag_prefers_bulk()`` is the whole table.

``FILEM`` is **not** in that table, which corrects the operation list above.
Its chunks are capped at ``PRTE_FILEM_RAW_CHUNK_MAX`` = 16 KB, and at that
size the answer depends on the DVM: at a few hundred daemons the tree's
``r*M*beta`` fanout dominates and the exchange wins, at ten the exchange's
extra ``log2(N)`` latency steps cost more than the fanout saves. A knob-free
table has no business holding a guess, so scattering ``FILEM`` needs either a
larger chunk or a measurement first.

The framing/movement split landed first (``25416cf534``) with ``tree_whole`` as
the only movement.  What follows is the bulk movement itself, which is now
written; the notes below describe what was built and why, and call out the two
places where the implementation went further than the sketch.

Participants are named on the wire, never re-derived
''''''''''''''''''''''''''''''''''''''''''''''''''''

Every daemon must agree on which exchange position is which rank.  Deriving
that locally from the failed-daemon set would have daemons disagree in the
window around a fault, and the exchange would then deadlock — each waiting on
a partner the other does not believe exists.

So the **originator stamps the participant rank list** into the scatter
message, exactly as it stamps the movement id, and a daemon finds itself by
searching that list.  At four bytes a rank this is 16 KB for a 4096-daemon
DVM, carried once alongside a payload that is large by definition.  This rule
is load-bearing: do not replace it with a locally-computed set.

The two phases
''''''''''''''

**Scatter, down the existing tree.**  ``scatter_allgather``'s ``forward()``
sends each routing-tree child only the chunks destined for that child's
subtree (``radix_subtree_index`` partitions them), keeping its own.  No new
connections and no new tag: this *is* the forward, so it inherits the ACK
rollup, ``process_first`` and replay machinery unchanged.  Chunk boundaries
come from ``prte_grpcomm_chunk_bounds()``.

**Allgather, over lateral links.**  A new tag
(``PRTE_RML_TAG_XCAST_BULK``), driven by ``prte_grpcomm_bruck_step()`` over
positions, sending with ``PRTE_RML_SEND_DIRECT`` and registering each partner
through ``prte_rml_lateral_register()``.  Blocks land **rotated**, so
reassembly maps slots through ``prte_grpcomm_bruck_owner()`` — never assume
natural order.  Steps can arrive out of order, so blocks are stored by owner
position rather than appended, and the driver loops on "do I hold the blocks
this step must send" rather than advancing one step per message received.

Two things fell out of building it that the sketch did not anticipate.

*The exchange can outrun the scatter.*  The two phases travel different routes,
so a partner nearer the root can start its exchange before our scatter has
reached us — and its chunks name positions in a participant list we have not
been given yet.  Those messages are parked whole, by op-id, and drained when
the scatter arrives.  They cannot be decoded on arrival, and dropping them
would deadlock the exchange.

*Registered lateral links are never deregistered.*  Withdrawing the
registration while the socket is still open would make a later drop read as a
routing-tree fault, which is the one thing the registry exists to prevent.
Retiring the link and the registration together is the idle-teardown work,
which is still not done.

The framing change: ``payload_complete``
''''''''''''''''''''''''''''''''''''''''

Today the framing treats "op received" as "payload held".  Under
scatter+allgather it is not.  A ``payload_complete`` flag on the op is set by
the movement — immediately for ``tree_whole``, and when the last block lands
for the bulk movement — and both local delivery and the ACK to the parent gate
on it.  ``nexpected`` stays the child count, because the rollup is still
subtree-shaped; that is what keeps the reliability machinery single-copy.

Faults degrade to ``tree_whole``
''''''''''''''''''''''''''''''''

A participant lost mid-scatter or mid-allgather cannot be recovered by the
exchange itself.  The controller still holds the whole payload for its own op
and the framing already has a replay path, so on any fault touching an
in-flight bulk op the movement **flips to** ``tree_whole`` **and the op
replays whole**.  Receivers holding partial chunks have not processed yet
(``op->processed`` guards that), so they simply take the whole payload and
complete.  Correctness over speed during recovery, and no second recovery
mechanism to get wrong.

Selection
'''''''''

At the originator only.  ``grpcomm_bcast_movement`` takes four values:

``tag`` (default)
   The movement follows what the message is — ``bulk_tag_prefers_bulk()``,
   which today names only the launch message.  No constants.
``size``
   The old size rule: bulk iff the payload is at least
   ``grpcomm_bcast_bulk_min_bytes`` and the DVM at least
   ``grpcomm_bcast_bulk_min_daemons``.  Kept as an escape hatch for a
   programming model that pushes something unexpectedly large through a tag
   nobody classified — **not** the production path, because it holds a number
   nobody has measured.
``tree`` / ``bulk``
   Force one movement everywhere, for testing.

Ordering-critical tags are excluded before any of this, in every mode.

**The byte threshold was the only genuine "crossover" in the design, and the
tag split retired it rather than resolving it.**  It had existed only because
the launch message and the shutdown command shared ``PRTE_RML_TAG_DAEMON``, so
the operation could not be recovered from the call and size was the last
signal available.  To opt a new payload in, give it a tag and add it to the
table; do not reach for the threshold.

That is worth stating plainly because the two selections in this design are
*not* the same kind of thing, and describing them as though they were is how
a made-up constant acquires an air of having been measured:

.. list-table::
   :header-rows: 1
   :widths: 30 26 44

   * - Decision
     - Discriminator
     - What is unknown
   * - broadcast: tiny vs large
     - payload bytes (today)
     - a real crossover — until the launch message gets its own tag, at which
       point the question disappears rather than being answered
   * - broadcast: ordering-critical
     - tag
     - nothing; a correctness exclusion, not tuning
   * - fence: barrier
     - ``PMIX_COLLECT_DATA`` absent
     - nothing — the cost model says the high-radix tree wins at *any* scale
   * - fence: modex
     - ``PMIX_COLLECT_DATA`` present
     - not a crossover; only "does the exchange beat the rollup here", a
       yes/no with no number to tune

A size test survives, if at all, only as a backstop for a programming model
that pushes something unexpectedly large through a tag nobody classified.

``PRTE_RML_TAG_WIREUP`` is excluded even though it is large and would
otherwise qualify, because it is in the ``process_first`` set — it changes the
child set, so it must be processed before forwarding.  That exclusion is
deliberate and must stay commented in the code, or it will be "optimised"
back in.

Piece 3 — allgather: framing versus movement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Built.** ``tree_gather_release`` and ``rd_allgather``, selected by
``grpcomm_fence_movement`` (``tree`` / ``allgather`` / ``auto``), defaulting to
``tree``.  Two things about the built shape differ from the sketch below and
are worth stating, because both were discovered by writing it:

*The seam is wider than "how it is released."*  An allgather changes what
*converged* means, not just what to do about it — the rollup is counting child
subtrees while the exchange is counting blocks — so a movement supplies three
things: ``converged``, ``contribute``, and ``answer``.  The framing keeps the
converged latch, the deadline, and everything about the tracker's lifetime.

*The exchange opts out of the recovery restart entirely.*  The restart exists
because a rollup's shape is the routing tree's and the tree just changed; an
exchange's shape is the participant list, which a repaired tree does not
touch.  Blocks already collected are still exactly what their owners
contributed, so re-offering our own would only duplicate what partners already
hold.  A participant genuinely lost is handled instead by the local fault
test, which ends the fence.

The shared framing is what ``grpcomm_fence.c`` already factors
out: the signature, the tracker, ``create_dmns()``, ``get_tracker()``,
``my_contribution``, the recovery epoch, ``abort_fence_op()``, and the
completion callback into PMIx. Movement is pluggable:

``tree_gather_release``
   Today: up-tree rollup, ``xcast`` release. Zero-payload barriers keep this;
   it is already optimal for them.

``rd_allgather`` (Bruck for non-power-of-two ``N``)
   ``log2 N`` lateral exchanges, every daemon ending with everything,
   **no release broadcast**. Large payloads.

**Ordering.** Blocks are stored by participant position and concatenated in
ascending order, so every daemon produces byte-identical output — better than
today, where the bucket order is the nondeterministic merge order at the root.

**Fault.** Simpler than the tree path. On the GLOBAL-scope pass a participant
tests its own ``coll->dmns`` against ``prte_rml_base.failed_dmns``; any
participant lost means the exchange cannot close, so it completes local
participants with ``PMIX_ERR_LOST_CONNECTION`` and deletes the tracker. Purely
local on every daemon — no controller, no epoch restart. There is no "lost a
pure relay" case at all, because relay-only daemons are not in the exchange.

**Timeout** becomes local for the same reason: there is no root, and for a
subset fence the master may not participate. A participant carrying
``PMIX_TIMEOUT`` arms its own timer and on firing broadcasts the abort so the
rest stop. This is a deliberate deviation from today's rule that the fence's
deadline is the DVM's to keep.

**Agreement.** Unlike the broadcast, every participant must independently
choose the same movement or the collective hangs. See the next section.
Regardless of how that resolves, the movement id goes on the wire and a
receiver whose tracker is running a different movement **aborts with a named**
``show_help`` **diagnostic** — turning the one catastrophic failure mode of the
design into a reportable error.

Piece 4 — getting the collect-data directive from PMIx
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**This piece turned out not to exist.  The directive was already arriving.**

The premise recorded here — that ``PMIX_COLLECT_DATA`` is consumed
client-side and the host sees only ``data``/``ndata``, so PMIx would have to
be changed to pass it up — is false, and was checked rather than reasoned
about.  The PMIx client packs the caller's ``info`` array onto the wire
verbatim; the server unpacks it into ``trk->info`` and hands *that* array
straight to ``pmix_host_server.fence_nb``.  So the directive reaches
``pmix_server_fencenb_fn()`` like any other.

Measured on a three-node DVM, printing every key the upcall received:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Fence
     - ``pmix.collect`` present?
     - ``ndata``
   * - ``PMIx_Fence`` with ``PMIX_COLLECT_DATA``
     - **yes**
     - 8
   * - ``PMIx_Fence`` with no directives (a barrier)
     - no
     - **8**

The second row also disposes of the fallback this section proposed.
``ndata == 0`` is not "realistically equivalent to no-collect": a pure
barrier arrives with **eight bytes**, not none, so a size-based rule would
have sent every barrier down the exchange — the opposite of what it was for.
Do not reintroduce it as a heuristic anywhere.

What was actually needed is therefore small, and entirely inside PRRTE: read
``PMIX_COLLECT_DATA`` out of the info array in the fence entry point and let
it choose the movement.  No openpmix change, no ``PMIX_CAP_*`` flag, no
``PRTE_CHECK_PMIX_CAP`` in ``config/prte_setup_pmix.m4``, and nothing to
guard with ``#if`` — there is no older PMIx to degrade against, because
nothing about this is new.

That also makes the fence's ``auto`` **better founded than the broadcast's**,
rather than worse.  A broadcast's ``auto`` is safe only because a broadcast
has one originator whose choice is authoritative.  A fence has none — but
every participant's upcall carries the same directive, because PMIx requires
it to be uniform across a fence and enforces that within a node.  So every
daemon resolves ``auto`` identically from an input none of them had to
agree on.

The wire interlock still matters and is unchanged: it is what catches a
genuinely non-uniform request, and it turns it into a named ``show_help``
diagnostic rather than a DVM-wide hang.

What the numbers turned out to be
---------------------------------

Measurements taken while the movements were still in the tree, kept because
they are about PRRTE rather than about the movements, and because two of them
correct claims made earlier in this document.  Where a figure could only be
produced by code that has since been removed, it says so.

How big is a real transport's blob?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The whole cost model turns on the per-rank modex contribution, and this
document quoted no figure for it.  Neither transport that matters on a real
machine could be measured here — the containers have no fabric — so these are
read out of the source.

**OFI is tens of bytes, and fixed.**  ``mtl/ofi`` publishes exactly one
endpoint address: ``opal_common_ofi_fi_getname()`` calls ``fi_getname()`` and
the result *is* the modex value.  ``btl/ofi`` publishes a four-byte count
plus one length-prefixed address per module.  The address sizes are provider
constants: EFA is ``EFA_EP_ADDR_LEN`` — a 16-byte GID plus qpn, pad and qkey;
tcp/sockets is ``FI_SOCKADDR``, i.e. a 16-byte ``sockaddr_in``.  So OFI sits
in the same band as the TCP BTL, and it does not grow with anything.

**UCX is the opposite, and is the interesting case.**  ``pml/ucx`` publishes
the entire ``ucp_worker`` address — and publishes it *twice*, once at
``PMIX_LOCAL`` with full flags and once at ``PMIX_GLOBAL`` with
``UCP_WORKER_ADDRESS_FLAG_NET_ONLY``; only the second is what a remote peer
fetches.  Its size comes from ``ucp_address_packed_size()``:

* a 1-2 byte header, plus an 8-byte worker UUID and an 8-byte client id when
  those flags are set;
* **per device**: md index, the device address and its length, optionally a
  path count and a system-device byte.  An IB device address is a base struct
  plus a 2-byte LID (plus an 8-byte GUID and a 2- or 8-byte subnet prefix),
  or a full 16-byte ``ibv_gid`` on RoCE;
* **per transport lane on that device**: a 2-byte transport-name checksum,
  the interface address and its length, a packed interface-attribute struct,
  and ``ep_addr_len`` plus a lane byte for each lane.

The property that matters: **it scales with devices times transports, not
with job size**.  A single-rail node with two or three network transports is
in the low hundreds of bytes; multi-rail, or a worker that also carries GPU
transports, reaches a kilobyte and beyond.  That is one to two orders of
magnitude above OFI, and it is per rank.

So the range worth testing is roughly 64 bytes to a few kilobytes a rank.
``scaletest --sizes`` sweeps exactly that.

Most of a modex fence's cost is not the bytes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This was a surprise, and it is the most useful thing measured here.  At 8
nodes and **64 bytes a rank** — OFI/TCP territory, 512 bytes of modex in the
*entire job* — the tree fence spent 877 µs against a bare barrier's 398 µs on
the same tree.  A 479 µs premium for half a kilobyte is not a bandwidth
story.

What it is paying for is the **release traversal**: a second trip down the
tree carrying a payload.  That term is why the fence's cost does not collapse
as the payload shrinks, and it is where a future win has to come from.  Note
the consequence for the open question at the end of this document: a
payload-aware radix or a chunked release attacks exactly this term, and does
so without lateral links.

And there *is* a crossover, which matters
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``scaletest --neighbors`` prices the other end of the trade: a fence that
collects nothing, followed by direct-modex gets of just the two ring
neighbours.  Measured on the tree-only build, 8 nodes, one proc per node,
median of three to five iterations — ``NEIGHBORS`` against the ``COLLECT`` of
a separate plain run at the same size:

===============  ==========  ============  =======
Bytes per rank   COLLECT     NEIGHBORS     ratio
===============  ==========  ============  =======
64 (1 key)       877 µs      1000 µs       1.14
8 KB (8x1 KB)    2433 µs     1455 µs       0.60
64 KB (8x8 KB)   5947 µs     3660 µs       0.62
===============  ==========  ============  =======

**At 64 bytes a rank, fetching two peers on demand is more expensive than
collecting the whole job.**  Two client round trips cost more than the extra
half-kilobyte on the tree did.  The advantage appears once the per-rank
contribution is kilobytes rather than tens of bytes — which is exactly the
OFI-versus-UCX split sized above, and it means the per-rank modex size is a
real variable and not one to wave away.

An earlier draft of this document asserted the opposite — that the on-demand
side was already ahead at 64 bytes a rank, so no crossover existed.  That
came from a configuration this tree no longer has, and the measurement above
replaces it.  Nothing in the tree serves the neighbour pattern specially;
these are the figures a proposal to do so would have to be argued from, and
they say such a proposal has to name its payload regime.

``PMIX_COLLECT_DATA`` is a sufficient discriminator
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two passages elsewhere in this document argued that the fence's selection
needs something finer than ``PMIX_COLLECT_DATA``.  Both were wrong, and the
correction matters because that flag is what the current default rests on.

The first leaned on a barrier regression as if the selection would inherit
it.  It would not: that number came from *forcing* a movement onto every
fence, while a selection gives a barrier the rollup, because a barrier
carries the flag false.  Open MPI's own post-modex hard barrier sets it false
explicitly, and ``MPI_Finalize`` passes no info array at all.

The second claimed the flag says nothing about how much data there is,
implying a size threshold was wanted.  That one is **not** refuted, and an
earlier draft of this section wrongly said it was: the crossover measured
above is real, so a design that changes what a fence delivers does have a
payload regime where it loses.  What the flag settles is the
barrier-versus-modex question, which is categorical and is all the current
default asks of it.  Anything that also wants to decide *how much* data
justifies a different treatment needs its own input, and would have to
measure the constant rather than inherit one.

It is also the only trustworthy signal available.  **"Is ``ndata`` zero" is
not** — a bare barrier arrives at the upcall with eight bytes of info — so
nothing here argues for a separate barrier entry point.  Do not reintroduce
either heuristic.

What a real MPI job says, and it is not what the benchmark says
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``scaletest`` contributes 8 KB to 512 KB a rank because it was written to
make the collective visible.  Open MPI built into the same swarm
(``OMPI_SRC``, see the harness guide) says something different, and it is
worth stating plainly because it cuts against the benchmark.

**At 16 ranks Open MPI's entire modex is 1319 bytes** — about 82 bytes a
rank, measured with ``grpcomm_base_verbose 5``.  ``MPI_Init`` took 26-29 ms
and ``MPI_Finalize`` 43-47 ms, indistinguishable across three runs.  At 64
ranks over 16 nodes, 93-102 ms.  This Open MPI has no UCX and no OFI (the
container has neither), and those are exactly the transports that publish a
large per-rank blob — so the honest statement is that collective changes are
invisible on a small TCP job, and any case for one rests on jobs whose
per-rank contribution is large, whose rank count is large, or both.  The
benchmark is not wrong; it is measuring a regime this particular MPI job is
nowhere near.

**And a bare MPI job measures none of this**, which is the trap worth
recording.  Open MPI resolves a remote peer the first time it talks to it:
``mpi_add_procs_cutoff`` defaults to 0 so the pre-add-everybody branch never
runs, and ``ob1`` demands the whole world only when a BTL declares
``MCA_BTL_FLAGS_SINGLE_ADD_PROCS``, which TCP does only with more than one
interface plus threads.  ``MPI_Init`` adds the node-local peers and nothing
else.  So ``MPI_Init``/``MPI_Finalize`` with nothing in between never touches
a remote peer, and cannot tell two ways of distributing the modex apart —
which is what a first attempt at this measurement duly reported, and the
reading "the on-demand path costs nothing" was an artifact of measuring
nothing.  ``mpinoop --ring``/``--all`` exist because of that; see the harness
guide.

**On the tree-only DVM the direct-modex count does not move with the pattern,
and should not be expected to.**  Measured at 8 ranks over 8 nodes, the
``DMODX REQ FOR`` count is 7 under a bare run, under ``--ring`` and under
``--all`` alike — and those seven are not first touch: they are one request
per non-master daemon for **rank 0**'s ``pml.base.2.0``, issued by Open MPI's
PML selection before the ``MPI_Init`` fence completes.  First touch is
answered locally because that fence collects, so every daemon already holds
every rank's data.  A configuration in which the count tracks the
communication pattern is one where the fence withheld data — which is what
the withdrawn movements did, and nothing in the tree does now.  Read these
flags for their *timings*, not for modex traffic.

Counting the requests at all needs ``--leave-session-attached``, or only the
master daemon's traces arrive: the same run reports **0 without the flag and
7 with it**.  Zero is the dangerous reading, since it is also exactly what a
genuinely local answer looks like.

What a client actually asks about a remote peer
-----------------------------------------------

This section is not about a movement, which is why it survives the reset
above.  It is about a requirement every design in this document took as
given, and which had never been checked.

Every daemon ends up holding a *complete* copy of the launch payload because
``prte_pmix_server_register_nspace`` publishes a ``PMIX_PROC_INFO_ARRAY`` for
every proc on every daemon, so that any daemon can answer a ``PMIx_Get``
about any rank.  That is an assumption about what clients ask for.  If it is
wrong, the largest routine payload the DVM moves is mostly being delivered to
daemons that will never be asked about it — and that is true whatever route
it travels on.

The launch message has just been made much smaller (PRRTE #2628): a job's
placement travels as a node map plus one proc map per app rather than a
record per process, taking it from ~46 to ~13 bytes a proc — 5.8 MB to 1.6 MB
at 131,072 procs.  What is left divides cleanly in two, and only one half is
broadcast-shaped:

* **The job-level fields and the maps.**  O(nodes), compressed, and genuinely
  identical for every daemon — rank-to-node is what any daemon answers a
  ``PMIx_Get`` about any rank with.
* **The per-proc residual array** — node rank, cpuset, state, attributes.
  O(total procs), and now the only part that scales with the job.  But a
  daemon needs a proc's cpuset and node rank in order to *fork* it, which is
  only true of the procs it hosts.

The scan
~~~~~~~~

Scanned across all of ``ompi/``, ``opal/`` and ``oshmem/`` in the Open MPI
``main`` tree, excluding ``3rd-party/``, for any ``PMIx_Get`` of a
``PMIX_``-prefixed key naming a proc that can be off-node.  Reserved keys
only — user modex keys (``OMPI_ARCH``, the BTL endpoint blobs) are the
direct-modex traffic that already works this way and are not in question.

**Non-optional, and genuinely remote: exactly one.**
``ompi/mca/topo/treematch/topo_treematch_dist_graph_create.c`` asks
``PMIX_NODEID`` for every rank in the communicator, so it *would* issue a
direct modex per peer if the answer were not held locally.

**Optional, so they cannot issue one at all.**  ``PMIX_OPTIONAL`` tells the
client library to answer from local data or fail; it never reaches the
server.  That covers ``PMIX_HOSTNAME`` (``opal_get_proc_hostname()`` and
``pml_base_select``) and the four ``PMIX_LOCALITY`` sites in
``ompi/proc/proc.c`` and ``ompi/communicator/comm.c``.

**And ``PMIX_LOCALITY`` never crosses the wire in either direction.**  Open
MPI *computes* it from each local peer's ``PMIX_LOCALITY_STRING`` and stores
it client-side with ``PMIx_Store_internal``.  A get naming a remote proc
misses and falls back to ``OPAL_PROC_NON_LOCAL``, which is the right answer
anyway.  Nothing in PMIx stores that key and nothing in PRRTE publishes it.

**Sites that look remote and are not**, which is most of them:

* ``btl/sm``'s ``PMIX_LOCAL_RANK`` — ``btl/sm`` only ever handles procs on
  this node.
* ``common_ofi``'s ``PMIX_LOCALITY_STRING`` and ``PMIX_PACKAGE_RANK`` — it
  walks the ``PMIX_LOCAL_PEERS`` list, so local by construction.
* The ~20 gets in ``ompi/runtime/ompi_rte.c``, and everything in
  ``opal/mca/hwloc/base/hwloc_base_util.c``, ``comm_init.c`` and
  ``btl/smcuda`` — all either self or a wildcard rank, i.e. job-level.
* ``ompi/dpm/dpm.c`` asks ``PMIX_LOCAL_PEERS`` and ``PMIX_LOCALITY_STRING``
  ``IMMEDIATE``, but about a *connected* namespace — a different job, which
  arrived by connect/accept rather than by a launch message.

The answers come from the maps, not from the per-proc array
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every key on that list is derived by PMIx from the node map and the proc map.
``pmix_gds_hash_store_map()`` walks the two together and stores, for each
rank, ``PMIX_HOSTNAME`` (the node the rank appeared under), ``PMIX_NODEID``
(that node's index in the map), ``PMIX_LOCAL_RANK`` (the rank's position in
its node's list) and ``PMIX_NODE_RANK``.

That third one is worth pausing on: it is the **same derivation** PRRTE's own
``prte_job_unpack`` now performs, arrived at independently from
``compute_local_rank()``.  PMIx has been computing local rank from the proc
map all along.

Two riders, both material:

* **It is the fallback, and it is now a per-key one.**  PRRTE supplies a
  per-proc array, so PRRTE's values win today.  That used to be
  all-or-nothing — a single ``PMIX_PROC_INFO_ARRAY`` anywhere set
  ``PMIX_HASH_PROC_DATA`` and suppressed the derivation for the *entire job*.
  It is not any more: ``store_derived()`` asks, per rank and per key, whether
  the host already said this, and fills in only what was left unsaid.  **That
  removes the obstacle to splitting the payload**: a daemon could be sent
  proc records for the procs it hosts and nothing else, and PMIx would derive
  hostname, nodeid and local rank for every other rank from the maps, with no
  direct modex and no loss.
* **PMIx's node-rank fallback is wrong, and says so.**  It sets
  ``node_rank`` equal to the local rank with the comment *"for now, we assume
  only the one job is running"*, which breaks when two jobs share a node.
  That is exactly the one field PRRTE found it could not derive either.  The
  two analyses agree on which field is the exception.

What this implies
~~~~~~~~~~~~~~~~~

**Nothing in Open MPI asks a remote proc for ``PMIX_CPUSET``, and nothing
asks a remote proc for ``PMIX_NODE_RANK``** — the only node-rank get is about
self.  Those two are the residual array.  On this evidence, broadcasting the
residuals is moving data no MPI process asks for about a remote peer.

The shape that follows is **broadcast the maps, scatter the residuals**: the
job-level fields and maps go to everyone as they do now, and each daemon
receives only the per-proc records for the procs it will fork.  Per-daemon
launch cost becomes O(nodes) + O(ppn) — *constant in job size* — and what is
left to broadcast is O(nodes), which is exactly what the tree is best at.

Note this is a change to what is *addressed to whom*, not a movement: it does
not need lateral links, and nothing in it revives what the reset removed.  A
per-destination payload is a different contract from a single payload
delivered identically to all, and that contract is the work.

Limits of this evidence
~~~~~~~~~~~~~~~~~~~~~~~

Stated rather than buried, because the conclusion is only as good as the
scan:

* **It is Open MPI.**  OpenSHMEM rides the same runtime layer
  (``oshmem/proc/proc.c`` asks only a wildcard ``PMIX_LOCAL_PEERS``), but
  other PMIx clients exist and were not scanned.
* **The treematch ``PMIX_NODEID`` get is non-optional.**  Under the maps it
  is answered locally, but it is the one site where being wrong shows up as a
  round trip per peer per communicator rather than as a fallback.
* **"Nothing asks" is weaker than "the answer would be right."**  If PRRTE
  stops supplying ``PMIX_NODE_RANK`` for remote procs, a remote get falls
  through to PMIx's one-job-per-node assumption and receives a *wrong* value
  rather than nothing.  Node rank is two bytes; if that matters, keep sending
  it to everyone on its own rather than relying on nobody asking.

Verification
------------

* **Unit** — ``test/unit/grpcomm/test_grpcomm.c``: partner-set derivation for
  RD/Bruck over the input matrix including non-power-of-two ``N``, the
  daemon-job NULL-array case and elastic vpid holes; movement-selection
  determinism; canonical block assembly producing identical bytes from any
  arrival order; scatter chunking round-trip.
* **Unit** — ``test/unit/rml/test_rml_routing.c``: a registered lateral link is
  classified as neither child nor lifeline, and its loss produces no tree
  repair.
* **Build** — ``--enable-debug`` (warnings as errors) clean, including the
  capability-guarded path both ways; ``make check``.
* **Multi-node** — ``contrib/dockerswarm``, run with
  ``--prtemca rml_base_radix 2`` so the tree is deep and lateral links are
  provably not tree edges. A large launch and a ``FILEM`` preload complete with
  every daemon holding identical bytes; a daemon dies mid-broadcast and the DVM
  survives; ``DAEMON_DIED`` ordering holds against a concurrent bulk broadcast;
  the movement-mismatch interlock fires cleanly when forced. A/B timing of each
  movement at several payload sizes.
* **Bisectability** — each piece must pass the full suite on its own before the
  next lands.

The multi-node baseline is **562 passed, 0 failed, 3 skipped** once this
work's own phases are counted.

**The performance question now has a tool.**  Everything above establishes
correctness; none of it says either movement is *faster*, and this document has
until now described that measurement as needing hardware nobody had.
``contrib/dockerswarm/scaletest.sh`` is that vehicle: it stands up its own
larger swarm and times a full-data ``PMIx_Fence`` against a bare barrier while
sweeping DVM size, procs per node, routing radix and payload size, writing a
CSV.  Those are exactly the two arms of the fence's selection, so
``grpcomm_fence_movement tree`` against ``allgather`` over the same sweep is a
direct A/B; ``grpcomm_bcast_movement tree`` against the default does the same
for the launch message.  What it still cannot supply is a real network — the
containers share a host, so the bandwidth term is not a cluster's — but it can
answer the shape question, which is what the defaults rest on.

Two practical notes, both of which have cost time here:

* ``check_PROGRAMS`` are built by ``make check``, **not** by ``make``.  Running
  a unit-test binary straight after ``make`` silently runs a stale one, and a
  newly added case simply does not appear in the output.
* For anything that changes the wire format, the multi-node run is
  load-bearing rather than a formality: it is what proves every daemon,
  including pure relays, agrees on the new layout.
* A phase usually starts **one** DVM and runs several cases against it, and
  ``cleanup_swarm`` ends it.  Inserting a self-contained case in the middle of
  such a phase leaves the later cases with no DVM, which presents as
  ``prun failed to initialize`` and reads like a collective failure.  New cases
  go after the last one that needs the shared DVM.

Open questions
--------------

#. **Is the lateral-link registry enough** to keep a dropped lateral link from
   ever being read as a lifeline loss? This is the one place where getting it
   wrong produces DVM-wide damage rather than a slow collective.
#. **Mixed-movement op ordering** — the hold described in Piece 2 needs the
   multi-node ordering case before it can be trusted.
#. **Descriptor budget at scale** for ``log2 N`` lateral links per
   daemon. The ring movement (two links) is the fallback.
#. **A cheaper win exists and should be measured alongside.** Much of the
   tree's cost is the ``d*r`` release fanout. A payload-aware radix for the
   release, or a chunked/pipelined ``xcast``, recovers a large fraction of the
   benefit with none of this risk.

   This remains **unmeasured**, and it is the largest open risk in the plan.
   The cost model above is standard (Thakur, van de Geijn), but PRRTE's own
   constants are not: ``alpha`` here is a progress-thread hop rather than a
   raw round trip, and ``xcast`` already compresses, which discounts the
   tree's bandwidth term by an unknown factor.  A measurement needs real
   multi-node hardware at realistic scale — ten containers on one host tell
   you nothing useful about either constant.  The seam and both halves of the
   arithmetic are already in place, so instrumenting an A/B of the two
   movements is cheap once such a machine is available.
