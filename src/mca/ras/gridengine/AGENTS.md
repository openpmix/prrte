# AGENTS.md — `ras/gridengine` (Grid Engine / SGE allocator)

Component guide for `src/mca/ras/gridengine/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/gridengine` reads a Sun/Univa/Altair Grid Engine (SGE) parallel-
environment allocation from `PE_HOSTFILE`. Its `query` selects at the
configurable priority `ras_gridengine_priority` (default **100**) only
when the full SGE environment is present: `SGE_ROOT`, `ARC`,
`PE_HOSTFILE`, and `JOB_ID` all set.

Files:

| File | Contents |
|------|----------|
| `ras_gridengine_component.c` | Registration; `query` gates on the four SGE env vars; `priority`, `verbose`, `show_jobid` MCA params. |
| `ras_gridengine_module.c` | `allocate`, `finalize`. |
| `ras_gridengine.h` | Component struct (`priority`, `verbose`, `show_jobid`) and externs. |

---

## How `allocate()` works

`prte_ras_gridengine_allocate` requires `PE_HOSTFILE` and `JOB_ID` (else
`PRTE_ERR_TAKE_NEXT_OPTION`), optionally echoes `JOB_ID`, then parses the
PE hostfile line by line. Each line is
`hostname slots queue arch` (whitespace/newline separated,
`strtok_r`). For each host it either bumps an existing node's `slots` by
the parsed count or creates a new `prte_node_t` at `PRTE_NODE_STATE_UP`
with `slots` from the file. An empty result is unrecoverable →
`no-nodes-found` help and `PRTE_ERR_NOT_FOUND`; otherwise `PRTE_SUCCESS`.

Verbosity is controlled by the component's own `verbose` output stream
(opened in `open` when `ras_gridengine_verbose != 0`), separate from the
framework `ras_base_verbose`.

---

## Things to watch when editing

- The parser tokenizes on `" \n"` and reads exactly four fields; only
  `hostname` and `slots` are used (queue/arch are logged, not stored).
- `query` is strict about **all four** env vars — SGE sets them
  together; loosening the gate risks selecting on a non-SGE machine.
- `get_slot_count` is `#if 0`-guarded dead code; ignore it.
</content>
