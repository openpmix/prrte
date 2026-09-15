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
| `oob_tcp_listener.c` | Listening sockets - one per selected address, sharing a port - and the accept path. |
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
  `tcp_peer_read_handshake` refuses a peer whose nspace is not ours (and
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

## The connect handshake is read without waiting

The listening port accepts a connection from anything that can reach it, and
the IDENT handshake that follows is read on the **main progress thread**. So
it is read as its bytes arrive and never by waiting for them:
`prte_oob_tcp_peer_recv_connect_ack` takes whatever the socket has into a
`prte_oob_tcp_handshake_t` and returns `PRTE_ERR_WOULD_BLOCK` when that is not
yet everything; the caller comes back on the socket's next read event. It used
to loop in `recv()` until the handshake was complete, and one byte sent to the
port by anything at all - then silence - stopped the daemon dead: blocked in
the kernel on Linux, spinning on `EAGAIN` on macOS, where an accepted socket
inherits the listener's non-blocking flag. A job that should have ended in
three seconds ran until the stray connection closed.

Where the partial handshake lives depends on which way the connection runs,
because an inbound one arrives before we know whose it is:

| direction | record | re-entered by |
|---|---|---|
| inbound (`recv_handler` in `oob_tcp.c`) | the accept op's `hshake` | re-adding the op's one-shot read event |
| outbound, our dial (`CONNECT_ACK` in `oob_tcp_sendrecv.c`) | `peer->hshake` | the peer's persistent read event |

Three things keep that sound:

- **An accepted socket is made non-blocking before its handshake is read**
  (`prte_oob_accept_connection`), and one that cannot be is closed instead.
  Linux never lets an accepted socket inherit `O_NONBLOCK`, and a blocking one
  turns "read what has arrived" back into "wait for it".
- **The header is judged before any payload is sized from it.** The nspace
  check, the message type, the ack flag's minimum length and the
  `prte_max_msg_size` cap all run as soon as the fixed header and its nspace
  are in, so a connection that is not ours never gets to name an allocation.
  The payload is allocated one byte longer than sent and terminated, which lets
  the version compare be a plain `strcmp`.
- **`peer->hshake` is emptied whenever a new socket is made for the peer**
  (`tcp_peer_create_socket`), so a reply half-read on an abandoned attempt
  cannot be finished with the next attempt's bytes.

## A failed handshake has already been dealt with - do not close it again

Every failure `prte_oob_tcp_peer_recv_connect_ack` returns other than
`PRTE_ERR_WOULD_BLOCK` has already disposed of the connection: the peer
closed, or the bare socket when there is no peer. `tcp_peer_send_connect_ack`
by contrast closes nothing, and each of its three callers closes the peer once.

This is not tidiness. Closing a peer whose connection is still being
established **starts a new connection attempt** - that is how
`prte_oob_tcp_peer_close` rotates to the peer's next address - so a second
close schedules a second attempt alongside the first. The second one then
closes the first's socket, whose send event is armed, and re-sets that event:
an assertion in a debug build and a corrupted event queue in an optimized one.
Three places did exactly that: `tcp_peer_send_connect_ack` closed the peer and
then its callers closed it again (and `try_connect` dialed again on top, on
the theory that a failed send meant a simultaneous connect), and the
`CONNECT_ACK` handler closed after every handshake failure the ack had already
handled.

For the same reason **`prte_oob_tcp_peer_try_connect` dials only a peer in
`CONNECTING`**, which every path that schedules it sets first. A retry parked
on a timer outlives the attempt that parked it, and by the time it fires the
peer may have connected by dialing *us* - `retry()` lets that win - or have
nacked us so that it does the dialing. Without the test the stale retry closed
the live connection's socket to open its own.

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

## When a peer cannot be reached, say which kind of failure it is

`try_connect` giving up produces the message the user actually reads about a
lost daemon, and there are **two failures behind it that want opposite
advice**:

- A peer we have **never** reached may not have started yet, may have failed
  to start, or may be behind a firewall. Suspecting the configuration is fair.
- A peer we **have** reached is a different story. The connection worked once,
  which exonerates the network and the firewall by that fact alone; the daemon
  has gone away since. On a managed cluster the usual reason is that the
  resource manager reclaimed the allocation it was living in — a job cancelled,
  or one that hit its time limit. Telling *that* user to check `iptables`
  points them away from the answer.

`peer->ever_connected` is what separates them, and it is not the same question
as `peer->established`: that one is cleared by `prte_oob_tcp_peer_close` so it
always describes *the connection being closed*, which at the failure site is a
connection that never came up — false in both cases. `ever_connected` is set
wherever a connection reaches `CONNECTED` and is never cleared.

Note also which layer the user hears first. `errmgr/dvm` reports the loss
properly (`node-died`, "PRTE has lost communication with a remote daemon"), but
`show_help` aggregates by topic and this message usually arrives ahead of it,
so this is the text that gets read. Keep it accurate.

### Reaching that path on purpose: `prte_oob_silent_loss_vpid`

The second case is nearly impossible to arrange by timing. Losing a daemon
normally goes the other way entirely: this process sees the socket close,
reports the loss, the node is marked down, and every later message for it is
refused (`PRTE_ERR_NODE_DOWN`) before it reaches the oob at all. The connect
attempt only happens when a daemon has gone away while we still believe in it.

`prte_oob_silent_loss_vpid` names a daemon vpid whose departure this process
must pretend not to have noticed — `peer_close` skips the `lost_connection`
notification for it. The node stays up, the next message opens a fresh
connection, and the failure lands where the message lives. It is fault
injection, not a tuning knob, and like `odls_base_fork_publish_delay` it is
**deliberately not restricted to a debug build**: the behavior it exists to
reach is in the build that ships. `contrib/dockerswarm`'s `test_rml` uses it.

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

`peer_cons` asks the **process-wide worker pool**
([`src/runtime/prte_worker_pool.h`](../../runtime/prte_worker_pool.h)) for a
base at construction (`peer->evbase`); the pool holds
`prte_num_worker_threads` bases (default **8**) on a ring and hands them out in
rotation. The OOB does not own that pool — it is shared with the odls fork
path, and it is built by `prte_init` and torn down by `prte_finalize`. Setting
`prte_num_worker_threads` to 0 makes every assignment `prte_event_base`, which
is exactly how the transport ran before any of this existed.

The point is not extra bandwidth — one thread's `writev` copy rate is several
times a 10-25 GbE link — it is **occupancy**: while the main thread is
deflating an xcast, registering a namespace, or running the odls fork path,
nothing services a socket at all.

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

Three more rules came out of races between a worker closing a peer and the
main thread reconnecting it:

- **A working connection is taken apart before it is published CLOSED.**
  `CLOSED` is what the main thread acts on - the next message for the peer
  starts a new connection, which reuses `peer->sd` and both event structures -
  so `prte_oob_tcp_peer_close`'s established branch deletes the events, closes
  the socket and drops the partial recv *under the peer lock*, and sets
  `CLOSED` last. Published first, a worker preempted mid-teardown let the main
  thread re-set events the worker was still deleting, and let the worker's
  `close` land on the new attempt's descriptor. Holding the lock across those
  deletes is safe in that branch alone: a connection that reached `CONNECTED`
  is closed only by the thread servicing its socket, where its events live, so
  there is no other thread's handler to wait on. The branch for a connection
  still being established must keep dropping the lock first, as it does.
  Anything that decides on `CONNECTED` without the lock - `start_connect` -
  looks again once it holds it.
- **Only `tcp_peer_connected` publishes `CONNECTED`, and it arms both socket
  events while it holds the lock.** From that instant a worker may own the
  socket and may already have failed a send and closed the peer, so a caller
  that arms the read event afterwards, or writes `CONNECTED` again, can arm an
  event on a dead descriptor or bring a closed peer back to life with no
  socket at all.
- **The routing tree is not the worker's to read.** Sends stranded by a lost
  connection are failed with `NODE_DOWN` or `UNREACH` according to
  `prte_rml_is_node_up()`, which reads a bitmap a DVM resize reallocates. So
  `tcp_peer_fail_lost_sends` lifts them on the closing thread and asks that
  question on `prte_event_base`. The loss report that would mark the node down
  is posted after it, to the same queue, so the answer is the same one the
  close would have got.

`retry()`, adopting an inbound socket for a peer that may still read
`CONNECTED`, follows the first rule's order too: out of `CONNECTED` under the
lock, then the deletes (which wait out a handler in flight). It also throws
away the old stream's half-read message and restarts its half-sent one from
the header - the far end discarded both with its end of that connection, and
carried across they would splice two streams together.

With an empty pool, `peer->evbase` **is** `prte_event_base`:
`PRTE_OOB_COMPLETE_SEND` completes inline instead of posting, the rebind is a
delete/re-set onto the base the events were already on, and the mutex is
uncontended. `prte_worker_pool_assign()` never returns NULL, precisely so
`peer_cons` needs no special case — and a peer built before the pool is up, or
after it is gone, still gets a usable base.

The one piece of state outside this directory that a worker touches directly is
the RML's incarnation table (`prte_rml_epoch_ok`, bootstrap only), which
reallocates; it carries its own mutex in `src/rml/rml.c`. Anything else you make
a socket handler call has to be safe off the main thread or has to be shifted.

## Interface selection and the listeners

`prte_oob_open` picks the interfaces; `prte_oob_tcp_start_listening` opens a
socket on each. Two rules here are easy to break by "simplifying":

- **Loopback is decided after the include list, not before.** `interface_selected()`
  applies the address-family, `vir*`, and include/exclude filters. The master
  keeps loopback only when no non-loopback interface survived them *and*
  either an include list was given or the host has no non-loopback interface
  at all. Deciding over every interface on the host - as the code once did -
  silently discarded a loopback the user had explicitly included whenever the
  host had any other interface, so `prte_if_include=lo0` for a single-node job
  failed with "no usable network interfaces". Do not extend it to exclude
  lists: an exclude that removes every real interface must fail at startup
  (the `test_rml` swarm phase asserts that diagnostic). Falling back to
  loopback there started the DVM and handed every remote daemon an address it
  could not use. A daemon never keeps loopback: its peers are on other nodes.
- **Listeners bind the selected addresses, never the wildcard.** One socket
  per selected address (deduplicated - `pmix_if_list` can list an address
  twice), all of a family on **one port**. One port because `set_addr` pairs
  every address in a URI with the *first* port listed (`atoi` of the whole
  port field), and because a static port - and the bootstrap's synthesized
  URIs - assume the same port everywhere; per-address ports would advertise
  ports nobody dials. With a kernel-chosen port the first address picks it and
  the rest are bound to match, retrying a few times if another address has it
  taken. Binding the wildcard would accept connections on excluded interfaces,
  which is what made a loopback-confined macOS DVM still need the firewall's
  permission for incoming connections. A family that cannot be listened on is
  removed from the contact URI (`unadvertise()`), so no peer dials a dead port.
- **Each family carries its own mask list** (`ipv4masks`, `ipv6masks`),
  indexed like its address list. A single shared list gave an IPv6-enabled
  build's URIs the other family's prefix lengths.
- **`pmix_ifmatches` answers in PMIx status codes.** Anything other than
  `PMIX_SUCCESS` or `PMIX_ERR_NOT_FOUND` is a specification it could not parse;
  comparing its result against a `PRTE_ERR_*` code never matches.
- **An accept error is usually about the connection, not the listener.**
  `accept_error_is_transient()` lists the per-connection failures
  (`ECONNABORTED` for a client that reset first, and the pending network errors
  Linux passes back); only something else gives up on the listener, and the
  event path must `prte_event_del` the persistent event before closing its
  socket and mark `sd = -1` so the destructor does not close it again.

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
- **Every path that gives up on a peer drains its queue**, through
  `tcp_peer_lift_queued_sends()` — the one place that collects it, shared by
  `tcp_peer_fail_queued_sends()` and, for a lost connection,
  `tcp_peer_fail_lost_sends()` — so the rule above cannot be honored in one
  arm and forgotten in the next. It is not only `prte_oob_tcp_peer_close()`:
  `prte_oob_tcp_peer_try_connect()` gives up in four places of its own (out of
  memory, `socket()` or an unrecoverable `bind()` failing, no address
  succeeding), and all but one of those used to return without touching the
  queue. (A failed IDENT send is not one of them: that fails only the address,
  and `peer_close` moves on to the next with the queue intact.) They are
  reconnect paths as well as first-connect ones, so what is queued there can
  be real work rather than a handshake. Collect the on-deck `peer->send_msg`
  as well as `peer->send_queue` — it is not on the list and is the one most
  easily missed — lift both under `peer->lock`, and complete them outside it,
  since completion runs the originator's callback.
- **`CLOSE_THE_SOCKET` is PMIx's, and it clears the variable.** Every file
  here reaches PMIx's `src/mca/ptl/ptl_types.h` before
  `oob_tcp_connection.h`, whose own definition is `#ifndef`-guarded and so
  never used: the macro in effect skips a negative descriptor, closes, and
  sets its argument to `-1`. So `CLOSE_THE_SOCKET(peer->sd)` does leave
  `peer->sd` at `-1` - a review once took the dead definition at its word and
  "fixed" a stale descriptor that was never stale - and its argument must be
  an lvalue, which is why `prte_oob_accept_connection` closes its `const`
  descriptor with plain `shutdown`/`close`.
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
`test/unit/rml/test_rml`, which runs under `make check` with no DVM. So is
`prte_oob_open` itself, with real sockets (`test_listeners_bound_to_selection`:
no wildcard listener, every listener advertised, one port per family;
`test_loopback_include_honored`: a loopback-only include opens on the master,
binds only loopback, and is refused on a daemon; an exclude list that leaves
only loopback is refused on the master too). Those two must run before
`test_payload_outlives_sends`, whose `PMIx_server_finalize` takes PMIx's
interface list down for the rest of the binary. Also covered: the
handshake reader (`test_handshake_never_waits`, over a `socketpair`: a valid
IDENT one byte at a time, a lone byte, a foreign namespace, a payload too
short for its ack flag), the dial guard (`test_stale_attempt_does_not_dial`),
and the
queued-send drain above (`test_queued_sends_complete_on_close`), driven through
`prte_oob_tcp_peer_close()` because that is the one give-up path reachable
without sockets; the arms in `prte_oob_tcp_peer_try_connect()` share the same
drain but cannot be provoked from a unit test, since they need `socket()` or
`bind()` to fail. Everything
else in this directory needs sockets between real daemons and lives in the
`test_rml` phase of `contrib/dockerswarm/run-tests.sh`: relaying through an
intermediate hop (which needs `--prtemca rml_base_radix 2`, since a ten-node
DVM at the default radix 64 is flat), a payload large enough to force partial
writes and reads, the message-size guard, interface include/exclude actually
binding (and binding *only* the selected interface, on both the HNP and a
prted, checked with `ss`), a loopback-only single-node job, and the teardown
path when a daemon dies under a live DVM.
