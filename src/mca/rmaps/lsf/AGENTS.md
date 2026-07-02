# AGENTS.md — `rmaps/lsf` (LSF affinity mapper)

Component guide for `src/mca/rmaps/lsf/`. Read the
[framework guide](../AGENTS.md) first.

---

## Role and priority

lsf maps a job according to the pinning LSF already decided, published in
the **`LSB_AFFINITY_HOSTFILE`** environment variable. When a job runs
under LSF with affinity resource requirements, LSF writes a file listing,
per rank, the host and the physical CPU ids that rank should use; this
mapper reads that file and reproduces the placement.

It is priority **100**, the **highest** of all rmaps mappers, so it gets
first refusal on every job. That is deliberate: if LSF pinned the job,
LSF's decision should win over any generic policy. It only actually
claims the job when the affinity file is present and non-empty; otherwise
it defers.

This component is built only when LSF support is configured
(`--with-lsf`).

Files:

| File | Contents |
|------|----------|
| `rmaps_lsf_component.c` | Registration and `query` (always returns priority 100). Built only when `--with-lsf` is configured (see `configure.m4`). |
| `rmaps_lsf.c` | `lsf_map()` and the affinity-file parser `file_parse()`. |
| `rmaps_lsf.h` | `prte_rmaps_lsf_map_t` (`{node_name, slot_list[64]}`), `RMAPS_LSF_MAX_SLOTS`. |

The design deliberately mirrors [`rank_file`](../rank_file/AGENTS.md):
parse an external file into a rank-indexed `rankmap`, then place each
rank on its named host with its slot list as the binding.

---

## Gate conditions (`lsf_map`)

Defers with `PRTE_ERR_TAKE_NEXT_OPTION` when:

- `PRTE_JOB_FLAG_RESTART` is set.
- `LSB_AFFINITY_HOSTFILE` is **not** in the environment (not an LSF
  affinity job).
- The user gave an explicit `--map-by` directive
  (`PRTE_MAPPING_GIVEN`) — an explicit request overrides LSF's pinning.
- `options->ordered` was requested.
- The affinity file parses to **zero ranks** (LSF set no pinning) — let a
  normal mapper handle it.

On acceptance it forces hwthread-as-cpu semantics (`PRTE_JOB_HWT_CPUS`,
`options->use_hwthreads = true`) because LSF reports CPUs as hwthreads,
defaults binding to hwthread if the user set none, stamps
`last_mapper = "lsf"`, and sets `options->map = PRTE_MAPPING_BYUSER`.

---

## `file_parse()` — reading `LSB_AFFINITY_HOSTFILE`

The file format is one line per rank:

```
Host1 0,1,2,3 0 2      # host  cpu_id_list  [NUMA_id_list  mem_policy]
Host1 4,5,6,7 1 2
```

For each line the parser:

1. Splits off the host name and the comma-separated CPU id list (the
   trailing NUMA/mempolicy fields are captured into `membind_opt` and
   currently dropped).
2. Strips the FQDN when `!prte_keep_fqdn_hostnames`.
3. Finds the host in `prte_node_pool` (`prte_quickmatch`).
4. **Converts LSF's *physical* CPU ids to hwloc *logical* indices**:
   each id is looked up with `hwloc_get_pu_obj_by_os_index()` and
   replaced by the PU's `logical_index`. This is the key translation —
   LSF speaks OS/physical numbering, the rest of PRRTE speaks hwloc
   logical numbering.
5. Stores a `prte_rmaps_lsf_map_t {node_name, slot_list}` at index
   `num_ranks++` in the module-static `rankmap`.

An empty file (stat size 0) means "no affinity" and returns success with
`num_ranks == 0`, which the gate turns into a defer.

---

## Placement (`lsf_map`)

Per app (honoring `options->app_idx`), for each rank `k`:

1. Look up `rankmap[vpid_start + k]`.
   - **Present:** use its slot list; find its host in the node list
     (supporting `+nK` relative indices, as in rank_file).
   - **Absent:** fall back to `options->cpuset` / `prte_hwloc_default_cpu_list`
     and take the next non-oversubscribed (or least-loaded) node; a rank
     with no entry and no default slot list is a `missing-rank` error.
2. `check_support`, `get_cpuset`, `check_avail`, `check_oversubscribed`,
   `setup_proc`.
3. Set `proc->name.rank = rank` explicitly (userranked).
4. **Bind from the slot list:** parse it with
   `prte_hwloc_base_cpu_list_parse()` (hwthread semantics), set
   `proc->cpuset`, verify inclusion in `node->available` unless
   `overload`, then consume those bits from `node->available`. Locale is
   left NULL (a slot list may span objects).
5. Insert into `jdata->procs`.

When not in per-app dispatch, `compute_vpids` back-fills local/app ranks.

---

## Things to watch when editing

- **Physical→logical CPU conversion is the whole point.** If
  `hwloc_get_pu_obj_by_os_index()` returns NULL for an id, the node's
  topology and LSF's view disagree — that's a hard error, not something
  to paper over.
- **`num_ranks` and `rankmap` are file-static.** They are reset
  (`num_ranks = 0`, fresh `rankmap`) at the start of a map and destructed
  on every exit path; keep that lifecycle intact to avoid stale state
  across jobs.
- **This mapper only sees the environment on the HNP.** It reproduces
  LSF's decision; it does not talk to LSF. Placement fidelity depends
  entirely on parsing the file correctly and matching hosts against
  `prte_node_pool`.
- **hwthread semantics are forced on** for LSF jobs — don't reintroduce
  core-based counting in the slot-list path.
- Shares slot-list/`+nK` logic and the fixed `char[64]` bound with
  rank_file; keep the two consistent when touching shared behavior.
- Honor `options->app_idx`, the `initial_map = (0 == num_nodes)`
  convention, and the defer-don't-error gate.
