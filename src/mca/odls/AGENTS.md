# AGENTS.md — The `odls` Framework (Daemon Local Launch Subsystem)

Orientation for AI agents and human contributors working in
`src/mca/odls/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`odls` is the **PRTE Daemon's Local Launch Subsystem**. It is the code
that actually **forks and execs the application processes** on each node,
tracks them, signals them, reaps them via `waitpid`, and reports their
state transitions back into the job state machine. Where `rmaps` decides
*which* proc goes *where* (on the HNP), `odls` is what *does the launch*
(on every daemon).

Unusually for an MCA framework, `odls` has two very different jobs
depending on where the process runs:

| Role | Where | What odls does |
|------|-------|----------------|
| **HNP / DVM master** | one process | Serializes the computed placement into a single **launch message** (`get_add_procs_data`) that is broadcast to all daemons. |
| **prted (daemon)** | every node | Parses that message (`construct_child_list`), works out which procs are *local*, and fork/execs them (`launch_local_procs`). |

The HNP is itself a daemon (vpid 0), so it also launches any local procs
assigned to its own node — but its launch message construction is the
part unique to it.

### Place in the launch state machine

```
… → MAP → MAP_COMPLETE → SYSTEM_PREP → LAUNCH_DAEMONS → … → LAUNCH_APPS → RUNNING → …
                                                              ▲
                                                              └── odls runs here
```

The flow that drives odls (see `src/mca/plm/base/plm_base_launch_support.c`
and `src/prted/prted_comm.c`):

1. A job reaches `PRTE_JOB_STATE_LAUNCH_APPS`. On the HNP,
   `prte_plm_base_launch_apps()` packs the daemon command
   (`PRTE_DAEMON_ADD_LOCAL_PROCS`, or `PRTE_DAEMON_DVM_ADD_PROCS` for a
   fixed DVM) into `jdata->launch_msg`, then calls
   `prte_odls.get_add_procs_data()` to append the placement/regex/setup
   payload.
2. That call ends (asynchronously, after `PMIx_server_setup_application`
   returns) by activating `PRTE_JOB_STATE_SEND_LAUNCH_MSG`, which xcasts
   `jdata->launch_msg` to every daemon over the RML.
3. Each daemon's `prted_comm.c` dispatch sees `PRTE_DAEMON_ADD_LOCAL_PROCS`
   and calls `prte_odls.launch_local_procs(buffer)`.
4. `launch_local_procs` → `construct_child_list` (decode) →
   `PRTE_ACTIVATE_LOCAL_LAUNCH` → `launch_local` (per-app fork/exec) →
   each child transitions to `PRTE_PROC_STATE_RUNNING`.

The same module also fields the kill/signal daemon commands
(`PRTE_DAEMON_KILL_LOCAL_PROCS`, `PRTE_DAEMON_SIGNAL_LOCAL_PROCS`) and
the errmgr's restart path.

---

## Directory layout

```
odls/
  odls.h                    # module vtable (5 fn ptrs) + component typedef + version macro
  odls_types.h              # PRTE_DAEMON_* command flags; child-error pipe struct
  base/
    base.h                  # framework globals struct, base-fn prototypes, the two caddy classes,
                            #   PRTE_ACTIVATE_LOCAL_LAUNCH / PRTE_ODLS_SET_ERROR macros
    odls_base_frame.c       # open/close/register; MCA params; the spawn-thread pool; class instances
    odls_base_select.c      # component selection (pick ONE, highest priority)
    odls_base_default_fns.c  # THE big one: build msg, parse msg, wireup, env setup, spawn, waitpid,
                            #   kill, restart — everything a component reuses
    odls_base_bind.c        # prte_odls_base_set(): apply cpu/memory binding in the child pre-exec,
                            #   proxy binding errors up the pipe
    help-prte-odls-base.txt  # xterm-related error text
  pdefault/                 # the only component (pri 10): real fork()/execve() launcher
```

Read `odls.h` and `base/base.h` first (the contract and the shared data
structures), then `base/odls_base_default_fns.c`, which is where almost
all real work lives. The `pdefault` component is a thin shell around the
base helpers — read it last.

---

## The module contract

Every odls component fills in a `prte_odls_base_module_t` (declared in
`odls.h`) with five function pointers:

```c
typedef struct prte_odls_base_module_1_3_0_t {
    prte_odls_base_module_get_add_procs_data_fn_t     get_add_procs_data;
    prte_odls_base_module_launch_local_processes_fn_t launch_local_procs;
    prte_odls_base_module_kill_local_processes_fn_t   kill_local_procs;
    prte_odls_base_module_signal_local_process_fn_t   signal_local_procs;
    prte_odls_base_module_restart_proc_fn_t           restart_proc;
} prte_odls_base_module_t;
```

| Function | Signature | Runs on | Meaning |
|----------|-----------|---------|---------|
| `get_add_procs_data` | `(pmix_data_buffer_t *data, pmix_nspace_t job)` | HNP | Serialize the whole job (proc→node map, regex nodemap/procmap, personality, uid/gid, app-setup info) into `data` for broadcast. Returns `PRTE_SUCCESS`/error. |
| `launch_local_procs` | `(pmix_data_buffer_t *data)` | daemon | Decode the message, build this node's child list, fork/exec the local procs. Returns `PRTE_SUCCESS`/error. |
| `kill_local_procs` | `(pmix_pointer_array_t *procs)` | daemon | Kill the listed procs (`NULL` ⇒ all local procs). Escalates SIGCONT→SIGTERM→SIGKILL. |
| `signal_local_procs` | `(const pmix_proc_t *proc, int32_t signal)` | daemon | Deliver `signal` to one proc (`NULL` ⇒ all local procs). |
| `restart_proc` | `(prte_proc_t *child)` | daemon | Re-fork a single already-known child (fault recovery / comm-spawn restart). |

The return protocol is the ordinary PRRTE one: `PRTE_SUCCESS` or a
`PRTE_ERR_*`. Unlike `rmaps`, there is **no** "take next option" — one
component wins and owns every call. Errors on the daemon side almost
always end by activating a proc or job **error state** rather than
returning up the stack, because the launch runs asynchronously on the
event loop (`PRTE_ACTIVATE_PROC_STATE(..., PRTE_PROC_STATE_FAILED_TO_LAUNCH)`,
`PRTE_ACTIVATE_JOB_STATE(..., PRTE_JOB_STATE_NEVER_LAUNCHED)`).

The version macro is `PRTE_ODLS_BASE_VERSION_2_0_0`.

---

## Component selection is "pick one"

`prte_odls_base_select()` (`odls_base_select.c`) is the standard MCA
"select the single best component" pattern: it calls `pmix_mca_base_select`,
copies the winning module into the global `prte_odls`, and everything in
the tree calls through `prte_odls.<fn>()`. There is currently exactly one
component — `pdefault`, priority **10** — deliberately low so a
site-specific launcher could override it. The framework's open/select
logic only runs the launch machinery inside a daemon; a tool never
selects an odls module for launching.

---

## What `base/` provides — the heart of the framework

Because there is only one component and it delegates almost everything,
**the base is the framework.** A component supplies just the primitive
`fork_local_proc` (and the raw `kill`/`signal` syscalls); the base does
message construction, parsing, wireup, environment assembly, threading,
`waitpid` interpretation, and cleanup. Walk these in order.

### 1. Framework globals and the spawn-thread pool (`odls_base_frame.c`)

`prte_odls_globals` (`prte_odls_globals_t` in `base.h`) holds:

- **`ev_bases` / `ev_threads` / `num_threads` / `max_threads` / `cutoff` /
  `next_base`** — a pool of libevent progress threads used to *parallelize
  forking* when a node hosts many local procs. `prte_odls_base_start_threads()`
  decides how many to spin up: a persistent DVM uses `max_threads` (default
  16); otherwise, below `cutoff` (default 32) local procs it uses **zero**
  dedicated threads (fork straight on `prte_event_base`), and above it
  scales to `num_local_procs / 8` capped at `max_threads`.
  `prte_odls_base_harvest_threads()` tears them down.
- **`xterm_ranks` / `xtermcmd`** — support for `--xterm`: a list of ranks
  whose output should be shown in separate `xterm` windows, parsed at
  framework open from the `prte_xterm` global.
- **`signal_direct_children_only`** — MCA flag controlling whether signals
  go to the child only or its whole process group.
- **`exec_agent`** — an optional wrapper command to exec instead of the app.

MCA params, all under `prte odls base`: `max_threads`, `num_threads`,
`cutoff`, `signal_direct_children_only`, `exec_agent`. Framework open also
**unblocks `SIGCHLD`** (odls must see child deaths) and builds the xterm
command vector. Framework close reaps the thread pool and releases the
global `prte_local_children` array.

This file also defines the two caddy classes (below) via
`PMIX_CLASS_INSTANCE`.

### 2. Building the launch message — `prte_odls_base_default_get_add_procs_data()`

Runs **on the HNP only**. Packs into the supplied buffer, in a strict
order that the parser below must mirror (there is a literal comment in the
source warning about this):

1. An `int8` flag: were new daemons launched for this job? If so, pack a
   nested buffer containing **every other active job** (`prte_job_pack`)
   plus each proc's `parent` daemon vpid — so a freshly added daemon
   learns about pre-existing jobs and can route collectives correctly.
2. The job being launched (`prte_job_pack`).
3. A **nodemap** regex and a **procmap** (ppn) regex, generated by asking
   the PMIx server (`PMIx_generate_regex` / `PMIx_generate_ppn`) to
   compress the node names and the per-node rank lists.
4. Job info: personality, a per-job network allocation request, the
   launching user's `uid`/`gid`, and — if envars have not yet been
   harvested — a `PMIX_SETUP_APP_ENVARS` directive.

It then calls `PMIx_server_setup_application()` **asynchronously**. The
completion callback `setup_cbfunc()` packs whatever setup blob PMIx
returned (as a `PMIX_BYTE_OBJECT`) onto `jdata->launch_msg`, then activates
`PRTE_JOB_STATE_SEND_LAUNCH_MSG` and wakes the waiting thread. The function
blocks on a `prte_pmix_lock_t` until that callback fires.

### 3. Parsing the message and wiring up — `prte_odls_base_default_construct_child_list()`

Runs **on every daemon** (including the HNP). This is the mirror image of
`get_add_procs_data`, and the single most important function to understand:

- Unpacks the "new daemons" flag; on a **non-master** daemon it unpacks the
  prior-jobs blob, adds each unknown job to the local `prte_job_data`
  array, reconnects each of its procs to the owning daemon's node, and
  registers the nspace with the local PMIx server. (The **master already
  has** all of this, so it discards its copy — `jdata->index = -1;
  PMIX_RELEASE`.)
- Unpacks the job to launch. On the master it throws away the unpacked copy
  and fetches the fully-populated local `prte_job_t`; on a daemon it keeps
  the unpacked copy, creates a `map` if needed, and resolves the job's
  **schizo** personality via `prte_schizo_base_detect_proxy()`.
- Unpacks the optional app-setup byte object, and folds any
  `PMIX_SET/ADD/UNSET/PREPEND/APPEND_ENVAR` items into the job attributes
  (prepended, so they apply before launch).
- **Wireup loop:** for every proc in the job, connect it to its node via
  the parent daemon (`daemons->procs[pptr->parent]->node`), add the node to
  the job map once (guarded by `PRTE_NODE_FLAG_MAPPED`, which is then reset),
  and — crucially — decide **locality**: if `pptr->parent ==
  PRTE_PROC_MY_NAME->rank`, the proc is *mine*. Local procs are retained
  onto the global **`prte_local_children`** array, flagged
  `PRTE_PROC_FLAG_LOCAL`, counted into `jdata->num_local_procs`, and their
  app is flagged `PRTE_APP_FLAG_USED_ON_NODE`. Restart jobs get
  `PRTE_PROC_NOBARRIER` set.
- Registers the nspace with the PMIx server (`prte_pmix_server_register_nspace`),
  runs `PMIx_server_setup_local_support` if setup info was present, starts
  the spawn threads, and blocks until local support is ready.
- On any failure it activates `PRTE_JOB_STATE_NEVER_LAUNCHED` so the HNP
  doesn't hang waiting for a daemon that silently died.

### 4. Kicking off the fork — `PRTE_ACTIVATE_LOCAL_LAUNCH` and `prte_odls_base_default_launch_local()`

The component's `launch_local_procs` finishes by invoking the
`PRTE_ACTIVATE_LOCAL_LAUNCH(job, fork_local_proc)` macro (in `base.h`),
which allocates a `prte_odls_launch_local_t` caddy, stashes the component's
`fork_local` primitive on it, and posts `prte_odls_base_default_launch_local`
to `prte_event_base`.

`prte_odls_base_default_launch_local()` is the per-node launch driver:

- Records a baseline `getcwd` (it will `chdir` around per app and must
  return here).
- Enforces the **system limits** on total children and open file
  descriptors; if over budget it retries via a `PRTE_DETECT_TIMEOUT` timer
  (up to a few times) rather than failing outright.
- For each **app used on this node**: sets up the working directory
  (`setup_path`, honoring `PRTE_APP_SSNDIR_CWD` / `PRTE_APP_USER_CWD`),
  merges `prte_launch_environ` into `app->env`, applies env directives
  (`process_envars` — the SET/ADD/UNSET/PREPEND/APPEND handling, with app
  attributes trumping job attributes), calls the schizo's `setup_fork`,
  links prepositioned files (`prte_filem`), checks the executable, and
  applies resource limits.
- For each **local child of that app** in `INIT`/`RESTART` state: registers
  the `waitpid` callback (`prte_wait_cb` → `prte_odls_base_default_wait_local_proc`),
  sets `PRTE_PROC_FLAG_ALIVE`, allocates a **`prte_odls_spawn_caddy_t`**,
  sets up IOF (`prte_iof_base_setup_prefork` / `setup_parent`), picks the
  next event base from the thread pool, and posts
  `prte_odls_base_spawn_proc` to it.

**STOP_ON_EXEC caveat:** if `PRTE_JOB_STOP_ON_EXEC` is set (debugger
attach), the fork is forced onto `prte_event_base` rather than a worker
thread, because the ptrace tracer must be the same thread that later
detaches — see the long comment near the thread-selection code.

### 5. The spawn step — `prte_odls_base_spawn_proc()`

Runs on the chosen event base. This is the last common code before the
component's raw fork:

- Honors `PRTE_JOB_DO_NOT_SPAWN` (mapping-only "donotlaunch" jobs): just
  mark the child `TERMINATED` and return.
- Calls `PMIx_server_setup_fork()` to inject the PMIx client environment.
- Resolves the actual command/argv: normal app, or `--xterm` wrapper, or a
  per-job `PRTE_JOB_EXEC_AGENT`, or the global `exec_agent`; optionally
  index-suffixes `argv[0]` with the rank (`PRTE_JOB_INDEX_ARGV`).
- Calls the component's **`cd->fork_local(cd)`** — the actual `fork`/`execve`.
- On success stores the pid (on the master) and activates
  `PRTE_PROC_STATE_RUNNING`; on failure activates a failure state.

### 6. Applying binding — `prte_odls_base_prepare_binding()` + `prte_odls_base_set()` (`odls_base_bind.c`)

Binding is split across the fork so the child stays async-signal-safe:

- **`prte_odls_base_prepare_binding(cd)`** runs in the **parent**, in
  `spawn_proc` just before `fork_local`. It does everything that
  allocates, parses, or prints: it parses the proc's computed
  `child->cpuset` (the hwloc bitmap string the mapper produced) into a
  stored `hwloc_cpuset_t`, classifies the binding, precomputes the
  memory-binding policy, emits `--report-bindings` output and the
  "incorrectly bound" warning, and — where the platform has
  `sched_setaffinity` (`PRTE_HAVE_SCHED_SETAFFINITY`) — precomputes a raw
  `cpu_set_t` affinity mask. All of this is stashed on the caddy.
- **`prte_odls_base_set(cd, write_fd)`** runs in the forked **child**, in
  the async-signal-safe window before `execve`. It only *issues the bind
  syscalls*: a bare `sched_setaffinity` with the precomputed mask on Linux,
  or `hwloc_set_cpubind` as the `#else` fallback (macOS and other platforms
  without `sched_setaffinity`), plus `hwloc_set_membind`. It allocates
  nothing and renders nothing.

Because the child is not a real PRTE process — and runs in that
async-signal-safe window — **it cannot use normal error reporting or
render a `show_help` message** (that allocates, reads the help file, and
scans directories, any of which can deadlock in a forked child). It
reports a fixed-size code-plus-errno record up the pipe via
`prte_odls_base_child_fail` (fatal, `_exit`s) / `prte_odls_base_child_warn`
(non-fatal, returns) — the `prte_odls_pipe_err_msg_t` /
`prte_odls_child_err_t` types in `odls_types.h` — and the *parent* renders
the human-readable diagnostic. Whether a binding failure is fatal or a
warning depends on `PRTE_BINDING_REQUIRED` and `PRTE_BINDING_POLICY_IS_SET`
(a *required, explicitly-requested* binding that fails kills the child; a
defaulted one degrades to a warning). If the proc has no cpuset but the
daemon itself is bound, the proc is "freed" to all allowed cpus.

The one remaining hwloc call in the child is `hwloc_set_membind` (memory
binding), which still allocates internally; converting it to a bare
`set_mempolicy`/`mbind` syscall would mean reproducing hwloc's NUMA
nodeset handling and is left for later.

### 7. Reaping children — `prte_odls_base_default_wait_local_proc()`

The `waitpid` callback, registered per child and fired by
`src/runtime/prte_wait.c` when SIGCHLD is reaped. It decodes
`proc->exit_code` (the raw wait status) into a proc state:

- `WIFEXITED` + zero ⇒ `PRTE_PROC_STATE_WAITPID_FIRED`.
- `WIFEXITED` + nonzero, with `PRTE_JOB_ERROR_NONZERO_EXIT` set ⇒
  `PRTE_PROC_STATE_TERM_NON_ZERO`.
- Exited "normally" but never did the required PMIx init/finalize sync ⇒
  `PRTE_PROC_STATE_TERM_WO_SYNC` (checked against `PRTE_PROC_FLAG_REG` /
  `PRTE_PROC_FLAG_HAS_DEREG` and `prte_allowed_exit_without_sync`).
- `WIFSIGNALED` ⇒ `PRTE_PROC_STATE_ABORTED_BY_SIG`, and the exit code is
  rewritten to `signo + 128` (shell convention, so `prog` and `prun prog`
  agree).
- Proc that called `prte_abort` ⇒ `PRTE_PROC_STATE_CALLED_ABORT`; a proc
  ordered dead (`KILLED_BY_CMD`) is passed straight through.
- **STOP_ON_EXEC** (`WIFSTOPPED` + `SIGTRAP` under `PRTE_JOB_STOP_ON_EXEC`):
  this is the debugger-attach stop. Detach with SIGSTOP so the child stays
  parked for the debugger, re-register the waitpid, fire
  `PRTE_PROC_STATE_READY_FOR_DEBUG`, and **do not** fall through to exit
  handling. This detach must run on `prte_event_base` (see the fork
  thread-affinity note above).

It ends at `MOVEON:` by cancelling the wait tracker and activating the
computed proc state.

### 8. Kill / signal / restart

- **`prte_odls_base_default_kill_local_procs()`** — walks the requested
  procs against `prte_local_children`, closes stdin IOF, cancels the
  waitpid (to avoid races), then escalates **SIGCONT → SIGTERM → SIGKILL**
  with `nanosleep` gaps, marking each `KILLED_BY_CMD`. It calls the
  component's raw `kill_local(pid, signum)`.
- **`prte_odls_base_default_signal_local_procs()`** — finds the target
  child (or all) and calls the component's raw `signal_local(pid, signum)`.
- **`prte_odls_base_default_restart_proc()`** — resets a single known
  child's state/flags and re-dispatches it through `prte_odls_base_spawn_proc`
  (same caddy/thread/IOF machinery as a first launch).

---

## Key data structures

| Type | Where | Purpose |
|------|-------|---------|
| `prte_odls_base_module_t` | `odls.h` | The 5-pointer vtable; the selected one lives in the global `prte_odls`. |
| `prte_odls_globals_t` / `prte_odls_globals` | `base.h` / `frame.c` | Framework-wide state: the spawn-thread pool, xterm ranks, exec agent, signal policy. |
| `prte_local_children` | `src/runtime/prte_globals` (a `pmix_pointer_array_t`) | The daemon's authoritative list of the procs it launched — every base fn iterates it. Allocated at framework open, released at close. |
| `prte_odls_spawn_caddy_t` | `base.h` | Per-child fork caddy: `cmd`, `wdir`, `argv`, `env`, `jdata`, `app`, `child`, IOF `opts`, and the `fork_local` fn ptr. Carries `ev` for thread-shifting. Heap-allocated, released after spawn. |
| `prte_odls_launch_local_t` | `base.h` | Per-node "start launching job J" caddy carried by `PRTE_ACTIVATE_LOCAL_LAUNCH`; holds `job`, `fork_local`, and a `retries` counter for the sys-limit backoff. |
| `prte_odls_pipe_err_msg_t` / `prte_odls_child_err_t` | `odls_types.h` | Fixed-size record written up the child→parent pipe (fatal flag + exit status + failure code + errno). Carries no strings and needs no allocation, so it is safe to emit from the async-signal-safe window before `execve`; the parent renders the `show_help` diagnostic from the code and errno. |
| `PRTE_DAEMON_*` command flags | `odls_types.h` | The daemon command byte that leads every RML control message to a prted (ADD_LOCAL_PROCS, KILL, SIGNAL, EXIT, …). |

---

## Threading model

- **Message build/parse, wireup, waitpid interpretation, kill/signal, and
  state activation** all run on the **progress thread** (`prte_event_base`)
  — the normal PRRTE event-driven model.
- **Only the fork/exec spawn step** may be off-loaded to the **odls
  worker-thread pool** (`prte_odls_globals.ev_bases`) to parallelize
  launching many procs. Each spawn is a self-contained caddy handed to one
  worker base; it touches only its own child, so no shared-state locking is
  needed on the hot path.
- `SIGCHLD` must stay unblocked (done at framework open); child death is
  delivered through `src/runtime/prte_wait.c`, which fires the registered
  `wait_local_proc` callback on `prte_event_base`.
- The two blocking base functions (`get_add_procs_data`,
  `construct_child_list`) use a `prte_pmix_lock_t` to wait for the async
  PMIx server callbacks, per the house caddy/lock pattern.

---

## Gotchas when editing

- **Pack/parse symmetry is sacred.** `get_add_procs_data` and
  `construct_child_list` are a hand-matched serializer/parser pair. Any
  change to the packed order/type in one **must** be mirrored in the other,
  or daemons will mis-decode the launch message and the job hangs or
  crashes. The source says so in capitals — heed it.
- **Locality is `parent == my vpid`.** A proc is "local" iff its `parent`
  daemon vpid equals this daemon's rank. Getting the wireup wrong silently
  launches procs on the wrong node or not at all.
- **`prte_local_children` is the single source of truth** on a daemon.
  Adding/removing a child there, and its `PRTE_PROC_FLAG_*` flags
  (`LOCAL`, `ALIVE`, `WAITPID`, `IOF_COMPLETE`, `REG`), gate the whole
  lifecycle. A child is only fully released once **both** `WAITPID` and
  `IOF_COMPLETE` are set.
- **Failure means activating a state, not returning.** On the daemon side
  the launch is asynchronous; report errors with
  `PRTE_ACTIVATE_PROC_STATE(FAILED_TO_LAUNCH/FAILED_TO_START)` or
  `PRTE_ACTIVATE_JOB_STATE(NEVER_LAUNCHED)` so the HNP can react — don't
  just bubble an `rc` up into the event loop.
- **The child cannot log normally — or render `show_help`.** Between
  `fork` and `execve` only async-signal-safe calls are permitted, so the
  child must not allocate, use stdio, scan `/proc/self/fd`, or call
  `show_help` (all of which can deadlock in a forked child). It reports a
  fixed-size code-plus-errno record up the pipe
  (`prte_odls_base_child_fail` / `prte_odls_base_child_warn`) and the
  parent renders the message; never call ordinary PRRTE logging there.
- **STOP_ON_EXEC pins the tracer thread.** Both the fork (in
  `launch_local`/`restart_proc`) and the ptrace detach (in
  `wait_local_proc`) must happen on `prte_event_base`; do not "optimize"
  them onto a worker thread.
- **`chdir` bookkeeping.** `launch_local`/`restart_proc` bounce the daemon's
  cwd per app and must always `chdir` back to `basedir` before returning.
- Standard PRRTE rules apply: `prte_config.h` first, braces on every block,
  `NULL ==`/constant-on-left comparisons, `PRTE_ERROR_LOG` for unexpected
  errors, no new compiler warnings.

---

## Debugging

```sh
prte --prtemca odls_base_verbose 5 ...     # trace child-list build, dispatch, waitpid
prte --prtemca odls_base_verbose 10 ...    # + sys-limit checks, per-thread dispatch
prte --prtemca odls_base_verbose 20 ...    # >15 dumps the exact argv/env being exec'd
prte --prtemca state_base_verbose 5 ...    # see LAUNCH_APPS / RUNNING transitions odls drives
prun --xterm 0,1 ...                        # route ranks 0,1 to xterm windows (see frame.c)
prun --report-bindings ...                  # print each child's applied binding (odls_base_bind.c)
```

Useful tuning params: `--prtemca odls_base_num_threads N` /
`odls_base_cutoff N` (spawn-thread pool sizing),
`--prtemca odls_base_exec_agent CMD` (wrap every exec),
`--prtemca odls_base_signal_direct_children_only 1` (don't signal the
child's whole process group).

---

## Where to go next

The launch primitive itself lives in the component:

- [`pdefault/AGENTS.md`](pdefault/AGENTS.md) — the default (and only)
  local-launch component: the real `fork()`/`execve()`, the parent/child
  pipe protocol, and how it plugs `fork_local_proc` into the base.
