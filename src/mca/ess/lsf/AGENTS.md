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
| `configure.m4` | Gates the build on `PRTE_CHECK_LSF`, **or** on `--enable-testbuild-launchers`. |

Because `configure.m4` gates on `PRTE_CHECK_LSF`, this component is
normally **only compiled where LSF is available** — on a platform without
it, it does not appear in the framework's `static-components.h` at all.

Configure with **`--enable-testbuild-launchers`** to build it anyway. It
needs no stub headers and links nothing: as the `Makefile.am` says, the
plugin calls no LSF library function, so its whole LSF dependency is two
`getenv` calls. That is precisely why it is worth building — a component
nothing compiles is a component that quietly stops compiling, which is
what had happened.

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

The whole function is one call into the base:

```c
return prte_ess_base_set_identity("LSF_PM_TASKID", -1);
```

— note the **`-1`**: `LSF_PM_TASKID` is **1-based** (LSF's process
manager numbers tasks from 1), so it is decremented to a 0-based offset
before being added to the base vpid. This is the single most important
detail in the file; the other RM modules (`slurm`/`pals`) use 0-based
node ids and pass `0`.

`prte_ess_base_set_identity()` does the rest — nspace, the base-vpid
parse, the range check, and `num_daemons`; see the [framework
guide](../AGENTS.md#daemon-identity-is-established-in-one-place). It
computes the sum as a **signed** value precisely because of this
component: a `LSF_PM_TASKID` of 0 with the `-1` adjustment would
otherwise wrap around to a rank up near `UINT32_MAX` instead of being
refused.

Like `pals` (and unlike `slurm`), `lsf` does **not** rewrite
`prte_process_info.nodename`.

---

## Things to watch when editing

- **The `- 1` is not a typo.** `LSF_PM_TASKID` counts from 1; dropping
  the decrement shifts every daemon's rank by one and collides ranks.
  This is the classic bug to avoid here.
- **`LSF_PM_TASKID` is validated, not just `NULL`-guarded.** A missing
  variable is `PRTE_ERR_NOT_FOUND`; a non-numeric one is refused with a
  diagnostic rather than read as 0 by `atoi`. The LSF process manager
  sets it when it launches the daemon, so this only fires in a
  misconfigured launch — the point is that such a launch fails loudly.
- **Build it before you claim it compiles.** This component is not built
  on a developer machine, and it had drifted into not compiling at all
  under the project's `-Wall -Wextra -Werror`: `rte_init` was missing its
  `PRTE_HIDE_UNUSED_PARAMS(argc, argv)` and a `my_node_rank` static sat
  unused. Configure a tree with `--enable-testbuild-launchers` and build
  it; an ordinary build will not tell you.
- **Keep it library-free.** The moment this component references a real
  LSF symbol, it stops being safe to build with
  `--enable-testbuild-launchers` (no stub header covers it) and stops
  being safe to link into `libprrte`. If you genuinely need one, it has to
  move to the stub-plus-DSO treatment the `plm`/`ras` LSF components get.
- Daemon-only. LSF allocation/launch integration lives in the `ras`/`plm`
  frameworks; this component is only the daemon's own RTE bring-up.
