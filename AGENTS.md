# AGENTS.md — Orientation for AI Coding Agents and Human Contributors

This document is an orientation guide for AI coding agents and their human operators working in the PRRTE code base.
This file is an *orientation map*, not the
full rulebook: the authoritative, human-maintained documentation lives
under [`docs/developers/`](docs/developers/) and
[`docs/contributing.rst`](docs/contributing.rst) (rendered at
<https://docs.prrte.org/>). When this file and those docs disagree,
**the docs win** — and please fix this file.

AI-assisted contributions are welcome. But PRRTE runs on the largest
supercomputers in the world and across a huge range of operating
systems and hardware. It is also used by numerous research groups
looking at developments such as elastic environments. We want careful,
portable code — not plausible-looking code that solves one problem in
one environment or use-case at the expense of others. Hold yourself to
the same bar as a thoughtful human contributor.

> **Note on layout:** PRRTE and PMIx are developed together, and many
> contributors work in both trees.  This guide deliberately follows the
> same section order as the companion
> [PMIx `AGENTS.md`](https://github.com/openpmix/openpmix/blob/master/AGENTS.md)
> so that a rule you learned in one project sits in the same place in the
> other.

---

## What is PRRTE?

PRRTE (PMIx Reference RunTime Environment) is an open-source, production
runtime system for launching and managing parallel jobs in HPC environments.
Unlike a library, PRRTE exposes no public API for callers — it is a
collection of executable tools that users invoke directly.  PRRTE acts as a
reference implementation of the PMIx-defined runtime services, and it relies
heavily on the PMIx project for its internal infrastructure: MCA
(Modular Component Architecture), utility routines, data structures, and
threading primitives.  Many PMIx symbols and headers PRRTE uses are internal
PMIx interfaces, **not** the PMIx public library API.

Source repository: https://github.com/openpmix/prrte
Documentation: https://docs.prrte.org/

---

## Terminology

Understanding PRRTE's vocabulary is essential before reading or modifying
code.

| Term | Meaning |
|------|---------|
| **DVM** | Distributed Virtual Machine — a persistent set of PRRTE daemons spread across an allocation, ready to launch jobs on demand. |
| **HNP** | Head Node Process — the `prte` process that acts as the DVM controller.  Also called the DVM master. |
| **prted** | PRRTE daemon — one instance runs on each node in the DVM and manages local processes. |
| **job** | A set of processes launched together under a single `prun` invocation.  Each job has a unique namespace. |
| **namespace** | A string that uniquely identifies a job within a DVM session (inherited from PMIx). |
| **rank** | A process's integer identifier within its namespace (global rank), within its node (node rank), or within the local set of processes on a node (local rank). |
| **app context** | A description of one executable to run: path, argv, environment, and process count.  A single `prun` may specify multiple app contexts. |
| **session** | The lifetime of a DVM — from `prte` startup through `pterm` shutdown. |
| **schizo** | The "personality" framework.  Schizo components translate between PRRTE's internal model and the command-line conventions of specific launchers (e.g., Open MPI's `mpirun`, SLURM's `srun`). |
| **MCA** | Modular Component Architecture — the plugin system, inherited from PMIx, that PRRTE uses for every swappable subsystem. |
| **framework** | An MCA abstraction layer that defines the interface for one functional area (e.g., `plm`, `rmaps`). |
| **component** | A plugin that implements a framework's interface for a specific environment or algorithm (e.g., `plm/slurm`). |
| **module** | The active instance of a component, returned after it wins selection. |

---

## User-Facing Tools

PRRTE provides no linkable library.  All user interaction is through
executables.

| Tool | Source | Purpose |
|------|--------|---------|
| `prte` | `src/tools/prte/` | Start a DVM (the HNP process).  Allocates and connects daemons across an allocation. |
| `prun` | `src/tools/prun/` | Submit and manage a job within a running DVM.  The everyday launch command. |
| `pterm` | `src/tools/pterm/` | Terminate a running DVM cleanly. |
| `prted` | `src/tools/prted/` | The per-node daemon, normally launched by the HNP — not invoked directly by users. |
| `prte_info` | `src/tools/prte_info/` | Query build configuration, list available MCA components, and display version information. |
| `pcc` | `src/tools/pcc/` | Compiler wrapper for applications that directly embed PMIx (not PRRTE — PRRTE has no linkable library). |

---

## Source Layout

```
src/
  tools/          # Executable entry points (prte, prun, pterm, prted, prte_info, pcc)
  mca/            # All MCA frameworks and their components
  runtime/        # Global state, init/finalize, job/node/proc data structures
  rml/            # Runtime Messaging Layer (point-to-point communication between daemons)
  util/           # Internal utilities (hostfile parsing, name formatting, attributes, ...)
  include/        # Internal header files (types.h, constants.h, ...)
  pmix/           # Thin shim connecting PRRTE to its PMIx dependency
  event/          # Libevent integration
  hwloc/          # hwloc integration (topology, binding)
```

The three central data structures — `prte_job_t`, `prte_node_t`, and
`prte_proc_t` — are defined in `src/runtime/prte_globals.h` and carry
all runtime state for a running job.  Code throughout the tree reaches
for these; understand them before touching launch, mapping, or error
handling paths.

---

## MCA Frameworks

Every swappable PRRTE subsystem is an MCA framework.  The framework
directory contains a `base/` subdirectory (selection, default behaviors,
utility stubs) and one or more component subdirectories.

| Framework | Location | Responsibility |
|-----------|----------|----------------|
| `ess` | `src/mca/ess/` | Environment-Specific Services — init/finalize for each process role (HNP, daemon, tool). |
| `plm` | `src/mca/plm/` | Process Launch Manager — spawns prted daemons across the system. Components: `ssh`, `slurm`, `lsf`, `pals`. |
| `ras` | `src/mca/ras/` | Resource Allocation Subsystem — discovers nodes and slots. Components: `slurm`, `pbs`, `lsf`, `flux`, `gridengine`, `hosts`. |
| `rmaps` | `src/mca/rmaps/` | Resource Mapping — assigns processes to nodes/slots. Components: `round_robin`, `ppr`, `rank_file`, `seq`. |
| `odls` | `src/mca/odls/` | PRRTE Daemon Local Launch Subsystem — the per-node daemon (prted) forks/execs application processes. |
| `iof` | `src/mca/iof/` | I/O Forwarding — routes stdout/stderr/stdin between daemons and the HNP. |
| `grpcomm` | `src/mca/grpcomm/` | Group Communication — collective operations among daemons (broadcast, barrier). |
| `errmgr` | `src/mca/errmgr/` | Error Manager — handles process faults, abnormal exits, and propagation of errors. |
| `state` | `src/mca/state/` | State Machine — drives the DVM and job lifecycle through defined states/transitions. |
| `schizo` | `src/mca/schizo/` | Personality layer — parses CLI options and environment for specific launcher personalities (prte, ompi). |
| `filem` | `src/mca/filem/` | File Management — pre-positions files across nodes before launch. |
| `prtereachable` | `src/mca/prtereachable/` | Reachability — determines which network interfaces can reach each node. |
| `prtebacktrace` | `src/mca/prtebacktrace/` | Backtrace support for crash diagnostics. |
| `prtedl` | `src/mca/prtedl/` | Dynamic linker abstraction (dlopen/dlclose). |
| `prteinstalldirs` | `src/mca/prteinstalldirs/` | Installation directory queries. |

### MCA component structure

Each component directory must contain:
- `<component>.h` — component struct (`prte_<fw>_<name>_component_t`)
- `<component>.c` — component registration, open, query, close
- `<name>_module.c` (or similar) — module implementation
- `Makefile.am`

The component's `query` function returns a priority; the highest-priority
component that successfully opens wins selection.

---

## PRRTE's Relationship with PMIx

PRRTE depends on PMIx (minimum version `6.1.0`) and uses PMIx internals
extensively.  This is not the same as calling the PMIx public library API.

**What PRRTE takes from PMIx:**

- **MCA infrastructure** (`src/mca/base/`, `src/mca/mca.h`) — the entire
  plugin/component system is PMIx's code, not PRRTE's.
- **Data structures and classes** — `pmix_list_t`, `pmix_pointer_array_t`,
  `pmix_hash_table_t`, `pmix_ring_buffer_t`, `pmix_value_array_t`.
- **Threading primitives** — `pmix_mutex_t`, `pmix_condition_t`,
  `pmix_threads_*`.
- **Utility functions** — `pmix_argv_*`, `pmix_environ_*`,
  `pmix_output_*`, `pmix_cmd_line_*`.
- **Event loop** — PMIx's libevent integration and progress threads.
- **Data packing/unpacking** — PMIx's buffer and bfrops subsystem.

Include paths for these symbols come from PMIx's installed headers (found
via `pkg-config` or `--with-pmix=`).  The shim at `src/pmix/pmix-internal.h`
and `src/pmix/pmix.c` is PRRTE's integration point — consult it before
reaching for PMIx symbols elsewhere.

### Check capability flags

PRRTE uses the `AC_PREPROC_IFELSE` idiom in its configure script to test for PMIx capability flags at build time.  PRRTE's `config/prte_setup_pmix.m4` defines a helper macro `PRTE_CHECK_PMIX_CAP` that reduces the check to:

```m4
PRTE_CHECK_PMIX_CAP([MY_NEW_FEATURE],
    [action if supported],
    [action if not supported])
```

The macro succeeds when `PMIX_CAP_MY_NEW_FEATURE` is defined in the installed `pmix_version.h`; it fails (and triggers the third argument) when the definition is absent, meaning the running PMIx predates the feature.

Code requiring a specific PMIx feature that is covered by a capability flag must be protected by an appropriate `#if FOO` clause.

---

## Coding Rules

These rules apply to **all** PRRTE C source files without exception.

### Mandatory header order

`prte_config.h` **must be the first `#include`** in every `.c` file.
Nothing comes before it — not system headers, not other PRRTE headers.

```c
#include "prte_config.h"

#include <stdio.h>          /* system headers follow */
...
#include "src/util/name_fns.h"  /* then PRRTE/PMIx headers */
```

### Symbol prefixes

| Scope | Prefix |
|-------|--------|
| Exported (tools, public data) | `PRTE_` (macros) / `prte_` (functions, types) |
| Internal only | `prte_` with no `PRTE_EXPORT` annotation |
| MCA component | `prte_<framework>_<component>_` |

Do not use `PMIX_` or `pmix_` prefixes for new PRRTE symbols.

Use `PRTE_EXPORT` to annotate symbols that must be visible outside the
compilation unit that defines them (for example, functions a tool or
another framework calls).  Leave purely internal symbols unannotated —
do not mark them `PRTE_EXPORT`.

### New files need the standard copyright/license header.

Copy the multi-institution BSD header block — including the `$COPYRIGHT$` and
`$HEADER$` tokens — from a neighboring file. If you substantially
change an existing file, add your copyright line to its block.

### `#define` logical macros to `0` or `1`; never `#undef` them.

Test with `#if FOO`, not `#ifdef FOO`, so a misspelling is a compiler
error, not a silent false.

### Constant-on-left comparisons

Always put the constant or NULL on the left side of an equality test:

```c
if (NULL == ptr)    /* correct */
if (ptr == NULL)    /* wrong — easy to mistype as assignment */

if (PRTE_SUCCESS == rc)   /* correct */
```

### Always brace blocks

Use `{ }` around every conditional or loop body, even single-line ones.

### Indentation

4 spaces, never tab characters.

### Spacing on conditional statements

Use a space to separate the condition from the surrounding keywords —
write `if (condition) {`, not `if(condition){`.  When a condition spans
multiple lines, put the combining operator at the end of the preceding
line:

```c
if (condition1 ||
    condition2) {
```

### Stay compiler-warning-free

PRRTE strives to build with zero compiler warnings. Do not introduce
code that adds new warnings. `--enable-devel-check` is implied when
building from a git repo with `--enable-debug`; it instructs the compiler
to treat all warnings as errors and enables maximum warning coverage
(e.g., unused variables). CI runs with this setting, so your code must
be warning-free before submitting.

### No hand-editing of generated files

Do not modify files produced by autotools (`configure`, `Makefile.in`, etc.), pre-rendered documentation, or third-party vendored code. Edit the source code instead.

### Update `.gitignore` for build products you introduce

If your change adds a new source file, component, or generated artifact
that the build produces something new from — a new executable or test
binary, a newly generated source/header, an object or library in a
directory that did not have one before — add the resulting build product
to the appropriate `.gitignore` so it does not show up as an untracked
file.  Never commit the build product itself; ignore it.  Check
`git status` after a clean build to confirm no generated file you created
is left untracked, and match the nearest existing `.gitignore` pattern
style (many component directories carry their own `.gitignore`).

### GOLDEN RULE: regenerate `show_help` content after touching any help file

The `show_help` messages are embedded into the binary from a generated
source file, `src/util/prte_show_help_content.c` (and its `src/include/`
companion), produced by `src/util/prte-convert-help.py` from the
`help-*.txt` files scattered across the tree.  The Make rule that builds
this generated file depends only on the converter script — **not** on the
individual `help-*.txt` files — so an ordinary `make` will **not** pick up
changes to help content.

Therefore, after **any** add, delete, or modification of `show_help`
content — including adding a brand-new `help-*.txt` file — you **must**
force the generated source to be rebuilt:

```sh
rm src/util/prte_show_help_content.*
make
```

Removing the stale generated file forces `make` to re-run the converter
and pick up your help-text changes.  Skipping this step leaves the binary
serving the old (or missing) messages even though the `.txt` source looks
correct.

### Unique numeric values for status and state codes

Error constants, job states, and process states are hand-assigned
numeric values, and every value must be **unique** within its family — a
duplicate silently makes two distinct codes compare equal and is a
miserable bug to track down.

- Error/return codes live in [`src/include/constants.h`](src/include/constants.h),
  numbered as offsets from `PRTE_ERR_BASE` and `PRTE_ERR_SPLIT`.
- Job and process states live in
  [`src/mca/plm/plm_types.h`](src/mca/plm/plm_types.h), numbered as
  offsets from `PRTE_JOB_STATE_ERROR` and `PRTE_PROC_STATE_ERROR` (note
  the sequences already skip some offsets — do not reuse a skipped one
  assuming it is free).

When adding a code, append it at the end of its list with the next
unused offset, and grep the relevant header to confirm no existing entry
already claims that value.

### Error handling

Functions that can fail return `int`.  Check every return value.  Use
`PRTE_ERROR_LOG(rc)` to record unexpected errors.  Use
`PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_*)` to trigger state
transitions rather than handling errors inline in launch paths.

### Memory management

Use `PMIX_NEW` / `PMIX_RELEASE` (PMIx's reference-counted object system)
for all PRRTE objects that embed `pmix_object_t`.  Raw allocations use
`malloc`/`free` directly — avoid `PMIX_MALLOC`/`PMIX_FREE` for plain
buffers unless interfacing with PMIx routines that require it.

### C standard

PRRTE targets C11.  Do not add `-Wno-*` flags to suppress warnings —
fix the underlying issue.  C++-style `//` comments are allowed and
preferred.

### Use the `__prte_attribute_*__` macros for compiler attributes.

[`src/include/prte_config_bottom.h`](src/include/prte_config_bottom.h),
pulled in transitively by `prte_config.h`, defines portable wrappers —
`__prte_attribute_unused__`, `__prte_attribute_noreturn__`,
`__prte_attribute_format__`, `__prte_attribute_deprecated__`, and many
more — that expand to the appropriate `__attribute__((...))` on
compilers that support it and to nothing elsewhere. Reach for these
(for example, to mark an unused function parameter) rather than writing
a bare `__attribute__` or leaving a warning unaddressed.

---

## Build & Test Procedures

PRRTE uses GNU Autotools.  The generated `configure` script is **not** in
the git repository; it is produced by running:

```sh
./autogen.pl
```

This must be re-run whenever `configure.ac` or any `*.m4` file under
`config/` is modified or added.  Editing a `Makefile.am` does **not** require the
full `autogen.pl` + `./configure` cycle — PRRTE builds in maintainer
mode, so a plain `make` regenerates the affected `Makefile[.in]` files and
completes the build.  After `autogen.pl`:

```sh
./configure [options]
make -j$(nproc)
make install
```

Common configure options:

| Option | Purpose |
|--------|---------|
| `--with-pmix=<path>` | Path to PMIx installation (required if not in default search path) |
| `--with-hwloc=<path>` | Path to hwloc installation |
| `--with-libevent=<path>` | Path to libevent installation |
| `--with-slurm` | Enable SLURM support |
| `--with-lsf=<path>` | Enable LSF support |
| `--with-pbs` | Enable PBS/Torque support |
| `--with-flux=<dir>` | Enable Flux support |
| `--enable-debug` | Build with debug symbols and extra assertions |
| `--enable-devel-check` | Enable strict compiler warnings (treat warnings as errors); on by default when `--enable-debug` is used in a git repo build |

Version requirements: PMIx ≥ 6.1.0, hwloc ≥ 2.1.0, libevent ≥ 2.0.21.

### Test-building your changes

Build from the repository root with `make -j$(nproc)`.  Running `make`
from the root is what respects the generated headers and the per-target
compiler flags; building from deep inside a subdirectory can miss a
regenerated header and give you a misleading result.

If you configured with `--enable-mca-dso` (components built as separate
DSOs rather than statically linked into the tools), you can rebuild a
single component after editing it by running `make install` in that
component's build directory — you do not have to relink every tool.  In
the default static build, a component change requires a normal
root-level `make` so the tools are relinked.

### Modifying the configure / build system

Editing the build system means regenerating it — `make` alone can't, and
trying will wedge the tree.  If you change `configure.ac` or any
`config/*.m4` file (including the embedded oac/Autotools macros), the
change does not take effect until the build system is regenerated.  Do
not rely on a plain `make`: PRRTE builds in maintainer mode, so `make`
auto-triggers a partial in-tree Autotools regeneration that frequently
fails (e.g., unexpanded `OAC_*` macros, `config.status` errors) and can
leave the tree half-regenerated and unbuildable.  Instead, regenerate and
reconfigure explicitly:

```sh
./autogen.pl
./configure <same options as the original configure>
make -j
```

Recover the original configure invocation options from the existing tree
with `./config.status --config` (or read the header of `config.log`).
This process is slow but mandatory after any build-system source change —
there is no safe shortcut.  As noted above, editing a `Makefile.am` alone
is the exception: a plain `make` regenerates the relevant `Makefile[.in]`
files and completes the build without the full cycle.

### Testing

PRRTE now ships real automated tests in addition to integration-level
launch testing.  Use the right layer for what you touched.

**Unit tests (`make check`).**  Self-contained unit tests live under
[`test/unit/`](test/unit/) and are wired into `make check` (for example,
`test/unit/rmaps/test_rmaps` exercises the mapper's policy parsing,
option resolution, and each mapping component — `round_robin`, `ppr`,
`seq`, `rank_file` — with no live DVM).  Run them from the build tree:

```sh
make check
```

Add new unit tests here, under the framework they cover, and wire them
into the appropriate `Makefile.am` `TESTS =` list so `make check` picks
them up.

**Offline mapper harness (`make check-offline`).**  Mapping, ranking, and
binding behavior can be exercised without launching anything.
[`test/offline/run_offline_maps.py`](test/offline/) drives
`prterun --rtos donotlaunch --display map` over a matrix of `--map-by`,
`--rank-by`, and `--bind-to` directives crossed with the synthetic hwloc
topologies in [`test/topologies/`](test/topologies/), then checks each
printed map against invariants derived from the topology.  It is **not**
part of `make check` (it needs a freshly built `prterun` and runs well
over a thousand cases); run it on demand from a build tree:

```sh
make -C test/offline check-offline
```

Whenever you change the mapper, run this harness — it is the cheapest way
to catch a mapping regression.

**Integration testing.**  For launch, I/O forwarding, and lifecycle
changes, run an actual DVM:

```sh
prte --daemonize                    # start DVM
prun -n 4 hostname                  # basic launch smoke test
pterm                               # shut down DVM
```

For resource-manager integration (SLURM, PBS, LSF), test within an actual
allocation on the relevant system.

**Never bend a test to accommodate a bug.** Do not weaken, skip, or
rewrite an existing test — and do not craft a new one — merely to make
buggy behavior pass.  Tests encode intended behavior: when one fails, the
default assumption is that the code is wrong, not the test.  If you find a
genuine bug in the code base, identify it, report it, and where
appropriate fix it — don't paper over it in the test suite.

### Did I break it? — verification checklist

Before you consider a change finished, work down this list; do the steps
that apply to what you touched:

1. **Clean, warning-free build.**  Configure with `--enable-debug` (which
   turns warnings into errors) and confirm the tree builds clean — pay
   special attention to conditionally-compiled paths (code behind a
   capability flag or an RM `--with-*` option) that your local build may
   not even be exercising.
2. **`make check`.**  The unit tests must pass.
3. **Offline mapper harness** (`make -C test/offline check-offline`) for
   any change to mapping, ranking, or binding.
4. **Live smoke test.**  Start a DVM, launch a small job, and shut it down
   (`prte --daemonize` → `prun -n 4 hostname` → `pterm`) for any change to
   launch, I/O forwarding, or the state machine.
5. **Docs build.**  For user-visible changes, update the RST under
   [`docs/`](docs/) and build the docs (`make` in `docs/` produces the
   Sphinx HTML) to confirm they render warning-free.
6. **Broaden when feasible.**  Where you can, repeat across environments
   and resource managers — PRRTE's whole reason for existing is
   portability across systems you may not have in front of you.

---

## Thread Safety & the Progress Thread

### Thread model

PRRTE is event-driven and single-threaded on the progress thread.  Use
the PRRTE event loop (`prte_event_base`) for deferred work.  Do not block on
the progress thread.

### Thread-shifting with caddies

A **caddy** is a short-lived heap object whose sole job is to carry a request's parameters across states within the progress thread.  Every caddy struct must contain at minimum:

| Field | Type | Purpose |
|-------|------|---------|
| ev | `pmix_event_t` | Required by Libevent to queue the caddy; **must be named `ev`** |
| lock | `prte_pmix_lock_t` | Thread synchronization (blocking operations wait on this; handlers wake it) |
| cbdata | `void *` | Opaque pointer passed through to the callback |
| callback pointer(s) | function pointer(s) | Cache the caller-supplied callback function(s) |

The pattern:

1. Allocate a caddy with `PMIX_NEW(caddy_type_t)`.
2. Assign the caddy's fields to point at the caller's parameters — **do not copy the data**.
3. Call `PRTE_PMIX_THREADSHIFT(cd, evbase, handler_fn)` to post the caddy to the progress thread's event queue.
4. The progress thread fires `handler_fn(cd)`, which performs the actual work.

Never read or write shared library state outside of the progress thread; do it only inside the handler that runs on the progress thread.
Do not allocate a caddy on the stack — it must outlive the function that creates it.

#### Blocking operations

When the caller must wait for the work to finish, embed a
`prte_pmix_lock_t` and block on it after the thread-shift.  The handler
wakes the caller by calling `PRTE_PMIX_WAKEUP_THREAD` on the same lock
before it returns:

```c
prte_pmix_lock_t lock;
PRTE_PMIX_CONSTRUCT_LOCK(&lock);
cd->lock = &lock;                                  /* caddy carries the lock */
PRTE_PMIX_THREADSHIFT(cd, prte_event_base, _do_the_work);
PRTE_PMIX_WAIT_THREAD(&lock);                      /* block until woken */
rc = lock.status;
PRTE_PMIX_DESTRUCT_LOCK(&lock);
```

The handler running on the progress thread does its work, records the
result in `lock->status`, and finishes with `PRTE_PMIX_WAKEUP_THREAD(cd->lock)`.

#### Non-blocking operations

When the caller supplies a callback and must not block, thread-shift and
return immediately:

```c
PRTE_PMIX_THREADSHIFT(cd, prte_event_base, _do_the_work);
return PRTE_SUCCESS;
```

Here the handler performs the work, invokes the caller's callback, and
releases the caddy with `PMIX_RELEASE(cd)` — there is **no**
`PRTE_PMIX_WAKEUP_THREAD` because no one is waiting on a lock.

---

## Performance Considerations

PRRTE launches and manages jobs at extreme scale, so the daemon startup,
mapping, and collective paths are performance-sensitive.

- Do not add allocations, locks, or branches to hot paths (launch,
  mapping, RML message handling, collectives) without a measured
  justification.
- Guard verbose debug output and expensive assertions behind
  `#if PRTE_ENABLE_DEBUG` (or a framework verbosity check) so they cost
  nothing in a production build.
- Prefer an MCA parameter over a hard-coded constant for any value that
  might need tuning across different systems or scales.

---

## State Machines

PRRTE drives both the DVM lifecycle and individual job lifecycles through
explicit state machines defined in `src/mca/state/`.  Process and job
states are declared in `src/mca/state/state_types.h`.  When debugging
launch or termination problems, enable framework verbosity to trace
state transitions:

```sh
prte --prtemca state_base_verbose 5 ...   # trace job state transitions
prte --prtemca plm_base_verbose 5 ...     # trace daemon launch
prte --prtemca rmaps_base_verbose 5 ...   # trace process mapping
```

Key job states in order: `PRTE_JOB_STATE_INIT` →
`PRTE_JOB_STATE_ALLOCATE` → `PRTE_JOB_STATE_MAP` →
`PRTE_JOB_STATE_LAUNCH_DAEMONS` → `PRTE_JOB_STATE_RUNNING` →
`PRTE_JOB_STATE_TERMINATED`.

---

## Working in a shared repository

Don't assume you're the only agent (or person) using this clone.  In
particular, if you're working in a **git worktree**, other worktrees may
be active against the same underlying repository at the same time.  Avoid
repo-wide git commands that reach outside your own working area and can
disrupt others — for example, `git worktree prune`, or `git stash`
(which writes to the repository-wide stash ref shared by all worktrees).
Keep your git operations scoped to your own branch and worktree.

As a narrow exception, creating a **new branch** when you need to park
work in progress (for example, instead of `git stash`) is fine.  Just be
careful not to collide with branches that other agents or people may be
using in the same clone — pick a clearly-scoped, unlikely-to-clash name.

---

## Contributing

### Commit messages

Write prose commit messages, not bullet lists.  The subject line should
complete the sentence "If applied, this commit will …".  The body must
explain **why** the change is needed, not just what it does.  Keep the
subject line under 72 characters, and wrap body lines at around 75
characters.  Don't add AI tooling attribution to commit messages.

Keep incidental fixes as their own commits.  Small "drive-by" bug fixes
you notice while working on something else are welcome, but it is usually
best to land them as standalone commits, separate from your main change,
so each can be evaluated and reviewed on its own.  One logical change per
commit keeps history reviewable and easy to bisect.

All commits require a `Signed-off-by:` line (DCO):

```
git commit -s
```

### Pull requests

- Open PRs against the `master` branch (development trunk).
- One logical change per PR.  Split unrelated fixes.
- Describe the problem being solved in the PR description, not just the
  solution.
- Documentation updates (`docs/`) are required for user-visible changes.

**Never push a branch to `origin`.** The `origin` remote is the shared
upstream repository (`openpmix/prrte`); pushing topic branches there is not
the project workflow.  Always push your branch to your personal fork remote
instead, and open the pull request from the fork against the upstream
`master` (or the appropriate release branch).  If you are unsure which remote
is the fork, run `git remote -v` and ask rather than guessing.

### Reporting bugs

File issues at https://github.com/openpmix/prrte/issues.  Include the
output of `prte_info --all` and a minimal reproduction case.

---

## General Guidance

When in doubt:

- Match the style and conventions of the surrounding code.
- Read the relevant pages under [`docs/developers/`](docs/developers/)
  before inventing a new pattern — the mechanism you need often already
  exists.
- For significant work, raise a GitHub issue to discuss the approach
  before implementing it.

PRRTE runs on the world's largest supercomputers across a wide range of
operating systems and hardware.  The project values careful, portable
code — not plausible-looking solutions that work in one environment or
use-case at the expense of others.
