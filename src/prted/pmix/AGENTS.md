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
| `pmix_server_gen.c` | `abort`, `client_finalized`, `client_connected2`, `tool_connected`, `iof_pull`, `push_stdin`, `log`. |
| `pmix_server_fence.c` | `fence_nb` and `direct_modex`. |
| `pmix_server_pub.c` | `publish`/`lookup`/`unpublish` (relayed to the data server). |
| `pmix_server_notify.c` | `notify_event` (up), and the RML receive that fans a peer's event out to local clients (down). |
| `pmix_server_group.c` | `group` — a thin pass-through to grpcomm. |
| `pmix_server_job_ctrl.c` | `job_control` — kill/terminate/signal/define-pset, as daemon commands. |
| `pmix_server_monitor.c` | `monitor` — heartbeat/file monitoring, fanned out to all daemons and collected. |
| `pmix_server_alloc*.c`, `pmix_server_session.c` | `allocate` and `session_control`, relayed to the DVM master and on to a scheduler. |

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
  agree on 0. Convert with `prte_pmix_convert_rc()` /
  `prte_pmix_convert_status()` at the boundary, and log with
  `PMIX_ERROR_LOG` or `PRTE_ERROR_LOG` to match. Mixing them silently
  reports the wrong error to the application.
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
(`prte_pmix_xfer_job_info`, `prte_pmix_xfer_app`) and the job-info cache
are pure data transforms and are covered there. Most of the rest of this
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
- **job-scoped signal delivery** in a DVM running more than one job.

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
