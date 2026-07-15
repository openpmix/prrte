# AGENTS.md — `ess/lsf` (daemon under IBM LSF)

Component guide for `src/mca/ess/lsf/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
pick-one selection model, and `prte_ess_base_prted_setup()`, which this
module wraps.

---

## Role and priority

`lsf` brings up a **`prted` daemon launched under IBM Spectrum LSF**.
Priority **40** — above the generic `env` default (1) but below
`slurm`/`pals` (50). The environment gates are mutually exclusive in
practice, so the ordering rarely matters; it exists so that if two RM
markers were somehow both present, the choice is deterministic.

Files:

| File | Contents |
|------|----------|
| `ess_lsf_component.c` | Registration; `prte_mca_ess_lsf_component_query` (priority 40 under LSF). |
| `ess_lsf_module.c` | `rte_init` / `rte_finalize` + `lsf_set_name()`. |
| `ess_lsf.h` | Component struct + open/close/query prototypes. |
| `configure.m4` | Gates the build on `PRTE_CHECK_LSF` — only built where the LSF libraries are present. |

Because `configure.m4` gates on `PRTE_CHECK_LSF` (and sets
`ess_lsf_CPPFLAGS`/`LDFLAGS`/`LIBS`), this component is **only compiled
where LSF is available**. On a platform without LSF it will not appear in
the framework's `static-components.h`.

---

## Selection (`prte_mca_ess_lsf_component_query`)

```c
if (PRTE_PROC_IS_DAEMON && NULL != getenv("LSB_JOBID")
    && NULL != prte_process_info.my_hnp_uri) {
    *priority = 40;
    *module   = &prte_ess_lsf_module;
    return PRTE_SUCCESS;
}
```

All three: we are a daemon, we are inside an LSF batch job (`LSB_JOBID`),
and we have a home URI to the HNP.

---

## `rte_init` — the LSF daemon path

The standard three-step daemon shape:

1. `prte_ess_base_std_prolog()`.
2. `lsf_set_name()` — LSF-specific identity.
3. `prte_ess_base_prted_setup()` — the shared bring-up.

`rte_finalize` is `prte_ess_base_prted_finalize()`.

---

## `lsf_set_name` — identity with a 1-based LSF task offset

1. Require `prte_ess_base_nspace`; load into `PRTE_PROC_MY_NAME->nspace`.
2. Require `prte_ess_base_vpid`; `strtoul` it to a base `vpid`.
3. **`PRTE_PROC_MY_NAME->rank = vpid + atoi(getenv("LSF_PM_TASKID")) - 1`**
   — note the **`- 1`**: `LSF_PM_TASKID` is **1-based** (LSF's process
   manager numbers tasks from 1), so it is decremented to a 0-based
   offset before being added to the base vpid. This is the single most
   important detail in the file; the other RM modules
   (`slurm`/`pals`) use 0-based node ids and do not subtract.
4. Set `prte_process_info.num_daemons = prte_ess_base_num_procs`.

Like `pals` (and unlike `slurm`), `lsf` does **not** rewrite
`prte_process_info.nodename`.

---

## Things to watch when editing

- **The `- 1` is not a typo.** `LSF_PM_TASKID` counts from 1; dropping
  the decrement shifts every daemon's rank by one and collides ranks.
  This is the classic bug to avoid here.
- **Build gating.** Any new LSF symbol must be covered by
  `PRTE_CHECK_LSF` in `configure.m4`, or the build breaks on non-LSF
  systems. The wrapper flags (`ess_lsf_CPPFLAGS`/`LDFLAGS`/`LIBS`) are
  substituted from there.
- **Minor quirk:** `rte_finalize` has a stray trailing empty statement
  (`;` after `return`); harmless dead code, but if you touch the file it
  is worth cleaning up.
- Daemon-only. LSF allocation/launch integration lives in the `ras`/`plm`
  frameworks; this component is only the daemon's own RTE bring-up.
