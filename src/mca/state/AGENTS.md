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
| `PRTE_JOB_STATE_TERMINATED` | `state` (component) | `check_complete` / `check_all_complete` — teardown |

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

The version macro is `PRTE_STATE_BASE_VERSION_1_0_0`.

### Return protocol

`add_job_state` / `add_proc_state` return `PRTE_ERR_BAD_PARAM` if the
state is already registered (uniqueness is enforced), `PRTE_SUCCESS`
otherwise. `set_*` returns `PRTE_SUCCESS` always (it appends when the
state is absent — except the proc variant, which returns
`PRTE_ERR_NOT_FOUND`). `remove_*` returns `PRTE_ERR_NOT_FOUND` if the
state was not registered. `activate_*` returns `void`: an unregistered
state that matches no ERROR/ANY fallback is **silently dropped** (see
below), so a typo'd or unregistered state fails quietly, not loudly.

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
| `prte_state_base_check_all_complete` | The generic "is every job done?" sweep: marks the job terminated, deregisters the nspace from the PMIx server, releases the job's mapped node/proc resources, and — when no non-daemon job remains alive — calls `prte_plm.terminate_orteds()`. (The DVM component uses its own richer `check_complete` instead; see that component's guide.) |
| `prte_state_base_local_launch_complete` | Optionally kicks `REPORT_PROGRESS` every 100 daemons when `PRTE_JOB_SHOW_PROGRESS` is set. |
| `prte_state_base_report_progress` | Prints the "App launch reported: N daemons / M procs" line. |
| `prte_state_base_cleanup_job` | Marks a job `NOTIFIED` and re-drives it through `TERMINATED`. |
| `prte_state_base_check_fds` | Debug leak check: enumerates open fds after a job completes (enabled by `state_base_check_fds`). |
| `prte_state_base_notify_data_server` | Tells a co-resident data server to purge a terminated nspace's published data. |
| `prte_state_base_recover_resources` | Idempotently returns one proc's slot/cpu resources to its node and drops the node from the map when empty — used on daemon-loss / partial-failure recovery paths. Written to tolerate being called twice for the same proc. |

`prte_state_base_set_runtime_options()` (in `state_base_options.c`)
translates a `PMIX_RUNTIME_OPTIONS` spec (or the framework defaults) into
per-job attributes — `error-nonzero-exit`, `recoverable`, `continuous`,
`autorestart`, `stop-on-exec`, `timeout`, `max-restarts`, etc. It is
called from the PMIx spawn path, not from a state handler, but lives here
because the defaults it reads are this framework's MCA params.

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
| `parent_fd` / `ready_msg` | (not params) | Startup handshake back to a parent launcher; "DVM ready" gating. |

`state_base_frame.c` also declares the framework
(`PMIX_MCA_BASE_FRAMEWORK_DECLARE`) and the `PMIX_CLASS_INSTANCE`s for
`prte_state_t` and `prte_state_caddy_t`.

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
  leaks the job object.
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

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`dvm/AGENTS.md`](dvm/AGENTS.md) — the HNP's full launch/terminate
  machine; read this second.
- [`prted/AGENTS.md`](prted/AGENTS.md) — the per-daemon slimmer machine.
