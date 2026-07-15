# AGENTS.md — `plm/pals` (Cray PALS / `aprun` launcher)

Component guide for `src/mca/plm/pals/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine handlers, the daemon callback/wireup, and the base helpers
referenced throughout.

---

## Role and priority

`pals` launches the `prted` daemons on HPE/Cray systems by fork/exec'ing
**`aprun`** (the Cray PALS / ALPS launcher family). Priority **100** (MCA
`plm_pals_priority`) — the highest of all `plm` components — but it is
**only built where Cray PALS is detected** (`PRTE_CHECK_PALS`, or under
`PRTE_TESTBUILD_LAUNCHERS`), so on ordinary systems it simply doesn't
exist and never competes with ssh/slurm/lsf. Where it *is* built its
`query` offers itself unconditionally at priority 100, so it wins. Like
the other RM launchers it lets the launcher place daemons, so
`daemon_nodes_assigned_at_launch = false`.

Files:

| File | Contents |
|------|----------|
| `plm_pals_component.c` | Registration (`debug`, `priority`, `aprun`, `args`), `query` (always available → priority 100). |
| `plm_pals_module.c` | `plm_pals_init`, `plm_pals_launch_job` (spawn), `launch_daemons` (build & exec aprun), `plm_pals_start_proc` (fork/exec), `pals_wait_cb`, terminate/signal/finalize. |
| `plm_pals.h` | `prte_mca_plm_pals_component_t` (priority, debug, aprun_cmd, custom_args). |
| `help-plm-pals.txt` | Error text. |

---

## When/why selected (`prte_mca_plm_pals_component_query`)

Trivial: returns `prte_mca_plm_pals_component.priority` (default 100) and
the module, always. There is no environment probe — the *build-time*
`PRTE_CHECK_PALS` gate is what restricts this component to Cray PALS
systems. Because 100 > slurm/lsf 75 > ssh 10, wherever pals is built it
is the default launcher.

---

## Spawn → launch flow

`plm_pals_launch_job` (the `spawn` entry) activates
`PRTE_JOB_STATE_INIT` (or `MAP` for restart). **`launch_daemons`**
(registered on `PRTE_JOB_STATE_LAUNCH_DAEMONS`):

1. `prte_plm_base_setup_virtual_machine`; handle `DO_NOT_LAUNCH` and
   `num_new_daemons == 0` by fast-forwarding to `DAEMONS_REPORTED`.
2. **Build the `aprun` argv:** strip stray `PMIX_LAUNCHER_*` env vars;
   `aprun` (or `plm_pals_aprun`); any `plm_pals_args`; then the daemon
   count and layout: `-n <num_new_daemons> -N 1 --cc none` (one daemon
   per node, no CPU binding of the prted). A per-node **nodelist** (`-L`)
   block exists but is currently `#if 0`'d out — pals relies on `aprun`
   using the full allocation rather than an explicit host list.
3. **Build the `prted` argv:** `prte_plm_base_setup_prted_cmd`, then
   `prte_plm_base_prted_append_basic_args(..., "pals", &proc_vpid_index)`,
   substitute `map->daemon_vpid_start` into the vpid slot,
   `prte_plm_base_wrap_args`.
4. Copy `prte_launch_environ` (already stripped of `PRTE_`/`PMIX_`
   vars) as the child env; read the prefix(es) from the daemon job
   object.
5. Exec via `plm_pals_start_proc`; set state `DAEMONS_LAUNCHED`. On error
   jump to `cleanup:` and activate `FAILED_TO_START`.

### `plm_pals_start_proc` — fork/exec aprun

`fork()`s and, in both parent and child, records a global `palsrun`
proc; registers a `prte_wait_cb` (`pals_wait_cb`) so it notices aprun's
exit. The child rewrites `PATH`/`LD_LIBRARY_PATH` and exports
`PRTE_PREFIX`/`PMIX_PREFIX` if a prefix was set, ties stdout/stderr to
`/dev/null` (unless debugging), `setpgid`s out of prun's process group,
and `execve`s aprun.

### aprun exit handling (`pals_wait_cb`)

Like srun, `aprun` returns the highest exit code among the tasks. On a
non-zero exit the callback activates `FAILED_TO_START` if the launch
hadn't yet succeeded, else `ABORTED` (a daemon died after launch).
`plm_pals_terminate_prteds` cancels the `palsrun` wait callback (so a
normal teardown doesn't look like an aprun failure) before xcasting the
exit command; `plm_pals_signal_job` signals `palsrun` directly via
`kill()`.

---

## Key struct and MCA params

`prte_mca_plm_pals_component_t` (`plm_pals.h`):

| Field / param | Meaning |
|---------------|---------|
| `priority` (100) | Selection priority (`plm_pals_priority`). |
| `aprun_cmd` (`"aprun"`) | Launcher command (`plm_pals_aprun`) — override for wrappers. |
| `custom_args` | Extra aprun args (`plm_pals_args`). |
| `debug` | Verbose launcher debugging (`plm_pals_debug`); defaults to `prte_debug_flag`. |

---

## Things to watch when editing

- **Build gate, not runtime gate.** `query` always says yes; correctness
  relies on `PRTE_CHECK_PALS` keeping this component out of non-PALS
  builds. Don't add a runtime env probe expecting it to gate selection.
- **The `-L` nodelist path is `#if 0`'d out.** aprun currently uses the
  full allocation; if you re-enable explicit host lists, re-test the
  daemon-count/layout math (`-n`/`-N`).
- **Error paths report through `PRTE_ERROR_LOG`, not `stderr`.**
  `plm_pals_init` used to carry a stray `fprintf(stderr, "OOPS ...")`
  debug artifact on the `prte_plm_base_comm_start` error path; it has
  been removed. Don't reintroduce raw `stderr` diagnostics — use
  `PRTE_ERROR_LOG` / `pmix_show_help`.
- **RM-driven placement** → `daemon_nodes_assigned_at_launch = false`; the
  node↔daemon binding is resolved at the daemon callback.
- **Environment already stripped.** `prte_launch_environ` is the pristine,
  `PRTE_`/`PMIX_`-free copy — keep the daemon settings on the command
  line, not in the forwarded env.
