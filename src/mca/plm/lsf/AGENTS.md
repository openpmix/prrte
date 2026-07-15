# AGENTS.md — `plm/lsf` (IBM Spectrum LSF launcher)

Component guide for `src/mca/plm/lsf/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine handlers, the daemon callback/wireup, and the base helpers
referenced throughout.

---

## Role and priority

`lsf` launches the `prted` daemons through the **LSF `lsb_launch()` API**
— a direct library call, not a fork/exec of an external command. Priority
**75**, selected inside an LSF batch job. Like SLURM it does one launch
call for the whole DVM and lets LSF place the daemons, so
`daemon_nodes_assigned_at_launch = false`.

This is the one `plm` component that **links against a vendor library**
(`lsf/lsbatch.h`, `-llsf`/`-lbat`), gated at configure time by
`PRTE_CHECK_LSF`. When `PRTE_TESTBUILD_LAUNCHERS` is set it builds
against the stub `testbuild_lsf.h` instead (so CI can compile the code
path without LSF installed).

Files:

| File | Contents |
|------|----------|
| `plm_lsf_component.c` | `query`: checks `LSB_JOBID`, rejects IBM CSM, calls `lsb_init` → priority 75. |
| `plm_lsf_module.c` | `plm_lsf_init`, `plm_lsf_launch_job` (spawn), `launch_daemons` (build argv + `lsb_launch`), terminate/signal/finalize. |
| `plm_lsf.h` | `prte_mca_plm_lsf_component_t` (just a `timing` flag). |
| `testbuild_lsf.h` | Stub LSF declarations for `PRTE_TESTBUILD_LAUNCHERS` builds. |
| `help-plm-lsf.txt` | Error text (`lsb_launch-failed`). |

---

## When/why selected (`prte_mca_plm_lsf_component_query`)

Declines unless **all** hold: `LSB_JOBID` is set, `CSM_ALLOCATION_ID` is
**not** set (IBM CSM systems use a different launch path), and
`lsb_init("PRTE launcher")` succeeds. On success it returns priority 75
and the module. There is no version probing.

---

## Spawn → launch flow

`plm_lsf_launch_job` (the `spawn` entry) activates
`PRTE_JOB_STATE_INIT` (or `MAP` for restart). **`launch_daemons`**
(registered on `PRTE_JOB_STATE_LAUNCH_DAEMONS`):

1. `prte_plm_base_setup_virtual_machine`; handle `DO_NOT_LAUNCH` and
   `num_new_daemons == 0` by fast-forwarding to `DAEMONS_REPORTED`.
2. Build the **nodelist** (`nodelist_argv`): the map's new-daemon nodes,
   skipping ones that already have a daemon. This becomes `lsb_launch`'s
   host-list argument.
3. Build the **`prted` argv**: strip stray `PMIX_LAUNCHER_*` env vars,
   `prte_plm_base_setup_prted_cmd`, then
   `prte_plm_base_prted_append_basic_args(..., "lsf", &proc_vpid_index)`,
   substitute `map->daemon_vpid_start` into the vpid slot,
   `prte_plm_base_wrap_args`.
4. Build the **environment** (`env`): copy `prte_launch_environ` (already
   stripped of `PRTE_`/`PMIX_` vars), then rewrite `PATH`/
   `LD_LIBRARY_PATH` and set `PRTE_PREFIX` / `PMIX_PREFIX` if a prefix
   was set on the daemon job.
5. **Disable the SIGCHLD handler** (`prte_wait_disable()`) around the
   call — `lsb_launch` tampers with SIGCHLD and leaves its handler NULL.
6. Call **`lsb_launch(nodelist_argv, argv, LSF_DJOB_REPLACE_ENV |
   LSF_DJOB_NOWAIT, env)`**. `NOWAIT` is essential: without it the call
   would block until the daemons *exit*; we need it to return so launch
   can proceed. `REPLACE_ENV` makes the daemons use our `env`. On failure
   → `lsb_launch-failed` help (with `lsberrno`/`lsb_sysmsg`) and
   `FAILED_TO_START`.
7. Re-enable the SIGCHLD handler (`prte_wait_enable()`), set state
   `DAEMONS_LAUNCHED`.

Because `lsb_launch` is an async library call (not a forked process),
there is **no `wait_cb`** as in slurm/pals — failures surface via the
`lsb_launch` return code and the normal daemon callback/failure path.

---

## Termination and signalling

All the base defaults: `terminate_orteds` → `prte_plm_base_prted_exit`,
`terminate_job`/`terminate_procs`/`signal_job` → the base xcast helpers.
`finalize` stops the recvs.

---

## Things to watch when editing

- **The SIGCHLD disable/enable bracket around `lsb_launch` is mandatory.**
  `lsb_launch` clobbers the handler; forgetting to restore it breaks
  PRRTE's child-reaping.
- **`LSF_DJOB_NOWAIT` must stay** — a blocking `lsb_launch` would hang the
  progress thread until the daemons die.
- **RM-driven placement** → `daemon_nodes_assigned_at_launch = false`; the
  node↔daemon binding is resolved at the daemon callback, not here.
- **CSM exclusion.** The `CSM_ALLOCATION_ID` guard in `query` keeps this
  component from grabbing IBM CSM systems — don't drop it.
- **Test builds.** Code must keep compiling under
  `PRTE_TESTBUILD_LAUNCHERS` against `testbuild_lsf.h`; don't reach for
  LSF symbols not stubbed there.
