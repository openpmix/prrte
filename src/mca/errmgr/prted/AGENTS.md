# AGENTS.md — `errmgr/prted` (the daemon-side reporter)

Component guide for `src/mca/errmgr/prted/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine callback model, and the progress-thread threading notes
referenced throughout.

---

## Role and priority

`prted` is the errmgr component that runs on **every prted daemon** (not
the HNP). Priority **1000** (MCA param `errmgr_prted_priority`), selected
by role: `errmgr_prted_component_query()` returns the module only when
`PRTE_PROC_IS_DAEMON`, otherwise `*module = NULL; *priority = -1;
return PRTE_ERROR`. Its job is the mirror image of the `dvm` component:
where `dvm` *decides* policy, `prted` *reports upward* — it detects local
process and daemon state changes, kills local children when required,
notifies the HNP over the RML, and exits cleanly when ordered.

Files:

| File | Contents |
|------|----------|
| `errmgr_prted_component.c` | Registration, `priority` MCA param (default 1000), `query` gated on `PRTE_PROC_IS_DAEMON`, open/close no-ops. |
| `errmgr_prted.c` | The module: `init`/`finalize`, handlers `job_errors`/`proc_errors`, the `prted_abort` distress path, and packing/killing helpers. |
| `errmgr_prted.h` | Exports `prte_mca_errmgr_prted_component` and `prte_errmgr_prted_module`. |

---

## How it wires into the state machine (`init`)

Same three registrations as `dvm`, but the handlers do daemon-local work:

```c
prte_state.add_job_state(PRTE_JOB_STATE_ERROR,          job_errors);
prte_state.add_proc_state(PRTE_PROC_STATE_COMM_FAILED,  proc_errors);
prte_state.add_proc_state(PRTE_PROC_STATE_ERROR,        proc_errors);
```

`finalize()` is a no-op.

---

## The HNP report protocol

Most of this component's work is packing a state update and sending it to
the HNP. The wire format built by the packing helpers is, in order:

1. `PRTE_PLM_UPDATE_PROC_STATE` command (`PMIX_UINT8`).
2. the job nspace (`PMIX_PROC_NSPACE`).
3. for each relevant proc: **rank, pid, state, exit_code**
   (`pack_state_for_proc`).
4. a terminator rank `PMIX_RANK_INVALID` marking the end / job complete.

The buffer is delivered with `PRTE_RML_RELIABLE_SEND(rc,
PRTE_PROC_MY_HNP->rank, alert, PRTE_RML_TAG_PLM)`. `pack_state_update`
packs *all* local children of a job (steps 2–4); the single-proc paths
pack the nspace then one `pack_state_for_proc` then the terminator.

A per-job **dedup flag**, `PRTE_JOB_FAIL_NOTIFIED`, ensures the daemon
reports a given job's failure to the HNP only once — except for
`PRTE_JOB_RECOVERABLE` jobs, which must receive *every* notification, so
the flag is deliberately not set for them.

---

## `job_errors` — daemon's view of job-level failures

After the standard acquire / `finalizing` / daemon-job back-fill and
`jdata->state = jobstate`, it switches on `jobstate`:

- `FAILED_TO_START` → `failed_start(jdata)` (see helpers), then fall
  through to send a state update to the HNP.
- `COMM_FAILED` → `killprocs(NULL, PMIX_RANK_WILDCARD)` (kill all local
  procs) then `prted_abort(...)` to order our own termination; `goto
  cleanup` (no HNP report — comms are gone).
- `HEARTBEAT_FAILED` → let the HNP handle it; `goto cleanup`.
- default → fall through.

The fall-through path packs `PRTE_PLM_UPDATE_PROC_STATE` +
`pack_state_update(alert, jdata)` and reliable-sends it to the HNP.

---

## `proc_errors` — daemon's view of process/daemon failures

The core reporter. After acquire and the `finalizing` guard it filters in
order:

1. **`HEARTBEAT_FAILED`** → ignore, the HNP owns it.
2. **Lifeline / unreachable family** (`LIFELINE_LOST`,
   `UNABLE_TO_SEND_MSG`, `NO_PATH_TO_TARGET`, `PEER_UNKNOWN`,
   `FAILED_TO_CONNECT`) → we've lost our lifeline to the HNP: set exit
   status, `killprocs` all children, and `prte_quit()`. Our routed
   children will see us leave and die on their own.
3. Look up `jdata`; if `NULL`, the job is already complete — ignore.
4. **`COMM_FAILED`:**
   - to self → ignore.
   - to a **non-daemon** (an application proc) → we can't trust we'll
     catch its waitpid, so re-inject it as a waitpid event: build a
     `prte_wait_tracker_t`, `PMIX_RETAIN` the child, and activate
     `prte_odls_base_default_wait_local_proc` on the event base. This
     reuses the normal local-termination path instead of duplicating it.
   - to a **daemon** → if `prte_prteds_term_ordered`, check whether any
     local child is still alive and whether routed children remain
     (`prte_rml_base.n_children`); when all are gone activate
     `DAEMONS_TERMINATED` to exit; otherwise just continue.
5. Look up the `child` proc_t; a `NULL` is a `PRTE_ERR_NOT_FOUND` and
   forces `PRTE_JOB_STATE_FORCED_EXIT`.

Then, per specific application-proc state:

- **`CALLED_ABORT`** → record `child->state`; unless
  `PRTE_JOB_FAIL_NOTIFIED`, pack just this proc and reliable-send to the
  HNP, then set the dedup flag (skipped for recoverable jobs).
- **not local** → ignore (only the owning daemon reports a proc).
- **`TERM_NON_ZERO`** → same report-once-to-HNP dance; afterward, if the
  proc is fully done (`IOF_COMPLETE && WAITPID && !RECORDED`), activate
  `PRTE_PROC_STATE_TERMINATED`.
- **`FAILED_TO_START` / `FAILED_TO_LAUNCH`** → set state, bump
  `jdata->num_terminated`, and **defer to the state machine**: only when
  `num_local_procs == num_terminated` (all local procs have attempted
  start) activate the corresponding job state so the HNP gets one
  consolidated report.
- **`state > TERMINATED`** (abnormal) → if `prte_prteds_term_ordered`,
  update the child's ALIVE/RECORDED bookkeeping and terminate the daemon
  when all children/routes are gone (no HNP alert — we're already
  leaving). Otherwise (`keep_going:`) report the abnormal termination to
  the HNP once, set `PRTE_PROC_FLAG_TERM_REPORTED` and the
  `PRTE_JOB_FAIL_NOTIFIED` dedup flag, and activate `TERMINATED` if the
  proc is fully done.
- **plain `TERMINATED`** (the final `else`) → if no live children of the
  job remain (`any_live_children`), pack a full job state update, **remove
  this job's children from `prte_local_children` and `PMIX_RELEASE` the
  jdata locally** (the job is complete on this node), then reliable-send
  the update to the HNP.

Note the `WAITPID`/`IOF_COMPLETE`/`RECORDED` flag triad gates the final
`PRTE_PROC_STATE_TERMINATED` activation — a proc is only "done" once both
its waitpid has fired and its I/O forwarding has drained, and it must not
be recorded twice.

---

## `prted_abort` — the FORCED_TERMINATE distress path

Called when an internal failure (e.g. a pack/unpack error) means the
daemon must die but a bare `exit()` would give the user no message. It:

1. Runs at most once (`prte_abnormal_term_ordered` guard), then sets that
   flag.
2. Emits the message via `help-errmgr-base.txt: simple-message`.
3. Packs a `PRTE_PLM_UPDATE_PROC_STATE` alert describing *itself*
   (nspace, its own vpid, its pid, `PRTE_PROC_STATE_CALLED_ABORT`, the
   error code, terminator rank) and reliable-sends it to the HNP on
   `PRTE_RML_TAG_PLM`, giving mpirun the chance to order termination.
4. Sets a **5-second self-destruct timer** (`wakeup` → `prte_quit`) so
   that if the messaging system itself is broken and the HNP never
   replies, the daemon still exits. If the reliable send fails outright
   it calls `prte_quit()` immediately.

---

## Local helpers

| Helper | Role |
|--------|------|
| `any_live_children(job)` | True if any child in `prte_local_children` for `job` (or any job, if nspace invalid) still has `PRTE_PROC_FLAG_ALIVE`. |
| `pack_state_for_proc(alert, child)` | Pack one proc's `{rank, pid, state, exit_code}`. |
| `pack_state_update(alert, jobdat)` | Pack nspace + every local child of `jobdat` + `PMIX_RANK_INVALID` terminator. |
| `failed_start(jobdat)` | Set the job `FAILED_TO_START`; for each local child in that state, force `IOF_COMPLETE`+`WAITPID` flags (so we don't hang on pipes that never opened) and activate `PRTE_PROC_STATE_TERMINATED`. |
| `killprocs(job, vpid)` | Kill local procs via `prte_odls.kill_local_procs` — all of them when `job` is invalid and `vpid == PMIX_RANK_WILDCARD`, else the single `{job, vpid}`. |

---

## Things to watch when editing

- **This component reports; it does not decide.** Keep DVM-wide policy
  (terminate the DVM, notify submitters) out of here — that belongs to
  `dvm`. The daemon's contract is: tell the HNP, manage local children,
  and exit only when ordered or when its lifeline is lost.
- **`PRTE_JOB_FAIL_NOTIFIED` dedup, with the recoverable exception.**
  Every "alert the HNP" branch checks the flag and sets it afterward —
  *except* for `PRTE_JOB_RECOVERABLE` jobs, which must keep receiving
  notifications so the set is deliberately skipped. Preserve both halves.
- **The `WAITPID`/`IOF_COMPLETE`/`RECORDED` flag triad** is what
  prevents premature or double `TERMINATED` activation and double-counted
  `num_terminated`. Don't collapse or reorder these checks.
- **`COMM_FAILED` to a non-daemon is re-routed as a waitpid**, not
  handled inline, to avoid duplicating the odls termination path — keep
  the `prte_wait_tracker_t` + `prte_odls_base_default_wait_local_proc`
  hand-off intact (including the `PMIX_RETAIN` guarding the race).
- **`prted_abort` must stay idempotent and must always arm the timer.**
  The 5-second `wakeup` timer is the only guarantee of exit when
  messaging is itself broken; a refactor that skips it on some path can
  wedge a daemon forever.
- **Watch the send-failure error paths.** Several branches `return`
  directly on a pack error *without* releasing the caddy (they predate
  the `goto cleanup` convention). If you touch those blocks, prefer
  routing through `cleanup` so the caddy is released — but verify against
  the current code, since a few paths intentionally `return` after
  already having released or transferred ownership.
