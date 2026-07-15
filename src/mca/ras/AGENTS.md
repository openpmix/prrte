# AGENTS.md — The `ras` Framework (Resource Allocation Subsystem)

Orientation for AI agents and human contributors working in
`src/mca/ras/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`ras` (Resource Allocation Subsystem) answers one question at DVM/job
startup: **which nodes, and how many slots on each, has this DVM been
given to work with?** It runs only on the HNP (DVM master) — non-HNP
procs are never allowed to allocate resources, which is why the
framework needs no proxy (see the note in `base/ras_base_frame.c`). It
is the first substantive step of the job-launch state machine:

```
PRTE_JOB_STATE_INIT → ALLOCATE → MAP → LAUNCH_DAEMONS → RUNNING → TERMINATED
                        ▲
                        └── ras runs here
```

The `state` framework fires `prte_ras_base_allocate()` when a job enters
`PRTE_JOB_STATE_ALLOCATE`. On success the job advances to
`PRTE_JOB_STATE_ALLOCATION_COMPLETE`; every failure path advances it to
`PRTE_JOB_STATE_ALLOC_FAILED`. The product of a successful allocation is
a populated **global node pool** (`prte_node_pool`, a
`pmix_pointer_array` of `prte_node_t`) — one entry per allocated node,
each carrying `slots`, `slots_max`, `slots_inuse`, a `state`, and flags
such as `PRTE_NODE_FLAG_SLOTS_GIVEN`. `prte_ras_base.total_slots_alloc`
holds the sum, and it is copied into `jdata->total_slots_alloc`. The
mapper (`rmaps`) consumes this pool at the next state.

Two situations the framework must serve (see the block comment atop
`ras.h`):

1. **Managed allocation** — the user already asked a scheduler (SLURM,
   PBS, LSF, …) for nodes before running `prte`/`prun`; the allocation
   arrives via environment variables. The right component recognizes its
   RM's env and reads the nodelist.
2. **Unmanaged allocation** — no scheduler; nodes come from
   `--host`/`--hostfile`/default-hostfile, or from nothing (fall back to
   the local host as a 1-slot node).

The framework also owns **dynamic allocation changes** at runtime
(reservations, add-host, elastic grow/shrink) via the `modify` path —
see below.

---

## Directory layout

```
ras/
  ras.h                    # module/component vtable + PRTE_RAS_BASE_VERSION_2_0_0
  base/
    base.h                 # framework-global struct (prte_ras_base) + all base API prototypes
    ras_base_frame.c       # framework open/close/register; ras_base MCA params; globals
    ras_base_select.c      # priority-ordered component selection (keeps ALL, like rmaps)
    ras_base_allocate.c    # THE driver + node-pool display + modify/reservation machinery
    ras_base_node.c        # prte_ras_base_node_insert: dedup nodes into the global pool
    ras_base_close.c       # STALE/uncompiled — not in base/Makefile.am; ignore it
    help-ras-base.txt      # user-facing error text
  slurm/                   # SLURM (SLURM_NODELIST) — pri 50; full elastic modify support
  pbs/                     # PBS/Torque/Cobalt (PBS_NODEFILE) — pri 100 (param)
  lsf/                     # LSF (lsb_getalloc) — pri 75
  gridengine/              # SGE/Grid Engine (PE_HOSTFILE) — pri 100 (param)
  flux/                    # Flux (resource.R via KVS) — pri 100 (param); optional build
  pmix/                    # query allocation from a host PMIx scheduler — pri 20
  hosts/                   # dash-host / hostfile / default localhost — pri 1 (catch-all)
  bootstrap/               # launcher-less bootstrap DVM node set — pri 20 (gated)
  simulator/               # fabricate a synthetic allocation for testing — pri 1000 (gated)
  testrm/                  # fake RM: read a fixed hostfile — pri 1000 (gated)
```

Read `ras.h` for the vtable, then `base/ras_base_allocate.c` (the
driver) and `base/ras_base_node.c` (where nodes actually land in the
pool). Then read `hosts/` (the default) and one RM component (`slurm/`).

---

## The module contract

Every component exposes a `prte_ras_base_module_t`
(`struct prte_ras_base_module_2_0_0_t` in `ras.h`) with up to six entry
points; only `allocate` is universally implemented:

```c
typedef int (*prte_ras_base_module_init_fn_t)(void);
typedef int (*prte_ras_base_module_allocate_fn_t)(prte_job_t *jdata, pmix_list_t *nodes);
typedef pmix_status_t (*prte_ras_base_module_modify_fn_t)(prte_pmix_server_req_t *req);
typedef void (*prte_ras_base_module_shrink_complete_fn_t)(prte_shrink_campaign_t *campaign);
typedef int (*prte_ras_base_module_release_fn_t)(prte_session_t *session);
typedef int (*prte_ras_base_module_finalize_fn_t)(void);
```

| vtable slot | Meaning |
|-------------|---------|
| `init` | Optional one-time setup at selection (SLURM allocates its session stack here; most components leave it NULL). |
| `allocate` | Discover nodes and **append `prte_node_t` objects to `nodes`** (a `pmix_list_t`). Does *not* touch the global pool — the base does that. |
| `modify` | Serve a runtime allocation change (`PMIX_ALLOC_NEW`/`EXTEND`/`RELEASE`/`REQ_CANCEL`) carried in a `prte_pmix_server_req_t`. |
| `shrink_complete` | Called once an elastic shrink campaign has drained, so the RM can hand freed nodes back to the scheduler. |
| `release_allocation` | Called when a `prte_session_t` is destructed, so the RM can release that session's allocation. |
| `finalize` | Teardown at framework close. |

### The `allocate()` return protocol

The driver (`prte_ras_base_allocate`) walks the selected modules in
priority order and interprets the return code — it is a protocol, not
just success/failure:

| Return | Meaning in the driver |
|--------|----------------------|
| `PRTE_SUCCESS` | Got an allocation; **stop** cycling modules and insert the nodes. |
| `PRTE_ERR_TAKE_NEXT_OPTION` | "Not my environment / nothing to contribute" — try the next module. **Not an error.** |
| `PRTE_ERR_ALLOCATION_PENDING` | An async allocation request is underway; do nothing and return (a later event resumes). |
| `PRTE_EXISTS` | A fixed allocation was already discovered (e.g. SLURM sees the jobid's session already exists); skip insertion, jump to display. |
| anything else | A real error: `PRTE_ERROR_LOG` it and activate `PRTE_JOB_STATE_ALLOC_FAILED`. |

Because of `TAKE_NEXT_OPTION`, **the first thing a component does is
decide whether the job is for it**. RM components gate on their env var
and bail with `TAKE_NEXT_OPTION` (or a hard error) when absent.

The `modify()` path has its own protocol (`prte_ras_base_modify`):
`PMIX_SUCCESS`/`OPERATION_IN_PROGRESS` → module owns the callback;
`PMIX_OPERATION_SUCCEEDED` → done atomically, base completes it;
`PMIX_ERR_TAKE_NEXT_OPTION`/`PMIX_ERR_NOT_SUPPORTED` → try next module.
`modify` requests can be keyed to a single component via `req->key`
(matched case-insensitively against the component name).

---

## Component selection is not "pick one"

`prte_ras_base_select()` (in `ras_base_select.c`) works like the `rmaps`
selector, not the usual single-winner MCA pattern: it queries every
component, runs each returned module's `init`, and stores **all** of
them **priority-sorted** in `prte_ras_base.selected_modules`
(a `pmix_list_t` of `prte_ras_base_selected_module_t`, each holding
`pri`, `module`, `component`). The driver then walks that list at
allocate time. With `ras_base_verbose > 4` the selector prints the final
prioritized list.

Query priorities (higher wins first):

```
simulator 1000  =  testrm 1000  >  pbs 100  =  gridengine 100  =  flux 100
   >  lsf 75  >  slurm 50  >  bootstrap 20  =  pmix 20  >  hosts 1
```

`hosts` is the catch-all default at priority **1** — it is always
available and is tried last, handling `--host`/`--hostfile`/default
hostfile and (via the base) the ultimate fall-back to a 1-slot local
node. The RM components make themselves available only when their
environment is detected (e.g. `slurm` requires `SLURM_JOBID`), so on any
given machine at most one RM answers, then `hosts` closes out the list.
`simulator`/`testrm` sit at 1000 so that when explicitly configured they
pre-empt everything.

---

## `prte_ras_base_allocate()` — the driver

This state callback in `ras_base_allocate.c` is the heart of the
framework. Its phases:

1. **Reuse guard.** In an *unmanaged* allocation, the DVM's initial
   (daemon-job) discovery is the fixed base allocation for the whole
   session. If `prte_ras_base.allocation_established` is set and this is
   **not** the DVM's own daemon job, the base *reuses* the existing pool
   and jumps to display — re-running discovery would re-read the default
   hostfile, overwrite established per-node slot counts, and clear
   `PRTE_NODE_FLAG_SLOTS_GIVEN`, hiding genuine oversubscription from the
   mapper. (The sanctioned way to grow an unmanaged allocation is
   add-host/add-hostfile → `prte_ras_base_modify`.)
2. **Cycle modules.** Walk `selected_modules` in priority order calling
   `mod->module->allocate(jdata, &nodes)`, honoring the return protocol
   above.
3. **Empty-list handling.** If no module contributed and
   `prte_allocation_required` is set → fatal (`ras-base:no-allocation`).
   Otherwise fabricate a single node from `prte_process_info.nodename`
   with `slots = 1`, `state = PRTE_NODE_STATE_UP`, and set
   `prte_hnp_is_allocated`.
4. **Insert.** `prte_ras_base_node_insert(&nodes, jdata)` drains the
   list into the global pool (see below).
5. **Establish + display.** Set `allocation_established = true`; if
   `ras_base_verbose > 4`, dump the allocation
   (`prte_ras_base_display_alloc`). Honor `PRTE_JOB_DISPLAY_TOPO`.
6. **Report + advance.** Copy `total_slots_alloc` into the job, fire the
   `PMIX_NOTIFY_ALLOC_COMPLETE` event if `prte_report_events`, then
   activate `PRTE_JOB_STATE_ALLOCATION_COMPLETE`.

---

## `prte_ras_base_node_insert()` — nodes into the global pool

`ras_base_node.c` is where a component's working list becomes the
authoritative `prte_node_pool`. **NOTE: it removes every item from the
input list.** Walk it carefully before touching allocation code:

- **Multiplier.** `ras_base_multiplier` (default 1) fabricates N copies
  of every node via `prte_node_copy`, to simulate a large cluster from a
  small one; it also stamps `PRTE_JOB_MULTI_DAEMON_SIM` on the job.
- **HNP dedup.** The HNP's own node is already at pool index 0. Any
  incoming node that `prte_check_host_is_local()` matches updates that
  entry in place (slots, `slots_max`, aliases, `rawname`, attributes)
  instead of adding a duplicate, and sets `prte_hnp_is_allocated`.
  `launch_orted_on_hn` + `NO_USE_LOCAL` handling can instead rename the
  HNP node to `"prte"`, flag it `PRTE_NODE_NON_USABLE`, and skip the
  dedup so the head node is left out of mapping.
- **General dedup.** For every other node, an exhaustive `prte_nptr_match`
  scan of the pool decides add-vs-update. `PRTE_NODE_ADD_SLOTS` means
  "adjust the existing slot count" (clamped to `[0, slots_max]`) rather
  than replace it.
- **Managed = sacred slots.** Under `prte_managed_allocation` (or when a
  node arrives with `PRTE_NODE_FLAG_SLOTS_GIVEN`), the base sets
  `SLOTS_GIVEN` so downstream code treats the slot count as fixed and
  never recomputes it from core count.
- **FQDN normalization.** `normalize_node()` truncates an FQDN to the
  short name, keeping the full name as `rawname` and an alias (unless
  `prte_keep_fqdn_hostnames` or the name is an IP). Sets
  `prte_have_fqdn_allocation`.
- **`DO_NOT_LAUNCH` daemons.** When the daemon job carries
  `PRTE_JOB_DO_NOT_LAUNCH` (offline mapper tests), a synthetic
  `prte_proc_t` daemon is attached to each node so the mapper sees a
  daemon without a live launch.
- Bumps `prte_ras_base.total_slots_alloc` by each node's slots.

---

## `--display-allocation` and cpus

- `prte_ras_base_display_alloc(jdata)` prints the whole `prte_node_pool`
  (name, `slots`, `max_slots`, `slots_inuse`, `state`, flags, aliases),
  in plain or `<allocation>` XML form (`PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT`).
  Guarded by `PRTE_JOB_ALLOC_DISPLAYED` so it prints once.
- `prte_ras_base_flag_string(node)` renders the node flag bitmask
  (`DAEMON_LAUNCHED`, `LOCATION_VERIFIED`, `OVERSUBSCRIBED`, `MAPPED`,
  `SLOTS_GIVEN`, `NONUSABLE`) for that display.
- `prte_ras_base_display_cpus(jdata, nodelist)` prints available
  processors per package for the requested nodes (`--display-cpus`).

---

## Dynamic allocation: modify, reservations, elastic grow/shrink

The bulk of `ras_base_allocate.c` beyond the driver serves runtime
allocation changes. Understand these entry points before touching
elastic-DVM or PMIx_Allocation code:

| Base function | Role |
|---------------|------|
| `prte_ras_base_modify()` | The `modify` driver (also a state callback). Cycles modules (keyed by `req->key`) to serve a `prte_pmix_server_req_t`; on `PMIX_SUCCESS` calls `prte_ras_base_complete_request`, then invokes the requester's `infocbfunc`. |
| `prte_ras_base_add_hosts()` | Collect `PRTE_APP_ADD_HOSTFILE`/`PRTE_APP_ADD_HOST` directives across apps into a `PMIX_ALLOC_EXTEND` request and hand it to `prte_ras_base_modify`. Sets `prte_dvm_ready = false` until processed. |
| `prte_ras_base_complete_request()` | The heavy reservation router. For `PMIX_ALLOC_NEW`/`EXTEND` it resolves the destination `prte_session_t` (honoring `PMIX_ALLOC_TARGET`/`SHARE`/`INHERITANCE`/`ID`/`REQ_ID`), parses `PMIX_ALLOC_NODE_LIST`, inserts the nodes, and attaches them to the reservation (`add_nodes_to_session`). For `PMIX_ALLOC_RELEASE` it tears down a named reservation or xcasts a `PRTE_DAEMON_SHRINK_CMD`. Marks `PRTE_JOB_EXTEND_DVM` and re-launches daemons for grows. |
| `prte_ras_base_release_allocation()` | Session-destruct hook: cycles modules whose `release_allocation` matches `session->alloc_module`. |
| `prte_ras_base_shrink_complete()` | Offers a drained `prte_shrink_campaign_t` to every module's `shrink_complete`. |
| `prte_ras_base_teardown_reservation()` | Drop a reservation's hold on its nodes (clear `node->session` back to the default pool), deregister it, and — if `return_to_scheduler` — shrink its daemon-carrying nodes out of the DVM. |
| `prte_ras_base_check_reservations_on_term()` | On namespace termination, fire each reservation's inheritance disposition (`PMIX_ALLOC_INHERIT_NONE`/`CHILD`/`CHILD_DEFAULT`/`DEFAULT`). |

Elastic shrink is a two-phase collective: `PMIX_ALLOC_RELEASE` records a
`prte_shrink_campaign_t`, xcasts the shrink command with a completion
callback (`shrink_xcast_complete` → thread-shifted
`shrink_campaign_complete`), which does a single batch routing-tree
repair and per-target HNP teardown. This only engages under
`prte_elastic_mode`; outside it the release is fire-and-forget.

---

## Sessions, pools, and the SLURM node→session deviation

A `prte_node_t` carries a `session` backpointer. **`session == NULL`
means the node belongs to the default (unreserved) pool**; a non-NULL,
non-default session means the node is *reserved* and withheld from
general use. The mapper filters nodes by session, so this is how
reservation and elastic-DVM node pools keep jobs on their intended
nodes.

A subtle but deliberate deviation lives in `slurm/ras_slurm_module.c`
(`prte_ras_slurm_assign_new_session`): the SLURM RAS migration creates a
`prte_session_t` per SLURM jobid **to track the allocation group**, and
retains a reference to each real (pool-bound, daemon-carrying) node in
that session — but it **intentionally leaves `node->session == NULL`**.
Those nodes form the DVM's *startup/default* session and must stay in the
general pool; the session object is only a tracking handle for later
identification/release, not a reservation. Do not "fix" this by setting
`node->session` — it would withhold the whole base allocation from
mapping. (This is captured in repo memory: *Node-reservation SLURM
deviation*.)

---

## Globals and MCA params

`prte_ras_base` (`base.h`, defined in `ras_base_frame.c`):

| Field | Meaning |
|-------|---------|
| `selected_modules` | Priority-sorted list of selected components. |
| `total_slots_alloc` | Sum of `slots` across the pool. |
| `multiplier` | `ras_base_multiplier` — fabricate N daemons/node to simulate scale (default 1). |
| `launch_orted_on_hn` | `ras_base_launch_orted_on_hn` — run a daemon on the head node. |
| `simulated` | Set when the simulator is in play. |
| `allocation_established` | Latched true once the first allocation completes; drives the reuse guard. |

MCA params: `prte_ras_base_multiplier`,
`prte_ras_base_launch_orted_on_hn` (framework), plus per-component params
documented in each component's guide.

---

## Conventions and gotchas

- **`allocate` appends to the list; the base owns the pool.** Never add
  to `prte_node_pool` from a component — build `prte_node_t`s and append
  to the `nodes` list; `node_insert` places them.
- **Slot conventions.** `slots` = usable slots; `slots = -1` (in the
  `hosts` hostfile parser) is a marker for "not specified, compute from
  topology later". Set `PRTE_NODE_FLAG_SLOTS_GIVEN` only when the count
  is authoritative. `slots_max = 0` means "no max".
- **Return `TAKE_NEXT_OPTION`, not an error, when the env is absent** —
  it is what lets `hosts` (and the local-node fallback) run.
- **The reuse guard is load-bearing** for spawn/child jobs in unmanaged
  allocations — don't bypass it.
- **`ras_base_close.c` is dead** (references `active_module`/`ras_opened`
  that no longer exist and is not in `base/Makefile.am`). Don't wire it
  back in or copy its shape.
- Standard PRRTE rules apply: `prte_config.h` first, braces on every
  block, `NULL ==`/constant-on-left, no new warnings,
  `PRTE_ERROR_LOG`/`PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED)`
  on failure.

---

## Debugging

```sh
prte --prtemca ras_base_verbose 5 ...     # trace selection + allocation + node_insert
prun --display-allocation ...              # print the resulting node pool
prte --prtemca ras_base_multiplier 8 ...   # fake an 8x-larger cluster
```

Verbosity ≥5 prints the final component priority list, each module's
allocate attempt, and every node inserted with its slot count — start
there.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`hosts/AGENTS.md`](hosts/AGENTS.md) — the default; read this second.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — SLURM, with full elastic modify.
- [`pbs/AGENTS.md`](pbs/AGENTS.md) — PBS/Torque/Cobalt nodefile.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — LSF `lsb_getalloc`.
- [`gridengine/AGENTS.md`](gridengine/AGENTS.md) — SGE `PE_HOSTFILE`.
- [`flux/AGENTS.md`](flux/AGENTS.md) — Flux `resource.R`.
- [`pmix/AGENTS.md`](pmix/AGENTS.md) — query a host PMIx scheduler.
- [`bootstrap/AGENTS.md`](bootstrap/AGENTS.md) — launcher-less bootstrap DVM.
- [`simulator/AGENTS.md`](simulator/AGENTS.md) — synthetic allocation for testing.
- [`testrm/AGENTS.md`](testrm/AGENTS.md) — fixed-hostfile fake RM.
</content>
</invoke>
