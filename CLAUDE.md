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

### New files need the standard copyright/license header.

Copy the multi-institution BSD header block — including the `Copyright (c) 2026      Nanook Consulting  All rights reserved.
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


### Stay compiler-warning-free

PRRTE strives to build with zero compiler warnings. Do not introduce
code that adds new warnings. `--enable-devel-check` is implied when
building from a git repo with `--enable-debug`; it instructs the compiler
to treat all warnings as errors and enables maximum warning coverage
(e.g., unused variables). CI runs with this setting, so your code must
be warning-free before submitting.

### No hand-editing of generated files

Do not modify files produced by autotools (`configure`, `Makefile.in`, etc.), pre-rendered documentation, or third-party vendored code. Edit the source code instead.


### Error handling

Functions that can fail return `int`.  Check every return value.  Use
`PRTE_ERROR_LOG(rc)` to record unexpected errors.  Use
`PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_*)` to trigger state
transitions rather than handling errors inline in launch paths.

### Thread model

PRRTE is event-driven and single-threaded on the progress thread.  Use
the PRRTE event loop (`prte_event_base`) for deferred work.  Do not block on
the progress thread.

### Thread-shifting with caddies

A **caddy** is a short-lived heap object whose sole job is to carry a request's parameters across states within the progress thread.  Every caddy struct must contain at minimum:

| Field | Type | Purpose |
|-------|------|---------|
| ev | `pmix_event_t` | Required by Libevent to queue the caddy; **must be named `ev`** |
| lock | `pmix_lock_t` | Thread synchronization (blocking operations wait on this; handlers wake it) |
| cbdata | `void *` | Opaque pointer passed through to the callback |
| callback pointer(s) | function pointer(s) | Cache the caller-supplied callback function(s) |

The pattern:

1. Allocate a caddy with `PMIX_NEW(caddy_type_t)`.
2. Assign the caddy's fields to point at the caller's parameters — **do not copy the data**.
3. Call `PRTE_PMIX_THREADSHIFT(cd, evbase, handler_fn)` to post the caddy to the progress thread's event queue.
4. The progress thread fires `handler_fn(cd)`, which performs the actual work.

Never read or write shared library state outside of the progress thread; do it only inside the handler that runs on the progress thread.
Do not allocate a caddy on the stack — it must outlive the function that creates it.


### Memory management

Use `PMIX_NEW` / `PMIX_RELEASE` (PMIx's reference-counted object system)
for all PRRTE objects that embed `pmix_object_t`.  Raw allocations use
`malloc`/`free` directly — avoid `PMIX_MALLOC`/`PMIX_FREE` for plain
buffers unless interfacing with PMIx routines that require it.

### C standard

PRRTE targets C11.  Do not add `-Wno-*` flags to suppress warnings —
fix the underlying issue.

---

## Build System

PRRTE uses GNU Autotools.  The generated `configure` script is **not** in
the git repository; it is produced by running:

```sh
./autogen.pl
```

This must be re-run whenever `configure.ac`, any `Makefile.am`, or any
`*.m4` file under `config/` is modified.  After `autogen.pl`:

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

## Contributing

### Commit messages

Write prose commit messages, not bullet lists.  The subject line should
complete the sentence "If applied, this commit will …".  The body must
explain **why** the change is needed, not just what it does.  Keep the
subject line under 72 characters.

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

### Testing

PRRTE does not have a standalone unit test suite.  Integration-level
testing is done by running actual parallel jobs through the DVM.  When
modifying launch, mapping, or I/O forwarding paths, test with:

```sh
prte --daemonize                    # start DVM
prun -n 4 hostname                  # basic launch smoke test
pterm                               # shut down DVM
```

For resource manager integration (SLURM, PBS, LSF), test within an actual
allocation on the relevant system.

### Testing the mapper without launching

Most `rmaps` work — mapping policies, ranking, and binding — can be verified
**without launching any daemons or processes**.  `prterun` will compute and
print the complete job map (process-to-node placement, ranks, and CPU
bindings) and then stop, when given the `donotlaunch` runtime option together
with `--display map`:

```sh
prterun --rtos donotlaunch --display map \
        --prtemca hwloc_use_topo_file test/topologies/test-topo.xml \
        -H node0:4,node1:4,node2:4 \
        --map-by node --rank-by node --bind-to core -n 8 hostname
```

This places the 8 procs round-robin across the three simulated nodes (3/3/2),
ranks them by node, and binds each to a core — and prints the result without
launching anything.

What each piece does:

- **`--rtos donotlaunch`** — run the mapper/ranker/binder, display the result,
  and exit.  Nothing is forked or exec'd; no DVM is started.  (`--do-not-launch`
  is a deprecated alias.)
- **`--display map`** — print the resulting map.  Use `--display allocation` or
  `--display map-devel` for more detail.  (`--display-map` is a deprecated
  alias.)
- **`--prtemca hwloc_use_topo_file <path>`** — bind against a *simulated* node
  topology loaded from an hwloc XML file instead of the local machine's
  topology.  Without it, binding is computed against the head node's own
  topology (and `donotlaunch` warns that it could not probe the compute
  nodes).  A ready-made multi-core topology lives at
  `test/topologies/test-topo.xml`; generate others with `lstopo file.xml`.
- **`-H node0:N,node1:M,...`** — declare the simulated nodes and their slot
  counts.  The counts (`N`, `M`, …) only need to be **at least** the number of
  procs you want placed on each node; they bound oversubscription, not the
  topology.  All simulated nodes share the one topology from the XML file.

The printed map shows, per node, each process's `App:` index, `Process rank:`,
and `Bound:` object — exactly the values produced by `src/mca/rmaps` and the
`src/hwloc` binding code — so different `--map-by` / `--rank-by` / `--bind-to`
combinations (and the by-node vs. by-slot rank distinctions that are only
visible across multiple nodes) can be compared directly.  This is the fastest
way to confirm a mapping/ranking/binding change before any integration test.

### Reporting bugs

File issues at https://github.com/openpmix/prrte/issues.  Include the
output of `prte_info --all` and a minimal reproduction case.
