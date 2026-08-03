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
| `configure.m4` | Gates the build on `PRTE_CHECK_PALS`, **or** on `--enable-testbuild-launchers`. |

Because `configure.m4` gates on `PRTE_CHECK_PALS`, this component is
normally **only compiled where the PALS environment/headers are
available** — on a platform without it, it does not appear in the
framework's `static-components.h` at all.

Configure with **`--enable-testbuild-launchers`** to build it anyway. It
needs no stub headers and links nothing: its whole PALS dependency is two
`getenv` calls. That is precisely why it is worth building — a component
nothing compiles is a component that quietly stops compiling, which is
what had happened (an unused `char *tmp` made it fail `-Werror`).

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

The whole function is one call into the base:

```c
return prte_ess_base_set_identity("PALS_NODEID", 0);
```

`prte_ess_base_set_identity()` loads the nspace, parses the base vpid,
adds `PALS_NODEID`, range-checks the sum, and sets
`prte_process_info.num_daemons` — see the [framework
guide](../AGENTS.md#daemon-identity-is-established-in-one-place). A
missing `PALS_NODEID` is `PRTE_ERR_NOT_FOUND`; one holding a non-number
is refused with a diagnostic rather than defaulting the offset to 0, the
way the old `atoi` did.

Unlike `slurm`, `pals` does **not** rewrite `prte_process_info.nodename`
— it trusts the hostname already established during `prte_init`.

---

## Things to watch when editing

- **`PALS_NODEID` gates identity, `PALS_APID` gates selection.** They are
  distinct variables; do not conflate them. A daemon can be selected
  (has `PALS_APID`) yet fail `set_name` if `PALS_NODEID` is absent.
- **The node offset is load-bearing**, exactly as in `slurm`: the base
  vpid is shared, and `PALS_NODEID` disambiguates each daemon's rank.
- **Keep it library-free.** The moment this component references a real
  PALS symbol, it stops being safe to build with
  `--enable-testbuild-launchers` (no stub header covers it) and stops
  being safe to link into `libprrte`. If you genuinely need one, it has to
  move to the stub-plus-DSO treatment the `plm` PALS component gets.
- This component is daemon-only. PALS allocation/launch integration lives
  in the `ras`/`plm` frameworks; here we only bring the daemon's RTE up.
