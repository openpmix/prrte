# AGENTS.md — The `prtedl` Framework (Dynamic Loader Abstraction)

Orientation for AI agents and human contributors working in
`src/mca/prtedl/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`prtedl` is a thin, portable abstraction over the platform's dynamic
linker — the four operations `dlopen` / `dlsym` / `dlclose` / `dlerror`,
plus a directory-scanning "for each plugin file" helper. It exists so
that the rest of PRRTE can load a DSO, resolve a symbol in it, and close
it without caring whether the underlying mechanism is the native POSIX
`libdl` or GNU Libtool's `ltdl`.

The header ([`prtedl.h`](prtedl.h)) states the design intent plainly:
this is a **compile-time framework**. Exactly **one** component is
chosen at `configure` time (by priority), and every other component is
excluded from the build entirely — so `prte_prtedl_base_static_components`
holds 0 or 1 entries, never more. This is unlike a normal runtime MCA
framework where several components co-exist and one is selected at
launch.

Historically (inherited from ORTE/Open MPI) this framework backed the
MCA base's own loader for DSO components. Two things are worth knowing
about its role in the *current* PRRTE tree:

- **It must be statically linked and it bootstraps first.** A framework
  whose job is to open DSOs obviously cannot itself be a DSO. The
  framework is declared `PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO` (see
  [`base/prtedl_base_open.c`](base/prtedl_base_open.c)) precisely so the
  MCA base never tries to `dlopen` a component of *this* framework, and
  both components force `COMPILE_MODE = static` in their `configure.m4`.
- **The wrapper API currently has no in-tree callers outside the
  framework itself.** A tree-wide search for `prte_dl_open`,
  `prte_dl_lookup`, `prte_dl_foreachfile`, or `prte_dl_base_select`
  finds only the framework's own code; PRRTE's actual DSO component
  loading is performed by **PMIx's** dl framework (PRRTE reuses PMIx's
  MCA infrastructure — see the top-level guide's "PRRTE's Relationship
  with PMIx"). `prtedl` still builds, registers, and can be opened, but
  treat it as the portable-DL *capability* PRRTE ships rather than a hot
  path. Do not delete or "modernize" it on the assumption it is dead
  without checking with maintainers.

---

## Directory layout

```
prtedl/
  prtedl.h                  # THE contract: handle type, the 4 fn-ptr typedefs,
                            #   component & module structs, version macro
  configure.m4             # framework-level config: STOP_AT_FIRST, --disable-dlopen wiring
  base/
    base.h                  # framework globals + public wrapper prototypes (prte_dl_*)
    prtedl_base_open.c      # framework declare (NO_DSO) + prte_dl_base_open()
    prtedl_base_select.c    # prte_dl_base_select(): pick the single winner
    prtedl_base_close.c     # prte_dl_base_close()
    prtedl_base_fns.c       # prte_dl_open/lookup/close/foreachfile wrappers
    static-components.h      # generated: the 0-or-1 chosen component(s)
  dlopen/                   # native libdl backend  (priority 80) — the usual winner
  libltdl/                  # GNU Libtool ltdl backend (priority 50)
```

Read [`prtedl.h`](prtedl.h) first — it is short and it defines the
entire contract. Then [`base/prtedl_base_fns.c`](base/prtedl_base_fns.c),
which is just as short and shows how the wrappers delegate to the
selected module.

---

## The module contract

The opaque handle type is **declared** in `prtedl.h` and **defined
privately by each component** — the base never sees inside it:

```c
struct prte_dl_handle_t;
typedef struct prte_dl_handle_t prte_dl_handle_t;
```

Each backend defines its own `struct prte_dl_handle_t` (holding a
`void *` libdl handle, or an `lt_dlhandle`, plus an optional cached
filename under `PRTE_ENABLE_DEBUG`). The module allocates the handle in
`open` and frees it in `close`; callers treat it strictly as opaque.

A component fills in a `prte_prtedl_base_module_t` (defined in
`prtedl.h`) — four function pointers:

| Field | Typedef | Purpose |
|-------|---------|---------|
| `open` | `prte_prtedl_base_module_open_fn_t` | dlopen a file, return a handle |
| `lookup` | `prte_prtedl_base_module_lookup_fn_t` | dlsym a symbol in a handle |
| `close` | `prte_prtedl_base_module_close_fn_t` | dlclose a handle and free it |
| `foreachfile` | `prte_prtedl_base_module_foreachfile_fn_t` | scan a path, callback per plugin basename |

Exact signatures (from `prtedl.h`):

```c
int (*open)(const char *fname, bool use_ext, bool private_namespace,
            prte_dl_handle_t **handle, char **err_msg);

int (*lookup)(prte_dl_handle_t *handle, const char *symbol,
              void **ptr, char **err_msg);

int (*close)(prte_dl_handle_t *handle);

int (*foreachfile)(const char *search_path,
                   int (*cb_func)(const char *filename, void *context),
                   void *context);
```

Argument and return conventions the base and both backends honor:

- **`fname == NULL`** to `open` means "open this process" (the main
  program image), not a file on disk.
- **`use_ext`** — if true, the module appends platform-appropriate
  suffixes (`.so`, `.dylib`, `.dll`, `.sl`, …) and tries each; if false,
  it opens exactly the name given.
- **`private_namespace`** — true → open in a *local* namespace
  (`RTLD_LOCAL` / ltdl private); false → *global* (`RTLD_GLOBAL`).
- **`err_msg`** — on failure a module may point this at an internal
  error string. It points to internal storage: **the caller must not
  free or modify it**, and its contents may change on the next `prtedl`
  call. (The two backends differ on ownership details — see their
  guides.)
- **Return value** is `PRTE_SUCCESS` or a `PRTE_ERR*`. `foreachfile`
  visits each *unique* regular, non-Libtool basename once — i.e. `foo.la`
  and `foo.so` collapse to a single callback with `"foo"`; the callback
  stops the walk early by returning non-`PRTE_SUCCESS`.

The component struct (`prte_prtedl_base_component_t`) adds just one field
beyond the MCA base component: an `int priority`. The framework version
macro is `PRTE_DL_BASE_VERSION_1_0_0`
(`PRTE_MCA_BASE_VERSION_3_0_0("prtedl", 1, 0, 0)`).

---

## What `base/` provides

The base is deliberately tiny — four `.c` files, no per-node scratch, no
policy resolution. It is a framework harness plus a set of one-line
delegating wrappers.

### `prte_dl_base_open()` — [`prtedl_base_open.c`](base/prtedl_base_open.c)

Calls `pmix_mca_base_framework_components_open()` to open all (0 or 1)
static components. The file's comment is instructive: the function is
technically unnecessary (a `NULL` open in the framework declare would do
the same), but it exists so this `.o` contains *some* executable code —
otherwise certain linkers (the comment names macOS) may drop the object
file. This file also holds the framework declaration:

```c
PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, prtedl, "Dynamic loader framework",
                                NULL, prte_dl_base_open, NULL,
                                prte_prtedl_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);
```

The `NO_DSO` flag is the bootstrap guarantee: the MCA base must not try
to dlopen components of the framework whose job *is* dlopen.

### `prte_dl_base_select()` — [`prtedl_base_select.c`](base/prtedl_base_select.c)

Calls `pmix_mca_base_select("prtedl", …)` to pick the single
best-priority module, then stashes the winners in the globals
`prte_prtedl_base_selected_component` and `prte_prtedl`. Returns
`PRTE_ERROR` only if no component was selected. Because configure already
narrowed the field to one component, this "selection" is nearly a
formality. (Note: no code in the current tree calls this; the winner is
whatever component survived `configure`.)

### `prte_dl_base_close()` — [`prtedl_base_close.c`](base/prtedl_base_close.c)

Calls `pmix_mca_base_framework_components_close()`. Nothing more.

### The wrapper API — [`prtedl_base_fns.c`](base/prtedl_base_fns.c)

Four public functions, each a null-guarded pass-through to the selected
module's corresponding function pointer, returning
`PRTE_ERR_NOT_SUPPORTED` when no module (or no such fn) is present:

| Wrapper | Delegates to |
|---------|--------------|
| `prte_dl_open()` | `prte_prtedl->open` (sets `*handle = NULL` first) |
| `prte_dl_lookup()` | `prte_prtedl->lookup` |
| `prte_dl_close()` | `prte_prtedl->close` |
| `prte_dl_foreachfile()` | `prte_prtedl->foreachfile` |

These are the entry points callers are meant to use; all are declared in
[`base/base.h`](base/base.h) and marked `PRTE_EXPORT`. The globals
`prte_prtedl` (module), `prte_prtedl_base_selected_component`, and
`prte_prtedl_base_framework` are also declared there.

---

## Component selection (build-time, by priority)

Selection happens in `configure`, not at runtime. The framework's
[`configure.m4`](configure.m4) sets:

```m4
m4_define(MCA_prte_prtedl_CONFIGURE_MODE, STOP_AT_FIRST)
```

`STOP_AT_FIRST` means the highest-priority component whose own
`configure.m4` succeeds wins outright, and the rest are dropped (as
opposed to `STOP_AT_FIRST_PRIORITY`, which would keep ties). Priorities:

```
dlopen 80  >  libltdl 50
```

So on any platform with a working POSIX `libdl` (essentially all of
them), **`dlopen` wins** and `libltdl` is not built. `libltdl` was the
fallback for the rare system that lacked `<dlfcn.h>` but had Libtool's
ltdl. Each component forces static compilation via
`MCA_prte_prtedl_<name>_COMPILE_MODE` → `"static"`.

The framework `configure.m4` also wires up `--disable-dlopen`: when DSO
support is disabled, it forces *all* prtedl components to fail so the MCA
system concludes it cannot build any — and then, if no static component
survived and dlopen was not explicitly disabled, it errors out with a
message suggesting the user install libltdl or pass `--disable-dlopen`.
On success it defines `PRTE_HAVE_DL_SUPPORT` to 1 and, if the winner
added `-L` link flags, appends them to `LD_LIBRARY_PATH` so later
configure run-tests can find the libraries.

---

## Conventions & gotchas

- **Never make this a DSO.** The `NO_DSO` framework flag and the
  per-component `static` compile mode are load-bearing. A DSO-loading
  framework that is itself a DSO is a chicken-and-egg bootstrap failure.
- **The handle is opaque and component-owned.** Only the winning
  component defines `struct prte_dl_handle_t`; the base and callers must
  never dereference it. `open` allocates it, `close` frees it.
- **`err_msg` is borrowed, not owned** by the caller — do not free it.
  (Ownership actually differs between backends; see each guide. Adding a
  caller that frees it would be a bug against at least one backend.)
- **Keep dependencies minimal.** This framework bootstraps very early and
  is meant to be tiny. Do not pull heavyweight PRRTE subsystems into it.
- **`.la`/`.lo` files are skipped** by `foreachfile`, and duplicate
  basenames collapse — that de-duplication contract is what lets callers
  iterate a plugin directory of `foo.la` + `foo.so` and see `foo` once.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`dlopen/AGENTS.md`](dlopen/AGENTS.md) — native POSIX `libdl` backend,
  priority 80, the normal winner. Read this second.
- [`libltdl/AGENTS.md`](libltdl/AGENTS.md) — GNU Libtool `ltdl` backend,
  priority 50, the fallback. **Note:** this component currently carries a
  botched `dl`→`prtedl` symbol rename — see its guide before touching it.
