# AGENTS.md — `errmgr/dvm` (the HNP error policy engine)

Component guide for `src/mca/errmgr/dvm/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine callback model, and the progress-thread threading notes
referenced throughout.

---

## Role and priority

`dvm` is the errmgr component that runs **only on the HNP / DVM master**.
Priority **1000** (MCA param `errmgr_dvm_priority`), but selection is by
role, not number: `dvm_component_query()` returns the module only when
`PRTE_PROC_IS_MASTER`, otherwise `*module = NULL; *priority = -1;
return PRTE_ERROR`. It is the process that decides DVM-wide policy when
something fails — whether to notify a job's submitter, terminate a single
job while keeping the DVM up, or tear the whole DVM down.

Files:

| File | Contents |
|------|----------|
| `errmgr_dvm_component.c` | Registration, `priority` MCA param (default 1000), `query` gated on `PRTE_PROC_IS_MASTER`, open/close no-ops. |
| `errmgr_dvm.c` | The module: `init`/`finalize`, the two state handlers `job_errors` and `proc_errors`, and helpers `_terminate_job` and `check_send_notification`. |
| `errmgr_dvm.h` | Exports `prte_mca_errmgr_dvm_component` and `prte_errmgr_dvm_module`. |

---

## How it wires into the state machine (`init`)

`init()` registers three callbacks and returns `PRTE_SUCCESS`:

```c
prte_state.add_job_state(PRTE_JOB_STATE_ERROR,          job_errors);
prte_state.add_proc_state(PRTE_PROC_STATE_COMM_FAILED,  proc_errors);
prte_state.add_proc_state(PRTE_PROC_STATE_ERROR,        proc_errors);
```

`PRTE_JOB_STATE_ERROR` and `PRTE_PROC_STATE_ERROR` are the **generic
error catch-alls**: activating any job/proc *error* state routes here
(the actual `caddy->job_state` / `caddy->proc_state` carries the specific
value). `COMM_FAILED` is registered separately because it is meant to run
at message priority so last messages from the failing peer can still be
drained. `finalize()` is a no-op.

---

## `job_errors` — job-level failures

Fires when a job is activated into an error state. Flow:

1. `PMIX_ACQUIRE_OBJECT(caddy)`; bail immediately if `prte_finalizing`.
2. If `caddy->jdata == NULL`, this refers to the **daemon job** — back-fill
   it from `prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace)` and
   `PMIX_RETAIN` it.
3. Copy `caddy->job_state` into `jdata->state`.
4. **Two policies, chosen by whose job it is:**

   **The daemon job itself** (`nspace == PRTE_PROC_MY_NAME->nspace`) —
   the DVM is in trouble:
   - `FAILED_TO_START` / `NEVER_LAUNCHED` / `FAILED_TO_LAUNCH` /
     `CANNOT_LAUNCH`: disable routing (`prte_routing_is_enabled = false`)
     and activate `PRTE_JOB_STATE_DAEMONS_TERMINATED` to exit.
   - `ABORTED` while `num_procs != num_reported`: a daemon likely died
     without finding its way back — show `help-errmgr-base.txt:
     failed-daemon` and disable routing.
   - Otherwise mark `num_terminated = num_procs` and activate
     `PRTE_JOB_STATE_TERMINATED` — there is nothing to do but exit,
     because the failure is in the DVM plumbing.

   **A submitted application job** (any other nspace) — keep the DVM
   alive, only the job dies:
   - Convert the job state to a PMIx error
     (`prte_pmix_convert_job_state_to_error`) and send a **spawn
     response** (`prte_plm_base_spawn_response`) so a quick-failing job
     still generates a reply to its requestor (`jdata->originator`).
   - `_terminate_job(jdata->nspace)` to kill any of its procs still
     running.
   - If the job never actually launched (`FAILED_TO_START`,
     `NEVER_LAUNCHED`, `FAILED_TO_LAUNCH`, `ALLOC_FAILED`, `MAP_FAILED`,
     `CANNOT_LAUNCH`), activate `PRTE_JOB_STATE_TERMINATED` explicitly —
     no proc states will fire to drive termination otherwise.
5. `PMIX_RELEASE(caddy)` on every exit path.

The `MAP_FAILED`/`ALLOC_FAILED`/`NEVER_LAUNCHED` cases are exactly the
failure states `rmaps`/`ras`/`plm` funnel here — this is the far end of
those frameworks' `..._FAILED` transitions.

---

## `proc_errors` — process- and daemon-level failures

The larger handler. After the standard acquire / `finalizing` / `jdata`
lookup, it splits into **daemon** vs **application** proc handling.

### Daemon proc errors (`nspace == PRTE_PROC_MY_NAME->nspace`)

For the communication-loss family — `COMM_FAILED`, `HEARTBEAT_FAILED`,
`UNABLE_TO_SEND_MSG`, `FAILED_TO_CONNECT`, `FAILED_TO_START`:

1. **Ignore my own connection** (`proc->rank == PRTE_PROC_MY_NAME->rank`).
2. **Elastic shrink echo-guard.** If `prte_elastic_mode` and the daemon
   is already not-`ALIVE` with `state >= PRTE_PROC_STATE_TERMINATED`, this
   is a harmless late comm-failure for a daemon already torn out of the
   DVM (the collective shrink-completion handler in `ras_base_allocate.c`
   proactively marked it) — ignore it, or we would double-decrement
   `num_daemons` and re-drive the abort logic. The `state >= TERMINATED`
   test is deliberate: it lets a genuine `FAILED_TO_START` daemon (never
   alive, but state still below TERMINATED) fall through and be handled.
3. Mark the daemon gone: `PRTE_FLAG_UNSET(pptr, PRTE_PROC_FLAG_ALIVE)`,
   record `pptr->state = state`, and `--prte_process_info.num_daemons`.
4. **Elastic grow rollback.** If `prte_plm_base_grow_target_failed(rank)`
   claims this rank (it was an in-flight grow target), the grow campaign
   absorbs the loss — `goto cleanup` and skip the general daemon-loss
   handling, which would otherwise abort the whole DVM over a failure the
   rollback already handled.
5. **Ordered termination in progress** (`prte_prteds_term_ordered ||
   prte_abnormal_term_ordered`): record the daemon as gone via
   `prte_rml_route_lost`, and if no routed children remain
   (`prte_rml_base.n_children == 0`) and all local children are dead,
   activate `DAEMONS_TERMINATED` to exit; else just note the remaining
   routes. `goto cleanup`.
6. **Unexpected daemon loss** (the real fault path): show
   `node-died` (unless `FAILED_TO_START`), call `prte_rml_route_lost`.
   On success **the HNP walks every job** and marks each proc that lived
   on the lost daemon's node `PRTE_PROC_STATE_TERM_WO_SYNC` (only rank 0
   / the HNP does this sweep), then `goto cleanup`. Otherwise mark the
   daemon job `PRTE_JOB_STATE_COMM_FAILED`, stash the offending proc in
   `PRTE_JOB_ABORTED_PROC`, set `PRTE_JOB_FLAG_ABORTED`, and set
   `exit_code` (defaulting to `PRTE_ERR_COMM_FAILURE`).
7. Because comms have failed we cannot trust the routing tree — force
   `prte_abnormal_term_ordered = true` and activate `DAEMONS_TERMINATED`.

Any non-comm daemon state hits the `pmix_output(0, "UNSUPPORTED DAEMON
ERROR STATE …")` branch — a real one indicates a bug upstream.

### Application proc errors

First, idempotency: `pptr->state = state` only if
`pptr->state < PRTE_PROC_STATE_TERMINATED` (a proc can be reported more
than once). If `prte_prteds_term_ordered`, check whether any local child
is still alive and, if not and no routed children remain, exit.

Then it always marks the waitpid fired
(`PRTE_ACTIVATE_PROC_STATE(WAITPID_FIRED)`) and, for a **remote** proc,
also marks `IOF_COMPLETE` (we'll hear nothing more about it). It computes
`flag = RECOVERABLE || CONTINUOUS` from the job attributes — the pivot
between "notify and keep going" and "abort the job":

| Proc state | `flag` set (recoverable/continuous) | `flag` clear |
|------------|-------------------------------------|--------------|
| `KILLED_BY_CMD` | notify `PMIX_ERR_PROC_KILLED_BY_CMD` + recover resources | if all procs terminated → `TERMINATED` |
| `ABORTED_BY_SIG` | notify `PMIX_ERR_PROC_ABORTED_BY_SIG` + recover | set `JOB_STATE_ABORTED_BY_SIG`, record aborted proc, `_terminate_job` |
| `TERM_WO_SYNC` | notify `PMIX_ERR_PROC_TERM_WO_SYNC` + recover | set `ABORTED_WO_SYNC`; if `exit_code == 0` force `PRTE_ERROR_DEFAULT_EXIT_CODE` so the user sees an error; `_terminate_job` |
| `FAILED_TO_START` / `FAILED_TO_LAUNCH` | *(unconditional)* set `FAILED_TO_START`/`_LAUNCH`, `_terminate_job`, activate `FAILED_TO_START`; if it was a daemon, show `failed-daemon-launch` | same |
| `CALLED_ABORT` | notify `PMIX_ERR_PROC_REQUESTED_ABORT` + recover | set `CALLED_ABORT`, `_terminate_job` |
| `TERM_NON_ZERO` | if `PRTE_JOB_ERROR_NONZERO_EXIT` also set: notify `PMIX_ERR_EXIT_NONZERO_TERM` + recover | set `NON_ZERO_TERM`, `_terminate_job`; always bump `PRTE_JOB_NUM_NONZERO_EXIT` |
| default | if `num_terminated == num_procs` → `TERMINATED` |

The abort branches all guard on `!PRTE_JOB_FLAG_ABORTED` so only the
**first** offending proc drives the abort, `PMIX_RETAIN` the recorded
proc so it survives, and stash it in `PRTE_JOB_ABORTED_PROC` for the
eventual error report. `prte_state_base_recover_resources` is what makes
a recoverable/continuous job survivable — it frees the dead proc's slot
so a replacement can be mapped.

---

## Helpers

### `_terminate_job(nspace)`
Builds a one-element proc array holding `{nspace, PMIX_RANK_WILDCARD}`
and calls `prte_plm.terminate_procs()` — the standard "kill this whole
job" call.

### `check_send_notification(jdata, proc, event)`
Emits a PMIx event to the *surviving* procs of a recoverable/continuous
job. Bails if `PRTE_JOB_NOTIFY_ERRORS` is not set, if
`prte_dvm_abort_ordered`, or if the job is already `ABORTED`. Otherwise
it hand-packs a data buffer — source rank (`PRTE_NAME_INVALID->rank` so
the PMIx server injects it locally), the `pmix_status_t` event, the
source proc, a `PMIX_RANGE_CUSTOM` range, then an info array
(`PMIX_EVENT_AFFECTED_PROC`, `PMIX_EVENT_CUSTOM_RANGE` = the whole job,
and `PMIX_EXIT_CODE` when `exit_code != -1`) — and `prte_grpcomm.xcast`s
it on `PRTE_RML_TAG_NOTIFICATION`. This is the mechanism a fault-tolerant
application uses to learn a peer died without the whole job being killed.

---

## Things to watch when editing

- **Daemon-job vs application-job is the top-level fork** in both
  handlers (`PMIX_CHECK_NSPACE(jdata->nspace,
  PRTE_PROC_MY_NAME->nspace)`). Keep the two policies distinct: a daemon
  failure may kill the DVM; an app failure must not.
- **The elastic guards are load-bearing and gated on `prte_elastic_mode`.**
  The shrink echo-guard (`!ALIVE && state >= TERMINATED`) and the grow
  rollback (`prte_plm_base_grow_target_failed`) each `goto cleanup` to
  skip the general daemon-loss abort. Removing or reordering them
  re-introduces the double-decrement / spurious-abort bugs recorded in
  the repo memory — do not touch without re-running the dockerswarm
  grow/shrink harness.
- **`flag` (recoverable/continuous) decides notify-vs-abort** for every
  application proc state. Preserve the `!PRTE_JOB_FLAG_ABORTED` "first
  failure wins" guard and the `PMIX_RETAIN` on the recorded proc, or you
  will either double-abort or free a proc that the later error report
  still needs.
- **Every path must `PMIX_RELEASE(caddy)`.** The handlers are littered
  with `goto cleanup`; a new early return that skips it leaks the caddy.
  (The `prte_finalizing` early return in `job_errors` was one such
  leak — it now releases the caddy before returning.)
- **Compare the state, don't test the constant.** The
  `FAILED_TO_START`/`FAILED_TO_LAUNCH` arm of `proc_errors` compares
  `PRTE_PROC_STATE_FAILED_TO_START == state`. An earlier version wrote
  `if (PRTE_PROC_STATE_FAILED_TO_START)` — a constant that is always
  true — which silently forced the `FAILED_TO_LAUNCH` case down the
  `FAILED_TO_START` branch. When you extend a state switch, always
  compare `state` explicitly.
- **In a liveness loop, test the iterated child, not the failed peer.**
  The daemon ordered-termination arm of `proc_errors` walks
  `prte_local_children` looking for one still `PRTE_PROC_FLAG_ALIVE`
  before it decides the node is empty and activates
  `DAEMONS_TERMINATED`. That test must read the loop variable
  (`proct`) — an earlier version tested the *failed daemon* `pptr`,
  whose `ALIVE` flag had just been cleared a few lines above, so the
  guard was always false and the DVM could declare itself done while a
  local child was still alive. The two sibling loops (the application
  arm here, and the daemon `proc_errors` loop in the `prted` component)
  both correctly test the iterated child; keep all three consistent.
