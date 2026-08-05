# `src/rml` — the Runtime Messaging Layer

This directory implements all daemon-to-daemon messaging in PRRTE: how a
`prte`/`prted` process sends a buffer to another daemon, how that buffer is
routed across the DVM, and how it is delivered to the code that posted a
receive for it.

This document is an orientation map for anyone (human or agent) about to modify
the code. For a narrative walkthrough of *how it works*, see
[`docs/how-things-work/rml/index.rst`](../../docs/how-things-work/rml/index.rst).

## History: three frameworks collapsed into one

`src/rml` used to be **three** separate MCA frameworks:

- **rml** — the messaging API (`send`/`recv`).
- **routed** — pluggable routing-tree algorithms.
- **oob** ("out of band") — pluggable transports, each with swappable modules.

In practice PRRTE only ever shipped one RML, one routing algorithm (a radix
tree), and one transport (TCP). The three frameworks were collapsed into this
single directory. When reading the code, keep this in mind: language about
"looping across components," "trying another module," or "some other
transport" is **historical**. There is exactly one path: RML → radix routing →
TCP. If you find such comments, they are cruft — fix them.

## File map

**Core RML (API, matching, dispatch):**

| File | Responsibility |
|------|----------------|
| `rml.h`, `rml_types.h` | Public interface: `PRTE_RML_SEND`/`RECV` macros, RML tags, the `prte_rml_base` global, and the send/recv/posted-recv object types. |
| `rml.c` | `prte_rml_open`/`close`/`register`; defines the `prte_rml_base` global and all RML class instances; `prte_rml_send_callback`; `prte_rml_is_node_up`. |
| `rml_send.c` | `prte_rml_send_buffer_nb` (plus the `_reliable_nb` and `_direct_nb` variants). Short-circuits sends to self; otherwise hands the message to the OOB. |
| `rml_recv.c` | `prte_rml_recv_buffer_nb` / `prte_rml_recv_cancel`. Thread-shifts the request onto the progress thread. |
| `rml_base_msg_handlers.c` | The heart of matching: `prte_rml_base_post_recv` (add/cancel a posted recv), `prte_rml_base_process_msg` (deliver an arrived message to a posted recv, or hold it in `unmatched_msgs`). |
| `rml_base_contact.c`, `rml_contact.h` | `prte_rml_parse_uris` — split a contact URI into name + address list. |
| `rml_purge.c` | `prte_rml_purge` — drop posted recvs and held messages for a departed peer. |

**Routing (the radix tree):**

| File | Responsibility |
|------|----------------|
| `routed_radix.c` | `prte_rml_get_route` (next hop toward a target), subtree indexing, and the tree (re)computation used on startup and after faults: `compute_routing_tree`, `repair_routing_tree`, `update_ancestors`, promotion/descendant fixups, `route_lost`. Also maintains `prte_rml_base.dead_dmns` — the *permanent* departed-daemon set that survives the recomputes an elastic grow triggers. |
| `radix.h` | Header-only radix-tree math over daemon ranks: parent/child/sibling navigation, `subtree_contains`/`subtree_index`, depth motion, and "next living" traversal that skips failed ranks. Pure functions on rank numbers, no I/O. |

**Fault tolerance / reliability (newer; not collapse cruft):**

| File | Responsibility |
|------|----------------|
| `rml_fault_handler.c` | The RML's own reaction to a recomputed tree: sets process states and drives death/adoption notices. |
| `relm/` | RELM — reliable messaging that survives daemon failures by re-driving messages over the repaired tree. Has its own small state machine (`relm/state_machine.c`, `relm/base/`). See [`relm/AGENTS.md`](relm/AGENTS.md). |

The transport lives in [`oob/`](oob/AGENTS.md), which has its own editing map.

**Transport (TCP — the `oob/` subdirectory):**

| File | Responsibility |
|------|----------------|
| `oob/oob.h` | `prte_oob_base` global + the `PRTE_OOB_SEND` entry macro. |
| `oob/oob_base_stubs.c` | `prte_oob_base_send_nb` — resolve the next hop, find/create its TCP peer, queue the message (connecting first if needed). Plus URI build (`get_addr`) and parse (`process_uri`/`set_addr`). |
| `oob/oob_tcp.c` | OOB open/close/register: MCA params, interface discovery, listener startup; `recv_handler` (connection handshake); `simulate_node_failure` (test hook). |
| `oob/oob_tcp_component.c` | Class instances; the `lost_connection` / `failed_to_connect` event handlers. |
| `oob/oob_tcp_listener.c` | Listening sockets and the accept path. |
| `oob/oob_tcp_connection.c` | Per-peer connection state machine: connect, IDENT ack/nack handshake, accept, close. |
| `oob/oob_tcp_sendrecv.c`, `.h` | The socket send/recv event handlers and their queueing macros. The recv-completion path decides **deliver locally vs. relay onward**. |
| `oob/oob_tcp_hdr.h` | The on-wire message header (`prte_oob_tcp_hdr_t`). |
| `oob/oob_tcp_peer.h`, `oob/oob_tcp_common.[ch]` | Peer/address objects; socket options and state-name helpers. |

## The two paths, in one paragraph each

**Send.** `PRTE_RML_SEND(rc, dst, buf, tag)` calls `prte_rml_send_buffer_nb`
(`rml_send.c`). A message to *self* is wrapped and re-posted locally without
touching the network. Otherwise it becomes a `prte_rml_send_t` and goes to
`PRTE_OOB_SEND`, which thread-shifts to `prte_oob_base_send_nb`
(`oob_base_stubs.c`). That routine asks `prte_rml_get_route` for the next hop,
looks up (or creates and connects) the TCP peer for that hop, and queues the
message. The connection state machine (`oob_tcp_connection.c`) and send handler
(`oob_tcp_sendrecv.c`) do the rest.

**Receive / relay.** When a peer's socket has a complete message, the recv
handler (`oob_tcp_sendrecv.c`) checks `hdr.dst`. If it is us, it calls
`PRTE_RML_POST_MESSAGE`, which thread-shifts to `prte_rml_base_process_msg`
(`rml_base_msg_handlers.c`): match the message against `posted_recvs` (peer +
tag, wildcards allowed) and fire the callback, or hold it in `unmatched_msgs`
until a matching recv is posted. If `hdr.dst` is *not* us, we are an
intermediate hop: the handler re-enters `PRTE_OOB_SEND` so the message is
routed on toward its destination.

## Routing in one paragraph

Daemons are ranks `0..N-1` (rank 0 is the HNP) arranged in a radix tree
(default radix 64; `--prtemca rml_base_radix`, with `routed_radix` as a
deprecated synonym — note it is **not** `prte_rml_radix`, which is silently
ignored). `prte_rml_get_route(target)` returns: `target` if it is me; my parent
(lifeline) if `target` is outside my subtree; otherwise the child whose subtree
contains `target`. After a daemon fault, `prte_rml_repair_routing_tree`
recomputes ancestors, children, and possible self-promotion, packages the delta
into a `prte_rml_recovery_status_t`, and notifies the RML, grpcomm, filem, and
relm fault handlers.

### The tree layout, precisely

`radix.h` is the whole of the math, and it is worth writing down because the
layout is **not** the contiguous one you might assume. Rank 0 is the root at
depth 0. A node of rank `r` at depth `d` has

```
width W = radix^d          count = 1 + radix + ... + radix^d
```

its children are `r+W, r+2W, ..., r+(radix)W`, and its subtree is every rank
`>= r` congruent to `r` modulo `W`. So at radix 2 the tree is
`0 -> {1,2}`, `1 -> {3,5}`, `2 -> {4,6}` — siblings are *strided*, not adjacent.
`radix_subtree_contains` is exactly that congruence test, which is why it is a
single modulo rather than a walk.

Two traversal helpers matter for fault recovery, and both are easy to misread:

- `radix_to_next` walks the tree **depth-first, right-first**, and
  `radix_to_next_living` keeps walking until it lands on a rank that is not in
  `failed_dmns`. That is how `update_ancestors` finds a dead ancestor's
  inheritor and how `update_descendants` replaces a dead child. If this walk
  ever ends early, live daemons silently vanish from the repaired tree — there
  is no other signal.
- Ranks are dense in `[0, n_dmns)`. Any candidate rank equal to `n_dmns` is one
  past the end, not the last one. This bit once: the walk stepped in from a
  node's rightmost child slot while testing `> n_dmns`, so a node whose
  rightmost slot landed exactly on `n_dmns` — rank 0 with the default radix 64
  in a **64-daemon DVM**, for instance — stopped on a rank that does not exist.

`test/unit/rml/test_rml_routing.c` pins both properties over a matrix of radices
and DVM sizes: every rank has exactly one parent, and the depth-first walk from
the root visits every rank exactly once.

### Radix values below 2 are not valid

The math divides by `radix - 1` and takes a logarithm to base `radix`, so a
radix of 1 (or 0) is a division by zero the moment a second daemon exists, not
a degenerate-but-workable tree. `prte_rml_register` is the single point where a
user value enters, and it clamps there with a warning. Do not "support" radix 1
by special-casing the math.

### Recomputing the tree is not the same as repairing it

`prte_rml_compute_routing_tree` runs at open **and again on every DVM grow**,
and `prte_rml_revive_routing_tree` shares its helper (`build_tree_from_base`).
Both rebuild from the fault-free radix positions and then route around whatever
is currently failed. Two consequences to keep in mind when editing that helper:

- It must **fully overwrite** the children array, not just write as many slots
  as there are children. `resize_ranks` fills only the slots it newly creates
  and is a no-op when the array is already `radix` long, so a partially-written
  array keeps the previous computation's children in its tail.
- It is the only place a returned (revived) rank can reappear.
  `prte_rml_update_ancestors` only ever walks a dead ancestor *forward* to its
  inheritor, so it can shorten an ancestor list but never grow one — rebuilding
  from base is what makes a revival possible at all.

## Lateral links — sends that deliberately bypass the tree

Everything above describes routed traffic: `prte_rml_get_route` picks the next
hop and the message walks the tree. That is right for control traffic and
wrong for a bandwidth-efficient collective, whose exchange partners are chosen
for their position in the *algorithm* rather than in the tree. At the default
radix a DVM of ≤65 daemons is flat, so daemon `p` sending to `p+1` would go
**through the HNP** — funnelling every byte through the one node such an
algorithm exists to keep out of the path.

`prte_rml_send_buffer_direct_nb()` (macro `PRTE_RML_SEND_DIRECT`) sets a
`direct` flag on the `prte_rml_send_t`; `prte_oob_base_send_nb` then uses
`msg->dst` as the hop instead of calling `prte_rml_get_route`. Nothing else
changes — the peer lookup finds or builds a connection from the target's
`PMIX_PROC_URI` exactly as it does for a tree neighbour, because the wireup
xcast gave every daemon every other daemon's URI. If the target's contact info
is not available the send **falls back to the routed path** rather than
failing, so a caller never has to handle "no direct route".

Three things about this are load-bearing:

- **A relayed message is never direct.** The relay in
  `oob_tcp_sendrecv.c` rebuilds the `prte_rml_send_t` field by field, so
  `direct` stays at its constructor default of `false`. Keep it that way: a
  message being forwarded by an intermediate hop is by definition being routed.
- **`send_cons()` must initialise `direct`.** `PMIX_NEW` mallocs, it does not
  zero, so a field the constructor forgets is heap garbage — here that would be
  a random subset of messages bypassing the tree.
- **The fallback clears the flag before retrying**, so it cannot loop, and so
  the relay bookkeeping stays honest if the message is forwarded later.

### Losing a lateral link is not a routing-tree fault

This is the part that is expensive to get wrong, and it has a single
choke point. `prte_rml_route_lost` is reached from both
`prte_mca_oob_tcp_component_lost_connection` and `..._failed_to_connect`, and
its default action is `prte_rml_repair_routing_tree()` — which marks the peer
failed, reshapes the tree, and notifies the grpcomm, filem and relm fault
handlers, ending every in-flight collective in the DVM. Doing that because a
*lateral* link dropped would be badly wrong: the connection that just died is
one some collective opened for bandwidth, not a lifeline.

So `prte_rml_base.lateral_links` records the ranks we hold a non-tree link to,
and `prte_rml_is_lateral_only()` is the single test the fault path uses:

- **Being in the tree wins over being registered.** A collective's exchange
  partner may happen to *be* our parent or one of our children; losing that
  connection is a genuine tree fault and must take the repair path. So the
  predicate answers false for any tree neighbour, registered or not.
- **A lateral loss is not a death diagnosis.** The gate deregisters the link
  and tells the registrant (`prte_rml_lateral_set_lost_callback`) so the
  collective can end or re-plan — and does nothing else. If the peer really
  died, the daemons for which it *is* a tree neighbour will detect that and the
  global fault notice arrives the usual way. Declaring it from here would be
  duplicative, and on a merely dropped socket simply wrong.
- **`lateral_links` is not re-initialised by `compute_routing_tree`**, for the
  same reason `dead_dmns` is not: a grow reshapes the tree but does not
  dissolve the exchange partners a collective is midway through talking to.

`test/unit/rml/test_rml_routing.c::test_lateral_links` pins all of that,
including the overlap case and survival across a recompute.

## Elastic DVM and launcher-less bootstrap

The tree is no longer fixed for the life of the DVM. In elastic mode it can
**grow** and **shrink**, and in a **bootstrapped** DVM the daemons come up
independently rather than being fanned out by a launcher. Both push new
behavior into the RML/OOB; keep these in mind when touching routing or the
connection path.

- **`dead_dmns` — a departure set that outlives a recompute.** `prte_rml_base`
  now carries three failure bitmaps, not two: `failed_dmns` and
  `global_failed_dmns` (both re-initialized on every `compute_routing_tree`)
  plus `dead_dmns`, which is constructed **once** in `prte_rml_open` and never
  re-initialized. `repair_routing_tree` records every departed rank in it, and
  `compute_routing_tree` restores those marks into the freshly-wiped
  `failed_dmns` before rebuilding ancestors/children. Without it, a grow would
  wipe the failure marks and route to the dead vpid of a shrunk-out daemon
  (#2491). The DVM never reuses a daemon vpid, so a hole in `[0, num_daemons)`
  is permanent.

- **A leaving daemon exits on the first lost route.** `prte_rml_route_lost`
  checks `prte_dvm_leaving` first: if this daemon has been named as a shrink
  target, it activates `PRTE_JOB_STATE_DAEMONS_TERMINATED` on *any* dropped
  connection rather than trying to recover. This prevents a departing daemon's
  normal disconnects from being misread as faults and propagated as adoption
  notices. It is only a fast path — a bounded timer in `prted_comm.c`
  guarantees departure even if no connection ever drops.

- **Bootstrap synthesizes peer URIs on demand.** Normally a peer's contact URI
  comes from the nidmap or the PMIx store. In a bootstrapped DVM no nidmap
  distributed URIs during formation, and a healed lifeline's new parent (a
  former grandparent) was never pre-synthesized. When `prte_bootstrap_setup` is
  set and no peer object exists, `prte_oob_base_send_nb` derives the next hop's
  URI from configuration via `prte_ess_base_bootstrap_peer_uri` and connects to
  it. A synthesized URI cannot know the peer's interface mask, so `set_addr`
  treats a missing/empty mask as `/0` (universally reachable) rather than
  rejecting the address.

- **Bootstrap tolerates a not-yet-present parent.** Because daemons boot
  independently, a parent may simply not be up when a child times out on it.
  In bootstrap mode, `prte_mca_oob_tcp_component_failed_to_connect` heals the
  tree the same way a lost live parent would — `prte_rml_route_lost` promotes
  to the next ancestor (a `COMM_FAILED` recovery) — instead of raising a fatal
  `FAILED_TO_CONNECT`. The HNP is the exception: it is retried forever and
  never allowed to time out.

- **Two new connection-retry knobs.** `prte_oob_base` gained `retry_max_delay`
  and `connect_max_time` (MCA params `prte_retry_max_delay`,
  `prte_connect_max_time`), and each peer gained a `first_attempt` timestamp.
  When `retry_max_delay > retry_delay`, the retry delay backs off exponentially
  (retry_delay, 2×, 4×, …) capped at `retry_max_delay`, so a daemon waiting on
  an absent peer polls fast at first and then settles onto a steady rate.
  `connect_max_time` bounds how long a **non-lifeline** peer is chased before
  giving up so the tree can heal to an ancestor; `0` (the default) means retry
  forever, preserving the original behavior. The HNP is never subject to
  `connect_max_time`.

## Who owns what — the memory rules

Nearly every defect this code has had is an ownership mistake, so the rules are
worth stating rather than re-deriving:

- **A send is completed, not released.** `PRTE_RML_SEND_COMPLETE(msg)` is the
  only correct way to finish a `prte_rml_send_t`: it clears the buffer pointer,
  fires the caller's callback with it, and then releases the send. A bare
  `PMIX_RELEASE(msg)` frees the buffer *without* telling the caller — fine for
  the default callback, silently wrong for anything that tracks completion (all
  of RELM). Every failure path in `prte_oob_base_send_nb` and
  `prte_oob_tcp_peer_close` must complete, not release.
- **The receive side owns its payload until someone takes it.** A
  `prte_oob_tcp_recv_t` frees `data` in its destructor. The two paths that hand
  the payload on — local delivery via `PMIx_Data_load`, and the relay — set
  `data = NULL` first. Anything else (a message dropped as a stale incarnation,
  a partial recv abandoned when the peer closed) is freed by the destructor.
- **A `prte_rml_recv_t` owns its buffer object even when it is empty.** The
  recv callback may unload the payload; the (now empty) `pmix_data_buffer_t`
  itself is still ours.
- **A stack `prte_rml_recovery_status_t` still needs destructing.** Its
  constructor heap-copies the current ancestor and child arrays, so *every*
  path out of `repair_routing_tree` — including the "no new information, just
  return" one, which is the common case under duplicate failure notices — has
  to `PMIX_DESTRUCT` it.
- **`PMIX_RELEASE` on a `pmix_list_t` does not touch its items.** Use
  `PMIX_LIST_RELEASE`/`PMIX_LIST_DESTRUCT` for a list whose items you built.
- **`prte_reachable_t` is a refcounted object**, not a plain struct — dispose
  of it with `PMIX_RELEASE`. `free()`ing it leaks the single block backing the
  whole weight matrix.
- **Compare `pmix_proc_t` nspaces with `PMIx_Check_nspace`.** `nspace` is a
  `char[]`; `a.nspace != b.nspace` compiles fine and compares two array
  *addresses*, which is always true.

## Gotchas before you edit

- **Single progress thread.** All RML/OOB state is owned by the progress
  thread. Cross-thread work uses a *caddy* + `PRTE_PMIX_THREADSHIFT` (see
  `PRTE_OOB_SEND`, `prte_rml_recv_buffer_nb`). Never read or write shared RML
  state off that thread, and never block on it.
- **The wire header is not an ABI.** `prte_oob_tcp_hdr_t` is exchanged only
  among daemons of the *same* DVM, which all run the same build. You may change
  its layout, but every daemon must agree — there is no versioning. It *is*
  byte-order converted, though, so keep `MCA_OOB_TCP_HDR_HTON`/`_NTOH` covering
  every multi-byte field you add, and zero a header before filling it — the
  whole struct goes on the wire, padding included.
- **One transport, one router.** Do not reintroduce component/module
  abstraction to "make it pluggable" unless there is a real second
  implementation; that abstraction is exactly what was removed.
- **Do not re-initialize `dead_dmns`.** It is deliberately the one failure
  bitmap that is *not* reset by `compute_routing_tree`; resetting it reopens the
  #2491 grow bug. `pmix_bitmap` auto-expands as ranks are set, so it needs no
  resizing when the DVM grows.
- **A tag nobody receives is a leak, not a no-op.** An arriving message with no
  matching posted recv is parked on `unmatched_msgs` *forever*. That is correct
  for a message whose recv is merely late, and wrong for a message that is only
  ever consumed inline — `PRTE_RML_TAG_WARMUP_CONNECTION` is the one such tag,
  and `prte_rml_base_process_msg` must dispose of it on every path rather than
  fall through to the matching loop.
- **`prte_oob_open` can fail for reasons a user causes** — an if_include or
  if_exclude that leaves no interface, a static port range nothing can bind.
  Check its return: the next thing `prte_rml_open` does is build a contact URI,
  and with no interface there is none to build.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.

## Testing

There are two layers, and the split is dictated by what needs a live DVM.

**Unit tests — `test/unit/rml/`, run by `make check`.**

| Binary | Covers |
|--------|--------|
| `test_rml` | `prte_oob_split_and_resolve`: turning an if_include/if_exclude string into the interface list the transport binds to. Pure parsing, no socket. |
| `test_rml_routing` | The radix math (`radix.h`), the routing tree (`compute_routing_tree`, `get_route`, `get_subtree_index`, `get_num_contributors`), the dead/absent-rank restoration across a recompute, the lateral-link registry and its overlap-with-the-tree rule, the boot-epoch incarnation guard, `prte_rml_purge`, and `prte_rml_parse_uris`. |

`test_rml_routing` stands `prte_rml_base` up by hand rather than calling
`prte_rml_open` (which would also start listeners), so adding a case is just a
matter of setting `radix`/`num_daemons`/`PRTE_PROC_MY_NAME->rank` and calling
`prte_rml_compute_routing_tree`. What it deliberately does **not** cover is
`repair_routing_tree` and `revive_routing_tree`: both end by calling the
grpcomm, filem, and relm fault handlers and by emitting notices over the RML,
none of which exist in a unit-test process.

**Multi-node — `contrib/dockerswarm`, the `test_rml` phase.** Everything the
unit tests cannot reach, because it needs real daemons on real sockets: a
message actually being *relayed* by an intermediate daemon (which needs a small
radix — at the default 64 a ten-node DVM is flat and nothing is ever relayed),
every daemon independently agreeing on the tree shape, a payload big enough to
drive the partial-write/partial-read bookkeeping, the max-message-size guard,
interface selection actually binding and connecting, and a daemon dying under a
live DVM so `prte_rml_route_lost` and the peer teardown run for real.
