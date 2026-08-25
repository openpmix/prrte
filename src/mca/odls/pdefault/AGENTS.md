# AGENTS.md — `odls/pdefault` (the fork/exec local launcher)

Component guide for `src/mca/odls/pdefault/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the launch
state machine, and the base helpers (`construct_child_list`,
`launch_local`, `spawn_proc`, `wait_local_proc`, `kill/signal/restart`,
`prte_odls_base_set`) that this component leans on for nearly everything.

---

## Role and priority

`pdefault` is the **default — and currently only — odls component**. It is
the code that performs the actual **`fork()` + `execve()`** of application
processes on a node using POSIX process primitives. Priority **10**,
deliberately low ("let others override us — we are the default", per
`component_query`), so a site could drop in a specialized launcher without
touching the base.

It is only built where `fork` exists: `configure.m4` does
`AC_CHECK_FUNC([fork])` and skips the component otherwise. Because the base
select logic only engages odls inside a daemon, `pdefault` is what every
`prted` (and the HNP for its own local procs) uses to launch apps.

Files:

| File | Contents |
|------|----------|
| `odls_pdefault_component.c` | Component struct + `component_query` (returns priority 10 and the module). |
| `odls_pdefault_module.c` | The module: the five vtable fns, the `fork`/`execve` primitive (`fork_local_proc`), and the parent/child pipe protocol (`do_parent`/`do_child`). |
| `odls_pdefault.h` | Extern declarations for the component and module structs. |
| `configure.m4` | Gates the build on `fork` support. |
| `help-prte-odls-default.txt` | Rendered error text for binding/exec/iof failures. |

---

## The module: mostly delegation

`prte_odls_pdefault_module` wires four of its five entry points straight
to base helpers, passing in the component's own syscall primitives:

```c
prte_odls_base_module_t prte_odls_pdefault_module = {
    .get_add_procs_data = prte_odls_base_default_get_add_procs_data,  // base, verbatim
    .launch_local_procs = launch_local_procs,                        // → base + fork_local_proc
    .kill_local_procs   = kill_local_procs,                          // → base + odls_default_kill_local
    .signal_local_procs = signal_local_procs,                        // → base + send_signal
    .restart_proc       = restart_proc,                              // → base + fork_local_proc
};
```

- **`get_add_procs_data`** is the base function itself — the HNP-side
  message builder has nothing OS-specific, so the component doesn't wrap it.
- **`launch_local_procs(data)`** calls
  `prte_odls_base_default_construct_child_list()` to decode the message and
  build the local child list, then fires
  `PRTE_ACTIVATE_LOCAL_LAUNCH(job, fork_local_proc)` — handing the base's
  per-node launch driver this component's `fork_local_proc` as the fork
  primitive.
- **`kill_local_procs(procs)`** → `prte_odls_base_default_kill_local_procs(procs,
  odls_default_kill_local)`. The base does the SIGCONT→SIGTERM→SIGKILL
  escalation; the component supplies only the raw delivery.
- **`signal_local_procs(proc, signal)`** →
  `prte_odls_base_default_signal_local_procs(proc, signal, send_signal)`.
- **`restart_proc(child)`** →
  `prte_odls_base_default_restart_proc(child, fork_local_proc)`.

So the component's *real* content is three primitives —
`fork_local_proc`, `odls_default_kill_local`, `send_signal` — plus the
child-side pre-exec sequence.

---

## The fork/exec primitive — `fork_local_proc()`

This is the function the base calls (as `cd->fork_local(cd)`) from inside
`prte_odls_base_spawn_proc`, once per child, on whichever event base the
base picked. The design, spelled out in the long header comment, is a
**pipe-synchronized fork**, with a pipe running each way:

1. Open a pipe `p[2]` (child→parent errors) and a pipe `gate[2]`
   (parent→child release).
2. `fork()`. Record `child->pid` (in *both* parent and child copies).
3. **Child** (`pid == 0`): close `p[0]` and `gate[1]`, call
   `do_child(cd, p[1], gate[0])` — which never returns (it either
   `execve`s or `_exit`s).
4. **Parent**: close `gate[0]`, `PMIX_POST_OBJECT(child)`, write one byte
   to `gate[1]` and close it, close `p[1]`, return `do_parent(cd, p[0])`.

The pipe is the child→parent error channel: the child sets it
close-on-exec, so if `execve` succeeds the pipe simply **closes with no
data** and the parent reads EOF ⇒ success. If anything fails before exec,
the child writes a fixed-size code-plus-errno record up the pipe and the
parent renders and prints the diagnostic.

`pipe()` or `fork()` failure sets `child->state =
PRTE_PROC_STATE_FAILED_TO_START` and returns `PMIX_ERR_SYS_LIMITS_PIPES` /
`PMIX_ERR_SYS_LIMITS_CHILDREN`.  All four descriptors have to be closed on
those paths — the gate pair is easy to forget, and leaking it leaks two
fds per failed launch.

### The gate — why the child waits to be released

**The child must not be able to die before its pid has been recorded.**
We fork on a *worker* thread, so `child->pid` is stored there, while the
`SIGCHLD` reaper (`wait_signal_callback`,
[`src/runtime/prte_wait.c`](../../../runtime/prte_wait.c)) runs on the
progress thread and attributes every pid it reaps by scanning the wait
trackers for that same field.  A process short-lived enough to run to
completion between `fork()` returning and that store — or merely a worker
thread descheduled right there — therefore matched **no tracker at all**,
and `waitpid` had already consumed the status, so nothing could retry: the
proc never left `RUNNING`, `PRTE_PROC_FLAG_WAITPID` was never set,
`jdata->num_terminated` stopped one short of `num_procs`, the job never
completed, `terminate_orteds` was never called, and `prterun` sat in its
event loop with every line of the job's output already printed.  It
reproduced in roughly 0.3% of `prterun -n 8 --map-by :oversubscribe
hostname` runs.

Registering the tracker earlier cannot fix it — the registration is
already made *before* the fork, deliberately, "to ensure we can capture
the callback on shortlived apps".  It is the **key** that arrives late,
not the tracker.

So `do_child()` blocks on `gate_fd` as its very first act, before
anything that can `_exit()` — including `prte_odls_base_child_fail()`.
The parent writes that byte only after storing the pid, which orders the
store ahead of anything the child can do, the child's exit included.  EOF
releases the child too, so a parent that dies mid-fork cannot strand it.
`PMIX_POST_OBJECT`/`PMIX_ACQUIRE_OBJECT` pair the store with the reaper's
read for the weakly-ordered case.

`odls_base_fork_publish_delay` (an MCA parameter, present in **every**
build — see the note in `odls_base_frame.c`) stalls the daemon in exactly
that window, which turns a fraction of a percent into a certainty:

```sh
prterun --prtemca odls_base_fork_publish_delay 200000 \
        -n 8 --map-by :oversubscribe hostname
```

That is the whole reproducer — one node, one daemon, no harness, because
the race is between one daemon's worker thread and its own progress
thread and nothing else participates.  Remove the gate read and it hangs
on the first run; with the gate it is 8 lines of output and exit 0 every
time.  Run it after touching anything in this fork path.

### `do_child()` — everything between fork and exec

Runs in the forked child; `__prte_attribute_noreturn__`. In order:

1. Block reading `gate_fd` until the parent releases us, then close it —
   see "The gate" above.  This is first for a reason: nothing below it may
   run before our pid has been recorded.
2. `setpgid(0,0)` — new process group so later signals reach grandchildren.
3. Make the pipe write-fd close-on-exec.
4. If this is a real child with output forwarding: `prte_iof_base_setup_child`
   to hook up stdout/stderr, then **`prte_odls_base_set(cd, write_fd)`** —
   the child half of the base binding routine, which only *issues* the
   cpu/memory bind syscalls the parent already prepared
   (`prte_odls_base_prepare_binding`, run pre-fork in `spawn_proc`) and
   proxies any binding error up the pipe. (If there is no child and no
   output forwarding, stdio is tied to `/dev/null`.)
5. Close every inherited descriptor except stdio and the pipe. It
   deliberately does **not** call `pmix_close_open_file_descriptors()`,
   which scans `/proc/self/fd` with `opendir`/`readdir` and so allocates —
   unsafe in the post-fork child.

   It uses **`close_range()`** (Linux) or **`closefrom()`** (BSD/macOS/
   Solaris) where configure found one, falling back to a `close()` loop
   bounded by `sysconf(_SC_OPEN_MAX)`. That fallback is the historical
   code and it is *expensive*: `_SC_OPEN_MAX` is routinely **1048576** on
   a modern system (both the CI containers and a stock macOS report
   exactly that), which makes the loop about **137 ms of pure syscall
   time for every process launched** — measured — against roughly 1 µs
   for the single-syscall form. And it is not merely the child's own
   time: `do_parent` is blocked reading this child's pipe for the whole
   of it, so for a job below the spawn-thread cutoff that cost lands
   serially on the daemon's progress thread. If you touch this, keep the
   bulk path.

   Because both bulk calls take only a *lower* bound and `write_fd` must
   survive, the code closes `[3, write_fd)` one at a time (a handful of
   descriptors) and takes `(write_fd, ∞)` in bulk. The `close_range`
   arm also falls back on a runtime failure: the syscall can be present
   at build time and refused at run time by an older kernel or a seccomp
   policy.
6. Restore the default disposition of **every** signal and unblock them
   all — the event library may have left them altered, and an app must not
   inherit a blocked SIGTERM.

   It has to be every signal, not the handful the daemon itself traps.
   `execve` resets a *handled* signal to its default on its own, but an
   *ignored* one stays ignored across it — and libevent's kqueue backend
   implements a signal event by setting that signal to `SIG_IGN`
   (everything but `SIGCHLD`; see its `kqueue.c`). The daemon registers a
   signal event for every signal it is willing to forward, which by
   default is all of them, so on macOS and the BSDs every application
   PRRTE launched started life with `SIGUSR1`, `SIGUSR2`, `SIGALRM`,
   `SIGCONT` and the rest already ignored, and a forwarded signal arrived
   and was silently discarded. Linux hid it: libevent's epoll backend
   uses a real handler there, which `execve` resets. `SIGKILL` and
   `SIGSTOP` cannot be changed and simply fail, which is harmless.

   The one-line check is `prterun -n 1 <prog>` where `<prog>` calls
   `sigaction(sig, NULL, &oa)` and prints whether each disposition is
   `SIG_DFL`; every one of them must be. Note that a Linux-only harness
   cannot see this at all, so a container test for it would pass
   vacuously.
7. `chdir(cd->wdir)` to the app's working directory.
8. If `PRTE_JOB_STOP_ON_EXEC`: `ptrace(PRTE_TRACEME, …)` so the app stops at
   `execve` for a debugger to attach.
9. **`execve(cd->cmd, cd->argv, cd->env)`.** On return (always an error) it
   simply reports `PRTE_ODLS_CHILD_ERR_EXEC` plus `errno`; the *parent*
   inspects `errno` and `stat`s the app to distinguish a bad interpreter
   (`ENOENT` but the file exists) from a missing/failed executable and
   renders `"execve error"`. (`cd->argv` is defaulted in the *parent*
   before the fork, so the child never allocates it.)

Every fatal failure calls `prte_odls_base_child_fail()` (writes the fixed
record, `_exit`s); binding warnings call `prte_odls_base_child_warn()`
(writes the record, returns). Both live in the base (`odls_base_bind.c`)
so the component and the binding code share one implementation. The record
is a fixed-size `prte_odls_pipe_err_msg_t` — no strings, no allocation, no
`show_help` — carrying a `prte_odls_child_err_t` code and `errno`; the
parent does all rendering.

### `do_parent()` — block until the child reports

Runs on the event base. Closes the child ends of the IOF pipes, then loops
reading fixed-size `prte_odls_pipe_err_msg_t` records:

- **Pipe closed / read timeout** (`PMIX_ERR_TIMEOUT`) ⇒ child exec'd
  successfully: set `child->state = RUNNING`, flag `ALIVE`, return
  `PRTE_SUCCESS`.
- **A record arrives** ⇒ `render_child_msg()` maps the code + `errno` to the
  right `pmix_show_help` topic and renders it (allocation and `show_help`
  are safe here in the parent). If `msg.fatal`, set `child->state =
  FAILED_TO_START`, unset `ALIVE`, and return `PRTE_ERR_SILENT` (the message
  was already shown). If it was only a warning, keep looping.
- **Read error** ⇒ set `child->state = UNDEF` and return a converted error.

The `PRTE_ERR_SILENT` return propagates back through the base's
`spawn_proc`, which then activates `PRTE_PROC_STATE_FAILED_TO_START`.

---

## The signal/kill primitives

- **`odls_default_kill_local(pid, signum)`** — used by the base kill path.
  When `HAVE_SETPGID`, it targets `-pgrp` (the process group's lead) so the
  signal reaches any children the app spawned. `ESRCH` (already gone) is
  treated as success.
- **`send_signal(pd, signal)`** — used by the base signal path. Honors the
  `prte_odls_globals.signal_direct_children_only` MCA flag: if set, signals
  only `pd`; otherwise signals the whole group (`-pd`). Maps `kill(2)` errno
  to PRRTE codes (`ESRCH` ⇒ ignored, `EPERM` ⇒ `PRTE_ERR_PERM`, etc.).

Both are static helpers passed as function pointers into the base — the
base owns the *policy* (which procs, what escalation), the component owns
the *mechanism* (the actual `kill`).

---

## Things to watch when editing

- **`do_child` is post-fork: async-signal-safety rules apply.** It runs in
  a forked child that has not yet exec'd. Keep it to async-signal-safe
  syscalls and the fixed-record `child_fail`/`child_warn` idiom; do not add
  malloc-heavy or lock-taking PRRTE calls, do not scan `/proc/self/fd`, and
  never render `show_help` or log through the normal channels — writing the
  fixed code-plus-errno record up the pipe is the only safe way to report.
- **Preserve the pipe protocol on both ends.** The child writers
  (`prte_odls_base_child_fail`/`_warn` in `odls_base_bind.c`, used by both
  `do_child` and the binding code) and the read loop in `do_parent` must
  agree on the fixed `prte_odls_pipe_err_msg_t` layout and the
  `prte_odls_child_err_t` code set. Adding a new failure point means adding
  a code to the enum **and** a case to `render_child_msg` in the parent.
  [`test/unit/odls/test_odls.c`](../../../../test/unit/odls/) drives both
  writers across a real pipe (and `child_fail` across a real `fork`) and
  checks the decoded record, so a layout change that breaks the pair fails
  `make check` rather than in production.
- **`do_parent` blocks the event base it runs on.** The read on the pipe is
  synchronous by design: the whole protocol is "the parent waits until the
  child either exec's or explains itself," and the child's window is a
  handful of syscalls. That is also why nothing slow may be added to
  `do_child` — a child that stalls before `execve` stalls whichever base
  the base layer dispatched this spawn to, which for a small job is
  `prte_event_base` itself.
- **`execve` is the point of no return.** Everything the app needs — cwd,
  env (`cd->env`), argv, binding, closed fds, restored handlers — must be
  in place *before* it. The base assembles env/argv/cmd in `spawn_proc`; the
  child only finalizes cwd, binding, fds, and signals.
- **STOP_ON_EXEC threading.** The `PTRACE_TRACEME` here pairs with the
  detach in the base's `wait_local_proc`; both must stay on
  `prte_event_base` (the base forces this). Don't move the fork onto a
  worker thread for a stop-on-exec job.
- **Don't reimplement base policy.** New behavior for *which* procs to
  launch/kill/signal, retries, IOF wiring, waitpid interpretation, or state
  transitions belongs in `base/odls_base_default_fns.c`, shared by any
  future component. Keep `pdefault` to the OS primitives.
- **`_exit`, not `exit`, in the child.** The child terminates via
  `prte_odls_base_child_fail` (which `_exit`s) so it never runs the parent's
  atexit handlers or flushes shared buffers.

---

## Use `prte_show_help()`, never `pmix_show_help()`

Everything above exists so the *parent* can render a diagnostic the child
could not — and for a long time that diagnostic was **silently dropped on
every node but the head one**.

`pmix_show_help()` routes through PMIx's `plog` framework, and
`plog/stdfd` writes its own `stderr` only when the calling process is a
PMIx **client or tool**. A `prted` is a PMIx **server**, so it takes the
other branch, which hands the rendered text to
`PMIx_server_IOF_deliver()` tagged with the *daemon's own* identity — for
which nothing has an IOF sink. The head node appeared to work only
because there the daemon's PMIx server is the one holding the tool
connection.

[`prte_show_help()`](../../../util/prte_show_help.h) is the drop-in
replacement: identical signature and rendering, but on a non-master
daemon it renders locally and relays the text to the HNP over
`PRTE_RML_TAG_SHOW_HELP`, and the HNP delivers it. Duplicate suppression
and aggregation then happen once, centrally, keyed by the same
filename/topic pair — better than the per-node suppression it replaces.

Every call site in this component and in the base is converted. **Keep it
that way**: a new `pmix_show_help()` here is a message that will not be
seen off the head node, and nothing will tell you.

Verify with a directory as the executable — `access(2)` reports a
directory as `X_OK` ("searchable"), so it clears the base's
`check_context_app()` and only fails at the `execve`:

```sh
prterun --host <compute>:1 -n 1 /tmp
#   ...
#   Local host:        <compute>      <-- rendered there, delivered here
#   Error:             it is a directory, not an executable
```

The swarm's `test_odls` asserts exactly that, including that the message
names the remote node.
