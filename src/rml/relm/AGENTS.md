# `src/rml/relm` — RELM, the reliable-messaging layer

RELM ("reliable messaging") sits *on top of* the RML. A normal RML send is
fire-and-forget: if a daemon on the path dies while a message is in flight, the
message is simply lost. RELM guarantees delivery across daemon failures by
tracking each message's state hop-by-hop and re-driving it over the routing tree
after the tree has been repaired. It is newer than the collapsed RML core and is
intentionally kept as its own module.

Read [`../AGENTS.md`](../AGENTS.md) for how RELM plugs into the RML, and
[`docs/how-things-work/rml/relm.rst`](../../../docs/how-things-work/rml/relm.rst)
for the full protocol walkthrough. The base implementation lives in
[`base/`](base/AGENTS.md).

## How it is reached

`prte_rml_send_buffer_reliable_nb` (`../rml_send.c`, wrapped by the
`PRTE_RML_RELIABLE_SEND` macro) calls `prte_relm.reliable_send`, which is
`prte_relm_start_msg`. That is the only entry point application code uses;
everything else in this directory runs in response to received RELM control
messages or to fault notices from the routing layer.

## The module indirection

RELM is structured like a mini-framework so alternative reliability strategies
could be dropped in, but today there is exactly **one** implementation — the
base module. `relm.c` copies `prte_relm_base_module` into the global
`prte_relm` at open time. The state machine
(`prte_relm_state_machine_t`, `state_machine.h`) holds function pointers for
every customization point (`new_rank`, `pack_state_update`, `update_state`,
`upstream_rank`, `downstream_rank`, `fault_handler`, …); `base/` fills them all
in. Do not add a second module speculatively.

## File map

| File | Responsibility |
|------|----------------|
| `relm.h`, `relm.c` | The `prte_relm_module_t` interface and the global `prte_relm`; `register`/`open`/`close`. `open` installs the base module. |
| `types.h`, `types.c` | The core objects: `prte_relm_msg_t` (per-message state + optional data), `prte_relm_rank_t` (per-destination message table), `prte_relm_signature_t`/GUID identity, the `prte_relm_state_t` enum, and UID sentinels. |
| `state_machine.h`, `state_machine.c` | The `prte_relm_state_machine_t` object and the generic engine: message lookup/creation (`find`/`get`), message ordering via prev/next UID links, `start_msg`, `release_msg`, the send-upstream/send-downstream state emitters, and the received-message and link-update handlers. |
| `util.h`, `util.c` | Pack/unpack helpers for signatures, states, UIDs, and data; the local post helper (`prte_relm_post`); state-name strings; and the `PRTE_RELM_*` output/error macros. |
| `base/` | The one concrete implementation of the state machine's callbacks. See [`base/AGENTS.md`](base/AGENTS.md). |

## The model in one paragraph

Every reliable message has a globally-unique identity — the pair `<src, uid>`,
extended with `dst` to a signature, hashed to a `prte_relm_guid_t`. Each daemon
on the path keeps a `prte_relm_msg_t` recording where that message is in its
lifecycle (`SENDING` → `SENT` → `ACKED` → `ACKACKED`, with `REQUESTED` for
replay and `PENDING` for ordering), plus the message data while it may still be
needed. Messages to the same destination are chained by `prev_uid`/`next_uid` so
ordering is preserved and an ACK implicitly acks everything before it. State
changes propagate as small RELM control messages (`PRTE_RML_TAG_RELM_STATE`)
sent one hop upstream (toward `src`) or downstream (toward `dst`) using ordinary
`prte_rml_get_route` hops. When the routing tree changes, the fault handler
purges dead paths and exchanges **link updates** (`PRTE_RML_TAG_RELM_LINK`) with
new neighbors so in-flight messages resume over the repaired tree.

## Gotchas before you edit

- **Single progress thread.** Like the rest of the RML, all RELM state is owned
  by the progress thread. `prte_relm_start_msg` packs on the caller's thread but
  hands off with `PRTE_PMIX_THREADSHIFT`; everything else already runs on the
  progress thread.
- **Ephemeral vs. lasting states.** States at or after
  `PRTE_RELM_EPHEMERAL_STATES_START` (`NEW`, `ACKACKED`, `CACHED`, `EVICTED`)
  drive transitions but must **never** be stored as a message's `state`. The
  engine asserts this; don't defeat it.
- **UIDs wrap.** `prte_relm_uid_t` is a `uint32_t` that is allowed to wrap; the
  design assumes a message is globally complete (and dereferenced) long before
  its UID is reused. Reserved sentinels (`UNKNOWN`/`NONE`/`INVALID`) sit at the
  top of the range — respect `PRTE_RELM_UID_MAX`.
- **Data lifetime.** A message's `data` is unloaded/emptied when it is posted or
  evicted; cached data is dropped by timer (`relm_base_cache_ms`) or when the
  cache exceeds `relm_base_cache_max_count`. Don't assume `msg->data.bytes` is
  non-NULL.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.
