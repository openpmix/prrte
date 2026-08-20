# AGENTS.md — `rmaps/round_robin` (the default mapper)

Component guide for `src/mca/rmaps/round_robin/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`prte_rmaps_options_t` scratch struct, and the base helpers referenced
throughout.

---

## Role and priority

round_robin is the **catch-all default mapper**, priority **10** (lowest
of all rmaps components), so it is the last mapper the base tries. It
handles the generic, hardware-agnostic-to-hardware-specific spread
policies: **by-slot, by-node, by-`<object>`** (numa/package/L3/L2/L1/
core/hwthread), and **by-pe-list**. Anything more specialized (ppr,
rankfile, seq, lsf) is claimed by a higher-priority mapper first; if the
user asked for none of those, the job lands here.

Files:

| File | Contents |
|------|----------|
| `rmaps_rr_component.c` | Registration, `priority` MCA param (default 10), `query` returning the module. |
| `rmaps_rr.c` | `prte_rmaps_rr_map()` — the module entry: gate checks + per-app dispatch to the four algorithms. |
| `rmaps_rr_mappers.c` | The four algorithms: `byslot`, `bynode`, `bycpu`, `byobj` — the last of which is a wrapper around the shared `map_targets` loop. |
| `rmaps_rr.h` | Prototypes for the four algorithm functions, plus `prte_rmaps_target_enum_t` — the vtable that says how a node's placement targets are listed. |

---

## Gate conditions (`prte_rmaps_rr_map`)

Before doing anything, the module defers (`PRTE_ERR_TAKE_NEXT_OPTION`)
when the job isn't its business:

- `PRTE_JOB_FLAG_RESTART` is set — rr only does initial launch.
- `PRTE_GET_MAPPING_POLICY(jdata->map->mapping) > PRTE_MAPPING_RR` — i.e.
  the policy is one of the specialized values (seq/user/ppr, whose
  numeric codes are all `> PRTE_MAPPING_RR == 16`). The round-robin
  policies (by-slot=9, by-node=1, the object codes 2–8, pe-list=11) are
  all `<= 16`, so only those fall through to rr.

If it accepts, it loops the app contexts; the base records that
round_robin was the component (`PRTE_APP_LAST_MAPPER`).

---

## Per-app dispatch inside the module

`prte_rmaps_rr_map` walks `jdata->apps`. For each app it:

1. Honors `options->app_idx`: in per-app (MPMD) dispatch it skips every
   app except the requested index.
2. Computes `initial_map = (0 == jdata->map->num_nodes)` so per-node
   MAPPED flags are cleared only on the genuine first pass (per-app
   dispatch enters this module once per app — treating each entry as a
   fresh map would double-add nodes a prior app already placed).
3. Calls `prte_rmaps_base_get_target_nodes()` for the app's node list.
4. Dispatches on `options->map` to one of the four algorithms:
   - `PRTE_MAPPING_BYNODE` → `prte_rmaps_rr_bynode`
   - `PRTE_MAPPING_BYSLOT` → `prte_rmaps_rr_byslot`
   - `PRTE_MAPPING_PELIST` → `prte_rmaps_rr_bycpu`
   - anything else (an object type) → `prte_rmaps_rr_byobj`. **There is
     deliberately no fallback here.** We map by an object only because the
     user asked for it, so being unable to is an error, not a licence to
     place the job by some other rule. This branch used to downgrade the
     policy to by-slot and retry — silently answering a question nobody
     asked.
5. Adds `app->num_procs` to `jdata->num_procs`.

Finally, when **not** in per-app dispatch (`app_idx < 0`), it calls
`prte_rmaps_base_compute_vpids()` for the whole job. In per-app dispatch
it skips this — the base ranks each app with correct cross-app numbering.

---

## The four algorithms (`rmaps_rr_mappers.c`)

All four share a skeleton: a "can we fit?" pre-check, a main placement
pass over the node list, and (except byobj) a **second pass** to spread
the overflow when oversubscription is allowed. All four reset an *unset*
binding policy to `BIND_TO_NONE` for a node when procs exceed cpus but
fit within slots (overloaded-but-not-oversubscribed), restoring
`savebind` before moving on.

### `prte_rmaps_rr_byslot` — fill nodes in order
Assigns up to `node->slots_available` procs to each node before moving
on, so procs are front-loaded. If `num_slots < app->num_procs` and
oversubscribe isn't allowed → hard `alloc-error`. Otherwise the leftover
procs are balanced across the remaining nodes in a second pass
(`extra_procs_to_assign` / `nxtra_nodes` compute the even split plus the
remainder onto the first few nodes). This is the most common default.

### `prte_rmaps_rr_bynode` — round-robin across nodes
Computes an average `(remaining procs) / (num nodes)` and lays that many
on each node per pass, capping at `slots_available` when not
oversubscribing, looping until all procs placed. One-proc-per-node
behavior emerges when there are fewer procs than nodes. Same second-pass
overflow handling as byslot.

### `prte_rmaps_rr_byobj` — spread across hwloc objects
Maps by an object type (`options->maptype`: package/numa/cache/core/
hwthread). It is a thin wrapper: it builds a `prte_rmaps_target_enum_t`
naming the hwloc objects of that type and hands it to
`prte_rmaps_rr_map_targets()`, which is the loop described below.

**Add a new kind of target by writing an enumerator, not by copying the
loop.** The enumerator answers three questions about one node — how many
targets, give me the *j*-th, and (optionally) set up and tear down per-node
state — and `map_targets` does everything else. That split exists because
the loop is the subtlest code in this component: the `redo:`/`check_avail`
interplay and the oversubscribe accounting have each been the source of a
real bug, and a second copy would acquire them again. `.name` is what the
diagnostics call a target, so an enumerator that is not an hwloc type still
produces a message that names what the user asked for.

Two sub-modes:
- **non-span** (default): fill all objects on a node before the next node
  (the `redo:` loop keeps working a node's objects until full) —
  byslot-like, front-loaded.
- **span** (`options->mapspan`): one proc per object cycling across *all*
  nodes as if they were one big node — load-balanced.

**Span is an interleave, and that takes two things: a budget of one target
per node per pass, and a cursor.** The outer `do { … } while` is the trip
round the node list, so a budget of 1 makes each pass lay down one proc per
node — which is the cycling, and is also why span needs none of the
even-split arithmetic below: balance falls out of the rotation, oversubscribed
or not. The cursor is what makes the *targets* advance: the k'th proc a node
receives goes on that node's k'th target, so it is kept per node (keyed by
`node->index`, the pool slot, which survives a node being dropped from the
list) and the object loop starts there and wraps.

Without both, span was not spanning at all: `mapspan` only suppressed the
`goto redo`, so the loop still filled each node's targets before moving on
and span differed from non-span only when a node was asked to hold more procs
than it has targets. On 3 nodes of 4 slots, `-n 8 --map-by core:span` came out
4/4/0 where the "one big node" reading it documents gives 3/3/2.

A target set that cannot be revisited (`tgts->nowrap`, the device maps) is
deliberately excluded: it gets a single pass by design, so there is no
rotation to join, and a budget of 1 would place one proc per node and stop.

Per object it checks free cpus against `cpus_per_rank` (only when
actually binding; if binding was reset to NONE for an oversubscribed
node, a cpu shortage must not block placement). A node with **zero** objects
of the requested type is a hard error (`rmaps:mapping-target-not-found`,
returned as `PRTE_ERR_SILENT`): the request cannot be answered on that node,
and quietly dropping the node shrinks the allocation the user gave us
without telling them. `outofcpus` vs. `allfull` distinguish the two failure
help messages (`allocation-overload` vs `failed-map`). Note this mapper has
**no** second-pass block — the outer `do { … } while (!allfull)` loop handles
iteration, and the per-pass budget below is what gives those later passes the
job byslot's second pass does.

**A node's budget is `slots_available`, not its slot count.** The loop asks
for one proc at a time, so nothing in `check_avail`/`check_oversubscribed` —
both of which measure against `node->slots` — stops it at the smaller number
a `-host` entry's `:N` suffix asked for. It therefore checks the field itself
before each placement (`setup_proc` draws it down by one per proc). Without
that, `--host a:1,b:1,c:1 -n 3` selecting within an existing allocation put
all three procs on `a` and left `b` and `c` out of the map — the total was
still checked against the caps, so the job ran, just nowhere near where it
was told to. See the framework guide.

**Each pass gives a node a budget, and the budget is what makes an
oversubscribed job come out balanced.** On the first pass it is
`node->slots_available` — what the node offers this job. On every pass after
that (which can only happen when oversubscription is allowed) it is an even
share of the procs still unplaced, recomputed per pass over the nodes still
on the list: the same `extra_procs_to_assign`/`nxtra_nodes` split byslot uses
for its second pass, which is why the two now agree — `-host a:2,b:2,c:2`
with `--map-by core:oversubscribe` gives 3/3/2 at `-n 8`, 4/4/4 at `-n 12`
and 6/6/6 at `-n 18`, exactly as `--map-by slot:oversubscribe` does.
Recomputing per pass (rather than once, as byslot does) also lets the split
correct itself when a node cannot take its share because its cpus ran out or
it hit `max_slots` and left the list.

Being allowed to oversubscribe says how many procs a node may end up with,
not that the head of the list may have them all: without the budget an
object map filled the first node with the entire job (8/0/0 above), and with
the budget applied only on the first pass it still gave that node the whole
overflow (8/2/2 at `-n 12`).

**The `redo:` loop and `check_avail` do not mix casually.** When
`check_avail` declines a node it may also have removed it from `node_list`
and released it (the `max_slots` case — see the framework guide). So the
`break` out of the object loop must be accompanied by `nodefull = true`:
without it the `goto redo` re-enters the object loop on a node that is no
longer on the list, `check_avail` removes it a second time, and the HNP
segfaults. This was reachable from `--map-by core:OVERSUBSCRIBE` on a
hostfile carrying `max_slots`.

### `prte_rmaps_rr_bycpu` — map to a pe-list
For `--map-by pe-list=<ranges>`, **job-level or per-app** — which cpus an app
of an MPMD job may use is as much its own business as which object it maps
by, so the app parser records the list on `PRTE_APP_CPUSET` and
`resolve_app_options` hands it to this mapper in `options->cpuset` exactly as
the job-level path does. The requested cpuset (`options->cpuset`)
is saved/restored across nodes (`savecpuset`) because the placement
consumes it. When not overloading/ordered it places exactly as many
procs as PEs listed; otherwise up to `slots_available`. Same
second-pass overflow split as byslot when oversubscribe is on. Binding
for these procs is done by `bind_to_cpuset` inside `setup_proc`.

`options->cpuset` is *owned* by the options struct, and `bind_to_cpuset`
frees and replaces it as it consumes entries — so every path that rewrites
it has to free what is there first, including the success path, and the
struct must be left holding a pointer its owner can free.

---

## Things to watch when editing

- **Keep the gate cheap and correct.** The `> PRTE_MAPPING_RR` test is
  what keeps rr from stealing seq/user/ppr jobs; the numeric ordering of
  the `PRTE_MAPPING_*` codes in `rmaps_types.h` is load-bearing. Note it
  asks about the *job's* policy even in per-app dispatch — so an app-level
  directive cannot pull a job away from a specialized mapper the job-level
  policy already selected. That is a framework-wide limitation of the
  per-app path, not one specific to rr.
- **Do not reintroduce a policy fallback.** Every mapping policy this
  component handles was asked for; if it cannot be honored, say so.
- **`initial_map` must stay `num_nodes`-based**, or per-app/MPMD jobs
  re-add nodes and inflate the map.
- **Free `options->target`/`options->job_cpuset` per node.** The
  algorithms free `target` on the "move to next node" paths and on
  success; bycpu also juggles `cpuset`/`savecpuset`. Leaks or double
  frees here are the classic bug.
- **Don't skip the base vpid call except in per-app mode.** The
  `if (options->app_idx < 0)` guard around `compute_vpids` is what keeps
  MPMD ranks from colliding.
- **`map_targets` is shared; keep it that way.** Anything that needs a
  different set of placement targets belongs in a `prte_rmaps_target_enum_t`,
  not in a second copy of the loop. If the enumerator allocates per-node
  state in `begin`, every exit from the node body has to reach `end` — the
  normal "move to the next node" tail and the `errout` label both do.
