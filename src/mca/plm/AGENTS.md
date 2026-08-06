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
  base/                       # ...and its own AGENTS.md - read it before editing any of this
    base.h                    # public base API (state-machine handlers, spawn_response, ...)
    plm_private.h             # framework-internal API + prte_plm_globals_t + prted-cmd helpers
    plm_base_frame.c          # framework open/close/register; the DEFAULT "local-only" module
    plm_base_select.c         # pick-ONE-component selection (highest priority wins)
    plm_base_receive.c        # the HNP command processor: tools/daemons → HNP (PRTE_RML_TAG_PLM)
                              #   ...and the writers for the UPDATE_PROC_STATE body it reads
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
| PLM commands | `LAUNCH_JOB_CMD = 1`, `UPDATE_PROC_STATE = 2`, `REGISTERED_CMD = 3`, `TOOL_ATTACHED_CMD = 4`, `READY_FOR_DEBUG_CMD = 5`, `LOCAL_LAUNCH_COMP_CMD = 6`, `TOOL_DEPARTED_CMD = 7`. |

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
| `slurm` | 75 | a Slurm job id is in the environment and Slurm answers `--version` — both via [`common/slurm`](../common/slurm/AGENTS.md), which probes once for every Slurm component. |
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
| `prte_plm_base_setup_job` | `INIT` → assign a jobid (`prte_plm_base_create_jobid`), record the job as an owner of the reservation(s) it targets (it has a namespace only now — see the command processor below), arm spawn/job timeout timers, then `INIT_COMPLETE`. |
| `prte_plm_base_allocation_complete` | `ALLOCATION_COMPLETE` → `LAUNCH_DAEMONS` (the component's handler). Has a bootstrap-DVM special case that stands up the VM directly. |
| `prte_plm_base_daemons_launched` | `DAEMONS_LAUNCHED` → **deliberately a no-op**; we wait for daemons to phone home rather than advancing. |
| `prte_plm_base_daemons_reported` | `DAEMONS_REPORTED` → size any node whose slot count was not given (`PRTE_NODE_FLAG_SLOTS_GIVEN`), sum the job's session nodes into `total_slots_alloc`, then `VM_READY`. |
| *(VM_READY)* | handled by `state/dvm`'s own `vm_ready()` — WIREUP xcast, elastic grow drain, file prepositioning, then `MAP`. The unregistered copies of this and the `INIT_COMPLETE` handler that used to sit in `plm/base` are gone; edit the live ones. |
| `prte_plm_base_mapping_complete` | `MAP_COMPLETE` → `SYSTEM_PREP`. |
| `prte_plm_base_complete_setup` | `SYSTEM_PREP` → `LAUNCH_APPS`. |
| `prte_plm_base_launch_apps` | `LAUNCH_APPS` → pack the `PRTE_DAEMON_ADD_LOCAL_PROCS` (or `DVM_ADD_PROCS` for a fixed DVM) command plus the `odls` add-procs payload into `jdata->launch_msg`. |
| `prte_plm_base_send_launch_msg` | `SEND_LAUNCH_MSG` → xcast `jdata->launch_msg` to all daemons on `PRTE_RML_TAG_DAEMON`. This is what actually starts application procs. |
| `prte_plm_base_post_launch` | `RUNNING` → cancel spawn timer, wire up IOF, optionally dump the proctable, send the spawn response. |
| `prte_plm_base_registered` | `REGISTERED` → mark the job registered. |

`prte_plm_base_spawn_response()` notifies the original spawn requestor
(tool via PMIx event, or another daemon via `PRTE_RML_TAG_LAUNCH_RESP`)
that the job launched. It is called with a **failure** status too —
`errmgr/dvm` and `state/dvm` both answer a failed job through it, so a
quick-failing job cannot leave its requestor unanswered. The
`PMIX_LAUNCH_COMPLETE` event it raises for a tool-requested spawn is
therefore emitted **only on success**: that event says the job is running
and a tool may read it that way. A failure travels by the two routes that
exist for it — the `PMIX_ERR_JOB_FAILED_TO_LAUNCH` event described below,
and the error status carried by the response itself, which is what
releases the requestor from `PMIx_Spawn`.

It is also **the only chance to tell the requestor *why* a launch failed.**
A response carrying an error status is what releases the requestor from
`PMIx_Spawn`, and it leaves immediately — before the job-end event that
normally carries the diagnostic is ever raised, and without an nspace to
register a handler against. So this function reports the failure ahead of
the response, by whichever route the requestor can actually receive on:

- **`prterun`** is both the DVM and the tool, so the message is rendered
  here and written as the failed job's stderr through
  `PMIx_server_IOF_deliver`.
- **A separate tool** (`prun`) has no IOF sink for the job yet — PMIx does
  not raise one until the spawn reply names the nspace — so a
  `PMIX_ERR_JOB_FAILED_TO_LAUNCH` event is custom-ranged to that one tool.
  `prun_common.c` registers a handler for that **concrete code before it
  spawns**; do not "simplify" it to rely on the default handler, which a
  PMIx server drops when it holds no default entry yet.

**Send the facts, not the prose.** Every one of these messages opens with
`prte_tool_basename`, which is per-process — so a message rendered on the
HNP tells a `prun` user that *`prte`* was unable to launch their
application. The event therefore carries the failing rank, node, executable,
working directory and error code, and the tool composes the sentence itself
with the same `prte_render_launch_failure()` (`src/runtime/prte_quit.c`) the
DVM uses. That helper deliberately takes values rather than
`prte_job_t`/`prte_proc_t`/`prte_app_context_t`, precisely so a tool — which
has none of them — can call it.

Both deliveries use the blocking form on purpose: the write has to be queued
before the response, or the requestor is gone when it arrives. The report is
single-shot (`PRTE_JOB_FLAG_ERR_REPORTED`, claimed here so the later job-end
path cannot repeat it in the DVM's voice), and a job that launched and
*then* failed never reaches this path at all — its response was already sent,
so the early `PRTE_JOB_SPAWN_NOTIFIED` return catches it.

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
   node state `UP`. The daemon-reported aliases must be **merged** into
   `node->aliases`, never assigned over it: the HNP has already recorded
   the allocation's own name (and its non-FQDN form) there, and those
   are exactly the names a hostfile/`--host` spec uses to refer to the
   node — dropping them makes `prte_nptr_match` stop matching it.
4. Unpacks the node **topology** (possibly compressed), de-duplicating
   against `prte_node_topologies` and recording an hwloc diff when it
   matches an existing one. Under `prte_homo_nodes` only daemon rank 1
   sends a topology and everyone else inherits it — `progress_daemons()`
   points every other daemon's node at that same `prte_topology_t`
   (nothing is duplicated; each node takes a **counted reference**, see
   below). It must come from a **daemon**, never from our own node
   (`prterun` may be on a login node with a different topology), and rank
   1 may no longer exist (an elastic shrink can remove it), so the source
   is the first daemon that *has* a topology — the survivors already
   inherited rank 1's, so a daemon added by a later grow (which reports
   none of its own, not being rank 1) still inherits the right one.

   **Diff ownership:** the node also owns whatever
   `hwloc_topology_diff_build()` produced, and `prte_node_destruct` frees
   it. hwloc allocates a diff list *whenever it has anything to say* —
   including the `TOO_COMPLEX` entry it returns **1** with when the two
   topologies genuinely differ, which is the usual outcome while walking
   the recorded topologies looking for a match. Destroy every list you do
   not adopt (a heterogeneous DVM otherwise leaks one per node per recorded
   topology), and destroy the node's previous diff before storing a new
   one, since a daemon can report in twice.

   **Topology ownership:** `node->topology` is a reference-counted
   pointer into the shared `prte_node_topologies` array, never a copy.
   Assigning one means `PMIX_RETAIN(t)` (and releasing whatever the node
   held before); `prte_node_destruct` drops the node's reference. That is
   what lets the topology outlive any individual node — it goes away only
   when the last node using it, and the array entry, are gone — and it is
   what makes a node safe to destroy at any point in an elastic
   shrink/grow.
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

### The `UPDATE_PROC_STATE` writers (`plm_base_receive.c`)

`prte_plm_base_pack_state_for_proc()` and
`prte_plm_base_pack_state_update()` build the body of the report a daemon
sends the HNP: per job, the nspace, then `{rank, pid, state, exit_code}`
per proc, then a `PMIX_RANK_INVALID` terminator. They are deliberately in
the same file as `prte_plm_base_recv()`, which is the only thing that
reads them — the wire carries no format version, so the pair has to
change in one commit, and there used to be three separate copies of the
writer (`errmgr/prted`, `state/prted`, and a hand-rolled one in
`prted_abort()`) for a maintainer to find. `test/unit/plm` round-trips
both shapes through an unpacker that repeats the receiver's sequence.
`skip_reported` is for the normal-termination caller: it packs only
children not yet flagged `PRTE_PROC_FLAG_TERM_REPORTED`, and flags what
it packs.

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

  Two things about this path are worth internalizing. **Every rejection
  must happen before the job is added to `session->jobs`** — that array
  borrows its entries and nothing ever removes one, so a job rejected
  afterwards stays as a phantom for the life of the session. And **the job
  has no namespace here**: the requester packs an unnamed job and the HNP
  names it later, in `prte_plm_base_setup_job`. Anything keyed on the
  job's own identity has to wait for that. The reservation-ownership grant
  did not, and so recorded an *empty* namespace as an owner — which
  `PMIx_Check_nspace` treats as a wildcard matching every namespace,
  retiring the ownership gate on that reservation. The grant now happens
  in `setup_job`, and `prte_session_add_owner` refuses an empty namespace.
- **`PRTE_PLM_UPDATE_PROC_STATE`** — daemons report per-proc pid/state/
  exit-code; the handler activates the corresponding proc state.
- **`PRTE_PLM_REGISTERED_CMD`** — procs registered for sync; advances to
  `REGISTERED` when all report.
- **`PRTE_PLM_READY_FOR_DEBUG_CMD`** — debugger-stop bookkeeping.
- **`PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`** — a daemon reports its local app
  procs launched (pid + state); advances to `STARTED` on the first and
  `RUNNING` when `num_launched == num_procs`.
- **`PRTE_PLM_TOOL_ATTACHED_CMD`** — register a connecting tool as a job.
- **`PRTE_PLM_TOOL_DEPARTED_CMD`** — the partner of the above: a tool the
  master registered has gone. Only the master holds a tool's job object, so
  a daemon the tool connected *through* has nothing local to retire and
  reports the departure instead. The master vets the namespace's
  `PRTE_JOB_FLAG_TOOL` before acting — the reporting daemon can only say
  "this peer was not a client of mine", and a namespace that turns out to be
  an application job must not be terminated on the strength of that. An
  unknown namespace is not an error; the DVM may have discarded it already.

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
  open the PLM if explicitly told to). An env var is split at its
  **first** `=` only: MCA values may themselves contain `=`, and
  splitting on all of them silently truncates the value. Replicating the
  environment can be suppressed entirely via
  `prte_plm_globals.pass_environ_mca_params` (the `ssh` component sets
  it from `plm_ssh_pass_environ_mca_params`, the documented remedy for
  `cmd-line-too-long`).
- **`prte_plm_base_wrap_args(argv)`** — quotes multi-word `...mca`
  argument values so shells/launchers don't split them.

### Building the daemon VM — `prte_plm_base_setup_virtual_machine`

Called first thing in every component's `launch_daemons`. It builds the
**daemon job's map**: the set of nodes that need a *new* daemon. It
handles a menagerie of cases — fixed DVM (nothing to do), extend/grow
DVM, dynamic spawn (only "added" nodes), unmanaged allocation
(`-host`/hostfile union), managed allocation (filter the node pool).

**The daemon job's map is persistent; two of its fields are not.**
`map->num_new_daemons` and `map->daemon_vpid_start` describe the launch
about to happen, not the DVM, so `setup_virtual_machine` clears both once
at the top — before any branch runs — and every branch recomputes them.
That placement is the fix for a real defect: when each branch cleared them
for itself, two of the four forgot, and a second launch that added a node
(`--add-host`, an elastic grow) handed `slurm`/`lsf`/`pals` the *first*
launch's start vpid, telling the new daemons to claim ranks that live
daemons already owned. `ssh` is immune (it substitutes each node's own vpid
per node), which is why it went unnoticed. Do not move the reset back into
the branches.

For each node needing a daemon it:

- creates a `prte_proc_t` and assigns it a vpid — normally the **next
  available one** (`daemons->num_procs`), but in a **bootstrapped DVM**
  the node's canonical rank (`node->index`, recorded by `ras/bootstrap`
  from `prte.conf`). That is not a preference: a bootstrapped daemon
  computed its own vpid from the same file before it ever contacted the
  HNP, and `prted_report_launch` looks a reporting daemon up in
  `daemons->procs` **by the rank it claims for itself**. Assign it a
  different one and the HNP either attaches the daemon to the wrong node
  or cannot find it at all. Records the first as
  `map->daemon_vpid_start` and bumps `map->num_new_daemons`;
- maintains `daemons->num_procs` as the vpid **span** (grown to cover the
  vpid just used, which for a sequential assignment is exactly the
  increment it replaces) — `num_procs` is the count only because vpids
  are normally consecutive, and it is read tree-wide as the upper bound
  of the daemon vpid range;
- links node↔daemon, sets `PRTE_NODE_FLAG_LOC_VERIFIED` iff
  `prte_plm_globals.daemon_nodes_assigned_at_launch` (see below);
- once daemons are added, recomputes the RML routing tree
  (`prte_rml_compute_routing_tree`) so the HNP can tree-spawn/xcast.

A **grow** (`PRTE_JOB_EXTEND_DVM`) is scoped to the nodes that request
brought in, and nothing else: an allocation request naming `node4` starts a
daemon on `node4` alone. Every producer of a grow — the no-scheduler
`ras_base_insert_node_string`, `ras/slurm`'s extend, and `ras/hosts` for
`add-host`/`add-hostfile` — marks its nodes `PRTE_NODE_STATE_ADDED` for
exactly this purpose, and the extend path selects on that mark (as the
dynamic-spawn path does). Do **not** revert it to scanning the whole node
pool for nodes lacking a daemon: after a shrink the pool holds exactly such
a node — the one just removed — and it would be silently dragged back into
an unrelated grow. `ras_base_node_insert` propagates the mark onto a pool
entry that already exists, which is what makes re-growing a previously
shrunk node work at all.

Relatedly, the grow campaign scans **all** its targets for the requestor
rather than reading `map->daemon_vpid_start`: a target whose session is gone
would otherwise leave the campaign with no requester, and a successful grow
would emit no phase-two completion event at all.

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
- **Launch caddies are owned by the SIGCHLD callback.** A component that
  hands a caddy to `prte_wait_cb(child, cb, caddy)` still owns it: the
  `prte_wait_tracker_t` destructor releases only its `child`, never
  `cbdata`. The callback must `PMIX_RELEASE` the caddy on **every** path
  (including the success path), or every launched daemon leaks a copy of
  its full remote command line.
- **A per-launch failure flag must be re-armed per launch.** Components
  funnel errors to a `cleanup:` label guarded by a `failed_launch` flag;
  where that flag is a file-static (because the launcher's `wait_cb`
  also reads it) it must be set `true` at the *top* of `launch_daemons`,
  not merely initialized once — otherwise the cleanup path can never
  report a failure.
- **`map->nodes` entries may have a NULL `daemon`.** Check
  `node->daemon` before reading `node->daemon->name.rank` — including in
  a tree-spawn "is this one of my children?" filter, which runs before
  the per-node launch checks.
- **Do not advertise a knob nothing reads.** `plm_node_regex_threshold`
  was registered and documented for years while the node regex travelled
  in the launch message, not on the command line; `plm_ssh_delay` was
  parsed and never applied. Both are gone. A dead knob is worse than no
  knob — it makes a user's correct diagnosis look wrong. `prte_plm_globals_t`
  is likewise down to the fields something actually reads.
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

## Testing

Launching daemons needs real nodes, so the coverage is split in three:

| Layer | What it covers |
|-------|----------------|
| [`test/unit/plm/test_plm.c`](../../../test/unit/plm/) (`make check`) | Everything the launch path builds *before* it forks: `plm_types.h` state/command-code uniqueness and the UNTERMINATED/TERMINATED/ERROR boundaries, the module vtable contract (default + `ssh`), `prte_plm_base_wrap_args`, `prte_plm_base_setup_prted_cmd` (plain / wrapped / custom agent), the full `prte_plm_base_prted_append_basic_args` command line (vpid template slot, rmaps/ras/plm suppression, `=`-bearing MCA values, the `pass_environ_mca_params` opt-out), `set_hnp_name`/`create_jobid` nspace assignment, the `UPDATE_PROC_STATE` round trip, and **`setup_virtual_machine`'s per-launch accounting** (`test_setup_vm`: each pass reports its own `num_new_daemons`/`daemon_vpid_start`, including the second launch that adds one node and the no-op third pass). It builds the global job/node pools by hand, as `test/unit/ras` does. |
| [`test/offline`](../../../test/offline/) (`make -C test/offline check-offline`) | `setup_virtual_machine` on the `DO_NOT_LAUNCH` path, via the mapper matrix. |
| [`contrib/dockerswarm`](../../../contrib/dockerswarm/) (`run-tests.sh linux`) | The real launch: radix-2 **tree-spawn** fan-out across 8 nodes (only a multi-level tree proves `remote_spawn` recursed), flat `no_tree_spawn` launch, `num_concurrent=1` throttled launch, an `=`-bearing MCA value surviving onto the prted cmd line, `pass_environ_mca_params 0`, node-name/alias reconciliation (allocate by IP, then `--host` by both the reported name and the original address), and a **second launch into a running DVM** (`--add-host`) adding exactly the one daemon it needs. |

Add a new pure/structural check to the unit test; anything that needs a
daemon to actually come up belongs in the dockerswarm suite.

Note what the swarm **cannot** reach: it has only `ssh`, so anything
specific to an RM launcher — `daemon_vpid_start`'s only consumers among
them — is covered by the unit test and, ultimately, by a real allocation.
There is no fake `srun`; `contrib/dockerswarm/fake-slurm.py` stands in for
the SLURM *control plane* (`ras/slurm`), not for its launcher.

---

## Where to go next

The base and each component directory have their own `AGENTS.md`:

- [`base/AGENTS.md`](base/AGENTS.md) — the launch machinery itself:
  `setup_virtual_machine`'s branches and its per-launch reset, the daemon
  callback's ownership rules, the command processor's two invariants, and
  what `prte_plm_globals_t` may hold.
- [`ssh/AGENTS.md`](ssh/AGENTS.md) — the default/fallback; **read this
  second** — rsh/ssh tree-spawn is the reference implementation.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — one `srun` launches all prteds.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — the `lsb_launch()` LSF API.
- [`pals/AGENTS.md`](pals/AGENTS.md) — Cray PALS `aprun`.
