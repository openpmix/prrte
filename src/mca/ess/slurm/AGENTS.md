# AGENTS.md — `ess/slurm` (daemon under SLURM)

Component guide for `src/mca/ess/slurm/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
pick-one selection model, and `prte_ess_base_prted_setup()`, which this
module wraps.

---

## Role and priority

`slurm` brings up a **`prted` daemon that was launched by `srun`** as
part of an mpirun-in-SLURM job. Priority **50** — above the generic
`env` default (1) and above `lsf` (40), tied with `pals`. It is selected
only when all three conditions hold, so it never contends with `pals`
(which requires `PALS_APID`) or `lsf` (`LSB_JOBID`) in practice.

Files:

| File | Contents |
|------|----------|
| `ess_slurm_component.c` | Registration; `prte_mca_ess_slurm_component_query` (priority 50 under SLURM). |
| `ess_slurm_module.c` | `rte_init` / `rte_finalize` + `slurm_set_name()`. |
| `ess_slurm.h` | Component struct + open/close/query prototypes. |
| `configure.m4` | Builds unconditionally (SLURM support is env-var based; no vendor library). |

Note: the `ess/slurm` component is **always built** — detecting SLURM is
just reading environment variables, so there is no `PRTE_CHECK_SLURM`
gate in its `configure.m4` the way `lsf`/`pals` gate on their libraries.

---

## Selection (`prte_mca_ess_slurm_component_query`)

```c
if (PRTE_PROC_IS_DAEMON && NULL != getenv("SLURM_JOBID")
    && NULL != prte_process_info.my_hnp_uri) {
    *priority = 50;
    *module   = &prte_ess_slurm_module;
    return PRTE_SUCCESS;
}
```

All three must hold: we are a daemon, we are inside a SLURM allocation
(`SLURM_JOBID`), and we were given a path home to the HNP
(`my_hnp_uri`). The last condition is what distinguishes "launched by
mpirun under SLURM" from merely "a SLURM allocation exists" — without a
home URI there is nothing for this daemon to attach to.

---

## `rte_init` — the SLURM daemon path

Identical three-step shape to every daemon module:

1. `prte_ess_base_std_prolog()`.
2. `slurm_set_name()` — SLURM-specific identity.
3. `prte_ess_base_prted_setup()` — the shared bring-up.

`rte_finalize` is just `prte_ess_base_prted_finalize()`.

---

## `slurm_set_name` — identity with a SLURM node offset

The only SLURM-specific logic. It differs from `env` by adding a
**per-node vpid offset** so that each `srun`-placed daemon lands on a
unique rank, and by correcting the nodename from SLURM's own value:

1. **`prte_ess_base_set_identity("SLURM_NODEID", 0)`** — the base vpid
   plus this node's SLURM node id. This is the crucial difference from
   `env`: a single base vpid is broadcast to all daemons, and each adds
   its `SLURM_NODEID` to get a distinct rank. The base helper does the
   nspace load, both parses, the range check, and `num_daemons`; see the
   [framework guide](../AGENTS.md#daemon-identity-is-established-in-one-place).
2. Replace `prte_process_info.nodename` with `getenv("SLURMD_NODENAME")`
   so the daemon's hostname matches exactly what SLURM reports (missing
   → `PRTE_ERR_NOT_FOUND`). This keeps node matching consistent with the
   allocation the HNP saw.

**Read `SLURMD_NODENAME` before freeing the old nodename.** Step 2 used
to `free(prte_process_info.nodename)` first and *then* check the
environment, so a missing `SLURMD_NODENAME` returned an error having left
that global pointing at freed memory — which every later reader,
including the error path being taken right then, would go on to use. The
order in the file now is getenv, then free, then replace. Keep it.

---

## Things to watch when editing

- **The `SLURM_NODEID` offset is load-bearing.** Getting it wrong (or
  dropping it) collides daemon ranks — a silent, miserable failure. The
  base vpid is the same for every daemon; the node id is what
  disambiguates.
- **`SLURM_NODEID` is validated, not just `NULL`-guarded.**
  `prte_ess_base_set_identity()` refuses a missing variable
  (`PRTE_ERR_NOT_FOUND`) and also one holding anything that is not a
  plain non-negative number — the old `atoi` read every such value as 0,
  which silently gave this daemon the base vpid. `srun` always sets the
  variable sanely, so this only fires in a misconfigured launch; the
  point is that such a launch fails loudly rather than forming a DVM with
  two daemons on the same rank.
- **`SLURMD_NODENAME` correction matters for node matching.** The HNP's
  allocation (from `ras/slurm`) uses SLURM's node names; if the daemon
  reports a different hostname (e.g. an FQDN vs short name), node
  reconciliation can fail. Do not remove the rename.
- **Don't add a library dependency.** SLURM support here is purely
  environmental; keep it that way so the component stays always-built.
- The `slurm` module is `PRTE_PROC_IS_DAEMON`-only. SLURM *allocation*
  discovery for the HNP lives in `ras/slurm`, and daemon *launch* in
  `plm/slurm` — this component is only the daemon's own RTE bring-up.
