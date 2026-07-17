# AGENTS.md — The `ess` Framework (Environment-Specific Services)

Orientation for AI agents and human contributors working in
`src/mca/ess/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`ess` (Environment-Specific Services) is the framework that **brings a
PRRTE process up and tears it down**. It answers one question: given the
environment I was launched into and the role I am supposed to play,
how do I initialize the entire runtime — my identity, my session
directory, the PMIx server, the communication stack, and every other
framework — and how do I shut it all back down cleanly?

It runs in exactly two PRRTE process roles:

| Role | Process | Selected component |
|------|---------|--------------------|
| **HNP / DVM master** | `prte` | `hnp` |
| **prted daemon** | `prted` | `env` (ssh/default), or an RM-specific module: `slurm`, `pals`, `lsf` |

`ess` is the **earliest** framework to do real work in a process's life.
It is opened and its module selected inside `prte_init()`
(`src/runtime/prte_init.c`), immediately after `schizo` (which `ess`
uses to help pick a personality):

```
prte_init()
  → open schizo, select schizo
  → open ess,  prte_ess_base_select()       ← pick the one winning module
  → prte_ess.init(argc, argv)               ← THE bring-up: opens every other framework
  ...
prte_finalize()
  → prte_ess.finalize()                     ← tear-down in reverse
  → close ess framework
```

Nothing else in the process is usable until `prte_ess.init()` returns.
The selected module's `init` is where `state`, `errmgr`, `plm`,
`grpcomm`, `odls`, `rmaps`, `ras` (HNP only), `iof`, `filem`,
`prtereachable`, and the `rml` are opened and selected, the PMIx server
is started, and the process's own job/proc/node data objects are
created. In other words, **`ess` is the orchestrator of startup**; the
other frameworks are its dependents.

Note: PRRTE's user-facing *tools* (`prun`, `pterm`, `prte_info`) are
PMIx tools — they attach to a running DVM through the PMIx tool
interface and do **not** select an `ess` module. They may open the `ess`
framework for symbol availability, but `prte_ess_base_select()` /
`prte_ess.init()` are only exercised by `prte` (HNP) and `prted`
(daemon). Historically `ess` also carried tool/singleton components;
today the surviving components serve only the HNP and daemon roles.

---

## Directory layout

```
ess/
  ess.h                       # module/component vtable: the init + finalize fn ptrs
  base/
    base.h                    # framework-global struct, base API prototypes, signal class
    ess_base_frame.c          # framework open/close/register; MCA params; signal-forwarding infra
    ess_base_select.c         # prte_ess_base_select() — PICK-ONE highest-priority winner
    ess_base_std_prolog.c     # prte_ess_base_std_prolog() — dt_init + wait_init (all modules)
    ess_base_std_prted.c      # prte_ess_base_prted_setup/_finalize() — shared daemon bring-up/tear-down
    ess_base_bootstrap.c      # launcher-less bootstrap: parse config, publish identity, synth peer URIs
    help-ess-base.txt         # user-facing signal-forwarding error text
    static-components.h       # generated: the components statically linked into this build
  hnp/                        # HNP / DVM master (pri 100, gated on PRTE_PROC_IS_MASTER)
  env/                        # generic daemon, ssh-launched (pri 1, the daemon default)
  slurm/                      # daemon under SLURM (pri 50, gated on SLURM_JOBID + hnp_uri)
  pals/                       # daemon under HPE/Cray PALS aprun (pri 50, gated on PALS_APID + hnp_uri)
  lsf/                        # daemon under IBM LSF (pri 40, gated on LSB_JOBID + hnp_uri)
```

Read `ess.h` first (it is tiny — two function pointers), then
`base/ess_base_std_prted.c`, which is where most of the framework's
real work actually lives: every daemon module is a thin wrapper around
it.

---

## The module contract

An `ess` module is the thinnest vtable in the tree. Every component
exposes exactly two functions through `ess.h`:

```c
typedef int (*prte_ess_base_module_init_fn_t)(int argc, char **argv);
typedef int (*prte_ess_base_module_finalize_fn_t)(void);

struct prte_ess_base_module_3_0_0_t {
    prte_ess_base_module_init_fn_t     init;
    prte_ess_base_module_finalize_fn_t finalize;
};
```

| Function | Meaning | Return protocol |
|----------|---------|-----------------|
| `init(argc, argv)` | Bring the entire runtime up for this process in this environment: establish identity, discover topology, open/select all downstream frameworks, start the PMIx server. | `PRTE_SUCCESS`, or a `PRTE_ERR_*`. Modules typically return `PRTE_ERR_SILENT` after emitting their own `show_help` so `prte_init` does not double-report. |
| `finalize()` | Reverse of `init`: close the frameworks the module opened, kill local procs, shut down the PMIx server. | `PRTE_SUCCESS` (errors are logged with `PRTE_ERROR_LOG` but generally not propagated). |

The component struct is a bare `pmix_mca_base_component_t` (typedef'd to
`prte_ess_base_component_t`) — `ess` has **no** component-level data
beyond the standard MCA header. The version macro is
`PRTE_ESS_BASE_VERSION_3_0_0` (declared in `ess.h`).

The selected module's two function pointers are copied into the single
global:

```c
PRTE_EXPORT extern prte_ess_base_module_t prte_ess;   /* ess.h */
```

`prte_init` calls `prte_ess.init(...)`; `prte_finalize` calls
`prte_ess.finalize()`. There is no per-component global structure kept
after selection (`ess_base_select.c` explicitly discards the winning
*component* and keeps only `*best_module`).

---

## Component selection **is** "pick one"

Unlike `rmaps` (which keeps every module priority-sorted), `ess` selects
a **single** winning module. `prte_ess_base_select()`
(`ess_base_select.c`) delegates to the generic `pmix_mca_base_select()`:
every component's `query` returns a priority, the highest wins, and its
module is copied into the `prte_ess` global:

```c
prte_ess = *best_module;
```

Each component's `query` gates itself on the process role and the
environment, so at most one component ever claims a positive priority
for a given process:

| Component | Priority | Gate (all must hold) |
|-----------|----------|----------------------|
| `hnp`   | **100** | `PRTE_PROC_IS_MASTER` |
| `slurm` | **50**  | `PRTE_PROC_IS_DAEMON` **and** `getenv("SLURM_JOBID")` **and** `prte_process_info.my_hnp_uri != NULL` |
| `pals`  | **50**  | `PRTE_PROC_IS_DAEMON` **and** `getenv("PALS_APID")` **and** `my_hnp_uri != NULL` |
| `lsf`   | **40**  | `PRTE_PROC_IS_DAEMON` **and** `getenv("LSB_JOBID")` **and** `my_hnp_uri != NULL` |
| `env`   | **1**   | `PRTE_PROC_IS_DAEMON` (always available to any daemon) |

The logic is: the HNP is unambiguous (`hnp`, priority 100, only ever
selected in the `prte` process). For a daemon, the RM-specific modules
sit above the generic `env` default so that if a daemon *is* running
under SLURM/PALS/LSF with a path home to the HNP, the RM module wins and
`env` is the fallback for everything else (notably ssh-launched
daemons). A `query` that does not apply returns `priority = -1`,
`module = NULL`, and `PRTE_ERROR`, so it cannot be chosen.

Because `lsf` and `pals` link against vendor libraries, they are only
*built* where those libraries are present (see each component's
`configure.m4` → `PRTE_CHECK_LSF` / `PRTE_CHECK_PALS`). On a platform
without them the framework's `static-components.h` will not list them at
all — do not assume every component compiled into your local build.

---

## What `base/` provides

The base is not a "default behavior" fallback the way some frameworks'
bases are — it is the **shared implementation** that the modules call
into. The daemon modules (`env`, `slurm`, `pals`, `lsf`) are almost
identical: each is little more than *set my name from the environment*
plus a call to the base's `prted_setup`.

### `ess_base_frame.c` — framework plumbing + signal forwarding

- Standard `PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, ess, ...)` with
  register/open/close hooks.
- **MCA parameters** (registered in `prte_ess_base_register`), each with
  a deprecated `prte_ess_*` synonym:
  - `ess_base_nspace` → `prte_ess_base_nspace` — the namespace string a
    daemon adopts as its identity.
  - `ess_base_vpid` → `prte_ess_base_vpid` — the daemon's vpid (rank),
    parsed with `strtoul`.
  - `ess_base_num_procs` → `prte_ess_base_num_procs` — the number of
    daemons in the DVM (becomes `prte_process_info.num_daemons`).
  - `ess_base_forward_signals` → the comma-delimited list of signals to
    forward to application processes (`"all"`, `"none"`, or names/ints).
- **Signal-forwarding infrastructure**, shared by every daemon:
  - `known_signals[]` — a table mapping signal name → number →
    `can_forward` flag. `SIGTERM`/`SIGHUP`/`SIGINT` are always handled
    (not forwardable via this param); `SIGKILL`/`SIGPIPE` can never be
    forwarded.
  - `prte_ess_base_setup_signals(char *signals)` — parses the requested
    list (handling `"none"`, `"all"`, names, and integers), rejects
    unknown or non-forwardable signals via `help-ess-base.txt`, and
    appends `prte_ess_base_signal_t` items onto the global
    `prte_ess_base_signals` list. Guarded by a `signals_added` latch so
    it only runs once. The forwardability (`can_forward`) gate is
    enforced on **both** input forms — a non-forwardable signal is
    rejected whether given by name (`SIGTERM`) or by number (`15`); keep
    the two parse branches in sync. Note the latch is set *before*
    parsing, so a call that fails partway still latches — the tool
    callers (`prte.c`, `prun_common.c`) abort on error, so this is
    benign in practice but worth knowing when testing.
  - `prte_ess_base_signal_t` — a `pmix_list_item_t` subclass
    (`signame`/`signal`/`can_forward`), instantiated here with
    constructor/destructor. The actual libevent signal handlers that
    consume this list are installed by `prte_ess_base_prted_setup()`.

### `ess_base_select.c` — the winner

`prte_ess_base_select()`: pick-one selection described above. ~20 lines.

### `ess_base_std_prolog.c` — the universal preamble

`prte_ess_base_std_prolog()`: the first thing **every** module's `init`
calls. It does the two things that must happen before anything else in
any role:

1. `prte_dt_init()` — register PRRTE's data-type (pack/unpack) support.
2. `prte_wait_init()` — set up the `waitpid`/`SIGCHLD` machinery.

On failure it shows `prte_init:startup:internal-failure` and returns the
error.

### `ess_base_std_prted.c` — the shared daemon bring-up

This is the heart of the framework for daemons.
`prte_ess_base_prted_setup()` is what `env`/`slurm`/`pals`/`lsf` all call
after setting their name. In order, it:

1. Installs signal handlers: `SIGPIPE` (ignored), `SIGTERM`/`SIGINT`
   (→ `shutdown_signal` → `PRTE_JOB_STATE_FORCED_EXIT`), and one
   `signal_forward_callback` handler per entry on
   `prte_ess_base_signals` (each forwards the signal to local procs by
   sending a `PRTE_DAEMON_SIGNAL_LOCAL_PROCS` command to itself over the
   RML). That command's payload is `(jobid=wildcard, signal-number)`,
   and the packed **signal number must be the actual caught signal**
   (`PRTE_EVENT_SIGNAL(...)`), matching what `prted_comm.c` unpacks — a
   subtle 2021 regression packed the wildcard nspace here instead,
   silently turning every forwarded signal into signal `0` (a no-op).
   Keep the pack type/value in lock-step with the unpack side.
2. Discovers the local hwloc topology if not already set.
3. Defines the HNP name (`PRTE_PROC_MY_HNP` = my nspace, rank 0).
4. Opens and selects `state`, opens `errmgr`.
5. Opens/selects `plm` **only if** `PRTE_MCA_plm` is set in the
   environment (ssh-style remote launch); an ordinary prted has no need
   of a PLM.
6. Creates the daemon job data object (`prte_job_t`), gives it the
   `"prte"` schizo personality by default, adds one app context and one
   proc object for itself, and marks the daemon job RUNNING/reported.
7. Creates the session directory tree, redirects `pmix_output` into it,
   and (under `prte_debug_daemons_file_flag`) sends stdout/stderr to a
   per-daemon log file.
8. Starts the PMIx server (`pmix_server_init` → later
   `pmix_server_start`), gathers interface aliases.
9. Opens/selects the communication stack: `prtereachable`, `rml`.
10. Selects `errmgr`; opens/selects `grpcomm`, `odls`, `rmaps`.
11. Adds the local topology to `prte_node_topologies`.
12. If a PLM was opened, calls `prte_plm.init()` (must come after comms).
13. Opens/selects `iof` and `filem`.

`prte_ess_base_prted_finalize()` reverses this: removes the signal
handlers, finalizes `errmgr`, closes `filem`/`grpcomm`/`iof`/`plm`,
kills local procs (`prte_odls.kill_local_procs`), closes
`odls`/`errmgr`, closes the `rml`, closes `prtereachable`/`state`, and
finalizes the PMIx server.

The HNP does **not** use `prted_setup`; `ess/hnp` open a very similar but
distinct set of frameworks inline (it additionally opens `ras`, sets the
HNP name via the PLM, and sets up its own node object). See
[`hnp/AGENTS.md`](hnp/AGENTS.md).

### `ess_base_bootstrap.c` — launcher-less DVM bootstrap

Support for starting a DVM without a launcher (each node's `prted`
reads a shared bootstrap configuration file and self-assigns identity).
This is **not** part of a module `init`; it is called directly by
`prted.c` and `prte_init.c` at specific, timing-sensitive points:

- `prte_ess_base_bootstrap_params()` — **phase 1**: parse the config
  file (`prte_bootstrap_parse`) and publish the DVM-wide MCA parameters
  (static ports, IP version/family, radix routing, retry backoff,
  tmpdir, fqdn handling). Must run *before* `prte_register_params()` so
  those variables read the environment on first registration; called
  from `prte_init.c`.
- `prte_ess_base_bootstrap(bool *is_controller)` — **phase 2**: once the
  local hostname is known, determine this node's role and rank
  (`prte_bootstrap_my_identity`). The controller adopts nspace
  `"<cluster>-prte-dvm"` and rank 0; an ordinary daemon publishes its
  identity through the `ess_base_*` params and synthesizes the
  controller's contact URI so it can phone home before any nidmap
  exists. Called from `prted.c`.
- `prte_ess_base_bootstrap_peer_uri(rank, &uri)` — synthesize the RML
  contact URI of any peer daemon purely from the config, so a daemon can
  reach a parent (or a re-parented grandparent after a lifeline heals)
  before contact info has been distributed. Called from `prted.c` and
  the OOB (`src/rml/oob/oob_base_stubs.c`).
- Helpers `parse_cidr`, `same_inaddr`, `pick_host_address`, `synth_uri`
  build a correctly-shaped `<name>;tcp://ip:port:mask` URI, using the
  `DVMNetworks` CIDRs to disambiguate a multi-homed host (and failing
  loudly rather than baking in a wrong interface).

The parsed `bootstrap_cfg` is deliberately retained for the life of the
process (see the comment at the end of `prte_ess_base_bootstrap`)
because peer-URI synthesis can happen much later. See the repo memory on
the bootstrap DVM work.

### `base.h` describes only live symbols

`base.h` once carried vestigial declarations — `prte_ess_env_get`,
`prte_ess_env_put`, `prte_ess_base_proc_binding`, and the
`prte_ess_base_std_buffering` variable — that were never defined anywhere
in the tree. They have been removed. If you reintroduce a helper here,
make sure it actually has a definition; a declaration without one is a
trap for the next reader.

---

## Global state and data structures

| Symbol | Where | Meaning |
|--------|-------|---------|
| `prte_ess` | `ess_base_frame.c` | The selected module's `{init, finalize}`; the framework's only runtime entry point. |
| `prte_ess_base_framework` | `ess_base_frame.c` | The MCA framework object (its `framework_output` is the verbosity channel). |
| `prte_ess_base_nspace` / `_vpid` / `_num_procs` | `ess_base_frame.c` | Daemon identity, from MCA params/env; consumed by each daemon module's `*_set_name`. |
| `prte_ess_base_signals` | `ess_base_frame.c` | List of `prte_ess_base_signal_t` to forward; populated by `setup_signals`, consumed by `prted_setup`. |

---

## Conventions and gotchas

- **Daemon modules are near-clones.** `env`/`slurm`/`pals`/`lsf` each do:
  `std_prolog` → `<rm>_set_name()` → `prte_ess_base_prted_setup()`, and
  finalize with `prte_ess_base_prted_finalize()`. The *only* real
  per-component logic is `*_set_name`: how the daemon derives its vpid
  and nodename from that RM's environment. If you are adding a new
  RM-launched daemon environment, that is the ~40-line function you
  write; everything else is base.
- **`set_name` derives the true vpid.** The base params give a starting
  vpid; the RM module adds a per-node offset (`SLURM_NODEID`,
  `PALS_NODEID`, `LSF_PM_TASKID - 1`) so each daemon lands on a unique
  rank. `env` uses the param verbatim (ssh launch assigns the vpid
  directly). Getting this offset wrong collides daemon ranks — a nasty,
  silent failure.
- **Selection is pick-one; keep `query` gates mutually exclusive.** A new
  component must return a positive priority *only* for the precise
  role+environment it serves, and `-1`/`PRTE_ERROR` otherwise, or you
  will contend with an existing module. Match the RM gate idiom:
  `PRTE_PROC_IS_DAEMON && getenv("<RM_MARKER>") && my_hnp_uri != NULL`.
- **`init` errors should be `PRTE_ERR_SILENT` after a `show_help`.** The
  modules emit their own `prte_init:startup:internal-failure` help and
  return `PRTE_ERR_SILENT` (respecting `prte_report_silent_errors`) so
  `prte_init` does not print a second, redundant message. On the `init`
  error path, release any partially-built `jdata`.
- **Order is load-bearing in `prted_setup`.** Comms must be up before
  `plm.init()`; the PMIx server must be init'd before gathering aliases;
  IOF comes after routes. Do not reorder the framework opens casually.
- **Version macro is `PRTE_ESS_BASE_VERSION_3_0_0`.** Bump deliberately;
  the module struct is `prte_ess_base_module_3_0_0_t`.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_ERROR_LOG` on unexpected errors.

---

## Debugging

```sh
prte  --prtemca ess_base_verbose 5 ...     # trace HNP bring-up
prted --prtemca ess_base_verbose 5 ...     # (via the DVM) trace daemon bring-up
```

At verbosity ≥1 the daemon modules print the name they set for
themselves; `prted_setup` dumps session-dir setup at ≥2 and full
topology at >15; the HNP module dumps its node aliases at >0 and
topology at >15. Because `ess` runs so early, an `init` failure usually
surfaces as the `prte_init:startup:internal-failure` help message naming
the failing step (e.g. `prte_ess_base_prted_setup`,
`prte_state_base_select`) — that string is your first clue.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`hnp/AGENTS.md`](hnp/AGENTS.md) — the DVM master; read this second.
- [`env/AGENTS.md`](env/AGENTS.md) — the generic ssh-launched daemon default.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — daemon under SLURM.
- [`pals/AGENTS.md`](pals/AGENTS.md) — daemon under HPE/Cray PALS.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — daemon under IBM LSF.
