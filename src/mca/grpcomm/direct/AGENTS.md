# AGENTS.md — `grpcomm/direct` (the RML-tree collective engine)

Component guide for `src/mca/grpcomm/direct/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the collective
model (signatures / trackers / two-phase collective), and the threading
rules referenced throughout.

---

## Role and priority

`direct` is the **only** grpcomm component and the sole implementation of
every collective in PRRTE. Its `query` (`grpcomm_direct_component.c`)
returns priority **5** and declares itself "always available", so it always
wins the single-winner selection. It implements the framework vtable by
running collectives **directly over the RML radix routing tree** — hence
the name — with the HNP as the root of every operation.

Files:

| File | Contents |
|------|----------|
| `grpcomm_direct.h` | Component struct `prte_grpcomm_direct_component_t`; the fence/group signature structs; the fence/group tracker structs; the fence caddy; all public entry-point prototypes; `print_signature()` debug helper. |
| `grpcomm_direct_component.c` | Registration, `direct_query` (priority 5), and the `PMIX_CLASS_INSTANCE` definitions for the signature/tracker/caddy classes. |
| `grpcomm_direct.c` | The module vtable (`prte_grpcomm_direct_module`), `init()` (construct trackers + register the six persistent RML receives), `finalize()`, and the combined `fault_handler`. |
| `grpcomm_direct_xcast.c` | The reliable, fault-tolerant broadcast: op-id sequencing, tree forwarding, ACK rollup, late-joiner/promotion handling, and the completion-callback FIFO. |
| `grpcomm_direct_fence.c` | Allgather/barrier: up-tree rollup to the HNP, then a `xcast` release back down. |
| `grpcomm_direct_group.c` | PMIx `PMIx_Group_construct/destruct/cancel`: membership assembly, context-id assignment, bootstrap, add-members, final-order, and the group-FT abort/cancel paths. |

The module vtable (`grpcomm_direct.c`):

```c
prte_grpcomm_base_module_t prte_grpcomm_direct_module = {
    .init = init,  .finalize = finalize,  .fault_handler = fault_handler,
    .xcast = prte_grpcomm_direct_xcast,   .xcast_nb = prte_grpcomm_direct_xcast_nb,
    .fence = prte_grpcomm_direct_fence,   .group = prte_grpcomm_direct_group
};
```

`init()` constructs the three tracker containers on the component
(`xcast_ops`, `fence_ops`, `group_ops`) and registers **six** persistent
RML receives: `XCAST`, `XCAST_ACK`, `FENCE`, `FENCE_RELEASE`, `GROUP`,
`GROUP_RELEASE`. `finalize()` destructs the trackers and cancels the
receives. `fault_handler()` simply fans the recovery notice out to the
xcast, fence, and group fault handlers in turn.

---

## Key data structures (`grpcomm_direct.h`)

- **`prte_grpcomm_direct_component_t`** — the component. Holds
  `xcast_ops` (a `prte_grpcomm_xcast_t`), `fence_ops` (list of
  `prte_grpcomm_fence_t`), and `group_ops` (list of `prte_grpcomm_group_t`).
- **`prte_grpcomm_xcast_t`** — global xcast state: the `ops` list of
  in-flight broadcasts, a `pending_completions` FIFO of completion
  callbacks awaiting relay-back, and three sequence counters
  (`op_id_inited`, `op_id_completed`, `op_id_completed_at_promotion`).
- **`prte_grpcomm_direct_fence_signature_t`** — a fence's identity: the
  array of participating `pmix_proc_t` (`signature`) and its size (`sz`).
  Two fences are "the same" iff their proc arrays match byte-for-byte.
- **`prte_grpcomm_direct_group_signature_t`** — a group's identity and
  payload: `op`, `groupID`, `assignID`/`ctxid`/`ctxid_assigned`, initial
  `members`, `bootstrap` count, `follower` flag, `addmembers`, and
  `final_order`.
- **`prte_grpcomm_fence_t` / `prte_grpcomm_group_t`** — the per-collective
  trackers: the signature, resolved participating daemons (`dmns`/`ndmns`),
  the `nexpected`/`nreported` rollup counters (group adds bootstrap
  leader/follower counters), a `bucket`/info-lists for gathered data, and
  the user `cbfunc`/`cbdata`.
- **`prte_pmix_fence_caddy_t`** — the thread-shift caddy for a fence
  request (mirrors `prte_pmix_grp_caddy_t` for groups, which lives in the
  framework header).

`create_dmns()` (duplicated in `fence.c` and `group.c`) turns a signature's
proc set into the set of daemon vpids that must participate: if the target
nspace is the daemon job itself, *all* daemons participate; otherwise it
walks each proc's `proc->node->daemon` (or, for `PMIX_RANK_WILDCARD`, every
daemon in that job's map) and de-dups. `prte_rml_get_num_contributors()`
then tells the tracker how many child contributions to expect.

---

## `xcast` — reliable fault-tolerant broadcast (`grpcomm_direct_xcast.c`)

This is the most intricate file. The public `prte_grpcomm_direct_xcast()`
is just `xcast_nb(tag, msg, NULL, NULL)`.

### Message flow

1. **Originate (`xcast_nb`).** Any daemon can originate. It builds an
   `op_t`, stashes the (possibly compressed) user message and tag, records
   the optional completion callback, and thread-shifts to `begin_xcast`.
2. **`begin_xcast`.** Packs the op and reliably sends it **to the HNP**
   (`PRTE_RML_RELIABLE_SEND(... PRTE_PROC_MY_HNP ... PRTE_RML_TAG_XCAST)`).
   The initiating op is then discarded — it is not the tracked op. If the
   originator is the master, it also enqueues one entry on the
   `pending_completions` FIFO (see *Completion callbacks* below).
3. **HNP assigns the op-id.** The HNP's `xcast_recv` sees `sig.op_id == 0`
   and stamps `sig.op_id = ++XCAST.op_id_inited` — a globally-unique,
   monotonically-increasing collective id. (A non-zero op-id arriving at
   the HNP is a bug → `PRTE_ERR_DUPLICATE_MSG`.)
4. **Forward down the tree (`forward_op`/`forward_op_to`).** Each daemon,
   on receiving a new op, forwards the packed op to each of its routing-tree
   children (`prte_rml_base.children`) and processes it locally.
5. **Process locally (`process_msg`).** Decompresses if needed and
   delivers the payload to *itself* at the user tag via
   `PRTE_RML_POST_MESSAGE` (not a real send — it is injected straight into
   the local RML message processor). `PRTE_RML_TAG_WIREUP` is special-cased
   to `process_wireup()` (decode nidmap).
6. **ACK rollup.** A leaf (`0 == n_children`) immediately `finish_op`s,
   sending an ACK to its parent. An interior daemon `finish_op`s only once
   `nreported == nexpected` (all children have ACKed). `finish_op` sends
   its own ACK upward, advances `op_id_completed`, processes the message if
   not already done, fires the completion callback (master only), and
   releases the op.

`send_ack`/`request_ack`/`xcast_ack` carry the ACK protocol on
`PRTE_RML_TAG_XCAST_ACK`, distinguished by an `is_request` bool: a plain
ACK ("my subtree is done"), or a *request* for an ACK (used after a fault
to re-poll a child without resending the payload).

### Ordering and fault tolerance

The comments in this file are the real spec — read them. The load-bearing
ideas:

- **`process_first` set.** Most xcasts forward before processing (to
  preserve message ordering), but `PRTE_RML_TAG_WIREUP` and
  `PRTE_RML_TAG_DAEMON_DIED` are processed *first* because they change the
  child set: a death *grows* our subtree (orphans promote to us), so we
  must repair before forwarding. `PRTE_RML_TAG_DAEMON_REVIVED` deliberately
  stays on the forward-first path (a return *shrinks* our subtree) — the
  comment explicitly says do not move it.
- **Late joiners.** A daemon that has never seen an xcast
  (`op_id_inited == 0`) but is handed op N>1 is a grown/rebooted/bootstrap
  daemon; it adopts ops `1..N-1` as already complete
  (`op_id_completed = op_id_completed_at_promotion = N-1`) so `finish_op`
  does not raise `PRTE_ERR_OUT_OF_ORDER_MSG` and force-exit.
- **Promotion / re-parenting.** `xcast_fault_handler` (local-scope only)
  reacts to `status->promoted` / `parent_changed` / `children_changed`:
  it invalidates upward ack-ids (new parent will re-issue them), resets
  `nexpected` to the new child count, starts new ack rounds
  (`ack_id_down++`), and holds replays (`replay_pending_parent`) after a
  promotion until the parent replays the ops in order.
- **`op_id_completed_at_promotion`** guards the "assume-complete" logic so
  a promoted daemon does not wrongly assume its *newly-acquired* subtree
  finished ops the daemon itself completed before promotion.

### Completion callbacks (the `pending_completions` FIFO)

The op the master ends up *tracking* is a fresh one built on receipt, not
the initiating op — so a callback cannot ride the initiating op. Instead
`begin_xcast` enqueues one `pending_completion_t` per **master-originated**
broadcast (NULL callback included, to keep alignment), in send order.
When the master receives that broadcast back and builds its tracked op, it
pops the FIFO head and attaches the callback. `finish_op` fires it — but
only on the master, where a completed op means the *entire DVM* has
received the broadcast. This is the hook the elastic DVM-shrink path uses.

---

## `fence` — allgather / barrier (`grpcomm_direct_fence.c`)

`prte_grpcomm_direct_fence()` is the vtable `fence`. It rejects a NULL
`procs` array (`PRTE_ERR_NOT_SUPPORTED`), builds a `prte_pmix_fence_caddy_t`,
and thread-shifts to the static `fence()` handler.

Message flow:

1. **`fence` handler.** Computes the fence signature from `cd->procs`,
   gets-or-creates the tracker (`get_tracker(..., true)`), packs signature +
   info + the local `data` payload into a relay buffer, and **sends it to
   itself** on `PRTE_RML_TAG_FENCE`. Sending to self funnels the local
   contribution through the same receive path everything else uses.
2. **`fence_recv`** (`PRTE_RML_TAG_FENCE`). Unpacks the signature, finds
   the tracker, merges info (`PMIX_TIMEOUT` takes the max; a non-success
   `PMIX_LOCAL_COLLECTIVE_STATUS` is sticky), copies the payload into
   `coll->bucket`, and bumps `nreported`. When `nreported == nexpected`:
   - **HNP:** the allgather is complete → pack signature + status + bucket
     and broadcast the result down via
     `prte_grpcomm.xcast(PRTE_RML_TAG_FENCE_RELEASE, reply)`.
   - **non-HNP:** the local subtree rollup is complete → forward the
     bucket up to `PRTE_PROC_MY_PARENT` on `PRTE_RML_TAG_FENCE`.
3. **`fence_release`** (`PRTE_RML_TAG_FENCE_RELEASE`, delivered by the
   xcast). Unpacks the signature + status, finds the tracker (missing
   tracker == "I had no local participants", not an error), and fires
   `coll->cbfunc(status, bytes, size, cbdata, relcb, bytes)` to hand the
   gathered data back to the PMIx server. Removes and releases the tracker.

`nexpected` counts routing-tree child contributors
(`prte_rml_get_num_contributors`) plus one if this daemon is itself a
participant. The tracker lives on `component.fence_ops`, keyed by exact
proc-signature match.

The fence `fault_handler` is currently **not resilient**: a TODO. If any
fence op is in flight when a daemon fails it activates
`PRTE_JOB_STATE_COMM_FAILED` (kills the job) rather than repairing.

---

## `group` — PMIx group operations (`grpcomm_direct_group.c`)

`prte_grpcomm_direct_group()` is the vtable `group`, driving
`PMIx_Group_construct` / `destruct` / `cancel`. It builds a
`prte_pmix_grp_caddy_t` (framework header) and thread-shifts to the static
`group()` handler. The rollup/release skeleton mirrors `fence`, but with a
much richer signature and payload.

### The `group` handler

- **Cancel short-circuit** (`#if PRTE_PMIX_HAVE_GROUP_FT`): a
  `PMIX_GROUP_CANCEL` op is *not* a rollup collective — it routes straight
  to the HNP via `request_group_cancel()` and returns.
- Otherwise it builds the group signature from `grpid` + `procs` (a NULL
  `procs` marks a bootstrap **follower**), scans the directives
  (`PMIX_GROUP_ASSIGN_CONTEXT_ID`, `PMIX_GROUP_BOOTSTRAP`, `PMIX_TIMEOUT`,
  `PMIX_GROUP_ADD_MEMBERS`, `PMIX_GROUP_INFO`, `PMIX_PROC_DATA` endpoints,
  `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`, `PMIX_LOCAL_COLLECTIVE_STATUS`),
  gets-or-creates the tracker, and relays.
- **Bootstrap** ops send **directly to the HNP** (there is no rollup tree —
  each daemon reports straight to the controller); non-bootstrap ops send
  to self on `PRTE_RML_TAG_GROUP`, entering the same up-tree rollup as
  fence.

### `grp_recv` (rollup) and `grp_release` (down-tree)

- **`grp_recv`** (`PRTE_RML_TAG_GROUP`). Handles the FT cancel first
  (HNP-only: find the in-flight construct by groupID and abort it). Then
  it merges the incoming contribution (status, timeout, grpinfo, endpoints)
  into the tracker and bumps the appropriate counter: bootstrap **leaders**
  (`nleaders_reported`), bootstrap **followers** (`nfollowers_reported`),
  or ordinary participants (`nreported`). Completion is
  `nleaders_reported == nleaders && nfollowers_reported == nfollowers` for
  bootstrap, else `nreported == nexpected`.
  - **HNP at completion:** for a construct it assigns the context id (if
    requested, from the decrementing `prte_grpcomm_base.context_id`),
    assembles the **final membership** (union of members + add-members,
    wildcard-preserving), applies `final_order` if given (else `qsort` for a
    stable order), packs signature + status + membership + grpinfo +
    endpoints, and broadcasts the result with
    `prte_grpcomm.xcast(PRTE_RML_TAG_GROUP_RELEASE, reply)`.
  - **non-HNP at completion:** roll the aggregated results up to
    `PRTE_PROC_MY_PARENT`.
- **`grp_release`** (`PRTE_RML_TAG_GROUP_RELEASE`, via the xcast). For a
  **destruct** it removes the group from the server's pset list and
  completes the local participants. For a **construct** it unpacks the
  final membership / context-id / grpinfo / endpoints, calls
  `PMIx_server_register_resources` (blocking on a caddy lock), records the
  new group in `prte_pmix_server_globals.groups`, and returns the assembled
  info to local clients via `coll->cbfunc`. Finally it deletes the tracker
  (`find_delete_tracker`, keyed by groupID).

### Group fault tolerance (`#if PRTE_PMIX_HAVE_GROUP_FT`)

Guarded by `PRTE_PMIX_HAVE_GROUP_FT` (from
`PRTE_CHECK_PMIX_CAP([GROUP_FT])`, i.e. the installed PMIx advertises
`PMIX_CAP_GROUP_FT`). Two related paths, both converging on
`abort_group_op()`:

- **Explicit cancel.** A client's `PMIx_Group_cancel` reaches
  `prte_grpcomm_direct_group()` with `op == PMIX_GROUP_CANCEL`.
  `request_group_cancel()` routes a signature-only message
  (`op == PMIX_GROUP_CANCEL` + groupID) to the HNP and acks the requester.
  The HNP's `grp_recv` (before `get_tracker`, so it never creates a
  spurious cancel tracker) calls `find_construct_op(groupID)` and, if the
  construct is still in flight, `abort_group_op(coll, PMIX_GROUP_CONSTRUCT_ABORT)`.
- **Participant failure.** `prte_grpcomm_direct_group_fault_handler` runs
  **only on the HNP, only in the GLOBAL-scope pass** (the global
  notification carries the consistent `failed_ranks` on every rank). It
  aborts every in-flight group op that a failed daemon participated in
  (constructs get `PMIX_GROUP_CONSTRUCT_ABORT`, destructs get
  `PMIX_SUCCESS` since they are tearing down anyway). An op whose daemon set
  was never resolved (e.g. a bootstrap) is aborted defensively.

`abort_group_op()` broadcasts a `PRTE_RML_TAG_GROUP_RELEASE` carrying only
the signature + a completion status; the normal `grp_release` non-success
path then completes each daemon's local participants with that status and
deletes the tracker — so a cancel/abort tears down the collective cleanly
**without tearing down the DVM**, which is the whole point of this work.

---

## Things to watch when editing

- **The live tracking model is entirely in this component.** The base
  contributes no collective tracking, signature packing, or xcast
  plumbing — do not look there for an API to build on, and do not model
  new work on the old `prte_grpcomm_API_*`/`grp_construct` shape (that
  retired-API stub file, `grpcomm_base_stubs.c`, has been removed).
- **Never touch trackers off the progress thread.** Every entry point
  thread-shifts through `prte_event_set`/`prte_event_active` for exactly
  this reason. The tracker lists, `context_id`, and xcast counters are
  progress-thread-only.
- **xcast ordering is a correctness invariant, not a nicety.** The
  `process_first` set, the late-joiner catch-up, and the promotion replay
  hold are what keep the reliable broadcast correct across DVM
  grow/shrink/unheal. Read the in-file comments before changing any of it;
  a mistake here manifests as `PRTE_ERR_OUT_OF_ORDER_MSG` force-exits or
  silent message loss during recovery.
- **Signatures must round-trip exactly.** Fence trackers match on a
  byte-for-byte `memcmp` of the proc array; group trackers match on
  groupID + op. If you add a field to a signature, update *both* its
  pack/unpack and its constructor/destructor, or trackers will fail to
  coalesce (hang) or leak.
- **Group-FT code must stay behind `#if PRTE_PMIX_HAVE_GROUP_FT`.** The
  cancel op, `find_construct_op`, and `request_group_cancel` are all
  guarded; keep it that way so PRRTE still builds against a pre-FT PMIx.
  Note the fault-handler abort loop itself is *unguarded* (it uses only
  status fields and `abort_group_op`), but the `PMIX_GROUP_CANCEL` handling
  is guarded — preserve that split.
- **The fence fault handler is a known gap.** It kills the job on any
  in-flight fence when a daemon fails (TODO to make it resilient). If you
  are adding fence resilience, that is the place — do not weaken it into a
  silent no-op.
- **Free every allocation the handler still owns before it returns.** The
  entry-point handlers (`begin_xcast`, `fence`, `group`) own the caddy/op
  they were thread-shifted; the recv handlers own the info arrays / darrays
  they unpack. The tracker caches only `cbfunc`/`cbdata`, never the caddy,
  so the handler is the last owner. Historic leaks here — `begin_xcast`
  never releasing the initiating `op_t`, the `group()` success returns
  never releasing `cd`, and `fence_recv` never freeing its unpacked `info`
  on the HNP-complete and intermediate paths — were all "return added, free
  forgotten" mistakes. Trace each new `return` back to what it strands.
- **`fence_recv`/`grp_recv` must `return` on a signature unpack failure.**
  The signature pointer is NULL-initialized and left NULL on failure, and
  the code immediately below dereferences it (`get_tracker(sig, …)`,
  `sig->op`). Logging without returning turns a corrupt RML message into a
  daemon crash.
- Standard PRRTE rules: `prte_config.h` first, constant-on-left, braces
  everywhere, `PMIX_ERROR_LOG`/`PRTE_ERROR_LOG`, no new warnings.
