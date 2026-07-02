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
| `rml_send.c` | `prte_rml_send_buffer_nb` (and the `_reliable_nb` variant). Short-circuits sends to self; otherwise hands the message to the OOB. |
| `rml_recv.c` | `prte_rml_recv_buffer_nb` / `prte_rml_recv_cancel`. Thread-shifts the request onto the progress thread. |
| `rml_base_msg_handlers.c` | The heart of matching: `prte_rml_base_post_recv` (add/cancel a posted recv), `prte_rml_base_process_msg` (deliver an arrived message to a posted recv, or hold it in `unmatched_msgs`). |
| `rml_base_contact.c`, `rml_contact.h` | `prte_rml_parse_uris` — split a contact URI into name + address list. |
| `rml_purge.c` | `prte_rml_purge` — drop posted recvs and held messages for a departed peer. |

**Routing (the radix tree):**

| File | Responsibility |
|------|----------------|
| `routed_radix.c` | `prte_rml_get_route` (next hop toward a target), subtree indexing, and the tree (re)computation used on startup and after faults: `compute_routing_tree`, `repair_routing_tree`, `update_ancestors`, promotion/descendant fixups, `route_lost`. |
| `radix.h` | Header-only radix-tree math over daemon ranks: parent/child/sibling navigation, `subtree_contains`/`subtree_index`, depth motion, and "next living" traversal that skips failed ranks. Pure functions on rank numbers, no I/O. |

**Fault tolerance / reliability (newer; not collapse cruft):**

| File | Responsibility |
|------|----------------|
| `rml_fault_handler.c` | The RML's own reaction to a recomputed tree: sets process states and drives death/adoption notices. |
| `relm/` | RELM — reliable messaging that survives daemon failures by re-driving messages over the repaired tree. Has its own small state machine (`relm/state_machine.c`, `relm/base/`). |

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
(default radix 64; `--prtemca prte_rml_radix`). `prte_rml_get_route(target)`
returns: `target` if it is me; my parent (lifeline) if `target` is outside my
subtree; otherwise the child whose subtree contains `target`. After a daemon
fault, `prte_rml_repair_routing_tree` recomputes ancestors, children, and
possible self-promotion, packages the delta into a
`prte_rml_recovery_status_t`, and notifies the RML, grpcomm, filem, and relm
fault handlers.

## Gotchas before you edit

- **Single progress thread.** All RML/OOB state is owned by the progress
  thread. Cross-thread work uses a *caddy* + `PRTE_PMIX_THREADSHIFT` (see
  `PRTE_OOB_SEND`, `prte_rml_recv_buffer_nb`). Never read or write shared RML
  state off that thread, and never block on it.
- **The wire header is not an ABI.** `prte_oob_tcp_hdr_t` is exchanged only
  among daemons of the *same* DVM, which all run the same build. You may change
  its layout, but every daemon must agree — there is no versioning.
- **One transport, one router.** Do not reintroduce component/module
  abstraction to "make it pluggable" unless there is a real second
  implementation; that abstraction is exactly what was removed.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.
