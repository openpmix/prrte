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

Everything landed so far is either behaviour-neutral or inert: nothing yet
sets ``direct`` on a send, and there is still only one data movement.  That is
deliberate — each step was verifiable on its own, and the multi-node suite has
not regressed at any point.

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

Both halves of the bulk movement's *arithmetic* went in before any of its
transport, and are tested exhaustively without one.  That ordering was chosen
because an error in either would surface later as what looks like a transport
or corruption bug rather than an arithmetic one.

``scatter_allgather`` is implemented and reachable, but nothing selects it on
its own: ``grpcomm_bcast_movement`` defaults to ``tree``, so every broadcast
still travels exactly as it did before the movement existed.  ``auto`` — the
size-based rule described under *Selection* below — is implemented and has to
be asked for.  That is a statement about evidence rather than about the code,
and the missing evidence is named under *Verification*: the crossover point
has not been measured on hardware that could measure it.  Turning ``auto`` on
by default is the next step, and it needs a number, not a patch.

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
   * - launch message, ``WIREUP``, ``FILEM`` chunks
     - one-to-all
     - large
     - partly
     - scatter + allgather
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

New delivery tags are appropriate for the launch message and the ``FILEM`` bulk
path, rather than overloading ``PRTE_RML_TAG_DAEMON`` whose receiver dispatches
on a command byte. But the *movement* choice is driven by measured size stamped
on the wire, not by the tag — otherwise every future large payload needs a new
tag to get the benefit.

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

At the originator only.  Bulk iff the tag is not ordering-critical, the
payload is at least ``grpcomm_bcast_bulk_min_bytes``, and the participant
count is at least ``grpcomm_bcast_bulk_min_daemons``.  A
``grpcomm_bcast_movement`` parameter (``auto``/``tree``/``bulk``) forces
either way, for testing and as an escape hatch.

``PRTE_RML_TAG_WIREUP`` is excluded even though it is large and would
otherwise qualify, because it is in the ``process_first`` set — it changes the
child set, so it must be processed before forwarding.  That exclusion is
deliberate and must stay commented in the code, or it will be "optimised"
back in.

Piece 3 — allgather: framing versus movement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The same split. The shared framing is what ``grpcomm_fence.c`` already factors
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

``PMIX_COLLECT_DATA`` appears **nowhere** in PRRTE: PMIx consumes it
client-side and the host sees only ``data``/``ndata``. So the only local
discriminator between a barrier and a modex is ``ndata == 0``, which is
*realistically* equivalent to no-collect but not guaranteed — a collect fence
could yield an empty blob on a daemon whose procs posted nothing.

The right fix is for PMIx to pass the directive up in the fence upcall's info
array, and a major version change is the moment to do it. Following the
established idiom:

* an openpmix change adding the directive to the upcall, plus a ``PMIX_CAP_*``
  flag;
* ``PRTE_CHECK_PMIX_CAP([FENCE_COLLECT_DIRECTIVE], …)`` in
  ``config/prte_setup_pmix.m4``;
* PRRTE guards the *behaviour* with
  ``#if PRTE_PMIX_HAVE_FENCE_COLLECT_DIRECTIVE`` and falls back to the
  ``ndata == 0`` heuristic against an older PMIx, accepting less optimal
  selection there and relying on the wire interlock above.

**Guard the behaviour, never the bytes**: the movement id is packed and
unpacked unguarded, so every daemon in a build agrees on the message layout.

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

The multi-node baseline is **540 passed, 0 failed, 3 skipped**.  Two practical
notes, both of which have cost time here:

* ``check_PROGRAMS`` are built by ``make check``, **not** by ``make``.  Running
  a unit-test binary straight after ``make`` silently runs a stale one, and a
  newly added case simply does not appear in the output.
* For anything that changes the wire format, the multi-node run is
  load-bearing rather than a formality: it is what proves every daemon,
  including pure relays, agrees on the new layout.

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
