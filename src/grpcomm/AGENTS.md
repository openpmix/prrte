# AGENTS.md — `grpcomm` (Group Communication)

Orientation for AI agents and human contributors working in
`src/grpcomm/`. This is a map, not the rulebook: the authoritative project
guidance lives in the top-level [`AGENTS.md`](../../AGENTS.md) and under
[`docs/`](../../docs/). When this file and those disagree, **the docs
win** — and please fix this file.

---

## What this code does

`grpcomm` provides the **collective communication services that span the
DVM's daemons**: scalable broadcast (`xcast`), allgather/barrier (`fence`),
and PMIx group operations (`group`). It is not intended for
point-to-point communication (the RML does that), nor as a
high-performance channel for large-scale data transfer.

grpcomm runs **on every daemon** (`prted` and the HNP). It is layered on
top of the RML and its routing tree: it opens no connections of its own —
it sends RML messages along the radix routing tree that `src/rml/`
maintains (`prte_rml_base.children`, `PRTE_PROC_MY_PARENT`,
`PRTE_PROC_MY_HNP`). It is the machinery behind almost every DVM-wide
action:

- `plm`/`state` broadcast launch messages, wireup (nidmap), and DAEMON
  commands with `prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, …)` /
  `PRTE_RML_TAG_WIREUP`. The receive side of the wireup lives here, in
  `process_wireup()` (`grpcomm_xcast.c`): after the nidmap it reads a
  **three-field record per daemon** — name, `PMIX_PROC_URI` (RML contact
  info), `PMIX_SERVER_URI` (that node's PMIx server rendezvous,
  redistributed only so any daemon can answer a tool's query; PRRTE never
  connects to it). The sender is `vm_ready()` in `state/dvm`. The loop
  skips storing what it already knows, so mind the invariant: **every
  field of a record must be unpacked before any `continue`**, or the next
  iteration reads the leftover string as a `pmix_proc_t`.
- The PMIx server shim satisfies `PMIx_Fence` /
  `PMIx_server_register_resources` via `prte_grpcomm_fence(...)` (see
  `src/prted/pmix/pmix_server_fence.c`).
- The PMIx server shim satisfies `PMIx_Group_construct/destruct/cancel`
  via `prte_grpcomm_group(...)` (see `src/prted/pmix/pmix_server_group.c`).
- The RML fault handler re-broadcasts `PRTE_RML_TAG_DAEMON_DIED` /
  `PRTE_RML_TAG_DAEMON_REVIVED` through `xcast` as part of DVM recovery.

---

## This is not an MCA framework — do not make it one again

grpcomm **was** an MCA framework, with a `base/` and a single component
called `direct`. It was collapsed into plain code for the same reasons
`src/rml` was (which had been *three* frameworks — rml, routed and oob):

- There was only ever one component. `direct` returned priority 5 and
  declared itself "always available"; the historical `bmg` component had
  long since been deleted.
- More importantly, **the choice that actually matters is not expressible
  as a component**. What varies is which algorithm a collective uses, and
  that is a per-operation, per-message-size decision: a barrier and a
  modex are both `fence` but want opposite algorithms, and the launch
  message and a shutdown command are both `xcast` on the same tag six
  orders of magnitude apart in size. A component is chosen once, for the
  whole interface, for the life of the process. Wrong axis.

So: to vary an algorithm, add a **movement strategy inside the
operation** — not a component. Do not reintroduce the component/module
abstraction unless a genuine second implementation of the *whole*
interface turns up; that abstraction is precisely what was removed.

---

## Directory layout

```
grpcomm/
  grpcomm.h            # the public API - what the rest of the tree calls
  grpcomm_internal.h   # trackers, signatures, globals, cross-file prototypes
  grpcomm.c            # init/finalize/fault_handler, globals, MCA params
  grpcomm_classes.c    # PMIX_CLASS_INSTANCE for every signature/tracker/caddy
  grpcomm_xcast.c      # reliable, fault-tolerant broadcast
  grpcomm_fence.c      # allgather / barrier
  grpcomm_group.c      # PMIx group construct/destruct/cancel
```

Read `grpcomm.h` first — it is the whole external contract. Then
`grpcomm_internal.h` for the object model.

---

## The API

Plain functions, all `PRTE_EXPORT`, declared in `grpcomm.h`:

| Function | Meaning / return protocol |
|----------|---------------------------|
| `prte_grpcomm_register()` | Register MCA parameters and open the verbosity stream. Called from `prte_register_params()`, long before init. |
| `prte_grpcomm_init()` | Construct the trackers, register the persistent RML receives. Called once per daemon from the ess. Failure is **fatal** — no trackers and no receives means every later collective quietly does nothing. |
| `prte_grpcomm_finalize()` | Tear down trackers, cancel the RML receives. |
| `prte_grpcomm_fault_handler(status)` | Invoked on **every** daemon by `src/rml/routed_radix.c` when the routing tree changes. Called twice per death (LOCAL scope then GLOBAL scope); which scope a collective keys on depends on what it needs, so read *Surviving a daemon failure* before adding a third. |
| `prte_grpcomm_xcast(tag, msg)` | Broadcast to **every** daemon, delivered at `tag`. Non-destructive to `msg`. Returns `PRTE_SUCCESS` on *acceptance*, not completion. |
| `prte_grpcomm_xcast_nb(tag, msg, cbfunc, cbdata)` | As above, but `cbfunc` fires on the **master** once the whole DVM has confirmed receipt. `cbfunc`/`cbdata` are ignored on non-master daemons. `xcast` is just `xcast_nb(tag, msg, NULL, NULL)`. |
| `prte_grpcomm_fence(procs, nprocs, info, ninfo, data, ndata, cbfunc, cbdata)` | Non-blocking allgather/barrier across the daemons hosting `procs`. A barrier supplies no data. Returns `PRTE_SUCCESS` once queued. |
| `prte_grpcomm_group(op, grpid, procs, nprocs, directives, ndirs, cbfunc, cbdata)` | PMIx group construct/destruct/cancel. |

### The one indirection: `prte_grpcomm_release_bcast`

`grpcomm_internal.h` declares a single function pointer, initialised to
`prte_grpcomm_xcast`, through which fence and group emit their **release**
broadcasts. It exists for two reasons, and neither of them is "keep the
framework alive in spirit":

- The release is precisely the part a different data movement changes. A
  rollup-to-controller collective must broadcast the answer back; a true
  allgather leaves every daemon already holding it, with nothing to
  release at all.
- It is the only way the unit test can observe the release a controller
  would emit without standing up an RML.

Production code sets it once, at startup, and never again.

---

## Global state

One struct, `prte_grpcomm_globals` (`grpcomm_internal.h`), defined in
`grpcomm.c`. This was two structs — the framework's "base" and the
component's own — until grpcomm stopped being a framework; they were
always one thing.

| Field | Meaning |
|-------|---------|
| `output` | Verbosity stream. `-1` until the `grpcomm_base_verbose` MCA parameter opens it. The parameter deliberately **keeps its old framework spelling**, because it is in every debugging recipe and in the guides. |
| `context_id` | The group context-id pool. A construct asking for `PMIX_GROUP_ASSIGN_CONTEXT_ID` gets this value, and it **decrements** — it counts *down* from `UINT32_MAX` so DVM-assigned ids cannot collide with ids assigned from the bottom of the range elsewhere. |
| `xcast_ops` | In-flight broadcasts, the `pending_completions` FIFO, and the three op-id sequence counters. |
| `fence_ops` | List of `prte_grpcomm_fence_t`. |
| `group_ops` | List of `prte_grpcomm_group_t`. |
| `completed_group_ops` | Bounded memo of already-released group ops (see below). |
| `recovery_epoch` | The collective recovery epoch, shared by fence and group: one failure, one restart, one epoch. |

Note the verbosity variable that backs the MCA parameter is at **file
scope** in `grpcomm.c`, not a local: the MCA layer keeps the pointer it is
handed and writes through it whenever the variable is set, so a stack slot
would be a dangling write the moment registration returns.

---

## The collective model

- **Routing tree.** Collectives ride the radix routing tree owned by
  `src/rml/`. The HNP is the root of every operation.
- **Signatures.** A collective is identified not by a global counter but
  by a *signature* — for `fence`, the array of participating procs
  (matched **byte-for-byte** with `memcmp`, so the order PMIx hands down
  is load-bearing); for `group`, the `groupID` + operation; for `xcast`,
  an HNP-assigned globally-unique `op_id`. A signature lets
  independently-arriving pieces of the same collective find each other.
- **Trackers.** Each daemon keeps a per-collective tracker on a global
  list, counting `nexpected` vs `nreported`. When a tracker completes
  locally it rolls up to the parent; when the HNP's tracker completes it
  broadcasts the release.
- **Two-phase collective.** `fence`/`group` are an **up-tree allgather**
  followed by a **down-tree release**. `xcast` itself is the down-tree
  half, made reliable with an ACK rollup.

`create_dmns()` (in both `grpcomm_fence.c` and `grpcomm_group.c`) turns a
signature's proc set into the daemon vpids that must participate;
`prte_rml_get_num_contributors()` then tells the tracker how many child
contributions to expect.

---

## `xcast` — reliable fault-tolerant broadcast (`grpcomm_xcast.c`)

The most intricate file. `prte_grpcomm_xcast()` is just
`xcast_nb(tag, msg, NULL, NULL)`.

### Message flow

1. **Originate (`xcast_nb`).** Any daemon can originate. Builds an `op_t`,
   stashes the (possibly compressed) message and tag, records the optional
   completion callback, thread-shifts to `begin_xcast`.
2. **`begin_xcast`.** Packs the op and reliably sends it **to the HNP**.
   The initiating op is then discarded — it is not the tracked op. A
   master originator also enqueues one entry on `pending_completions`.
3. **HNP assigns the op-id.** `sig.op_id == 0` becomes
   `++op_id_inited` — globally unique and monotonic. A non-zero op-id
   arriving at the HNP is a bug (`PRTE_ERR_DUPLICATE_MSG`).
4. **Forward down the tree** to each routing-tree child, then process
   locally.
5. **Process locally (`process_msg`).** Decompresses and delivers to
   *itself* at the user tag via `PRTE_RML_POST_MESSAGE` — not a real send.
   `PRTE_RML_TAG_WIREUP` is special-cased to `process_wireup()`.
6. **ACK rollup.** A leaf `finish_op`s immediately; an interior daemon
   only once all children have ACKed. `finish_op` ACKs upward, advances
   `op_id_completed`, fires the completion callback (master only), and
   releases the op.

### Framing versus movement

`xcast` is split in two, and the split is the point of the whole restructure.

**The framing** — op-id assignment, the `XCAST_ACK` rollup with its
`is_request` re-poll, the `process_first` ordering set, late-joiner catch-up,
the promotion replay hold, the `pending_completions` FIFO, and the
fault-handler reactions to parent/children changes — is the hard, correctness-
critical part, and it is identical whichever way the bytes travel. There is one
implementation and there should stay one.

**The movement** is how the payload physically reaches the daemons below us.
It is a small vtable (`bcast_movement_t`: an id, a name, and a `forward`), and
`forward_op()` dispatches through it after doing the framing's own work:

- the do-not-launch check, which is not about movement at all;
- resetting `replay_pending_parent` / `nexpected` / `nreported`. **The ACK
  rollup stays subtree-shaped whichever movement carries the bytes** — a daemon
  reports its subtree complete once it holds the payload *and* all its children
  have reported — so this accounting is framing, and `nexpected` remains
  `n_children`.

Today there is one movement, `tree_whole`: send the whole payload to each
routing-tree child. It is right for a small message, where the cost is the
depth of the tree and a high radix makes that one or two hops. It is wrong for
a large one, because a node with `r` children serializes `r` full copies on its
outbound link at every level — `d*r*M*beta`, which is essentially the entire
cost of broadcasting a launch message or a preload chunk at scale.

**The movement is stamped on the wire**, between the ack id and the payload, by
whoever originated the broadcast. A broadcast has a single originator, so
unlike an allgather there is nothing to agree on — the originator decides and
says so. Two consequences to respect:

- **Read it in wire order.** `xcast_recv` unpacks it immediately after the ack
  id, *before* the "already complete, just ack" short-circuit, because reading
  it later would take its bytes as the start of the message.
- **An unknown movement is refused, not guessed.** Every daemon in a DVM runs
  the same build, so an id this build does not implement is a bug rather than
  version skew; guessing would deliver a misparsed payload.

The ids are explicit and must never be renumbered — they are on the wire, and a
renumber would silently repoint an in-flight broadcast into a different
movement, failing as if the payload were corrupt.

When a second movement lands, it supplies data transport only. If you find
yourself copying any of the framing into it, stop: that is the mistake this
split exists to prevent.

### Ordering and fault tolerance

The in-file comments are the real spec — read them. The load-bearing ideas:

- **`process_first` set.** Most xcasts forward before processing (to
  preserve ordering), but `PRTE_RML_TAG_WIREUP` and
  `PRTE_RML_TAG_DAEMON_DIED` are processed *first* because they change the
  child set: a death *grows* our subtree (orphans promote to us), so we
  must repair before forwarding. `PRTE_RML_TAG_DAEMON_REVIVED`
  deliberately stays on the forward-first path (a return *shrinks* our
  subtree) — the comment explicitly says do not move it.
- **Late joiners.** A daemon that has never seen an xcast
  (`op_id_inited == 0`) but is handed op N>1 is a grown/rebooted/bootstrap
  daemon; it adopts ops `1..N-1` as complete so `finish_op` does not raise
  `PRTE_ERR_OUT_OF_ORDER_MSG` and force-exit.
- **Promotion / re-parenting.** `xcast_fault_handler` (local scope only)
  invalidates upward ack-ids, resets `nexpected`, starts new ack rounds,
  and holds replays (`replay_pending_parent`) after a promotion until the
  parent replays in order.
- **`op_id_completed_at_promotion`** stops a promoted daemon wrongly
  assuming its *newly-acquired* subtree finished ops it completed before
  promotion.

### Completion callbacks (the `pending_completions` FIFO)

The op the master ends up *tracking* is a fresh one built on receipt, not
the initiating op — so a callback cannot ride the initiating op. Instead
`begin_xcast` enqueues one entry per **master-originated** broadcast (NULL
callback included, to keep alignment), in send order; on receipt the
master pops the FIFO head and attaches it. `finish_op` fires it, master
only, where a completed op means the *entire DVM* has the broadcast. This
is the hook the elastic DVM-shrink path uses.

---

## `fence` — allgather / barrier (`grpcomm_fence.c`)

`prte_grpcomm_fence()` rejects a NULL `procs` array
(`PRTE_ERR_NOT_SUPPORTED`), builds a `prte_pmix_fence_caddy_t`, and
thread-shifts to the static `fence()` handler.

1. **`fence` handler.** Computes the signature, gets-or-creates the
   tracker, packs signature + info + payload, and **sends it to itself** on
   `PRTE_RML_TAG_FENCE` — funnelling the local contribution through the
   same receive path everything else uses.
2. **`fence_recv`.** Checks the epoch, unpacks the signature, finds the
   tracker, merges info (`PMIX_TIMEOUT` takes the max; a non-success
   `PMIX_LOCAL_COLLECTIVE_STATUS` is sticky), copies the payload into
   `coll->bucket`, bumps `nreported`. At `nreported == nexpected`:
   - **HNP:** broadcast the result via `prte_grpcomm_release_bcast`.
   - **non-HNP:** forward the bucket up to `PRTE_PROC_MY_PARENT`.
3. **`fence_release`.** Finds the tracker (missing tracker == "I had no
   local participants", not an error) and fires `coll->cbfunc` to hand the
   gathered data back to the PMIx server. Removes and releases the tracker.

**`create_dmns()`'s answer is a pair, and NULL means two different
things.** A signature naming the daemon job is "every daemon in the DVM",
reported as a count with a **NULL array**; `set_nexpected()` answers that
as `n_children + 1`. A NULL array with a **zero** count is the opposite
statement, "nothing resolved". Both readers used to index the array
regardless, which dereferenced NULL for any fence naming the daemon job.

**A signature that cannot be read is refused, and refusal leaves nothing
behind.** `create_dmns()` returns `PRTE_ERR_BAD_PARAM` for an empty
signature or one whose first entry has no nspace — both of which are what
a truncated RML message unpacks to, and the second is worse than it looks,
because `PMIX_CHECK_NSPACE` answers *yes* for an empty nspace against
anything, so it would be taken for a daemon-job fence and sized to expect
the whole DVM. `get_tracker()` then removes the half-built tracker before
returning NULL. That removal is not tidiness: a tracker with no daemon set
can never complete, and the next fence with the same signature would
*find* it, see a rollup expecting nothing, and answer immediately with
data it never gathered.

**Every exit from the `fence()` handler destructs the signature it built
and completes the caller.** The signature is a stack object with a
malloc'd proc array that `get_tracker()` copies rather than adopts, so
walking away from it leaks it once per `PMIx_Fence`. And a failure there
is invisible to everyone else: the entry point answered `PRTE_SUCCESS` the
moment the request was queued, so no release is coming. The handler
completes its own participants with the reason, after clearing the
tracker's `cbfunc` so a later abort cannot complete them twice.

**The fence's deadline is the DVM's to keep.** A participant can put a
`PMIX_TIMEOUT` on a fence; the controller arms a timer on the first
contribution carrying one (largest offered wins), disarms on convergence,
and ends the fence with `PMIX_ERR_TIMEOUT` through `abort_fence_op()`.
Nothing else is watching: the PMIx server library arms a timeout while it
gathers the *local* contributions, then deletes it the instant the request
is handed to the host — deliberately, so a late host answer cannot reach a
tracker it already released.

**A release with no local callback still has data to free.** A daemon
holding a tracker only because it relayed for its subtree has no `cbfunc`
— the common case on any interior daemon — so that branch frees the
payload itself.

**Only a fence that was in flight when the failure landed is ended.** A
fence *started afterwards* completes among the survivors, because
`prte_rml_get_num_contributors()` skips failed daemons when the tracker is
built. That is deliberate, and it is the opposite of where the group
policy sits (at completion, in `check_complete`). The failed-daemon set is
permanent, so applying the test at completion would make every later fence
naming that namespace fail for the life of the DVM, leaving a recoverable
job unable to fence at all after its first loss. If you move this check,
that is the regression to expect.

A fence whose participants all survive lost only a message path and
re-converges at the new epoch — so losing a pure relay is invisible to it.
A fence that genuinely lost a participant cannot produce its answer,
because an allgather has no opt-in to running degraded, so the controller
ends it with `PMIX_ERR_LOST_CONNECTION`. Note the difference from a group
construct, which *can* complete on survivors when asked: a fence has no
equivalent of `PMIX_GROUP_FT_COLLECTIVE`.

---

## `group` — PMIx group operations (`grpcomm_group.c`)

The rollup/release skeleton mirrors `fence`, with a much richer signature
and payload.

- **Cancel short-circuit** (`#if PRTE_PMIX_HAVE_GROUP_FT`): a
  `PMIX_GROUP_CANCEL` op is not a rollup collective — it routes straight
  to the HNP via `request_group_cancel()` and returns.
- Otherwise it builds the signature from `grpid` + `procs` (a NULL `procs`
  marks a bootstrap **follower**), scans the directives
  (`prte_grpcomm_group_parse_directives()`), gets-or-creates the tracker,
  and relays.
- **Bootstrap** ops send **directly to the HNP** (no rollup tree — each
  daemon reports straight to the controller); non-bootstrap ops send to
  self on `PRTE_RML_TAG_GROUP`.

**A signature owns its arrays — always.** Two directives carry a proc
array (`PMIX_GROUP_ADD_MEMBERS`, `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`), and
the array inside a directive belongs to the **PMIx server** that delivered
the upcall: PMIx frees the directives, arrays and all, once the operation
completes. The signature's destructor frees what the signature holds, so
the parse takes a *copy* of each. Pointing at the caller's array instead
is a double free of live heap, and it is not theoretical. If you add a
directive that carries an array, copy it in `copy_directive_procs()` like
the other two; do not add a "clear it again before the destructor runs"
step anywhere.

**Completion** is `nleaders_reported >= nleaders && nfollowers_reported >=
nfollowers` for bootstrap, else `nreported >= nexpected` — `>=` rather
than `==` because a fault can lower `nexpected` under a tracker that has
already counted more than it now needs; the `converged` latch keeps that
from answering twice. A bootstrap additionally requires `0 < nleaders`:
both counts are zero until the first *leader* arrives to supply them, so
without that guard a tracker created by a follower would satisfy `>= 0` on
its own and converge on one contribution with an empty membership.

**`grp_release` is a continuation, not a wait.** The
`PMIx_server_register_resources` call used to be waited on with
`PMIX_WAIT_THREAD`. Nothing raced, but the wait parked the **progress
thread** — and unlike the teardown waits converted for
[#2534](https://github.com/openpmix/prrte/issues/2534), this one runs on
every daemon for every group construct. It is now chained through
`grp_release_regcbfunc` → `grp_release_resume` → `grp_release_complete`.
Three things about the shape are load-bearing:

- **A file-local caddy owns everything that has to survive the gap**, and
  frees all of it in its destructor.
- **The tracker is re-looked-up in the continuation, not carried.** The
  progress thread is free while PMIx works, so an abort or duplicate
  release can delete the tracker in the interim; a cached pointer would
  dangle.
- **`PMIX_OPERATION_SUCCEEDED` means it already finished**, so that path
  falls straight through rather than waiting for a callback that will
  never come.

`abort_group_op()` broadcasts a release carrying only signature +
status; the normal non-success path then completes each daemon's local
participants and deletes the tracker — so a cancel/abort tears down the
collective **without tearing down the DVM**, which is the whole point.

**`ft_collective` means "some *surviving* participant asked for it."** It
is accumulated by sticky-OR as contributions merge, so a participant that
requested it and then died before rolling up is not visible. This is a
deliberate superset of PMIx's own rule, which takes the first
`PMIX_GROUP_FT_COLLECTIVE` in the aggregated block info and ignores the
rest — do not "fix" it to match.

---

## Surviving a daemon failure

This applies to **both** fence and group; they differ only in what they do
about a collective that genuinely lost a participant.

A daemon failure invalidates every in-flight rollup: `nexpected` is derived
from the routing tree, and the tree just changed shape. Recovery is a
**restart**, not a repair — each daemon discards what it gathered,
recomputes what the repaired tree owes it, and re-offers
`coll->my_contribution`, kept for exactly this.

**The epoch.** One `recovery_epoch` per daemon, shared by both
collectives, stamped on every `PRTE_RML_TAG_GROUP` and
`PRTE_RML_TAG_FENCE` message as `[epoch][body]`. A contribution stamped
older than the receiver's epoch belongs to a round that no longer exists
and is dropped before it is merged.

**Why a per-link round does not work here, and why one epoch does.** The
obvious model is xcast's — `ack_id_down` chosen by the parent, echoed by
the child. It does not transfer. xcast's payload flows *down*, so the
parent always holds the op and can re-poll; a rollup flows *up*, so the
parent may hold nothing at all (a pass-through daemon's tracker is created
lazily, on the first arriving contribution). A round cannot invalidate a
contribution the parent has **already absorbed** — that invalidation has
to travel up, and rounds travel down.

What makes a single epoch work is that the restart is *simultaneous*. The
GLOBAL fault scope reaches every daemon from one broadcast, in the same
order everywhere, after the tree is repaired. `PRTE_RML_TAG_DAEMON_DIED`
is in xcast's `process_first` set, so a daemon hands the notice to its own
delivery before forwarding, and libevent runs that active event before its
next round of socket reads — so a daemon has advanced its epoch before it
can read a contribution from a child that advanced first. A **newer**
stamp should therefore be unreachable; the recv handlers handle one anyway
by adopting it and logging, so the ordering argument stays falsifiable.

**A partial restart is the failure mode to fear**, not a hang: a daemon
that resets and re-sends into a parent that did not would have its subtree
counted twice and another not at all — a quietly wrong result. That is why
`prte_grpcomm_advance_epoch()` walks *every* tracker.

**A converged collective is finished, on the controller.** Neither the
restart nor the fault handler touches a tracker the controller has already
answered: the release is on the wire, ordered ahead of anything a restart
could send. Re-running the rollup there answers twice — a second release, a
second context id consumed, the same group registered twice everywhere.
The test is *only* valid on the controller: `converged` on any other daemon
means it rolled its aggregate up to its parent, and re-sending that
aggregate is precisely what recovery is for.

**Still aborted, deliberately:** bootstrap operations (no resolved daemon
set, and `nleaders` is a count with no identities) and anything in flight
across a **revival** (`PRTE_RML_TAG_DAEMON_REVIVED` is forward-first by
design, so it cannot give the ordering an epoch advance needs). The
revival path is discriminated by LOCAL scope with an empty `failed_ranks`.

Two supporting pieces: `nreported` is backed by `reported_slots`, a bitmap
keyed on `prte_rml_get_subtree_index()` of the sender, so a replayed
contribution is idempotent rather than double-counted (the info-list
accumulation appends with no key matching, so a double-count is also a
duplicated payload). And `completed_group_ops` is a bounded memo of
already-released operations, consulted by `grp_recv` so a straggler cannot
build a tracker nothing will ever complete or delete; `group()` clears the
matching entry, because a local client starting an operation is proof the
previous one of that name is over.

---

## Things to watch when editing

- **Everything runs on the single progress thread.** Every entry point
  (`fence`, `group`, `xcast_nb`) immediately thread-shifts: heap caddy/op,
  `prte_event_set(prte_event_base, &cd->ev, …)`, `PMIX_POST_OBJECT`,
  `prte_event_active`. The global data — trackers, `context_id`, xcast
  counters — is only touched inside those handlers. The caddy's event
  member must be named `ev`.
- **RML receives are persistent**, registered in `prte_grpcomm_init()` and
  cancelled in `prte_grpcomm_finalize()`: `PRTE_RML_TAG_XCAST`,
  `..._XCAST_ACK`, `..._FENCE`, `..._FENCE_RELEASE`, `..._GROUP`,
  `..._GROUP_RELEASE`.
- **`xcast` returns on acceptance, not completion.** If you need to know
  the whole DVM received a broadcast, use `xcast_nb` with a callback.
- **Non-destructive to the caller's buffer.** `xcast`/`fence` copy the
  payload; the caller keeps ownership.
- **xcast ordering is a correctness invariant, not a nicety.** The
  `process_first` set, the late-joiner catch-up and the promotion replay
  hold are what keep the reliable broadcast correct across DVM
  grow/shrink/unheal. Read the in-file comments before changing any of it;
  a mistake here manifests as `PRTE_ERR_OUT_OF_ORDER_MSG` force-exits or
  silent message loss during recovery.
- **A collective's own state is copied, never borrowed.** The arrays a
  caller hands an entry point belong to the PMIx server that delivered the
  upcall, which frees them once the completion callback fires. The caddy
  may point at them for the length of the handler, but anything that
  outlives it (a signature, a tracker) has to hold a copy. Both halves of
  that rule have been broken historically.
- **Every entry-point handler owns its caddy/op — release it on *all*
  paths.** The tracker caches only `cbfunc`/`cbdata`, never the caddy, so
  the handler is the last owner. Historic leaks here — `begin_xcast` never
  releasing the initiating `op_t`, the `group()` success returns never
  releasing `cd`, `fence_recv` never freeing its unpacked `info` — were all
  "return added, free forgotten" mistakes. Trace each new `return` back to
  what it strands.
- **Unpack failures must `return`, not fall through.** The recv handlers
  unpack a signature into a NULL-initialised pointer and the code
  immediately below dereferences it. Logging without returning turns a
  corrupt RML message into a daemon crash.
- **Signatures must round-trip exactly.** Fence trackers match on a
  byte-for-byte `memcmp`; group trackers on groupID + op. If you add a
  field, update *both* pack/unpack and constructor/destructor, or trackers
  fail to coalesce (hang) or leak.
- **Fence and group share one recovery epoch, advanced in one place** —
  `prte_grpcomm_fault_handler()` in `grpcomm.c`. Do not give a collective
  its own counter: the restart is only safe because it is simultaneous,
  and two counters racing to advance would break exactly that.
- **Group-FT code must stay behind `#if PRTE_PMIX_HAVE_GROUP_FT`** (from
  `PRTE_CHECK_PMIX_CAP([GROUP_FT])`), so PRRTE still builds against a
  pre-FT PMIx. The fault-handler abort loop itself is *unguarded*; the
  `PMIX_GROUP_CANCEL` handling is guarded — preserve that split.
  **The wire format is the exception**: the signature's `ft_collective`
  field is packed and unpacked unguarded, so every daemon in a DVM agrees
  on the message layout no matter what its PMIx advertised. Guard the
  *behaviour*, never the bytes.
- **One allocator per array.** The proc arrays on a signature are built
  with `PMIX_PROC_CREATE` and freed with `PMIX_PROC_FREE` everywhere —
  those allocate and free *inside the PMIx library*, so a plain `free()`
  crosses the library boundary.
- Standard PRRTE rules: `prte_config.h` first, constant-on-left, braces
  everywhere, `PMIX_ERROR_LOG`/`PRTE_ERROR_LOG`, no new warnings.

---

## Testing

A structural unit test lives at
[`test/unit/grpcomm/test_grpcomm.c`](../../test/unit/grpcomm/) and is
wired into `make check`. It cannot drive a real collective (that needs a
live DVM) but it guards the invariants that hold with no DVM:

- `prte_grpcomm_release_bcast` defaults to the real broadcast. Nothing
  else establishes that, and a NULL or wrongly-aimed default would
  silently drop every release a controller emits — a DVM-wide hang with no
  diagnostic.
- `prte_grpcomm_globals.context_id` starts at `UINT32_MAX`, and every
  signature/tracker/caddy class constructs with the documented defaults
  and destructs without leaking or crashing.
- **Building a fence tracker**: what a signature naming the daemon job, a
  signature naming nobody, and an unresolvable signature each produce —
  the last of which must leave *nothing* on the tracker list.
- **Parsing a group's directives**: that the signature ends up owning
  copies of the proc arrays, so the caller's arrays survive the
  signature's destructor intact (the test asserts on the *pointers*, which
  is the only way to see the difference before something double-frees).
- **Daemon-failure decisions**: the departed-member predicate, and the
  fence fault handler's choice between re-converging a fence that merely
  lost a message path and ending one that lost a participant. The test
  stands the failed-daemon set and job map up by hand and stubs
  `prte_grpcomm_release_bcast` to capture the release the controller would
  emit. That is where to pin a change to the *policy*; the recovery itself
  needs the dockerswarm harness.

Cases that reach at internals are compiled only when
`PRTE_TEST_GRPCOMM_INTERNALS` is set — grpcomm is compiled straight into
`libprrte` now, so the only thing that can put them out of reach is
`-fvisibility=hidden` (`PRTE_HIDE_INTERNALS`).

The live collectives are covered by the
[dockerswarm harness](../../contrib/dockerswarm/AGENTS.md): `xcast` and
`fence` fall out of nearly every phase, and `test_grpcomm` drives
`PMIx_Group_construct`/`_destruct` and the fence FT cases across the
node containers with the `groupcon` client. That phase exists because
`grp_release()` runs on **every** daemon, and a daemon that merely
*received* the down-tree broadcast — rather than originating it as the
HNP — does not exist on one host.

---

## Debugging

```sh
prte --prtemca grpcomm_base_verbose 5 ...   # trace collective progress
prte --prtemca plm_base_verbose 5 ...       # daemon launch / xcast of launch msg
prte --prtemca state_base_verbose 5 ...     # job-state transitions that drive fences
prte --prtemca rml_base_verbose 5 ...       # routing-tree (children/parent) view
```

Verbosity ≥1 narrates each `xcast`, `fence` and `group` call and its
rollup counts (`nexpected` vs `nreported`); ≥5 adds per-child relay/ack
traffic. Because collectives ride the routing tree, a "collective hang" is
almost always a routing-tree problem — check the RML and the fault
handlers first.

Note that `prte --daemonize` detaches the HNP's stderr, so verbosity given
that way goes nowhere you can read. Use `prterun` for a foreground DVM, or
`--leave-session-attached` with a hand-started daemon.
