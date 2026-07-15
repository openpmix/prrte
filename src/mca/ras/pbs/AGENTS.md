# AGENTS.md — `ras/pbs` (PBS / Torque / Cobalt allocator)

Component guide for `src/mca/ras/pbs/`. Read the
[framework guide](../AGENTS.md) first for the module contract and the
`allocate()` return protocol.

---

## Role and priority

`ras/pbs` reads a PBS/Torque (or Argonne Cobalt) allocation from the
node file. It makes itself available at the configurable priority
`ras_pbs_priority` (default **100**) only when it detects a PBS job:
`PBS_ENVIRONMENT` **and** `PBS_JOBID` are set, or `COBALT_JOBID` is set.

Files:

| File | Contents |
|------|----------|
| `ras_pbs_component.c` | Registration; `query` gates on the PBS/Cobalt env; `priority` and `smp` MCA params. |
| `ras_pbs_module.c` | `allocate`, `finalize`, and the `discover` nodefile parser. |
| `ras_pbs.h` | Component struct (`smp_mode`) and externs. |

---

## How `allocate()` works

`allocate` requires `PBS_JOBID` (falling back to `COBALT_JOBID`), saves
it in `prte_job_ident` for error reporting, then calls `discover`:

- Opens `PBS_NODEFILE` (or `COBALT_NODEFILE`). PBS lists one line per
  allocated slot, so the *same hostname may repeat*; `discover` dedups by
  name and **bumps `slots` per repeat**. Each distinct host becomes a
  `prte_node_t` at `PRTE_NODE_STATE_UP`, tagged with `PRTE_NODE_LAUNCH_ID`
  (its ordinal in the file).
- **SMP mode** (`ras_pbs_smp`, default false): for big SMP machines
  (e.g. SGI) where listing each node once/slot is impractical, the file
  lists each node once and `PBS_PPN` gives cpus/node. In SMP mode a
  repeated hostname is an error (`smp-multi`); slots are set to `PBS_PPN`.
- An empty result is unrecoverable in the PBS world → `no-nodes-found`
  help and `PRTE_ERR_NOT_FOUND`.

On success it records `prte_num_allocated_nodes` and returns
`PRTE_SUCCESS`; the base's `node_insert` places the nodes.

---

## Things to watch when editing

- The nodefile parser (`pbs_getline`) caps lines at
  `PBS_FILE_MAX_LINE_LENGTH` (512) and strips the trailing newline.
- `discover` ignores any user hostfile/dash-host — PBS is authoritative.
- The Cobalt variant is a first-class alias throughout; keep both env
  names handled in `query`, `allocate`, and `discover`.
</content>
