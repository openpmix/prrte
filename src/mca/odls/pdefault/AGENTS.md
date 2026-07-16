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
**pipe-synchronized fork**:

1. Open a pipe `p[2]`.
2. `fork()`. Record `child->pid` (in *both* parent and child copies).
3. **Child** (`pid == 0`): close the read end, call `do_child(cd, p[1])` —
   which never returns (it either `execve`s or `_exit`s).
4. **Parent**: close the write end, return `do_parent(cd, p[0])`.

The pipe is the child→parent error channel: the child sets it
close-on-exec, so if `execve` succeeds the pipe simply **closes with no
data** and the parent reads EOF ⇒ success. If anything fails before exec,
the child writes a fixed-size code-plus-errno record up the pipe and the
parent renders and prints the diagnostic.

`pipe()` or `fork()` failure sets `child->state =
PRTE_PROC_STATE_FAILED_TO_START` and returns `PMIX_ERR_SYS_LIMITS_PIPES` /
`PMIX_ERR_SYS_LIMITS_CHILDREN`.

### `do_child()` — everything between fork and exec

Runs in the forked child; `__prte_attribute_noreturn__`. In order:

1. `setpgid(0,0)` — new process group so later signals reach grandchildren.
2. Make the pipe write-fd close-on-exec.
3. If this is a real child with output forwarding: `prte_iof_base_setup_child`
   to hook up stdout/stderr, then **`prte_odls_base_set(cd, write_fd)`** —
   the base binding routine (cpu + memory affinity from `child->cpuset`),
   which proxies any binding error up the pipe. (If there is no child and
   no output forwarding, stdio is tied to `/dev/null`.)
4. A plain `close()` loop over `[3, sysconf(_SC_OPEN_MAX))` — closes
   everything except stdio and the pipe. (It deliberately does **not** call
   `pmix_close_open_file_descriptors()`, which scans `/proc/self/fd` with
   `opendir`/`readdir` and so allocates — unsafe in the post-fork child.)
5. Restore default signal handlers (`SIGTERM/INT/HUP/PIPE/CHLD`) and
   unblock all signals — the event library may have left them altered, and
   an app must not inherit a blocked SIGTERM.
6. `chdir(cd->wdir)` to the app's working directory.
7. If `PRTE_JOB_STOP_ON_EXEC`: `ptrace(PRTE_TRACEME, …)` so the app stops at
   `execve` for a debugger to attach.
8. **`execve(cd->cmd, cd->argv, cd->env)`.** On return (always an error) it
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
