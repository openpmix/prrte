# AGENTS.md — `src/tools` (the executables)

Orientation for AI agents and human contributors working in
`src/tools/`. This is a map, not the rulebook: the authoritative project
guidance lives in the top-level [`AGENTS.md`](../../AGENTS.md) and under
[`docs/`](../../docs/). When this file and those disagree, **the docs
win** — and please fix this file.

---

## What lives here

PRRTE ships no linkable library, so this directory is the entire public
face of the project. Every `bin_PROGRAMS` target PRRTE installs is built
here, and nothing else is.

| Directory | Installs | `prte_tool_actual` | Body |
|-----------|----------|--------------------|------|
| [`prte/`](prte/) | `prte`, symlink `prterun` | `"prte"` / `"prterun"` | `main()` → `prte()` in [`src/prted/prte.c`](../prted/prte.c) |
| [`prted/`](prted/) | `prted` | `"prted"` | **all of it is here** — [`prted.c`](prted/prted.c) is a real 900-line `main()` |
| [`prun/`](prun/) | `prte-submit`, symlink `prun` | `"prun"` | [`prun.c`](prun/prun.c) does setup, then `prun_common()` in [`src/prted/prun_common.c`](../prted/prun_common.c) |
| [`pterm/`](pterm/) | `prte-term`, symlink `pterm` | `"pterm"` | **all of it is here** — [`pterm.c`](pterm/pterm.c) |
| [`prte_info/`](prte_info/) | `prte-info`, symlink `prte_info` | `"prte_info"` | **all of it is here** — [`prte_info.c`](prte_info/prte_info.c) |
| [`pcc/`](pcc/) | symlink `pcc` → PMIx's `pmixcc` | — | no source at all |

So three of the six tools are implemented in this directory, and two are
thin wrappers over `src/prted`. Read
[`src/prted/AGENTS.md`](../prted/AGENTS.md) before changing `prte` or
`prun` — that is where their behavior actually is.

### Installed names, and why they are not the source names

The revised names (`prte-submit`, `prte-term`, `prte-info`) are what the
build produces. The familiar names (`prun`, `pterm`, `prte_info`) are
**symlinks** created by each directory's `install-exec-hook`, guarded by
`if PRTE_WANT_LEGACY_TOOLS` — a site can build with
`--with-legacy-tools=no` and get only the revised names. `prterun` is a
symlink to `prte` and is **not** guarded, because it is not a legacy
name: it is a different personality of the same binary.

Consequences you have to keep in mind:

- **`argv[0]` is load-bearing.** `prte_tool_basename` is
  `pmix_basename(argv[0])` and appears in every error message; more
  importantly `prte()` decides between "start a DVM" and "run a job now"
  partly from how it was invoked. Never assume the basename equals the
  target name.
- **`prte_tool_actual` is the tool's *identity*,** set as a literal
  string in each `main()` before anything else. `schizo`'s `parse_cli`
  switches on it to pick the option table and the help file
  ([`src/mca/schizo/prte/schizo_prte.c`](../mca/schizo/prte/schizo_prte.c)).
  A new tool that forgets to set it gets the "no-proxy" error, not a
  crash — that was made deliberate; keep it that way.

---

## The startup sequence every tool follows

The order below is not stylistic. Each step depends on the one before,
and the tools that got it wrong are the reason several of the rules in
this file exist.

```
prte_tool_basename / prte_tool_actual     <- identity, before anything can report an error
prte_init_minimum()                       <- installdirs; NEEDED to find help files
prte_schizo_base_parse_prte/_pmix()       <- pre-scan argv for --prtemca/--pmixmca
prte_init_util(<proc type>)               <- output system, hostname, backtrace
schizo framework open + select
prte_schizo_base_detect_proxy(personality)<- which personality parses our CLI
schizo->parse_cli(argv, &results, ...)    <- everything else reads `results`
... tool-specific work ...
```

- **MCA parameters must be pre-scanned before `prte_init_util`.** An MCA
  variable reads its environment exactly once, at first registration, and
  that happens inside `prte_init_util`. This is why every tool calls
  `prte_schizo_base_parse_prte()`/`_pmix()` on the raw argv *first* —
  and why `prted` additionally hunts for `--bootstrap` by hand before the
  CLI is parsed at all (see [`prted/AGENTS.md`](prted/AGENTS.md)).
- **`detect_proxy` can return NULL** and every tool checks it. It also
  must fall back to the default personality — see
  [`../mca/schizo/AGENTS.md`](../mca/schizo/AGENTS.md).
- **`parse_cli` returning `PRTE_OPERATION_SUCCEEDED` is success, not
  failure.** It means PMIx already printed `--help` or `--version` output
  and the tool must exit 0 without doing anything else.
- **`pmix_argv_copy_strip()`** is what makes `prun -n 2 "a b"` work;
  tools that parse their own argv (`prted`, `prun`) copy-and-strip first
  and hand the *copy* to schizo.

### Rules for `main()` in this directory

- **Return 1, never a PRRTE error code.** `main()` hands the shell the
  low eight bits of whatever it returns, so `return PRTE_ERR_BAD_PARAM`
  becomes exit status 251 and `return -1` becomes 255. Report the error,
  then `return 1`. `prte_exit_status`/`PRTE_UPDATE_EXIT_STATUS` are the
  exception: they carry a *job's* exit status, which is meaningful.
- **Check `prte_init_util()`.** Everything after it — the output system,
  show_help lookups, the install dirs — assumes it succeeded.
- **Do not put logic in `main()` that you would want to test.** There is
  no way to call `main()` from a unit test, which is why the `--pid`
  parser, the appfile reader, and the umask reader now live in
  [`../util/prte_cmd_line.c`](../util/prte_cmd_line.c) with tests in
  [`test/unit/tools`](../../test/unit/tools/). Put new option-value
  interpretation there, not here.
- **Use the `PRTE_CLI_*` macros, not string literals.** That is the
  entire reason [`../util/prte_cmd_line.h`](../util/prte_cmd_line.h)
  exists: a typo in a literal is an option that is accepted by the
  parser and then silently never acted on. `pterm` had three such
  options (`--namespace` was one) before this was cleaned up.

---

## Signals

`prte`, `prun` and `pterm` all install POSIX handlers (`signal()`, not
libevent) for SIGINT/SIGTERM/SIGHUP, because a libevent signal event
cannot fire while the process is stuck inside a PMIx call. The handler
does one thing — `write()` a byte to `term_pipe[1]` — and a libevent
read event on `term_pipe[0]` runs `clean_abort()` in a safe context.

Two invariants, both of which have been broken here:

1. **The real teardown belongs in `clean_abort()`'s *first-time* path**,
   not inside its "an abort is already in progress" branch. `pterm`'s
   version had nothing outside that branch, so the first ctrl-c was
   silently swallowed and a pterm blocked on an unresponsive DVM could
   not be interrupted at all.
2. **A signal handler may not call `exit()`**, `fprintf`, or anything
   else that is not async-signal-safe. Use `write()` and `_exit()`. The
   handlers in `src/prted/prte.c` predate this rule on their
   third-ctrl-c path; do not copy that part.

The escalation the user expects is: first signal → orderly termination;
second → a warning; third → gone.

---

## Testing

**Unit — [`test/unit/tools/`](../../test/unit/tools/) (`make check`).**
Covers what a tool decides before it touches a runtime: the `--pid`
value (integer and `file:` forms, and their distinct failure modes), the
appfile expander, and the daemon umask reader. Anything you factor out
of a `main()` belongs here.

**Live smoke test.** Every change in this directory needs at least:

```sh
prte --daemonize && prun -n 4 hostname && pterm
```

and, for anything touching option handling, the failure paths too —
a rejected command line must exit non-zero *and* leave a running DVM
alone.

**Multi-node — [`contrib/dockerswarm/`](../../contrib/dockerswarm/).**
`run-tests.sh`'s `test_tools()` covers what a single host cannot: that
`prte-info` reports the same build on every node, that `--pid file:`
selects among several DVMs, that an appfile spreads across nodes, that a
job's exit status reaches `prun`, and that a bad command line to one
tool does not disturb the DVM.

---

## Where to go next

- [`../prted/AGENTS.md`](../prted/AGENTS.md) — the bodies of `prte`,
  `prterun` and `prun`, and the thread rules that govern them.
- [`../mca/schizo/AGENTS.md`](../mca/schizo/AGENTS.md) — the option
  tables, the personalities, and the help files these tools print.
- [`../util/prte_cmd_line.h`](../util/prte_cmd_line.h) — the `PRTE_CLI_*`
  names and the shared value parsers.
