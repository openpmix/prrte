# AGENTS.md — `ess/env` (the generic daemon default)

Component guide for `src/mca/ess/env/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
pick-one selection model, and `prte_ess_base_prted_setup()`, which this
module is a thin wrapper around.

---

## Role and priority

`env` brings up a **generic `prted` daemon** — the fallback for any
daemon that is not running under a recognized resource manager. It is
the lowest-priority daemon component, priority **1**, so any
RM-specific module (`slurm`/`pals`/`lsf`) that recognizes its
environment outranks it. In practice `env` is what serves an
**ssh-launched** daemon (the `plm/ssh` component), where the daemon's
identity is handed to it directly via MCA parameters rather than derived
from an RM.

Files:

| File | Contents |
|------|----------|
| `ess_env_component.c` | Registration; `prte_mca_ess_env_component_query` returning priority 1 iff `PRTE_PROC_IS_DAEMON`. |
| `ess_env_module.c` | `rte_init` / `rte_finalize` + `env_set_name()`. |
| `ess_env.h` | Component struct + open/close/query prototypes. |

---

## Selection (`prte_mca_ess_env_component_query`)

```c
if (PRTE_PROC_IS_DAEMON) {
    *priority = 1;
    *module   = &prte_ess_env_module;
    return PRTE_SUCCESS;
}
*priority = -1; *module = NULL; return PRTE_ERROR;
```

It is available to **any** daemon (the source comment: "only used by
daemons that are launched by ssh so allow any enviro-specific modules to
override us"). Because its priority is 1 and every RM module gates on a
positive check with priority ≥40, `env` wins only when no RM module
claims the process.

---

## `rte_init` — the minimal daemon path

`rte_init` is the canonical example of a daemon module — three steps:

1. **`prte_ess_base_std_prolog()`** — `prte_dt_init` + `prte_wait_init`.
2. **`env_set_name()`** — establish this daemon's identity from the base
   MCA params (see below).
3. **`prte_ess_base_prted_setup()`** — the entire shared daemon bring-up
   (signals, topology, state/errmgr/grpcomm/odls/rmaps/iof/filem/comms,
   PMIx server, session dir, job/proc objects). See the framework guide.

`rte_finalize` is just `prte_ess_base_prted_finalize()`.

All the environment-specific logic in this component is the ~25-line
`env_set_name`.

---

## `env_set_name` — identity from parameters

Unlike the RM modules, `env` takes the daemon's vpid **verbatim** — there
is no per-node offset to add, because the ssh launcher assigns each
daemon a distinct vpid directly:

1. Require `prte_ess_base_nspace` (from `ess_base_nspace`); load it into
   `PRTE_PROC_MY_NAME->nspace`. Missing → `PRTE_ERR_NOT_FOUND`.
2. Require `prte_ess_base_vpid` (from `ess_base_vpid`); `strtoul` it into
   `PRTE_PROC_MY_NAME->rank`. Missing → `PRTE_ERR_NOT_FOUND`.
3. Set `prte_process_info.num_daemons = prte_ess_base_num_procs`.

These three parameters (`ess_base_nspace`, `ess_base_vpid`,
`ess_base_num_procs`) are the standard channel by which the HNP tells a
launched daemon who it is; they are set on the daemon's command
line/environment by the PLM. The launcher-less bootstrap path
(`ess_base_bootstrap.c`) publishes the same three parameters, so a
bootstrapped ordinary daemon also comes up through `env`.

---

## Things to watch when editing

- **No vpid offset here — that is deliberate.** If you find yourself
  wanting to add `+ nodeid`, you are writing an RM module, not editing
  `env`. Keep `env` the verbatim-identity default.
- **`set_name`'s return code is checked.** `rte_init` now aborts to its
  `error:` label if `env_set_name()` fails (a missing `nspace`/`vpid`
  yields `PRTE_ERR_NOT_FOUND`), rather than falling through into
  `prted_setup` with a half-built identity. All four daemon modules
  (`env`/`slurm`/`pals`/`lsf`) do this consistently — keep it that way if
  you add another.
- **This is the model to copy.** A new generic-daemon variant should
  follow this exact shape: `std_prolog` → `set_name` → `prted_setup`.
