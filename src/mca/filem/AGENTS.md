# AGENTS.md — The `filem` Framework (File Management)

Orientation for AI agents and human contributors working in
`src/mca/filem/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`filem` (File Management) **pre-positions files across the DVM before a
job launches.** When a user asks for their executable and/or data files
to be staged out to every node — `prun --preload-binary`,
`--preload-files a,b,c` (which set the `PRTE_APP_PRELOAD_BIN` and
`PRTE_APP_PRELOAD_FILES` app-context attributes) — `filem` is what
actually moves the bytes from the HNP to the daemons and then links them
into each local process's session directory so the app finds them by a
relative path.

Unlike most PRRTE frameworks, `filem` runs on **both** ends of the DVM:

- **HNP (DVM master)** — orchestrates. It scans the job's app contexts
  for preload requests, reads the source files, and broadcasts their
  contents to every daemon.
- **prted (per-node daemon)** — receives. Each daemon writes the bytes
  into its node's top session directory, unpacks archives, and later
  symlinks the staged files into the session directory of every local
  process in the job.

### Place in the launch flow

Pre-positioning happens on the DVM state machine at
`PRTE_JOB_STATE_VM_READY`, **before mapping**:

```
… → PRTE_JOB_STATE_VM_READY → (filem preposition) → PRTE_JOB_STATE_MAP → …
```

`vm_ready()` in `src/mca/state/dvm/state_dvm.c` calls
`prte_filem.preposition_files(jdata, files_ready, jdata)`. The transfer
is **asynchronous**: `preposition_files` returns immediately, and when
every daemon has acknowledged every file the framework fires the
`files_ready` completion callback, which advances the job to
`PRTE_JOB_STATE_MAP` (or `PRTE_JOB_STATE_FILES_POSN_FAILED` on error).
The same entry point is also reached from the PLM launch-support path
(`src/mca/plm/base/plm_base_launch_support.c`).

Then, much later, when a daemon is about to fork the local application
processes, `odls` calls the second framework entry point,
`prte_filem.link_local_files(jdata, app)`
(`src/mca/odls/base/odls_base_default_fns.c`), to create the per-proc
symlinks.

---

## Directory layout

```
filem/
  filem.h                     # module/component vtable + the request/file-set/process-set classes
  base/
    base.h                    # framework-global decls, the "none" no-op prototypes, base comm API
    filem_base_frame.c        # framework open/close; the default "none" prte_filem module
    filem_base_select.c       # component selection — classic PICK-ONE (single winner)
    filem_base_fns.c          # the request/file-set/process-set PMIX_CLASS_INSTANCEs + all "none" no-ops
    filem_base_receive.c      # dormant base RML service: remote-path / node-name query commands
    owner.txt                 # owner/status (INTEL, maintenance)
  raw/                        # the ONLY component (pri 0): xcast-based chunked staging
```

Read `filem.h` first — it defines the data structures (`request`,
`file_set`, `process_set`) and the module vtable. Then read the `raw`
component, which is where all the real work lives; the base provides
mostly the class definitions and a stack of no-op fallbacks.

---

## The module contract

A `filem` module is a `prte_filem_base_module_t` (alias for
`prte_filem_base_module_1_0_0_t`, in `filem.h`). Its vtable is broad —
it carries a classic put/get/rm/wait file-transfer API **and** the
higher-level preposition/link API — but in practice only two of these
functions do anything today (see the raw component). Every function
pointer:

| Field | Signature | Meaning | Return |
|-------|-----------|---------|--------|
| `filem_init` | `int (void)` | Module init (called by `select` on the winner). | `PRTE_SUCCESS` |
| `filem_finalize` | `int (void)` | Module teardown (called on framework close). | `PRTE_SUCCESS` |
| `fault_handler` | `void (const prte_rml_recovery_status_t *)` | React to a daemon failure during transfer. | — |
| `put` / `put_nb` | `int (prte_filem_base_request_t *)` | Push file(s) to remote proc(s), blocking / async. | `PRTE_SUCCESS`/`PRTE_ERROR` |
| `get` / `get_nb` | `int (prte_filem_base_request_t *)` | Pull file(s) from remote proc(s), blocking / async. | `PRTE_SUCCESS`/`PRTE_ERROR` |
| `rm` / `rm_nb` | `int (prte_filem_base_request_t *)` | Remove remote file(s), blocking / async. | `PRTE_SUCCESS`/`PRTE_ERROR` |
| `wait` | `int (prte_filem_base_request_t *)` | Block until one async request completes. | `PRTE_SUCCESS`/`PRTE_ERROR` |
| `wait_all` | `int (pmix_list_t *)` | Block until a list of async requests completes. | `PRTE_SUCCESS`/`PRTE_ERROR` |
| `preposition_files` | `int (prte_job_t *, prte_filem_completion_cbfunc_t, void *)` | **Stage a whole job's preload files to every node (async).** | `PRTE_SUCCESS`, callback on completion |
| `link_local_files` | `int (prte_job_t *, prte_app_context_t *)` | **Symlink already-staged files into each local proc's session dir.** | `PRTE_SUCCESS`/error |

The completion callback type is
`typedef void (*prte_filem_completion_cbfunc_t)(int status, void *cbdata)`.

**Only `preposition_files` and `link_local_files` are actually invoked
anywhere in PRRTE today** (from the state machine / PLM and from odls,
respectively). The put/get/rm/wait half of the vtable is legacy FileM
API that the `raw` module deliberately wires to the base "none" no-ops.
If you are adding file-staging behavior, you almost certainly want to
work through the preposition/link pair, not put/get.

### The version macro

Components declare `PRTE_FILEM_BASE_VERSION_2_0_0`
(`PRTE_MCA_BASE_VERSION_3_0_0("filem", 2, 0, 0)`). Note the module
*struct* is still named `..._1_0_0_t`; the framework version and the
struct version are independent numbers here.

---

## Component selection is "pick one"

`prte_filem_base_select()` (in `filem_base_select.c`) is a **classic
single-winner selection**, unlike `rmaps`: it calls `pmix_mca_base_select`,
copies the highest-priority component's module into the global
`prte_filem`, and runs its `filem_init`. If **no** component is selected
it is not an error — the framework simply keeps the default **"none"**
module (all no-ops) that `filem_base_frame.c` statically installs into
`prte_filem`. Selection is driven from the HNP and daemon ESS init
(`src/mca/ess/hnp/ess_hnp_module.c`,
`src/mca/ess/base/ess_base_std_prted.c`).

Today the tree ships exactly one component, `raw`, whose `query` returns
priority **0**. So in a normal build `raw` always wins; the "none"
module is what you get only if `raw` is unbuilt/unselected.

---

## What `base/` provides

The base is thin. It contributes the data classes, the no-op fallback
module, and a (currently dormant) RML query service.

### Data structures (`filem.h` + `filem_base_fns.c`)

Three reference-counted PMIx classes model a transfer request. They are
constructed/destructed in `filem_base_fns.c` via `PMIX_CLASS_INSTANCE`:

- **`prte_filem_base_process_set_t`** — a `{source, sink}` pair of
  `pmix_proc_t`s naming who a file moves *from* and *to*. Wildcards mean
  "all procs of a job"; INVALID means "not applicable". Constructed to
  `PRTE_NAME_INVALID` on both ends.
- **`prte_filem_base_file_set_t`** — one `{local_target, remote_target}`
  file pairing, plus `app_idx`, local/remote **hints**
  (`PRTE_FILEM_HINT_NONE`/`SHARED`), and a **`target_flag`** file-type
  code (`PRTE_FILEM_TYPE_FILE`/`DIR`/`TAR`/`BZIP`/`GZIP`/`EXE`/`UNKNOWN`).
  The destructor frees both path strings.
- **`prte_filem_base_request_t`** — a whole request: a list of process
  sets, a list of file sets, plus internal bookkeeping arrays
  (`is_done`, `is_active`, `exit_status`, `num_mv`) and a
  `movement_type` (`PUT`/`GET`/`RM`/`UNKNOWN`). The destructor drains and
  releases both lists and frees the bookkeeping arrays.

The `raw` component largely ignores `process_set`/`request` and works
directly with `file_set` plus its own private classes; these types
exist because the put/get API is defined in terms of them.

### The "none" module (`filem_base_fns.c` + `filem_base_frame.c`)

`filem_base_frame.c` statically initializes the global `prte_filem` to a
module made entirely of the `prte_filem_base_none_*` functions from
`filem_base_fns.c`. Each of these is a no-op that returns `PRTE_SUCCESS`
(the `preposition` no-op politely fires the completion callback with
`PRTE_SUCCESS` so the state machine still advances). This is what runs
when file staging is disabled or unselected — pre-positioning simply
does nothing and the job proceeds.

**Every slot must be wired, including `fault_handler`.** The none module
carries a `prte_filem_base_none_fault_handler` no-op, because
`routed_radix.c` calls `prte_filem.fault_handler(status)`
**unconditionally** on every daemon-fault recovery — with no NULL guard.
A none module that left the slot NULL (as it historically did) crashed
the DVM the first time a daemon failed while filem was unselected. If you
add a field to the module vtable, give the none module a no-op for it and
confirm no unconditional caller dereferences a NULL slot.

### The dormant base RML service (`filem_base_receive.c`)

`base.h` declares a small RML service — `prte_filem_base_comm_start()`,
`prte_filem_base_comm_stop()`, and the `prte_filem_base_recv()` handler
— that answers two commands on `PRTE_RML_TAG_FILEM_BASE`:

- `PRTE_FILEM_GET_PROC_NODE_NAME_CMD` — given a `pmix_proc`, reply with
  the name of the node that proc is on (looked up via
  `prte_get_job_data_object` / the job's proc array).
- `PRTE_FILEM_GET_REMOTE_PATH_CMD` — given a filename, resolve it to an
  absolute path (prepending `getcwd` if relative), `stat` it, and reply
  with the absolute path plus a file-type code
  (`FILE`/`DIR`/`UNKNOWN`).

This is scaffolding for a put/get-style component that needs to
negotiate remote absolute paths before transferring. **No code in the
tree currently calls `prte_filem_base_comm_start`, and the `raw`
component runs its own receives instead**, so these handlers are
effectively dead today — accurate to note, and a place to be careful:
`raw` and this base service would both claim `PRTE_RML_TAG_FILEM_BASE`
if the base service were ever started. The global
`prte_filem_base_is_active` bool is likewise defined but currently
unused.

Because it is dead code it accumulated latent bugs; two were recently
corrected and are worth knowing if you ever revive it. `comm_stop`'s
"already stopped?" guard was inverted (`if (recv_issued)` instead of
`if (!recv_issued)`), so it early-returned without ever cancelling the
recv. And `filem_base_process_get_remote_path_cmd` leaked the unpacked
`filename` on the `getcwd`-failure path (it `return`ed instead of
`goto CLEANUP`). If you start posting this service for real, audit its
error paths first.

---

## Threading & the async transfer model

`filem` does real asynchronous I/O on the progress thread, so it follows
the caddy/threadshift pattern described in the top-level `AGENTS.md`. The
`preposition_files` API is non-blocking: it kicks off work and returns,
and a **completion callback** (`prte_filem_completion_cbfunc_t`) is
invoked once every daemon has acknowledged every file. Do not block the
progress thread waiting for a transfer; register a callback and let the
state machine advance from there.

The concrete mechanics — chunked reads driven by libevent write events,
`xcast` broadcast to daemons, per-daemon ack counting — all live in the
`raw` component and are documented in its guide. The key framework-level
contract is just: **`preposition_files` is fire-and-forget with a
completion callback; `link_local_files` is synchronous and runs on the
daemon at fork time.**

---

## Conventions & gotchas

- **Preload paths are forced relative.** Staged files always land under a
  node's session directory; the framework rewrites absolute source paths
  to relative remote targets so a stray `/etc/...` can never be
  overwritten on a remote node. Preserve that safety property.
- **`preposition_files` MUST fire its callback on every exit path**
  (including "nothing to stage" and error), or the job wedges at
  `VM_READY` forever. The "none" module and `raw` both take care to do
  this.
- **Two entry points, two ends of the DVM.** `preposition_files` runs on
  the HNP; `link_local_files` runs on the daemon. Don't assume a single
  address space or that HNP-side state is visible daemon-side — the only
  channel between them is the RML/xcast traffic.
- **Selection failure is not an error.** Leaving `prte_filem` as the
  "none" module is a supported configuration; never treat a missing
  component as fatal.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG`/`PRTE_ACTIVATE_JOB_STATE` for errors.

---

## Debugging

```sh
prte --prtemca filem_base_verbose 5 ...    # trace staging decisions on both ends
prun --preload-binary ...                  # stage the executable itself
prun --preload-files a.dat,b.tar.gz ...    # stage data files (archives auto-extracted)
```

Framework verbosity ≥1 already prints the list of files chosen for
positioning and every chunk sent/received; ≥10 traces per-proc symlink
creation and archive path enumeration. A stuck job at `VM_READY` almost
always means a preposition callback that never fired or an ack that never
arrived from a daemon.

---

## Testing

The staging machinery (`raw_preposition_files`, the daemon receive/write
path, `raw_link_local_files`) is asynchronous, progress-thread,
multi-node I/O and needs a live DVM — exercise it with the
`--preload-binary`/`--preload-files` smoke tests above and the
integration harnesses.

What *is* unit-testable with no DVM lives in
[`test/unit/filem/test_filem.c`](../../../test/unit/filem/) (wired into
`make check`):

- the three request classes (`process_set`/`file_set`/`request`) —
  construct-to-defaults and clean destruct/drain;
- the default **"none" module** — every slot is a success-returning
  no-op, `preposition_files` fires its completion callback, and
  `fault_handler` is non-NULL and callable (the regression pin for the
  `routed_radix.c` crash described above).

The test opens the framework but deliberately does **not** run
`prte_filem_base_select()`, so `prte_filem` stays the none module.

---

## Where to go next

- [`raw/AGENTS.md`](raw/AGENTS.md) — the only component: how files are
  chunked, `xcast`-broadcast to every daemon, written into the session
  directory, unarchived, and symlinked into each proc's directory. Read
  this next.
