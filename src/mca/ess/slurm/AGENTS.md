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

1. Require `prte_ess_base_nspace`; load into `PRTE_PROC_MY_NAME->nspace`.
2. Require `prte_ess_base_vpid`; `strtoul` it to a base `vpid`.
3. **`PRTE_PROC_MY_NAME->rank = vpid + atoi(getenv("SLURM_NODEID"))`** —
   the base vpid plus this node's SLURM node id. This is the crucial
   difference from `env`: a single base vpid is broadcast to all
   daemons, and each adds its `SLURM_NODEID` to get a distinct rank.
4. Replace `prte_process_info.nodename` with `getenv("SLURMD_NODENAME")`
   so the daemon's hostname matches exactly what SLURM reports (missing
   → `PRTE_ERR_NOT_FOUND`). This keeps node matching consistent with the
   allocation the HNP saw.
5. Set `prte_process_info.num_daemons = prte_ess_base_num_procs`.

---

## Things to watch when editing

- **The `SLURM_NODEID` offset is load-bearing.** Getting it wrong (or
  dropping it) collides daemon ranks — a silent, miserable failure. The
  base vpid is the same for every daemon; the node id is what
  disambiguates.
- **`SLURMD_NODENAME` correction matters for node matching.** The HNP's
  allocation (from `ras/slurm`) uses SLURM's node names; if the daemon
  reports a different hostname (e.g. an FQDN vs short name), node
  reconciliation can fail. Do not remove the rename.
- **Don't add a library dependency.** SLURM support here is purely
  environmental; keep it that way so the component stays always-built.
- The `slurm` module is `PRTE_PROC_IS_DAEMON`-only. SLURM *allocation*
  discovery for the HNP lives in `ras/slurm`, and daemon *launch* in
  `plm/slurm` — this component is only the daemon's own RTE bring-up.
