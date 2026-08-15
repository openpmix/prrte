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

It is also the only trustworthy signal available.  **"Is ndata zero" is
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

**Measuring on-demand retrieval requires turning Open MPI's collecting fence
off** — ``OMPI_MCA_pmix_base_collect_data=0`` — and this is the step that is
easy to omit and impossible to detect from the result.  At the default the
``MPI_Init`` fence collects, so every daemon already holds every rank's data,
first touch is a local hit, and the ``DMODX REQ FOR`` count sits flat at 7
across a bare run, ``--ring`` and ``--all`` alike.  Those seven are not first
touch at all: they are one request per non-master daemon for **rank 0**'s
``pml.base.2.0``, from PML selection.  A count that does not move with the
communication pattern means the fence collected, not that resolution is free.

With the fence off, the counts are exact, and they are the arithmetic the
probe was built to expose — *distinct peers each rank touches × nprocs*, at
8 ranks over 8 nodes:

==========  =======  ==========  ==========
Mode        DMODX    touch       repeat
==========  =======  ==========  ==========
baseline    0        0.000 ms    0.000 ms
``--ring``  32       9.583 ms    0.027 ms
``--all``   56       6.343 ms    0.019 ms
==========  =======  ==========  ==========

``--all`` is 7 peers × 8 = 56.  ``--ring`` is **4** per rank rather than 2:
rank 1 fetches ``{0,2}`` for the ring itself **union** ``{0,3,5}`` for the
lining-up barrier's recursive-doubling partners.  That confirms, in the
counts, the caution recorded in ``mpinoop``'s own header — the barrier beside
a phase resolves ``log2(N)`` partners of its own, and they are charged to the
barrier rather than to the pattern under test.  The key fetched is
``btl.tcp.6.1``, the BTL endpoint blob, so this is genuine peer resolution.

Note what the timings then say: first touch costs 9.6 ms against a 0.03 ms
repeat.  Resolution, not communication, is what the first exchange pays for.

Counting the requests at all needs ``--leave-session-attached``, or only the
master daemon's traces arrive.  The ``--all`` run above reports **7 without
the flag and 56 with it** — an eighth of the truth, and 7 is exactly the
number a *collecting* fence produces, so the under-count does not even look
wrong.  Both mistakes have to be ruled out before a number here means
anything.

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

**And PMIX_LOCALITY never crosses the wire in either direction.**  Open
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

**Nothing in Open MPI asks a remote proc for PMIX_CPUSET, and nothing
asks a remote proc for PMIX_NODE_RANK** — the only node-rank get is about
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
* **The treematch PMIX_NODEID get is non-optional.**  Under the maps it
  is answered locally, but it is the one site where being wrong shows up as a
  round trip per peer per communicator rather than as a fallback.
* **"Nothing asks" is weaker than "the answer would be right."**  If PRRTE
  stops supplying ``PMIX_NODE_RANK`` for remote procs, a remote get falls
  through to PMIx's one-job-per-node assumption and receives a *wrong* value
  rather than nothing.  Node rank is two bytes; if that matters, keep sending
  it to everyone on its own rather than relying on nobody asking.

What Slurm's PMI2 does, and what it tells us
--------------------------------------------

Slurm's ``src/plugins/mpi/pmi2`` is an independent implementation of the same
problem that runs at production scale, so it is worth being precise about what
it does differently.

**Its KVS fence is our tree fence, with the same asymptotics.**  ``kvs.c``
merges up the stepd tree and the root then does one
``slurm_forward_data(step_nodelist, ...)`` of the whole merged KVS to every
node — O(N·b) delivered per node, which is ``tree_gather_release``.  Slurm did
not make the allgather scale.

**What scales is that MPI stops asking for one.**  ``ring.c`` implements
``PMIX_Ring``, which hands each process only its left and right neighbour
values: O(1) per process at any N.  It is an up-sweep/down-sweep scan —
RING_IN carries (count, left, right) up, and the root sends each child a
*different* RING_OUT giving that subtree's starting rank and its own
neighbours.  The argument for it is "PMI Extensions for Scalable MPI Startup"
(Chakraborty et al., EuroMPI/ASIA 2014): with a ring plus on-demand connection
establishment, the business-card allgather is not needed at startup at all.
PRRTE's equivalent lever is the direct modex, not a faster ``rd_allgather``.

Three things transfer.

**A fence sequence number, which they derive locally.**  ``kvs_seq`` starts at
1, is incremented in ``temp_kvs_send()`` ("expecting new kvs after now"),
travels on the wire in both directions, and is checked on arrival.  Our fence
signature carries no such number: a fence is identified by its participant
list alone, and a job fences over the same list repeatedly.  What we have
instead is ``reported_slots``, a bitmap of which child subtrees have reported,
which makes a *duplicate* harmless but cannot tell a duplicate of the current
round from a straggler of an older one.

They treat any mismatch as fatal, and can afford to because their rollup has
no traffic that arrives out of turn.  That is our position too, today: with
the release travelling down the tree and nothing travelling across it, a
contribution cannot overtake the release that ended the previous fence.  The
gap is worth writing down anyway, because it is latent rather than absent.  A
contribution naming a generation we had already retired would match no
tracker, and ``get_tracker(sig, true)`` would build one — a tracker nothing
will ever complete or delete.  Nothing can produce that arrival on one route.
Anything that introduces a second one has to add the sequence number first.

**A scan is a shape we do not have.**  Every message in ``ring.c`` is O(1) no
matter how large the job, because each child is sent only what its own subtree
needs.  Our release broadcasts the whole result to everyone.  For any
operation where a participant needs a slice rather than the aggregate — rank
assignment, neighbour exchange, or the per-daemon half of a launch message —
that is the difference between O(1) and O(N) per daemon.

**Their state reset is synchronous with the send.**  ``pmix_ring_out()`` sends
to its children and then clears the per-child slots and the count inside the
same handler, so no next-round message can attach to the previous round's
state.  That is the same rule as retiring a tracker before delivering its
release, reached from the other direction.

Two things not to copy: every error path calls
``slurm_kill_job_step(SIGKILL)`` — there is no fault tolerance in the
collective at all, which is most of why their code is smaller than ours — and
only one ring may be in flight, with state in a fixed per-child array indexed
arithmetically, because they have no notion of a collective over a subset.
Our tracker identity machinery is the price of semantics they do not offer,
not accidental complexity.

Scattering the fence release: measured, and not worth it as a tag
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ring.c``'s per-child down messages prompt an obvious question about ours.
The release broadcast is the one routinely large message a fence sends, and it
goes out whole to every child at every level.  ``PRTE_RML_TAG_DAEMON_LAUNCH``
exists so that a broadcast's *purpose* is declarable where it is sent, rather
than guessed at from how big it happens to be; giving the fence release the
same treatment was tried.

The numbers below were taken while the movements were still in the tree, and
the barrier column in particular reflects code that has since been removed.
They are kept for the structural finding, which does not depend on any of it.
Ratios are with the release preferring a per-child send, against the same run
without:

===================  ==================  ==================
payload              collect (modex)     barrier
===================  ==================  ==================
8 KB/rank, N=8       0.93x               1.18x
8 KB/rank, N=16      1.17x               1.39x
512 KB/rank, N=16    0.94x               1.50x
===================  ==================  ==================

The modex gained at most 6% and was not reliably better at all below a
megabyte, while the barrier — which shares the tag — lost 18–50%, and the loss
grew with N because it was paying a large message's fixed costs to move 157
bytes.

The structural finding is the useful part, and it survives the movements being
gone: **the fence release is the one broadcast whose tag does not determine
its size.**  The premise that a tag declares a purpose holds for the launch
message and fails here, because a barrier's release and a modex's release
travel the same tag six orders of magnitude apart.  That is precisely the
situation that splitting the launch message onto its own tag was meant to
remove.  So if this is ever worth doing, the *fence* must say so — it knows
which kind of release it is emitting — by plumbing a preference through
``prte_grpcomm_release_bcast``, rather than by a table inferring one from the
tag.

Read the 6% as a lower bound rather than a verdict: the measurement is
loopback between containers on one host, which is precisely the environment
where a bandwidth optimisation shows least.

Reassessing the allgather, after the reset
------------------------------------------

Everything above is a record of what was built and why it came back out.  This
section is a fresh look at the underlying question — *is there a better
allgather for the modex, and is it worth building* — taken on tree-only
master.  It reaches a different ordering than the plan did, it corrects one
thing said about Slurm, and it names the one measurement that has to happen
before any of the rest is worth arguing about.

Two Slurm collectives, and only one of them is an allgather
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

It is easy to remember Slurm as "a modified ring for PMI-2 and a pure ring for
PMIx" and take both as allgather algorithms to compare against.  Only the
second is one, and the distinction is the most useful thing in the comparison.

``src/plugins/mpi/pmi2/ring.c`` implements ``PMIX_Ring``, and **it is not an
allgather at all**.  It is an up-sweep/down-sweep *scan*: RING_IN carries
(count, left, right) up the stepd tree, and the root sends each child a
*different* RING_OUT giving that subtree's starting rank and its own two
neighbour values.  Every message is O(1) no matter how large the job, because
each child is sent only what its own subtree needs.  The argument for it is
"PMI Extensions for Scalable MPI Startup" (Chakraborty et al., EuroMPI/ASIA
2014): given a ring plus on-demand connection establishment, the business-card
allgather is not needed at startup at all.  Slurm's PMI-2 *fence* — ``kvs.c``
— is still a tree gather plus a whole-KVS ``slurm_forward_data`` to every
node, which is exactly ``tree_gather_release`` with exactly our asymptotics.

So Slurm's scalable answer at the PMI-2 layer was **to change the interface so
that MPI stops asking for an allgather**, not to make the allgather faster.

Their PMIx plugin is the case that *is* a genuine ring allgather —
``pmixp_coll_ring.c``, a contribution ring in which each node forwards blocks
around the ring over N-1 steps, added alongside the existing tree collective
rather than replacing it.  The detail worth chasing, if this is ever revisited,
is that it carries explicit per-collective *ring contexts* keyed by a sequence
number so that consecutive and overlapping collectives cannot be confused —
which, if so, means Slurm hit precisely the defect that killed the movements
here and answered it with a context id rather than by abandoning laterals.

.. note::

   The ``pmixp_coll_ring.c`` description is from recollection and has **not**
   been read against Slurm's source in this tree.  ``contrib/slurmswarm``
   builds Slurm from source, so it is checkable; verify before leaning on it.

Level 0 — the wall that no algorithm moves
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Before comparing schedules it is worth being blunt about what none of them
change.  A full modex moves ``Θ(N²n)`` bytes across the DVM and delivers
``D = N*n`` bytes **into every daemon**.  That is a property of the operation,
not of the algorithm: an allgather's output is the concatenation of its
inputs, and every participant is required to hold all of it.

Put the numbers in.  Ten thousand nodes, 128 procs a node, a UCX-sized 1 KB a
rank: ``n`` is 128 KB a daemon and ``D`` is **1.28 GB**, which every daemon
must receive.  At 10 GbE that is a floor of about **one second**, with a
perfect algorithm and an idle network.  Today's tree at radix 64 is depth 3,
so its release term ``d*r*D*beta`` is around **200 seconds** — two orders of
magnitude above the floor, discounted by whatever ``xcast``'s single
compression pass buys on real modex data, which sits near the incompressible
floor and so is not much.

Two conclusions follow, and they pull in opposite directions.  A
bandwidth-optimal allgather is worth roughly 100x here, which is the
difference between impossible and merely expensive — that is a real prize.
And it still leaves a second of unavoidable startup cost that grows linearly
in the job, which no schedule will ever remove.  **Bruck buys a constant.
Only not doing the allgather buys an exponent.**

Level 1 — the constants, and they may not need new topology
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The tree fence's cost is dominated by the release, and the release is a
broadcast of ``D`` bytes costing ``d*(alpha + r*D*beta)``.  Two separate
factors inflate it, and they come off independently.

**Pipelining removes the depth factor.**  Chunk the payload and the levels
overlap: filling the pipeline costs ``d*(alpha + r*c*beta)`` for a chunk size
``c``, and steady state costs ``r*D*beta``, so for ``D >> c``

.. code-block:: text

    T  ~=  d*alpha  +  r*D*beta

The ``d`` is gone from the bandwidth term entirely.  ``xcast`` does not chunk
today — it compresses once at the originator and forwards whole buckets — so
this factor is available and unclaimed.

**The radix is then the whole game, and this is the part worth stating
loudly.**  At ``r = 2`` the pipelined tree costs ``log2(N)*alpha + 2*D*beta``.
Scatter-plus-RD-allgather — the algorithm Piece 2 built and the reset removed
— costs ``2*log2(N)*alpha + 2*D*beta``.  **They have the same bandwidth term,
and the pipelined binary tree has half the latency.**  The lateral machinery,
the Bruck schedule, the partner sets and the exchange's whole fault model buy
nothing over a chunked broadcast on a radix-2 routing tree.

Why a low radix cannot be arranged locally
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The obvious next thought is to keep the routing radix high for control traffic
and use a low one just for the release.  It does not work, and the reason is
the same one recorded under "Lateral links cannot be avoided", reached from a
new direction.

``prte_oob_base_send_nb`` resolves a next hop through ``prte_rml_get_route``,
so a daemon can reach its children and its parent and nothing else without a
lateral link.  To broadcast at radix 2 inside a radix-64 routing tree, a
daemon would have to send to nodes that are its routing *siblings* or nephews
— and every one of those routes back through the parent, so the message
crosses the parent's uplink anyway and the fanout is not reduced.  Chaining
children to each other fails identically.  There is no local rearrangement:
the ``r`` in ``r*D*beta`` is the routing radix, full stop.

So the fork is real and it is only two-way:

* **Lower prte_rml_base.radix globally** and pipeline the large messages.
  Costs nothing in new topology and no new failure modes.  What it spends is
  ``d*alpha`` on every *small* message: at 10 000 nodes, radix 2 is depth 14
  against radix 64's depth 3, so a shutdown command or a ``DAEMON_DIED``
  notice pays roughly 700 µs instead of 150 µs at a 50 µs progress-thread hop.
  Whether that is acceptable is a judgement about control-plane latency, and
  it is the real trade this option asks us to make.
* **Build the lateral overlay** and pay for the links, the pre-warm, the
  descriptor budget, the fault model and the collective-identity problem.

``rml_base_radix`` is already an MCA parameter with a floor of 2
(``src/rml/rml.c:284``), and ``scaletest.sh`` already sweeps it and already
reports ``collect_med_us - barrier_med_us`` as the payload's own cost.  **So
the first option is measurable today, with no new code at all.**  That is
open question #4 below, and it has been the largest unmeasured risk in this
plan since the plan was written.

If we do go lateral: ring against Bruck, for PRRTE specifically
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Assume the measurement says the radix route is not enough.  The choice of
exchange schedule is then live again, and the instinct to pick a ring "because
it is simpler and it is what Slurm's PMIx plugin does" deserves examination.

**It does not de-risk what actually failed.**  What broke the movements was
collective *identity* across consecutive fences, and that defect is identical
under a ring: two routes exist the moment any traffic goes across rather than
down, and a participant may legally begin fence *N+1* before a peer has
finished *N*.  Choosing a ring changes nothing about it.

A ring does have three real advantages here that the earlier comparison —
which judged it purely on ``N-1`` against ``log2 N`` — did not weigh:

#. **Variable-size contributions are native.**  PRRTE's per-daemon
   contribution varies with procs-per-node and with which keys each rank
   published.  Bruck and recursive doubling want equal blocks, so unequal ones
   need a size allgather first or an extra indexing pass; a ring just forwards
   whatever it was handed.
#. **Two descriptors instead of** ``log2 N``, constant in DVM size, which
   retires open question #3 and most of the pre-warm and idle-teardown work
   deferred out of Piece 1.
#. **Fault handling is legible.**  A ring node has exactly one predecessor and
   one successor and healing is "splice past the dead node".  Bruck's partner
   set changes globally when a participant dies, which is why the exchange had
   to opt out of the recovery restart and grow a local deadline instead.

Against all of that: ``(N-1)*alpha``.  At 10 000 daemons and a 50 µs hop that
is **half a second of pure latency**, which eats the entire bandwidth win the
ring was chosen for.  The scale where any of this matters is exactly the scale
where a ring stops working, so **Bruck remains the endpoint** and a ring is
worth building only as a hierarchical component — a ring inside a group,
recursive doubling between group leaders — which is more machinery than either
alone.

The ordering matters more than the choice.  **The sequence number should land
first, on tree-only code, as standalone hygiene**, before any movement exists
to need it.  The section above already records the gap as latent rather than
absent: a contribution naming a retired generation would match no tracker and
``get_tracker(sig, true)`` would build one that nothing ever completes.
Nothing can produce that arrival on one route, which is precisely why it is
safe and cheap to close now, in isolation, where it can be reviewed on its own
terms rather than as one part of a 3000-line movement.

The *other* half of that fix has already landed: ``0d9dde1c8a`` retires a
tracker before delivering its result, at both sites, because "a client is free
to fence again over the same participants as soon as [the callback] returns".
That was the part described as necessary but not sufficient, so one third of
the identity work is in place on tree-only master.

Level 2 — the only change that moves the exponent
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Slurm's real answer was to stop performing the allgather, and PRRTE's
equivalent lever is the direct modex.  Three things that were not all true
when the movements were written are true now:

* The daemon-to-daemon path can actually answer.  It used to park a request
  until the target process committed something, which for PRRTE's own
  placement and binding keys is a deadlock, since nothing requires those to be
  ``PMIx_Put`` at all.  ``answer_from_job()`` fixed it, and the three distinct
  "I cannot answer" states are now told apart.
* Open MPI does not want the data.  ``mpi_add_procs_cutoff`` is 0, ``ob1``
  demands the whole world only for a BTL declaring
  ``MCA_BTL_FLAGS_SINGLE_ADD_PROCS``, and remote procs materialise on first
  use.  Exactly one non-optional reserved key names an off-node proc, and
  PMIx derives it and its neighbours from the node and proc maps.
* The crossover has been measured, and it favours on-demand once the per-rank
  contribution is kilobytes rather than tens of bytes — and the win grows with
  ``N``, because collecting is ``O(N)`` a daemon while resolving is
  ``O(peers touched)``.

What has always blocked it is semantic: ``PMIX_COLLECT_DATA`` says the caller
asked for the data, and a fence that does not deliver it reinterprets a
directive.

**There is a reading that does not reinterpret anything, and it is worth
putting on the table.**  What a caller actually buys with ``COLLECT_DATA`` is
not a *transfer*; it is the guarantee that a later ``PMIx_Get`` for a
participant's key will complete rather than block forever on a peer that never
published.  A transfer is one way to provide that guarantee.  A *certification*
is another: the fence rolls up the fact that every participant has committed,
and the release carries that fact — O(1) — instead of the bytes.  ``PMIx_Get``
then falls back to a direct modex which is now known to be answerable, which
is exactly the failure class that ``answer_from_job()`` closed.  The
post-fence semantics a caller can observe are unchanged; the delivery is lazy.

That costs barrier time plus one resolution per peer actually touched, and it
loses precisely where the crossover says it loses — small ``n``, small ``N``,
every peer touched anyway — so it wants a threshold rather than being
unconditional.  It is a policy decision, not a measurement, and it is the only
item here with an order-of-magnitude story rather than a constant-factor one.

The radix sweep, and what it actually answered
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Open question #4 was measured on 2026-08-14, on a 32-container ``scale`` swarm
built from PRRTE ``4bef237d92`` and PMIx ``101a8fc0`` — both at ``master``
head.  ``rml_base_radix`` 2/4/8/64 crossed with 8/16/32 daemons and 512 B /
8 KB / 64 KB a rank, incompressible payload (``--entropy``), one proc a node.

**Read the first iteration, not the median.**  This is the trap the sweep
walked into and it invalidates any summary taken over the run.  Each iteration
publishes *distinct* keys, so a daemon's store grows monotonically through the
run and every fence is over a larger store than the last.  The effect is not
subtle and it is not noise: **the minimum was iteration 1 in all 36 configs**,
without a single exception, and by iteration 10 the same fence cost 5-20x what
it cost on iteration 1.  A median over ten iterations is therefore not a
steady-state estimate of anything — it is the sixth iteration's value, which
depends on the iteration count.  Later analysis should use iteration 1 or a
fresh DVM per point.  (This also means a *repeated* modex gets steadily more
expensive as the store fills, which is worth knowing on its own.)

On iteration 1, the payload's own cost — ``COLLECT`` minus ``BARRIER``, in
microseconds:

==========  ============  ======  ======  ======  ======  ==========
daemons     bytes/rank    r=2     r=4     r=8     r=64    r2 / r64
==========  ============  ======  ======  ======  ======  ==========
8           512 B          1201     873     951     848      1.42
8           8 KB           1604    1225    1229    1022      1.57
8           64 KB          5523    4838    4586    4482      1.23
16          512 B          2762    2956    2720    2547      1.08
16          8 KB           4051    4506    4263    4227      0.96
16          64 KB         11068   10665   10526    9681      1.14
32          512 B          9425    9466   10111    9944      0.95
32          8 KB          12708   13070   14365   13377      0.95
32          64 KB         32440   32880   40594   33819      0.96
==========  ============  ======  ======  ======  ======  ==========

**Lowering the radix does not help.**  There is a faint trend in the predicted
direction — radix 2 loses 23-57% at 8 daemons, breaks even at 16, and is 4-5%
ahead at 32 — but nowhere near the 3-30x the cost model predicts, and the one
place it leads is the one place the host is most saturated.

**And the swarm cannot decide this question, for two structural reasons that
will not go away with more samples.**  Both are worth stating because they
retire the measurement rather than deferring it.

#. **There are no per-node uplinks for r to contend on.**  The ``r*D*beta``
   term is per-node-per-level *NIC* contention.  Total bytes crossing the wire
   in a broadcast are ``(N-1)*D`` at **any** radix; only the critical path
   changes, and a critical path needs independent links to shorten.  Thirty-two
   containers sharing one host kernel have one loopback path, so lowering the
   radix cannot reduce the bytes and can only add depth — which is exactly the
   sign of the result.
#. **The Docker VM has 8 CPUs and 8 GB for 32 containers.**  Aggregate
   per-rank work is ``O(N^2)`` across the DVM, and on a fixed core count that
   lands in the wall clock as ``N^2``, swamping any algorithmic difference.
   The observed scaling at fixed payload is about ``N^2`` (512 B a rank: 848 ->
   2547 -> 9944 µs at radix 64), which is the host, not the collective.

So open question #4 is **not answered by this harness and cannot be** — the
existing note that it "needs real multi-node hardware" is now specific rather
than cautionary.  What the sweep does establish is network-independent and
more useful than the radix answer:

**The fence's cost is dominated by per-rank work, not by bytes.**  At 8
daemons, going from 4 KB of modex in the entire job to 512 KB — 128x the bytes
— costs 5.3x (848 -> 4482 µs).  At 16 daemons, 3.8x; at 32, 3.4x.  A term that
large and that insensitive to payload is not bandwidth.  This confirms across
three node counts and three payload sizes what "Most of a modex fence's cost
is not the bytes" saw at one point.

A second sweep pins that down, and it is worth stating as a model rather than
as a direction.  Eight daemons, radix 64, iteration 1, holding total modex
bytes constant while varying rank count and key count independently:

============  ==========  =======  ==========  =============
total modex   ranks       keys     bytes/rank  data cost
============  ==========  =======  ==========  =============
8 KB          8           1        1 KB           875 µs
16 KB         8           1        2 KB          1041 µs
64 KB         8           1        8 KB          1192 µs
64 KB         8           8        8 KB          1334 µs
128 KB        8           8        16 KB         1944 µs
512 KB        8           8        64 KB         4642 µs
512 KB        8           1        64 KB         5365 µs
512 KB        32          8        16 KB         4420 µs
4 MB          8           8        512 KB       20885 µs
============  ==========  =======  ==========  =============

Three things fall out.  **Key count does not matter** — 8x1 KB against
1x8 KB moves by 12%, and the sign flips at the larger size, so there is no
per-key term worth naming.  **Rank count matters only through a modest fixed
term** — at 512 KB total, 32 ranks cost *less* than 8 (4420 against 4642), so
there is no per-rank cost in the byte-dominated regime; at near-zero payload,
4x the ranks costs 1.8x, so the floor is sublinear in ranks and closer to
per-daemon.  And the whole series fits a two-term model:

.. code-block:: text

    8 daemons, 8 ranks:    cost ~=  850 us  +  ~6 ns/byte of total modex
    8 daemons, 32 ranks:   cost ~= 1600 us  +  ~3 ns/byte

So there is a **fixed per-fence cost** — 850 µs at 8 daemons, and from the
radix table roughly 2.5 ms at 16 and 10 ms at 32 — plus a byte term running at
an effective 150-300 MB/s (a debug build, packing, unpacking and hash-storing
on loopback).  The two are equal at about **140 KB of total modex**.

That crossover is the number to carry away.  **Open MPI's entire modex at 16
ranks is 1319 bytes** — a hundredfold below it.  A real job is wholly inside
the fixed-cost regime, where the payload is free and the cost is the fence
machinery itself: the rollup, the release traversal, the tracker.

This corrects a reading taken from the first sweep alone, where 128x the bytes
cost only 5.3x and that looked like evidence of per-rank work.  It was not —
it was the fixed term dominating at both ends of too narrow a range.

The consequence for the algorithm question is sharper than either reading.  A
bandwidth-optimal allgather optimises the byte term, and the byte term does
not become the larger half until a job's *total* modex passes ~140 KB.  At
10 000 nodes and 128 procs a node that threshold is passed by four orders of
magnitude, so the prize at extreme scale is real and Level 0's arithmetic
stands.  But everything below roughly a thousand ranks is spending its time in
a fixed cost that Bruck, a ring, a lower radix and a pipelined release all
leave exactly where it is.  **Level 2 is the only item on the list that
attacks the term that dominates the jobs people actually run**, because not
collecting is the only option that removes the fence's payload path rather
than accelerating it.

The fixed term is ``gds/shmem3``, and it is not ours
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The fixed term was chased down on 2026-08-14 and it is not in PRRTE at all.

**The isolating experiment.**  ``scaletest --nkeys 0`` runs a fence with
``PMIX_COLLECT_DATA`` true and no ``PMIx_Put``, no ``PMIx_Commit`` and no
payload — the whole collect path, over nothing.  It costs **the same as a
barrier**: −63 to +82 µs at 8 ranks and −210 to −121 µs at 32.  So the collect
*machinery* is free.  One key of **one byte** — eight bytes of modex in the
entire job — costs **948 µs** at 8 ranks and 1458 µs at 32.  The step needs
data to exist and does not care how much there is.

**What switches on.**  PMIx's ``gds`` framework has two components here:
``hash`` at priority 10 and ``shmem3`` at priority 20, so ``shmem3`` wins by
default.  Forcing the other collapses the step (8 daemons, one proc a node,
one-byte values, iteration 1, ``COLLECT`` minus ``BARRIER`` of the largest
local duration):

=====================  =========  =========  =========
gds component          keys = 0   keys = 1   keys = 8
=====================  =========  =========  =========
``shmem3`` (default)      +27 µs   +1326 µs   +1624 µs
``hash`` (forced)         −36 µs    +108 µs     +82 µs
=====================  =========  =========  =========

Module selection was confirmed directly rather than inferred — under
``--pmixmca gds hash`` with ``gds_base_verbose``, ``hash`` is the only
component queried.

**Why, from the source.**  ``server_store_modex()`` in
``src/mca/gds/shmem3/gds_shmem3.c`` will not write into a modex segment it has
already finished: local clients have it mapped, storing into it can rehash the
table and reallocate the key index underneath a reader, and the segment was
sized for the first modex so a larger second one overruns the allocator.  So a
second fence **releases the segment, bumps** ``modex_generation`` **and calls**
``shmem3_segment_create_and_attach()`` — a fresh shared-memory segment, built
and populated from scratch, on every collecting fence that carries data.
``server_mark_modex_complete()`` then advertises it into the reply of **each
local client**, which maps it.  The code cites openpmix#4087 and points at
``examples/modex_twice.c`` as the canary.

**It is not a fixed tax — its share grows with the DVM.**  At 8 KB a rank,
one proc a node, iteration 1:

=========  ==================  ============  =================
daemons    ``shmem3``          ``hash``      shmem3 premium
=========  ==================  ============  =================
8              1320 µs            688 µs        +632 µs
16             6424 µs           1679 µs       +4745 µs
32            16435 µs           3799 µs      +12636 µs (77%)
=========  ==================  ============  =================

The segment holds the whole modex, so it grows as ``N*n`` and so does the cost
of building it and of every client mapping it.  **At 32 daemons three quarters
of the modex fence's payload cost is the GDS rather than the collective.**

**And it is not the container's filesystem**, which was the obvious way for
this number to be an artifact.  The backing file goes in the session dir
(``PMIX_NSDIR``, else ``PMIX_TMPDIR``) and ``/tmp`` in these containers is
overlayfs.  Re-run with ``--prtemca prte_tmpdir_base /dev/shm`` — tmpfs, and
verified in use — at 32 daemons: **14781 µs to 13074 µs, a 12% change.**  The
cost is the work inside the segment, not the file creation.

**This is a trade, not a defect.**  ``shmem3`` exists so that ``PMIx_Get``
after the fence is a shared-memory read rather than a lookup or a wire round
trip; it buys cheap gets with expensive fences.  But it means the term that
dominates a realistically-sized modex is on the PMIx side of the boundary, and
**every collective option in this document leaves it exactly where it is** —
Bruck, a ring, a lower radix and a pipelined release all optimise a byte term
that, at 32 daemons, is a quarter of the cost.

Two consequences.  It **strengthens Level 2 again**: not collecting skips the
segment build as well as the transfer, so it removes the dominant term and the
byte term together.  And it **cuts the other way on gets**, which the crossover
measurement already hinted at — the on-demand path gives up exactly the cheap
``PMIx_Get`` that ``shmem3`` is paying for in advance.  That is the real
tension in the ``COLLECT_DATA`` decision, and it is sharper than "does the
directive permit it".

The follow-up work belongs in openpmix, not here: rebuilding the segment from
scratch on every collecting fence is what costs, and sizing it with headroom,
appending a generation, or deferring construction are all PMIx-side questions.

Delta segments and a delta commit
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The proposal, and it is the one the ``shmem3`` comment itself anticipates:
instead of rebuilding a segment holding the whole modex to date, have each
generation's segment hold **only the new information**, and have a retrieval
search the segments newest-to-oldest and return the first hit — which is by
construction the newest value for that key.

**Measured, at 32 daemons and 8 KB a rank, over eight consecutive collecting
fences.**  Least-squares fit of the cost of fence *k* (``COLLECT`` minus
``BARRIER``, max local duration, µs):

.. code-block:: text

    shmem3 (today):   11719  +  4801*k
    hash:              4355  +   474*k

The slope is the whole story.  Rebuilding the cumulative modex makes fence *k*
cost ``O(k)``, so a run of *K* fences costs ``O(K^2)``; ``shmem3``'s slope is
**ten times** ``hash``'s.  Delta segments attack exactly that term:

=========  ==========  =================  =========
fences     today       delta segments     saving
=========  ==========  =================  =========
1           11.7 ms         11.7 ms        **none**
8            228 ms          107 ms          2.1x
20          1147 ms          324 ms          3.5x
=========  ==========  =================  =========

**The first row is the important caveat.**  At the first fence the delta *is*
the whole modex, so nothing changes — and the 77% figure above was a single
fence.  A job whose only collecting fence is at ``MPI_Init`` gains nothing.
The win is for repeated collecting fences: MPI Sessions, ``PMIx_Group``
construct/destruct, connect/accept, dynamic spawn.

**Two things recommend it beyond the slope.**  The fixed per-segment overhead
is only ~2456 µs of the 7660 µs ``shmem3`` premium at 32 daemons — measured
with a one-byte payload — so 68% of the premium is content-proportional and
therefore is what shrinking the segment reaches.  And search-back **preserves
the immutability invariant** ``pmix_gds_shmem3_fetch`` relies on for
``is_tsafe = true`` — "a segment a client can see is never written again" —
arguably more cleanly than release-and-rebuild, which is precisely the
operation the current code has to keep away from a live reader.

**Three costs to design against.**  Retrieval becomes ``O(number of
generations)``, and a *miss* searches all of them — which matters because
``PMIX_OPTIONAL`` gets and the fall-through-to-direct-modex path are both
misses, so the get side pays for the fence side's win.  Each client
accumulates a mapping per generation, so descriptor and VMA pressure need a
compaction policy — fold into one segment after N generations, which
reintroduces the rebuild but amortised over N.  And the fixed ~2456 µs is paid
once per fence either way, so it is a floor this idea cannot go below.

**Decision: the commit becomes a true delta.**  There is no valid reason to
re-ship data that has already been delivered.  Today's payload is cumulative —
"a commit ships the process's whole local store and a server contributes each
local proc's full set from its own" — and that is why ``hash``'s slope is 474
µs a fence rather than zero: every participant re-receives everything it
already has, every time.  A delta commit flattens *both* arms and cuts the
transfer as well as the storage, and it is the exact trigger condition the
``shmem3`` comment names for needing search-back, so the two changes are one
piece of work rather than two.

Two implementation points that follow, and the second is easy to miss.

#. **No tombstones are needed.**  Nothing in this path deletes a key, and a
   re-``PMIx_Put`` of an existing key is an overwrite whose new value ships in
   the newer generation — which search-back returns first.  So "newest hit
   wins" is a complete rule, not an approximation.
#. **There are two different deltas, and only one of them is well defined
   locally.**  A *client to its server* delta is unambiguous: the client knows
   what it has already sent.  A *server's contribution to a fence* is a delta
   relative to what the other participants already hold, which is a property
   of the collective's history rather than of the proc — and in an elastic DVM
   a newly added daemon holds none of it.  The cumulative payload gives that
   daemon its catch-up for free today; a delta design has to provide one
   deliberately, either by handing a joiner every generation or by compacting
   for it.  This is a requirement on the design, not an objection to it.

**And it is not the lever for the first fence**, which is where a
single-collective job spends everything.  There the cost is the rate at which
the in-segment hash is built: ``shmem3`` runs at roughly 35 ns a byte against
``hash``'s 14 for the same data, so what that case wants is a cheaper
construction, not a smaller segment.  The two optimisations are independent
and both are worth having.

.. note::

   These figures are single runs on a host with 8 cores serving 32
   containers, and each per-iteration series has a visible non-monotone step.
   The two slopes differ by 10x, comfortably outside that noise, but the
   absolute values are indicative rather than precise.

Separating the barrier from the modex
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The two operations that share the fence want different things, and treating
them as one collective is what makes the radix a compromise that serves
neither.  Taking them apart, on 2026-08-14:

**The barrier is not where the time is, and this harness cannot rank its
options anyway.**  Three things about it are solid.  Its cost has a fixed
floor of about **165 µs** at any DVM size — the client-to-server-to-daemon
round trip that no topology touches — rising to 1.4 ms at 32 daemons
(``T ~= 165 + 40*(N-1)`` µs at radix 64, where every one of N = 2..32 is depth
1).  A ``--nkeys 0`` fence, which is the whole collect path over no data,
costs the **same as a barrier** at every scale measured (+1.6, −4.4, +12.2,
+10.5, −7.0 µs at N = 2..32).  And at 32 daemons the barrier is 1.4 ms against
the modex's 16 ms.

What is *not* solid is any ranking of its radices.  A repeat of one identical
configuration (16 daemons, radix 4) measured 686 µs in the morning and 1521 µs
in the afternoon — **2.2x apart on the same build and the same swarm**.  Across
six overlapping configurations the ratios were 0.88, 1.17, 2.22, 1.07, 0.97,
0.86.  Every radix effect and every worker-thread effect observed here is
inside that spread, so the apparent ordering is not evidence.

.. warning::

   An earlier draft of this section proposed that a barrier's cost is
   ``2*d*alpha + 2*d*r*c`` for a per-message cost ``c``, and predicted that
   radix 4 would beat radix 64 by ~1.9x at 32 daemons.  **That prediction was
   measured and failed** — radix 64 was fastest at every node count — and the
   follow-up did not rescue it.  The framing is not supported and should not
   be carried forward without cluster data.  What survives is only the
   observation that a root's child count appears in the cost at all, which the
   depth-1 series does show.

**The OOB worker bases were measured, and made the barrier worse here.**
``prte_oob_progress_threads`` defaults to 0, so every other number in this
document was taken with each daemon servicing all its peer sockets on one
event base.  Forced to 4 — verified by thread count, +4 on both the HNP and
remote daemons — the barrier was 1.2-1.9x *worse* at five of six points.  That
is not evidence against the feature: its stated win is link **occupancy**,
which needs spare cores, and 32 daemons times 4 extra threads on an 8-core VM
have none.  It is evidence that this harness cannot evaluate it.

Two things the worker bases do **not** change, and the distinction is the
useful one.  They divide the per-message *servicing* term by the number of
bases.  They cannot touch the *byte* term, because the bytes still cross one
NIC — the same finding that came out of the send-thread proposal earlier
(~10-13%, and no change to scaling).  So they land almost entirely on the
barrier and almost not at all on the modex.

**The modex argument needs none of those constants**, which is why it survives
the failed prediction: it is byte-counting.  A radix-``r`` forwarder transmits
``r`` copies of the release; an allgather node transmits one.

**The gather and the broadcast are asymmetric, and that settles the rollup.**
A broadcasting daemon receives one copy of ``D`` and sends ``r`` copies, so its
egress is ``r*D``.  A gathering daemon receives ``r`` messages and sends
**one** aggregate, so fanout costs it nothing at all.  Every axis therefore
favours a *high* radix for the rollup — smaller per-node egress (``D/64``
against ``D/3`` for a child of the root), fewer hops, and ``d`` times fewer
total byte-hops — and only the release wants a low one.  Writing the cost of
the pair as ``(1 + r_release) * D/B``:

====================================  ==============  ==========
configuration                         cost            vs today
====================================  ==============  ==========
radix 64 both (today)                 ``65 * D/B``      1x
gather 64, release 3                  ``4 * D/B``     **16x**
Bruck allgather                       ``1 * D/B``       65x
====================================  ==============  ==========

So a two-radix tree — a high radix for command traffic, barriers and the
rollup, a low one for the release — captures 16x of an available 65x with no
exchange schedule and no new fault model.  Its ceiling is the gather's own
``D/B``, which is irreducible because the HNP must receive the entire modex;
that is why gather-then-broadcast can never reach an allgather, and why Bruck
remains worth another 4x on top.

**The enabling mechanism is already in the tree.**  A radix-3 release edge is a
*sibling* edge in a radix-64 routing tree, so the two-radix scheme needs
direct sends to non-children — which is exactly Piece 1, and Piece 1 survived
the reset.  ``prte_rml_send_buffer_direct_nb``, ``PRTE_RML_SEND_DIRECT``,
``prte_rml_base.lateral_links`` and ``prte_rml_is_lateral_only`` are all on
master and currently unused.  This gives them a purpose without reviving the
exchange that was withdrawn.

What a second topology costs in identity and in tracking
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two radices, or a Bruck allgather, means more than one path between daemons.
That reopens the defect that withdrew the movements, and it raises a second
question about reliability accounting.  Both have concrete answers.

**The consecutive-fence race does come back, and it was already solved.**  A
signature is only a participant list, so consecutive fences over it are
indistinguishable; with a release down one path and blocks across another, a
peer may legally begin fence *N+1* and reach a daemon still in *N*.  The fix
was three parts, and the demonstration is on record: the 16-daemon reproducer
went from failing on the first attempt to **8 of 8 passing**.

* **Retire the tracker before delivering** — merged as ``0d9dde1c8a``.
  Necessary, not sufficient.
* **A generation on the signature**, packed on the wire and part of
  ``get_tracker``'s key.  It needs no exchange: every participant takes part in
  every fence over the set, and two identical fences may never be in flight at
  once, so the *k*-th fence over a signature is the *k*-th one on every daemon
  by construction, and each derives the same number by counting what it has
  retired.
* An intermediate hold-on-converged step, superseded by the generation.

Three things must be settled before that is trusted at a second topology, and
the first is a genuine hole rather than a detail.

#. **An elastic grow breaks the local derivation.**  The generation is derived
   by counting retirements, and a daemon added by a grow has counted none.
   The signature of a job-wide fence does not change when the job grows —
   ``{nspace, WILDCARD}`` — but ``create_dmns`` now resolves it to a larger
   daemon set that includes a daemon at generation 0 while every other
   participant is at *k*.  It originates 0, its parent looks up *(sig, 0)*,
   nothing matches, and that is precisely the hang the generation exists to
   prevent.  It needs a deliberate rule — adopt the highest generation seen
   for a signature, have the HNP stamp it, or seed a joiner at grow time.
   Note that Slurm's ``kvs_seq`` is **carried on the wire in both directions
   and checked on arrival** rather than only derived, which is exactly the
   robustness this case wants.
#. **A lateral collective needs three levels of identity, not two.**  A tree
   message is identified by its signature.  A generation makes that *(signature,
   generation)*.  But a Bruck allgather has ``log2 N`` **steps** within one
   collective, each carrying a different block, so the identity is
   *(signature, generation, step)*.  Each level is an independent way to
   alias, and the tree never needed any of them.
#. **More partners is more exposure.**  A ring has two; Bruck has ``log2 N``.
   The mechanism is identical and the generation closes it, but the number of
   windows scales, which argues for making a mismatch **loud** — the rule
   already established for the movement id, where a disagreement produces a
   named ``show_help`` rather than a hang.

**And yes, the patterns need separate trackers.  The current code says why.**

* **A completion tracker is a statement about one topology.**  ``op_t`` holds
  ``nexpected`` — "# children at time of (re)start" — plus ``ack_id_up``,
  ``ack_id_down`` and ``replay_pending_parent``, whose comment is that "our
  completion information for older ops is invalid when our subtree grows".
  Acks roll up the routing tree.  You cannot account for coverage of a message
  that traversed tree *B* by rolling up acks on tree *A*, so each movement
  needs its own completion accounting.
* ``op_id`` **is a single global space** — "HNP's assigned collective ID,
  globally unique".  Two patterns allocating from it will alias.  They need
  separate spaces, or the topology in the key.
* **The fence receive path would drop lateral traffic today.**
  ``prte_grpcomm_fence_recv`` computes ``prte_rml_get_subtree_index(sender)``
  and, on a negative result, logs ``PRTE_ERR_NOT_FOUND`` and returns — "we are
  not this daemon's parent, so this contribution is not ours to aggregate".  A
  Bruck block, or a radix-3 release arriving from what the routing tree calls a
  sibling, is not in any of this daemon's subtrees and is discarded.  Each
  pattern therefore needs its own tag and its own receive path, which is what
  the deleted ``PRTE_RML_TAG_XCAST_BULK`` and ``_FENCE_EXCHANGE`` were.

**Ordering between patterns is mostly a non-issue**, and the operations table
at the top of this document is why: the modex is marked as needing no
ordering, and command traffic only "some".  The one pair that matters is a
fault notice racing collective traffic on a *different* topology, and
``recovery_epoch`` already stamps every message on the fence tag for it.  But
note that ``fence_recv`` currently treats a **newer** epoch as "should be
unreachable" and adopts it defensively.  With two topologies in play that
branch stops being unreachable and becomes routine, so it has to be made
correct rather than merely defensive.

Measuring it needs a cluster
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Both sweeps above ended at the same place: the container harness cannot decide
any of this, for reasons that are structural rather than statistical.  It has
no per-node uplinks for the ``r`` factor to contend on, and too few cores, so
its wall clock reports the CPU scheduler.

``contrib/scaling/cluster-sweep.sh`` is the answer to that — one script, run
inside a real allocation, that sweeps radix against DVM size against payload
and also prices the GDS component and the OOB worker bases, then tars up the
results.  It detects SLURM, PBS or a hostfile; it needs only PRRTE on ``PATH``
and a compiler.  ``contrib/scaling/README.md`` is written for someone outside
this project who has been handed the script and nothing else.

It is built around the three traps that produced wrong answers here: it runs
**one** measured fence per job and repeats the *job* rather than the iteration
(because the client publishes new keys each iteration, so iteration *k* is a
fence over *k* rounds of data); it verifies every DVM came up with all its
daemons before measuring; and it records a manifest of the machine and the
build, because twice now a number has been hard to interpret afterwards for
want of one.

What to do, and in what order
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Revised after the sweep, which moved two items and deleted one.

#. **Make the commit a true delta, and give shmem3 delta segments with
   search-back.**  One piece of work in openpmix, against openpmix#4087: the
   delta commit is the trigger condition the ``shmem3`` comment already names
   for needing search-back, and it is what flattens the transfer as well as
   the storage.  It turns a run of repeated modex fences from ``O(K^2)`` into
   ``O(K)`` — 2.1x at eight fences, 3.5x at twenty.  It does **not** reduce
   the cost of a single collecting fence; see the caveat above.
#. **Make the first fence cheaper**, which is a separate lever and reaches the
   case a single-collective job actually pays.  ``shmem3`` builds its
   in-segment hash at ~35 ns a byte against ``hash``'s ~14 for the same data;
   the segment rebuild is 77% of the payload cost at 32 daemons and grows with
   DVM size.  This is construction rate, not segment size.

   The immediately available lever while both are open: ``--pmixmca gds hash``
   is a one-parameter change that cuts the fence cost 4.3x at 32 daemons.  It
   is not free — it gives up the cheap post-fence ``PMIx_Get`` that ``shmem3``
   is buying — so it is a knob to characterise, not a default to change.
#. **Decide the COLLECT_DATA-as-commit-barrier question.**  The only item
   that attacks the dominant term for jobs below roughly a thousand ranks, and
   the only one with an order-of-magnitude story rather than a constant-factor
   one.  It is a policy call, not a measurement.
#. **Land the fence sequence number standalone**, on tree-only code, and
   settle its elastic-join rule (see below).  The retire-before-deliver half
   is already merged as ``0d9dde1c8a``.  Cheap, reviewable in isolation, and it
   takes the defect that killed the movements off the critical path of
   anything that comes later.
#. **Hold the radix and the pipelined release.**  Both target the byte term,
   which does not dominate until ~140 KB of total modex, and the radix half is
   now measured as neutral-to-harmful.  Chunking ``xcast`` is still right for
   the *launch* message and ``FILEM``, which are large by construction — but
   that is a broadcast argument, not a fence one, and it should be made on
   those operations' own numbers.
#. **Hold Bruck until there is real hardware.**  Its entire case rests on
   ``alpha`` and ``beta`` constants that a single-host container swarm cannot
   supply — now demonstrated rather than asserted — and 2847 lines have
   already been spent on that bet once.

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

   **Measured 2026-08-14, and retired as unanswerable on this harness** — see
   "The radix sweep, and what it actually answered" above.  A lower radix does
   not help (radix 2 loses 23-57% at 8 daemons and is within noise at 32), and
   the container swarm cannot decide the question in principle, because the
   ``r*D*beta`` term needs independent per-node uplinks that 32 containers on
   one host do not have.  What the sweep did establish is that the fence's
   cost is ~850 µs of fixed overhead at 8 daemons plus ~6 ns a byte, equal at
   about 140 KB of total modex — so a real job is nowhere near the regime any
   bandwidth optimisation improves.

   The original text of this question follows, and its caveats still hold.

   This remains **unmeasured**, and it is the largest open risk in the plan.
   The cost model above is standard (Thakur, van de Geijn), but PRRTE's own
   constants are not: ``alpha`` here is a progress-thread hop rather than a
   raw round trip, and ``xcast`` already compresses, which discounts the
   tree's bandwidth term by an unknown factor.  A measurement needs real
   multi-node hardware at realistic scale — ten containers on one host tell
   you nothing useful about either constant.  The seam and both halves of the
   arithmetic are already in place, so instrumenting an A/B of the two
   movements is cheap once such a machine is available.
