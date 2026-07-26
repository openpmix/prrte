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
- The async result must be processed on the progress thread — don't
  short-circuit the `infocbfunc` → `passthru` thread-shift.
- **KNOWN GAP: `infocbfunc` does real work before it thread-shifts.**
  It runs on the *PMIx* progress thread, and the top-level
  [`AGENTS.md`](../../../../AGENTS.md) golden rule says such a callback
  must do nothing beyond capturing its arguments and posting the event.
  Today it writes `req->status`, calls `PMIX_INFO_FREE` on `req->info`,
  and rewrites `req->info`/`ninfo`/`rlcbfunc`/`rlcbdata` — all PRRTE
  object state, mutated off the PRRTE progress thread. It has not been
  observed to bite (nothing else touches the `req` between the forward
  and the answer), but the correct shape is a caddy that carries
  `status`/`info`/`ninfo`/`rel`/`relcbdata` across the shift, with every
  mutation of `req` moved into `passthru`. Fix this before adding any
  other writer of the request array.
- `modify()` sets `req->copy = true` after pointing `req->info` at its
  own `xfer` array. If a caller ever hands in a request that *already*
  owns its info (`copy == true` on entry — `prte_ras_base_add_hosts`
  builds one), the previous array leaks. It is unreachable today only
  because that request carries `req->key = "hosts"`, which the base's
  component filter uses to skip this module.
</content>
