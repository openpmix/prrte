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
`seq`/`ppr`/`rankfile`/`pe-list=…` plus colon modifiers
(`PE=n`, `SPAN`, `OVERSUBSCRIBE`, `NOLOCAL`, `HWTCPUS`, `INHERIT`,
`ORDERED`, `FILE=…`, …). `--rank-by slot`/`node`/`fill`/`span`.

All three may be given **per app context** as well as per job. On a command
line the first app segment is where the job is described: a directive
written there and nowhere else is the job's, however many apps follow.
Written on any later app it is that app's alone, and its silent siblings
take the defaults. So an MPMD line can become per-app with a single
directive, and when it does there may be no job-level directive left to
hang anything on. The
per-app parsers live beside the job-level ones in `rmaps_base_frame.c`
(`prte_rmaps_base_set_app_{mapping,ranking,binding}_policy`) and record onto
`app->attributes`; `prte_rmaps_base_resolve_app_options()` turns those into
the per-app `options` a mapper sees. Keep the two levels in step — a policy
or qualifier accepted at one and refused at the other is the recurring bug
in this file (`pe-list` was job-level only; `:SPAN` and `:ORDERED` were
silently rejected per app).

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
`PRTE_ERR_TAKE_NEXT_OPTION` if not (wrong mapping policy, a restart it
can't handle, …). See any component's guide for its
exact gate conditions.

**The policy IS the choice of mapper.** There is no "use this component"
request anywhere in the framework: each component claims the policies it
implements, so naming one says nothing the policy has not already said, and
when the two disagreed there was no answer that could be right.
`PMIX_MAPPER` is refused on the spawn path
(`PMIX_ERR_NOT_SUPPORTED`) and is not advertised in
`PMIX_QUERY_SPAWN_SUPPORT`.

`--prtemca rmaps <component>` cannot be refused — which components load is
settled at framework open, before any job exists — so a restriction that
cannot serve the job's policy simply means no mapper accepts. That is an
error, reported by `report_no_mapper()` with the loaded set named
(`mapper-restricted`), not a silent placement by some other rule.

**A gate reads `options`, never `jdata->map`.** The policy and the
was-it-given flag live in the `prte_rmaps_options_t` the base hands over —
`options->map`, `options->mapgiven` — precisely because in per-app dispatch
each app answers those questions for itself. Asking the job instead is why a
per-app `--map-by seq`/`rankfile`/`ppr` never reached its mapper: the job's
policy is whatever default was resolved for the apps that gave no directive,
and seq/rank_file/ppr all defer on it. The base fills these fields from the
job for a whole-job dispatch, so one test serves both paths.

**The base records who mapped**, not the mapper, and it records it *per app*
(`PRTE_APP_LAST_MAPPER`). A mapper that stamps itself on entry cannot know
it will still be the answer, and in per-app dispatch it is asked once per
app. `prte_job_map_t` carries no mapper name at all: two apps of one job can
be placed by two components, so one job-level name could only ever be half
the answer — and mapping happens on the HNP alone, so the two strings it
used to put on the wire were read by nothing at the far end.

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

This ~1050-line function in `rmaps_base_map_job.c` is the heart of the
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

   **Order matters here, in one direction only.** Deriving the mapping
   reads `jdata->map->binding`, so any binding the *user* asked for has to
   be on the job before that runs — including one inherited from the parent
   job or from the DVM-wide `bindto` MCA parameter. Those two arms used to
   run afterwards, so `--prtemca bindto package` derived `BYCORE` and was
   then refused by the bind-upwards check below, while `--bind-to package`
   on the command line derived `BYPACKAGE` and worked. Deriving a binding
   *from* the mapping (`set_default_binding`) is the opposite dependency and
   necessarily still runs after the mapping is known; `bind_inherited` is
   what keeps the two from both firing. See
   [`src/hwloc/AGENTS.md`](../../hwloc/AGENTS.md), "`bindto` is a real
   parameter, not a suggestion".

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

`cleanup` owns the caddy, the colocation `darray`, the launch-proxy
`pmix_proc_t`, and both options structs' cpusets and strings. **Never release
any of those at the early-out itself** — an early-out that also released the
caddy dropped a live event caddy twice.

Note also that `rc` at any given point holds the result of the last call that
ran, which on a *validation* failure (a policy combination that cannot work)
is `PRTE_SUCCESS`. Set `jdata->exit_code` explicitly on those paths —
`PRTE_ERR_SILENT` when the help text has already been shown — rather than
assigning `rc`.

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
- `mapgiven` — whether the policy in `map` is the user's rather than one the
  base derived. From the app in per-app dispatch, from the job otherwise;
  `lsf`, which claims only a job nobody described, gates on it.
- `start_vpid` — the first global rank this dispatch may assign. Only the
  mappers that number their own procs (rank_file, seq, lsf) read it;
  everyone else leaves ranking to `compute_vpids`, which threads the same
  cursor. A user-ranked mapper that started from zero for every app gave two
  apps the same ranks, and the second app's procs then replaced the first's
  in `jdata->procs`.

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
| `prte_rmaps_base_check_avail()` | Can this node/object take more procs? Also adds the node to `jdata->map->nodes` exactly once and sets `options->target`. **It can also remove and release the node** — see below. |
| `prte_rmaps_base_check_oversubscribed()` | After placing a proc, flag/deny oversubscription per the node's SLOTS_GIVEN and the job's OVERSUBSCRIBE directive. |
| `prte_rmaps_base_setup_proc()` | Create the `prte_proc_t`, attach it to the node, assign node rank, bump `slots_inuse`, and **bind it** (`prte_rmaps_base_bind_proc`). |
| `prte_rmaps_base_compute_vpids()` | Assign global ranks by slot/node/fill/span, then derive local & app ranks. |
| `prte_rmaps_base_bind_proc()` | Bind a proc: dispatch to `bind_generic` / `bind_multiple` (pe>1) / `bind_to_cpuset` (pe-list), or no-op for by-user/bind-none. |

The universal mapper loop is therefore: for each app → `get_target_nodes`
→ for each node → `get_cpuset`, `check_support`, `get_ncpus`,
`check_avail`, then `setup_proc` (which binds) and `check_oversubscribed`
per proc → finally `compute_vpids` (only when `app_idx < 0`).

**Order matters: `check_oversubscribed` runs *after* `setup_proc`.** It
reads the node's proc count, so asking before the proc is placed judges the
node one proc behind. Every mapper follows the order above.

### `check_avail` may take the node off your list

When a node has reached its `slots_max` — a hard bound no oversubscribe
directive can lift — `check_avail` returns false *and* removes the node from
the `node_list` you handed it, releasing the reference that list held. Two
obligations follow:

- **A false return means "done with this node."** Do not offer the same node
  again in the same pass. Doing so asks `check_avail` to remove an item that
  is no longer on the list and to drop a reference it no longer holds; that
  corrupted the list and segfaulted the HNP (`--map-by <object>` against a
  hostfile carrying `max_slots`).
- **Pass `NULL` if your list is not a list of the nodes you are placing on.**
  The sequential mapper walks hostfile entries, not `prte_node_t`s, so it
  passes `NULL` and simply takes the "no" — handing over a list of the wrong
  type meant removing a `prte_node_t` from a list of `seq_node_t`.

### Who owns what in `options`

`node->available` and `options->job_cpuset` are **never NULL**. The mappers
copy and intersect both without checking (`hwloc_bitmap_copy(node->jobcache,
node->available)` in `get_target_nodes()` is the first one), so a NULL is a
segfault in the HNP inside hwloc. When a `--cpu-set` cannot be resolved
against a node's topology, `prte_hwloc_base_filter_cpus()` and
`prte_rmaps_base_get_cpuset()` hand back an **empty** set — the node then
offers nothing and is reported unusable through the normal path, and
`src/hwloc` has already told the user which entry failed. See
[`src/hwloc/AGENTS.md`](../../hwloc/AGENTS.md).

`job_cpuset` and `target` are hwloc bitmaps the mappers recycle per node;
`cpuset` is a string the *struct* owns (it comes out of an attribute, which
returns a copy). `bind_to_cpuset` consumes `cpuset` one
entry at a time and rewrites it, so the pointer at the end of a map is
whatever the mapper left, not what was read in. `prte_rmaps_base_map_job()`
frees all three at `cleanup`, and the per-app copy gets its own `strdup` of
the string so two structs never share one allocation.

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
set the rank themselves; because their `SEQ`/`BYUSER` mapping policy makes
the base set `options.userranked`, `compute_vpids` only back-fills
local/app ranks.

**Every scheme that loops until `app->num_procs` needs a "did this pass rank
anything?" guard.** by-node and by-span both cycle rather than iterating a
bounded index, so a pass that matches no proc — an app whose `num_procs`
outruns what was actually placed, or a proc whose locale is not an object of
the mapping type — must break the loop rather than repeat it. Without the
guard the mapper does not fail, it hangs, and a hung mapper is a wedged HNP.

In per-app (MPMD) dispatch the base calls `compute_vpids` once per app with
`app_idx = n` and a `next_vpid` cursor threaded across the calls; that cursor
is the only thing keeping two apps from both starting at rank 0. The
by-user mappers number their own procs, so they get the same cursor as
`options->start_vpid` and `compute_vpids` advances it past the app it was
handed. A per-app rankfile or sequence file therefore numbers *that app's*
ranks — the global rank each one lands on depends on how many procs the
apps before it took.

---

## Binding

`rmaps_base_binding.c` binds each proc as it is set up. `bind_generic`
walks the candidate hwloc objects of type `options->hwb`, intersects
their cpusets with what's still available on the node, and picks the
first with free cpus (honoring an optional per-object `limit`). If none
is free it either errors (required binding) or, when `overload` is
allowed, round-robins onto the least-loaded object without consuming
`node->available` (so a non-overloading later job still sees the node as
full).

**Binding to an object means the whole object, read within the job's
cpu-set.** `set_proc_cpuset()` is the only place a `proc->cpuset` is
written for object binding, and it intersects the object with
`node->jobcache` — the node's availability as this job first found it,
which is where a DVM-wide cpu-set has already been applied. `jobcache` and
not `node->available`, because `available` shrinks as procs are placed and
every proc bound to the same object has to get the same answer. A per-job
`--cpu-set` never narrows `node->available` at all, so it is applied
separately out of `options->job_cpuset`. Handing back `obj->cpuset` raw —
which is what this used to do — meant a cpu-set was honored by `--bind-to
core` (where the object sits inside the set anyway) and silently discarded
by `--bind-to package` or `numa`: the rank came back owning every core of
the object. Note that `prte_node_construct()` leaves `jobcache` allocated
but **empty**, and the colocation path reaches binding without going
through `get_target_nodes`, so the intersection falls back to the bare
object when it would otherwise come up empty.

**`--bind-to` is parsed in two places and gated in a third.** Per-app by
`prte_rmaps_base_set_app_binding_policy()` here, job-level by
`prte_hwloc_base_set_binding_policy()` in
[`src/hwloc`](../../hwloc/AGENTS.md), and whitelisted before either of them
by `bndquals[]` in `schizo/base/schizo_base_frame.c` — a qualifier missing
from that list never reaches a parser at all, which is how `report` came to
be implemented, refused, and undocumented at the same time. All three
policies and all the qualifiers agree now, with one deliberate exception:
`report` is **job-level only**, because the attribute behind it
(`PRTE_JOB_REPORT_BINDINGS`) has no per-app counterpart — reporting is a
property of the whole job. The per-app parser therefore refuses it *by
name* (`job-only-modifier`) rather than as an unrecognized qualifier, since
the same spelling is legal one app segment earlier.

The `limit` qualifier is parsed **twice** — per-app by
`prte_rmaps_base_set_app_binding_policy()` here, and job-level by
`prte_hwloc_base_set_binding_policy()` in
[`src/hwloc`](../../hwloc/AGENTS.md). The two must agree. Both back it with a
`uint16_t` attribute, so a value that does not fit has to be *refused* rather
than cast (`limit=70000` used to become 4464), and both refuse zero, because
`bind_generic` reads a limit of zero as "no limit at all" (`0 < options->limit`)
rather than as what the user wrote.

`bind_multiple` handles `cpus_per_rank > 1`; `bind_to_cpuset`
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
- **A policy we cannot honor is an error, never a substitution.** Every
  mapping policy that reaches a mapper is one the user asked for, so a
  mapper that cannot satisfy it must say so and fail the job. Silently
  falling back to something else — round_robin used to downgrade an
  unavailable object map to by-slot, and to drop the offending node from
  consideration first — places the job by a rule nobody asked for and
  quietly shrinks the allocation the user gave us.
- **Qualifier names may be abbreviated; read values after the `=`, never at
  a fixed offset.** `PMIX_CHECK_CLI_OPTION` matches any unambiguous prefix,
  so `P=2` is `PE=2` and `F=path` is `FILE=path`. Indexing past the full
  spelling turned `--map-by core:P=2` into pes-per-proc **0** (and then a
  misleading "out of resource") and `seq:F=path` into an attempt to open the
  path five characters in. `qualifier_value()` in `rmaps_base_frame.c` is the
  one way to get a qualifier's value. (`src/hwloc/hwloc.c` had the same bug in
  the job-level `--bind-to` `LIMIT=` value and now uses
  `pmix_cli_qualifier_value()`.) A related trap in the same family: an
  **empty** string matches whatever `PMIX_CHECK_CLI_OPTION` tests it against
  first, because the comparison is only `min(strlen(a), strlen(b))` long —
  which is why the `--map-by :QUALIFIER` form needs its own explicit branch,
  and why `--bind-to :overload-allowed` used to resolve to `none`.
- **A cpu number shown to a user goes through
  `prte_hwloc_base_cpuset2ranges()`.** The bits of a cpuset are PU *OS*
  indices; every grammar this framework accepts — slot lists, rankfile
  entries, `--cpu-set` — is in hwloc *logical* indices. `seq`, `rank_file`
  and `lsf` each printed a raw bitmap as the "available" and "overlapping"
  cpus of a collision message while quoting the user's logical slot list
  beside it. `prte_proc_t.cpuset` is the one deliberate exception: it is a
  wire format, read back with `hwloc_bitmap_list_sscanf()`, and must stay in
  OS indices — so never show *it* to a user either. See
  [`src/hwloc/AGENTS.md`](../../hwloc/AGENTS.md).
- **Module-static state outlives the job.** `rank_file` and `lsf` both keep
  their parsed rank map and rank count in file statics. Reset them at the
  start of every map and reclaim them on *every* exit, success or failure —
  a stale count is handed to the next job as its process count.
- **The version macro is `PRTE_RMAPS_BASE_VERSION_5_0_0`.** The `4_0_0`
  alias is deliberately redefined to `5_0_0` so stale out-of-tree
  components fail loudly instead of silently violating ABI.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG`/`PRTE_ACTIVATE_JOB_STATE` for errors.

---

## Testing

Three layers, and each answers a question the others cannot. Run the first
two for any change in here; run the third when you touch a mapper that names
hosts, or anything that can fail a map.

**1. Unit tests — `test/unit/rmaps/` (`make check`).** Everything reachable
without a node pool, a topology, or a DVM:

| File | Covers |
|------|--------|
| `test_policy_parse.c` | the per-app `--map-by`/`--rank-by`/`--bind-to` parsers |
| `test_job_policy.c` | the job-level parsers (policy+qualifiers, ppr, pe-list, rankfile) and the policy printer |
| `test_job_qualifiers.c` | `hoist_job_directives` — the qualifiers that describe the whole job |
| `test_resolve_options.c` | `resolve_app_options` and the rank/bind default derivations |
| `test_ranking.c` | `compute_vpids`: by-slot/by-node traversal, the per-app cursor, by-user pass-through, and that a cycling scheme terminates |
| `test_check_avail.c` | `check_avail`: the map-add-once rule, `max_slots`, and the node-removal contract above |
| `test_dispatch.c`, `test_<component>.c` | each mapper's accept/defer gate |

`test_ranking.c` builds a job map by hand — synthetic nodes carrying
synthetic procs — which is why ranking is checkable at all without a DVM.
`test_check_avail.c` builds synthetic nodes and stays on the bind-to-none
path so no topology is needed. Both are cheap patterns to extend.

**2. Offline mapper harness — `make -C test/offline check-offline`.** Over a
thousand `--map-by` × `--rank-by` × `--bind-to` combinations against the
synthetic topologies in `test/topologies/`, checked against invariants
derived from the topology. This is the cheapest way to catch a placement
regression and you should run it for *any* change to mapping, ranking, or
binding. It is not part of `make check` (it needs a freshly built
`prterun`).

**3. Multi-node — `contrib/dockerswarm/run-tests.sh linux`,
`test_rmaps()`.** Three things only a live, multi-node, *persistent* DVM can
show:

- **An impossible mapping must fail the job and leave the DVM standing.** A
  mapper that walks off the end of its node list takes the HNP down, and on
  a persistent DVM that is every other user's job as well. Two cases: a
  request past `max_slots`, and a map by an object the node does not have.
- **The user-ranked mappers name hosts.** With one node any placement is the
  right placement; rankfile and seq only mean something when there is more
  than one host to get wrong.
- **Per-entry and per-app cpu lists have to reach the right proc.** A seq
  entry's cpuset and an app's `pe-list` both bind specific procs to specific
  cpus, which is only checkable against a real topology.

It also covers rank-by node vs. slot ordering and per-app (MPMD) rank
numbering, both of which are invisible on a single node. Placement cases
report rank and host from the process itself rather than parsing
`--display map`, so what is checked is where the job actually ran; the
binding cases read `--display map`, which is the only thing that says both
where a rank went and what it got there.

**Compile coverage for `lsf`.** The LSF mapper is only built with
`--with-lsf`. On a machine without LSF, configure with
`--enable-testbuild-launchers` to compile it against declaration-only stubs;
that tree builds and runs but the stubbed components can do no real work, so
do not install it over a good installation.

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
