# AGENTS.md — The `plm` Framework (Process Launch Manager)

Orientation for AI agents and human contributors working in
`src/mca/plm/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`plm` (Process Launch Manager) answers one question: **how do we get a
`prted` daemon running on every node of the DVM?** It is the switchyard
for daemon launch. It runs on the HNP (DVM master): the HNP orchestrates
the launch, the daemons phone home, and the state machine advances. The
components differ only in the *mechanism* used to start the remote
daemons — `ssh` tree-spawn, `srun`, `lsb_launch`, `aprun` — and in
whether the launcher (SLURM/LSF/PALS) or PRRTE itself decides which
daemon lands on which node.

`plm` sits in the DVM/job-launch state machine at `LAUNCH_DAEMONS`:

```
INIT → INIT_COMPLETE → ALLOCATE → ALLOCATION_COMPLETE → LAUNCH_DAEMONS
     → DAEMONS_LAUNCHED → DAEMONS_REPORTED → VM_READY → MAP → MAP_COMPLETE
     → SYSTEM_PREP → LAUNCH_APPS → SEND_LAUNCH_MSG → RUNNING → TERMINATED
                          ▲                    ▲
       plm launches prteds here    daemons phone home / apps launch
```

The `state` framework fires the component's `launch_daemons` handler
when the daemon job enters `PRTE_JOB_STATE_LAUNCH_DAEMONS`. That handler
calls `prte_plm_base_setup_virtual_machine()` to compute the daemon map
(which nodes need a new daemon, and what vpid each gets), then spawns
those daemons by whatever mechanism the component implements. Crucially,
**launch is asynchronous**: the handler returns after *starting* the
launch and sets the job to `DAEMONS_LAUNCHED`. The job does **not**
advance until every launched daemon calls back to the HNP
(`prte_plm_base_daemon_callback`), at which point the framework advances
to `DAEMONS_REPORTED` and on to `VM_READY`. Only then does mapping and
application launch proceed.

Two very different flows share this framework:

1. **DVM formation** — the daemon job (`PRTE_PROC_MY_NAME->nspace`) is
   launched once to stand up the daemons. This is the launcher-heavy
   path.
2. **Application jobs** — every later `prun`/`comm_spawn` job flows
   through the same state machine, but its `LAUNCH_DAEMONS` step usually
   finds `map->num_new_daemons == 0` ("no new daemons required") and
   fast-forwards straight to `DAEMONS_REPORTED`. Application launch
   itself is not a `plm` component's job — it is the `odls` on each
   daemon, driven by the base `launch_apps`/`send_launch_msg` handlers.

---

## Directory layout

```
plm/
  plm.h                       # the module/component vtable (function-pointer struct)
  plm_types.h                 # **job / proc / node / app STATE codes** + PLM command codes (tree-wide!)
  base/
    base.h                    # public base API (state-machine handlers, spawn_response, ...)
    plm_private.h             # framework-internal API + prte_plm_globals_t + proxy/prted-cmd helpers
    plm_base_frame.c          # framework open/close/register; the DEFAULT "local-only" module
    plm_base_select.c         # pick-ONE-component selection (highest priority wins)
    plm_base_receive.c        # the HNP command processor: tools/daemons → HNP (PRTE_RML_TAG_PLM)
    plm_base_launch_support.c # the heart: state handlers, daemon callback/wireup, setup_vm, arg building
    plm_base_prted_cmds.c     # xcast-based terminate/kill/signal commands to daemons
    plm_base_jobid.c          # HNP nspace + per-job jobid assignment
    help-plm-base.txt         # user-facing error text
  ssh/                        # DEFAULT/fallback (pri 10): rsh/ssh tree-spawn — the reference impl
  slurm/                      # SLURM (pri 75): one srun launches all prteds
  lsf/                        # LSF (pri 75): lsb_launch() API call
  pals/                       # Cray PALS (pri 100, only built where PALS exists): aprun
```

Read `plm_types.h` first — it is consumed **tree-wide**. It carries the
authoritative `PRTE_JOB_STATE_*`, `PRTE_PROC_STATE_*`,
`PRTE_NODE_STATE_*`, and `PRTE_APP_STATE_*` numeric codes plus the
`prte_plm_cmd_flag_t` command codes. Then read
`plm_base_launch_support.c`, where control actually lives.

### `plm_types.h` — the state codes (repo-critical)

These `#define`d integers are hand-assigned and **every value must stay
unique within its family** (see the top-level AGENTS.md rule on status
codes). A few load-bearing boundaries and values:

| Family | Boundary / key values |
|--------|-----------------------|
| Proc state | `UNTERMINATED = 15`; `RUNNING = 4`, `REGISTERED = 5`; `TERMINATED = 20`; `ERROR = 50` (error codes are offsets from it — `FAILED_TO_START = ERROR+3`, `COMM_FAILED = ERROR+6`, `ABORTED = ERROR+2`, …). Anything `< UNTERMINATED` means still-running. |
| Job state | `LAUNCH_DAEMONS = 8`, `DAEMONS_LAUNCHED = 9`, `DAEMONS_REPORTED = 10`, `VM_READY = 11`, `RUNNING = 14`; `UNTERMINATED = 30`, `TERMINATED = 31`; `ERROR = 50` (`FAILED_TO_START = ERROR+3`, `NEVER_LAUNCHED = ERROR+10`, `MAP_FAILED = ERROR+19`, …). |
| Node state | `UP = 3`, `DOWN = 2`, `DO_NOT_USE = 5`, `NOT_INCLUDED = 6`, `ADDED = 7`. |
| PLM commands | `LAUNCH_JOB_CMD = 1`, `UPDATE_PROC_STATE = 2`, `REGISTERED_CMD = 3`, `TOOL_ATTACHED_CMD = 4`, `READY_FOR_DEBUG_CMD = 5`, `LOCAL_LAUNCH_COMP_CMD = 6`. |

Note the sequences deliberately **skip** some offsets (e.g. job error
`ERROR+15`) — do not reuse a gap assuming it is free.

---

## The module contract

Every component fills in the same vtable, declared in `plm.h` as
`prte_plm_base_module_t` (version macro
`PRTE_PLM_BASE_VERSION_2_0_0`). **All entries are mandatory** in
principle, but in practice most components reuse the base implementations
for everything except `spawn`, `init`, and `finalize`. The selected
module is copied wholesale into the global `prte_plm`.

| Field | Signature | Meaning / return |
|-------|-----------|------------------|
| `init` | `int (*)(void)` | One-time setup: start the PLM recvs, register the component's `launch_daemons` handler on `PRTE_JOB_STATE_LAUNCH_DAEMONS`, set `daemon_nodes_assigned_at_launch`. Returns `PRTE_SUCCESS`/error. |
| `set_hnp_name` | `int (*)(void)` | Create the DVM's base nspace + HNP procID. **Every** component points this at `prte_plm_base_set_hnp_name`. |
| `spawn` | `int (*)(prte_job_t *jdata)` | **Non-blocking.** Kick a job into the launch state machine (`ACTIVATE_JOB_STATE INIT`, or `MAP` for a restart). The actual daemon launch happens later in the `LAUNCH_DAEMONS` handler, not here. Returns immediately. |
| `remote_spawn` | `int (*)(void)` | Called *on a daemon* to launch that daemon's own children — the tree-spawn fan-out. Only `ssh` implements it; everyone else leaves it `NULL`. |
| `terminate_job` | `int (*)(pmix_nspace_t)` | Kill all procs of a job. Base impl `prte_plm_base_prted_terminate_job` (xcast a kill-local-procs command). |
| `terminate_orteds` | `int (*)(void)` | Tear down the daemons themselves. Base impl `prte_plm_base_prted_exit` (xcast `PRTE_DAEMON_EXIT_CMD` / `HALT_VM`). SLURM/PALS wrap it to also reconcile their launcher-process bookkeeping. |
| `terminate_procs` | `int (*)(pmix_pointer_array_t *procs)` | Kill a specific proc set. Base impl `prte_plm_base_prted_kill_local_procs`. |
| `signal_job` | `int (*)(pmix_nspace_t, int32_t)` | Signal a job's procs. Base impl `prte_plm_base_prted_signal_local_procs`. |
| `finalize` | `int (*)(void)` | Stop recvs, free launcher state. |

Unlike `rmaps` (whose module return codes are a per-job "is this mine?"
protocol), `plm` selects **one** module and it owns everything. The
"non-blocking spawn, wait for callback" contract is the thing to
internalize: never block the progress thread waiting for a daemon to
come up.

---

## Component selection — pick ONE (unlike rmaps)

`prte_plm_base_select()` (in `plm_base_select.c`) uses the standard
`pmix_mca_base_select()`: it queries every component, and the **single
highest-priority** component that returns a module wins. Its module is
copied into `prte_plm`. If *no* component selects (e.g. purely local
operation with no launcher), selection quietly leaves the **default
"local-only" module** defined in `plm_base_frame.c` in place and returns
success — an error is only raised later if someone actually tries to
launch daemons ("no-available-pls").

Selection is driven by the **resource-manager environment**, not by a
fixed priority ladder — each component's `query` inspects the
environment and either offers itself or bows out:

| Component | Priority | Selected when… |
|-----------|----------|----------------|
| `pals` | 100 (MCA `plm_pals_priority`) | Always offers itself — **but only built where Cray PALS is detected** (`PRTE_CHECK_PALS`), so it is simply absent elsewhere. |
| `slurm` | 75 | `SLURM_JOBID` is set in the environment (and `srun --version` runs). |
| `lsf` | 75 | `LSB_JOBID` set, IBM CSM **not** enabled (`CSM_ALLOCATION_ID` unset), and `lsb_init()` succeeds. Only built when LSF headers/libs are found. |
| `ssh` | 10 (MCA `plm_ssh_priority`) | Always available once a launch agent (`ssh`/`rsh`, or `qrsh`/`llspawn`/`pbs_tmrsh`) is found in PATH. The **default/fallback**. |

Because it is pick-one, in a SLURM allocation `slurm` (75) beats `ssh`
(10); on a bare cluster only `ssh` is present. `ssh` is deliberately the
lowest-priority, always-there catch-all — read it first, it is the
reference implementation.

The `rsh` name is a registered **alias** for `ssh` (see
`mca_plm_base_register` in `plm_base_frame.c`): `--prtemca plm rsh` still
works and maps to the `ssh` component, and its MCA vars alias too.

---

## What `base/` provides — walk the important pieces

A component is mostly glue around the base. The base owns the entire
state machine, the daemon callback/wireup, the orted command-line
construction, and all the xcast-based termination. Understand these
before touching a component.

### The state-machine handlers (`plm_base_launch_support.c`)

The `state` framework calls these as a job walks the launch states. Each
is a libevent handler taking `(int fd, short args, void *cbdata)` where
`cbdata` is a `prte_state_caddy_t *` carrying the `jdata`. They form the
spine of launch:

| Handler | State it services → what it does |
|---------|----------------------------------|
| `prte_plm_base_setup_job` | `INIT` → assign a jobid (`prte_plm_base_create_jobid`), arm spawn/job timeout timers, then `INIT_COMPLETE`. |
| `prte_plm_base_setup_job_complete` | `INIT_COMPLETE` → `ALLOCATE`. |
| `prte_plm_base_allocation_complete` | `ALLOCATION_COMPLETE` → `LAUNCH_DAEMONS` (the component's handler). Has a bootstrap-DVM special case that stands up the VM directly. |
| `prte_plm_base_daemons_launched` | `DAEMONS_LAUNCHED` → **deliberately a no-op**; we wait for daemons to phone home rather than advancing. |
| `prte_plm_base_daemons_reported` | `DAEMONS_REPORTED` → set node slots (unmanaged allocations), compute `total_slots_alloc`, then `VM_READY`. |
| `prte_plm_base_vm_ready` | `VM_READY` → check topology limits, preposition files (`filem`), then `MAP`. |
| `prte_plm_base_mapping_complete` | `MAP_COMPLETE` → `SYSTEM_PREP`. |
| `prte_plm_base_complete_setup` | `SYSTEM_PREP` → `LAUNCH_APPS`. |
| `prte_plm_base_launch_apps` | `LAUNCH_APPS` → pack the `PRTE_DAEMON_ADD_LOCAL_PROCS` (or `DVM_ADD_PROCS` for a fixed DVM) command plus the `odls` add-procs payload into `jdata->launch_msg`. |
| `prte_plm_base_send_launch_msg` | `SEND_LAUNCH_MSG` → xcast `jdata->launch_msg` to all daemons on `PRTE_RML_TAG_DAEMON`. This is what actually starts application procs. |
| `prte_plm_base_post_launch` | `RUNNING` → cancel spawn timer, wire up IOF, optionally dump the proctable, send the spawn response. |
| `prte_plm_base_registered` | `REGISTERED` → mark the job registered. |

`prte_plm_base_spawn_response()` notifies the original spawn requestor
(tool via PMIx event, or another daemon via `PRTE_RML_TAG_LAUNCH_RESP`)
that the job launched.

### The daemon callback / wireup — the "report back"

This is the crux of the whole framework. After a component starts a
`prted`, that daemon connects back to the HNP and sends its identity on
`PRTE_RML_TAG_PRTED_CALLBACK`. The recv is
**`prte_plm_base_daemon_callback`**. For each daemon in the buffer it:

1. Looks up the daemon's `prte_proc_t` by rank in the daemon job, sets
   `daemon->state = PRTE_PROC_STATE_RUNNING`, sets `PRTE_PROC_FLAG_ALIVE`.
2. Unpacks and stores the daemon's **contact URI** (`daemon->rml_uri`,
   stashed as `PMIX_PROC_URI`) — this is how the HNP learns to talk to
   the daemon.
3. Unpacks the **node name** (+ aliases), reconciling it with the
   allocation's name (the daemon's `gethostname` result wins; the
   original becomes an alias). Sets `PRTE_NODE_FLAG_DAEMON_LAUNCHED` and
   node state `UP`.
4. Unpacks the node **topology** (possibly compressed), de-duplicating
   against `prte_node_topologies` and recording an hwloc diff when it
   matches an existing one. Under `prte_homo_nodes` only daemon rank 1
   sends a topology and everyone else inherits it.
5. Bumps `jdatorted->num_reported`. When the count reaches
   `num_procs`, `progress_daemons()` sets the daemon job to
   `DAEMONS_REPORTED` and activates that state for every application job
   parked in `DAEMONS_LAUNCHED` — releasing the whole DVM to proceed to
   `VM_READY`.

The failure counterpart is **`prte_plm_base_daemon_failed`** (recv on
`PRTE_RML_TAG_REPORT_REMOTE_LAUNCH`): a daemon (or a proxy launcher)
reports that a specific daemon vpid failed to start; it marks that proc
`FAILED_TO_START` and activates the proc-failure state. `ssh`'s
`ssh_wait_daemon` and the tree-spawn children send to this tag.

Both recvs are registered by **`prte_plm_base_comm_start()`**
(`plm_base_receive.c`), which every component calls from `init`. On the
master it also registers the stack-trace recv; the base
`prte_plm_base_recv` on `PRTE_RML_TAG_PLM` is registered on all procs.

### The command processor (`plm_base_receive.c`)

`prte_plm_base_recv` is the HNP's inbound command handler for
tools/daemons on `PRTE_RML_TAG_PLM`. It runs inside an event (so it is
thread-safe on the progress thread) and switches on
`prte_plm_cmd_flag_t`:

- **`PRTE_PLM_LAUNCH_JOB_CMD`** — the big one. Unpacks a `prte_job_t`,
  records the originator, assigns a `schizo` personality
  (`prte_schizo_base_detect_proxy`), resolves the target **session(s)**
  (spawn-target list via `resolve_spawn_targets`, else session-id /
  alloc-id / ref-id / parent session, with ownership checks), links the
  child to its parent, processes `add-host`/`add-hostfile`, and finally
  calls `prte_plm.spawn(jdata)` (or caches the job if the DVM isn't ready
  yet).
- **`PRTE_PLM_UPDATE_PROC_STATE`** — daemons report per-proc pid/state/
  exit-code; the handler activates the corresponding proc state.
- **`PRTE_PLM_REGISTERED_CMD`** — procs registered for sync; advances to
  `REGISTERED` when all report.
- **`PRTE_PLM_READY_FOR_DEBUG_CMD`** — debugger-stop bookkeeping.
- **`PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`** — a daemon reports its local app
  procs launched (pid + state); advances to `STARTED` on the first and
  `RUNNING` when `num_launched == num_procs`.
- **`PRTE_PLM_TOOL_ATTACHED_CMD`** — register a connecting tool as a job.

### orted command-line construction

Every launcher has to build the argv that starts a remote `prted`. The
base provides the shared pieces:

- **`prte_plm_base_setup_prted_cmd(&argc, &argv)`** — splits
  `prte_launch_agent` (default `"prted"`) into argv and returns the
  index of the `prted` word (so a wrapper like `valgrind ... prted` can
  be handled).
- **`prte_plm_base_prted_append_basic_args(&argc, &argv, ess,
  &proc_vpid_index)`** — appends the standard daemon options: debug
  flags, `--prtemca ess <ess>`, `ess_base_nspace`, `ess_base_vpid
  <template>` (recording `proc_vpid_index` so the launcher can substitute
  the real vpid per node), `ess_base_num_procs`, the HNP URI, and every
  relevant `PMIX_MCA_`/`PRTE_MCA_` env var and cmd-line MCA param —
  while **deliberately skipping** the `rmaps`, `ras`, and `plm`
  frameworks (the daemons must not re-run mapping/allocation, and only
  open the PLM if explicitly told to).
- **`prte_plm_base_wrap_args(argv)`** — quotes multi-word `...mca`
  argument values so shells/launchers don't split them.

### Building the daemon VM — `prte_plm_base_setup_virtual_machine`

Called first thing in every component's `launch_daemons`. It builds the
**daemon job's map**: the set of nodes that need a *new* daemon. It
handles a menagerie of cases — fixed DVM (nothing to do), extend/grow
DVM, dynamic spawn (only "added" nodes), unmanaged allocation
(`-host`/hostfile union), managed allocation (filter the node pool) —
and for each node needing a daemon it:

- creates a `prte_proc_t`, assigns it the **next available vpid**
  (`daemons->num_procs`), records the first as `map->daemon_vpid_start`,
  and bumps `map->num_new_daemons`;
- links node↔daemon, sets `PRTE_NODE_FLAG_LOC_VERIFIED` iff
  `prte_plm_globals.daemon_nodes_assigned_at_launch` (see below);
- once daemons are added, recomputes the RML routing tree
  (`prte_rml_compute_routing_tree`) so the HNP can tree-spawn/xcast.

`map->num_new_daemons` is the key output: `== 0` means every node
already has a daemon, so the component fast-forwards to
`DAEMONS_REPORTED`. It also records elastic **grow campaigns** and the
launch fence in elastic mode (see the repo memory on the re-grow vpid
hole and the add-nodes-to-session ordering bug — this function is where
that logic lives).

### xcast termination/signal (`plm_base_prted_cmds.c`)

Daemon-directed control messages, all packed and broadcast to every
daemon via `prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, …)`:

- `prte_plm_base_prted_exit(cmd)` — `PRTE_DAEMON_EXIT_CMD`, or
  `PRTE_DAEMON_HALT_VM_CMD` when terminating abnormally / before wireup.
- `prte_plm_base_prted_terminate_job(nspace)` — wildcard-proc → kill.
- `prte_plm_base_prted_kill_local_procs(procs)` —
  `PRTE_DAEMON_KILL_LOCAL_PROCS`.
- `prte_plm_base_prted_signal_local_procs(job, signal)`.

### jobid / HNP name (`plm_base_jobid.c`)

- `prte_plm_base_set_hnp_name()` — establishes the DVM base nspace
  (`basename-hostname-pid`, or an inherited `PMIX_SERVER_NSPACE`) and the
  HNP procID `base@0`.
- `prte_plm_base_create_jobid(jdata)` — assigns each new job the nspace
  `base@N` with a monotonic `next_jobid` (wrapping/reusing when
  exhausted).

---

## `daemon_nodes_assigned_at_launch` — a launcher-shaped fork

One global flag (`prte_plm_globals.daemon_nodes_assigned_at_launch`)
captures the single biggest behavioral difference between the launchers:

- **`ssh` → `true`.** We `ssh` to a *specific* host, so we know exactly
  which node each daemon vpid lands on at launch time; the node↔daemon
  binding is verified immediately (`PRTE_NODE_FLAG_LOC_VERIFIED`).
- **`slurm` / `lsf` / `pals` → `false`.** The resource manager does its
  own proc→node placement, so PRRTE cannot know in advance which daemon
  vpid ends up on which node. The binding is resolved only when each
  daemon phones home in `prte_plm_base_daemon_callback` and reports its
  actual node name. (When `PRTE_JOB_DO_NOT_LAUNCH` is set — mapper
  testing — these flip back to `true` so the mapper has node assignments
  to work with.)

Get this wrong and either the map is bogus (assigning before the RM
placed daemons) or wireup stalls.

---

## Conventions, threading, and gotchas

- **Non-blocking is the law.** `spawn` returns immediately; launch
  completes asynchronously via the daemon callback. Never block the
  progress thread waiting for `ssh`/`srun`/`aprun`. Components that fork
  a launcher process register a `prte_wait_cb` (SIGCHLD) to notice its
  exit rather than waiting on it.
- **State activation, not inline error handling.** On any launch error,
  `PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START)` (or
  `FAILED_TO_LAUNCH`) and let the errmgr/state machine unwind — every
  component's `launch_daemons` funnels errors to a `cleanup:` label that
  does exactly this. Don't try to tear down inline.
- **`map->num_new_daemons == 0` fast-path.** Every component's
  `launch_daemons` must handle "no new daemons" by jumping straight to
  `DAEMONS_LAUNCHED`→`DAEMONS_REPORTED`. This is what makes application
  jobs (which add no daemons) flow through without a launcher.
- **`PRTE_JOB_DO_NOT_LAUNCH`.** Mapper/`--display-map` runs never spawn
  anything — the handler jumps to `DAEMONS_REPORTED` after `setup_vm`.
  Preserve this in any new launcher.
- **Environment hygiene.** Launchers that forward the whole environment
  (SLURM, LSF, PALS) strip `PMIX_`/`PRTE_`-prefixed env vars in the
  child before exec — those are re-supplied on the daemon command line
  and forwarding them can corrupt tool connections.
- **Prefix handling.** Any `--prefix` lives on the *daemon job* object
  (`PRTE_JOB_PREFIX` / `PRTE_JOB_PMIX_PREFIX`); launchers read it there
  and rewrite `PATH`/`LD_LIBRARY_PATH` (and export `PRTE_PREFIX` /
  `PMIX_PREFIX`) so the remote `prted` and its PMIx can be found.
- **The version macro is `PRTE_PLM_BASE_VERSION_2_0_0`** (`plm` 2.0.0).
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG` for unexpected errors.

---

## Debugging

```sh
prte --prtemca plm_base_verbose 5 ...     # trace daemon launch + callbacks
prte --prtemca state_base_verbose 5 ...   # trace the job state transitions
prte --prtemca rml_base_verbose 5 ...     # trace the daemon report-back messages
prte --prtemca plm_ssh_verbose 5 ...      # ssh: shell probe, agent, per-node argv
prun --do-not-launch --display-map ...    # exercise setup_vm/mapping without spawning
prte --debug-daemons ...                  # leave daemon sessions attached, see prted output
```

`plm_base_verbose >= 5` dumps the setup_vm node/daemon accounting, every
`prted_report_launch` callback (with contact URI and node name), and the
progress toward `DAEMONS_REPORTED` — start there when a launch hangs.
A launch that "hangs at DAEMONS_LAUNCHED" almost always means a daemon
failed to connect back (firewall, wrong prefix, missing library) — check
for the `daemon failed to report back` message from `ssh_wait_daemon`.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`ssh/AGENTS.md`](ssh/AGENTS.md) — the default/fallback; **read this
  second** — rsh/ssh tree-spawn is the reference implementation.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — one `srun` launches all prteds.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — the `lsb_launch()` LSF API.
- [`pals/AGENTS.md`](pals/AGENTS.md) — Cray PALS `aprun`.
