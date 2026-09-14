# `src/rml/relm` — RELM, the reliable-messaging layer

RELM ("reliable messaging") sits *on top of* the RML. A normal RML send is
fire-and-forget: if a daemon on the path dies while a message is in flight, the
message is simply lost. RELM guarantees delivery across daemon failures by
tracking each message's state hop-by-hop and re-driving it over the routing tree
after the tree has been repaired. It is newer than the collapsed RML core and is
kept as its own subdirectory.

Read [`../AGENTS.md`](../AGENTS.md) for how RELM plugs into the RML, and
[`docs/how-things-work/rml/relm.rst`](../../../docs/how-things-work/rml/relm.rst)
for the full protocol walkthrough.

## How it is reached

`prte_rml_send_buffer_reliable_nb` (`../rml_send.c`, wrapped by the
`PRTE_RML_RELIABLE_SEND` macro) calls `prte_relm_start_msg`. That is the only
entry point application code uses; everything else in this directory runs in
response to received RELM control messages or to fault notices from the routing
layer.

`relm.h` is the whole of RELM's interface to the rest of the tree — four
symbols: `prte_relm_start_msg` (from `../rml_send.c`),
`prte_relm_fault_handler` (from `../routed_radix.c`), and
`prte_relm_register`/`open`/`close` (from `../rml.c`). Keep it that way; a
fifth caller reaching past those is a sign the layering is being bypassed.

## There is one implementation, and the code says so

RELM used to be shaped like a mini-framework: a `prte_relm_module_t` of
function pointers copied into a global `prte_relm` at open time, a `base/`
subdirectory supplying the one implementation, and a second bundle of nine
callbacks on the state machine that `base`'s `init()` wired to its own
functions. Nothing ever selected anything — there was no second module, and no
MCA variable that could have chosen one — so the layer of indirection cost a
reader two hops to find code that was never in doubt, and `docs/todo.rst`
carried a standing item about the selector that did not exist.

That is gone. The protocol's entry points (`prte_relm_pack_state_update`,
`prte_relm_handle_state_update`, `prte_relm_pack_link_update`,
`prte_relm_handle_link_update`, `prte_relm_upstream_rank`,
`prte_relm_downstream_rank`, `prte_relm_fault_handler`) are ordinary functions
that the generic engine calls directly, and the `base/` subdirectory has been
flattened into this one — the same shape `src/rml` and `src/grpcomm` took when
their frameworks were collapsed. The **structural** split is still here and is
the one worth keeping: `state_machine.c` is the generic engine (identity,
lookup, ordering, the send emitters) and `state_updates.c`/`link_updates.c` are
the protocol built on it. That is a compile-time boundary, not a dispatch one.

If a genuinely different reliability strategy ever turns up, reintroduce the
dispatch then, with the second implementation in hand — the interface is four
symbols wide, so it is a cheap thing to add and was an expensive thing to
maintain empty.

## File map

| File | Responsibility |
|------|----------------|
| `relm.h`, `relm.c` | The subsystem's outward interface, and the `prte_relm_base` config (verbosity, cache TTL `cache_ms`, cache cap `cache_max_count`). `register` registers the MCA params; `open` builds the state machine and posts the two persistent RELM receives (`PRTE_RML_TAG_RELM_STATE`, `PRTE_RML_TAG_RELM_LINK`); `close` reverses it. |
| `types.h`, `types.c` | The core objects: `prte_relm_msg_t` (per-message state + optional data), `prte_relm_rank_t` (per-destination message table), `prte_relm_signature_t`/GUID identity, the `prte_relm_state_t` enum, and UID sentinels. |
| `state_machine.h`, `state_machine.c` | The `prte_relm_state_machine_t` object (the live message tables, the cache, the link bitmaps, the UID counter) and the generic engine: message lookup/creation (`find`/`get`), message ordering via prev/next UID links, `start_msg`, `release_msg`, the send-upstream/send-downstream state emitters, and the received-message and link-update handlers. |
| `util.h`, `util.c` | Pack/unpack helpers for signatures, states, UIDs, and data; the local post helper (`prte_relm_post`); state-name strings; and the `PRTE_RELM_*` output/error macros. |
| `state_updates.c` | The heart of the protocol: `prte_relm_handle_state_update` dispatches a state change by who originated it — `local_update`, `downstream_update` (from the neighbor toward `dst`), or `upstream_update` (from the neighbor toward `src`) — plus `prte_relm_pack_state_update` and the cache-eviction timer callback. |
| `link_updates.c` | Post-fault recovery: `prte_relm_pack_link_update`/`prte_relm_handle_link_update` exchange in-flight message state with new neighbors after a promotion, `prte_relm_fault_handler` reacts to a `PRTE_RML_FAULT_SCOPE_LOCAL` recovery (purge dead paths, then re-exchange), and the upstream/downstream "links updated" bitmaps gate when updates may be sent. |

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
  engine asserts this; don't defeat it. There is exactly one sanctioned
  exception, and it is scoped to a single call: `ACKACKED` is written into
  `msg->state` for the length of `prte_relm_send_state_downstream()`, because
  that emitter packs whatever `msg->state` holds, and the previous state is
  put back before anything else runs. It used to be left there, and the
  message it was left on is about to be released — which does not make it
  unreachable (next point). What found it there was `prte_relm_update_state()`,
  refusing and failing the job: from a late send completion, or from the
  destructor's own eviction of a message still cached, which also left the
  freed message linked on `cached_messages`.
- **A message can outlive its release, so a reference is not a message.**
  `prte_relm_release_msg()` drops the table's reference, but
  `prte_relm_send_state_downstream()` holds another for its completion
  callback, and a send to a peer that has died completes only when the
  transport gives up — on a node that vanished without a RST, long after the
  tree was repaired and the message delivered, ACKed and released some other
  way, or purged by the fault handler. So the completion callback acts only if
  `prte_relm_find_msg()` still answers that same object. Anything else holding
  a `prte_relm_msg_t *` across an event, or across a call that can complete a
  message, has to ask the table the same way (the PENDING drain does).
- **A send that failed is not a send, and `SENDING` ignores replay requests.**
  A request for a message that is `SENDING` is taken for the send already
  under way — which, to a dead hop, will never arrive. The completion callback
  still records `SENT` whatever the status, and relies on the link-update
  exchange to repair; but if the status is a failure and the route to `dst`
  has moved since the send was queued, that exchange has already happened
  while the message sat queued, and the request it produced was ignored. So
  the callback resends on the new route. A duplicate is safe everywhere: an
  intermediate keeps the bytes it holds, and a destination that has posted the
  message only ACKs again.
- **A link update's `SENT` is a report about the *sender*.** Upstream packs
  `SENT` for every message it has passed toward the receiver. A receiver with
  no record of the message (`INVALID`) did not get it — the only copy died
  with the daemon that used to sit between them — and it must ask for the
  replay itself: nothing below it can have the message, and a link update is
  only sent on links that changed, so no one further down may ever hear of it.
  The destination must not take the generic local `SENT` transition at all
  (a local `SENT` means *we* forwarded, which a destination never does); it
  re-sends upstream what it had told the lost link, an ACK or a request. Both
  cases used to fall through to the generic transition — an intermediate
  recorded `SENT` and waited, a destination logged `BAD_PARAM` — and the
  message was never delivered, with its data pinned at the source for the life
  of the DVM.
- **Decide to ignore an update before looking the message up.**
  `prte_relm_get_msg()` *creates* a message it has no record of, and
  `prte_relm_get_prev_msg()` its predecessor, both `INVALID` — and nothing
  releases an `INVALID` message short of its source dying. So
  `prte_relm_message_handler()` drops an update from a rank that is neither the
  message's upstream nor its downstream neighbour (a lingering one from a
  replaced link), and one for a message whose source is down (already purged),
  before the lookup, consuming the rest of the update so a link update carrying
  several stays aligned. `prte_relm_handle_state_update()` still ignores an
  off-path sender, but by then the lookup has happened. An out-of-range
  signature is *not* dropped there: that is a broken peer, and `get_msg`
  refusing it fails the job.
- **Nothing is sent upstream for a source that is down.** Everything that
  travels upstream is ultimately for the source, and the route toward a dead
  rank leads to that rank or to none, which `prte_rml_send_buffer_nb()` refuses
  — and a refused RELM send fails the job. Daemons learn of a death at
  different moments, so a request from one that has not heard yet can reach one
  that has; `prte_relm_send_state_upstream()` drops it instead.
- **Draining a PENDING backlog is a loop, not recursion.** Posting a message
  ACKs it, and the ACK is what posts its successor. Written the obvious way
  that is several stack frames per message, and a backlog is exactly what
  builds up behind a message lost to a daemon failure until its replay lands:
  the unit test's 50,000 is past what a debug build's 8 MB stack survives. The
  outermost `ACKED` walks the chain (`waking_pending` stops the nested ones),
  and re-finds each message after posting it rather than trusting the
  pointer.
- **UIDs wrap, and the wrap has to step over the sentinels.**
  `prte_relm_uid_t` is a `uint32_t` that is allowed to wrap; the design assumes
  a message is globally complete (and dereferenced) long before its UID is
  reused. Reserved sentinels (`UNKNOWN`/`NONE`/`INVALID`) sit at the top of the
  range, *above* `PRTE_RELM_UID_MAX` — which is why the counter is handed out
  by `prte_relm_next_uid()` and not by a bare `next_uid++`. The bare increment
  walked through all three on its way back to zero, and a UID above the max is
  not a wrap but a poisoned signature: `prte_relm_get_msg()` refuses it and
  answers NULL, so three of every 2^32 reliable messages took the daemon down
  instead of being sent. Every lookup helper enforces `PRTE_RELM_UID_MAX`;
  anything that generates a UID must respect it.
- **A signature collision is fatal, by decision.** Two live messages under one
  `<src,uid,dst>` cannot both be delivered, and the one that is dropped is
  silently lost by the layer that exists to not lose it — with no way to tell
  which of the two is real. So both detectors in `state_updates.c` (a
  second `NEW` for a UID that already names a message, and a `SENDING` whose
  payload differs from the one already held) report and activate
  `PRTE_JOB_STATE_FORCED_EXIT`, like every other broken invariant here. They
  used to log `PRTE_ERR_OP_IN_PROGRESS` and carry on. Since the UIDs are
  generated, reaching either detector means the identity space is already
  broken; there is no narrower repair, because handing the message a different
  UID leaves the stale entry that proved the counter untrustworthy and every
  other daemon on the path keys on the same duplicated pair. Note the
  `SENDING` detector compares the *bytes*, not just their length: a
  same-length collision is exactly as damaging and was previously undetected.
- **Data lifetime.** A message's `data` is unloaded/emptied when it is posted or
  evicted; cached data is dropped by timer (`relm_base_cache_ms`) or when the
  cache exceeds `relm_base_cache_max_count`. Don't assume `msg->data.bytes` is
  non-NULL.
- **`prte_relm_start_msg()` owns the caller's buffer once it succeeds, and
  unloading a buffer is not releasing it.** This is the same contract
  `prte_rml_send_buffer_nb()` follows — every `PRTE_RML_RELIABLE_SEND` caller
  hands the buffer over on success and releases it itself on an error return.
  `start_msg` consumed the buffer's *payload* with `PMIx_Data_unload()` into a
  fresh RELM-framed buffer and then dropped the emptied container on the
  floor: a `pmix_data_buffer_t` leaked **per inter-daemon reliable message**,
  for the life of the DVM. The signature to recognize is a valgrind record
  with direct bytes but *zero* indirect ones — the payload was moved out and
  freed, only the shell survived. If you add another framing step here,
  release what you unloaded.
- **Every lookup helper can answer NULL.** `prte_relm_find_rank`,
  `find_msg`, `get_rank`, `get_msg`, `find_prev_msg`, `get_prev_msg` all return
  NULL for a bad signature, an out-of-range rank, or a hash-table failure. They
  are usually called where a predecessor "must" exist — which is exactly the
  assumption a fault breaks. Check.
- **The hash tables hold references, not copies.** Removing a
  `prte_relm_rank_t` from `prte_relm_sm->ranks` (or a `prte_relm_msg_t` from
  `rank->msgs`) drops only the table's entry; the object came from `PMIX_NEW`
  and still has to be `PMIX_RELEASE`d, along with everything hanging off it.
  The purge in `link_updates.c` is where this matters most.
- **`prte_relm_close` may run without `prte_relm_open`.** `prte_rml_open` has
  error returns before it reaches RELM, and `prte_rml_close` runs regardless —
  so `prte_relm_sm` may still be NULL, and that is the guard `close` checks.
  It also tears down in the order the state machine needs: cancel the receives,
  release the state machine, and only then close the output channel it traces
  through — releasing it evicts every cached message, and eviction traces.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.

## Testing

Most of RELM cannot be unit tested, and the reason is structural rather than an
oversight: every entry point either thread-shifts onto `prte_event_base` or is
reached from the RML's fault handler, and the state machine's whole purpose is
what happens across *several* daemons when one of them dies. That coverage
lives in `contrib/dockerswarm` — the `test_rml` phase drives the relay and
lost-daemon paths RELM sits on, and the elastic grow/shrink phases exercise the
link-update exchange after a tree change.

What *is* pure computation is the identity layer, and
`test/unit/rml/test_relm.c` (run by `make check`) covers it: the UID generator
and its wrap, the `<src,uid,dst>` signature and GUID, the find/get lookup
helpers and what they refuse, the prev/next ordering chain, and
`prte_relm_release_msg`. It stands the state machine up by hand with `PMIX_NEW`
rather than calling `prte_relm_open()`, which would also post RML receives.

A single transition is reachable too, from a chosen seat in the tree, and the
same binary drives a few: a link update's `SENT` at a destination and an
intermediate, lingering and dead-source updates in one buffer, `ACKACKED` on a
cached message that something still references, and a 50,000-message PENDING
drain. What makes that possible is small and worth copying: a radix-2 tree from
`prte_rml_compute_routing_tree()` (with the failure bitmaps constructed by
hand), `PMIx_server_init()` so the packers run, `prte_event_base_open()` so
sends have somewhere to queue — the test never runs the loop, so nothing is
actually sent — and `prte_state.activate_job_state` pointed at a counter,
since RELM activates a job state only to fail the job. A case can therefore
check the state a message is left in and that the job was not failed, but not
what went on the wire. The send-completion callback is `static` and is not
covered; the pack/unpack helpers in `util.c` are covered only through these
cases.
