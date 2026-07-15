# AGENTS.md — `ess/pals` (daemon under HPE/Cray PALS)

Component guide for `src/mca/ess/pals/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
pick-one selection model, and `prte_ess_base_prted_setup()`, which this
module wraps.

---

## Role and priority

`pals` brings up a **`prted` daemon launched by the PALS variant of
`aprun`** (HPE/Cray's Parallel Application Launch Service). Priority
**50** — above the generic `env` default (1) and above `lsf` (40), tied
with `slurm`; the mutually exclusive environment gates keep them from
actually contending.

Files:

| File | Contents |
|------|----------|
| `ess_pals_component.c` | Registration; `prte_mca_ess_pals_component_query` (priority 50 under PALS). |
| `ess_pals_module.c` | `rte_init` / `rte_finalize` + `pals_set_name()`. |
| `ess_pals.h` | Component struct + open/close/query prototypes. |
| `configure.m4` | Gates the build on `PRTE_CHECK_PALS` — only built where PALS is present. |

Because `configure.m4` gates on `PRTE_CHECK_PALS`, this component is
**only compiled where the PALS environment/headers are available**. On a
platform without PALS it will not appear in the framework's
`static-components.h` at all.

---

## Selection (`prte_mca_ess_pals_component_query`)

```c
if (PRTE_PROC_IS_DAEMON && NULL != getenv("PALS_APID")
    && NULL != prte_process_info.my_hnp_uri) {
    *priority = 50;
    *module   = &prte_ess_pals_module;
    return PRTE_SUCCESS;
}
```

All three: we are a daemon, we are inside a PALS application
(`PALS_APID`, the PALS application id), and we have a home URI to the
HNP. Note the gate uses `PALS_APID` while identity uses `PALS_NODEID`
(below) — different variables for different purposes.

---

## `rte_init` — the PALS daemon path

The standard three-step daemon shape:

1. `prte_ess_base_std_prolog()`.
2. `pals_set_name()` — PALS-specific identity.
3. `prte_ess_base_prted_setup()` — the shared bring-up.

`rte_finalize` is `prte_ess_base_prted_finalize()`.

---

## `pals_set_name` — identity with a PALS node offset

1. Require `prte_ess_base_nspace`; load into `PRTE_PROC_MY_NAME->nspace`.
2. Require `prte_ess_base_vpid`; `strtoul` it to a base `vpid`.
3. **`PRTE_PROC_MY_NAME->rank = vpid + atoi(getenv("PALS_NODEID"))`**,
   but only if `PALS_NODEID` is present — if it is **not** set,
   `pals_set_name` returns `PRTE_ERR_NOT_FOUND` rather than defaulting
   the offset to 0. (Contrast `slurm`, which calls `atoi` on
   `SLURM_NODEID` unconditionally.)
4. Set `prte_process_info.num_daemons = prte_ess_base_num_procs`.

Unlike `slurm`, `pals` does **not** rewrite `prte_process_info.nodename`
— it trusts the hostname already established during `prte_init`.

---

## Things to watch when editing

- **`PALS_NODEID` gates identity, `PALS_APID` gates selection.** They are
  distinct variables; do not conflate them. A daemon can be selected
  (has `PALS_APID`) yet fail `set_name` if `PALS_NODEID` is absent.
- **The node offset is load-bearing**, exactly as in `slurm`: the base
  vpid is shared, and `PALS_NODEID` disambiguates each daemon's rank.
- **Build gating.** Any new PALS dependency must be reflected in
  `configure.m4`'s `PRTE_CHECK_PALS`; do not introduce a hard PALS
  reference that breaks the build on non-PALS systems.
- This component is daemon-only. PALS allocation/launch integration lives
  in the `ras`/`plm` frameworks; here we only bring the daemon's RTE up.
