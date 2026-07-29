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
| `oob_base_stubs.c` | `prte_oob_base_send_nb` — resolve the next hop via `prte_rml_get_route`, find or create its TCP peer, and queue the message (connecting first if needed). Also URI build (`get_addr`) and parse (`process_uri` / `set_addr`), including the bootstrap fallbacks. |
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
`prte_rml_get_route(dst)` and looks up that hop's `prte_oob_tcp_peer_t`. If no
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

Every message carries a `prte_oob_tcp_hdr_t` (`oob_tcp_hdr.h`): `origin`, final
`dst`, `tag`, a sequence number, payload length, the origin's boot `epoch`, and
a message `type` (`IDENT`/`PROBE` for the handshake, `USER` for a normal
message). It is exchanged **only** among daemons of the same DVM, which all run
the same build, so it is **not** a stable ABI — you may change its layout, but
every daemon must agree; there is no versioning.

Two rules that are not obvious from the struct:

- **Every multi-byte field is byte-order converted**, by
  `MCA_OOB_TCP_HDR_HTON`/`_NTOH`. Add a field, extend both macros — a field
  that is quietly not converted works perfectly until two daemons differ in
  endianness.
- **The whole struct goes on the wire, padding included.** The handshake builds
  its header on the stack, so zero it before filling it in; leaving a field (or
  the padding) undefined ships uninitialized bytes and trips every memory
  checker.

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

## Gotchas before you edit

- **Single progress thread.** All OOB state — peers, sockets, send queues — is
  owned by the progress thread. Cross-thread entry goes through `PRTE_OOB_SEND`
  (a caddy + `PRTE_PMIX_THREADSHIFT`). Never touch peer/socket state off that
  thread, and never block on it.
- **The header is not an ABI.** See above; do not add versioning, but do keep
  every daemon in a build in sync.
- **Finish a send, never just free it.** A `prte_rml_send_t` that is abandoned
  — no route, no peer, the connection torn down — must go through
  `PRTE_RML_SEND_COMPLETE` so the caller's callback runs with a status.
  `PMIX_RELEASE` frees the buffer and tells nobody, which is invisible for the
  default callback and a lost message for RELM.
- **The recv object owns its payload.** `prte_oob_tcp_recv_t`'s destructor
  frees `data`; the paths that hand the payload on (`PMIx_Data_load` for local
  delivery, the relay) null the pointer first. Add a third path and it has to
  do the same, or free it.
- **`prte_oob_base_send_nb` runs on a *hop*, and the hop can be
  `PMIX_RANK_INVALID`** when the target sits behind a hole the tree cannot
  reach past. Handle that before it reaches the peer lookup.
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
