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
receives. `fault_handler()` fans the recovery notice out to the xcast,
fence, and group fault handlers in turn — each ends what it cannot
recover — and then, on the global-scope pass, advances the shared
`recovery_epoch` once, which restarts everything still in flight.

---

## Key data structures (`grpcomm_direct.h`)

- **`prte_grpcomm_direct_component_t`** — the component. Holds
  `xcast_ops` (a `prte_grpcomm_xcast_t`), `fence_ops` (list of
  `prte_grpcomm_fence_t`), `group_ops` (list of `prte_grpcomm_group_t`), a
  bounded `completed_group_ops` memo, and the `recovery_epoch` that fence
  and group share.
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
participant — `set_nexpected()`, which is also what the recovery restart
re-runs. The tracker lives on `component.fence_ops`, keyed by exact
proc-signature match.

**`create_dmns()`'s answer is a pair, and NULL means two different
things.** A signature naming the daemon job is "every daemon in the DVM",
reported as a count with a **NULL array** — there is no array to walk, and
`set_nexpected()` answers it as `n_children + 1`. A NULL array with a
**zero** count is the opposite statement, "nothing resolved". Both readers
used to index the array regardless, which dereferenced NULL for any fence
naming the daemon job.

**A signature that cannot be read is refused, and refusal leaves nothing
behind.** `create_dmns()` returns `PRTE_ERR_BAD_PARAM` for an empty
signature or one whose first entry has no nspace — both of which are what
a truncated RML message unpacks to, and the second of which is worse than
it looks, because `PMIX_CHECK_NSPACE` answers *yes* for an empty nspace
against anything, so it would be taken for a daemon-job fence and sized to
expect the whole DVM. `get_tracker()` then removes the half-built tracker
from `fence_ops` before returning NULL. That removal is not tidiness: a
tracker with no daemon set can never complete, and the next fence with the
same signature would *find* it, see a rollup expecting nothing, and answer
immediately with data it never gathered.

**Every exit from the `fence()` handler destructs the signature it built
and completes the caller.** The signature is a stack object with a malloc'd
proc array that `get_tracker()` copies rather than adopts, so walking away
from it leaks it once per `PMIx_Fence` — which was the state of the success
path. And a failure there is invisible to everyone else: the entry point
answered `PRTE_SUCCESS` the moment the request was queued, and the
contribution never entered the rollup, so no release is coming. The handler
completes its own participants with the reason, after clearing the
tracker's `cbfunc` so a later abort cannot complete them twice.

**The fence's deadline is the DVM's to keep.** A participant can put a
`PMIX_TIMEOUT` on a fence, and the controller arms a timer for it on the
first contribution that carries one (largest offered wins), disarming on
convergence and ending the fence with `PMIX_ERR_TIMEOUT` through
`abort_fence_op()` if it fires. Nothing else is watching: the PMIx server
library arms a timeout of its own while it gathers the *local*
contributions, then deletes it the instant the request is handed to the
host — deliberately, so a late host answer cannot reach a tracker it
already released. From that hand-off on, the deadline exists only here.
The harness cannot reproduce a firing: a fence that stalls for a reason
the fault handler already covers is aborted by the fault handler instead,
and one that stalls before the local contributions are complete is timed
out by PMIx before the DVM ever sees it.

**A release with no local callback still has data to free.** `fence_release`
unloads the broadcast into a byte object and hands it to `coll->cbfunc`,
which frees it via `relcb` when the PMIx server is done. A daemon that
holds a tracker only because it relayed for its subtree has no `cbfunc`,
and that is the common case on any interior daemon — so that branch has to
free the payload itself.

The fence `fault_handler` restarts what it can and ends what it cannot.
A fence whose participants all survive lost only a message path and
re-converges over the repaired tree at the new recovery epoch — so the
loss of a pure relay is invisible to it. A fence that genuinely lost a
participant cannot produce its answer, because an allgather has no
opt-in to running degraded, so the controller ends that fence with
`PMIX_ERR_LOST_CONNECTION` via `abort_fence_op()`. Note the difference
from a group construct, which *can* complete on the survivors when asked:
a fence has no equivalent of `PMIX_GROUP_FT_COLLECTIVE`.

It shares the recovery epoch and the restart machinery with `group` —
see *Surviving a daemon failure* below, which describes both.

**Only a fence that was in flight when the failure landed is ended.** A
fence *started afterwards* completes among the survivors, because
`prte_rml_get_num_contributors()` skips the failed daemons when the
tracker is built. That is deliberate, and it is the opposite of where the
group policy sits (at completion, in `check_complete`). The reason is
that the failed-daemon set is permanent: applying the test at completion
would make every later fence naming that namespace fail for the life of
the DVM, which would leave a recoverable job unable to fence at all after
its first loss. If you move this check, that is the regression to expect.

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
  (`prte_grpcomm_direct_group_parse_directives()`:
  `PMIX_GROUP_ASSIGN_CONTEXT_ID`, `PMIX_GROUP_BOOTSTRAP`, `PMIX_TIMEOUT`,
  `PMIX_GROUP_ADD_MEMBERS`, `PMIX_GROUP_INFO`, `PMIX_PROC_DATA` endpoints,
  `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`, `PMIX_LOCAL_COLLECTIVE_STATUS`),
  gets-or-creates the tracker, and relays.

  **A signature owns its arrays — always.** Two directives carry a proc
  array (`PMIX_GROUP_ADD_MEMBERS`, `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`),
  and the array inside a directive belongs to the **PMIx server** that
  delivered the upcall: PMIx frees the directives, arrays and all, once the
  operation completes. The signature's destructor frees what the signature
  holds, so the parse takes a *copy* of each. Pointing at the caller's
  array instead is a double free of live heap, and it is not a theoretical
  one — the destructor grew its `final_order` free before the third
  populator was noticed, and the add-members pointer was being cleared only
  after the pack, which covers no failure before it. If you add a directive
  that carries an array, copy it in `copy_directive_procs()` like the other
  two; do not add a "clear it again before the destructor runs" step
  anywhere.
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
  `nleaders_reported >= nleaders && nfollowers_reported >= nfollowers` for
  bootstrap, else `nreported >= nexpected` — `>=` rather than `==` because a
  fault can lower `nexpected` under a tracker that has already counted more
  than it now needs; the `converged` latch is what keeps that from answering
  twice. A bootstrap additionally requires `0 < nleaders`: both of its counts
  are zero until the first *leader* arrives to supply them, so without that
  guard a tracker created by a follower would satisfy `>= 0` on its own and
  converge on one contribution with an empty membership.
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
  final membership / context-id / grpinfo / endpoints and calls
  `PMIx_server_register_resources`. It is **split in two** around that
  call — see below. The continuation, `grp_release_complete`, records the
  new group in `prte_pmix_server_globals.groups`, returns the assembled
  info to local clients via `coll->cbfunc`, and deletes the tracker
  (`find_delete_tracker`, keyed by groupID).

#### `grp_release` is a continuation, not a wait

The registration used to be waited on with `PMIX_WAIT_THREAD`. Nothing
raced — the lock is a `pmix_lock_t`, so the wakeup from the PMIx thread
touched no PRRTE object — but the wait parked the **progress thread**,
and `prte_event_base` has no thread of its own (`event.c`: "PRTE tools
block in their own loop over the event base"). While it sat there the
daemon was deaf: no RML, no IOF, no other job's state transitions. And
unlike the teardown waits converted for
[#2534](https://github.com/openpmix/prrte/issues/2534), this one is not
a once-per-job cost — it runs on **every daemon for every group
construct**, so a session-heavy job pays it over and over.

So the ordering the wait was enforcing — the group's resources have to
be in the local PMIx server before the local participants are told the
construct succeeded — is now expressed by chaining:

```c
rc = PMIx_server_register_resources(cd->info, cd->ninfo,
                                    grp_release_regcbfunc, cd);
if (PMIX_SUCCESS == rc) {
    return;                 /* the completion callback owns the caddy */
}
```

`grp_release_regcbfunc` runs on the PMIx thread and does nothing but
record the status and `PRTE_PMIX_THREADSHIFT` to `grp_release_resume`,
which calls `grp_release_complete` on the progress thread. Three things
about the shape are load-bearing:

- **A file-local caddy owns everything that has to survive the gap** —
  `prte_grpcomm_release_caddy_t` carries the signature, the status, the
  `nlist`, and every array unpacked from the broadcast, and frees all of
  it in its destructor. The unpacking writes into caddy fields directly
  so that the dozen `goto notify` error paths need no per-path transfer.
- **The tracker is re-looked-up in the continuation, not carried.** The
  progress thread is free while PMIx works, so an abort or a duplicate
  release can complete and delete the tracker in the interim — in which
  case the local participants have already been serviced and there is
  nothing to do. A cached `coll` pointer would be dangling.
- **`PMIX_OPERATION_SUCCEEDED` means it already finished**, and we are
  still on the progress thread, so that path falls straight through to
  `grp_release_complete` rather than waiting for a callback that will
  never come.

Two defects went with the conversion. The registration status was
assigned to a dead local and discarded, so a group whose resources never
made it into the server was still reported to clients as successfully
constructed; it now propagates. And both `PMIX_INFO_LIST_CONVERT` calls
ignored their result while reading `darray` unconditionally — on an
empty list (a construct with no context-id request, no group info and no
endpoints) `darray` is left untouched, so `cd->info` picked up whatever
the *previous* assignment had put there, which on the ilist path was the
`pmix_proc_t` membership array about to be handed to
`PMIx_server_register_resources` as `pmix_info_t` and then freed twice.

Coverage is `test_grpcomm` in the dockerswarm harness — one host cannot
exercise a daemon that merely *received* the release. That phase also runs
the construct with a `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER` directive
(`groupcon --order`, the ranks reversed), which is the end-to-end check on
the array-ownership rule above: the order it asks for is one the DVM would
never arrive at by sorting, and a daemon that freed the caller's array
dies of a double free rather than merely returning the wrong order.

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
- **Daemon failure.** See *Surviving a daemon failure* below.

`abort_group_op()` broadcasts a `PRTE_RML_TAG_GROUP_RELEASE` carrying only
the signature + a completion status; the normal `grp_release` non-success
path then completes each daemon's local participants with that status and
deletes the tracker — so a cancel/abort tears down the collective cleanly
**without tearing down the DVM**, which is the whole point of this work.
It also sets `coll->aborting`, because the tracker is not deleted until
that broadcast comes back around.

### Surviving a daemon failure

This applies to **both** fence and group; where they differ is only in what
they do about a collective that genuinely lost a participant.

A daemon failure invalidates every in-flight rollup: `nexpected` is derived
from the routing tree, and the tree just changed shape. Recovery is a
**restart**, not a repair — each daemon discards what it has gathered,
recomputes what the repaired tree owes it, and re-offers the contribution
it originally made (`coll->my_contribution`, kept for exactly this).

**The epoch.** One `recovery_epoch` per daemon, on the component and shared
by both collectives, stamped on every message on `PRTE_RML_TAG_GROUP` and
`PRTE_RML_TAG_FENCE` as `[epoch][body]`, tells one round from
the next; a contribution stamped older than the receiver's epoch belongs to
a round that no longer exists and is dropped before it is merged.

**Why a per-link round does not work here, and why one epoch does.** The
obvious model is xcast's — `ack_id_down` chosen by the parent, echoed by the
child. It does not transfer. xcast's payload flows *down*, so the parent
always holds the op and can re-poll; a group rollup flows *up*, so the
parent is the party that may hold nothing at all (a pass-through daemon's
tracker is created lazily, on the first arriving contribution). A round
cannot invalidate a contribution the parent has **already absorbed** —
that invalidation has to travel up, and rounds travel down.

What makes a single epoch work is that the restart is *simultaneous*. The
GLOBAL fault scope reaches every daemon from one broadcast, in the same
order everywhere, after the tree is repaired. `PRTE_RML_TAG_DAEMON_DIED` is
in xcast's `process_first` set, so a daemon hands the notice to its own
delivery before forwarding it, and libevent runs that active event before
its next round of socket reads — so a daemon has advanced its epoch before
it can read a contribution from a child that advanced first. A **newer**
stamp should therefore be unreachable. `grp_recv` handles one anyway, by
adopting the epoch (which is what the fault notice would have done a moment
later) and logging it, so the ordering argument stays falsifiable.

**A partial restart is the failure mode to fear**, not a hang: a daemon
that resets and re-sends into a parent that did not would have its subtree
counted twice and another subtree not at all — a quietly wrong membership.
That is why `advance_group_epoch()` walks *every* tracker.

**The policy lives at completion, not in the fault handler.**
`check_complete()` on the controller decides what a construct that lost a
member does: complete on the survivors if `sig->ft_collective`, else
`PMIX_GROUP_CONSTRUCT_ABORT`; abort either way if nothing is left. That is
the one place with the whole picture, it runs exactly once, and it covers
an operation whose first contribution had not yet reached the controller
when the failure landed. The fault handler keeps an early abort purely so
participants of a doomed construct are not made to wait out a
re-convergence that can only end that way.

**`ft_collective` means "some *surviving* participant asked for it."** It
is accumulated by sticky-OR as contributions merge, so a participant that
requested it and then died before rolling up is not visible. Note this is a
deliberate superset of PMIx's own rule, which takes the first
`PMIX_GROUP_FT_COLLECTIVE` in the aggregated block info and ignores the
rest — do not "fix" it to match.

**Losing a pure relay** — a daemon hosting no member but sitting between
the controller and one that does — now completes rather than stalling. Its
`nexpected` falls to zero on the recompute and `check_complete()` fires at
once. Previously such an op was not "affected" by the handler's test, so it
was never aborted either, and hung on a count that could never be reached.

**Still aborted, deliberately:** bootstrap operations (no resolved daemon
set, and `nleaders` is a count with no identities, so there is no way to
work out how much of one died) and anything in flight across a **revival**
(`PRTE_RML_TAG_DAEMON_REVIVED` is forward-first by design, so it cannot
give the ordering an epoch advance needs). The revival path is discriminated
by LOCAL scope with an empty `failed_ranks`.

**A converged collective is finished, on the controller.** Neither the
restart nor the fault handler touches a tracker the controller has already
answered: the release is on the wire, ordered ahead of anything a restart
could send, and every daemon retires its tracker when it lands. Re-running
the rollup there answers twice — a second release, a second context id
consumed, the same group registered twice on every daemon. The test is
*only* valid on the controller: `converged` on any other daemon means it
rolled its aggregate up to its parent, and re-sending that aggregate is
precisely what recovery is for, since the failure may be what swallowed
it.

Two supporting pieces are worth knowing about. `nreported` is backed by
`reported_slots`, a bitmap keyed on `prte_rml_get_subtree_index()` of the
sender, so a replayed contribution is idempotent rather than double-counted
— the info-list accumulation appends with no key matching, so a
double-count is also a duplicated payload. And `completed_group_ops` is a
bounded memo of operations already released, consulted by `grp_recv` so a
straggler cannot build a tracker nothing will ever complete or delete;
`group()` clears the matching entry, because a local client starting an
operation is proof the previous one of that name is over.

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
- **Fence and group share one recovery epoch, advanced in one place.**
  It lives on the component and is bumped by the framework's own
  `fault_handler` in `grpcomm_direct.c`, which then restarts both. Do not
  give a collective its own counter: the restart is only safe because it
  is simultaneous across the DVM, and two counters racing to advance would
  break exactly that. A per-collective handler decides what it cannot
  recover and marks those trackers `aborting`; the shared restart skips
  them.
- **One allocator per array.** The proc arrays on a signature are built
  with `PMIX_PROC_CREATE` and freed with `PMIX_PROC_FREE` everywhere —
  those allocate and free *inside the PMIx library*, so a plain `free()`
  on one crosses the library boundary. It works today because both sides
  land on the same libc; it would not survive a PMIx that accounts for its
  own allocations.
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
