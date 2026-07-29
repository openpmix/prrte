# AGENTS.md — `src/tools/pterm` (terminate a DVM)

Orientation for `pterm`, installed as `prte-term` with a legacy symlink
`pterm`. Start with [`src/tools/AGENTS.md`](../AGENTS.md) for what all
the tools share.

---

## What it does

`pterm` is the smallest complete PRRTE tool, and the whole of it is in
this directory. It connects to a DVM as a PMIx tool, issues one
`PMIx_Job_control_nb(PMIX_JOB_CTRL_TERMINATE)` against the whole DVM,
waits for the acknowledgement, then waits for the connection to drop —
which is how it knows the DVM actually went away rather than merely
heard the order.

It launches nothing. `--allow-run-as-root` is accepted only for symmetry
with the other tools.

---

## Finding the right DVM

This is the only interesting decision `pterm` makes, and every one of
these options is simply translated into an info key handed to
`PMIx_tool_init`:

| Option | Key |
|--------|-----|
| `--pid <n>` / `--pid file:<path>` | `PMIX_SERVER_PIDINFO` |
| `--namespace <name>` | `PMIX_SERVER_NSPACE` |
| `--dvm-uri <uri>` / `file:<path>` | `PMIX_SERVER_URI` |
| `--system-server-first` | `PMIX_CONNECT_SYSTEM_FIRST` |
| `--system-server-only` | `PMIX_CONNECT_TO_SYSTEM` |
| `--wait-to-connect`, `--num-connect-retries` | retry delay / count |

Two things this got wrong, both of which are the same mistake:

- **An option that is accepted but never read is worse than one that is
  rejected.** `--namespace` was documented in `help-pterm.txt` and
  present in the option table, but `pterm` never looked at it, so a user
  disambiguating between DVMs by name got a silent connection to
  whichever DVM PMIx happened to pick. Same for `-v`. If you add an
  option to `ptermoptions` in
  [`schizo_prte.c`](../../mca/schizo/prte/schizo_prte.c), read it here in
  the same commit.
- **A malformed `--pid` must be an error.** It used to fall through
  silently and `pterm` then terminated whatever DVM it found — the
  opposite of what a user who bothered to name a PID wants. The value is
  now interpreted by `prte_parse_pid_option()`
  ([`../../util/prte_cmd_line.c`](../../util/prte_cmd_line.c)), which
  distinguishes "not a PID at all" from "could not open that file" from
  "that file holds no PID", because the tool prints a different message
  for each.

`pterm` calls `prte_init_util(PRTE_PROC_MASTER)` — deliberately. It has
to resolve `prte_local_tmpdir_base` exactly the way the HNP did, or it
looks for the rendezvous file in the wrong place.

---

## Threads, locks and signals

`pterm` has no PRRTE event base of its own driving the runtime: it runs a
PMIx progress thread (`prte_progress_thread_init`) and its main thread
blocks in `PRTE_PMIX_WAIT_THREAD`, i.e. in a condition variable. That is
why `evhandler()` and `infocb()` may call `PRTE_PMIX_WAKEUP_THREAD`
**directly** from the PMIx thread, which anywhere in `src/prted` (except
`prun_common.c`) would be a golden-rule violation. Read the thread rule
in the top-level [`AGENTS.md`](../../../AGENTS.md) before copying this
pattern outward.

Two traps in the wait:

- **A PMIx call that does not return `PMIX_SUCCESS` will never invoke its
  callback.** Waiting on the lock in that case hangs forever — which is
  what `pterm` did when `PMIx_Job_control_nb` failed outright. Destruct
  the lock and report the error instead.
- **The first ctrl-c has to do the work.** `clean_abort()` runs on the
  progress thread when the POSIX handler pokes `term_pipe`; the
  termination belongs in its first-time path. It used to be entirely
  inside the "already aborting" branch, so a `pterm` waiting on a wedged
  DVM ignored the first two signals.

---

## Testing

**Unit — [`test/unit/tools/`](../../../test/unit/tools/):** the `--pid`
value parser, in both forms and all three failure modes.

**Live:** the useful cases are the ones that must *not* kill anything:

```sh
prte --daemonize
pterm foo              # rejected: exit 1, DVM survives
pterm --pid junk       # rejected: exit 1, DVM survives
pterm --pid file:/no   # rejected: exit 1, DVM survives
pterm                  # "TERMINATING DVM...DONE", exit 0
```

To exercise the signal path, `kill -STOP` the DVM first so `pterm` blocks
on the acknowledgement, then send it one SIGINT: it must exit.

**Multi-node — [`contrib/dockerswarm/`](../../../contrib/dockerswarm/):**
`test_tools()` runs two DVMs at once and checks that `--pid file:` takes
down the one that was named and leaves the other running.
