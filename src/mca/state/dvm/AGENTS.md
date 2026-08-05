# AGENTS.md — `state/dvm` (the HNP state machine)

Component guide for `src/mca/state/dvm/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`prte_state_caddy_t` caddy, the activation machinery, and the common
`base/` handlers referenced throughout.

---

## Role and selection

`dvm` is the **full job-launch and job-termination state machine that
runs on the HNP** (the DVM master / `prte` process). It owns the long
`INIT → … → ALL_JOBS_COMPLETE` sequence and every termination/notify
state. It is selected when `PRTE_PROC_IS_MASTER` is true, at priority
**100** (`state_dvm_component.c`, `state_dvm_component_query`); on any
non-master process its query returns `PRTE_ERR_NOT_AVAILABLE` so it
cannot win. Exactly one state component runs per process, so on the HNP
`dvm` is the state machine.

Files:

| File | Contents |
|------|----------|
| `state_dvm_component.c` | Registration; `query` returns priority 100 + the module only when `PRTE_PROC_IS_MASTER`. |
| `state_dvm.c` | Everything else: the module vtable, the `launch_states[]`/`launch_callbacks[]` tables, `init`/`finalize`, and the DVM-specific handlers. |
| `state_dvm.h` | Extern decls for `prte_mca_state_dvm_component` and `prte_state_dvm_module`. |

The vtable (`prte_state_dvm_module`) points all ten API slots at the
base implementations (`prte_state_base_activate_job_state`,
`prte_state_base_add_job_state`, …). The component's only real work is
in `init()`, which builds the tables, and in the handlers below.

---

## The job-state table it installs

`init()` walks two index-aligned arrays and calls
`prte_state.add_job_state()` for each pair. In order:

| # | Job state | Handler | What the transition does |
|---|-----------|---------|--------------------------|
| 1 | `PRTE_JOB_STATE_INIT` | `prte_plm_base_setup_job` | Assign nspace, set up the job object. |
| 2 | `PRTE_JOB_STATE_INIT_COMPLETE` | `init_complete` *(local)* | Kick `ALLOCATE` (always routed through allocate so the DVM can be expanded). |
| 3 | `PRTE_JOB_STATE_ALLOCATE` | `prte_ras_base_allocate` | `ras` discovers nodes/slots. |
| 4 | `PRTE_JOB_STATE_ALLOCATION_COMPLETE` | `prte_plm_base_allocation_complete` | Proceed toward daemon launch. |
| 5 | `PRTE_JOB_STATE_DAEMONS_LAUNCHED` | `prte_plm_base_daemons_launched` | Daemons spawned; await reports. |
| 6 | `PRTE_JOB_STATE_DAEMONS_REPORTED` | `prte_plm_base_daemons_reported` | All prteds checked in. |
| 7 | `PRTE_JOB_STATE_VM_READY` | `vm_ready` *(local)* | Wire up the DVM; release held jobs; announce "DVM ready". |
| 8 | `PRTE_JOB_STATE_MAP` | `prte_rmaps_base_map_job` | `rmaps` assigns procs to nodes. |
| 9 | `PRTE_JOB_STATE_MAP_COMPLETE` | `prte_plm_base_mapping_complete` | Map done. |
| 10 | `PRTE_JOB_STATE_SYSTEM_PREP` | `prte_plm_base_complete_setup` | Final pre-launch setup. |
| 11 | `PRTE_JOB_STATE_LAUNCH_APPS` | `prte_plm_base_launch_apps` | Build/send the launch message to daemons. |
| 12 | `PRTE_JOB_STATE_SEND_LAUNCH_MSG` | `prte_plm_base_send_launch_msg` | Xcast the app-launch command. |
| 13 | `PRTE_JOB_STATE_STARTED` | `job_started` *(local)* | Notify a launch-proxy tool that the first proc started. |
| 14 | `PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE` | `prte_state_base_local_launch_complete` | Optional progress reporting. |
| 15 | `PRTE_JOB_STATE_READY_FOR_DEBUG` | `ready_for_debug` *(local)* | Notify the tool the job is stopped and ready to attach. |
| 16 | `PRTE_JOB_STATE_RUNNING` | `prte_plm_base_post_launch` | Post-launch bookkeeping. |
| 17 | `PRTE_JOB_STATE_REGISTERED` | `prte_plm_base_registered` | All procs registered for sync. |
| 18 | `PRTE_JOB_STATE_TERMINATED` | `check_complete` *(local)* | The big teardown — see below. |
| 19 | `PRTE_JOB_STATE_NOTIFY_COMPLETED` | `dvm_notify` *(local)* | Emit the job-end PMIx event; xcast DVM cleanup. |
| 20 | `PRTE_JOB_STATE_NOTIFIED` | `cleanup_job` *(local)* | Detach child, release the job object; terminate orteds if flagged. |
| 21 | `PRTE_JOB_STATE_ALL_JOBS_COMPLETE` | `prte_quit` | Shut the HNP down. |

Then `init()` adds three more entries outside the arrays:

- `PRTE_JOB_STATE_DAEMONS_TERMINATED` → `prte_quit`
- `PRTE_JOB_STATE_FORCED_EXIT` → `force_quit` *(local; orders orted
  termination then releases)*
- `PRTE_JOB_STATE_REPORT_PROGRESS` → `prte_state_base_report_progress`

**Note what is deliberately absent:** `PRTE_JOB_STATE_LAUNCH_DAEMONS`.
The comment in `state_dvm.c` says *"individual plm's must add a state for
launching daemons"*; `prte_plm_base_frame.c` registers it with
`prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons)`.
So the launch step between `ALLOCATION_COMPLETE` and `DAEMONS_LAUNCHED`
is supplied by the plm base, not by this component.

## The proc-state table it installs

Six proc states, **all** routed to the base workhorse
`prte_state_base_track_procs`:

```
PRTE_PROC_STATE_RUNNING, READY_FOR_DEBUG, REGISTERED,
IOF_COMPLETE, WAITPID_FIRED, TERMINATED  →  prte_state_base_track_procs
```

`track_procs` rolls per-proc events up into job-state activations
(`STARTED`, `RUNNING`, `REGISTERED`, `TERMINATED`, …). Contrast this with
the prted component, which routes the same six proc states to its **own**
local `track_procs` (they are different functions with the same name).

---

## Key local handlers

### `init_complete`
Always activates `ALLOCATE` — even for a job that seems fully specified —
so that a request to expand the DVM has a hook.

### `vm_ready`
Runs when the DVM's daemons have all reported. For the daemon job it
builds a nidmap, appends **per daemon: its name, its `PMIX_PROC_URI`, and
its `PMIX_SERVER_URI`** to that same buffer, and `xcast`s
`PRTE_RML_TAG_WIREUP` to all daemons (skipped for a single-daemon or
`DO_NOT_LAUNCH` DVM). The first two are the RML wireup; the third is not
used by PRRTE at all — it is redistributed so that *any* daemon can answer
a tool's hostname-qualified `PMIX_SERVER_URI` query, rather than only the
master that collected it (see [`../../../pmix/AGENTS.md`](../../../pmix/AGENTS.md)).
The receiving end is `process_wireup()` in `src/grpcomm`, and the three
fields are a **per-record group** — its skip-what-we-already-know
`continue`s must not skip an unpack. Because this handler re-sends the
whole set every time `VM_READY` fires, an elastic **grow** redistributes
with no code of its own. On any pack/get failure it drives
`PRTE_JOB_STATE_FORCED_EXIT` with a `NULL` job (tearing the whole DVM
down) — **and each of those five bailouts must still release the
caddy**; they did not, and every wireup failure leaked the caddy plus the
job reference it held. In elastic mode it then drains completed grow campaigns
(`prte_plm_base_grow_drain(true)`) so held jobs are admitted only after
new daemons are wired up. For the master's own job it sets
`prte_dvm_ready`, emits the "DVM ready" handshake (stdout or
`parent_fd`), and spawns any cached jobs. For an app job it either parks
it (`WAITING_FOR_DAEMONS`, elastic grow in progress) or prepositions
files and advances to `MAP`.

### `check_complete` + `check_complete_resume` (the teardown)
Registered on `PRTE_JOB_STATE_TERMINATED` and by far the largest handler.
It is **split in two** around `PMIx_server_deregister_nspace`:
`check_complete` runs up to the deregistration, issues it with
`dvm_dereg_complete` as the callback, and returns; that callback (on the
PMIx thread) does nothing but `PRTE_PMIX_THREADSHIFT` the caddy back onto
`prte_event_base`, where `check_complete_resume` finishes the job. It used
to block instead — see the framework guide's *Never block a state handler
on PMIx*, which explains why that stalled the whole HNP and why the
`PMIx_server_IOF_deliver` further down is a deliberate exception.
`check_complete_resume` also owns the exit-status decision, including
`report_child_jobs_separately()`.
It: cancels the job timeout; if the job is the daemon job (or NULL),
drains pending grow campaigns and, once `prte_rml_base.n_children == 0`,
activates `DAEMONS_TERMINATED`; otherwise marks the app job terminated,
applies reservation-inheritance dispositions
(`prte_ras_base_check_reservations_on_term`), sends the spawn response,
clears local children, tells IOF the job is done, deregisters the nspace
from the PMIx server, and (for a non-persistent run) reports abnormal
termination and shuts down when the last job ends. (That second half is `check_complete_resume`.) It then **releases the
job's mapped resources** (walks `jdata->map`, decrements
`slots_inuse`/`num_procs`, restores each proc's bound cpus to
`node->available`, releases procs/nodes, frees the map), removes any
named psets, aborts non-separated child jobs, and finally activates
`NOTIFY_COMPLETED`. This is the only job-teardown path in the tree — the
base used to carry an unregistered second copy, which has been removed.

### `dvm_notify`
Builds the `PMIX_EVENT_JOB_END` notification (status, affected proc,
optional abort text), packs it, and `xcast`s it on
`PRTE_RML_TAG_NOTIFICATION` to all daemons. For a persistent DVM it also
xcasts `PRTE_DAEMON_DVM_CLEANUP_JOB_CMD` on `PRTE_RML_TAG_DAEMON` so even
non-participating daemons release the terminated job's slot accounting
(critical now that mapping runs on the backend daemons). Ends by
activating `NOTIFIED`.

### `cleanup_job`
On `NOTIFIED`: if `terminate_dvm` was flagged, terminate the orteds
once; detach the job from its spawn parent's child list; release the job.

---

## Wiring to other frameworks

The table above *is* the wiring: `ras` owns `ALLOCATE`, `rmaps` owns
`MAP`, `plm` owns most of the launch states plus the injected
`LAUNCH_DAEMONS`. This component supplies the DVM-lifecycle glue
(`init_complete`, `vm_ready`) and the termination/notify machinery. It
also leans on `grpcomm.xcast` for WIREUP/notification/cleanup broadcasts,
the PMIx server for nspace deregistration and events, and `prte_plm`
(`terminate_orteds`, `terminate_procs`, `spawn`) for lifecycle actions.

---

## Gotchas when editing

- **Keep `launch_states[]` and `launch_callbacks[]` index-aligned.**
  They are zipped together in `init()`; inserting a state without
  inserting its callback at the same index silently misassigns every
  later handler.
- **Don't add `LAUNCH_DAEMONS` here.** It is intentionally injected by
  the plm base. Adding it in both places trips the duplicate check in
  `add_job_state` (`PRTE_ERR_BAD_PARAM`).
- **New states need a `plm_types.h` code too.** Registering a handler
  for a state that has no unique numeric value in
  [`src/mca/plm/plm_types.h`](../../../plm/plm_types.h) is the classic
  mistake — see the framework guide.
- **`check_complete` and the other resource-release paths must stay
  consistent.** The same map-teardown logic (restore cpus, decrement
  counters, drop procs/nodes) appears in `check_complete` here, in the
  prted component's `track_procs`, and in
  `prte_state_base_recover_resources` (the errmgr's per-proc recovery). A
  change to how resources are recovered usually needs to be mirrored.
- **A handler that hands its caddy to a continuation must NOT release it.**
  `check_complete` returns without releasing once it has passed the caddy
  to `PMIx_server_deregister_nspace`; `check_complete_resume` owns it from
  there. Adding an early return between those two points needs a release;
  adding one after the hand-off must not have one.
- **Every other handler ends with `PMIX_RELEASE(caddy)` — the error paths too.**
  This is where this component has repeatedly gone wrong: `vm_ready`'s
  five `FORCED_EXIT` bailouts, `job_started`'s missing-launch-proxy
  bailout, and `dvm_notify`'s two `DVM_CLEANUP_JOB` pack failures all
  returned without releasing. `PRTE_ACTIVATE_JOB_STATE(...); return;` is
  **not** a release — the activation queues a *new* caddy; yours is still
  yours to drop. When adding a bailout, add the release with it.
- **Release a `pmix_proc_t` from `prte_get_attribute` exactly once.**
  `PRTE_JOB_LAUNCH_PROXY` hands back an allocated `pmix_proc_t`;
  `ready_for_debug` released it as soon as it had been copied into the
  info list, and then released it *again* on a later error path.
- **Clear `PRTE_NODE_FLAG_MAPPED` before releasing the node.** The map
  holds a reference, so `PMIX_RELEASE(node)` can be the last one; touching
  the flag afterwards is a use-after-free waiting for the refcount to line
  up. Same ordering applies in the prted component and in
  `prte_state_base_recover_resources`.
- **A `continue` inside the per-proc release loop skips the release.**
  `check_complete` restores each proc's bound cpus before dropping it from
  the node; when the cpuset failed to decode it used to `continue`, which
  also skipped `pmix_pointer_array_set_item(node->procs, i, NULL)` and the
  `PMIX_RELEASE(proc)` — leaking the proc and leaving a dangling entry on
  the node. That block is now a `do { … } while (0)` so a `break`
  abandons only the cpu restore.
- **Log the status you actually failed with.** Two pack failures in
  `vm_ready` called `PMIX_ERROR_LOG(ret)` where `ret` still held the
  *previous* `PMIx_Get`'s status (`PMIX_SUCCESS`), reporting success for a
  failure. Check the variable name when you copy an error block.
- **Persistent vs. non-persistent branches differ.** `check_complete`
  and `dvm_notify` behave differently under `prte_persistent`; test both
  a one-shot `prterun` and a `prte --daemonize` + `prun` + `pterm` cycle
  when touching them.
