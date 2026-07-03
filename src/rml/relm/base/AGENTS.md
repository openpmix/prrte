# `src/rml/relm/base` — the RELM base implementation

This is the one concrete implementation of the RELM state machine. The parent
directory ([`../AGENTS.md`](../AGENTS.md)) defines the generic engine and the
`prte_relm_state_machine_t` bundle of callbacks; the files here supply those
callbacks and register the module. There is no second implementation — the
"module" indirection exists only so one could be added, so don't build the
abstraction out further without a real second strategy.

For the protocol itself see
[`docs/how-things-work/rml/relm.rst`](../../../../docs/how-things-work/rml/relm.rst).

## File map

| File | Responsibility |
|------|----------------|
| `base.h`, `base.c` | `prte_relm_base_module` (the module the global `prte_relm` is copied from) and `prte_relm_base` (config: verbosity, cache TTL `cache_ms`, cache cap `cache_max_count`). `init` builds the state machine, wires every callback to a `prte_relm_base_*` function, and posts the two persistent RELM receives (`PRTE_RML_TAG_RELM_STATE`, `PRTE_RML_TAG_RELM_LINK`). `register` registers the MCA params. |
| `state_updates.c` | The heart of the protocol: `prte_relm_base_update_state` dispatches a state change by who originated it — `local_update`, `downstream_update` (from the neighbor toward `dst`), or `upstream_update` (from the neighbor toward `src`) — plus `pack_state_update` and the cache-eviction timer callback. |
| `link_updates.c` | Post-fault recovery: `pack_link_update`/`update_link` exchange in-flight message state with new neighbors after a promotion, `fault_handler` reacts to a `PRTE_RML_FAULT_SCOPE_LOCAL` recovery (purge dead paths, then re-exchange), and the upstream/downstream "links updated" bitmaps gate when updates may be sent. |
| `state_machine.h` | Static inline defaults for the simple callbacks: `new_rank`/`new_msg` (just `PMIX_NEW`), and `upstream_rank`/`downstream_rank` (just `prte_rml_get_route` toward `src`/`dst`). Declares the non-trivial ones implemented in the `.c` files. |

## Two message flows to understand

- **Steady state (`state_updates.c`).** A message walks `SENDING` → `SENT` at
  each hop as its data is forwarded downstream, then an ACK walks back upstream
  (`ACKED`) and an ACK-of-ACK (`ACKACKED`) walks down again to release state.
  `local_update` is the interesting switch: it decides whether to forward,
  post-to-the-application (at `dst`), cache, request a replay, or tear down.
  Ordering is enforced through the `prev_uid`/`next_uid` chain — a `PENDING`
  message waits for its predecessor to be posted, and an ACK implicitly acks all
  earlier messages.

- **Recovery (`link_updates.c`).** When the routing tree is repaired,
  `fault_handler` runs on the **local**-scope notice (it ignores global scope —
  by then RML recovery has already happened and the tree shows no delta). It
  purges messages to/from failed ranks and messages this daemon is no longer on
  the path for, then, for every changed link, sends a link update carrying all
  in-flight message states routed through that link. A depth stamp on each
  update lets a receiver discard lingering updates from before a promotion. The
  `upstream_links_updated` / `downstream_links_updated` bitmaps ensure a daemon
  has gathered enough neighbor state before it forwards updates onward.

## Gotchas before you edit

- **Scope matters.** The fault handler acts only on
  `PRTE_RML_FAULT_SCOPE_LOCAL`. Global-scope notices arrive after RML recovery
  and report no tree change; a component must therefore save whatever it needs
  between the local and global calls. Don't move recovery work to global scope.
- **Depth-stamped link updates.** A link update is ignored unless the sender's
  reported depth matches the expected parent/child depth. If you change how
  promotions renumber depth, keep `pack_link_update`/`update_link` in sync or
  recovery messages will be silently dropped.
- **Never persist ephemeral states.** `CACHED`/`EVICTED`/`NEW`/`ACKACKED` are
  transitions, not stored states (see `../AGENTS.md`).
- **Warnings are errors.** Debug builds enable `--enable-devel-check`; keep the
  tree warning-free.
