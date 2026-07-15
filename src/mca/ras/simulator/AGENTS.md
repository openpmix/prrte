# AGENTS.md — `ras/simulator` (synthetic allocation for testing)

Component guide for `src/mca/ras/simulator/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/simulator` **fabricates a synthetic allocation** — a made-up set of
nodes with made-up slot counts and the local topology — so mapping,
ranking, and binding can be exercised without a real cluster. It selects
at priority **1000** (pre-empting every real allocator) but *only* when
`ras_simulator_num_nodes` is set; otherwise it disqualifies. Jobs
allocated this way are marked **do-not-launch**.

Files:

| File | Contents |
|------|----------|
| `ras_sim_component.c` | Registration; `query` gates on `num_nodes`; the `num_nodes`/`slots`/`max_slots`/`have_cpubind`/`have_membind` MCA params. |
| `ras_sim_module.c` | `allocate`, `finalize`. |
| `ras_sim.h` | `prte_ras_sim_component_t` (num_nodes, slots, slots_max, topofiles, topologies, have_cpubind, have_membind). |

---

## How `allocate()` works

Driven entirely by MCA params (all comma-separated, one entry per
topology group):

- `ras_simulator_num_nodes` — how many nodes to create per group
  (required; also the `query` gate).
- `ras_simulator_slots` / `ras_simulator_max_slots` — per-group slot and
  max-slot counts; a short list is back-filled by repeating the last
  entry. If omitted, slots default to the topology's PU/core count via
  `prte_hwloc_base_get_npus`.

For each group it mints nodes named `nodeA0000`, `nodeA0001`, … (the
prefix's last letter advances per group), each at `PRTE_NODE_STATE_UP`,
sharing the process's topology (`prte_node_topologies[0]`, retained per
node) and an `available` cpuset derived from the job's `PRTE_JOB_CPUSET`
/ `PRTE_JOB_HWT_CPUS`. It records `prte_num_allocated_nodes` and sets
**`PRTE_JOB_DO_NOT_LAUNCH`** on the job so nothing is actually spawned
(this, combined with the base's `node_insert`, fabricates a daemon per
node so the mapper has something to work with). Returns `PRTE_SUCCESS`.

Often paired with `ras_base_multiplier` to inflate node counts further.

---

## Things to watch when editing

- `num_nodes` doubles as the availability gate — clearing it disables the
  component entirely.
- The topology is *shared* (`PMIX_RETAIN`) across all synthetic nodes;
  don't free it per node.
- This path is a primary consumer of the base's `DO_NOT_LAUNCH` daemon
  fabrication — keep the attribute set.
</content>
