# AGENTS.md — The `rmaps` Framework (Resource Mapping)

Orientation for AI agents and human contributors working in
`src/mca/rmaps/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`rmaps` (Resource MAPping Subsystem) answers one question for every job:
**which process (rank) runs on which node, against which hardware object,
and how is it bound to CPUs?** It runs only on the HNP (DVM master), as
one step in the job-launch state machine:

```
PRTE_JOB_STATE_INIT → ALLOCATE → MAP → LAUNCH_DAEMONS → RUNNING → TERMINATED
                                  ▲
                                  └── rmaps runs here
```

The `state` framework fires `prte_rmaps_base_map_job()` when a job
enters `PRTE_JOB_STATE_MAP`. When mapping succeeds the job is advanced
to `PRTE_JOB_STATE_MAP_COMPLETE`; every failure path advances it to
`PRTE_JOB_STATE_MAP_FAILED`. The result of a successful map is a fully
populated `jdata->map` (`prte_job_map_t`): a list of participating
`prte_node_t`s, each carrying the `prte_proc_t`s assigned to it, with
every proc given a vpid (global rank), node rank, local rank, app rank,
locale (`proc->obj`), and — if binding is in force — a `proc->cpuset`
string. The daemons never re-run the mapper; the HNP ships them the
computed placement.

Three concepts govern the whole framework, and they are **orthogonal**:

| Concept | Question | Set via | Stored in |
|---------|----------|---------|-----------|
| **Mapping** | Which object/node does each proc land on, and in what order are procs laid down? | `--map-by` | `jdata->map->mapping` |
| **Ranking** | What vpid (global rank) does each mapped proc get? | `--rank-by` | `jdata->map->ranking` |
| **Binding** | Which CPUs is each proc restricted to? | `--bind-to` | `jdata->map->binding` |

`--map-by slot`/`node`/`core`/`l3cache`/`numa`/`package`/`hwthread`/
`seq`/`ppr`/`rankfile`/`pe-list=…`/`dist` plus colon modifiers
(`PE=n`, `SPAN`, `OVERSUBSCRIBE`, `NOLOCAL`, `HWTCPUS`, `INHERIT`,
`ORDERED`, `FILE=…`, …). `--rank-by slot`/`node`/`fill`/`span`.

---

## Directory layout

```
rmaps/
  rmaps.h                     # module/component vtable: the single map_job fn ptr
  rmaps_types.h               # prte_job_map_t, prte_rmaps_options_t, all policy #defines & macros
  base/
    base.h                    # framework-global struct + all base API prototypes
    rmaps_private.h           # base API used only by components (target-nodes, setup-proc, vpids, bind)
    rmaps_base_frame.c        # framework open/close/register; --map-by/--rank-by string parsers
    rmaps_base_select.c       # priority-ordered component selection (keeps ALL, does not pick one)
    rmaps_base_map_job.c      # THE orchestrator — policy resolution, per-app dispatch, colocation
    rmaps_base_support_fns.c  # get_target_nodes, setup_proc, check_avail, oversubscribe, cpuset
    rmaps_base_ranking.c      # compute_vpids: by-slot/node/fill/span vpid assignment
    rmaps_base_binding.c      # bind_proc/bind_generic/bind_multiple/bind_to_cpuset
    rmaps_base_print_fns.c    # policy → human string
    help-*.txt                # user-facing error/help text
  round_robin/                # DEFAULT mapper (pri 10): by-slot/node/object/pe-list
  ppr/                        # pattern mapper (pri 90): N procs per resource type
  seq/                        # sequential mapper (pri 60): one proc per hostfile line
  rank_file/                  # explicit rankfile mapper (pri 95): rank→host+cpuset
  lsf/                        # LSF-affinity mapper (pri 100): reads LSB_AFFINITY_HOSTFILE
```

Read `rmaps_types.h` first — it defines `prte_rmaps_options_t` (the
scratch struct threaded through the entire map) and every policy bit
macro. Then read `rmaps_base_map_job.c`, which is where control actually
lives.

---

## The module contract

A mapper is astonishingly thin on the surface. Every component exposes a
single function through `rmaps.h`:

```c
typedef int (*prte_rmaps_base_module_map_fn_t)(prte_job_t *jdata,
                                               prte_rmaps_options_t *options);
```

The return value is a protocol, not just success/failure:

| Return | Meaning |
|--------|---------|
| `PRTE_SUCCESS` | I mapped this job; stop trying other mappers. |
| `PRTE_ERR_TAKE_NEXT_OPTION` | Not my kind of job — try the next mapper. **Not an error.** |
| `PRTE_ERR_RESOURCE_BUSY` | Mapped, but no free resources right now (dynamic spawn). |
| anything else | A real error; the base fails the whole map. |

The base cycles the selected mappers in priority order until one returns
`PRTE_SUCCESS` (or `RESOURCE_BUSY`). Because of this, **the first thing
every mapper does is decide whether the job is for it** and bail with
`PRTE_ERR_TAKE_NEXT_OPTION` if not (wrong `req_mapper`, wrong mapping
policy, a restart it can't handle, …). See any component's guide for its
exact gate conditions.

---

## Component selection is not "pick one"

`prte_rmaps_base_select()` (in `rmaps_base_select.c`) is unlike most MCA
frameworks: it does **not** select a single winning module. It queries
every component, keeps *all* that return a module, and stores them
**priority-sorted** in `prte_rmaps_base.selected_modules`. The actual
"selection" happens per-job at map time by walking that list and letting
each mapper accept or defer. Current priorities:

```
lsf 100  >  rank_file 95  >  ppr 90  >  seq 60  >  round_robin 10
```

So round_robin is the catch-all default (lowest priority, accepts the
generic slot/node/object policies), and the specialized mappers sit
above it and grab the jobs that match their niche.

---

## `prte_rmaps_base_map_job()` — the orchestrator

This ~950-line function in `rmaps_base_map_job.c` is the heart of the
framework. Understand its phases before touching anything:

1. **Setup & special cases.** Init `options`, validate the job has a
   schizo personality, create `jdata->map` if absent. Detect
   **colocation** (`PRTE_JOB_DEBUG_DAEMONS_PER_*`, `PRTE_JOB_COLOCATE_*`)
   — these bypass the mappers entirely via `map_colocate()`.

2. **Inheritance.** For dynamic spawns, decide whether the child job
   inherits the parent's mapping/ranking/binding/ppr/pes/cpu-type/env
   directives (`PRTE_JOB_INHERIT`/`NOINHERIT`, the `prte_rmaps_base.inherit`
   default). Initial launches inherit the MCA-param defaults.

3. **Default policy resolution.** If the user did not pin a mapping
   policy, derive one (`prte_rmaps_base_set_default_mapping()`, or the
   schizo's override): 1 cpu/rank + ≤2 procs → by-core/hwthread; more →
   by-package/numa; multiple cpus/rank → by-slot; a binding policy
   without a mapping policy → map by the binding object. Then resolve
   default ranking and binding the same way.

4. **Proc counts.** For mappers that don't compute their own counts,
   sum `app->num_procs` (calling `get_target_nodes` to learn slot counts
   when an app gave no explicit `-n`).

5. **Policy → hwloc object.** Translate the bare mapping policy into
   `options.maptype`/`options.mapdepth`/`options.hwb` (the hwloc object
   type to map and bind against), with sanity checks (can't bind above
   where you mapped; by-core with pe>1 needs hwthreads; etc.).

6. **Dispatch.** Three mutually exclusive paths:
   - **colocation** → `map_colocate()`.
   - **no per-app policies** → walk `selected_modules`, first mapper to
     accept wins; that mapper also computes vpids for the whole job.
   - **per-app (MPMD) policies present** (any app set `PRTE_APP_MAPBY`/
     `RANKBY`/`BINDTO`) → loop over apps, and for each app build a
     private `app_options` via `prte_rmaps_base_resolve_app_options()`,
     run the mappers for that single app (`options.app_idx == n`), then
     rank just that app's procs with a running `next_vpid` cursor.

7. **Finish.** Bump `prte_total_procs`, honor `--display-map`/
   `--report-bindings`, and activate `MAP_COMPLETE`.

**The `cleanup:` label matters.** Because job-state activation is
asynchronous, the function cannot read `jdata->state` to tell success
from failure — it relies on the `map_succeeded` local. Any new early-out
must go through `cleanup` and ensure a non-zero `jdata->exit_code` on
failure, or a failed map silently reports success.

---

## `prte_rmaps_options_t` — the scratch struct

Defined in `rmaps_types.h`, one instance lives on
`prte_rmaps_base_map_job`'s stack (zeroed at entry, `app_idx = -1`) and
is passed by pointer through *everything*. It carries three groups of
state: resolved input policy (`map`, `rank`, `bind`, `maptype`,
`mapdepth`, `hwb`, `cpus_per_rank`, `use_hwthreads`, `pprn`,
`oversubscribe`, `overload`, `ordered`, …), and per-node working scratch
the support functions fill in and mappers consume (`ncpus`, `nprocs`,
`target` cpuset, `job_cpuset`, `obj`, `nnodes`, `nobjs`). Two fields are
load-bearing for the per-app path:

- `app_idx` — `-1` means "map all apps" (classic whole-job dispatch);
  `>= 0` means "map only `jdata->apps[app_idx]`" (per-app/MPMD dispatch).
  Every mapper's app loop honors this, and every mapper skips its own
  `compute_vpids` call when `app_idx >= 0` (the base does cross-app
  ranking instead, so ranks don't collide between apps).
- `nprocs` — reused as a per-node "how many to place here" counter by the
  support functions; do not assume it still holds the job total.

`options.target` and `options.job_cpuset` are hwloc bitmaps the mappers
must free between nodes; leaks here are the classic rmaps bug.

---

## The base helpers every mapper leans on

These live in `rmaps_base_support_fns.c` / `rmaps_base_binding.c` /
`rmaps_base_ranking.c` and are declared in `base.h` + `rmaps_private.h`.
A mapper is mostly glue around them:

| Helper | Role |
|--------|------|
| `prte_rmaps_base_get_target_nodes()` | Build the usable node list for an app: honor `-host`/`-hostfile`, filter by the job's target session(s), drop down/excluded/no-daemon/full nodes, compute total available slots, apply the bookmark starting point. |
| `prte_rmaps_base_get_cpuset()` | Compute `options->job_cpuset` for a node (the job's allowed CPUs, or a `pe-list`-generated set). |
| `prte_rmaps_base_get_ncpus()` | How many usable cpus/cores an object (or whole node) offers under the current cpu-type. |
| `prte_rmaps_base_check_avail()` | Can this node/object take more procs? Also adds the node to `jdata->map->nodes` exactly once and sets `options->target`. |
| `prte_rmaps_base_check_oversubscribed()` | After placing a proc, flag/deny oversubscription per the node's SLOTS_GIVEN and the job's OVERSUBSCRIBE directive. |
| `prte_rmaps_base_setup_proc()` | Create the `prte_proc_t`, attach it to the node, assign node rank, bump `slots_inuse`, and **bind it** (`prte_rmaps_base_bind_proc`). |
| `prte_rmaps_base_compute_vpids()` | Assign global ranks by slot/node/fill/span, then derive local & app ranks. |
| `prte_rmaps_base_bind_proc()` | Bind a proc: dispatch to `bind_generic` / `bind_multiple` (pe>1) / `bind_to_cpuset` (pe-list), or no-op for by-user/bind-none. |

The universal mapper loop is therefore: for each app → `get_target_nodes`
→ for each node → `get_cpuset`, `check_support`, `get_ncpus`,
`check_avail`, then `setup_proc` (which binds) and `check_oversubscribed`
per proc → finally `compute_vpids` (only when `app_idx < 0`).

---

## Ranking (vpid assignment)

`prte_rmaps_base_compute_vpids()` runs *after* placement and only assigns
the integer ranks; it does not move procs. The four schemes differ in
traversal order (ASCII diagrams are in the source):

- **by-slot** — fill each node completely before the next (front-loaded).
- **by-node** — one rank per node, round-robin across nodes.
- **by-fill** — fill each hwloc object completely before the next object.
- **by-span** — one rank per object, cycling objects across all nodes.

`fill` and `span` require mapping by an actual hwloc object (numa…hwthread);
the base rejects them otherwise. `by-user` mappers (rankfile, seq, lsf)
set the rank themselves and pass `userranked`, so `compute_vpids` only
back-fills local/app ranks.

---

## Binding

`rmaps_base_binding.c` binds each proc as it is set up. `bind_generic`
walks the candidate hwloc objects of type `options->hwb`, intersects
their cpusets with what's still available on the node, and picks the
first with free cpus (honoring an optional per-object `limit`). If none
is free it either errors (required binding) or, when `overload` is
allowed, round-robins onto the least-loaded object without consuming
`node->available` (so a non-overloading later job still sees the node as
full). `bind_multiple` handles `cpus_per_rank > 1`; `bind_to_cpuset`
handles `pe-list` and soft-cgroup cases. Binding writes `proc->cpuset`
(a hwloc bitmap string). Rankfile/LSF/seq compute the cpuset directly
from their slot lists and skip `bind_generic`.

---

## Session targeting (elastic / multi-pool DVMs)

`get_target_nodes` filters nodes by session: a job maps only onto nodes
whose owning session is in the job's target set (`jdata->target_sessions`,
else `jdata->session`). A node with `session == NULL` belongs to the
default (unreserved) pool. This is how reservation and elastic-DVM node
pools keep jobs on their intended nodes — be careful not to regress it
when touching node iteration. See the repo memory on the SLURM RAS
node→session deviation.

---

## Conventions specific to this framework

- **Policy bits are packed.** A `prte_mapping_policy_t` holds the policy
  in the low byte and directive flags (SPAN, NO_OVERSUBSCRIBE, GIVEN, …)
  in the high byte. Always use the `PRTE_GET/SET/UNSET_MAPPING_*` and
  `PRTE_*_RANKING_*` macros — never bit-twiddle by hand. `_IS_SET` tests
  whether the user actually specified a policy vs. a derived default,
  which changes error-vs-fallback behavior all over the code.
- **`GIVEN` vs. derived.** Huge amounts of logic hinge on whether a
  policy was user-specified (`..._GIVEN` / `..._POLICY_IS_SET`) or a
  default the framework picked. Preserve that distinction.
- **`initial_map`.** Each mapper computes `initial_map = (0 == jdata->map->num_nodes)`
  so it only clears per-node MAPPED flags on the true first pass —
  critical because per-app dispatch enters the mapper once per app.
- **Mappers accept then defer.** Keep the "is this job mine?" gate at the
  very top and return `PRTE_ERR_TAKE_NEXT_OPTION` (never a hard error)
  when it isn't.
- **The version macro is `PRTE_RMAPS_BASE_VERSION_5_0_0`.** The `4_0_0`
  alias is deliberately redefined to `5_0_0` so stale out-of-tree
  components fail loudly instead of silently violating ABI.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG`/`PRTE_ACTIVATE_JOB_STATE` for errors.

---

## Debugging

```sh
prte --prtemca rmaps_base_verbose 5 ...   # trace mapping decisions
prun --display-map ...                     # print the computed placement
prun --display-devel-map ...               # + binding detail
prun --report-bindings ...                 # per-proc cpuset report
prun --display-allocation ...              # what get_target_nodes saw
```

Verbosity ≥5 in the rmaps framework output dumps the selected-module
priority list, per-node slot accounting, and every placement/bind
decision — start there.

---

## Where to go next

Each component directory has its own `AGENTS.md` explaining that mapper
in detail:

- [`round_robin/AGENTS.md`](round_robin/AGENTS.md) — the default; read this second.
- [`ppr/AGENTS.md`](ppr/AGENTS.md) — processes-per-resource patterns.
- [`seq/AGENTS.md`](seq/AGENTS.md) — sequential, one proc per hostfile line.
- [`rank_file/AGENTS.md`](rank_file/AGENTS.md) — explicit rank→host+cpuset.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — LSF affinity-hostfile mapping.
