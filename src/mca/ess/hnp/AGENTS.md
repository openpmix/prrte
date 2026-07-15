# AGENTS.md — `ess/hnp` (the DVM master)

Component guide for `src/mca/ess/hnp/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
pick-one selection model, and the shared base functions referenced
throughout.

---

## Role and priority

`hnp` brings up the **HNP (Head Node Process) / DVM master** — the
`prte` process that controls the Distributed Virtual Machine. It is the
highest-priority `ess` component, priority **100**, and is selected in
exactly one process: the one where `PRTE_PROC_IS_MASTER` is true. No
other role ever selects it, and it never contends with the daemon
modules (they gate on `PRTE_PROC_IS_DAEMON`).

Files:

| File | Contents |
|------|----------|
| `ess_hnp_component.c` | Registration; `hnp_component_query` returning priority 100 iff `PRTE_PROC_IS_MASTER`. |
| `ess_hnp_module.c` | `rte_init` / `rte_finalize` — the full HNP bring-up and tear-down. |
| `ess_hnp.h` | Declares the component struct only (no module-private API). |

Unlike the daemon modules, `hnp` does **not** call the shared
`prte_ess_base_prted_setup()`. It opens a similar but distinct set of
frameworks inline in `rte_init`, because the master has extra
responsibilities (it names itself via the PLM, discovers the allocation
via `ras`, and builds its own node object).

---

## Selection (`hnp_component_query`)

```c
if (PRTE_PROC_IS_MASTER) {
    *priority = 100;
    *module   = &prte_ess_hnp_module;
    return PRTE_SUCCESS;
}
*priority = -1; *module = NULL; return PRTE_ERROR;
```

The `PRTE_PROC_MASTER` bit in `prte_process_info.proc_type` is set by the
`prte` tool during its own startup, before `prte_init` opens `ess`. So by
the time `query` runs, the process already knows it is the master.

---

## `rte_init` — HNP bring-up

`rte_init(argc, argv)` in `ess_hnp_module.c` is the master's startup
sequence. Its ordering matters; the highlights, in order:

1. **`prte_ess_base_std_prolog()`** — `prte_dt_init` + `prte_wait_init`
   (shared with every module).
2. **Topology discovery** — `prte_hwloc_base_get_topology()` if not
   already set.
3. **`state`** framework open + select.
4. **`errmgr`** framework open (selected later, after comms).
5. **`plm`** open + select, then **`prte_plm.set_hnp_name()`** — the HNP
   name (nspace + rank 0) is defined by the PLM component for this
   environment, which is why `plm` must be opened this early. A
   `PRTE_ERR_FATAL` from `plm` select is downgraded to `PRTE_ERR_SILENT`
   (the PLM already showed help).
6. **Daemon job object** — create the `prte_job_t` for the daemon job,
   register it (`prte_set_job_data_object`), assign the `"prte"` schizo
   personality by default, attach it to `prte_default_session`, and mark
   it `PRTE_JOB_STATE_DAEMONS_REPORTED`. Add one app context (argv[0] +
   argv), and build the HNP's own `prte_node_t` (this node) and
   `prte_proc_t` (this daemon), wiring the proc into the node's `daemon`
   field and flagging the node UP / DAEMON_LAUNCHED / LOC_VERIFIED. The
   daemon job is marked RUNNING with one proc reported.
7. **Session directory** — `prte_session_dir(PRTE_PROC_MY_NAME)`.
8. **PMIx server** — `pmix_server_init()`, then gather interface aliases
   (`pmix_ifgetaliases`) and copy them onto the node object
   (`node->aliases`). Emit the XML start tag if `prte_xml_output`.
9. **Comms** — open/select `prtereachable`, `prte_rml_open()`, then
   `pmix_server_start()`.
10. **`grpcomm`** open + select, then **`errmgr` select**.
11. **`prte_plm.init()`** — module-specific PLM init, after comms (may
    start a non-blocking recv).
12. **`ras`** open + select — resource allocation discovery. This is a
    key HNP-only step the daemon path never performs.
13. **`rmaps`** open + select. Add the local topology to
    `prte_node_topologies`, set `node->topology` and
    `node->available` (the filtered CPU set).
14. **`odls`** open + select.
15. Redirect `pmix_output` into the proc-specific session dir.
16. **`iof`** and **`filem`** open + select.

On any failure it shows `prte_init:startup:internal-failure` (unless
`PRTE_ERR_SILENT`/`prte_report_silent_errors`), releases the partially
built `jdata` (which cleans up the session directory tree), and returns
`PRTE_ERR_SILENT`.

The frameworks the HNP opens but the daemon path does **not** are
`ras` and (unconditionally) `plm` — the master must discover the
allocation and name/launch daemons. Conversely, the daemon path opens
`plm` only when `PRTE_MCA_plm` is set.

---

## `rte_finalize` — HNP tear-down

Reverse order: finalize `errmgr`; close
`filem`/`grpcomm`/`iof`/`plm`; kill local procs
(`prte_odls.kill_local_procs`) unless `prte_abnormal_term_ordered`; close
`odls`; close the `rml`; close `prtereachable`/`errmgr`/`state`; emit the
XML end tag; finalize the PMIx server; flush stdout/stderr.

---

## Key structs the HNP builds

The HNP is the only role that constructs the *authoritative* DVM data
structures for itself at init:

- `prte_job_t` for the daemon job (nspace = my nspace), stored via
  `prte_set_job_data_object`, tied to `prte_default_session`.
- `prte_node_t` for this node, placed in `prte_node_pool` at
  `PRTE_PROC_MY_NAME->rank`, with `daemon`, `topology`, `available`,
  `aliases`, and the UP/LAUNCHED/VERIFIED flags all set.
- `prte_proc_t` for this daemon, in the job's `procs` array and
  cross-referenced from `node->daemon`.

These are the seeds the rest of the launch machinery grows: `ras`
populates more nodes, `rmaps` maps jobs onto them, `plm` launches
daemons onto them.

---

## Things to watch when editing

- **Do not reorder the early PLM steps.** The HNP name comes from
  `prte_plm.set_hnp_name()`, so `plm` must be opened/selected before the
  job/proc/node objects (which key off `PRTE_PROC_MY_NAME`) are built.
- **`ras` and `rmaps` are HNP-only.** Keep allocation/mapping bring-up
  here, not in the shared `prted_setup` — daemons must never open them.
- **Session-dir cleanup rides on `jdata`.** The error path releases
  `jdata` to tear down the session directory tree; if you add new
  early-out paths before `jdata` exists, guard the `NULL != jdata` check
  (already present) and do not leak the tree.
- **This module intentionally diverges from `prted_setup`.** Resist the
  urge to "unify" it with the daemon path — the master's extra steps
  (self node object, ras, plm naming) are the whole point.
