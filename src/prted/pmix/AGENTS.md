# AGENTS.md — `src/prted/pmix` (the PMIx server host module)

Orientation for AI agents and human contributors working in
`src/prted/pmix/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file. Read
[`../AGENTS.md`](../AGENTS.md) first for how this code fits into the
daemon.

---

## What this is

Every PRRTE daemon — the HNP and every `prted` — embeds a PMIx server.
Application processes and tools connect to it and issue PMIx calls. For
anything the PMIx library cannot answer out of its own cache, it calls
*up* into the host, and **this directory is that host**. The whole
directory is one implementation of `pmix_server_module_t`.

```
   app / tool                 PMIx server library            PRRTE
   ──────────                 ───────────────────            ─────
   PMIx_Spawn()      ──►      spawn upcall          ──►  pmix_server_spawn_fn()
                                                              │ thread-shift
                                                              ▼
                                                         interim() → PLM
   PMIx_Get(remote)  ──►      direct_modex upcall    ──►  pmix_server_dmodex_req_fn()
   PMIx_Fence()      ──►      fence_nb upcall        ──►  pmix_server_fencenb_fn() → grpcomm
   PMIx_Notify_event ──►      notify_event upcall    ──►  pmix_server_notify_event() → xcast
```

The module table is at the top of `pmix_server.c`. Each entry is an
upcall; each file below implements a related group of them.

| File | Upcalls / role |
|------|----------------|
| `pmix_server.c` | Init/finalize, MCA params, the module table, the request trackers, direct-modex RML plumbing (`dmdx_recv`/`dmdx_resp`), logging relay, the scheduler relay (`pmix_server_sched`). The biggest file. |
| `pmix_server_internal.h` | `prte_pmix_server_req_t` (the universal request tracker), `prte_pmix_server_op_caddy_t`, the globals struct, and the thread-shift macros. **Read this first.** |
| `pmix_server_register_fns.c` | Turning a `prte_job_t` into the info arrays PMIx needs (`PMIx_server_register_nspace`, `register_client`, tool registration). |
| `pmix_server_dyn.c` | `spawn` — plus `prte_pmix_xfer_job_info()`/`prte_pmix_xfer_app()`, the translators from PMIx directives to PRRTE job/app attributes. Also `connect`/`disconnect`. |
| `pmix_server_queries.c` | `query` — namespaces, proc tables, psets, groups, allocations. |
| `pmix_server_gen.c` | `abort`, `client_finalized`, `client_connected2`, `tool_connected`, `iof_pull`, `push_stdin`, `log`. `client_finalized` is how a **tool**'s departure is learned as well — see below. |
| `pmix_server_fence.c` | `fence_nb` and `direct_modex`. |
| `pmix_server_pub.c` | `publish`/`lookup`/`unpublish` (relayed to the data server; `init_server()` attaches the master to an **external** one). |
| `pmix_server_notify.c` | `notify_event` (up), and the RML receive that fans a peer's event out to local clients (down). |
| `pmix_server_group.c` | `group` — a thin pass-through to grpcomm. |
| `pmix_server_job_ctrl.c` | `job_control` — kill/terminate/signal/define-pset, as daemon commands. |
| `pmix_server_monitor.c` | `monitor` — heartbeat/file monitoring, fanned out to all daemons and collected. |
| `pmix_server_alloc*.c` | `allocate`, relayed to the DVM master and on to a scheduler. |
| `pmix_server_session.c` | `session_control` — see its own section below. |

---

## THE rule for this directory

> **Every function in here that PMIx calls executes on the PMIx progress
> thread. It must capture its arguments and post an event to
> `prte_event_base`, and do nothing else.**

This is the top-level golden rule, and this directory is where it
applies to essentially every function. A `prte_job_t`, a `prte_node_t`, a
`prte_pmix_server_req_t`, `prte_pmix_server_globals.local_reqs` — all of
these belong to the PRRTE progress thread. Touching them from a PMIx
callback is a data race, and freeing something from one is worse.

The canonical shape (from `pmix_server_job_ctrl.c`):

```c
pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, ...)
{
    prte_pmix_server_op_caddy_t *cd;

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proc, requestor, sizeof(pmix_proc_t));
    cd->procs = (pmix_proc_t *) targets;      /* borrowed, see below */
    cd->nprocs = ntargets;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _job_ctrl, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}
```

Three things about that:

- **Arrays are borrowed, not copied.** PMIx keeps the `info`/`procs`/`apps`
  arrays alive until the host invokes the completion callback. That is
  why the caddy stores pointers. It is also why the shifted handler must
  not return without eventually calling `cbfunc`.
- **`PMIX_POST_OBJECT` / `PMIX_ACQUIRE_OBJECT` bracket the hand-off.**
  They are the memory barriers that make the caddy's contents visible to
  the other thread. Every shifted handler starts with `PMIX_ACQUIRE_OBJECT`.
- **The same rule applies in the other direction.** A callback *PMIx*
  invokes on us to deliver a result — `send_alloc_resp`, the session
  `infocbfunc`, `modex_resp` — also runs on the PMIx thread and must
  capture into its own caddy rather than writing the request. Both of
  those use a dedicated response caddy (`prte_alloc_resp_caddy_t`,
  `prte_sessctrl_resp_caddy_t`, `prte_dmdx_resp_caddy_t`) that holds a
  reference on the request so it cannot be reclaimed underneath the
  shift. Copy that pattern; do not write the request from the PMIx
  thread "just this once".

**One upcall in here does not post an event, and it is not an oversight.**
`pmix_server_group_fn()` validates its group id and calls
`prte_grpcomm_group()` straight through, because *that* function is the one
that builds a caddy and posts to `prte_event_base` — the shift happens one
call deeper, so doing it here as well would shift twice. What makes the
pass-through legal is that nothing above the hand-off reads PRRTE state: the
only globals it touches are `prte_pmix_server_globals.output` (an `int`
written once during init) and `PRTE_NAME_PRINT`, whose scratch buffers are
thread-specific storage. Add anything to that function that reads a
`prte_job_t`, a node, or a request array and it stops being legal — move the
work into `prte_grpcomm_group()`'s handler rather than shifting here.

The daemon-command side follows the same discipline: `prted_comm.c`'s
`HALT_VM`/`SHRINK`/`DVM_CLEANUP_JOB` cases issue their PMIx call and
return, and pick up in `_daemon_continue()` once PMIx is done. Nothing on
a running daemon's command path blocks its own event loop waiting on
PMIx.

The two `PRTE_PMIX_WAIT_THREAD` calls left in this directory are both in
`pmix_server_init()`, which runs inside `prte_init()` — before the event
loop exists. There is no loop to park there, so blocking is correct. Any
*new* one outside that function is almost certainly a bug.

The *wakeup* side of those two calls is the mirror image of the golden
rule, and deliberately so: `regcbfunc()` fires on the PMIx progress thread
and calls `PRTE_PMIX_WAKEUP_THREAD` directly rather than thread-shifting.
It has to. The waiter is `pmix_server_init()` itself, blocked in
`PRTE_PMIX_WAIT_THREAD` on the lock's condition variable with nothing
driving `prte_event_base` yet, so a shifted wakeup would never be
dispatched and the daemon would never finish starting. The shifted form
(`prte_pmix_shifted_wakeup()`) is for waiters that are driving the event
loop — which is everybody else.

The narrow exception is a callback that PMIx guarantees to invoke
synchronously on our own thread — `prte_pmix_server_register_nspace`'s
completion when we called it from a shifted handler. Those are marked
with a comment saying so at each site.

---

## `prte_pmix_server_req_t` — the universal tracker

Almost every asynchronous operation in this directory is tracked by one
of these (`pmix_server_internal.h`). It is deliberately a union of every
field any operation might need, which makes it flexible and makes
ownership bugs easy. The parts that matter:

| Field | Meaning |
|-------|---------|
| `tproc` | The **target** of the operation — for a dmodex, the proc whose data was asked for. |
| `target` | Only the monitor and tool-connection paths set this. It is `{"", PMIX_RANK_INVALID}` everywhere else. |
| `proxy` | The peer daemon on the other end of a relayed request. |
| `local_index` | Our index into `prte_pmix_server_globals.local_reqs`. Sent on the wire; the peer echoes it back so we can find the request again. |
| `remote_index` | *Their* index, to echo back to them. |
| `copy` / `dircopy` / `moncopy` | Does the destructor own `info` / `directives` / `monitor`? Set it when you allocate into those fields, or the array leaks. |
| `event_active` / `cycle_active` | Is `ev` / `cycle` armed as a timer? |
| `inprogress` | Someone else (host or PMIx library) currently holds this request; do not release it. |

**`tproc` versus `target` is a live trap.** `PMIx_Check_nspace()` treats
an *empty* nspace as matching anything, and `PMIx_Check_rank()` treats
`PMIX_RANK_WILDCARD` as matching anything. So
`PMIX_CHECK_PROCID(&r->target, &something_wildcard)` returns **true** for
every request that never set `target` — which is all of them outside
monitor/tool-connect. The dmodex de-duplication loop in
`pmix_server_fence.c` had exactly this bug: a `PMIX_RANK_WILDCARD`
job-level fetch matched the first unrelated entry in the array, got
parked as "already requested", and was never answered. Compare `tproc`
against `tproc`.

**...and then screen the entries that name no target at all.** Comparing
`tproc` against `tproc` is only half of it, because a great many requests
never name a target proc — monitor, publish/lookup, spawn, tool connection
— and they sit in the same two arrays as the ones that do. Their `tproc` is
the `{"", PMIX_RANK_INVALID}` sentinel, whose empty nspace matches *every*
namespace, so a job-level fetch (`rank=WILDCARD`) pairs with the first one
of them in the array. **Three** loops match on `tproc` and each must skip
`PMIX_NSPACE_INVALID(r->tproc.nspace)` first: the de-duplication loop in
`pmix_server_fence.c`, the "anyone else waiting for this target" sweep at
the end of `pmix_server_dmdx_resp()`, and `prte_pmix_server_clear()`.
Without the guard the first parks a request that is never answered, and the
other two *release* a request somebody is still waiting on.

**...and the sentinel screen is still not enough, because some requests that
are not modexes name a real proc.** The three scheduler relays — allocate,
session control, and the group context id — deliberately record the
*requesting* process in `tproc`, at both ends of the relay
(`assign_group_ctxid()` in `pmix_server_job_ctrl.c`, and the branches of
`pmix_server_sched()`). That is a genuine identity, so it sails past a
`PMIX_NSPACE_INVALID` check, and it sits in `local_reqs` alongside the
modex requests. A job-level fetch names `nspace/WILDCARD`, and
`PMIX_CHECK_PROCID` covers every rank of that namespace — so the "anyone
else waiting for this target" sweep in `pmix_server_dmdx_resp()` matched a
relayed request from any proc of the job being fetched, cleared its slot and
released it. Nobody was answered: the daemon that relayed it, and the client
behind it, waited for the life of the DVM, and for allocate and session
control that was also a *third* release on an object built to take two.

The discriminator is `mdxcbfunc`, not `tproc`: `PRTE_DMX_REQ` is the only
thing that builds a direct-modex request and it always sets one, and no other
operation sets it at all. Both halves of `pmix_server_dmdx_resp()` test it —
the sweep, and the indexed lookup above it, where the index arrived on the
wire and names a slot that is reused the moment its occupant retires. **So
if you add an operation to `local_reqs`, do not set `mdxcbfunc`**, and if you
ever need to clear it, remember that `rqcon()` NULLing it is the only reason
a recycled block does not carry the previous occupant's — which this code
now *calls* rather than merely compares. `test_request_tracker` pins both.

**The sentinel exists only because the constructor writes it.** `PMIX_NEW`
mallocs and does not zero, so `rqcon()` leaving `tproc` alone did not mean
"empty" — it meant whatever the previous occupant of that block left there,
which for a recycled request is a real proc identity. That is the same
field the three loops above key on, so the matching was not merely
unguarded, it was nondeterministic. `test_request_tracker` in
[`test/unit/prted/test_prted.c`](../../../test/unit/prted/test_prted.c)
pins it down, and does it by recycling a released request rather than by
inspecting a fresh one, because a fresh one usually comes off a zeroed
page and looks fine.

**A job object outlives its map, and finding one proves nothing.** The map
is built when the job is mapped and released the moment the job completes
(`check_complete_resume`, `state/dvm`), while the job object itself lives
until `cleanup_job` runs — a *later* event, and the only one that records
the departure `prte_pmix_server_job_has_departed()` tests. A dmodex request
landing in between finds a job that is present, is not yet "departed", and
has a NULL `map`. The `PMIX_RANK_WILDCARD` arm handed that job straight to
`prte_pmix_server_register_nspace()`, which assembles the job-level data by
walking `map->nodes` — a NULL dereference that took the daemon down after
the job had run correctly, so it read as a clean run with a segfault at the
end. The proc-level arm immediately below already refused the equivalent
case (a proc the mapper has not placed) with `PMIX_ERR_NOT_FOUND`; the
wildcard arm now refuses it the same way. The window is small and entirely
ordinary — a parent asking about a short-lived child it just spawned
(`examples/dynamic.c`) hit it in roughly 1% of runs on the container
harness, in both spellings of `report-child-jobs-separately` and on master
before this change.

**Two request arrays, two meanings.** `local_reqs` holds requests *we*
originated and are waiting on; `remote_reqs` holds requests *a peer*
asked us to service. `prte_pmix_server_clear()` sweeps only
`remote_reqs`, because the local ones belong to callbacks that will still
fire.

Two things about that sweep, both of which it got wrong:

- **Discarding a request is not answering it.** Every entry it drops is a
  peer daemon waiting for a reply, and dropping it in silence leaves that
  peer waiting for the life of the DVM. The sweep runs when a job is done,
  so the honest reply is `PRTE_ERR_NOT_FOUND` — the proc it asked about is
  gone and its data with it, which is a question that no longer has an
  answer rather than a failure. Only for requests not marked `inprogress`:
  those are held by somebody who will answer them.
- **It has to skip the entries that name no target.** The match is on
  `req->tproc`, and a *monitor* request never sets one — so its nspace is
  empty, which is PMIx's wildcard and matches every job. Every job that
  ended was quietly taking the outstanding monitor requests with it. Guard
  with `PMIX_NSPACE_INVALID()` first; this is the same trap described above
  for `tproc` versus `target`, in a second place.
- **A request it leaves alive must forget its index.** The slot is free the
  instant it is cleared and the array hands out the lowest free one, so an
  `inprogress` request that keeps its old `local_index` will, when its
  holder finally answers, clear the slot that by then belongs to an
  unrelated request — and leave *that* peer waiting. Setting `local_index`
  back to `-1` is the whole fix; `pmix_pointer_array_set_item()` refuses a
  negative index, so every later use is inert rather than wrong. The same
  rule appears again in the session-control path, for the same reason.

---

## The relay pattern

Anything a non-master daemon cannot answer itself is packed and sent to
the DVM master over an RML tag, and the answer comes back on a partner
tag. All of them follow the same shape, and all of them carry the
requester's index so the reply can find its way home:

| Operation | Request tag | Response tag |
|-----------|-------------|--------------|
| direct modex | `PRTE_RML_TAG_DIRECT_MODEX` | `..._DIRECT_MODEX_RESP` |
| spawn | `PRTE_RML_TAG_PLM` | `..._LAUNCH_RESP` |
| tool connection | `PRTE_RML_TAG_PLM` | `..._TCONN_RESP` |
| allocate / session ctrl | `PRTE_RML_TAG_SCHED` | `..._SCHED_RESP` |
| monitor | `..._MONITOR_REQUEST` | `..._MONITOR_RESP` |
| log | `PRTE_RML_TAG_LOGGING` | `..._LOGGING_RESP` |

When adding to a relay, remember that the index arriving on the wire is
untrusted input. `pmix_pointer_array_get_item()` bounds-checks and
returns NULL, so check for NULL — but do not then `return` without
completing the request, because the requester is still waiting.

**A relay must answer even a request it cannot parse.** Every `goto reply`
in `pmix_server_sched()` happens after the requestor's index has been
unpacked, so there is always somebody addressable on the other end holding
a tracker for it — and until recently the "we could not build a request"
arm printed a line and returned, which left that daemon's client blocked
for the life of the DVM. `send_sched_error()` packs the two fields
`pmix_server_alloc_request_resp()` reads when the status is not success
(the status and the index) and sends them; that is the minimum a relay
owes a peer it cannot serve.

**A timed-out request is finished, and its index belongs to the peer
again.** `timeout_cbfunc()` answers the peer with `PRTE_ERR_TIMEOUT` and
must then retire the request outright: delete the retry cycle, clear the
array slot, and release it unless somebody else holds it. Leaving it armed
does two bad things — the daemon retries a request nobody is waiting for
until the DVM ends, and a retry that eventually succeeds sends a *second*
reply under an index the peer has long since reused, so an unrelated
request of theirs is completed with data it never asked for. The
`timed_out` flag is what stops the same thing happening from the other
direction, when the answer we were waiting on arrives after we gave up:
`_mdxresp()` drops a late payload rather than putting it on the wire.

**...and a relay that cannot *build* its request must fail it, not ship
what it managed.** The tool-connection relay in `_toolconn()`
(`pmix_server_gen.c`) logged each pack failure and packed on, so the master
received a buffer it could not read - and with no index unpacked from it,
nobody to answer. The tool then waited for the life of the DVM. A pack
failure there now releases the buffer, gives the array slot back, and
answers the tool.

**The info array on a tool connection is the tool's own, straight off the
wire.** `ptl_base_connection_hdlr.c` unpacks whatever the connecting
process sent and hands it to the `tool_connected` upcall with only the
uid/gid/version appended, so `_toolconn()` is parsing untrusted input in
exactly the way `prte_pmix_xfer_job_info()` is - see the NULL-value rule
under "Directive translation" below, which is the same rule. It bit here
too: `PMIX_HOSTNAME` and `PMIX_CMD_LINE` were `strdup`ed unchecked, and a
`PMIX_STRING` carrying no string survives the wire as a NULL (the packer
writes a zero length, the unpacker hands back NULL), so any tool could
segfault the daemon it attached to by connecting.

**A directive's value is untrusted until its type has been checked, and
`job_control` is the second place that bit.** The directives reach
`process_job_ctrl()` exactly as the client sent them - PMIx's job-control
path forwards the array to the host without inspecting what any entry holds
- so reading a fixed member of the value union is reading whatever eight
bytes the caller chose to put there. `PMIX_JOB_CTRL_DEFINE_PSET` took the
pset name straight from `value.data.string` and handed it to the packer,
which calls `strlen` on it: any process attached to a daemon could fault it
with one `PMIx_Job_control` carrying that key with, say, a `PMIX_SIZE`
value. Check the type, and check the string is there - and while you are
about it check the parts of the request the *receiving* daemon will need,
because a directive that is merely useless rather than fatal still gets
broadcast to the whole DVM and answered with success. A pset with no members
is the case: `PMIx_Proc_create(0)` returns NULL and PMIx refuses a set with
no name or no members, so every daemon logged an error and the requestor was
told it had worked.

**A query's qualifiers are the same array under another name.** `_query()`
(`pmix_server_queries.c`) reads six of them - `PMIX_NSPACE`, `PMIX_GROUP_ID`,
`PMIX_HOSTNAME`, `PMIX_PSET_NAME`, `PMIX_ALLOC_ID`, `PMIX_ALLOC_PROPERTY` -
and every one is a string it goes on to hand to `strlen`, `strcmp`,
`PMIx_Check_nspace` or a session lookup, so this is the fault version of the
rule rather than the quiet one. Refuse a mistyped qualifier with
`PMIX_ERR_BAD_PARAM`; do **not** fall back to treating it as absent, because
"absent" has a meaning for all six of them and it is a different answer, not
an error — a mistyped `PMIX_HOSTNAME` treated as unset makes the server-URI
query answer about *this* node. The two numeric qualifiers were already read
through `PMIx_Value_get_number()`, which refuses a non-number; check what it
says, or a bad value silently leaves the sentinel in place and is ignored.

**The same rule in publish/lookup/unpublish costs a mis-route rather than a
fault, which is why it survived longer.** `scan_directives()`
(`pmix_server_pub.c`) reads `PMIX_RANGE` and `PMIX_TIMEOUT` out of the
client's own info array, and neither is a pointer — so reading the wrong
member of the union produces a plausible value instead of a crash. The range
is what decides where the request goes: SESSION and GLOBAL are relayed to the
master and on to the data server, LOCAL is answered here. Read it out of the
wrong member and a publish the application meant to be visible across the
session is quietly kept on this daemon, and the lookup that was supposed to
find it does not. The timeout is the width half of the same rule — read with
`PMIx_Value_get_number()`, never off `value.data.integer`, because a caller may
legally have sent a `PMIX_SIZE`.

**And the RML gives a refused buffer back.** `PRTE_RML_RELIABLE_SEND` takes
ownership of what it is handed *only by succeeding*; on an error return the
buffer is still the caller's, emptied of its payload or not. See
[`../../rml/relm/AGENTS.md`](../../rml/relm/AGENTS.md). Every relay in this
directory that reaches its callback tail through a failed send has to release
its buffer on the way.

**A collecting relay must complete on *every* path.** `monitor`
fans out to all daemons and counts responses (`ndaemons` vs `nreported`).
It increments `nreported` as soon as a response arrives, so a response
that then fails to unpack must still fall through to the "have all
daemons reported?" check. Returning early there means that if the *last*
daemon's response is malformed, the request never completes and the
client hangs forever.

The same obligation runs the other way, on the daemon *serving* the
request: `mycbfn()` used to abandon a reply it could not pack, and a
daemon that goes quiet is one the requestor counts forever.
`send_monitor_error()` is the minimum owed there — our vpid, the room
number in the requestor's tracker, and why.

**A daemon that dies mid-collective is accounted, not waited on.** Nothing
in this rollup is keyed on the routing tree — it xcasts the request and then
counts direct replies — so the tree can be repaired around a dead daemon
while the request goes on counting a DVM that no longer exists. It is wired
into the same dispatch every other in-flight collective uses:
`prte_pmix_server_fault_handler()`, called from `src/rml/routed_radix.c`
beside `prte_grpcomm_fault_handler()`.

What it does there is *not* what fence and group do. Their recovery is a
restart — discard, recompute what the repaired tree owes, re-offer the
contribution kept for the purpose — which works because a fence contribution
is idempotent and re-derivable. A monitor reply is a sample of live state on
a node that has just ceased to exist; there is nothing to re-offer. So
recovery here is to stop waiting and say the sample is short. It needs no
epoch, and deliberately does not have one.

That is why the request records **which** daemons it is waiting on
(`expected_dmns`) and which have been accounted for (`reported_dmns`), rather
than the bare count it used to keep. A count cannot answer the only question
a death poses — had that daemon already reported? — and both readings of it
are wrong: decrement when it had reported and `nreported` sails past the
target, decrement when it had not and you are correct only by luck. The
identities also make the accounting idempotent, which it must be, because the
routing tree reports every death **twice**, once at LOCAL scope and again at
GLOBAL, and a rank can be named again later by an adoption notice to a new
parent. Screening against `expected_dmns` is the other half: a death this
request was never waiting on must not be counted at all, or it completes
early — before the daemons it *is* waiting on have answered.

The same two sets are what make a duplicate response harmless, which the
bare count did not: counting one daemon twice completed the request before
the daemon whose slot it took was heard from.

**The status says how much of the DVM answered.** All expected daemons
reported success — `PMIX_SUCCESS`. Some answered and some could not —
`PMIX_ERR_PARTIAL_SUCCESS`, because the caller is holding a sample of part of
the DVM and cannot otherwise tell. None answered — the reason, not a claim of
partiality. Note that `pstatus` carries the *caller's* monitor code until
`mfn()` packs it into the fan-out, and is cleared there before it starts
accumulating the collective's own result; and that it keeps the **first**
non-success rather than the last, which used to mean one daemon's failure was
erased by the next daemon's success.

**And a count of zero completes too.** `ndaemons` is `num_daemons - 1`,
because the requesting daemon skips its own copy of the broadcast — so on
a DVM of one it is zero, the xcast reaches nobody who will answer, and the
completion test lives *only* in the response handler that nothing will
ever run. PMIx has already taken this node's own contribution before
up-calling (it asks the host only about participation it judges remote,
and merges the local half itself), so an empty success is the whole of the
honest answer. `mfn()` gives it before it builds anything.

**The monitor tracker carries two indices and they name different
arrays.** `local_index` is our own room — in `local_reqs` on the daemon
that originated the request, in `remote_reqs` on the daemon serving it.
`remote_index` is the *requestor's* room in *its* `local_reqs`, and it
means nothing as a subscript on any array of ours. `mycbfn()` cleared
`remote_reqs[remote_index]` when it finished: that left the served
request's real slot holding a pointer to the object it then freed — which
`prte_pmix_server_clear()` walks — and unlinked whatever unrelated peer
request happened to occupy the slot it did name. The two coincide while
one request is in flight, which is why it survived: both are zero.

**A response's index does not prove what kind of request it found.**
`pmix_server_monitor_resp()` bounds-checks the index it unpacks and
NULL-checks the slot, but a slot is handed out again the instant its
occupant retires, so a response that crossed with a retirement lands on a
live request of some other kind — where it counts a report, overwrites the
status, and merges monitor results into an `info` array that request never
allocated. `req->monitor` is the discriminator (this file sets it and
nothing else does), exactly as `req->mdxcbfunc` is for the direct-modex
response. Any new operation sharing these arrays needs the same
treatment.

---

## What a daemon publishes, and what it derives

`prte_pmix_server_register_nspace()` hands its local PMIx server a
`PMIX_PROC_INFO_ARRAY` for **every** proc of a job — rank, global rank,
app rank, appnum, local and node rank, node id, hostname and, for the procs
it hosts, cpuset and the locality string generated from it. Every daemon,
for the whole job. That is a table proportional to the job on a node that
will only ever run its own slice of it, and `prte_hostname_cutoff` — which
drops one field of it above a thousand nodes — is the record of that wall
having been met once already.

**Only the two location keys are withheld today, and for a different
reason** (the cpuset scatter, below). `prte_pmix_lazy_procdata` — on by
default — switches on the *deriving* half of the narrowing: when PMIx asks
for one of the placement keys through the `direct_modex` upcall,
`dmodex_req()` answers out of `jdata->procs[rank]` rather than going to the
hosting daemon. The *withholding* half is **not** in: the per-proc loop
here still emits an array for every proc of the job on every daemon, so the
derivation almost never fires — everything the eager registration published
is found locally first. Do not read the paragraphs below as a description
of a daemon that publishes only its own procs; they describe the rules any
such narrowing must keep to, and
[`docs/todo.rst`](../../../docs/todo.rst) carries the measurement of what
the derivation is worth as it stands and what the other half would cost.
Four things about the derivation are load bearing:

- **The key set is what PRRTE is the authority for, not what an app wants.**
  `derivable_key()` lists the placement and binding this DVM computed —
  a closed set. Everything else on a proc was put there by the application,
  and PRRTE knows nothing about it, so those requests go out on the wire
  exactly as before. Do not extend the list by guessing at what some MPI
  asks for.
- **`derive_proc_data()` mirrors the per-proc section of
  `register_nspace()`.** A key added to one and not the other is a key
  that becomes a wire round trip (harmless, but a silent loss of the
  point), or worse, one whose two spellings disagree. `PMIX_PARENT_ID` and
  `PMIX_DEVICE_DISTANCES` are deliberately *not* derived: they are still
  published by the daemon that hosts the proc, so a request for one falls
  through to that daemon and is answered there.
- **The answer is a packed blob of `pmix_kval_t`, not a bare success.**
  PMIx stores a modex reply itself, and for a specific rank it stores it
  under `PMIX_REMOTE` — which is where the re-satisfy after a reply then
  looks. Storing the values here with `PMIx_Data_store_internal` would put
  them in `PMIX_INTERNAL`, which that lookup does not reach, and the
  request would fail with the data sitting in memory. (The empty-blob
  answer is correct only for `PMIX_RANK_WILDCARD`, where PMIx assumes
  `register_nspace` supplied it.)
- **A refresh must not be answered from here.** `PMIX_GET_REFRESH_CACHE` is
  the caller saying our copy may be stale, which is exactly when we must
  go and ask.
- **`dmodex_req()` calls `PMIx_Get` on the PRRTE progress thread, and that
  is safe for one reason only.** It is the blocking form, and it blocks the
  caller until the *PMIx* progress thread answers — so the question is
  whether that answer can require anything of the thread now waiting.  It
  cannot, because `get_data()` refuses outright for a peer that is a server
  and not a tool (`PMIx_ERR_NOT_FOUND` rather than a request parked pending
  a commit), and a daemon's `pmix_globals.mypeer` stays server-only:
  `PMIx_tool_attach_to_server()` builds a peer for the server it attached to
  and never restamps ours, so even the master with a scheduler attached does
  not take the tool branch.  What is left is a local datastore lookup on the
  other thread.  Two things would break that: making our own peer a tool,
  and letting a `PMIX_GET_REFRESH_CACHE` through — the refresh runs *ahead*
  of the server check inside `PMIx_Get`, which is why the call here is
  guarded by `!refresh_cache` and not merely skipped as an optimization.
- **`PMIX_REQUIRED_KEY` never arrives with a NULL string**, so the `strdup`
  of it is not the crash it looks like: PMIx loads that key under
  `if (NULL != key)`, and the only other producer
  (`pmix_pending_nspace_requests`) hands up the array the first one built.
  A NULL key reaches us as no `PMIX_REQUIRED_KEY` at all, which is why
  `prte_pmix_server_derivable_key()` takes NULL and answers false.
- **A `PMIx_Get`'s `PMIX_TIMEOUT` reaches us, and we are the only one who
  honors it.** PMIx arms no timer on a host request — deliberately, so as
  not to race a host that also supports one — but it does hand the caller's
  directives up to `direct_modex`, they are packed on to the servicing
  daemon, and `pmix_server_dmdx_recv()` arms the timer. That is why the
  attribute table registers `PMIX_TIMEOUT` under `PMIx_Get`.

**`register_nspace()` is not called once per job.** The wildcard arm of
`dmodex_req()` calls it again whenever a client asks a daemon that hosts none
of the job's procs for job-level data the local server does not hold — once
per such get. Rebuilding the info arrays is what that caller wants; anything
in there with a *side effect* has to be idempotent, or it repeats at that
rate. The process sets are the case: appending the registry entry again
duplicated the pset in every query answer and took a reference on the job
object that nothing would give back, so that append is now guarded by a
lookup.

### A daemon registers a namespace exactly once

That is the invariant, and `dmodex_req()`'s wildcard arm is what enforces
it. **Do not assume the caller that gets there hosts none of the job's
procs.** PMIx sends the host a `PMIX_RANK_WILDCARD` `direct_modex` on two
quite different grounds (`pmix_server_get.c`): it holds nothing at all for
the namespace, *or* it holds the namespace but the **reserved** key asked
for came back empty from its own store. Only the first is ours to fix. The
second reaches the daemon that forked the asking client as readily as any
other, because PRRTE does not publish every reserved job-level key — a
`PMIx_Get` of `PMIX_NUM_SLOTS` from an ordinary app is enough, and on a
one-node `prterun -n 2` it produced a *second* `register_nspace()` on the
HNP with both of its own ranks coming back `PMIX_ERR_DUPLICATE_KEY`.

Re-registering cannot help: it assembles and stores the identical data from
the same job object, so a key that was not in it the first time is not in
it now, and the client waits through the whole thing to be told
`NOT_FOUND` anyway. So the arm answers `PMIX_ERR_NOT_FOUND` directly when
the job is already registered, which is what `PRTE_JOB_NSPACE_REGISTERED`
is for — the attribute that until now was written and never read.

**It is set when the registration *completes*, not when we finish
assembling it** (`_nspace_reg_done`, which is why the registration caddy
holds a reference on the job). "Registered" has to mean *PMIx has our
answer*; a request arriving mid-flight would otherwise be told `NOT_FOUND`
about data that was seconds from landing. That window is instead the one
place a namespace can still be registered twice, which is why the client
loop keeps `PMIX_ERR_DUPLICATE_KEY` as a tolerated status — PMIx must
refuse a second add of a rank (a duplicate entry in its rank list puts that
list permanently past `nlocalprocs`, so `all_registered` is never set and
every collective involving the namespace hangs) and reports it as a hard
error.

**None of this is the lazy-procdata derivation.** That answers a request for
a *per-proc* placement key (`prte_pmix_server_derivable_key()` — rank,
appnum, nodeid, hostname, cpuset, locality) out of the job object, and it is
asked for with a *specific* rank, so it arrives at the proc-level arm below.
It never comes through the wildcard arm, and the wildcard arm has nothing to
derive.

**A client we cannot register is a launch failure, not a log line.** Every
other status from `PMIx_server_register_client` aborts the registration and
returns an error. Forking a proc our own PMIx server will refuse at
`PMIx_Init` only moves the failure to where nobody can read it — the
application sees an obscure error some way downstream of a launch that
appeared to succeed. Returning the error puts it where it belongs:
`job_reg_join()` in `odls` fails this daemon's procs and activates
`PRTE_JOB_STATE_NEVER_LAUNCHED`, and the tool's `PMIx_Spawn` returns
`PMIX_ERR_JOB_FAILED_TO_LAUNCH`.

### A binding we were not sent is not a binding of "none"

`prte_proc_t.cpuset` is NULL in two completely different situations, and
since the launch message started **scattering** the cpusets — each daemon
is sent only the bindings of the procs it will fork, see
[`src/mca/odls/AGENTS.md`](../../mca/odls/AGENTS.md) — both are ordinary:

| NULL cpuset on | means |
|----------------|-------|
| a proc we host | the mapper bound nothing (`--bind-to none`, or it could not) |
| any other proc | we were never told, and are not the authority |

Everything that reads a cpuset therefore has to ask *whose proc it is*
first, because the answer to "where is it bound" differs and neither
answer is an error:

- **`register_nspace()`** publishes `PMIX_CPUSET` and the locality string
  when it has a cpuset, publishes a **NULL** `PMIX_LOCALITY_STRING` — which
  positively asserts "unbound" — only for a proc *it hosts*, and publishes
  nothing at all for any other. Silence is what makes a get fall through to
  somebody who knows; a NULL locality is an answer, and the wrong one.
- **`derive_proc_data()`** does the same, and `dmodex_req()` goes further:
  when the key asked for is `PMIX_CPUSET` or `PMIX_LOCALITY_STRING` and we
  hold neither the cpuset nor the proc, it declines to derive at all and
  sends the request to the daemon that forks the proc.

The failure this prevents is silent — a peer told that a bound process is
unbound, on a path where nothing returns an error.

### The hosting daemon answers a placement key from the job, not from the process

That referral only works because of the other half, in
`pmix_server_dmdx_recv()` (`pmix_server.c`): a direct modex whose
`PMIX_REQUIRED_KEY` is one of the `derivable_key()` set is answered
**straight out of `jdata->procs[rank]`**, by the same
`prte_pmix_server_derive_proc_data()`, before the handler goes anywhere near
its own PMIx server.

That ordering is load bearing, and getting it wrong is a hang rather than a
wrong answer. The ordinary path below it asks the local server for the key
and, not finding it, parks the request on a two-second retry until the
process publishes something. For application data that is exactly right —
the asker wants a value the process has not produced yet. For **PRRTE's own**
placement and binding it is a deadlock: those keys have never required a
`PMIx_Put` from the proc being asked about, and a process that only ever
*gets* never commits anything, so the request is never satisfied. Seen
outright with `contrib/dockerswarm/peerinfo.c`, which does nothing but get:
every rank hung.

So the rule for anything added to `derivable_key()`: **this daemon must be
able to answer it from what PRRTE knows, on both sides.** The asking daemon
derives it when it holds the facts; the hosting daemon derives it when asked;
neither waits on the process.

**This depends on a PMIx that surfaces the request at all**, which is why
it could not have been written earlier: a client's get for a reserved key
its server did not hold used to have `PMIX_IMMEDIATE` forced onto it,
confining the search to that server and answering `PMIX_ERR_NOT_FOUND`
without ever up-calling. Not a live concern — PRRTE requires PMIx 7.0 at
configure *and* at runtime (`pmix_server.c`, `PRTE_PMIX_MINIMUM_VERSION`),
and every PMIx that clears that bar surfaces the request.

**What a peer may ask for is not knowable from this tree.** PRRTE is not
one MPI's runtime; several programming libraries build on it, and any of
them may read a reserved key of a proc on another node. That is the whole
reason the split is drawn at *what PRRTE is the authority for* rather than
at what anything has been observed to want, and it is why the wire path
below the derivation has to stay correct rather than becoming vestigial.
In particular, do not read the test coverage below as evidence that these
keys are rarely wanted — it says what the harness does, and nothing about
what an application does.

Nothing on one node exercises any of this — every proc is local and there
is nothing to derive. `contrib/dockerswarm`'s `peerinfo` client is what
does: each rank asks every other rank in its job where it is, and the
harness asserts that what a rank was told about a peer is what that peer
says about itself. That comparison *is* the A/B, because a proc's own
daemon publishes its data either way.

---

## Answering a query

`_query()` is one very long `if`/`else if` chain over the keys of each
`pmix_query_t`, in the shape of `prte_pmix_xfer_job_info()` and with the same
hazards. Four things about it that are not local to any one arm:

- **It walks every job object in the array, and meets them at every stage of
  their lives.** So a pointer that is populated at some point in a job's
  lifecycle is not populated for every job it will see: `jdata->session` is
  optional (the launch path falls back to `prte_default_session` where it
  finds none) and `proct->node` is NULL until the mapper places the proc.
  Both faulted here, and the proc table two arms further down had already had
  the check the namespace-info arm was missing.
- **An arm that builds an info list owns it on every way out.** The nesting
  goes three deep in places — a per-proc list inside a per-job list inside the
  arm's own — and an early exit has to release all of the ones it is inside,
  not just the innermost. Nine exits released the inner list and left the
  outer one holding every job gathered so far.
- **`dry` is shared by every arm** and is reused, converted and destructed
  over and over. It is initialized at its declaration because the `done:`
  label is reachable from qualifier failures that run before any arm has
  touched it.
- **The status the caller gets is decided at `done:` — but only if no arm
  decided it first.** Nothing at all is `PMIX_ERR_NOT_FOUND`, fewer results
  than keys asked for is `PMIX_QUERY_PARTIAL_SUCCESS`, and an arm that could
  not answer sets `ret` and jumps. That last case is why the decision has to
  be gated on `ret` still being success: an arm that gives up leaves the
  result list empty, which is indistinguishable at `done:` from having looked
  and found nothing — so an ungated substitution replaces every real error
  with `PMIX_ERR_NOT_FOUND` and the caller cannot tell "there is no such
  thing" from "you asked wrongly". An arm that adds no result and does *not*
  set `ret` — the two resource-usage ones — makes the query answer "looked
  and found nothing" where the default arm would have said
  `PMIX_ERR_NOT_SUPPORTED`; see
  [`docs/todo.rst`](../../../docs/todo.rst).

---

## Info-array expansion

### A daemon must not answer out of state it does not have

A prted holds the *identity* half of the DVM's node table and nothing
else. The nidmap ships node names, aliases, daemon vpids and pool slots
(see [`src/util/nidmap.c`](../../util/nidmap.c)) — never `slots`,
`slots_max`, `slots_inuse` or node `state`, because every writer of those
runs only on the master: the `ras` components, the hostfile and
`dash_host` parsers, `plm_base_setup_virtual_machine()`. `prte_sessions`
on a prted likewise holds the default session and nothing more. An arm
that reads either on a daemon gets a default-constructed **zero, returned
as `PMIX_SUCCESS`** — a wrong answer no caller can tell from a right one.
The rank that happens to land on the master gets the truth; every other
rank gets nothing, silently.

So such an arm does not read that state directly. It goes through
`prte_get_allocated_nodes()`, `prte_get_allocation_session()` or
`prte_get_allocation_sessions()`
([`src/runtime/prte_globals.c`](../../runtime/prte_globals.c)), which
succeed on the master and return `PRTE_ERR_NOT_AUTHORITATIVE` anywhere
else. An arm that gets that back jumps to the `defer:` label at the foot
of the key loop; at `done:` the deferred keys go to the master on
`PRTE_RML_TAG_QUERY`, and its reply is merged into the results gathered
locally, so the client sees one answer covering every key it asked for.

**The decision is made by the read, never by a list of keys.** Which keys
need the master is not knowable in advance, and a written-down list goes
stale the first time someone adds one — silently, because the stale case
returns a plausible zero rather than failing. Which *reads* cannot be
satisfied locally is exactly the three accessors above, and that is
enforced where it is used. A key added tomorrow that reads capacity is
relayed with no edit here; one that reads only what the nidmap and the
launch message already deliver stays local with no edit either.
[`test/unit/check_query_authority.py`](../../../test/unit/check_query_authority.py)
fails `make check` if this file reaches that state any other way.

Deferral is per **key**, not per query and not per request, because the
things a daemon must answer for *itself* can arrive in the same
`PMIx_Query_info` as a key only the master can answer: `PMIX_HWLOC_XML_V1`
and `_V2` export this node's topology, an unqualified `PMIX_SERVER_URI` is
this daemon's own URI, and `PMIX_QUERY_LOCAL_PROC_TABLE` means the procs
this daemon is hosting. Relaying a whole query would answer those about
the master.

The master answers with the same `_query()`: `pmix_server_query_request()`
rebuilds a caddy and posts it, and there nothing defers, so it completes
locally. The relay uses the tracker pattern described under
[The relay pattern](#the-relay-pattern), and carries the *original*
requestor so the master defaults the query to the right job rather than to
the asking daemon. `contrib/dockerswarm`'s `slotinfo` client is the
regression test: on one node every rank is on the master, and the bug
cannot be seen at all.


Several relays add an entry (usually `PMIX_REQUESTOR`) to an info array
before passing it on. The idiom is three steps and all three are
required:

```c
PMIX_INFO_CREATE(xfer, ninfo + 1);          /* 1. allocate one extra    */
for (n = 0; n < ninfo; n++) {
    PMIX_INFO_XFER(&xfer[n], &info[n]);
}
PMIX_INFO_LOAD(&xfer[ninfo], PMIX_REQUESTOR, &proc, PMIX_PROC);
++ninfo;                                     /* 2. count the new entry  */
req->copy = true;                            /* 3. record who frees it  */
```

Skipping (2) is the classic failure: the added entry is never sent
downstream (defeating the point of adding it) and the destructor frees
one element short. Worse, if the "empty" case sets `ninfo = 1` *and*
allocates one element, the `PMIX_INFO_LOAD` writes past the end of the
array. `pmix_server_sched()` in `pmix_server.c` shipped that
out-of-bounds write; `pass_request()` in `pmix_server_session.c` has
always had it right and is the model.

---

## Directive translation (`pmix_server_dyn.c`)

`prte_pmix_xfer_job_info()` and `prte_pmix_xfer_app()` are the two
biggest `if`/`else if` chains in the tree: they map PMIx spawn directives
onto PRRTE job and app attributes. Notes for extending them:

- **Both return PRRTE codes**, not PMIx status. Their callers convert.
- **The final `else` is not an error.** An unrecognized job-level key is
  cached via `pmix_server_cache_job_info()` for delivery at nspace
  registration, so that an attribute PRRTE knows nothing about still
  reaches the application. Keep that fallback.
- **A conflicting policy is an error, never an overwrite.** `PMIX_MAPBY`
  and `PMIX_PPR` are two spellings of the same thing, so giving both is
  refused; likewise a second `RANKBY`/`BINDTO`.
- **`prte_pmix_xfer_app()` does not own `jdata`.** Its caller constructed
  the job and will dispose of it on an error return; releasing it here
  hands the caller a dangling pointer. (It used to do this on a `getcwd`
  failure.)
- **A directive the mapper must read has to be `PRTE_ATTR_GLOBAL`.** The
  mapper sees an unpacked *copy* of the job, and only global attributes
  survive the pack. This is why every `prte_set_attribute` in these
  functions passes `PRTE_ATTR_GLOBAL`.
- **Watch for shadowed branches.** The chain is long enough that a key
  handled early makes a later `else if` on the same key dead code —
  `PMIX_TIMEOUT` was matched by the `SPAWN_TIMEOUT` branch and its own
  branch was unreachable.
- **A value the branch *parses* has to be checked; one it merely passes on
  does not.** `PMIX_INFO_LOAD(&i, KEY, NULL, PMIX_STRING)` is ordinary,
  legal PMIx — `pmix_bfrops_base_value_load()` zeroes the union for a NULL
  `data`, so the info arrives as a `PMIX_STRING` whose `data.string` is
  NULL, and PMIx's own `PMIX_INFO_TRUE` handles that case deliberately.
  The many branches that hand the pointer straight to `prte_set_attribute`
  or to a policy setter are safe, because those all test for NULL. The
  branches that read it are not, and four of them faulted: `PMIX_STDIN_TGT`
  (`strcmp`), `PMIX_PARENT_ID` (`PMIX_XFER_PROCID` on a NULL proc),
  `PMIX_WDIR` (`pmix_path_is_absolute` dereferences its argument
  immediately), and both timeout keys — `PMIX_CONVERT_TIME` indexes
  `tmp[sz-1]` without checking that the split produced anything, so a NULL
  string faults *inside PMIx*. Any of these let a client segfault the
  daemon it was attached to with one `PMIx_Spawn`, which is why
  `test_xfer_job_info` now pins each of them.
- **Read a number with `PMIx_Value_get_number()`, never off a fixed union
  member.** `u16 = info->value.data.uint32` reads four bytes of a union the
  caller may have filled with two, which the truncation back to `uint16_t`
  rescues on a little-endian machine and does not on a big-endian one. The
  function converts from whatever integer type the caller actually used,
  refuses a value the destination cannot hold without changing sign or
  losing precision, and says so in its status — which is also the only way
  a non-numeric value
  gets refused rather than silently becoming a count.
- **`pmix_getcwd()` answers in PMIx statuses.** It looks like one more
  PRRTE-shaped `int` helper and it is not — it returns `PMIX_ERR_BAD_PARAM`
  and `PMIX_ERR_IN_ERRNO`. `prte_pmix_xfer_app()` returned one of those to
  a caller reading PRRTE codes, which then ran it through
  `prte_pmix_convert_rc()` — the PRRTE→PMIx direction — and the client got
  a bare `PMIX_ERROR`. The same trap is in `spawn()`, where `rc` carried a
  `PMIx_Data_pack` status on one path and a `prte_job_pack`/RML code on the
  others while one `goto callback` converted them all the same way; the two
  spaces live in separate variables there now.

---

## Connected assemblages (`pmix_server_connect.c`)

`PMIx_Connect` is not a communication operation — the fence it runs is a
means, not the point. What it asks for is that the host **treat the
participants as one application for fault purposes**, and the concrete
obligation that creates is one event: a member that terminates, or calls
`PMIx_Finalize`, without first calling `PMIx_Disconnect` owes the rest of
the assemblage a `PMIX_ERR_PROC_TERM_WO_SYNC`. A connect never followed by a
disconnect is therefore not an untidy loose end; it is the case the event
exists for.

Keeping that promise needs the membership, and where it is held is the whole
design:

- **The DVM master holds it, alone.** It is the one process that learns of
  every proc termination in the DVM — including the procs of a node whose
  daemon has died, which is exactly when an assemblage most wants telling,
  and exactly when a record kept on that daemon would have died with it.
- **Each participating daemon reports it**, on `PRTE_RML_TAG_CONNECTED`,
  from the *completion* of the connect collective (`connect_release`) rather
  than from its start — so a connect that failed is never recorded. Every
  such daemon reports, and the master takes the first and ignores the rest:
  there is no cheap election here that does not depend on one particular
  daemon still being alive, and the reports are a few dozen bytes on an
  operation that has just paid for a DVM-wide collective.
- **Termination is noticed in one place**: the `PRTE_PROC_STATE_TERMINATED`
  arm of `prte_state_base_track_procs()`, behind the `PRTE_PROC_FLAG_RECORDED`
  guard that makes it exactly once per proc. Every death reaches it, whether
  the proc exited, failed, or was force-marked terminated by the errmgr
  because its daemon is gone.
- **A member entry may be a wildcard rank**, and usually is — a connect
  between two jobs is written `{A/WILDCARD, B/WILDCARD}`. It stands for
  every proc of that namespace both when matching a departure and when
  addressing the event, where `PMIX_EVENT_CUSTOM_RANGE` carries the member
  array as-is and PMIx's own `PMIX_CHECK_PROCID` does the covering.
- **Matching a *set* is literal, though.** Two assemblages are the same one
  if they name the same participants, order disregarded; but `A/0` and
  `A/WILDCARD` are different participants, because PMIx will not pair a
  connect expressed one way with a connect expressed the other. A disconnect
  naming a set that was never recorded must fail to find it rather than drop
  somebody else's.
- **A record outlives one member's job.** The survivors are still connected
  to each other and still owed an event apiece, so a record is dropped only
  by a disconnect or when nothing named in it exists any more
  (`prte_pmix_server_connection_purge()`, from
  `prte_pmix_server_job_departed()`).

Two things follow from that record, and they are the rest of the definition:

- **A spawn connects the child to the parent process** (`connection_spawned()`,
  from `prte_plm_base_spawn_response()`), because the PMIx definition makes
  that the default for `PMIx_Spawn`. Which launches have a parent process at
  all is the delicate part: `jdata->originator` does *not* answer it, since
  `plm_base_receive` overwrites it with the daemon that relayed the request.
  `PRTE_JOB_LAUNCH_PROXY` survives from the requestor's own daemon and is the
  process that actually asked — screened against our own namespace (a
  prterun-style launch records the daemon) and against `PRTE_JOB_FLAG_TOOL`
  (a `prun` records its own tool procID, and a tool is not a member of a job).
  `PMIX_SPAWN_CHILD_SEP` opts out.

  **The same screen governs `PMIX_PARENT_ID`** (`register_nspace()`), and for
  the same reason: an app reads that key to ask "was I spawned by another
  application process?". Publishing the launch proxy unscreened answers *yes*
  to every ordinary `prun ./app`, so a program that branches on it — a spawned
  child doing one thing and its parent another, which is what the key is for —
  runs the wrong half of itself. That was live: the daemon screen was there
  and the tool screen was not, and the swarm's `connect:` cases (whose client
  detects its role exactly that way) all failed on it, every one of them with
  the parent calling itself the child.
- **A failure that terminates one member's job terminates the assemblage**
  (`connection_job_failed()`, from `_terminate_job()` in `errmgr/dvm` — the
  one place every failure-driven teardown goes through), each such job
  announced with `PMIX_ERR_JOB_TERM_WO_SYNC` and explained to the user with
  the `connected-term` help topic. `pmix_terminate_connected=0` turns it off.
  The assemblage is marked `terminating` as it goes, so the failures its own
  teardown produces do not drive it again.

**Dissolving is more generous than recording, deliberately.** Recording
compares memberships exactly; a disconnect dissolves any assemblage all of
whose members it *names*, wildcards covering ranks. That asymmetry is what
lets an application out of the assemblage a spawn created for it: the spawn
connects the child to the parent **process**, while an application that wants
out disconnects the two **jobs** — the shape `MPI_Comm_disconnect` has — and
under an exact-set rule that request would match nothing and the implicit
assemblage could never be left. It cannot dissolve anything by accident: a
request only covers a member it names, and a wildcard member is covered only
by a wildcard.

A proc in more than one assemblage — a spawned child that also connects
explicitly — produces **one** event, addressed to the union of the
memberships, not one per assemblage. It does, though, produce that event
**per proc**, and each one is a DVM-wide xcast: `notify_assemblage()`
broadcasts to every daemon and lets PMIx's custom range decide who hears
it. That is the PMIx definition — the promise is made to each member about
each departure — but it means an `MPI_Comm_spawn`ed job of N ranks pays N
broadcasts as it ends, since a child is connected to its parent by default
and running to completion is *not* a disconnect. The registry check comes
first, so a DVM with no assemblages pays nothing at all; see
[`docs/todo.rst`](../../../docs/todo.rst) for what a cheaper shape would
have to preserve.

Note what this depends on: PMIx used to execute a connect or disconnect whose
participants were **all local** without calling the host at all, which left
PRRTE unable to see a single-node assemblage — and, worse, unable to see the
disconnect that would dissolve one it had created itself at spawn. That is
fixed in the PMIx server library (openpmix: the host is called whenever it
offers the entry point; locality alone is no longer a reason to skip it).
Against a PMIx without that fix, a single-node spawn assemblage cannot be
dissolved.

---

## Session control (`pmix_server_session.c`)

`PMIx_Session_control` is the scheduler's API for creating, operating on and
reclaiming a `prte_session_t` — PRRTE's allocation object. The whole surface
is implemented here; the user-facing description of what each directive does
(and where PRRTE deliberately deviates) is
[`docs/how-things-work/schedulers/session_control.rst`](../../../docs/how-things-work/schedulers/session_control.rst).
The parts that matter when editing this file:

- **Who decides.** A request from the scheduler is a directive *to* this DVM
  and is executed here. One from anybody else is forwarded to the scheduler —
  **unless there is no scheduler**, in which case the DVM master is the
  authority and answers for itself, gated on `prte_session_is_owned_by`. That
  decision is made in **two** places, `pass_request()` here and
  `pmix_server_sched()` in `pmix_server.c` (the relay from a peer daemon), and
  they must agree: otherwise the same request is served or refused depending
  on which daemon the caller happened to attach to.
- **Parse first, act second.** `parse_directives()` walks the whole array and
  records; nothing acts during the walk. An info array has no defined order,
  and the previous single-pass version mis-handled an `INSTANTIATE` that
  arrived *before* its `SESSION_JOB` — the job was built but never attached to
  the session. Operations are alternatives, not a program: more than one is
  `PMIX_ERR_BAD_PARAM`.
- **An unrecognized directive is not an error.** A request may be addressed at
  more than one kind of RTE. Refuse only what PRRTE is being asked to do and
  cannot (`PROVISION_*`, anything asking it to *choose* machines).
- **Every value in the request is the caller's own, and this upcall is
  reachable by any application process or tool attached to any daemon.** It is
  the same untrusted-input rule as `_toolconn()` and `process_job_ctrl()` under
  "The relay pattern", in another place, and the strings are the fault version
  of it: `PMIX_SESSION_CTRL_ID`, `PMIX_ALLOC_ID`, `PMIX_ALLOC_REQ_ID`,
  `PMIX_ALLOC_TIME`, `PMIX_PERSONALITY` and `PMIX_NSPACE` all go on to a
  `strdup`, a `strlen` or a parser, so a mistyped one faulted the daemon on a
  path that needs no authorization at all (the time is read during the parse,
  before anyone has asked who is calling). `get_string_directive()` is the one
  place they are read; keep it that way. `PMIX_ALLOC_INHERITANCE` is the quiet
  half of the same rule — it decides whether the session's nodes are handed
  *away* to the scheduler, so it is read with `PMIx_Value_get_number()`,
  naming `PMIX_ALLOC_INHERIT`, rather than off a fixed union member. PMIx
  accepts that value under the attribute's own type and as a plain integer
  of any width that can hold it, so a host need not care which spelling the
  caller chose. And a `PMIX_NSPACE` must name something: an
  empty one is PMIx's wildcard, so it selects every job in the session instead
  of none.
- **`set_response()` may run only ONCE per parsed request.** It frees the info
  array the request arrived with, and every string the parsed `prte_sessctrl_t`
  holds points into that array — so a second call reads the copy the first
  released. `apply_to_all()` used to call it once per session and did exactly
  that; it now builds the answer once, after the loop, naming no session
  (a request addressed to all of them has no one session to report). For the
  same reason `set_response()` replaces the request's array even when it has
  nothing to say: leaving the old one there answers the caller with its own
  directives echoed back as results.
- **`answer_request()` owns the completion for both callers**, because they
  answer through different callbacks: a local client's answer goes to PMIx's
  own callback (synchronous), a relayed one to `send_alloc_resp` (which
  thread-shifts and packs later). A response array this file built is
  therefore **detached** from the request and handed over with its own release
  callback — tying it to the request instead would free it under the deferred
  half. The same detach is why `local_index` is invalidated after the slot is
  cleared: `_send_alloc_resp` clears it again from the other side, by which
  time the index may belong to somebody else.
- **A deferred answer returns `PMIX_OPERATION_IN_PROGRESS`.** Only an
  instantiation that launches apps does this — the answer carries the launched
  nspace, so it waits for the launch. Everything else answers inline.
- **The relayed path must be thread-shifted, and must arrive holding an extra
  reference.** `pmix_server_sched()` is an RML *receive* callback, and serving
  a session control drives the PLM and xcasts daemon commands — re-entering
  the RML from inside its own dispatch. Post an event (the allocation branch
  of that same handler already does). And `_send_alloc_resp` releases the
  request **twice** — explicitly, and again through its caddy's destructor —
  so the request has to be handed over with the `PMIX_RETAIN` the allocation
  branch takes. Without it the last release runs the destructor while the
  object lock is held and the master deadlocks against itself
  (`futex_wait_queue` in `pmix_obj_update`, from `ardes`).
- **A session created here is DETACHED by default**, unlike a reservation. A
  reservation dies with the tool that took it; a session is named and
  addressable and must outlive the request that created it, or its own jobs
  would be reclaimed the moment the requesting tool exited.
- **Reclaiming a session is `reclaim_session()`, and it is not a terminate.**
  It runs when the last job of an auto-completing or terminating session
  retires (`prte_pmix_server_session_job_terminated()`, called from
  `state_dvm.c`'s `check_complete`). Whether the nodes go back to the
  *scheduler* or back to the DVM's general pool is the session's
  `PMIX_ALLOC_INHERITANCE` disposition, and the default is the general pool:
  handing them away is destructive and must be asked for. Note that
  `prte_ras_base_teardown_reservation` never shrinks the master's own daemon
  out of the DVM — a single-node DVM whose whole allocation is one session
  would otherwise terminate itself when that session ended.
- **The completion report is accumulated, not gathered.** `PMIX_SESSION_COMPLETE`
  must carry every job's termination status, and the job objects do not survive
  to the end of the session — so each one is recorded on `session->results` as
  it retires.

---

## Servers we are a client OF, and which one is primary

A daemon is a PMIx server, but the master is also a PMIx **tool** of anything
it has to reach outside this DVM. There are two such things, and they are
reached the same way — `PMIx_tool_attach_to_server()`:

- a **scheduler**, in `prte_pmix_set_scheduler()` (`pmix_server_alloc*.c`).
  *Where* to look for it is not knowable here: the parameters that say so
  belong to `ras/pmix`, which may be a plugin, so nothing in `libprrte`
  may name its symbols. That component pushes them in at selection with
  `prte_pmix_set_scheduler_directives()` and they are held in
  `prte_pmix_server_globals` until the attach — which happens **once**,
  triggered by whichever of allocation, session control or tool connection
  needs a scheduler first, which is why they have to be recorded ahead of
  all three rather than supplied by the caller that happens to get there;
- an **external data server**, in `init_server()` (`pmix_server_pub.c`) — a
  data server living in another DVM, which the RML cannot address at all
  because it names a peer by rank in the sender's own namespace. See
  [`../../runtime/data_server/AGENTS.md`](../../runtime/data_server/AGENTS.md).

**PMIx sends a client-side call to whichever attached server is currently
primary, and only one may be primary at a time.** So *every* operation that
uses one of these connections must name its own server first:
`prte_pmix_set_primary_server()` is the single point that does it, it is a
no-op when the named server is already primary, and it must not be treated as
a once-at-startup step. This is race-free because the PMIx client calls pack
and send synchronously before returning, on the thread that just designated
the primary — so nothing can switch it out from under an in-flight request.
`PMIx_tool_set_server()` blocks until the PMIx progress thread completes it,
which is fine from the PRRTE progress thread and fatal from the PMIx one.

The flag this replaced (`scheduler_set_as_server`) recorded "the scheduler has
been made primary" once and never reconsidered — correct only for as long as
the scheduler was the sole connection.

**A failed attach to the external data server is not a state the DVM can
carry on from, and it looks exactly like one.** `init_server()` sets
`pubsub_init` before it does any work, so it runs once whatever happens, and
only the request that provoked it learns that it failed. What the *next*
request then does is the trap: the target for anything the master cannot
relay outward is the master itself, which is this daemon's own data server.
So after a failed attach the DVM went on publishing successfully into a store
no other DVM will ever look in, and answering lookups out of it — one error
message at the first request, and then a peer DVM blocked in
`MPI_Lookup_name` with nothing to connect the two. `server_init_rc` remembers
the outcome and every later request is refused with it. Anything else added
here that can fail once and be skipped forever needs the same treatment.

---

## A tool's departure

`_client_finalized()` is the **only** notice a daemon gets that a tool has
gone. A tool is not a child, so no waitpid fires for it, and the connection
it drops afterwards raises no `PMIX_ERR_LOST_CONNECTION` either — PMIx
suppresses that event for a peer it has already marked finalized. So the
handler retires the tool's job object itself
(`PRTE_ACTIVATE_PROC_STATE(..., PRTE_PROC_STATE_TERMINATED)` when the job
carries `PRTE_JOB_FLAG_TOOL`), which is what drives the tool's namespace
through the state machine and, with it, the inheritance disposition of any
allocation the tool reserved (see
[`../../mca/ras/AGENTS.md`](../../mca/ras/AGENTS.md)). PMIx delivers this
upcall for a tool only from `PMIX_CAP_TOOL_FINALIZED` onwards; before that
the tool's job object, and anything it held, simply accumulated for the
life of the DVM.

**A tool's rank is its own to choose, and the job object has to be built at
it.** `prte_pmix_server_register_tool()` files the tool's `prte_proc_t` in
`jdata->procs` at that rank, because that is how everything downstream finds
it — `prte_state_base_track_procs()` above all, which is what drives the
namespace through the state machine. Filed at zero instead, a tool that
self-assigned any other rank was simply never found there and its job never
retired, taking with it the allocation disposition this section is about.
The rank arrives from the connecting process, so screen it first: the
sentinel ranks are not subscripts, and `PMIX_RANK_UNDEF` handed to
`pmix_pointer_array_set_item()` asks the array to grow to four billion
entries. The same applies to everything else `_toolconn()` recorded off the
wire — an empty `PMIX_NSPACE` is one `prte_set_job_data_object()` refuses,
and a `PMIX_CMD_LINE` that never arrived leaves a NULL that
`PMIx_Argv_split()` turns into a NULL `argv`. `test_tool_registration` pins
the checks that can be reached without a live PMIx server.

`lost_connection_hdlr()` in `pmix_server.c` is the other half — the
*abnormal* departure — and it is registered at the end of
`pmix_server_init()`. Watch what precedes that registration: an early
`return` anywhere above it silently costs the daemon this handler and the
allocation-timeout relay. That happened, and was invisible from both ends,
because the blocking form of `PMIx_server_register_resources()` reports
success as `PMIX_OPERATION_SUCCEEDED` and `prte_pmix_convert_status()` maps
that onto `PRTE_SUCCESS`. **Any PMIx call in this file whose completion
callback is `NULL` can return `PMIX_OPERATION_SUCCEEDED`; test for both.**

---

## Locally-originated notifications

The top-level `AGENTS.md` has the full rule; the short version, because
it is enforced here:

- Any event **we** originate with `PMIx_Notify_event` must carry
  `PMIX_INFO_LOAD(&info[i], "prte.notify.donotloop", NULL, PMIX_BOOL)`.
- `pmix_server_notify_event()` checks for that marker **first** and
  returns `PMIX_OPERATION_SUCCEEDED` synchronously without
  thread-shifting. Without it, a *blocking* `PMIx_Notify_event` issued
  from the progress thread deadlocks against its own deferred upcall.
- It also breaks the xcast echo: `pmix_server_notify()` (the RML receive)
  broadcasts to every daemon including the originator, and the originator
  must update its group registry but must not re-notify its own clients.

**An event's info array is the generating client's own, and it reaches every
daemon in the DVM.** PMIx hands what a process passed to `PMIx_Notify_event`
straight to the host without inspecting what any entry holds — the same
untrusted-input rule as `_toolconn()` and `process_job_ctrl()` under "The
relay pattern", in a third place, and the worst-placed of the three because
the host's answer to the event is to broadcast it. So anything in here that
*reads* an entry must check `value.type` first:
`prte_pmix_server_group_member_left()` took the `PMIX_GROUP_ID` off
`value.data.string` and the departing proc off `value.data.proc`, so one
`PMIx_Notify_event(PMIX_GROUP_LEFT, ...)` carrying either key with, say, a
`PMIX_SIZE` value faulted every daemon at once, on the `strcmp` or on the
`PMIX_CHECK_PROCID`. Any process attached to any daemon could send it, and
the registry is non-empty on every daemon from the first successful
`PMIx_Group_construct` onwards, because `grpcomm_group.c` appends the group
everywhere.

The identity has to be concrete as well as well-typed. `PMIX_CHECK_PROCID`
counts an empty nspace and a wildcard rank as matching anything, so a
departure naming neither removed whichever member happened to sit first in
the array — the wildcard trap that runs through this whole directory, here
in a place where the wildcard arrived from outside rather than from an
uninitialized field. A proc leaves a group one at a time; insist on being
told which one.

The helper is not static only so that `test_group_left` can pin all of that
without a DVM.

**The xcast's failure is a PRRTE code and the caller reads PMIx ones.**
`_notify_event()` used to flatten every `prte_grpcomm_xcast()` failure to a
bare `PMIX_ERROR` before handing it to the client's completion callback,
which is the convert-direction mistake described under "Conventions" wearing
its other face — not converting at all.

**The two event-registration hooks are deliberately empty**, and
[`docs/todo.rst`](../../../docs/todo.rst) carries the reasoning: a
notification is broadcast to every daemon and filtered against each local
PMIx server's own registrations there, so the host has nothing to record.
Acting on them would trade that broadcast for a DVM-wide replicated set of
codes that has to survive grow, shrink and daemon loss.

---

## Conventions specific to this directory

- **PMIx status vs. PRRTE status.** Functions reachable from an upcall
  must return `pmix_status_t`; functions in the PRRTE half return `int`
  PRRTE codes. They are *different numbering schemes* that happen to
  agree on 0 — and, worse, that **overlap** everywhere else, so a code
  used in the wrong space is not obviously foreign, it is some other
  real code. Convert with `prte_pmix_convert_rc()` /
  `prte_pmix_convert_status()` at the boundary, and log with
  `PMIX_ERROR_LOG` or `PRTE_ERROR_LOG` to match. Mixing them silently
  reports the wrong error to the application. See
  [`../../pmix/AGENTS.md`](../../pmix/AGENTS.md) for the table of what
  collides with what.
  - **Converting the wrong *direction* is the easy version of this
    mistake**, because it type-checks and the success case still works
    (both spaces call success 0). `pmix_server_queries.c` ran the result
    of `PRTE_MODEX_RECV_VALUE_OPTIONAL` — which yields a **PMIx**
    status — through `prte_pmix_convert_rc()`, the PRRTE→PMIx direction,
    so every failure of the `PMIX_SERVER_URI` query reached the tool as a
    bare `PMIX_ERROR`. Before converting, ask which space the value is
    already in. An unpack status is the commonest case of a value
    that is *already* a PMIx status — `pmix_server_alloc_request_resp()`
    converted three of them — and `send_error()` in `pmix_server.c` is the
    commonest case of the opposite, a function whose parameter is a PRRTE
    code and which converts on the way out.
  - **A function that returns `pmix_status_t` must return one on every
    path.** `prte_server_send_request()` answers with the packers' PMIx
    statuses and, on a send failure, answered with the RML's PRRTE code —
    and each of its three callers hands what it returns straight to a
    client's completion callback. Keep the two in separate variables, as
    that function now does, so the compiler shows which is which.
- **A helper must not answer for its caller.** `process_directive()` in
  `pmix_server_session.c` used to invoke `req->infocbfunc` itself on an
  error and then return to a caller that also invokes it — a double
  callback and a double release. If the caller has a `callback:` tail,
  the helper returns a status and nothing else.
- **Widths on the wire.** Pack what the receiver unpacks. Handing PMIx
  the address of a `size_t` with `PMIX_INT32` reads the wrong four bytes
  on a big-endian machine.
- **Verbose output is free.** Everything here is behind
  `pmix_output_verbose(N, prte_pmix_server_globals.output, ...)`; add
  traces liberally at level 2.
- Standard PRRTE rules apply: `prte_config.h` first, braces everywhere,
  constant-on-the-left comparisons, `PMIX_NEW`/`PMIX_RELEASE` for class
  objects, no new compiler warnings.

---

## Testing

**Unit — `test/unit/prted/`.** The directive translators
(`prte_pmix_xfer_job_info`, `prte_pmix_xfer_app`), the job-info cache, the
session time-limit parser (`prte_pmix_server_parse_session_time`), the
group-departure validator (`test_group_left`), the query qualifier
validation and status decision (`test_query_qualifiers`, driven through the
real `pmix_server_query_fn` upcall with the test thread turning the event
base), the departed-jobs list, and the connected-assemblage registry
(`test_connections` — set matching, wildcard coverage, and when a record is
purged) are pure data transforms and are covered there. Most of the rest of
this directory needs a live PMIx server and at least one peer daemon.

**Live smoke test.** `prte --daemonize && prun -n 4 hostname && pterm`
exercises register_nspace, register_client, fence, and the spawn path on
one node.

**Multi-node — `contrib/dockerswarm/`, `test_prted()`.** The things that
only exist with more than one daemon:

- a **wildcard direct modex** — `PMIx_Get` of job-level data on a daemon
  that hosts none of that job's procs, which is the path the `tproc`/
  `target` bug hung;
- a **tool connecting through a non-master daemon**, which exercises the
  `TCONN` relay rather than the HNP's local shortcut;
- **job-scoped signal delivery** in a DVM running more than one job;
- **session control** (`test_session`) — a reservation actually withholding
  its nodes from a general job, a request relayed from a non-master daemon,
  and a session signal reaching the jobs on every node they occupy. It is
  driven by `examples/sessionctrl.c`, which `build.sh` installs;
- **connect/disconnect** (`test_connect`, driven by
  `contrib/dockerswarm/connector.c`) — a spawned child connected to its
  parent on another node, leaving with and without disconnecting first, and
  failing with and without having disconnected first. The single-node run of
  the same client covers the same ground once the PMIx fix above is in place;
  what only the swarm shows is an assemblage that genuinely spans daemons.

---

## Debugging

```sh
prte --prtemca pmix_server_verbose 5 ...   # every upcall, relay and reply
prte --prtemca grpcomm_base_verbose 5 ...  # what fence/group hand off to
prte --prtemca state_base_verbose 5 ...    # the states these upcalls activate
```

If a client is hung in a PMIx call, the first question is always "which
request array is it sitting in, and what was supposed to take it out?" —
verbosity 2 prints the local/remote index of every request as it is
created and retired.
