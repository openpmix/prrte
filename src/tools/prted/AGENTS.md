# AGENTS.md — `src/tools/prted` (the daemon's `main()`)

Orientation for the PRRTE daemon executable. Start with
[`src/tools/AGENTS.md`](../AGENTS.md) for what all the tools share, and
with [`src/prted/AGENTS.md`](../../prted/AGENTS.md) for the daemon *body*
— this directory holds only the startup, and everything the daemon does
afterwards lives there.

---

## What this file is

`prted.c` is not a wrapper. It is the only tool `main()` that runs a full
PRRTE runtime, and it contains the entire **check-in sequence**: how a
freshly launched daemon tells the DVM it exists, and how a tree-spawned
daemon collects its children's check-ins before passing one message up.

Once the event loop at the bottom starts, control belongs to
`prted_comm.c` and the frameworks. Nothing in this file runs again.

---

## The startup order, and why each step is where it is

```
umask from PRTE_DAEMON_UMASK_VALUE   <- before anything creates a file
unsetenv PRTE_MCA_schizo_proxy/personality
save prte_launch_environ             <- BEFORE we add any PMIX_/PRTE_ vars of our own
parse_prte / parse_pmix              <- MCA params, before first registration
scan argv for --bootstrap            <- sets prte_bootstrap_setup
prte_init_util(PRTE_PROC_DAEMON)     <- registers MCA vars; reads the env set above
schizo open/select/detect_proxy
schizo->parse_cli(...)               <- SILENT: a daemon has no user to warn
prte_ess_base_bootstrap()            <- bootstrap only: am I the controller?
prte_register_params()
daemonize -- ONLY on --daemonize, and not when debugging (see below)
prte_init(MASTER or DAEMON)          <- the whole runtime
[bind to prte_daemon_cores]
register PRTE_RML_TAG_DAEMON recv    <- from here on we can be given orders
check-in sequence (below)
event loop
```

- **A `prted` forks only when its launcher tells it to.** The fork +
  `setsid()` in `prte_daemon_init_callback` leaves the launcher tracking
  a process that exits the moment the real daemon is up, and whether that
  is a favor or a fatal mistake depends entirely on who is doing the
  tracking:
  - `plm/ssh` **wants** it, and passes `--daemonize`. The ssh session
    closes, and without tree spawn the concurrency slot frees so the next
    group of daemons can go out.
  - A resource manager does **not**. `srun`, `lsb_launch` and `palsd`
    each hand the daemon a task slot and watch the process they started;
    detaching from it tells the RM the task finished. Under Slurm that
    ends the step, and `slurmstepd` then SIGKILLs whatever is left in the
    step's cgroup — which includes the daemon, because `setsid()` escapes
    a process tree and not a cgroup. `plm/slurm`, `plm/lsf` and
    `plm/pals` therefore withhold `--daemonize`, and the daemon remains
    the task. (This was
    [issue #2757](https://github.com/openpmix/prrte/issues/2757): the
    detach was unconditional and `--daemonize`, though `prted` accepts it,
    was never read. It was survivable only for as long as a separate bug
    left the fork's parent blocked forever, accidentally keeping the step
    alive.)

  Even when it does not fork, a non-debug `prted` still points its
  stdin/stdout/stderr at `/dev/null` via `prte_daemon_detach_io()` — it
  has no more business writing on the launcher's terminal than a
  daemonized one does. `--leave-session-attached` and `--debug-daemons`
  suppress both halves, which is the whole point of them.

- **The pristine environment must be captured before we add to it.**
  `prte_launch_environ` is what every application process inherits; it is
  built by copying `environ` and dropping everything starting with
  `PMIX_` or `PRTE_`. Anything you `setenv()` earlier leaks into every
  app in the job.
- **`--bootstrap` is found by scanning argv directly**, before the CLI
  parser runs, because a bootstrapping daemon has to publish the DVM-wide
  MCA parameters *before* `prte_init_util` registers them. The comment in
  the source is the authority here.
- **A bootstrapped daemon that finds it is on the controller host
  promotes itself to HNP** by setting `prte_process_info.proc_type =
  PRTE_PROC_MASTER` before `prte_init`, then jumps straight to the event
  loop (`goto bootstrap_wait`) — it does *not* run the check-in
  sequence, because in a bootstrapped DVM the daemons phone it, not the
  other way round.
- **`umask` is applied first of all** and an unparsable value is
  *ignored*, not obeyed. Reading an empty string as `0` — which the old
  inline `strtol` did — leaves every file the daemon creates
  world-writable. The parser is
  `prte_parse_umask()` in [`../../util/prte_cmd_line.c`](../../util/prte_cmd_line.c),
  and it is unit tested.

---

## The check-in sequence

A launched daemon must tell the HNP: its name, its URI, its node name and
aliases, and (unless `--uniform-nodes`) its topology. That message goes
to `PRTE_RML_TAG_PRTED_CALLBACK`.

**Direct launch** (`ssh` without tree spawn, `slurm`, `pals`, …): the
daemon packs the message and sends it straight to the HNP with
`PRTE_RML_RELIABLE_SEND`.

**Tree spawn:** the daemon sends the same message *to itself*, which
lands in `rollup()`. `rollup()` accumulates one buffer per direct child
plus its own, and `report_prted()` forwards the whole bundle to the
parent once `prte_rml_base.n_children + 1` have arrived **and** the node
regex has been received. That last condition is what
`node_regex_waiting` is for: a daemon cannot know how many children to
expect until the nidmap tells it.

Rules for this code:

- **A failed `PRTE_RML_SEND` has not consumed the buffer** —
  `prte_rml_send_buffer_nb()` only takes ownership when it returns
  success. Release it on the error path, and set `ret` before
  `goto DONE`: six paths here used to jump to `DONE` with `ret` still
  `PRTE_SUCCESS`, so a daemon that could not pack its own check-in exited
  reporting success.
- **`DONE` runs `prte_finalize()`.** The one branch that must *not*
  finalize — the `prted_debug_failure` simulated crash, which exists
  precisely to look like an abnormal death to the HNP — exits directly
  instead. Do not "simplify" it into the `DONE` path.
- **`prte_odls.kill_local_procs(NULL)` after the event loop** is the
  daemon's promise that no application process outlives it.

---

## Debug hooks that live here

| Knob | Effect |
|------|--------|
| `prted_debug_failure` (rank or wildcard) | this daemon kills itself at startup — used to test the errmgr's daemon-loss path |
| `prted_debug_failure_delay` | …after N seconds, via `shutdown_callback` |
| `prte_daemon_cores` | bind the daemon itself to a core list |
| `--debug-daemons`, `--debug-daemons-file` | stay attached / log per daemon |
| `PRTE_DAEMON_UMASK_VALUE` | umask for everything the daemon creates |

`--leave-session-attached` and `--debug-daemons` both suppress
daemonizing, which is what makes the daemon's output reachable at all.

---

## Testing

There is no unit test for `main()` itself; what was testable in it — the
umask parser — is in [`test/unit/tools/`](../../../test/unit/tools/).
Everything else about this file is inherently multi-process:

- **Live:** `prte --daemonize` starts one of these on the local node.
- **Multi-node:** [`contrib/dockerswarm/`](../../../contrib/dockerswarm/)
  is the only place the tree-spawn rollup, the bootstrap promotion, and
  the daemon-loss paths are actually exercised.
