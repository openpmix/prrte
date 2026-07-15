# AGENTS.md — The `errmgr` Framework (Error and Recovery Manager)

Orientation for AI agents and human contributors working in
`src/mca/errmgr/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`errmgr` (Error and Recovery Manager) is the **central clearing house
for process- and daemon-state changes** in a running DVM. When any part
of PRRTE decides a process or daemon has entered an error state — it
aborted, exited non-zero, called `PMIx_Abort`, failed to launch, or its
communication link dropped — that fact is funneled into this framework,
which then decides **what to do about it**: report it, notify peers,
recover the resources, terminate the offending job, or tear down the
whole DVM.

Unlike most frameworks, `errmgr` does **not** expose a rich vtable that
callers invoke directly. Its module struct carries only `init`,
`finalize`, and a `logfn` (see below). The real work happens through the
**state machine**: at `init()` time the selected component registers
callbacks on specific job and process error states, and the `state`
framework fires those callbacks — as thread-shifted events on the
progress thread — whenever code anywhere calls
`PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_*)` or
`PRTE_ACTIVATE_PROC_STATE(&name, PRTE_PROC_STATE_*)` with an error
state. So the errmgr's behavior *is* its set of state-machine handlers.

Where it runs matters, because the two components implement completely
different policies for the same states:

| Process role | Selected component | Job |
|--------------|--------------------|-----|
| HNP / DVM master (`PRTE_PROC_IS_MASTER`) | `dvm` | Decide DVM-wide policy: notify the job's submitter, terminate a failed job while keeping the DVM alive, or tear the DVM down. |
| prted daemon (`PRTE_PROC_IS_DAEMON`) | `prted` | Report local process/daemon state changes **up to the HNP**, kill local children, and exit cleanly when ordered. |
| tool (`prun`, `pterm`, …) | **none** | Tools never call `prte_errmgr_base_select()`, so they keep the default log-only module. |

Its place in the lifecycle is orthogonal to the launch path: `rmaps`,
`plm`, etc. drive a job *forward* through
`INIT → ALLOCATE → MAP → LAUNCH_DAEMONS → RUNNING → TERMINATED`; the
errmgr is what catches the job (or a daemon) when any of those steps —
or a running proc — goes wrong, and routes it to `TERMINATED` (or, for
recoverable jobs, back to a survivable state).

---

## Directory layout

```
errmgr/
  errmgr.h                    # module + component structs; the (thin) vtable; version macro
  base/
    base.h                    # framework struct + prte_errmgr_base_select() prototype
    errmgr_private.h          # prte_errmgr_default_fns + prte_errmgr_base_log() prototype
    errmgr_base_frame.c       # framework open/close/DECLARE; the global prte_errmgr module
    errmgr_base_select.c      # pmix_mca_base_select — classic single-winner pick-one
    errmgr_base_fns.c         # prte_errmgr_base_log() — the default logfn implementation
    help-errmgr-base.txt      # user-facing error/help text (failed-daemon, node-died, …)
    static-components.h       # generated: lists dvm + prted as the static components
  dvm/                        # HNP component (pri 1000, gated PRTE_PROC_IS_MASTER)
  prted/                      # daemon component (pri 1000, gated PRTE_PROC_IS_DAEMON)
```

Read `errmgr.h` first (it is short — the whole contract is ~40 lines),
then jump straight to the component you care about; the two `_module.c`
files (`dvm/errmgr_dvm.c`, `prted/errmgr_prted.c`) are where all the
policy lives.

---

## The module contract

The module struct (`prte_errmgr_base_module_2_3_0_t`, in `errmgr.h`) is
deliberately tiny:

```c
typedef int  (*prte_errmgr_base_module_init_fn_t)(void);
typedef int  (*prte_errmgr_base_module_finalize_fn_t)(void);
typedef void (*prte_errmgr_base_module_log_fn_t)(int error_code, char *filename, int line);

struct prte_errmgr_base_module_2_3_0_t {
    prte_errmgr_base_module_init_fn_t     init;      /* register state callbacks here */
    prte_errmgr_base_module_finalize_fn_t finalize;
    prte_errmgr_base_module_log_fn_t      logfn;
};
```

| Member | Meaning / protocol |
|--------|--------------------|
| `init` | Return `PRTE_SUCCESS`/`PRTE_ERROR`. **This is where a component wires itself into the state machine** via `prte_state.add_job_state()` / `prte_state.add_proc_state()`. May be `NULL` (the default module leaves it `NULL`). |
| `finalize` | Return `PRTE_SUCCESS`/`PRTE_ERROR`. May be `NULL`; both real components make it a no-op. |
| `logfn` | `void`, no error return. Formats and prints a `PRTE_ERROR_LOG`-style message. **Must always be non-NULL** — see the gotcha below. |

The public global that everyone links against is
`PRTE_EXPORT extern prte_errmgr_base_module_t prte_errmgr;` — after
selection this holds a *copy* of the winning component's module struct.

### The `logfn` gotcha

`logfn` looks like the hook behind `PRTE_ERROR_LOG`, but it is not.
`PRTE_ERROR_LOG(rc)` is a standalone macro in
[`src/util/error.h`](../../../src/util/error.h) that calls `pmix_output`
directly; it never touches `prte_errmgr.logfn`. In the current tree
**nothing outside this framework calls `prte_errmgr.logfn`** — it is
effectively vestigial, retained because the module struct is initialized
with it even before the framework is opened. The header comment in
`errmgr_base_frame.c` is emphatic about that initialization:

```c
/* NOTE: ABSOLUTELY MUST initialize this struct to include the log
 * function as it gets called even if the errmgr hasn't been opened
 * yet due to error */
prte_errmgr_base_module_t prte_errmgr = { .logfn = prte_errmgr_base_log };
```

Do not "clean up" that initialization to `{0}`; a NULL `logfn` used
during a very early failure would crash instead of reporting.

---

## The component struct

`prte_errmgr_base_component_3_0_0_t` (in `errmgr.h`) is a standard MCA
component wrapper plus three ints — `verbose`, `output_handle`,
`priority`. Both real components only ever register `priority` as an MCA
param; `verbose`/`output_handle` are unused in the current components
(framework verbosity comes from `errmgr_base_verbose`, below). The
version macro components must use is:

```c
#define PRTE_ERRMGR_BASE_VERSION_3_0_0 PRTE_MCA_BASE_VERSION_3_0_0("errmgr", 3, 0, 0)
```

---

## What `base/` provides

The base is intentionally thin — it does selection, open/close, and one
log function; it holds **no** error policy of its own (that all lives in
the components).

### `errmgr_base_frame.c` — framework plumbing and the default module

- `PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, errmgr, …, prte_errmgr_base_open,
  prte_errmgr_base_close, prte_errmgr_base_static_components, …)` declares
  `prte_errmgr_base_framework`. Because the register hook is `NULL`, the
  only framework-level MCA param is the auto-provided `errmgr_base_verbose`.
- `prte_errmgr_base_open()` loads `prte_errmgr = prte_errmgr_default_fns`
  (log-only) and opens all components.
- `prte_errmgr_base_close()` calls the selected module's `finalize` (if
  any), then **restores the default log-only fns** so `prte_errmgr.logfn`
  stays valid through shutdown, then closes the components.
- Defines the two module globals:
  - `prte_errmgr_default_fns` — `{ init=NULL, finalize=NULL,
    logfn=prte_errmgr_base_log }`. The fallback used by tools and during
    early errors.
  - `prte_errmgr` — the live module, pre-seeded with the default `logfn`.

### `errmgr_base_select.c` — `prte_errmgr_base_select()`

Classic **single-winner** MCA selection (unlike `rmaps`, which keeps all
modules): it calls `pmix_mca_base_select("errmgr", …)`, which queries
every component's `query` and keeps the one returning the highest
priority. The winner's module struct is **copied** into the global
`prte_errmgr`, and its `init()` is invoked (a non-`PRTE_SUCCESS` return
fails the whole selection). If no component is selectable it returns
`PRTE_ERROR`. Only the HNP (`ess/hnp`) and daemons (`ess/base`'s
`ess_base_std_prted`) call this; tools skip it and keep the default
module.

### `errmgr_base_fns.c` — `prte_errmgr_base_log()`

The default `logfn`. Turns an error code into a string via
`PRTE_ERROR_NAME` (`prte_strerror`) and prints
`"<name> PRTE_ERROR_LOG: <errstring> in file <f> at line <n>"` through
`pmix_output(0, …)`. If `prte_strerror` returns `NULL` (a "silent"
error) it prints nothing. This is the only executable code the base
contributes to error handling.

### `help-errmgr-base.txt`

The user-facing messages the components emit via `pmix_show_help`:
`failed-daemon-launch`, `failed-daemon`, `node-died`, `simple-message`.
Per the top-level GOLDEN RULE, if you touch this file you must
`rm src/util/prte_show_help_content.* && make` to force the generated
show-help content to be rebuilt.

---

## Component selection

`query` (in each component's `_component.c`) is a pure **role gate**:

| Component | Gate | Priority when it applies |
|-----------|------|--------------------------|
| `dvm` | `PRTE_PROC_IS_MASTER` | `1000` (MCA param `errmgr_dvm_priority`) |
| `prted` | `PRTE_PROC_IS_DAEMON` | `1000` (MCA param `errmgr_prted_priority`) |

Both default to priority **1000**, but that never collides: each returns
`*module = NULL; *priority = -1; return PRTE_ERROR` when the process is
not its role, so in any given process **at most one** component is
selectable. This is why the "highest priority wins" machinery is almost
decorative here — role, not priority, does the selecting. (The priority
params still let you swap in an out-of-tree replacement for one role by
registering a higher number.)

There is deliberately **no** generic/default component: a tool that
never calls `prte_errmgr_base_select()` simply runs with
`prte_errmgr_default_fns`, which does nothing but log.

---

## Threading and the state machine

Every real errmgr handler (`job_errors`, `proc_errors`, and the daemon's
`prted_abort`/`wakeup`) runs **on the progress thread**, invoked by the
`state` framework as a libevent callback. The callback signature is the
state-machine one, `(int fd, short args, void *cbdata)`, where `cbdata`
is a `prte_state_caddy_t *` carrying the `jdata`, the target `name`, and
the `job_state`/`proc_state`. The very first thing each handler does is
`PMIX_ACQUIRE_OBJECT(caddy)` and the last thing is `PMIX_RELEASE(caddy)`
on every exit path (watch the `goto cleanup;` discipline — a missed
release leaks the caddy). Because everything is single-threaded on the
progress thread, the handlers freely read and mutate global runtime
state (`prte_local_children`, `prte_process_info.num_daemons`,
`prte_rml_base.n_children`, the abort/term-ordered flags) without locks.

Handlers **must not block** and must not do their own thread-shifting for
the common path — they are already on the right thread. They cause
further work by activating *other* states
(`PRTE_ACTIVATE_JOB_STATE`/`PRTE_ACTIVATE_PROC_STATE`), which enqueue
new events, rather than calling termination logic inline.

---

## Conventions and gotchas specific to this framework

- **The interesting code is not in the vtable.** Reading `errmgr.h` tells
  you almost nothing about behavior. Grep each component's `init()` for
  `add_job_state` / `add_proc_state` to find the real entry points.
- **Two components, divergent policy, shared state names.** `dvm` and
  `prted` both register handlers for `PRTE_JOB_STATE_ERROR`,
  `PRTE_PROC_STATE_COMM_FAILED`, and `PRTE_PROC_STATE_ERROR`, but do
  opposite things (decide vs. report-upward). When you change how one
  role treats a state, check whether the other role needs the mirror
  change.
- **`finalizing` short-circuits.** Both `job_errors` handlers bail
  immediately if `prte_finalizing` is set — a shutting-down DVM must not
  re-drive error policy. Preserve that guard.
- **The daemon-job special case.** In both components a `jdata` whose
  nspace equals `PRTE_PROC_MY_NAME->nspace` is *the daemon job itself*,
  not an application job, and is handled on a separate branch (its
  failure means the DVM is in trouble). `caddy->jdata == NULL` is treated
  as "the daemon job" and back-filled from
  `prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace)`.
- **Elastic-DVM interactions live in `dvm/proc_errors`.** Grow-target
  rollback (`prte_plm_base_grow_target_failed`) and the shrink echo-guard
  (`prte_elastic_mode && !ALIVE && state >= TERMINATED`) are subtle and
  gated on `prte_elastic_mode`; see the `dvm` component guide and the
  repo memory on elastic-DVM ordering bugs before editing them.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG`/`PMIX_ERROR_LOG` on unexpected `rc`,
  `PRTE_ACTIVATE_*_STATE` to drive transitions rather than inline work.

---

## Debugging

```sh
prte --prtemca errmgr_base_verbose 5 ...   # trace errmgr decisions
prte --prtemca state_base_verbose 5 ...    # see the state transitions that fire errmgr
prte --prtemca plm_base_verbose 5 ...      # daemon launch/termination context
```

The errmgr handlers emit at verbosity 1–5 on
`prte_errmgr_base_framework.framework_output`: level 1 logs each
job/proc state received, level 5 traces the per-state decision (which
proc, which policy branch, whether a notification was sent, whether the
DVM is being torn down). Because the errmgr sits downstream of the state
machine, pairing `errmgr_base_verbose` with `state_base_verbose` is the
fastest way to see *what* transitioned and *how* the errmgr reacted.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`dvm/AGENTS.md`](dvm/AGENTS.md) — the HNP-side policy engine: notify
  submitters, terminate jobs, tear down the DVM, elastic grow/shrink
  fault handling.
- [`prted/AGENTS.md`](prted/AGENTS.md) — the daemon-side reporter: push
  local proc/daemon state to the HNP, kill local children, exit on order.
