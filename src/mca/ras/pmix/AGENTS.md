# AGENTS.md — `ras/pmix` (PMIx scheduler allocator)

Component guide for `src/mca/ras/pmix/`. Read the
[framework guide](../AGENTS.md) first for the module contract and the
`modify()` protocol.

---

## Role and priority

`ras/pmix` connects the DVM to a **host PMIx server acting as a system
scheduler** and forwards allocation requests to it. It answers the query
at priority **20**, but only when it has actually been pointed at a
scheduler — a `uri`, an nspace, a `server_pid`, a `server_host`, an
explicit `connection_order`, or `system_scheduler`. With none of those
set it declines, which matters because selection keeps exactly one
module: answering on spec would shadow `ras/hosts` (priority 1) in every
unmanaged environment and nothing would read a hostfile.

Its `allocate` does nothing — this component exists for **runtime
allocation requests** (`modify`) that a scheduler must satisfy. Note the
consequence of that under single-owner selection: pointing a DVM at a
PMIx scheduler makes this the allocator, and it discovers no initial
allocation at all, so the base falls through to the one-slot local-node
fabrication (or to `ras-base:no-allocation` under
`prte_allocation_required`). That is recorded in
[`docs/todo.rst`](../../../../docs/todo.rst) along with the rest of what
this component does not yet do.

Files:

| File | Contents |
|------|----------|
| `ras_pmix_component.c` | Registration; `open`/`register`; `query` returns the module at priority 20; scheduler-connection MCA params. |
| `ras_pmix.c` | `init` (publish the scheduler's whereabouts), `allocate` (no-op), `modify`, `finalize`, and the async passthrough callbacks. |
| `ras_pmix.h` | `prte_ras_pmix_component_t` (server procid, uri, connection order, retries, …). |

---

## How it works

- **`allocate()`** always returns `PRTE_ERR_TAKE_NEXT_OPTION` — it never
  contributes nodes to initial discovery.
- **`modify()`** first calls `prte_pmix_set_scheduler()` to attach to a
  scheduler; if none is reachable it answers `PMIX_ERR_UNREACH`. When a
  scheduler *is* attached, it appends the requester's id
  (`PMIX_REQUESTOR`) to the info array, builds the answer's caddy, and
  forwards the request with `PMIx_Allocation_request_nb`. The async
  answer arrives in `infocbfunc`, which thread-shifts to `passthru`
  (touching the global request array must happen on the progress
  thread), merges the answer into the request, completes it via
  `prte_ras_base_complete_request`, and relays the result to the original
  caller.

- **`init()`** is where the MCA parameters become real. Every
  `ras_pmix_*` connection parameter is the name of a PMIx attach
  attribute:

  | parameter | attribute |
  |-----------|-----------|
  | `uri` | `PMIX_SERVER_URI` |
  | `nspace` | `PMIX_SERVER_NSPACE` |
  | `server_pid` | `PMIX_SERVER_PIDINFO` |
  | `server_host` | `PMIX_SERVER_HOSTNAME` (declared; no PTL reads it yet, so it is forwarded rather than acted on) |
  | `connection_order` | `PMIX_CONNECTION_ORDER` |
  | `max_retries` | `PMIX_CONNECT_MAX_RETRIES` |
  | `retry_delay` | `PMIX_CONNECT_RETRY_DELAY` |
  | `system_scheduler` | `PMIX_CONNECT_TO_SCHEDULER` (always sent) |

  `init()` builds that array and hands it to
  `prte_pmix_set_scheduler_directives()`, which parks it in
  `prte_pmix_server_globals` for `prte_pmix_set_scheduler()` to use when
  it attaches. A parameter left at its default contributes nothing, so a
  DVM that has been told nothing gets the same bare rendezvous scan the
  attach has always done.

  **`rank` is the one with no attribute**, and deliberately sends
  nothing: `PMIX_SERVER_RANK` is a `PMIx_server_init` attribute — the
  rank a server takes for *itself* — and PMIx identifies an attach target
  by nspace and rendezvous, not by rank. Sent here it would select
  nothing and be forwarded to the server as an assertion PRRTE has no
  business making. See [`docs/todo.rst`](../../../../docs/todo.rst).

---

## Things to watch when editing

- **There is no "schedulerless fallback" any more, and the old
  `PMIX_ERR_TAKE_NEXT_OPTION` return was its last trace.** It existed so
  that `ras/hosts`, further down the module list, could serve a grow or
  shrink locally in a DVM with no scheduler. Selection now keeps exactly
  one module and this one declines the query unless it has been pointed
  at a scheduler, so a schedulerless DVM never reaches `modify()` here at
  all — `ras/hosts` is asked directly — and where this component *is*
  selected there is nothing to defer to. All the old return bought was
  leaving `req->pstatus` holding the `PMIX_ERR_NOT_SUPPORTED` that
  `prte_ras_base_modify` seeds, which told the requester to give up when
  the truth was "the scheduler is out of touch, try again".
- **`infocbfunc` must stay trivial, and must not allocate.** It runs on
  the *PMIx* progress thread, so per the top-level
  [`AGENTS.md`](../../../../AGENTS.md) golden rule it does nothing but
  fill a `prte_ras_pmix_caddy_t` and `PRTE_PMIX_THREADSHIFT` it; every
  mutation of the `prte_pmix_server_req_t` happens in `passthru`, on the
  PRRTE progress thread. Do not "simplify" by writing the answer straight
  onto the request — that is a cross-thread write to state the PRRTE
  progress thread owns. The caddy is built by `modify()` *before* the
  request goes out for the same reason: there is nowhere to report a
  failure from inside that callback, so an allocation that came up short
  there left the request in `local_reqs` and the client waiting on a
  callback that would never arrive.
- **The caddy holds a reference on the request** (taken by `modify()`,
  released by the caddy's destructor) so the request cannot be reclaimed
  underneath the shift. `PMIx_Allocation_request_nb` answers anything
  other than `PMIX_SUCCESS` without ever calling back, so that is the one
  return on which `modify()` must release the caddy itself.
- **The scheduler's answer is merged into the request, not substituted
  for it.** `prte_ras_base_complete_request` routes the grow or the
  teardown using directives that belong to the *original* request:
  `PMIX_ALLOC_ID`/`PMIX_ALLOC_REQ_ID` name which reservation an EXTEND or
  a RELEASE is for, and `PMIX_ALLOC_TARGET`/`PMIX_ALLOC_SHARE`/
  `PMIX_ALLOC_INHERITANCE` decide which session a `PMIX_ALLOC_NEW`'s
  nodes join and what becomes of them afterwards. A scheduler has no
  reason to echo any of those — they are PRRTE-local routing, not
  anything it was asked to decide — so replacing `req->info` outright
  threw them away and then failed the request *locally* after the
  scheduler had already committed the change: an EXTEND with nothing left
  to name its target is refused `PMIX_ERR_BAD_PARAM`, and a RELEASE by
  allocation id falls through to a node list that is no longer there and
  is refused `PMIX_ERR_NOT_FOUND`. `merge_answer()` keeps every request
  key the answer does not mention, lets the answer win where they
  collide, and drops the `PMIX_REQUESTOR` this component added.
- **`passthru` records the answer in `pstatus`, not just `status`.**
  `pstatus` is what drives `prte_ras_base_complete_request` and what the
  requester is told; `prte_ras_base_modify` seeds it with
  `PMIX_ERR_NOT_SUPPORTED`, so writing only `status` meant a granted
  allocation was never applied and the requester was told the operation
  was unsupported.
- **`passthru` hands the requester `prte_pmix_server_req_release` and
  returns without releasing the request**, mirroring
  `prte_ras_base_modify`'s tail. The info the requester is looking at is
  owned by the request (or by PMIx, released through it), so dropping
  the request there would pull it out from under a callback that has not
  finished with it.
- **Anything that repoints `req->info` must free a previously *owned*
  array first.** Both `modify()` here and the base's
  `ras_base_set_alloc_response` do. A request relayed from a remote peer
  arrives with `copy == true` (see `pmix_server.c`), so this is a live
  path, not a theoretical one.

- **Nothing here ever gives anything back.** The module declares
  `scheduler_owned = true` but implements neither `release_allocation`
  nor `shrink_complete`, the two hooks the framework offers for handing
  nodes back to the RM, and a request the scheduler grants but PRRTE then
  fails to apply locally is not compensated either. See
  [`docs/todo.rst`](../../../../docs/todo.rst).

- **The parameters have to be *pushed*, because the boundary only crosses
  one way.** The knowledge of where the scheduler is belongs to this
  component, which may be a run-time loadable plugin — so nothing in
  `libprrte` may name `prte_mca_ras_pmix_component` (a unit test tried,
  and failed the `--enable-mca-dso` link). The attach belongs to
  `prte_pmix_set_scheduler()`, is made **once** for the whole daemon, and
  is triggered by whichever of allocation, session control or tool
  connection wants a scheduler first. So `init()` — which runs at
  selection, ahead of all of them — pushes, rather than the shim pulling.
  Doing it any later would let one of those consumers burn the one look
  with nothing to aim it. The other half of that ordering is that
  `pmix_server_init()` — which zeroes the pair of globals the directives
  live in — runs at `ess_hnp_module.c:249`, well ahead of
  `prte_ras_base_select()` at :338. Reverse them and the store is wiped
  the moment it is made.
- **An MCA variable writes an `int` through whatever storage it is
  given**, so `rank`, `server_pid` and the retry pair are registered
  against file-static `int` proxies and copied into their real types
  afterwards, the way `nspace` already was. Handing the base a
  `pmix_rank_t *` or a `pid_t *` had it write four bytes of `int` through
  a pointer of another type.
- **An over-long `ras_pmix_nspace` is refused, not truncated.**
  `PMIx_Load_nspace` silently cuts at `PMIX_MAX_NSLEN`, and a truncated
  name is a different scheduler — most likely one that does not exist.
