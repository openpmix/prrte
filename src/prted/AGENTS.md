# AGENTS.md — `src/prted` (the daemon body and the tool front ends)

Orientation for AI agents and human contributors working in
`src/prted/`. This is a map, not the rulebook: the authoritative project
guidance lives in the top-level [`AGENTS.md`](../../AGENTS.md) and under
[`docs/`](../../docs/). When this file and those disagree, **the docs
win** — and please fix this file.

---

## What lives here, and what does not

`src/prted/` is not an MCA framework. It is the **body of the running
system**: the code that every PRRTE process actually executes, as opposed
to the pluggable policy that the frameworks under `src/mca/` supply.

The name is historical and misleading in one important way: this
directory holds far more than the `prted` daemon. Everything here is
compiled into `libprrte`, and the thin `main()` wrappers under
`src/tools/` call into it.

| File | Runs in | Purpose |
|------|---------|---------|
| `prte.c` | `prte`, `prterun` | The HNP/DVM-master body. Command line → PRRTE init → DVM startup → (optionally) spawn one job → event loop. ~1700 lines, and the single longest function in the tree. |
| `prted_comm.c` | every daemon incl. the HNP | The `PRTE_RML_TAG_DAEMON` command dispatcher: one `switch` over `prte_daemon_cmd_flag_t`. This is how the HNP tells daemons to launch, signal, kill, shrink, halt. |
| `prte_app_parse.c` | `prte`, `prterun`, `prun` | Turns an application command line (with `:`-separated app contexts) into a list of `prte_pmix_app_t`. |
| `prun_common.c` | `prun`, and `prterun --dvm` | The tool-side body: attach to a DVM as a PMIx tool, spawn, forward I/O and signals, wait for termination. |
| `prted.h` | — | The handful of symbols the tools need from here. |
| `pmix/` | every daemon | The PMIx **server host module** — the ~10k lines that answer PMIx's upcalls. It has its own [`AGENTS.md`](pmix/AGENTS.md); read it before touching anything in there. |

Not here: `src/tools/prted/prted.c` is the daemon's `main()` and its
startup/rollup logic. `src/runtime/` holds the globals
(`prte_job_t`, `prte_node_t`, `prte_proc_t`) that all of this code
manipulates.

**The rollup message is a wire format with two ends.** A daemon builds it in
`src/tools/prted/prted.c` and the master unpacks it in
`prte_plm_base_daemon_callback()`
([`plm_base_launch_support.c`](../mca/plm/base/plm_base_launch_support.c)) —
change one and you must change the other, in field order. Today it carries:
the daemon's name, its `PMIX_PROC_URI` (RML contact info), its
`PMIX_SERVER_URI` (the PMIx server rendezvous, for tools — see
[`../pmix/AGENTS.md`](../pmix/AGENTS.md)), the node name, node aliases, and
optionally the topology. Note that a tree-spawning parent relays a child's
buffer by copying the payload wholesale and only *peeking* at the first two
fields for its own use, so appending a field is safe there — but inserting
one before `PMIX_PROC_URI` is not.

---

## Who executes what

This is the first thing to get straight, because the same file often runs
in three different roles with different rules:

```
        prte / prterun                prted (per node)          prun (tool)
        ────────────────              ────────────────          ───────────
 body:  prte.c                        src/tools/prted/prted.c   prun_common.c
        prted_comm.c (as a daemon)    prted_comm.c              (no daemon cmds)
        prted/pmix/*   (as HNP)       prted/pmix/*  (as prted)  —
```

- **The HNP is also a daemon.** `prte.c` registers
  `prte_daemon_recv` for `PRTE_RML_TAG_DAEMON` just as `prted` does,
  because "send this to all daemons" includes the HNP. Any new daemon
  command must therefore behave sensibly at the HNP too — several
  existing cases branch on `PRTE_PROC_IS_MASTER` for exactly this reason.
- **`prterun` is `prte` wearing a different hat.** `prte()` detects that
  it is being run as a proxy (`prte_schizo_base_detect_proxy`), and, if
  `--dvm` was given, hands straight off to `prun_common()` and never
  starts a DVM at all. Otherwise it starts a one-shot DVM, spawns the
  job, and exits when it completes (`prte_persistent == false`).
- **`prun` never runs any of this except `prun_common.c` and
  `prte_app_parse.c`.** It is a PMIx tool; it has no PRRTE runtime.

---

## The thread rule — this is where it bites hardest

`src/prted` sits directly on the boundary between the PMIx progress
thread and the PRRTE progress thread, so the top-level golden rule
("thread-shift every PMIx callback and server-module upcall") is not
abstract advice here: it is the single most common source of bugs in this
directory. Read that section of the top-level
[`AGENTS.md`](../../AGENTS.md) before writing anything that takes a
callback.

Two concrete consequences specific to this directory:

**The daemon's event base is driven by its main thread.** Both
`src/tools/prted/prted.c` and `prte.c` run

```c
while (prte_event_base_active) {
    prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
}
```

so every RML receive callback — including `prte_daemon_recv` — executes
*on the thread that drives the event base*. That means:

- A `prte_pmix_shifted_wakeup()` posted from the PMIx thread can only be
  processed by that same thread. Code here that blocks waiting for such a
  wakeup must use the loop-driving idiom, never `PRTE_PMIX_WAIT_THREAD`:

  ```c
  while (prte_event_base_active && lock.active) {
      prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
  }
  ```

  `prte.c` does this in five places and says so in comments at each; copy
  that pattern rather than inventing a new one.

- **No daemon command blocks the loop.** `prted_comm.c` contains no
  `PRTE_PMIX_WAIT_THREAD` at all, and should not acquire one. The three
  commands that have to wait on PMIx — `HALT_VM`, `SHRINK` and
  `DVM_CLEANUP_JOB` — are written as continuations instead: the command
  issues the PMIx call and returns, PMIx's completion callback captures
  and thread-shifts (`_daemon_cont_cbfunc`), and the work that would have
  followed a wait runs in `_daemon_continue()` on the progress thread.
  A `prte_daemon_caddy_t` carries the state across.

  This is not stylistic. Blocking there froze the daemon's entire event
  loop — no RML traffic, no timers, no other command — and, worse, meant
  that anything PMIx needed *from us* to finish the call could never run.
  The `prte.notify.donotloop` marker on those notifications exists
  precisely because one such deadlock was found the hard way.

  Two things to keep right when adding to this pattern: the info array
  must be heap-allocated and owned by the caddy (PMIx needs it valid
  after `prte_daemon_recv` has returned, so a stack array is a
  use-after-free), and a PMIx call that returns anything other than
  `PMIX_SUCCESS` will *not* invoke the callback — run the continuation
  directly in that case, since you are already on the right thread.

**`prun_common.c` is the exception.** `prun` is a tool: its main thread
does not drive a PRRTE event base, so the classic
`PRTE_PMIX_WAIT_THREAD` / `PRTE_PMIX_WAKEUP_THREAD` pair is correct there
and is used throughout. Do not "fix" it to match `prte.c`.

---

## `prte.c` — the HNP body

One enormous function, `prte()`, in roughly this order. Knowing the order
matters because a surprising amount of the code depends on *when* it runs
relative to `prte_init()`.

1. **Pre-init.** Save a pristine environment for launched procs (all
   `PMIX_`/`PRTE_` vars stripped), pre-scan argv for MCA params, open the
   event base, install signal handlers, open and select `schizo`.
2. **Personality & proxy detection.** `detect_proxy` picks the schizo
   module. If it returns NULL we cannot continue — every later use of
   `jdata->schizo` is an unchecked dereference.
3. **Command-line parse** through the selected schizo, then the standalone
   options (`--daemonize`, `--report-uri`, `--singleton`, `--prefix`,
   `--report-pid`, `--keepalive`, …).
4. **Proxy hand-off.** `--dvm` + proxy ⇒ `prun_common()` and `exit()`.
5. **`prte_init(PRTE_PROC_MASTER)`.** After this the daemon job object,
   the node pool, and the PMIx server all exist. **Anything that must
   reject bad user input before PMIx sees it has to happen above this
   line** — `--singleton` is the cautionary tale: the value is handed
   straight to `PMIx_server_init` as `PMIX_SINGLETON`, so a malformed one
   faults inside the PMIx library long before PRRTE's own
   `prep_singleton()` would look at it.
6. **DVM startup.** `PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATE)`,
   then loop the event base until `prte_dvm_ready`.
7. **Spawn (non-persistent only).** Build `pmix_app_t`s from the parsed
   apps, `PMIx_Spawn_nb`, push stdin.
8. **`proceed:`** — loop the event base until something clears
   `prte_event_base_active`, then `prte_finalize()` and `exit()`.

### Things in `prte.c` that are easy to get wrong

- **`goto DONE` is the only exit.** It runs `prte_finalize()`. An early
  `return` after `prte_init()` leaks the session directory.
- **The `--prefix` normalizers.** There are four textually identical
  copies (PRRTE prefix from the command line, from `PRTE_PREFIX`, PMIx
  prefix from the command line, from `PMIX_PREFIX`). They now all call
  `prte_strip_trailing_pathsep()`. Do not re-inline the loop: the version
  that was inlined four times used `sizeof(param)` on a `char *` and
  overran the heap for any prefix shorter than a pointer.
- **The signal handlers are not async-signal-safe.**
  `abort_signal_callback()` is installed with `signal()` and, on the third
  ctrl-c, calls `prte_job_session_dir_finalize()` and
  `PMIx_server_finalize()`. That is a known wart on a
  we-are-dying-anyway path; do not add to it. The first ctrl-c correctly
  does nothing but `write()` to `term_pipe`, which wakes `clean_abort`
  inside the event loop.
- **`prte_event_reinit()` after `--daemonize`.** The event base is opened
  before the fork and some backends (kqueue on macOS) do not survive it.
- **`prep_singleton()` builds a job by hand.** It fabricates a
  `prte_job_t`/`prte_app_context_t`/`prte_proc_t` and registers the
  nspace, so a singleton can `PMIx_Init` against this DVM. It is the only
  place outside the mappers that constructs a job map.

---

## `prted_comm.c` — the daemon command dispatcher

`prte_daemon_recv()` is a single `switch` over the command byte. Adding a
command means: a new `PRTE_DAEMON_*` value in
[`src/mca/plm/plm_types.h`](../mca/plm/plm_types.h) (unique offset — see
the top-level AGENTS.md rule on hand-assigned codes), a `case` here, and
a string in `get_prted_comm_cmd_str()`.

| Command | Effect |
|---------|--------|
| `KILL_LOCAL_PROCS` | Kill the named procs, or all of them if the list is empty. |
| `SIGNAL_LOCAL_PROCS` | Deliver a signal to the named **job**'s local procs. |
| `ADD_LOCAL_PROCS` / `DVM_ADD_PROCS` | Hand the launch message to `odls.launch_local_procs`. |
| `ABORT_PROCS_CALLED` | An app called `PMIx_Abort`; terminate the listed procs (deduplicated against everything already ordered to die). |
| `DEFINE_PSET` | Register a process set with the local PMIx server. |
| `EXIT_CMD` | Orderly shutdown once our children and routing children are gone. |
| `HALT_VM_CMD` | Abnormal shutdown; notify attached tools, then terminate. |
| `SHRINK_CMD` | We are being removed from the DVM (see below). |
| `DVM_CLEANUP_JOB_CMD` | A job finished; deregister its nspace and clear pending server ops. |
| `GET_STACK_TRACES` | Run `gstack` on each local proc of a job and ship the output to the HNP. |

Rules that this switch has broken before and will break again:

- **Every `case` owns its allocations.** The `switch` has one shared
  `CLEANUP:` label that does nothing but `return`. There is no common
  teardown, so a `goto CLEANUP` from the middle of a case leaks whatever
  that case had allocated — buffers, `PMIx_Proc_create` arrays, unpacked
  strings. Free explicitly before jumping.
- **`PMIX_NEW` objects need `PMIX_RELEASE`, not `free()`.** A
  `prte_proc_t` built to carry a name still has a destructor.
- **`break` inside a `for` inside a `case` leaves the loop, not the
  switch.** The `GET_STACK_TRACES` case relies on this deliberately;
  read carefully before adding one.
- **Do not block.** See the thread rule above — a command that must wait
  on PMIx is written as a continuation, not as a wait on a lock.
- **Do not assume the daemon job object exists.** `prte_get_job_data_object`
  can return NULL during teardown.

### The shrink path is subtler than it looks

A daemon told to leave the DVM must **not** exit immediately in elastic
mode: the master completes the shrink campaign from the broadcast's
collective ACK, so leaving before our subtree's ACK propagates means the
completion handler never fires. `PRTE_DAEMON_SHRINK_CMD` therefore sets
`prte_dvm_leaving` and arms a bounded (2 s) departure timer, with
`prte_rml_route_lost()` providing an earlier exit if the lifeline
actually drops. The legacy (non-elastic) path exits at once because
nothing is tracking completion. The long comment above
`prte_shrink_depart_ev` is the authoritative explanation.

---

## `prte_app_parse.c` and `prun_common.c`

`prte_parse_locals()` splits the command line on `:` into app contexts
and produces `prte_pmix_app_t` list entries plus two out-params
(`hostfiles`, `hosts`) and a `jobdata` list of directives that belong to
the **job** rather than to any one app. That last list is why
`--map-by` written on the *first* app segment and nowhere else applies to
the whole job however many apps there are — `prte.c` and `prun_common.c`
both walk `jobdata` looking for `PMIX_MAPBY`/`RANKBY`/`BINDTO` and the two
prefix keys. Written on any later app it stays with that app, and the apps
that gave none fall back to the defaults: saying nothing is not the same as
agreeing.

`prun_common()` is the tool body: `PMIx_tool_init` (with whatever DVM
search directive the user gave), register event handlers for job
termination and debugger events, `PMIx_Spawn_nb`, push stdin, wait, then
report the job's exit status. Signal forwarding is done with
`PMIx_Job_control(PMIX_JOB_CTRL_SIGNAL)` against the spawned nspace,
which is what eventually arrives at `prted_comm.c`'s
`SIGNAL_LOCAL_PROCS`.

---

## Testing

**Unit — `test/unit/prted/` (`make check`).** Everything in here that can
be exercised without a DVM: the `--prefix` normalizer, the `--singleton`
identifier parser, `prte_pmix_xfer_job_info()`'s directive handling
(including its conflict rejection and its cache-the-unknown default),
`prte_pmix_xfer_app()`'s translation and — importantly — its ownership
contract with the caller's job object, and the job-info cache. Extend it
whenever you add a decision that does not need a live runtime.

**Offline mapper harness.** Not relevant here unless you touch how job
directives reach the mapper — but if you change `prte_pmix_xfer_job_info`,
run `make -C test/offline check-offline`, since that is the function that
turns a spawn request into a mapping policy.

**Live smoke test.** For anything touching the command dispatcher, DVM
startup, or teardown:

```sh
prte --daemonize && prun -n 4 hostname && pterm
```

**Multi-node — `contrib/dockerswarm/`.** Most of what is interesting in
this directory only exists across nodes: a daemon that hosts none of a
job's procs, a tool connecting through a non-master daemon, a signal that
must reach one job and not another, a shrink that has to ACK before it
departs. `run-tests.sh`'s `test_prted()` covers these; see
[`contrib/dockerswarm/AGENTS.md`](../../contrib/dockerswarm/AGENTS.md).

---

## Debugging

```sh
prte --prtemca state_base_verbose 5 ...        # job/DVM state transitions
prte --prtemca plm_base_verbose 5 ...          # daemon launch
prte --prtemca pmix_server_verbose 5 ...       # every PMIx upcall and reply
prte --debug-daemons ...                       # the pmix_output() traces in prted_comm.c
prte --debug-daemons-file ...                  # ...to a file, per daemon
```

`prte_debug_daemons_flag` guards the plain-language traces in
`prted_comm.c`; `prte_pmix_server_globals.output` (verbosity ≥2) guards
the ones under `pmix/`.

---

## Where to go next

- [`pmix/AGENTS.md`](pmix/AGENTS.md) — the PMIx server host module. Two
  thirds of the code in this directory lives there.
- [`../mca/state/AGENTS.md`](../mca/state/AGENTS.md) — what
  `PRTE_ACTIVATE_JOB_STATE` actually does.
- [`../mca/plm/AGENTS.md`](../mca/plm/AGENTS.md) — the other end of the
  daemon command channel.
- [`../mca/odls/AGENTS.md`](../mca/odls/AGENTS.md) — what
  `ADD_LOCAL_PROCS` and `KILL_LOCAL_PROCS` hand off to.
