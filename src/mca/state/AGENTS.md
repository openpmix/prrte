# AGENTS.md — The `state` Framework (State Machine)

Orientation for AI agents and human contributors working in
`src/mca/state/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`state` is the **engine that drives the entire DVM and every job/proc
lifecycle**. Almost nothing in PRRTE happens by a direct function call
from one subsystem to the next; instead, subsystems *activate a state*,
and the state framework fires the callback registered for that state on
the progress thread. Launch, mapping, daemon startup, I/O-forwarding
teardown, and job termination are all sequenced this way. If you are
tracing "what happens after X?", the answer is almost always "X
activates a state, and the state machine calls the handler wired to it."

Two parallel state machines exist, each a `pmix_list_t` of
`prte_state_t` (state→callback) entries:

| Machine | Global list | Keyed on | Activated with |
|---------|-------------|----------|----------------|
| **job** | `prte_job_states` | `prte_job_state_t` + a `prte_job_t *` | `PRTE_ACTIVATE_JOB_STATE(jdata, state)` |
| **proc** | `prte_proc_states` | `prte_proc_state_t` + a `pmix_proc_t` name | `PRTE_ACTIVATE_PROC_STATE(proc, state)` |

Both global lists are declared in
[`src/runtime/prte_globals.h`](../../runtime/prte_globals.h) and defined
(statically initialized) in `src/runtime/prte_globals.c`. The selected
component **owns** them: its `init()` constructs and populates them, its
`finalize()` tears them down.

The canonical job-launch sequence the DVM machine drives (states defined
in [`src/mca/plm/plm_types.h`](../../plm/plm_types.h)):

```
INIT → INIT_COMPLETE → ALLOCATE → ALLOCATION_COMPLETE
     → LAUNCH_DAEMONS → DAEMONS_LAUNCHED → DAEMONS_REPORTED → VM_READY
     → MAP → MAP_COMPLETE → SYSTEM_PREP → LAUNCH_APPS → SEND_LAUNCH_MSG
     → STARTED → LOCAL_LAUNCH_COMPLETE → RUNNING → REGISTERED
     → ... → TERMINATED → NOTIFY_COMPLETED → NOTIFIED → ALL_JOBS_COMPLETE
```

Other frameworks hook in by registering their handler on a specific
state — this is the whole point of the design:

| State | Handler owner | What runs |
|-------|---------------|-----------|
| `PRTE_JOB_STATE_ALLOCATE` | `ras` | `prte_ras_base_allocate` — discover nodes/slots |
| `PRTE_JOB_STATE_LAUNCH_DAEMONS` | `plm` | `launch_daemons` — **added by the plm base**, not by `state` (see below) |
| `PRTE_JOB_STATE_MAP` | `rmaps` | `prte_rmaps_base_map_job` — assign procs to nodes |
| `PRTE_JOB_STATE_LAUNCH_APPS` | `plm` | `prte_plm_base_launch_apps` — tell daemons to fork/exec |
| `PRTE_JOB_STATE_TERMINATED` | `state` (component) | `check_complete` — teardown |

The `LAUNCH_DAEMONS` entry is deliberately **absent** from the DVM
component's own table: the comment in `state_dvm.c` reads *"individual
plm's must add a state for launching daemons"*, and indeed
`prte_plm_base_frame.c` calls
`prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons)`.
This is the customization model in action — a component in another
framework editing the state machine after the base builds it.

---

## Directory layout

```
state/
  state.h                    # module/component vtable; the ACTIVATE_*/REACHING_* macros
  state_types.h              # prte_state_t (state→cbfunc list item) and prte_state_caddy_t
  base/
    base.h                   # prte_state_base_t globals + all base API prototypes
    state_base_frame.c       # framework open/close/register; MCA params; class instances
    state_base_select.c      # PICK-ONE component selection (unlike rmaps)
    state_base_fns.c         # THE machinery: activate/add/set/remove + common handlers
    state_base_log.c         # the DVM state log (the state_base_log_* params)
    state_base_options.c     # prte_state_base_set_runtime_options (runtime-options parser)
    help-state-base.txt      # user-facing help text
  dvm/                       # HNP/DVM-master machine (pri 100 when PRTE_PROC_IS_MASTER)
  prted/                     # per-daemon machine (pri 100 when PRTE_PROC_IS_DAEMON)
```

Read `state_types.h` and `state.h` first (they are short), then
`state_base_fns.c` — the activation machinery and common handlers there
are where control actually lives. The **state code definitions** are
*not* in this tree: job states, proc states, app states, and node states
all live in [`src/mca/plm/plm_types.h`](../../plm/plm_types.h). Per the
top-level guide, those hand-assigned numeric offsets from
`PRTE_JOB_STATE_ERROR` / `PRTE_PROC_STATE_ERROR` **must be unique** —
adding a state means picking the next unused offset and grepping to
confirm nothing already claims it.

---

## Core data structures (`state_types.h`)

```c
typedef void (*prte_state_cbfunc_t)(int fd, short args, void *cb);

typedef struct {
    pmix_list_item_t super;
    prte_job_state_t  job_state;
    prte_proc_state_t proc_state;
    prte_state_cbfunc_t cbfunc;
} prte_state_t;                      /* one state→callback entry */

typedef struct {
    pmix_object_t super;
    prte_event_t  ev;                /* required by libevent to queue the caddy */
    prte_job_t   *jdata;
    prte_job_state_t job_state;
    pmix_proc_t   name;
    prte_proc_state_t proc_state;
} prte_state_caddy_t;                /* carried to the handler on the progress thread */
```

`prte_state_t` is a list item: the same struct serves both machines, one
of `job_state`/`proc_state` being meaningful depending on which list it
sits in. `prte_state_caddy_t` is the framework's **caddy** (see the
top-level thread-safety section): every state handler receives one as its
`void *cbdata`. Its class destructor (`state_base_frame.c`) calls
`prte_event_del(&caddy->ev)` and releases `caddy->jdata`, so a handler's
final `PMIX_RELEASE(caddy)` also drops the job reference taken at
activation time. Note the caddy carries the `pmix_proc_t` **by value**
(`name`), not by pointer — the proc machine is usable from contexts where
no `prte_proc_t` object exists.

> **`PMIX_NEW` does not zero its allocation.** `pmix_obj_new_tma()` is a
> plain `malloc`, so every caddy field a handler might read has to be set
> by `prte_state_caddy_construct()` — and it is. This is not cosmetic:
> `caddy->job_state` is read by handlers reached through the **ERROR/ANY
> fallback**, which is precisely where the activation may have carried no
> job at all. The errmgr's `job_errors` does `jdata->state =
> caddy->job_state` against the *daemon* job in exactly that case. The
> dispatcher therefore records `caddy->job_state = state`
> **unconditionally**, before it even looks at `jdata`. If you add a field
> to the caddy, initialize it in the constructor.

There is a unit test for all of this — [`test/unit/state/`](../../../test/unit/state/),
`make check`.

### Caddy field ownership

| Field | Set by | Meaning when the activation carried no job/proc |
|-------|--------|--------------------------------------------------|
| `jdata` | `activate_job_state` (with `PMIX_RETAIN`) | `NULL` — a job-machine handler **must** NULL-check it |
| `job_state` | `activate_job_state`, always | the state that fired (never the fallback that matched) |
| `name` | `activate_proc_state` (by value) | rank `PMIX_RANK_INVALID` |
| `proc_state` | `activate_proc_state` | `PRTE_PROC_STATE_UNDEF` |

---

## The module contract (`state.h`)

Every component fills in `prte_state_base_module_t`
(`prte_state_base_module_1_0_0_t`) — an unusually large vtable of ten
function pointers, but in practice **every component points all ten at
the base implementations**; components differ only in the *tables they
build* in `init()`, not in the API behavior:

| Field | Signature | Meaning |
|-------|-----------|---------|
| `init` | `int (*)(void)` | Construct `prte_job_states`/`prte_proc_states` and populate them. Component-specific. |
| `finalize` | `int (*)(void)` | Destruct the two lists. |
| `activate_job_state` | `void (*)(prte_job_t *, prte_job_state_t)` | Post an event to run the state's callback. |
| `add_job_state` | `int (*)(prte_job_state_t, prte_state_cbfunc_t)` | Append a state→cbfunc entry; refuses duplicates. |
| `set_job_state_callback` | `int (*)(prte_job_state_t, prte_state_cbfunc_t)` | Replace a state's callback (or append if absent). |
| `remove_job_state` | `int (*)(prte_job_state_t)` | Drop a state entry. |
| `activate_proc_state` | `void (*)(pmix_proc_t *, prte_proc_state_t)` | Proc-machine counterpart of activate. |
| `add_proc_state` / `set_proc_state_callback` / `remove_proc_state` | — | Proc-machine counterparts. |

The winning module is copied into the global `prte_state` instance;
callers reach the vtable through it (`prte_state.activate_job_state(...)`).
You should almost never call the vtable directly — use the macros.

The version macro is `PRTE_MCA_BASE_VERSION(state)`.

### Return protocol

`add_job_state` / `add_proc_state` return `PRTE_ERR_BAD_PARAM` if the
state is already registered (uniqueness is enforced), `PRTE_SUCCESS`
otherwise. `set_*` returns `PRTE_SUCCESS` always (it appends when the
state is absent — except the proc variant, which returns
`PRTE_ERR_NOT_FOUND`). `remove_*` returns `PRTE_ERR_NOT_FOUND` if the
state was not registered. `activate_*` returns `void`: an unregistered
state that matches no ERROR/ANY fallback is **silently dropped** (see
below), so a typo'd or unregistered state fails quietly, not loudly.
`activate_proc_state` also drops a `NULL` proc name — the proc machine is
keyed entirely on the name, so there is nothing to dispatch on.

The asymmetry in `set_*` is deliberate and load-bearing, not an
oversight: the **job** variant appends when the state is absent (so it
doubles as "add if missing"), the **proc** variant refuses with
`PRTE_ERR_NOT_FOUND`. Both are pinned by the unit test.

---

## The activation machinery (`state_base_fns.c`)

`prte_state_base_activate_job_state()` is the heart of the framework.
Walking `prte_job_states`, it:

1. Records the positions of any `PRTE_JOB_STATE_ANY` and
   `PRTE_JOB_STATE_ERROR` entries as it scans (fallbacks).
2. On finding the requested `state`, fires `PRTE_REACHING_JOB_STATE`
   (a verbose trace), allocates a `prte_state_caddy_t` with
   `PMIX_NEW`, stashes `jdata` + `state` in it, **`PMIX_RETAIN(jdata)`**
   so the job survives the async hop, and calls
   `PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, s->cbfunc)` to queue
   the handler on the progress thread. Then returns.
3. If the state is **not** found: if `state > PRTE_JOB_STATE_ERROR` and
   an ERROR entry exists, use that; else if an ANY entry exists, use
   that; else the activation is ignored (verbose message only).
4. A matched entry with a `NULL` cbfunc is also ignored (verbose only).

`prte_state_base_activate_proc_state()` is the exact mirror, keyed on
`PRTE_PROC_STATE_ANY` / `PRTE_PROC_STATE_ERROR`, copying the proc name
into the caddy by value.

This is why the `PRTE_ACTIVATE_JOB_STATE` / `PRTE_ACTIVATE_PROC_STATE`
macros (in `state.h`) are the recommended entry points: besides emitting
a timestamped verbose trace (`ACTIVATE JOB <ns> STATE <s> AT
file:line`), they simply call the vtable's `activate_*`. **The caddy is
heap-allocated and thread-shifted, never touched on the caller's stack**
— exactly the caddy/threadshift pattern the top-level thread-safety
section describes. Every handler runs on the progress thread and is
responsible for the final `PMIX_RELEASE(caddy)`.

### `add` / `set` / `remove`

- `prte_state_base_add_job_state()` — scans for a duplicate (returns
  `PRTE_ERR_BAD_PARAM` if found), then `PMIX_NEW(prte_state_t)`, sets
  the state + cbfunc, and appends.
- `prte_state_base_set_job_state_callback()` — finds the entry and
  overwrites its cbfunc; if absent, appends a new one (so it doubles as
  "add if missing").
- `prte_state_base_remove_job_state()` — finds, `pmix_list_remove_item`,
  `PMIX_RELEASE`.
- `prte_state_base_print_job_state_machine()` — dumps every
  state→cbfunc(DEFINED/NULL) pair; called automatically from a
  component's `init()` when framework verbosity > 5.

Proc-machine variants (`..._proc_state`) are structurally identical.

---

## Common handlers `base/` provides

These live in `state_base_fns.c` and are wired into components' tables
(especially the DVM and prted proc machines). They are the reusable
"what actually happens at this state" bodies:

| Handler | Role |
|---------|------|
| `prte_state_base_track_procs` | The proc-state workhorse. Advances `pdata->state`, counts `num_launched`/`num_reported`/`num_terminated`, and rolls those counts up into job-state activations: first `RUNNING` → `STARTED`, all running → `RUNNING`; all registered → `REGISTERED`; `IOF_COMPLETE` + `WAITPID_FIRED` together → `TERMINATED`; all terminated → job `TERMINATED` (and, if the daemon job and routes are gone, `DAEMONS_TERMINATED`). Also gates `READY_FOR_DEBUG`. |
| `prte_state_base_local_launch_complete` | Optionally kicks `REPORT_PROGRESS` every 100 daemons when `PRTE_JOB_SHOW_PROGRESS` is set. |
| `prte_state_base_report_progress` | Prints the "App launch reported: N daemons / M procs" line. |
| `prte_state_base_check_fds` | Debug leak check: enumerates open fds after a job completes (enabled by `state_base_check_fds`). |
| `prte_state_base_notify_data_server` | Tells the data server a job has ended, naming the lifetime that ended (`PMIX_PERSIST_APP`) so it drops the nspace's published data that was not to outlive it. Not gated on an external data server: the built-in one needs telling too, and while it was gated nothing was ever reclaimed from it. |
| `prte_state_base_recover_resources` | Idempotently returns one proc's slot/cpu resources to its node and drops the node from the map when empty — used on daemon-loss / partial-failure recovery paths. Written to tolerate being called twice for the same proc. |

### Every handler here is wired — keep it that way

`base/` used to also carry `prte_state_base_check_all_complete` and
`prte_state_base_cleanup_job`. Nothing registered either one: both
components install their own richer `check_complete`/`cleanup_job`, so the
two exported bodies were unreachable — and, worse, a second copy of the
teardown logic that drifted away from the one that actually runs. They have
been removed, along with the `normal-termination-but` help topic that only
`check_all_complete` used.

The lesson generalizes: a handler in `base/` earns its place by being
registered in some component's `init()` table (or added by another
framework, as plm does for `LAUNCH_DAEMONS`). Before extending or "fixing"
one, `grep` for a registration — an unregistered handler is not a fallback,
it is dead weight that will diverge from the live path.

### Never block a state handler on PMIx — there is no other thread

`prte_event_base` has **no progress thread of its own**. `event.c` sets
`prte_event_base = prte_sync_event_base` with the comment *"PRTE tools
block in their own loop over the event base, so no progress thread is
required"* — so the thread running a state handler **is** the only thread
that drives PRRTE. Park it and the whole process goes deaf: no RML, no
IOF, no other job's state transitions, until it is woken.

The teardown handlers used to do exactly that around
`PMIx_server_deregister_nspace`: hand PMIx a callback that woke a
`prte_pmix_lock_t` directly from the PMIx thread, then sit in
`PRTE_PMIX_WAIT_THREAD`. That is unbounded — the deregistration runs each
peer's filesystem epilog — and on a persistent DVM it stalls every other
job in flight. It also only worked *because* the wakeup was direct: the
thread-shifted wakeup the top-level golden rule asks for could never have
run on a parked thread, so "fixing" the callback in isolation would have
deadlocked. The two halves were consistently wrong in a way that
cancelled out.

The correct shape is the caddy/threadshift pattern, and it is what both
live paths now use:

```c
/* on the progress thread: issue and return, handing over the caddy */
PMIx_server_deregister_nspace(nspace, dereg_complete, caddy);
return;

/* on the PMIx thread: nothing but the hop back */
static void dereg_complete(pmix_status_t status, void *cbdata)
{
    PRTE_PMIX_THREADSHIFT((prte_state_caddy_t *) cbdata,
                          prte_event_base, job_teardown);
}
```

`state_dvm.c` splits `check_complete` → `check_complete_resume` this way,
and `state_prted.c` splits `track_procs` → `job_teardown`. The caddy
carries the job across the hop; note that a **proc**-state activation
leaves `caddy->jdata` NULL, so `state_prted.c` takes a reference of its
own before handing it over.

Two deliberate exceptions, both of which pass `NULL` as the callback so
that **PMIx blocks on its own lock** and no PRRTE object is touched from
the PMIx thread:

- `PMIx_server_IOF_deliver` in `check_complete_resume`. Waiting is
  required, not merely tidy: the API borrows the source proc and the byte
  object *by pointer* and both live on the caller's stack. The stall is
  acceptable here — it runs only for a non-persistent (`prterun`) DVM
  already tearing down after an abnormal exit, and the delivery is a
  bounded, purely PMIx-internal write with no host upcall.

**What the split changes semantically.** Other events now run between the
two halves, where before the whole handler was one indivisible dispatch.
Nothing is dispatched *concurrently* — there is still only one thread — so
a second activation of the same state cannot interleave *inside* either
half; it queues behind them. But a continuation must not assume the world
is unchanged since the first half ran. Re-check anything you cached, and
keep the re-entry guards ahead of the hand-off: `state_prted.c` sets
`PRTE_JOB_TERM_NOTIFIED` before it issues the deregistration precisely so
a second `TERMINATED` cannot re-enter the branch while the continuation is
pending. The job object itself is safe — the caddy holds a reference, and
the only other remover of a `prte_job_data` slot is the `prte_job_t`
destructor.

For the record, the deregistration path in PMIx makes **no host-module
upcall** today — `_deregister_nspace`, `remove_client` and `_iofdeliver`
touch only PMIx internals — so the old code did not actually deadlock.
But nothing enforced that, and a single future upcall on that path would
have turned a stall into a hang. Do not reintroduce the pattern.

`prte_state_base_set_runtime_options()` (in `state_base_options.c`)
translates a `PMIX_RUNTIME_OPTIONS` spec (or the framework defaults) into
per-job attributes — `error-nonzero-status`, `recoverable`, `continuous`,
`autorestart`, `stop-on-exec`, `timeout`, `max-restarts`, etc. It is
called from `prte`/`prterun`'s own option handling (`src/prted/prte.c`),
from the PMIx spawn path (`pmix_server_dyn.c`), and with a `NULL` spec
from each schizo component's `set_default_rto`. It lives here because the
defaults it reads are this framework's MCA params.

> **A boolean runtime option is consumed by PRESENCE, not by value.**
> Every call site tests these with
> `prte_get_attribute(&attrs, KEY, NULL, PMIX_BOOL)`, which is true as
> soon as the key is on the list — whatever value it holds. So `opt=false`
> must leave the attribute **absent**. Note that `prte_set_attribute()`
> only removes a false boolean when the key is *already* on the list; when
> it is not, it happily appends it with a false value, and the option then
> reads as **enabled** everywhere. The `set_bool_option()` helper in
> `state_base_options.c` is the correct way to apply one; use it for any
> boolean directive you add. Watch the sense, too: `aggregate-help` drives
> `PRTE_JOB_NOAGG_HELP`, its negation.

> **`report-child-jobs-separately` is a run-level policy, not a job-level
> one.** It decides whose exit status `prterun` returns, and it is
> consulted as *each* job reaches teardown — including a child the primary
> job spawned, which never carries the directive itself. So the parser
> records it on the **daemon job** as well (the same thing `donotlaunch`
> and `donotspawn` do), and `state_dvm.c`'s `report_child_jobs_separately()`
> reads it from there or from the `prte_report_child_jobs_separately` MCA
> param. The `!prte_persistent` gate on that copy matches where it is
> read: only a one-shot DVM reports an exit status, so a job spawned into
> a persistent DVM cannot change the policy for everyone else.
>
> On a **persistent** DVM the same policy therefore has to be applied by
> the *tool*, which is the process that has an exit status to report, and
> `prun` does it with `prte_state_base_report_child_sep()` — the reader
> that lives beside the walk in `state_base_options.c` so the directive has
> one spelling and one set of truth rules. The DVM sends `prun` the facts
> it needs and no policy: `dvm_notify()` stamps every `PMIX_EVENT_JOB_END`
> with `PMIX_SPAWN_TREE_ROOT` (`prte_job_t::launcher`, or the job itself
> when nothing spawned it) and `PMIX_SPAWN_TREE_ACTIVE` (how many jobs in
> that tree have yet to terminate). See `src/prted/AGENTS.md`.

> **Do not use the directive loop's index as a scratch variable.** The
> walk is `for (n = 0; NULL != options[n]; n++)`. Two branches used to
> assign to `n` — the converted seconds in the `timeout` branch and the
> app-array index in `max-restarts` — which resumed the walk at an
> arbitrary offset, dropping every later directive and reading past the
> end of the option array. Declare your own index.

---

## Framework globals & MCA params (`state_base_frame.c`)

`prte_state_base` (type `prte_state_base_t`, declared in `base.h`) holds
framework-wide toggles, several registered as MCA params:

| Field | MCA param | Meaning |
|-------|-----------|---------|
| `run_fdcheck` | `state_base_check_fds` | Check for fd leaks after each job. |
| `recoverable` | `state_base_recoverable` | Default `recoverable` runtime option. |
| `max_restarts` | `state_base_max_restarts` | Default per-proc restart limit. |
| `continuous` | `state_base_continuous` | Default `continuous` runtime option. |
| `error_non_zero_exit` | `state_base_error_non_zero_exit` | Treat non-zero exit as an error (default true). |
| `show_launch_progress` | `state_base_show_launch_progress` | Emit DVM-startup progress reports. |
| `notifyerrors` | `state_base_notify_errors` | Raise a PMIx event on reportable proc errors. |
| `autorestart` | `state_base_autorestart` | Auto-restart failed procs up to the limit. |
| `log_jobstate` | `state_base_log_jobstate` | Record every job-state transition this process orders. |
| `log_procstate` | `state_base_log_procstate` | Record every proc-state transition this process orders. |
| `log_path` | `state_base_log_path` | Directory the state log is written into. |
| `log_file` / `log_fp` | (not params) | The resolved file name and its handle; owned by `state_base_log.c`. |
| `parent_fd` / `ready_msg` | (not params) | Startup handshake back to a parent launcher; "DVM ready" gating. |

`state_base_frame.c` also declares the framework
(`PMIX_MCA_BASE_FRAMEWORK_DECLARE`) and the `PMIX_CLASS_INSTANCE`s for
`prte_state_t` and `prte_state_caddy_t`.

---

## The DVM state log (`state_base_log.c`)

The three `log_*` parameters above are the whole interface to this facility,
and deliberately so. An early draft of the bootstrap configuration file
carried `ControllerLogJobState` / `ControllerLogProcState` /
`ControllerLogPath` and their `PRTEDLog*` twins; they were **removed from the
format** (`docs/configuration.rst`, `docs/plans/bootstrap/`). A key in
`prte.conf` is written once and then applies to every daemon of every DVM the
cluster starts, with nobody watching, while the volume of a per-transition
record scales with the processes launched and fills the same disk the session
directories live on. So: an MCA parameter, asked for by the run that wants
it. Off by default, and the framework never opens a file unless one of the
two booleans is set.

`prte_state_base_log_open()` runs from the framework's `open`, which is early
in `ess` init and late enough that the three things naming the file — the
hostname, the session-directory base, and the role — are all settled. The
file is `<dir>/prtectrlr-<host>-log` for the master and
`<dir>/prted-<host>-log` for a daemon, so a controller co-resident with a
`prted` does not contend with it. Records are emitted from the top of
`prte_state_base_activate_job_state` / `..._proc_state`, **ahead of the
dispatch walk** — the documented content is the moment a transition was
*ordered*, and a state no handler is registered for is exactly the thing
someone reads this log to discover, so it has to appear even though the
dispatcher drops it.

Three things about the implementation are deliberate and easy to undo by
accident:

- **It does not use `pmix_output`'s file support.** A `pmix_output` stream
  resolves its filename **lazily, at the first write**, from the *global*
  `output_dir`/`output_prefix` that `pmix_output_set_output_file_info()` last
  set — and `ess/hnp` and `ess_base_std_prted.c` both set those to the proc
  session directory during init. The save/set/open/restore idiom the
  `pmix_output.h` header describes therefore does **not** put a stream in a
  chosen directory: the stream lands wherever the globals point when the
  first record fires. A plain `FILE*` is both simpler and correct here.
- **The file is opened for append, line-buffered, and `FD_CLOEXEC`.** Append
  because the name carries only the host and the role, so a restart would
  otherwise erase the previous run (a `#` banner separates them);
  line-buffered so a record is on disk before the transition it names has
  been acted on, which is the point of keeping the log; close-on-exec because
  the `odls` forks application processes.
- **A failed open disables the flags.** Leaving them set would cost a branch
  on every transition for a file nobody is writing, and the `show_help` has
  already been shown once.

Records from two threads can appear out of timestamp order by microseconds —
`odls` activates `RUNNING` from a worker thread while the terminate join runs
on the progress thread (see the out-of-order note under "Conventions"). The
lines themselves never interleave: stdio serializes the write. The timestamp
is taken before that lock, and deliberately so — it records when each
transition was ordered, which is the truthful thing when two really were
ordered concurrently.

---

## Component selection — this *is* "pick one"

Unlike `rmaps` (which keeps every component), `state` uses the ordinary
MCA "select the single best module" flow.
`prte_state_base_select()` (in `state_base_select.c`) calls
`pmix_mca_base_select`, copies the winning module into the global
`prte_state`, and calls its `init()`. Selection is **role-driven** via
each component's `query`:

| Component | Wins when | Priority |
|-----------|-----------|----------|
| `dvm` | `PRTE_PROC_IS_MASTER` (the HNP) | 100 |
| `prted` | `PRTE_PROC_IS_DAEMON` | 100 |

Exactly one applies to any given process, so there is no real
contention — the process's role decides which machine it runs.

---

## Conventions & gotchas

- **Adding a new state is a two-file job.** Define the numeric code in
  [`src/mca/plm/plm_types.h`](../../plm/plm_types.h) (next unused offset
  from the right base; grep to confirm uniqueness — a duplicate silently
  makes two states compare equal), **and** register a handler for it in
  the appropriate component's `init()` table (or via
  `prte_state.add_job_state` from the owning framework, as plm does for
  `LAUNCH_DAEMONS`). A code with no registered handler is silently
  dropped unless an ERROR/ANY fallback catches it.
- **Handlers must release the caddy.** Every state callback ends with
  `PMIX_RELEASE(caddy)` (which drops the retained `jdata`). Forgetting it
  leaks the job object — **including on every early-return error path**.
  This is the single most common defect in this tree: a pack failure or a
  missing attribute bails out with a bare `return` and strands both the
  caddy and the job reference it holds.
- **A job-state handler must NULL-check `caddy->jdata`.** Roughly eighty
  call sites activate with no job at all
  (`PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED)` and
  friends, mostly from `src/rml/` and the fault paths). Those land on
  whatever ERROR/ANY catch-all is registered, so *any* handler you put on
  a fallback entry will see a `NULL` job sooner or later. `caddy->job_state`
  is always valid; `caddy->jdata` is not.
- **A proc's state can arrive out of order, and it must never go
  backwards.** `prte_odls_base_spawn_proc()` activates `RUNNING` at the tail
  of the launch, from a **worker thread**, while the WAITPID/IOF join that
  records the termination runs on the progress thread. A proc short-lived
  enough to die and be reaped before the launch path finishes therefore
  produces `WAITPID FIRED` → `IOF COMPLETE` → `NORMALLY TERMINATED` →
  **`RUNNING`**, microseconds apart and in that order. `state/prted`'s
  `track_procs` must not let that last one overwrite the terminated state,
  and its launch-complete report must not pack `RUNNING` for a proc already
  flagged `PRTE_PROC_FLAG_RECORDED`: the join has fired, so nothing is left
  to correct the lie, and the DVM master believes that proc is alive for the
  rest of the DVM's life. The job never completes, `terminate_orteds` is
  never called, and `prterun` hangs with every daemon still up. The
  `WAITPID_FIRED` arm guards the opposite order for the same reason ("do NOT
  update the proc state as this can hit while we are still trying to notify
  the HNP of successful launch for short-lived procs"); the two halves belong
  together. Anything slow on the parent's launch path widens the window —
  it was found through hwloc's `memory not bound` warning, which adds a pipe
  record and a `show_help` render to `do_parent` before the `RUNNING`
  activation.
- **Never do real work in the activator.** Activation only queues an
  event; the handler runs later on the progress thread. Don't assume the
  handler has run when `PRTE_ACTIVATE_*_STATE` returns, and don't read
  `jdata->state` right after activating — it changes asynchronously.
- **The two ordering arrays must stay index-aligned.** In each
  component, the `*_states[]` array and its `*_callbacks[]` array are
  walked together by index; inserting into one without the other
  silently misassigns handlers.
- **ERROR vs. ANY fallbacks are ordered.** `state > *_STATE_ERROR` routes
  to the ERROR handler; everything else routes to ANY. Registering an
  ERROR or ANY catch-all changes the behavior of *every* otherwise
  unregistered state.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==` / constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG` on unexpected `rc`.

---

## Debugging

```sh
prte --prtemca state_base_verbose 5 ...    # trace every state transition
```

At verbosity > 0 the `PRTE_ACTIVATE_*` / `PRTE_REACHING_*` macros print
timestamped `ACTIVATE …STATE… AT file:line` lines — the single most
useful tool for understanding "why did the job stop here?". At
verbosity > 5 each component dumps its full state→cbfunc table at
`init()` time (`prte_state_base_print_job_state_machine` /
`..._proc_state_machine`). Pair it with `plm_base_verbose` (daemon
launch) and `rmaps_base_verbose` (mapping) to see the handlers those
states invoke.

---

## Testing

| Layer | Where | Covers |
|-------|-------|--------|
| Unit | [`test/unit/state/test_state.c`](../../../test/unit/state/test_state.c) (`make check`) | the table API and its return protocol; dispatch incl. the ERROR/ANY fallback ordering and the NULL-cbfunc/NULL-proc guards; the caddy initialization contract (the NULL-job case above); `set_runtime_options` directive parsing — boolean sense, directive ordering, unknown/bad-combination refusals; `prte_state_base_report_child_sep()` agreeing with that walk on every spelling of the directive; per-role component selection; the state log — the path rule (absolute/relative/absent/no-base), the file name, and that a transition is recorded even when the dispatcher drops it (the test runs with empty tables, so every activation it makes *is* dropped). |
| Multi-node | `test_state` in [`contrib/dockerswarm/run-tests.sh`](../../../contrib/dockerswarm/run-tests.sh) | **the state log's role split** — the controller and a `prted` writing their own files, which one process wearing both hats cannot show, and which also proves the parameters reach the daemons at all (that is the launch path's job, not this framework's); that no log appears unasked; the runtime-option directives end-to-end through `prterun` (a real forked child is what makes the boolean sense observable, via `odls`); **`report-child-jobs-separately` against a real parent/child job pair** (see below); a `NULL`-job `NEVER_LAUNCHED` reaching the errmgr fallback and tearing the DVM down promptly instead of hanging; and `check_complete`'s resource accounting, which only shows up as successive jobs re-filling the same allocation on one persistent DVM. |

Testing a policy that discriminates between a **primary** job and one it
**spawned** needs an application that calls `PMIx_Spawn`. The tree ships
one: [`examples/dynamic.c`](../../../examples/dynamic.c), which
`contrib/dockerswarm/build.sh` installs as `dynamic`. Rank 0 spawns
`client` **from its own cwd**, so the child's exit status is whatever you
put at that path — the swarm test drops in a script that exits 7, runs the
parent from that directory, and asserts `prterun` returns 7 by default and
0 under the option (with the child's status still reported). The child
script has to exist on every node the child could map onto; the spawn names
an absolute path and `/tmp` is per-container.

Nothing here needs the offline mapper harness — this framework does not
touch mapping.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`dvm/AGENTS.md`](dvm/AGENTS.md) — the HNP's full launch/terminate
  machine; read this second.
- [`prted/AGENTS.md`](prted/AGENTS.md) — the per-daemon slimmer machine.
