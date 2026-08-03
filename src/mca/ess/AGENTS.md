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
`grpcomm`, `odls`, `rmaps` and `ras` (both HNP only), `iof`, `filem`,
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
    ess_base_frame.c          # framework open/close/register; MCA params; daemon identity;
                              #   the signal-forwarding list the TOOLS consume
    ess_base_select.c         # prte_ess_base_select() — PICK-ONE highest-priority winner
    ess_base_std_prolog.c     # prte_ess_base_std_prolog() — dt_init + wait_init (all modules)
    ess_base_std_prted.c      # prte_ess_base_prted_setup/_finalize() — shared daemon bring-up/tear-down
    ess_base_bootstrap.c      # launcher-less bootstrap: parse config, publish identity, synth peer URIs
    help-ess-base.txt         # user-facing signal-forwarding and identity error text
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
    it only runs once.

    **The two parse branches must stay in step.** A request can name a
    signal (`SIGTERM`) or give its number (`15`), and each check has to
    exist on both sides or the number form becomes a way around it:

    - the forwardability (`can_forward`) gate — enforced on both;
    - the **range** check — a number must be a real signal this platform
      can deliver (`0 < n < _NSIG`). Without it `strtoul` happily takes
      `-1` (wrapping it) or `999`, and the libevent handler install that
      follows fails *silently* on both, so the user gets no forwarding
      and no diagnostic.

    The latch is set only once the list is fully built. It used to be set
    on entry, which burned the one allowed pass on a request that was
    rejected partway through — leaving whatever it had already appended
    installed, with no way to replace it, and making every rejection
    unreachable a second time and so untestable.
  - `prte_ess_base_signal_t` — a `pmix_list_item_t` subclass
    (`signame`/`signal`), instantiated here with constructor/destructor.
    It used to carry a third field, `can_forward`, that no code ever set
    or read — every item on the list carried an uninitialized bool. The
    forwardability decision belongs to `known_signals[]` and is made
    during the parse; an entry only reaches this list once it has passed.

### Signal forwarding is a tool-side feature

This is the thing to know before touching anything named `*signal*` in
this framework: **the list `prte_ess_base_setup_signals()` builds is
consumed by the tools, not by daemons.**

`prte`/`prterun` (`src/prted/prte.c`) and `prun`
(`src/prted/prun_common.c`) each call it with their own
`--forward-signals` value and then install their own handlers over the
resulting list. When one fires, the tool relays the signal to the DVM,
and each daemon receives it as a `PRTE_DAEMON_SIGNAL_LOCAL_PROCS` command
on the RML — unpacked in `src/prted/prted_comm.c` as
`(nspace, int32 signal)` — which is what actually reaches the application
processes.

Nothing calls `prte_ess_base_setup_signals()` in a `prted`, and the plm
does not forward the `ess_base_forward_signals` MCA parameter to daemons
either, so `prte_ess_base_signals` is **always empty in a daemon**.
`prte_ess_base_prted_setup()` used to install a handler per entry anyway,
with a `signal_forward_callback` of its own; that block could never run,
and it has been removed. It was not harmless dead code — a 2021
regression in that callback (packing a wildcard where the signal number
belongs) was found and fixed years later in code nothing executes, and it
misleads anyone reasoning about how a signal reaches a process.

If you are chasing signal delivery, start at the tool, not here. If you
ever want a daemon to honor a signal sent *directly* to it, that is a new
feature: it needs the list populated in the daemon and the parameter
plumbed out to it, neither of which exists.

### Daemon identity is established in one place

`prte_ess_base_set_identity(const char *offset_envar, int offset_adjust)`
turns the identity a launcher published into this daemon's name, and it
is the **entire** body of all four daemon modules' `*_set_name`:

| Component | Call |
|-----------|------|
| `env`   | `prte_ess_base_set_identity(NULL, 0)` |
| `slurm` | `prte_ess_base_set_identity("SLURM_NODEID", 0)` |
| `pals`  | `prte_ess_base_set_identity("PALS_NODEID", 0)` |
| `lsf`   | `prte_ess_base_set_identity("LSF_PM_TASKID", -1)` |

It loads `prte_ess_base_nspace` into `PRTE_PROC_MY_NAME->nspace`, parses
`prte_ess_base_vpid`, adds the per-node index the RM exports through
`offset_envar` (an RM hands every daemon it starts the *same* base vpid;
the node index is what tells them apart), applies `offset_adjust`
(LSF numbers its tasks from one, everyone else from zero), and sets
`prte_process_info.num_daemons` from `prte_ess_base_num_procs`.

**Every input is validated, and that is the point of the function
existing.** The four modules previously each wrote the obvious shorthand
— `strtoul(prte_ess_base_vpid, NULL, 10)` and `atoi(getenv(...))`, with
no check on either — and that shorthand has a silent, severe failure
mode: any non-numeric value reads as **0**, and rank 0 is the DVM
controller. A daemon handed a garbled vpid therefore adopts the HNP's
identity, and the DVM comes apart later in ways that point nowhere near
the bad input. So a value that is missing, not a plain non-negative
decimal number, or out of rank range is refused with a diagnostic naming
it (`ess-base:bad-identity` / `ess-base:rank-out-of-range` in
`help-ess-base.txt`).

Two details worth keeping:

- The sum is computed and compared as a **signed long**, not a
  `pmix_rank_t`. `LSF_PM_TASKID` of 0 with the `-1` adjustment would
  otherwise wrap to a rank near `UINT32_MAX`, and a sum past the end of
  the rank space would be *truncated into* the valid range by the
  narrowing cast rather than caught by it.
- The refusals return **`PRTE_ERR_SILENT`**, because they have already
  shown their own specific `show_help`. Returning a substantive code
  instead makes the module's `error:` label print the generic
  `prte_init:startup:internal-failure` message on top of it. That rule
  holds everywhere in this framework: **a `show_help` and a non-silent
  return code together mean the user gets two messages.**

If you are adding a new RM-launched daemon environment, this is the
function you call — do not re-implement the parse. The per-component
logic is now only *which* environment variable names the node index.

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

1. Installs signal handlers: `SIGPIPE` (ignored) and `SIGTERM`/`SIGINT`
   (→ `shutdown_signal` → `PRTE_JOB_STATE_FORCED_EXIT`). **That is all a
   daemon installs** — see "Signal forwarding is a tool-side feature"
   below.
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
10. Selects `errmgr`; opens/selects `grpcomm`, `odls`. **Not `rmaps`, and
    not `ras`** — both are HNP-only. A daemon never maps and never
    allocates: the mapper is driven by the DVM state machine, which no
    daemon runs, and the launch path does not even forward the `rmaps`
    MCA params out to a `prted`. The base parsers/printers a daemon does
    use (translating a local client's `map-by`/`rank-by` spawn directives
    in `pmix_server_dyn`) are compiled into `libprrte` and need no open
    framework behind them.
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
| `prte_ess_base_signals` | `ess_base_frame.c` | List of `prte_ess_base_signal_t` to forward; populated by `setup_signals` and consumed **by the tools** (`prte.c`, `prun_common.c`) — never by a daemon. |

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

  This cuts both ways, and `prte_ess_base_prted_setup()` used to get it
  wrong in the other direction: steps that deliberately set
  `PRTE_ERR_SILENT` *because they had already shown a better message*
  (`pmix_server_init`, the schizo personality selection) had the generic
  message printed on top of them anyway, because the shared `error:`
  label showed it unconditionally. It now carries the same
  `PRTE_ERR_SILENT != ret && !prte_report_silent_errors` guard every
  module has. **Any new `show_help` on a bring-up path has to be paired
  with a silent return code**, or the user gets two messages for one
  fault.

- **Name a help file with its `.txt`.** `pmix_show_help` resolves the
  file by an exact `strcmp` against the generated table in
  `prte_show_help_content.c`, whose keys are the help files' basenames —
  extension included. A call site that drops it does not fail loudly; it
  resolves to nothing and the user is handed *"Sorry! ... I couldn't find
  that help reference"* instead of the diagnostic. A whole family of
  startup-failure messages, this framework's `std_prolog` among them,
  was unprintable that way. `prte-convert-help.py` now checks every
  citation and fails the build on a mangled name, so this cannot recur
  silently — but write the `.txt`.
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

## Testing

Almost all of `ess` is bring-up by construction — it opens the whole
downstream framework stack, starts the PMIx server, and creates the
session directory — so most of it needs a live DVM and is covered by the
integration/dockerswarm harnesses rather than by a unit test.

### Unit tests — [`test/unit/ess/test_ess.c`](../../../test/unit/ess/)

Two pieces are pure, DVM-free input parsing, and both are covered. Run
them with `make check` (whole suite) or `make -C test/unit/ess check`.

- **`prte_ess_base_set_identity()`** — driven over the shapes of all four
  daemon modules (verbatim vpid, vpid + node index, and LSF's one-based
  index) and, more importantly, over the bad inputs: non-numeric,
  trailing garbage, empty, negative, missing, and sums that fall outside
  the rank space. Each bad case asserts the call is **refused** and that
  it left no rank behind — because the failure this function exists to
  prevent is the quiet one, where garbage reads as rank 0 and the daemon
  adopts the HNP's identity. The out-of-range cases have already earned
  their keep: they caught the range check comparing a value that had
  been narrowed to `pmix_rank_t` first, so a sum past the end of the rank
  space was truncated *into* the valid range.
- **`prte_ess_base_setup_signals()`** — the `"none"` no-op, a table of
  requests that must be refused (unknown name, non-forwardable by name
  *and* by number, non-numeric, negative, zero, out of range, and a bad
  entry inside an otherwise good list), and finally one substantive parse
  asserting each accepted entry carries a real, **non-zero** signal
  number — the value the forwarding callback packs, and the exact field a
  2021 regression got wrong.

  Note the ordering is deliberate: `setup_signals` latches after its
  first *successful* non-`"none"` parse, so the rejections all run first
  and the accepting case runs last.

### Multi-node — `test_ess` in [`contrib/dockerswarm`](../../../contrib/dockerswarm/)

Three things need real daemons:

- **Every daemon derives a distinct identity.** A rank collision is
  silent — the DVM just loses a node — so the case asserts a job mapped
  one-per-node reaches as many *distinct* hosts as there are nodes.
- **A daemon given an unusable identity fails cleanly**, with the
  diagnostic and with no daemon left running.
- **A bad `--forward-signals` request is refused** through a real tool
  invocation, by number as well as by name.

Note what is *not* there: a case that signals a `prted` and expects the
signal to reach a process. Daemons install no handlers for forwarded
signals (see above); such a case would just kill the daemon. The relay
that does exist is covered by `test_event` and `test_prted`.

### The help-file citation check

`prte-convert-help.py` now verifies that every `pmix_show_help` call site
names a help file that exists, and fails the build when one names a PRRTE
file with the extension mangled. That check exists because of this
framework: `ess_base_std_prolog.c` asked for `"help-prte-runtime"`
without the `.txt`, which resolves to nothing. It runs on every
`make check`.

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`hnp/AGENTS.md`](hnp/AGENTS.md) — the DVM master; read this second.
- [`env/AGENTS.md`](env/AGENTS.md) — the generic ssh-launched daemon default.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — daemon under SLURM.
- [`pals/AGENTS.md`](pals/AGENTS.md) — daemon under HPE/Cray PALS.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — daemon under IBM LSF.
