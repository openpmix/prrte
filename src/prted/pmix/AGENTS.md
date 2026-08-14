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
| `pmix_server_pub.c` | `publish`/`lookup`/`unpublish` (relayed to the data server). |
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

The daemon-command side follows the same discipline: `prted_comm.c`'s
`HALT_VM`/`SHRINK`/`DVM_CLEANUP_JOB` cases issue their PMIx call and
return, and pick up in `_daemon_continue()` once PMIx is done. Nothing on
a running daemon's command path blocks its own event loop waiting on
PMIx.

The two `PRTE_PMIX_WAIT_THREAD` calls left in this directory are both in
`pmix_server_init()`, which runs inside `prte_init()` — before the event
loop exists. There is no loop to park there, so blocking is correct. Any
*new* one outside that function is almost certainly a bug.

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

**A collecting relay must complete on *every* path.** `monitor`
fans out to all daemons and counts responses (`ndaemons` vs `nreported`).
It increments `nreported` as soon as a response arrives, so a response
that then fails to unpack must still fall through to the "have all
daemons reported?" check. Returning early there means that if the *last*
daemon's response is malformed, the request never completes and the
client hangs forever.

---

## What a daemon publishes, and what it derives

`prte_pmix_server_register_nspace()` used to hand its local PMIx server a
`PMIX_PROC_INFO_ARRAY` for **every** proc of a job — rank, global rank,
app rank, appnum, local and node rank, node id, hostname, cpuset and the
locality string generated from it. Every daemon, for the whole job. That
is a table proportional to the job on a node that will only ever run its
own slice of it, and `prte_hostname_cutoff` — which drops one field of it
above a thousand nodes — is the record of that wall having been met once
already.

With `prte_pmix_lazy_procdata` (the default where PMIx allows it) a daemon
publishes only the procs it hosts. Everything it withheld is still in its
own job object, so when PMIx asks for one of those procs through the
`direct_modex` upcall, `dmodex_req()` answers out of `jdata->procs[rank]`
instead of going to the hosting daemon. Four things about that are load
bearing:

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

## Info-array expansion

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
    already in.
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
(`prte_pmix_xfer_job_info`, `prte_pmix_xfer_app`), the job-info cache, and
the session time-limit parser (`prte_pmix_server_parse_session_time`) are
pure data transforms and are covered there. Most of the rest of this
directory needs a live PMIx server and at least one peer daemon.

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
  driven by `examples/sessionctrl.c`, which `build.sh` installs.

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
