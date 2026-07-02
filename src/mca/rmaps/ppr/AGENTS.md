# AGENTS.md — `rmaps/ppr` (processes-per-resource mapper)

Component guide for `src/mca/rmaps/ppr/`. Read the
[framework guide](../AGENTS.md) first.

---

## Role and priority

ppr implements the **`--map-by ppr:N:<resource>`** pattern: "place
exactly **N** processes on each **resource**" — where resource is
`node`, `package`/`socket`, `numa`, `L1`/`L2`/`L3`cache, `core`, or
`hwthread`. It is priority **90**, so it sits above seq and round_robin
but below rank_file and lsf. It also handles the "load-balanced NPERxxx"
case. It is claimed only when the mapping policy is `PRTE_MAPPING_PPR`
*and* the job carries a `PRTE_JOB_PPR` attribute string.

Files:

| File | Contents |
|------|----------|
| `rmaps_ppr_component.c` | Registration, `priority` param (default 90), `query`. |
| `rmaps_ppr.c` | `ppr_mapper()` — the entire mapper. |
| `rmaps_ppr.h` | Component/module externs. |

Unlike round_robin, ppr is a single self-contained function; there is no
separate "mappers" file.

---

## How the pattern reaches the mapper

The `N:resource` string is parsed in **two places before** ppr runs:

1. `prte_rmaps_base_set_mapping_policy()` (in `rmaps_base_frame.c`) sees
   the leading `ppr:` token, stores the `N:resource` pattern as the
   `PRTE_JOB_PPR` job attribute, and sets policy `PRTE_MAPPING_PPR`.
2. `prte_rmaps_base_map_job()` re-reads `PRTE_JOB_PPR`, splits it, and
   translates the resource word into `options.maptype` (an
   `hwloc_obj_type_t`, `HWLOC_OBJ_MACHINE` for "node") and
   `options.mapdepth`, and sets `options.pprn = N`.

So by the time `ppr_mapper()` is entered, the interesting inputs live in
`options->pprn` (procs per resource) and `options->maptype` (which
resource). The raw `PRTE_JOB_PPR` string is still fetched inside the
mapper for its gate check and error messages.

---

## Gate conditions (`ppr_mapper`)

Defers with `PRTE_ERR_TAKE_NEXT_OPTION` when:

- `PRTE_JOB_FLAG_RESTART` is set.
- `jdata->map->req_mapper` names a different mapper.
- The job has no `PRTE_JOB_PPR` attribute, or
  `PRTE_GET_MAPPING_POLICY(jdata->map->mapping) != PRTE_MAPPING_PPR`.

On acceptance it stamps `last_mapper = "ppr"`.

---

## What the mapper does

1. **Derive the equivalent map/rank policy from `maptype`.** A ppr by an
   object type is recorded as the corresponding `BY<object>` mapping so
   downstream code (display, ranking) is consistent; ppr-by-node becomes
   by-node/rank-by-node. It also enforces the framework rule that
   rank-by-fill/span requires an object mapping.
2. **Cache job-level `pprn` and `cpus_per_rank`** (`jobppn`, `jobpes`)
   before the per-app loop, restoring them per app when an app didn't
   override them (per-app dispatch fills these via
   `resolve_app_options`).
3. **Per app** (honoring `options->app_idx` and the `initial_map =
   (0 == num_nodes)` convention):
   - `get_target_nodes` for the node list.
   - **Compute `app->num_procs` when the app gave no `-n`**: for
     by-node it's `pprn × #nodes`; for an object type it sums
     `pprn × #objects-of-that-type` across the node list. This is ppr's
     defining behavior — the process count is *derived from the hardware
     and the pattern*, not given by the user.
   - Verify the count fits the allocation (or oversubscribe is allowed,
     in which case an unset binding is reset to NONE).
   - **Place procs.** Two branches:
     - **by-node** (`HWLOC_OBJ_MACHINE`): put `pprn` procs on each node
       directly (no object loop).
     - **by-object**: for each node get `#objs = nbobjs_by_type`, and for
       each object place `pprn` procs bound to it (`setup_proc` with the
       `obj`), skipping objects that lack enough free cpus.
   - Overloaded-but-not-oversubscribed nodes get an unset binding reset
     to NONE (same idiom as round_robin).
   - Stops at `app->num_procs`; errors with `ppr-too-many-procs` if it
     can't place them all.
4. When not in per-app dispatch, calls `compute_vpids` for the whole job.

---

## Things to watch when editing

- **`pprn`/`cpus_per_rank` restore logic.** In per-app dispatch these
  come from `resolve_app_options`; in whole-job dispatch they come from
  job-level parsing. The `if (0 == options->pprn) options->pprn = jobppn;`
  restore is what makes both paths work — don't remove it.
- **Derived proc counts only when `app->num_procs == 0`.** If the user
  gave `-n`, respect it; ppr then places up to that many and may leave a
  node partially filled.
- **`rc = PRTE_SUCCESS;` reset before `jdata->num_procs +=`.** The
  per-node placement loop can leave `rc` holding the benign
  `PRTE_ERR_TAKE_NEXT_OPTION` "node full" signal from the last
  `check_oversubscribed`; the explicit reset stops that leaking out as
  the mapper's overall result. A regression here makes successful ppr
  jobs look like failures.
- **`jobppr` must be freed on every exit path** (it's `strdup`'d from the
  attribute); the `error:` label and the success path both free it.
- Honor `options->app_idx` and the `initial_map` convention exactly as
  round_robin does — the per-app/MPMD hazards are identical.
