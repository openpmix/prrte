# AGENTS.md — `ras/testrm` (fixed-hostfile fake RM)

Component guide for `src/mca/ras/testrm/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/testrm` is a **fake resource manager for testing**: it pretends a
scheduler handed out exactly the nodes listed in a hostfile you name via
an MCA param. It selects at priority **1000** (pre-empting real
allocators) but only when `ras_testrm_hostfile` is set; otherwise it
disqualifies. It is the simplest possible allocator.

Files:

| File | Contents |
|------|----------|
| `ras_testrm_component.c` | Registration; `query` gates on `hostfile`; the `hostfile` MCA param. |
| `ras_testrm.c` | `allocate`, `finalize`. |
| `ras_testrm.h` | `prte_ras_testrm_component_t` (`hostfile`) and externs. |

---

## How `allocate()` works

`allocate` simply calls
`prte_util_add_hostfile_nodes(nodes, prte_mca_ras_testrm_component.hostfile)`
and returns its status — the standard flex hostfile parser fills the
list with `prte_node_t`s (honoring `slots=` etc.), and the base's
`node_insert` places them. There is no dedup, no env probing, no
managed-allocation marking: it behaves like a scheduler allocation but
sourced from a file, which is what makes it useful for reproducible
mapper/launch tests distinct from the user-facing `--hostfile` path
(which flows through `ras/hosts`).

---

## Things to watch when editing

- `hostfile` doubles as the availability gate — no param, no component.
- Unlike `ras/hosts`, this component ignores `--host`/`--hostfile`/the
  default hostfile; it reads *only* its configured file. Keep it minimal
  — its value is being a predictable, RM-shaped test fixture.
</content>
