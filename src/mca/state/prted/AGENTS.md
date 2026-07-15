# AGENTS.md — `state/prted` (the daemon state machine)

Component guide for `src/mca/state/prted/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`prte_state_caddy_t` caddy, the activation machinery, and the common
`base/` handlers referenced throughout.

---

## Role and selection

`prted` is the **slimmer state machine that runs on every per-node
daemon** (`prted`). A daemon does not orchestrate allocation, mapping, or
the DVM lifecycle — the HNP does that (see [`../dvm`](../dvm/AGENTS.md)).
The daemon's job is narrow: fork/exec its local application procs, watch
their lifecycle, and **report status back up to the HNP**. So its state
tables are short. It is selected when `PRTE_PROC_IS_DAEMON` is true, at
priority **100** (`state_prted_component.c`, `state_prted_component_query`);
on a non-daemon process its query returns `PRTE_ERROR`, so it cannot win.

Files:

| File | Contents |
|------|----------|
| `state_prted_component.c` | Registration; `query` returns priority 100 + the module only when `PRTE_PROC_IS_DAEMON`. |
| `state_prted.c` | The module vtable, the short job/proc state tables, `init`/`finalize`, and the daemon-side handlers (`track_jobs`, `track_procs`) plus the state-update packers. |
| `state_prted.h` | Extern decls for `prte_mca_state_prted_component` and `prte_state_prted_module`. |

Like the DVM component, `prte_state_prted_module` points all ten vtable
slots at the base implementations; the component's substance is its
`init()` tables and its two local handlers.

---

## The state tables it installs

### Job states (only two)

| Job state | Handler | What it does |
|-----------|---------|--------------|
| `PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE` | `track_jobs` *(local)* | Pack every local child's vpid/pid/state for this job and send `PRTE_PLM_LOCAL_LAUNCH_COMP_CMD` to the HNP on `PRTE_RML_TAG_PLM`. |
| `PRTE_JOB_STATE_READY_FOR_DEBUG` | `track_jobs` *(local)* | Pack local child vpid/pid and send `PRTE_PLM_READY_FOR_DEBUG_CMD` to the HNP. |

Plus two added outside the arrays in `init()`:

- `PRTE_JOB_STATE_FORCED_EXIT` → `prte_quit`
- `PRTE_JOB_STATE_DAEMONS_TERMINATED` → `prte_quit`

That is the whole job machine — a daemon never sees `ALLOCATE`, `MAP`,
`LAUNCH_APPS`, etc. as *states*; those are HNP concerns. The daemon
receives the HNP's launch command over the RML and acts on it, then uses
these two states to report the outcome back.

### Proc states (six, all local `track_procs`)

```
PRTE_PROC_STATE_RUNNING, READY_FOR_DEBUG, REGISTERED,
IOF_COMPLETE, WAITPID_FIRED, TERMINATED  →  track_procs  (local)
```

**Important contrast with the DVM component:** the DVM machine routes
these same six proc states to the *base* `prte_state_base_track_procs`;
this component routes them to its **own** local `track_procs`. They share
a name but are different functions — the daemon's version reports up to
the HNP rather than rolling counts into HNP-side job states.

---

## Key local handlers

### `track_procs` (the daemon proc workhorse)
Keyed on `caddy->name` / `caddy->proc_state`. Per incoming proc state:

- **`RUNNING`** — set `pdata->state`, bump `jdata->num_launched`; when it
  equals `num_local_procs`, activate `PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE`
  (which fires `track_jobs` to notify the HNP).
- **`REGISTERED`** — bump `num_reported`; when all local procs have
  registered, pack `PRTE_PLM_REGISTERED_CMD` + local vpids and send to
  the HNP.
- **`IOF_COMPLETE` / `WAITPID_FIRED`** — set the corresponding proc flag;
  when *both* have fired (and the proc isn't already recorded), activate
  `PRTE_PROC_STATE_TERMINATED`. (These two events race, so termination is
  gated on the pair.)
- **`READY_FOR_DEBUG`** — count local procs ready; when all are, activate
  `PRTE_JOB_STATE_READY_FOR_DEBUG` to tell the HNP.
- **`TERMINATED`** — mark recorded, bump `num_terminated`; if the daemon
  is terminating and its routes/children are gone, activate
  `PRTE_JOB_STATE_DAEMONS_TERMINATED`. When all local procs of the job
  have terminated (and not already notified), pack and send
  `PRTE_PLM_UPDATE_PROC_STATE` to the HNP, then locally tear the job
  down: deregister the nspace from the PMIx server, release the job's
  mapped node/proc resources (decrement counters, drop procs/nodes, free
  the map), optionally run the fd-leak check, notify a data server, and
  release the job object.

### `track_jobs`
Handles the two job states above. It walks `prte_local_children`, and for
each child in the target job packs vpid + pid (+ state/exit-code for
failed procs; `RUNNING` is packed for still-live procs to avoid a race),
then reliable-sends the assembled buffer to `PRTE_PROC_MY_HNP`.

### Packers
`pack_state_for_proc` (vpid/pid/state/exit-code for one proc) and
`pack_state_update` (all not-yet-reported local children of a job,
terminated with a `PMIX_RANK_INVALID` sentinel) build the
`PRTE_PLM_UPDATE_PROC_STATE` payload; the latter sets
`PRTE_PROC_FLAG_TERM_REPORTED` so a proc is reported exactly once.

---

## Wiring to other frameworks

Everything this component does flows **upward to the HNP over the RML**
using `PRTE_RML_RELIABLE_SEND` to `PRTE_PROC_MY_HNP` with
`PRTE_RML_TAG_PLM` and the `prte_plm_cmd_flag_t` command codes
(`PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`, `PRTE_PLM_READY_FOR_DEBUG_CMD`,
`PRTE_PLM_REGISTERED_CMD`, `PRTE_PLM_UPDATE_PROC_STATE` — all defined in
[`src/mca/plm/plm_types.h`](../../../plm/plm_types.h)). It also calls the
PMIx server (nspace deregistration), `prte_iof` (channel close /
completion), and `prte_quit` (on the two exit states). It does **not**
touch `ras`/`rmaps`/`plm` launch logic — those run on the HNP.

---

## Gotchas when editing

- **The daemon and HNP machines must stay consistent.** The daemon's
  `track_procs` reports states that the HNP's `prte_state_base_track_procs`
  and `check_complete` consume; changing what/when the daemon reports can
  strand the HNP waiting for a count that never completes. When you touch
  one side, re-check the other.
- **Two functions named `track_procs` exist.** This local one (daemon,
  reports up) and `prte_state_base_track_procs` (HNP-side, rolls up job
  states). Don't confuse them when grepping.
- **`num_local_procs`, not `num_procs`.** The daemon reasons about *its
  local* procs; the counters here (`num_launched`, `num_reported`,
  `num_terminated`) are compared against `num_local_procs`. The HNP
  compares against the job-wide `num_procs`.
- **Report-once flags are load-bearing.** `PRTE_PROC_FLAG_RECORDED`,
  `PRTE_PROC_FLAG_TERM_REPORTED`, and `PRTE_JOB_TERM_NOTIFIED` prevent
  double-counting/double-sending on the racy IOF/waitpid paths. Preserve
  them.
- **New states need a `plm_types.h` code and index-aligned tables.** Same
  rule as the framework guide: add the unique numeric value in
  [`plm_types.h`](../../../plm/plm_types.h) and register the handler in
  the matching `*_states[]`/`*_callbacks[]` position.
- **Every handler ends with `PMIX_RELEASE(caddy)`** (via the `cleanup:`
  label). New early-outs must reach it.
