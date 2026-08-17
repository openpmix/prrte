# `src/rml/oob` — the TCP transport

This directory is the OOB ("out of band") transport: the code that actually
moves an RML message's bytes across a socket to the next daemon in the routing
tree. It is one layer below the RML API and routing logic in the parent
directory; read [`../AGENTS.md`](../AGENTS.md) first for the whole picture, and
[`docs/how-things-work/rml/oob.rst`](../../../docs/how-things-work/rml/oob.rst)
for the narrative walkthrough.

## History: one transport, not a framework

OOB was once an MCA framework with pluggable transport *components* (each with
swappable *modules*) — TCP, and room for others. PRRTE only ever shipped TCP.
The framework was collapsed into this single directory. Comments or names that
speak of "trying the next component," "another transport module," or a
component-selection loop are **historical cruft** — there is exactly one
transport, TCP. If you find such language, fix it. Do **not** reintroduce the
component/module abstraction to "make it pluggable" unless a real second
transport actually exists; that abstraction is precisely what was removed.

## File map

| File | Responsibility |
|------|----------------|
| `oob.h` | The `prte_oob_base` global (peers list, interfaces, listeners, TCP tuning params) and the `PRTE_OOB_SEND` entry macro that thread-shifts a send onto the progress thread. |
| `oob_base_stubs.c` | `prte_oob_base_send_nb` — resolve the next hop (via `prte_rml_get_route`, or the target itself for a `direct` send), find or create its TCP peer, and queue the message (connecting first if needed). Also URI build (`get_addr`) and parse (`process_uri` / `set_addr`), including the bootstrap fallbacks. |
| `oob_tcp.c` | OOB `open`/`close`/`register`: MCA parameter registration, local interface discovery, listener startup, the connection-handshake `recv_handler`, and `simulate_node_failure` (test hook). |
| `oob_tcp_component.c` | Class instances for peers/addresses/messages, plus the `lost_connection` and `failed_to_connect` event handlers. |
| `oob_tcp_connection.c` | The per-peer connection state machine: connect with retry/backoff, the IDENT ack/nack handshake, accept, and close. |
| `oob_tcp_listener.c` | Listening sockets and the accept path. |
| `oob_tcp_sendrecv.c`, `.h` | The socket send/recv event handlers and their queueing macros. The recv-completion path decides **deliver locally vs. relay onward**. |
| `oob_tcp_hdr.h` | The on-wire message header, `prte_oob_tcp_hdr_t`. |
| `oob_tcp_peer.h` | The peer object (`prte_oob_tcp_peer_t`): name, addresses, socket, state, send queue, retry bookkeeping. |
| `oob_tcp_common.[ch]` | Socket-option helpers and state-name strings. |
| `help-oob-tcp.txt` | `pmix_show_help` text for TCP errors. |

## The send path in one paragraph

`PRTE_OOB_SEND(msg)` thread-shifts to `prte_oob_base_send_nb`
(`oob_base_stubs.c`). It drops the message if the destination is down or the
retry budget (`prte_rml_max_retries`) is exhausted, reporting failure through
the send callback. Otherwise it computes the next hop with
`prte_rml_get_route(dst)` — or, for a `direct` send, uses `dst` itself, falling
back to the routed hop if that peer cannot be resolved — and looks up that
hop's `prte_oob_tcp_peer_t`. If no
peer exists it obtains the contact URI (directly for the HNP, from the PMIx
store otherwise, or — in a bootstrapped DVM — synthesized via
`prte_ess_base_bootstrap_peer_uri`) and builds one with `process_uri`. If the
peer is connected the message is queued for immediate send
(`MCA_OOB_TCP_QUEUE_SEND`); otherwise it is queued pending
(`MCA_OOB_TCP_QUEUE_PENDING`) and the connection state machine in
`oob_tcp_connection.c` is started. Once the socket is up and the IDENT
handshake has completed, the send handler in `oob_tcp_sendrecv.c` writes the
header then the payload.

## The recv path in one paragraph

When a peer's socket has delivered a complete message, the recv handler in
`oob_tcp_sendrecv.c` inspects `hdr.dst`. If it is us, it calls
`PRTE_RML_POST_MESSAGE` to hand the message up to the RML matching layer. If it
is not us, this daemon is an intermediate hop: the handler rebuilds a
`prte_rml_send_t` with the same `origin`/`dst` and re-enters `PRTE_OOB_SEND`, so
relaying is just "send again from here."

## The wire header

Every message carries a `prte_oob_tcp_hdr_t` (`oob_tcp_hdr.h`): the origin's
boot `epoch`, the `origin` and final `dst` **ranks**, the `tag`, a sequence
number, the payload length, and a message `type` (`IDENT`/`PROBE` for the
handshake, `USER` for a normal message). It is exchanged **only** among daemons
of the same DVM, which all run the same build, so it is **not** a stable ABI —
you may change its layout, but every daemon must agree; there is no versioning.

Five rules that are not obvious from the struct:

- **A data message carries no namespace at all.** Its wire length is exactly
  `PRTE_OOB_TCP_HDR_FIXED` — 30 bytes — and the receiver rebuilds both procids
  with *its own* nspace via `PRTE_OOB_TCP_HDR_PROC()`. Three things make that
  safe rather than merely usually-true: only `ess/hnp` and the prted path open
  an OOB endpoint (no tool and no application process has one), every send
  entry point takes a **rank** which `send_buffer()` resolves in
  `PRTE_PROC_MY_NAME->nspace`, and a relay only forwards traffic of its own
  job. So a foreign namespace is unrepresentable, not just unused. This is
  what the header costs now; it began as two whole `pmix_proc_t` at 552 bytes
  and rides *every* RML message — for a cpuset slice with 101 bytes of
  payload it was 85% of the message.
- **The connect handshake does carry one, and that is where it is checked.**
  `tcp_peer_recv_connect_ack` refuses a peer whose nspace is not ours (and
  refuses one that gives none, which would otherwise fall back to ours and
  defeat the check). Once per connection, instead of restated on every
  message. Do not remove it to "simplify": it is the enforcement that lets
  every other header omit the field.
- **`nslen` describes the wire length by itself.** The handshake sets it, a
  data header sets it to zero, and both are read the same way — fixed part
  first, then `nslen` characters, then the terminator the receiver supplies
  (`PRTE_OOB_TCP_HDR_END_NSPACE`). Anything that sends or reads a header uses
  `PRTE_OOB_TCP_HDR_FIXED` and `PRTE_OOB_TCP_HDR_LEN()`, **never**
  `sizeof(prte_oob_tcp_hdr_t)` — the struct is 288 bytes, because the nspace
  array is the receiver's landing space.
- **Reading a name out of a header takes two reads off the socket**, even
  though the second is empty for a data message: `hdr_recvd` is not enough to
  build a `pmix_proc_t`, `nspace_recvd` is the flag that says the names are
  complete.
- **Every multi-byte field is byte-order converted**, by
  `MCA_OOB_TCP_HDR_HTON`/`_NTOH`. Add a field, extend both macros — a field
  that is quietly not converted works perfectly until two daemons differ in
  endianness. `nslen` is a single byte and needs no conversion, which is why
  the length macros may be used on a header in either order.
- **The fixed part goes on the wire whole, padding included.** The handshake
  builds its header on the stack, so zero it before filling it in; leaving a
  field (or the padding) undefined ships uninitialized bytes and trips every
  memory checker.

## Message-size bound

`prte_max_msg_size` (MBytes, default 100) bounds what a *receiving* daemon will
`malloc` for an incoming message. This matters more than a tuning knob usually
does: the length comes straight off the wire, so without the check a peer
dictates the allocation. It is enforced in two places — the normal recv path in
`oob_tcp_sendrecv.c` and the handshake in `oob_tcp_connection.c` — and both
respond by refusing the message and closing the connection with the
`msg-too-big` help text.

Note when trying to *test* this: PMIx compresses the launch buffer, so a large
payload of repeated bytes shrinks to nothing on the wire and sails under any
cap. Driving the cap to `0` is the unambiguous probe.

## Connection retry and backoff

`prte_oob_tcp_peer_try_connect` (`oob_tcp_connection.c`) drives retries. The
base case is a fixed `retry_delay`-second wait, bounded by `max_recon_attempts`.
Two knobs modify this (both default to preserving the original behavior):

- **`prte_retry_max_delay`** (`prte_oob_base.retry_max_delay`): when larger than
  `retry_delay`, the delay backs off exponentially — `retry_delay`, 2×, 4×, …,
  capped at `retry_max_delay`. The exponent is clamped before the shift so an
  unbounded `num_retries` cannot overflow.
- **`prte_connect_max_time`** (`prte_oob_base.connect_max_time`): caps how long
  a **non-lifeline** peer is chased (measured from the peer's `first_attempt`
  timestamp) before giving up so the routing tree can heal to an ancestor. `0`
  means retry forever. The HNP is never subject to this — it is retried forever.

## Bootstrap specifics

In a launcher-less (bootstrapped) DVM daemons boot independently, so:

- **Peer URIs are synthesized on demand.** With `prte_bootstrap_setup` set and
  no peer object present, `prte_oob_base_send_nb` derives the next hop's URI
  from configuration rather than the (absent) nidmap. See
  `prte_ess_base_bootstrap_peer_uri`.
- **Missing interface masks are tolerated.** A synthesized URI cannot know the
  peer's interface mask, so `set_addr` treats a missing/empty mask as `/0`
  (universally reachable) instead of rejecting the address.
- **A not-yet-present parent is not fatal.** In bootstrap mode,
  `prte_mca_oob_tcp_component_failed_to_connect` heals the tree via
  `prte_rml_route_lost` (promoting to the next ancestor, a `COMM_FAILED`
  recovery) rather than raising `FAILED_TO_CONNECT`.

## Which thread services a peer's socket

By default, all of it runs on `prte_event_base` — one progress thread, exactly
as it always did. `prte_oob_progress_threads` (`prte_oob_base.num_progress_threads`,
default **0**) starts N worker progress threads and hands each peer one of them,
round-robin, at construction (`peer->evbase`). The point is not extra bandwidth —
one thread's `writev` copy rate is several times a 10-25 GbE link — it is
**occupancy**: while the main thread is deflating an xcast, registering a
namespace, or running the odls fork path, nothing services a socket at all.

The split is deliberate and narrow:

| Runs on `prte_event_base`, always | Runs on `peer->evbase` |
|---|---|
| the connection state machine (`try_connect`, `complete_connect`, the IDENT handshake, `accept`) | the send handler and the recv handler, once **CONNECTED** |
| routing (`prte_rml_get_route`), the peer table, `prte_oob_base_send_nb` | `MCA_OOB_TCP_QUEUE_MSG` and the send queue |
| every send **completion** callback, and the `lost_connection` notice | the `writev`/`read` themselves |

Four rules keep that safe, and all four are load-bearing:

- **Events start on the main base and move at CONNECTED.** `tcp_peer_event_init`
  binds to `prte_event_base`; `tcp_peer_connected` calls `tcp_peer_rebind_events`,
  which deletes both events and re-`event_set`s them on `peer->evbase`. libevent
  will not re-target a *pending* event, so the delete is not optional — and it
  clears the `*_ev_active` flags, which is why every caller re-adds afterwards.
- **`peer->lock` guards `send_msg`/`send_queue`/`send_ev_active`**, and the
  once-only state transition at the top of `prte_oob_tcp_peer_close`. It is held
  across queue manipulation only — **never** across a `writev` and never across a
  callback. The lock is always the *outer* lock: a holder may take a libevent
  base lock (`event_add`/`event_del`), never the reverse.
- **`prte_event_del` is the synchronization with a running handler.** Deleting an
  event whose callback is executing on another thread blocks until that callback
  returns. That is what makes the rest of `peer_close` safe — once both socket
  events are gone, no handler for that peer can be in flight, so `recv_msg` and
  the socket are the closer's alone. It is also why `peer_close` must release
  `peer->lock` *before* those deletes: the handler it is waiting for may be
  waiting for the lock.
- **A worker never writes the peer's connection state.** `prte_oob_tcp_queue_msg`
  finding the peer unconnected posts `prte_oob_tcp_peer_start_connect` to the
  main base rather than setting `MCA_OOB_TCP_CONNECTING` itself.
- **Whoever observes CONNECTED last arms the send event.** Both
  `prte_oob_tcp_queue_msg` and `tcp_peer_connected` end by checking "peer is
  CONNECTED and has a message on deck with no send event running — arm it",
  and *both* checks are required because they now race. `MCA_OOB_TCP_QUEUE_PENDING`
  passes `activate = false`, which used to mean "the connection machinery will
  start this send" — true only while the queueing ran on the same thread. If
  `tcp_peer_connected` wins the race it finds an empty queue and arms nothing;
  the pending `queue_msg` then arrives, parks the message on deck, and, seeing
  `activate` false, arms nothing either. The message sits on a live connection
  behind a dead event forever. That is not a slow path — it wedged an 8-node
  radix-2 launch roughly one run in three, with every thread idle in
  `epoll_wait` and a single peer showing `send_msg != NULL, send_ev_active ==
  false`. If you add a third place that queues onto a peer, it needs the same
  tail.

With `num_progress_threads == 0`, `peer->evbase` **is** `prte_event_base`:
`PRTE_OOB_COMPLETE_SEND` completes inline instead of posting, the rebind is a
delete/re-set onto the base the events were already on, and the mutex is
uncontended. `prte_oob_base.ev_bases` still exists and still holds one entry, so
every call site indexes it unconditionally — do not "optimize" that array away.

The one piece of state outside this directory that a worker touches directly is
the RML's incarnation table (`prte_rml_epoch_ok`, bootstrap only), which
reallocates; it carries its own mutex in `src/rml/rml.c`. Anything else you make
a socket handler call has to be safe off the main thread or has to be shifted.

## Gotchas before you edit

- **Peer/socket state belongs to one thread at a time.** Cross-thread entry into
  the OOB goes through `PRTE_OOB_SEND` (a caddy + `PRTE_PMIX_THREADSHIFT`), which
  always lands on `prte_event_base`. Never touch peer state from an arbitrary
  thread, and never block on any of these threads. See the section above for
  what the worker bases are allowed to touch.
- **The header is not an ABI.** See above; do not add versioning, but do keep
  every daemon in a build in sync.
- **Finish a send, never just free it.** A `prte_rml_send_t` that is abandoned
  — no route, no peer, the connection torn down — must go through
  `PRTE_RML_SEND_COMPLETE` so the caller's callback runs with a status.
  `PMIX_RELEASE` frees the buffer and tells nobody, which is invisible for the
  default callback and a lost message for RELM. From inside the transport, use
  `PRTE_OOB_COMPLETE_SEND(peer, msg)` instead: it completes inline when the peer
  is on the main base and posts the completion there when it is not.
- **The recv object owns its payload.** `prte_oob_tcp_recv_t`'s destructor
  frees `data`; the paths that hand the payload on (`PMIx_Data_load` for local
  delivery, the relay) null the pointer first. Add a third path and it has to
  do the same, or free it.
- **A `direct` send is the one case where the hop is the destination.** It
  exists so a bandwidth-efficient collective is not funnelled through the root;
  see [`../AGENTS.md`](../AGENTS.md), *Lateral links*. Losing such a connection
  must not be reported as a routing-tree fault.
- **`prte_oob_base_send_nb` runs on a *hop*, and the hop can be
  `PMIX_RANK_INVALID`** when the target sits behind a hole the tree cannot
  reach past. Handle that before it reaches the peer lookup.
- **`PRTE_MODEX_RECV_VALUE_OPTIONAL` yields a `pmix_status_t`.** The URI
  lookup in `prte_oob_base_send_nb` is the only use of it in this directory.
  Comparing its result against `PRTE_SUCCESS` works — both spaces call
  success 0 — but it is comparing against the wrong space, and anything
  beyond the success test needs `prte_pmix_convert_status()` first. See
  [`../../pmix/AGENTS.md`](../../pmix/AGENTS.md).
- **Don't tear down a peer over one bad address.** `set_addr` parses a URI that
  may name several addresses, and the peer object it is filling in may be an
  existing one with a live socket and queued sends. A malformed address is a
  reason to skip that address, not to remove the peer from `prte_oob_base.peers`.
- **`prte_reachable.reachable()` returns a refcounted object**, and the
  `pmix_list_t` of remote interfaces you build for it holds objects the list
  destructor will not touch. `PMIX_RELEASE` the former, `PMIX_LIST_RELEASE` the
  latter.
- **`prte_oob_open` has real failure modes.** No usable interface (an
  if_include/if_exclude that leaves nothing) and no bindable port both return an
  error with a `show_help` explaining it. Callers must not walk on.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.

## Testing

`prte_oob_split_and_resolve` — the interface-selection parser — is covered by
`test/unit/rml/test_rml`, which runs under `make check` with no DVM. Everything
else in this directory needs sockets between real daemons and lives in the
`test_rml` phase of `contrib/dockerswarm/run-tests.sh`: relaying through an
intermediate hop (which needs `--prtemca rml_base_radix 2`, since a ten-node
DVM at the default radix 64 is flat), a payload large enough to force partial
writes and reads, the message-size guard, interface include/exclude actually
binding, and the teardown path when a daemon dies under a live DVM.
