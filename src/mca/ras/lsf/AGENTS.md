# AGENTS.md — `ras/lsf` (LSF allocator)

Component guide for `src/mca/ras/lsf/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/lsf` reads an IBM Spectrum LSF allocation through the LSF batch
API. Its `query` gates on `LSB_JOBID` being set **and** a successful
`lsb_init("PRTE launcher")`; if both hold it selects at priority **75**.

Files:

| File | Contents |
|------|----------|
| `ras_lsf_component.c` | Registration; `query` gates on `LSB_JOBID` + `lsb_init`; `skip_affinity_file` MCA param. |
| `ras_lsf_module.c` | `allocate`, `finalize`. |
| `ras_lsf.h` | Externs, including `prte_ras_lsf_skip_affinity_file`. |
| `testbuild_lsf.h` | Stand-in `lsbatch.h` used when `PRTE_TESTBUILD_LAUNCHERS` is set, so the component compiles without real LSF headers. |

---

## How `allocate()` works

`allocate` calls **`lsb_getalloc(&nodelist)`** to get the flat list of
allocated hosts (a negative return → `nodelist-failed` help and
`PRTE_ERR_NOT_AVAILABLE`). LSF returns one entry per slot, and repeats
are *adjacent*, so the module walks the list and, whenever the current
name equals the previous node's name, **bumps that node's `slots`**;
otherwise it creates a new `prte_node_t` at `PRTE_NODE_STATE_UP` with
`slots = 1`. The LSF-owned array is freed with `PMIx_Argv_free`. Returns
`PRTE_SUCCESS`; the base inserts the nodes.

---

## The affinity hostfile: not here

Despite the `skip_affinity_file` MCA param registered in this component
(`prte_ras_lsf_skip_affinity_file`), **the `LSB_AFFINITY_HOSTFILE` is not
consumed by this allocator** — node/slot discovery here is purely
`lsb_getalloc`. That param is read by the LSF **mapper**
(`src/mca/rmaps/lsf/`), which uses `LSB_AFFINITY_HOSTFILE` to derive
per-rank CPU affinity. Keep the two concerns separate: `ras/lsf`
discovers nodes+slots; `rmaps/lsf` handles affinity.

---

## Things to watch when editing

- The dedup relies on repeats being **adjacent** (only the immediately
  previous node is checked) — this matches LSF's output ordering; don't
  assume a full-list scan.
- `testbuild_lsf.h` exists so CI can build the component without LSF
  installed; keep any new LSF API you call mirrored there under
  `PRTE_TESTBUILD_LAUNCHERS`.
</content>
