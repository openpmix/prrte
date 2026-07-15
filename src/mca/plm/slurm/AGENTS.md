# AGENTS.md — `plm/slurm` (SLURM `srun` launcher)

Component guide for `src/mca/plm/slurm/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine handlers, the daemon callback/wireup, and the base helpers
referenced throughout.

---

## Role and priority

`slurm` launches the `prted` daemons by running a **single `srun`** that
starts one daemon per node across the allocation. Priority **75**,
selected when running inside a SLURM allocation. Unlike `ssh` it does
**one** launcher invocation for the whole DVM — `srun` fans out to the
nodes itself — and it lets SLURM decide proc→node placement, so PRRTE
learns each daemon's node only when the daemon phones home
(`daemon_nodes_assigned_at_launch = false`).

Built only when SLURM is detected at configure time (`PRTE_CHECK_SLURM`);
PRRTE does **not** link against any SLURM library — it just execs `srun`.

Files:

| File | Contents |
|------|----------|
| `plm_slurm_component.c` | Registration (`plm_slurm_args`), `query` (SLURM detection + `srun --version` parsing → priority 75). |
| `plm_slurm_module.c` | `plm_slurm_init`, `plm_slurm_launch_job` (spawn), `launch_daemons` (build & exec srun), `plm_slurm_start_proc` (fork/exec), `srun_wait_cb`, terminate/signal/finalize. |
| `plm_slurm.h` | `prte_mca_plm_slurm_component_t` (custom_args, early, ancient, major, minor). |
| `help-plm-slurm.txt` | Error text (no-srun, srun-failed, ancient-version, no-hosts-in-list). |

---

## When/why selected (`prte_mca_plm_slurm_component_query`)

Offers itself at priority **75** when `SLURM_JOBID` is set. It then runs
`srun --version` and parses the `major.minor` version, recording two
flags used later:

- `ancient` — older than 17.11 (`srun_wait_cb` refuses to run against it).
- `early` — older than 23.11; controls whether `--external-launcher` is
  passed (added on newer SLURM).

If `srun` can't be run or the version can't be parsed, it declines.

---

## Spawn → launch flow

`plm_slurm_launch_job` (the `spawn` entry) just activates
`PRTE_JOB_STATE_INIT` (or `MAP` for restart). The work is in
**`launch_daemons`** (registered on `PRTE_JOB_STATE_LAUNCH_DAEMONS`):

1. `prte_plm_base_setup_virtual_machine`; handle `DO_NOT_LAUNCH` and
   `num_new_daemons == 0` by fast-forwarding to `DAEMONS_REPORTED`.
2. **Build the `srun` argv:**
   - `srun`
   - `--external-launcher` (unless `early`)
   - `--ntasks-per-node=1` (one `prted` per node)
   - `--kill-on-bad-exit` — **unless** the job is recoverable/continuous
     or `prte_elastic_mode`, in which case `--no-kill
     --kill-on-bad-exit=0` (a node/daemon dying must not tear down an
     elastic DVM).
   - `--mpi=none` (daemons aren't MPI tasks), `--cpu-bind=none` (don't
     let TaskAffinity pin the prted to one core).
   - any `plm_slurm_args`.
   - a **nodelist**: the map's new-daemon nodes (skipping ones that
     already have a daemon); errors `no-hosts-in-list` if empty.
   - `--jobid=<id>` — taken from the first node's `PRTE_NODE_ALLOC_ID`
     attribute and the matching session; lets PRRTE launch into another
     allocation than the one it's in. Errors if no job id / session.
   - `--nodes=N --nodelist=...` (only when not using the whole
     allocation) and `--ntasks=N` where `N = num_new_daemons`.
3. **Build the `prted` argv** appended after srun's:
   `prte_plm_base_setup_prted_cmd`, then
   `prte_plm_base_prted_append_basic_args(..., "slurm", &proc_vpid_index)`.
   Substitute `map->daemon_vpid_start` into the vpid slot — SLURM starts
   the tasks and each daemon offsets from this base to compute its own
   vpid. `prte_plm_base_wrap_args` quotes multi-word args (in case srun is
   wrapped by a script).
4. Read the prefix(es) from the daemon job object and exec via
   `plm_slurm_start_proc`. Set state `DAEMONS_LAUNCHED`. On any error jump
   to `cleanup:` and activate `FAILED_TO_LAUNCH`.

### `plm_slurm_start_proc` — fork/exec srun

`fork()`s; the parent records the srun pid (the first one becomes the
`primary_srun_pid`) and registers a `prte_wait_cb` (`srun_wait_cb`) on a
dummy proc so it notices srun's exit. The child:

- **Purges `PMIX_*`/`PRTE_*` from the environment** — SLURM forwards the
  whole environment to the daemons, which we must not do (it could carry
  tool-connection envars); everything needed is on the command line.
- Rewrites `PATH`/`LD_LIBRARY_PATH` and exports `PRTE_PREFIX`/
  `PMIX_PREFIX` if a prefix was set (so srun propagates them).
- Ties stdout/stderr to `/dev/null` unless debugging, `setpgid`s out of
  prun's process group (so shell `Ctrl-C` doesn't hit srun), and
  `execvp`s srun.

---

## srun exit handling (`srun_wait_cb`)

`srun` returns the **highest** exit code among the tasks it ran, so a
non-zero status could be srun itself failing *or* a `prted` exiting
badly — either way the job didn't start, so the pid reported is
meaningless (it's srun's, not the failed proc's). The callback:

- Refuses to proceed against an `ancient` SLURM (`ancient-version`).
- On non-zero exit → `srun-failed` help + activate
  `DAEMONS_TERMINATED`.
- On clean exit of the **primary** srun → fire `DAEMONS_TERMINATED` (set
  `num_terminated = num_procs` first to avoid a bogus error message) so
  `prun`/the HNP can exit.

`plm_slurm_terminate_prteds` similarly special-cases the "we never
launched additional daemons" case (`primary_pid_set == false`) by firing
`DAEMONS_TERMINATED` directly instead of waiting on a nonexistent srun.

---

## Key struct and MCA param

`prte_mca_plm_slurm_component_t` (`plm_slurm.h`): `custom_args` (MCA
`plm_slurm_args`, appended to srun), plus the version-detection fields
`early`, `ancient`, `major`, `minor` set by `query`. `signal_job` just
forwards to `prte_plm_base_prted_signal_local_procs` (signals go through
the daemons, not srun).

---

## Things to watch when editing

- **One srun, RM-driven placement.** There is no per-node launch loop
  like ssh — srun places the daemons. Hence
  `daemon_nodes_assigned_at_launch = false`; don't assume a node↔daemon
  binding before the callback.
- **Version gates are load-bearing.** `--external-launcher` (23.11+) and
  the `ancient`/`early` flags come straight from parsed `srun --version`;
  keep the parsing and the flag semantics in sync.
- **Elastic mode changes the kill flags.** `--no-kill
  --kill-on-bad-exit=0` in elastic/recoverable/continuous mode is
  deliberate — a node loss must not kill the whole srun.
- **Environment purge in the child is mandatory** — SLURM forwards the
  full environment; leaving `PMIX_`/`PRTE_` vars in breaks tool
  connections and duplicates command-line settings.
