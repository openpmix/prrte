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
  `prte_odls_base_harvest_threads()` tears them down and resets the pool
  state to "nothing built yet".

  Two things about this that are easy to get wrong, because both failures
  are invisible:
  - **"Have we built a pool?" is `ev_bases != NULL`, not
    `ev_threads != NULL`.** `ev_threads` holds the *names* of dedicated
    threads and stays NULL whenever the answer was "no dedicated threads
    at all", so keying the guard off it re-entered the sizing code on
    every job.
  - **`num_threads` is both the request and the answer**, which makes it
    dangerous to reset. It is registered at `-1` ("you decide") and
    `start_threads` *overwrites* it with the size it picked. So `-1` in
    that variable is a standing invitation to size a pool from whatever
    job comes next — and `harvest_threads` runs from framework **close**.
    Putting the sentinel back there let a `start_threads` call arriving
    during teardown build a whole new pool of real threads on the way out
    (a 40-proc node printed `START 5 LAUNCH THREADS` twice, the second
    time after `kill_local_procs`). Harvest records `0` — "no threads" —
    and `start_threads` returns immediately once `prte_finalizing` is set.
    Do not "restore" the sentinel there.

  **`prte_persistent` is what makes the difference here, and a daemon has
  to be *told* it.** `start_threads` short-circuits to `max_threads` (16)
  when `prte_persistent` is set, on the reasoning that a persistent DVM
  will service many jobs and should size for the worst of them. That flag
  is decided in `prte()` (`src/prted/prte.c`) — which only the HNP runs.
  It used to default to **true** and be assigned nowhere else, so every
  daemon believed it was persistent no matter how the DVM had started: a
  plain `prterun -n 1 hostname` spun 16 progress threads on each compute
  node, and the cutoff/`num_local_procs / 8` scaling below was dead
  everywhere but the head node. It is now an MCA parameter that the HNP
  appends to each daemon's command line
  (`prte_plm_base_prted_append_basic_args`), defaulting to false; a
  bootstrapped daemon, which has no launcher to tell it anything, sets it
  for itself because that DVM is persistent by construction. If you add
  daemon-side code that reads `prte_persistent`, it now means what it
  says — but check `--prtemca odls_base_verbose 5` for `START n LAUNCH
  THREADS` if you are ever unsure which way a given daemon decided.
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

1. The job being launched (`prte_job_pack`).
2. A **nodemap** regex and a **procmap** (ppn) regex, generated by asking
   the PMIx server (`PMIx_generate_regex` / `PMIx_generate_ppn`) to
   compress the node names and the per-node rank lists.
3. Job info: personality, a per-job network allocation request, the
   launching user's `uid`/`gid`, and — if envars have not yet been
   harvested — a `PMIX_SETUP_APP_ENVARS` directive.

**Note what is deliberately *not* here.** This message used to lead with an
`int8` flag and, when a launch had brought new daemons in, a nested buffer
holding **every other active job** — so a freshly added daemon could resolve
their namespaces. That tied the size of the launch message to the number of
jobs resident in the DVM, sent them to every daemon rather than the ones that
needed them, and still did nothing for a daemon added by a bare elastic grow,
which launches no job and therefore sends no launch message at all. Those
jobs now ride with the nidmap at `VM_READY`
(`prte_util_pack_job_catchup()` in [`src/util/nidmap.c`](../../util/nidmap.c)),
which is sent precisely when the daemon set changes.

**And note what does the placement now.** The job no longer packs a record
per process saying where it went: it packs a node map and one proc map per
app, and the receiver rebuilds each proc's rank, hosting daemon, app, app
rank and local rank from those — see
[`src/runtime/data_type_support/AGENTS.md`](../../runtime/data_type_support/AGENTS.md).
What is left per proc is only what the maps cannot say (node rank, cpuset,
state, attributes), which is about 13 bytes a proc against the ~46 this
message cost per proc before any of this. `plm_base_verbose 2` prints the
size; `--rtos donotlaunch` will size a job of any shape without launching
it.

It then calls `PMIx_server_setup_application()` **asynchronously and does
not wait**. It returns `PRTE_SUCCESS` immediately, having handed a
`prte_odls_jcaddy_t` to PMIx; the completion callback `setup_cbfunc()`
runs on the *PMIx* thread, so it only serializes the returned info into a
byte object and thread-shifts. `_setup_complete()`, back on
`prte_event_base`, packs that byte object onto `jdata->launch_msg` and
activates `PRTE_JOB_STATE_SEND_LAUNCH_MSG` — which is what actually
continues the launch. **Nothing blocks**: a `PRTE_SUCCESS` return here
means "the callback will fire", and an error return means it never will
(the caddy is released on the spot).

### 3. Parsing the message and wiring up — `prte_odls_base_default_construct_child_list()`

Runs **on every daemon** (including the HNP). This is the mirror image of
`get_add_procs_data`, and the single most important function to understand:

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
- Registers the nspace with the PMIx server
  (`prte_pmix_server_register_nspace`) and **returns**. The registration
  completes asynchronously; a `prte_odls_jcaddy_t` carries the launch across
  it, and `job_reg_join()` is the join point. It runs `PMIx_server_setup_local_support` if setup info was
  present, starts the spawn threads, and fires
  `PRTE_ACTIVATE_LOCAL_LAUNCH` once everything has reported. **Nothing
  blocks here either.**
  - The join is guarded by a **sentinel**: `cd->pending` starts at 1 and is
    only decremented by the `job_reg_join()` call at the bottom of the
    function, so the count cannot reach zero — and release the caddy —
    part-way through issuing the registrations. Keep that shape if you add
    another registration.
  - The registration callback (`_job_reg_complete`) is invoked *on the PRRTE
    progress thread*:
    `prte_pmix_server_register_nspace` thread-shifts before calling back.
    That is what makes the unlocked `pending` bookkeeping safe.
- On any failure it activates `PRTE_JOB_STATE_NEVER_LAUNCHED` so the HNP
  doesn't hang waiting for a daemon that silently died.
- **`jdata` at the `REPORT_ERROR` label must be the job we were told to
  launch, or NULL.** The prior-jobs loop that used to sit at the top of this
  function decoded *other* jobs, and reused `jdata` to do it — so a failure
  later in the loop drove the wrong (possibly already-released) object to
  `NEVER_LAUNCHED` and the job actually being launched was never reported at
  all. That loop has moved to the nidmap, where it decodes into its own
  variable for the same reason; keep any new decoding here to its own.

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
  places prepositioned files **in that working directory** (`prte_filem`,
  which reads the `app->cwd` `setup_path` just resolved — so the order of
  those two calls is load-bearing), checks the executable, and applies
  resource limits.

  **`process_envars` owns the envar directives — all of them.** It runs
  for every personality whatever that personality's `setup_fork` does,
  which is why the schizo hook deliberately does not repeat them: applying
  `PREPEND`/`APPEND` twice is not a no-op (it duplicates every entry a
  user prepends onto `PATH`), and `prte_schizo_base_setup_fork` used to do
  exactly that. The schizo hook is left with the PMIx prefix only.

  **Order is the contract.** The list is walked front to back, and that
  order is the order the user's directives take effect in: `SET` replaces
  a value outright while `PREPEND`/`APPEND` edit the one already there, so
  `--prepend-env FOO[:] x --set-env FOO=1` leaves `FOO=1` and the reverse
  order leaves `FOO=x:1`. Neither is a merge policy we get to choose — the
  user said what to do and in what sequence. What builds the list in that
  sequence is `prte_append_attribute()` in `pmix_server_dyn.c`; the
  `prte_prepend_attribute()` calls in `construct_child_list` are the
  deliberate exception, putting the values `PMIx_server_setup_application`
  returned in FRONT so the user's own directives, which arrive on the job
  already, are applied afterwards and win. That block is walked in reverse
  for the same reason: prepending a block one entry at a time reverses it.

  Two shapes to keep in mind when editing it:
  - **`UNSET` is a `PMIX_STRING`, not a `pmix_envar_t`** — it names a
    variable and carries no value. It has to be handled *before* the
    `PMIX_ENVAR != attr->data.type` filter that guards the rest of the
    loop, or `--unset-env` silently does nothing. A trailing `*` makes the
    name a prefix.
  - **Match an environment entry's name up to and including the `=`.** A
    bare `strncmp` of the name's length matches any variable that merely
    starts with it, so prepending onto `PATH` would edit `PATHEXT`
    instead — whichever the environment happens to list first. That is
    what `envar_value()` is for.
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
- **No base function blocks.** `get_add_procs_data` and
  `construct_child_list` both hand their work to PMIx and return; the
  launch is carried across the gap by a `prte_odls_jcaddy_t` and resumed
  by a thread-shifted completion handler. Neither uses a
  `prte_pmix_lock_t`, and neither should grow one — they run on
  `prte_event_base`, which is the only thread that can run the handler
  they would be waiting for (see the top-level `AGENTS.md`, "thread-shift
  every PMIx callback").

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
- **Raw back-pointers on unpacked objects are NULL on the daemon.** Fields
  like `prte_app_context_t.job` are only set on the HNP (in `ess/hnp` and
  the dynamic-spawn path); they are not serialized into the launch message,
  so on any daemon that rebuilt the job via `construct_child_list` they are
  NULL. Never dereference `app->job` (or similar back-pointers) in a
  daemon-side path — `setup_path` did, and `--preload-binary` (which sets
  `PRTE_APP_SSNDIR_CWD`) segfaulted on it. Get the job from the caller,
  which already holds `jobdat`, or via `prte_get_job_data_object(nspace)`.
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
- **Use `prte_show_help()`, not `pmix_show_help()`.** A `prted` cannot
  deliver its own help text — PMIx's `plog/stdfd` only writes `stderr` for
  a client or tool, and for a *server* it hands the message to an IOF sink
  that does not exist — so every `show_help` a daemon rendered used to be
  dropped on every node but the head one. `prte_show_help()`
  ([`src/util/prte_show_help.h`](../../util/prte_show_help.h)) is a
  drop-in that relays to the HNP. Every call site in this framework is
  converted; keep new ones that way.
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
  `launch_local` establishes `basedir` with `getcwd` at entry; if that fails
  it must **not** fall through to the shared `chdir(basedir)` cleanup (the
  buffer is uninitialized) — bail directly.
- **Every fatal launch error aborts the whole job.** All fatal paths in
  `launch_local` — `setup_path`, `setup_fork`, `filem`, `check_context_app`,
  `init_sys_limits`, the `chdir(basedir)`-back failure, the sys-limit
  giveups, **and** the per-child IOF-setup failures — use the same idiom:
  flag the directly-affected procs with `PRTE_ODLS_SET_ERROR(job, rc, j)`
  (or activate the single failing child) **and then**
  `PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH)` before
  `goto GETOUT`. Failing only *this app's* procs is **not** enough: `goto
  GETOUT` skips every later app, whose procs stay in `INIT` forever, and on
  a real daemon the errmgr only reports `FAILED_TO_LAUNCH` once
  `num_terminated == num_local_procs` — so the job hangs. The job-state
  activation is what drives the prompt, uniform teardown. Keep new fatal
  paths on this idiom.
- **The `child` loop variable is only valid inside the per-child loop.** In
  `launch_local`, `child` is `NULL` at function entry and, in the per-*app*
  loop (before the inner per-child loop runs), is either `NULL` (first app)
  or a **stale** proc left over from the previous app. Error paths in the
  per-app section (e.g. the `chdir(basedir)`-back failure) must **not**
  dereference `child` — use the job-abort idiom above.
- **Every exit from `spawn_proc` must `PMIX_RELEASE(cd)`.** The spawn caddy
  is `PMIX_NEW`'d in `launch_local` and owned by `spawn_proc`; the success
  tail, the `errorout:` path, **and** the early `PRTE_JOB_DO_NOT_SPAWN`
  return all have to release it, or every donotlaunch/mapping-only proc
  leaks a caddy (plus its `wdir` string).
- **`setup_path` writes through to `app->cwd`.** `launch_local` calls it as
  `setup_path(jobdat, app, &app->cwd)`, so it overwrites (and must first
  free) the app's existing `cwd`; `restart_proc` instead passes a local
  temp. Don't assume `*wdir` starts NULL.
- **One cleanup label, not scattered `return`s.** `get_add_procs_data` and
  `construct_child_list` build `pmix_info_t` arrays and `PMIX_INFO_LIST`s
  that every exit has to release. Prefer a single `goto REPORT_ERROR` over
  a bare `return` in the middle — a stray `return` there both leaks and, on
  the daemon side, skips the `NEVER_LAUNCHED` activation that keeps the HNP
  from hanging.
- **A launch that never forked owes a `prte_wait_cb_cancel`.**
  `launch_local` flags a child `ALIVE` and registers its waitpid *before*
  the IOF setup and the dispatch to `spawn_proc`, so anything that fails in
  between leaves a wait tracker — holding a reference on the child —
  waiting for a pid that will never exist. `spawn_proc`'s `errorout:` keys
  the cancel off `0 == child->pid`, which is exactly the pre-fork case; a
  *post*-fork failure keeps its registration, because that child does exist
  and its `SIGCHLD` is still coming.
- **Never hand a pid of 0 to the kill/signal primitives.** Both component
  primitives turn a pid into a process *group* (`-pid`) so a signal reaches
  whatever the app itself spawned — which makes `pid == 0` catastrophic
  rather than merely useless: `kill(0)` and `kill(-0)` are both "every
  process in the **caller's** group", i.e. the daemon signals *itself*, its
  other children, and under `prterun` the launching tool. A child sits at
  pid 0 for real windows — before its fork, after a failed launch, and once
  `kill_local_procs` has cleared it — and `prted_comm.c` asks the odls for
  **every local child of the named job, by name**, so those procs are
  handed to us as a matter of course. Both branches of
  `signal_local_procs` gate on `0 >= child->pid && ALIVE`, and both
  primitives refuse a non-positive pid on their own. Keep both layers: the
  cost of missing it once is a node's daemon killing itself.
- **A failing proc's `exit_code` is deliberately a *union* of the PMIx and
  PRRTE numbering schemes — do not "fix" it by converting.** Everything
  that reaches `PRTE_ODLS_SET_ERROR` (or sets `child->exit_code` directly)
  ends up switched on by `prte_render_launch_failure()` in
  [`src/runtime/prte_quit.c`](../../../src/runtime/prte_quit.c), and that
  switch has arms for **both**: `PMIX_ERR_JOB_EXE_NOT_FOUND`,
  `PMIX_ERR_EXE_NOT_ACCESSIBLE`, `PMIX_ERR_JOB_WDIR_NOT_FOUND`,
  `PMIX_ERR_SYS_LIMITS_*` sit next to `PRTE_ERR_FAILED_GET_TERM_ATTRS` and
  friends. So the statuses `pmix_util_check_context_app()` and
  `pmix_util_check_context_cwd()` return must be stored **as they are**.
  Running them through `prte_pmix_convert_status()` is exactly the sort of
  thing the boundary rule in the top-level `AGENTS.md` asks for, and here
  it is wrong: it collapses them onto a generic `PRTE_ERROR` and the user
  gets `Error code: -3001 / Error name: Error` instead of being told which
  executable or directory was the problem. (Tried; the swarm's
  `test_odls` caught it.)
- **`ADD` is not `SET`.** `PRTE_JOB_ADD_ENVAR` / `PRTE_APP_ADD_ENVAR` mean
  "set this envar **unless it already has a value**" — that is what
  `src/util/attr.h` says and what the `PMIX_ADD_ENVAR` they are translated
  from says. `prte_odls_base_process_envars` passes `overwrite = false` for
  them and `true` for `SET`; the two lines look identical otherwise, which
  is how they came to be the same.
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

## Testing

The fork/exec/waitpid/kill lifecycle runs only against real child
processes inside a live DVM, so most of odls is covered by the
integration harness — e.g. `prterun -n 4 hostname` (basic launch),
MPMD (`prterun -n 2 echo a : -n 2 hostname`, which walks the per-app
`setup_path` loop), and a non-zero exit (`prterun -n 2 sh -c 'exit 3'`,
which exercises `wait_local_proc`'s status decode).

### Multi-node — `test_odls` in the swarm harness

[`contrib/dockerswarm/run-tests.sh`](../../../contrib/dockerswarm/)
carries a `test_odls` for the half that only exists once a **remote**
daemon has to decode the launch message and fork against its own copy:

- the envar directives (SET/ADD/UNSET/PREPEND/APPEND) end to end, via the
  `envspawn` client — they have no command-line surface at all, so a
  `PMIx_Spawn` is the only way to reach them, and `envspawn` pins its
  child to a node its parent is not on so the fork happens elsewhere;
- the **exec agent**, which each daemon reads from its own MCA state;
- a bad `execve` on a remote node — the child cannot render its own
  message, so this is the proof that the *parent daemon's*
  `render_child_msg` reaches the tool (including the `stat`-based
  "it is a directory" case);
- an **MPMD launch whose first app is bad**, which is the regression
  guard for the job-abort idiom below: the observable is that it
  terminates rather than hanging;
- the waitpid decode arriving from a remote node (`exit 3` stays 3, a
  SIGTERM death becomes 143);
- the SIGCONT→SIGTERM→SIGKILL escalation, against a child that traps
  SIGTERM.

### Without a DVM

What runs without one is the **structural** contract plus the pieces that
are pure functions, in
[`test/unit/odls/test_odls.c`](../../../test/unit/odls/) (wired into
`make check`): the pdefault module vtable is fully wired and reuses the
base `get_add_procs_data`; the component names itself `"pdefault"`; the
`PRTE_DAEMON_*` command bytes are pairwise unique; the
`prte_odls_child_err_t` fatal/warn split holds (NONE == 0, every warn
code sorts after every fatal code); and the two caddy classes
(`prte_odls_spawn_caddy_t`, `prte_odls_launch_local_t`) construct with
the documented NULL/zero defaults and destruct cleanly (both the
all-NULL and the fully-populated paths).

Three cases go further than structure, because the code under them is a
pure function of its inputs:

- **`prte_odls_base_process_envars`** — exported (rather than file-static)
  precisely so this test can drive it: SET vs ADD, the front-to-back
  ordering, `UNSET` with and without a trailing `*`, the "match up to and
  including the `=`" rule (a directive on `PATH` must not edit `PATHEXT`),
  and app-trumps-job.
- **The child→parent pipe record** — `child_warn` across a real pipe, and
  `child_fail` across a real `fork`, checking both the decoded record and
  that the child died with the exit status it was handed.
- **The spawn-thread pool sizing** — that the "already built?" guard and
  the `-1` sentinel survive a `harvest_threads`. The persistent-DVM branch
  is *not* exercised: it spins `max_threads` real progress threads, which a
  bare test process cannot start and stop, so the test clears
  `prte_persistent` first.

Add structural regression guards here; anything that needs a running
launch belongs in the swarm harness or the integration harness.

---

## Where to go next

The launch primitive itself lives in the component:

- [`pdefault/AGENTS.md`](pdefault/AGENTS.md) — the default (and only)
  local-launch component: the real `fork()`/`execve()`, the parent/child
  pipe protocol, and how it plugs `fork_local_proc` into the base.
