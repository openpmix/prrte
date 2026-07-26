# AGENTS.md — `ras/pmix` (PMIx scheduler allocator)

Component guide for `src/mca/ras/pmix/`. Read the
[framework guide](../AGENTS.md) first for the module contract and the
`modify()` protocol.

---

## Role and priority

`ras/pmix` connects the DVM to a **host PMIx server acting as a system
scheduler** and forwards allocation requests to it. It is always
available (query priority **20**), on the assumption that the system may
include a PMIx-capable scheduler. Its `allocate` does nothing —
discovery of the *initial* allocation is handled by an RM component or
`hosts`; this component exists for **runtime allocation requests**
(`modify`) that a scheduler must satisfy.

Files:

| File | Contents |
|------|----------|
| `ras_pmix_component.c` | Registration; `open`/`register`; `query` returns the module at priority 20; scheduler-connection MCA params. |
| `ras_pmix.c` | `allocate` (no-op), `modify`, `finalize`, and the async passthrough callbacks. |
| `ras_pmix.h` | `prte_ras_pmix_component_t` (server procid, uri, connection order, retries, …). |

---

## How it works

- **`allocate()`** always returns `PRTE_ERR_TAKE_NEXT_OPTION` — it never
  contributes nodes to initial discovery.
- **`modify()`** first calls `prte_pmix_set_scheduler()` to attach to a
  scheduler. **If none is reachable it returns
  `PMIX_ERR_TAKE_NEXT_OPTION`** — crucial, because in a schedulerless
  DVM the `ras/hosts` component handles grow/shrink locally; returning a
  hard error (e.g. `PMIX_ERR_UNREACH`) here would abort the modify loop
  before `hosts` is consulted. When a scheduler *is* attached, it appends
  the requester's id (`PMIX_REQUESTOR`) to the info array and forwards
  the request with `PMIx_Allocation_request_nb`. The async answer arrives
  in `infocbfunc`, which thread-shifts to `passthru` (touching the global
  request array must happen on the progress thread), completes the
  request via `prte_ras_base_complete_request`, and relays the result to
  the original caller.

MCA params (`ras_pmix_*`) configure the scheduler connection: `uri`,
`nspace`, `rank`, `system_scheduler`, `connection_order`, `server_pid`,
`server_host`, `max_retries`, `retry_delay`.

---

## Things to watch when editing

- **Keep the schedulerless fallback intact.** The
  `PMIX_ERR_TAKE_NEXT_OPTION`-on-no-scheduler behavior is what lets the
  no-scheduler elastic-DVM path (handled by `ras/hosts`) work. See repo
  memory on `PMIX_ERR_UNREACH` regressions.
- **`infocbfunc` must stay trivial.** It runs on the *PMIx* progress
  thread, so per the top-level [`AGENTS.md`](../../../../AGENTS.md)
  golden rule it does nothing but fill a `prte_ras_pmix_caddy_t` and
  `PRTE_PMIX_THREADSHIFT` it; every mutation of the
  `prte_pmix_server_req_t` happens in `passthru`, on the PRRTE progress
  thread. Do not "simplify" by writing the answer straight onto the
  request — that is a cross-thread write to state the PRRTE progress
  thread owns.
- **The caddy holds a reference on the request** (released by its
  destructor) so the request cannot be reclaimed underneath the shift.
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
</content>
