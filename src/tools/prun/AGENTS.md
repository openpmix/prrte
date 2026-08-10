# AGENTS.md — `src/tools/prun` (submit a job to a DVM)

Orientation for `prun`, installed as `prte-submit` with a legacy symlink
`prun`. Start with [`src/tools/AGENTS.md`](../AGENTS.md) for what all the
tools share.

---

## What is here, and what is not

`prun.c` is the front half only. It ends with

```c
rc = prun_common(&results, schizo, pargc, pargv);
```

and everything a user thinks of as "prun" — attaching to the DVM as a
PMIx tool, translating the parsed command line into `pmix_app_t`s,
`PMIx_Spawn_nb`, I/O forwarding, signal forwarding, waiting for the job
— is [`src/prted/prun_common.c`](../../prted/prun_common.c), shared with
`prterun --dvm`. **Read [`src/prted/AGENTS.md`](../../prted/AGENTS.md)
before changing prun's behavior.** This file only does what has to happen
before that call.

What has to happen here:

| Step | Why here |
|------|----------|
| identity, MCA pre-scan, `prte_init_minimum`, `prte_init_util(PRTE_PROC_TYPE_NONE)` | the standard tool startup; see [`../AGENTS.md`](../AGENTS.md). The pre-scan comes **first**: `prte_init_minimum()` is where `prte_register_params()` runs, and a `--prtemca` value pushed into the environment after that is never seen. |
| open an event base | `prun_common` needs one to exist |
| schizo open/select, `normalize_argv`, `detect_proxy`, `parse_cli` | picks the personality and produces `results` |
| the no-arguments case | `prun` alone prints usage and exits 1 |
| `--report-pid` | must be written before we could possibly block |
| `--report-uri` | stashed in `prte_pmix_server_globals` for `prun_common` |
| `--app <file>` | expands the appfile into `pargv` |
| open the `ess` framework | only for its signal-name list — no component is selected |

---

## Two special cases in the argv handling

**The executable may come first.** `prun ./myapp arg` — no leading `-`
on `argv[1]` — skips `parse_cli` entirely and copies the tail verbatim.
That is deliberate (there is nothing to parse), but it means every
side effect of `parse_cli` is skipped too, including the schizo
component's session-directory prefix. Anything you add to the parse path
must also be safe by its absence.

**`--app <file>` appends to `pargv`, after `parse_cli` has already
run.** Each line of the appfile becomes one app context, `:`-delimited,
and `prte_parse_locals()` (in
[`src/prted/prte_app_parse.c`](../../prted/prte_app_parse.c)) splits them
apart later. The reader is `prte_load_appfile()` in
[`../../util/prte_cmd_line.c`](../../util/prte_cmd_line.c) — shared with
`prte.c`, which has the same option, and unit tested. Note that
`pargc` is recomputed but never used: `prun_common()` hides it with
`PRTE_HIDE_UNUSED_PARAMS` and works from the NULL-terminated `pargv`.

---

## Exit status

`prun`'s exit status is the *job's* outcome, so this is the one tool
where returning a computed value is the point:
`PRTE_UPDATE_EXIT_STATUS(rc)` then `exit(prte_exit_status)`. Setup
failures before that — a rejected command line, an unreadable appfile —
`return 1`, because a raw PRRTE error code reaches the shell as a
meaningless 251 or 255.

The `DONE:` label unlinks the `--report-pid` file. Any new failure path
that can be reached after the pid file was written must go through it.

---

## Testing

**Unit — [`test/unit/tools/`](../../../test/unit/tools/):** the appfile
expander.

**Live:**

```sh
prte --daemonize
prun -n 2 hostname          # basic
prun hostname               # executable-first path
prun --app appfile          # appfile path
prun -n 1 false; echo $?    # the job's status is prun's status
pterm
```

**Multi-node — [`contrib/dockerswarm/`](../../../contrib/dockerswarm/):**
an appfile whose app contexts land on different nodes, and exit-status
propagation from a non-head node — neither of which a single host can
tell apart from the trivial case.
