# AGENTS.md — The `prteinstalldirs` Framework (Installation Directories)

Orientation for AI agents and human contributors working in
`src/mca/prteinstalldirs/`. This is a map, not the rulebook: the
authoritative project guidance lives in the top-level
[`AGENTS.md`](../../../AGENTS.md) and under [`docs/`](../../../docs/).
When this file and those disagree, **the docs win** — and please fix
this file.

---

## What this framework does

`prteinstalldirs` answers one question: **where was PRRTE installed?**
It populates a single global struct of directory paths — prefix, bindir,
libdir, sysconfdir, and the rest of the GNU/Autotools directory family —
that the rest of the tree consults whenever it needs to find an
installed file (a helper binary, a plugin, a help text, a wrapper-data
file).

The framework's entire public surface is two symbols declared in
[`prteinstalldirs.h`](prteinstalldirs.h):

```c
/* Install directories.  Only available after prte_init() */
PRTE_EXPORT extern pmix_pinstall_dirs_t prte_install_dirs;

/* Expand ${field}/@{field} references in a string using prte_install_dirs */
PRTE_EXPORT char *prte_install_dirs_expand(const char *input);
```

That is the whole API. There is no per-job work, no state machine, no
runtime dispatch. The framework runs its logic **once**, at framework
open, and then `prte_install_dirs` is a read-only global for the life of
the process.

### It initializes extremely early

`prteinstalldirs` is the **first** framework PRRTE opens. In
`src/runtime/prte_init.c`, `pmix_mca_base_framework_open(&prte_prteinstalldirs_base_framework, …)`
is called *before* `pmix_init_util()` — because the very next thing
`prte_init` does is reach for `prte_install_dirs.pmixlibdir` to tell the
MCA infrastructure where PRRTE's plugins live:

```c
if (check_exist(prte_install_dirs.pmixlibdir)) {
    pmix_asprintf(&path, "prte@%s", prte_install_dirs.pmixlibdir);
}
ret = pmix_init_util(NULL, 0, path);
```

Nothing else can work until the install dirs are known, so this
framework has essentially no dependencies and must never grow any.

### Who consumes it

`prte_install_dirs` and `prte_install_dirs_expand()` are read tree-wide.
Current consumers include `src/runtime/prte_init.c`,
`src/runtime/prte_mca_params.c`, `src/tools/prte_info/prte_info.c`,
`src/prted/prte.c`, `src/util/prte_bootstrap.c`, and every `plm`
launcher that has to build a remote `PATH`/`LD_LIBRARY_PATH` or locate
`prted` (`plm/ssh`, `plm/slurm`, `plm/lsf`, `plm/pals`). Treat the field
set and their names as a stable contract — renaming or dropping a field
is a tree-wide break.

---

## Directory layout

```
prteinstalldirs/
  prteinstalldirs.h                    # the two public symbols + the component struct type
  configure.m4                         # sets CONFIGURE_MODE to PRIORITY (order components by priority)
  Makefile.am                          # framework convenience lib; installs prteinstalldirs.h
  base/
    base.h                             # framework-global struct + prte_install_dirs_expand_setup()
    prteinstalldirs_base_components.c  # framework open/close: the MERGE + expansion driver
    prteinstalldirs_base_expand.c      # the ${field}/@{field} + $PRTE_DESTDIR expansion engine
    static-components.h                # generated: lists env then config, priority order
  config/                              # compile-time paths baked in by configure (priority 0)
  env/                                 # PRTE_* environment-variable overrides (priority 10)
```

There is no `*_frame.c` / `*_select.c` split as in bigger frameworks —
the "frame" logic (open/close) and the merge both live in
`prteinstalldirs_base_components.c`, and there is no per-request
selection at all. Read that file and `prteinstalldirs_base_expand.c` and
you have read the framework.

---

## The `prte_install_dirs_t` fields

The struct type is PMIx's `pmix_pinstall_dirs_t` (from PMIx's installed
`pinstalldirs/pinstalldirs_types.h` — PRRTE reuses the PMIx type rather
than defining its own). Every component fills the same fields, and the
base copies them one-for-one. The fields, in struct order, and the
Autotools directory variable each is ultimately derived from:

| Field | Autotools var | GNU meaning |
|-------|---------------|-------------|
| `prefix` | `@prefix@` | install root (e.g. `/usr/local`) |
| `exec_prefix` | `@exec_prefix@` | root for machine-specific files |
| `bindir` | `@bindir@` | user executables (`prte`, `prun`, …) |
| `sbindir` | `@sbindir@` | sysadmin executables |
| `libexecdir` | `@libexecdir@` | programs run by other programs |
| `datarootdir` | `@datarootdir@` | root of read-only arch-independent data |
| `datadir` | `@datadir@` | read-only arch-independent data |
| `sysconfdir` | `@sysconfdir@` | config files (`prte-mca-params.conf`, …) |
| `sharedstatedir` | `@sharedstatedir@` | arch-independent mutable state |
| `localstatedir` | `@localstatedir@` | machine-specific mutable state |
| `libdir` | `@libdir@` | object code / libraries |
| `includedir` | `@includedir@` | user-facing headers |
| `infodir` | `@infodir@` | GNU info files |
| `mandir` | `@mandir@` | man pages |
| `pmixdatadir` | `@prtedatadir@` | **PRRTE's** `$(datadir)/prte` (pkgdatadir) |
| `pmixlibdir` | `@prtelibdir@` | **PRRTE's** `$(libdir)/prte` (pkglibdir) — where plugins live |
| `pmixincludedir` | `@prteincludedir@` | **PRRTE's** `$(includedir)/prte` (pkgincludedir) |

> **Naming trap — the last three fields.** The struct is a PMIx type, so
> its final three fields are literally named `pmixdatadir` / `pmixlibdir`
> / `pmixincludedir`. PRRTE reuses them to hold *its own* package
> directories (`prtedatadir`/`prtelibdir`/`prteincludedir`), i.e. the
> `pkg*` values. This is why `plm` code reads `prte_install_dirs.pmixlibdir`
> to find PRRTE plugins, and why the expansion engine maps the *reference*
> spelling `@{pkglibdir}` onto the *field* `pmixlibdir` (see below). Do
> not "fix" the field names, and do not assume `pmixlibdir` has anything
> to do with the PMIx installation.

---

## The component contract

A `prteinstalldirs` component does **not** expose a module vtable or a
`query`/`map`-style function. The component struct *is* the payload:

```c
struct prte_prteinstalldirs_base_component_2_0_0_t {
    pmix_mca_base_component_t component;      /* standard MCA header  */
    pmix_pinstall_dirs_t      install_dirs_data;  /* the paths it contributes */
};
```

Each component simply publishes a filled (or partially filled)
`install_dirs_data`. A component may leave any field `NULL` to mean "I
have no opinion about this directory." The two components differ only in
*when* and *how* their `install_dirs_data` gets populated:

- **`config`** fills every field at *compile time* — the values are
  `#define`d string literals baked in from `configure` (via the
  generated `config/install_dirs.h`). Its struct is fully static; no
  open function is needed.
- **`env`** fills fields at *open time* from `PRTE_*` environment
  variables, in an `pmix_mca_open_component` callback
  (`prteinstalldirs_env_open`). Any variable that is unset (or set to
  the empty string) leaves that field `NULL`.

The version macro every component uses is
`PRTE_INSTALLDIRS_BASE_VERSION_2_0_0`
(`PRTE_MCA_BASE_VERSION_3_0_0("prteinstalldirs", 2, 0, 0)`).

---

## Selection is "keep all and merge", not "pick one"

Unlike a normal MCA framework, `prteinstalldirs` never selects a single
winner. It opens **all** components and merges their contributions into
the one global `prte_install_dirs`. Two properties make the merge
deterministic:

1. **Priority sets the merge order.** `configure.m4` declares
   `MCA_prte_prteinstalldirs_CONFIGURE_MODE` = `PRIORITY`, so components
   are ordered by their configure-time priority. `env` has priority
   **10**, `config` has priority **0**, so the generated
   `static-components.h` lists them **`env` first, then `config`**:

   ```c
   const pmix_mca_base_component_t **prte_prteinstalldirs_base_static_components[] = {
     &prte_mca_prteinstalldirs_env_component_ptr,
     &prte_mca_prteinstalldirs_config_component_ptr,
     NULL
   };
   ```

2. **First non-NULL wins, per field.** In
   `prte_prteinstalldirs_base_open()` the merge is a `PMIX_LIST_FOREACH`
   over the components in that order, applying `CONDITIONAL_COPY` to each
   field:

   ```c
   #define CONDITIONAL_COPY(target, origin, field)             \
       do {                                                    \
           if (origin.field != NULL && target.field == NULL) { \
               target.field = origin.field;                    \
           }                                                   \
       } while (0)
   ```

   A field is copied only if the origin has a value **and** the target
   slot is still empty. Because `env` is visited first, **a set
   `PRTE_*` environment variable overrides the compiled-in value**, and
   `config` fills in every field `env` left `NULL`. This is the whole
   override story: `env` (high priority, visited first) shadows `config`
   (low priority, the fallback) on a per-field basis.

Because the framework is declared with
`PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER | PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO`,
it registers **no** MCA parameters (there is no
`prteinstalldirs_base_verbose`) and its components are **always** static
— never built as DSOs. That is deliberate: this code runs before the DSO
loader and the MCA param system are even usable.

---

## What `base/` provides in detail

### `prteinstalldirs_base_components.c` — open, merge, expand, close

`prte_prteinstalldirs_base_open()` does three things in order:

1. **Open the components** with
   `pmix_mca_base_framework_components_open()`. This runs each
   component's open callback — importantly, `env`'s
   `prteinstalldirs_env_open()`, which reads the environment into that
   component's `install_dirs_data`.
2. **Merge** — the `CONDITIONAL_COPY` loop described above, one line per
   field, copying `env`-then-`config` contributions into
   `prte_install_dirs`.
3. **Expand** every field in place by calling
   `prte_install_dirs_expand_setup()` on it and storing the returned
   heap string back into the field. After this step each field is either
   `NULL` or a freshly `malloc`'d, fully-resolved absolute path.

> The framework never closes its components after opening (see the `NTH`
> comment in the source): doing so would deregister things unnecessarily,
> and the merged data has already been copied out.

`prte_prteinstalldirs_base_close()` `free()`s every field of
`prte_install_dirs` and `memset`s the struct back to zero, then closes
the components. This is safe precisely because step 3 replaced any
borrowed pointers (notably `env`'s raw `getenv` results) with owned heap
copies — see the `env` component guide.

### `prteinstalldirs_base_expand.c` — the reference/variable engine

This file implements the `${field}` / `@{field}` expansion pass. The
public entry points:

| Function | `PRTE_DESTDIR`? | Used when |
|----------|-----------------|-----------|
| `prte_install_dirs_expand(input)` | **no** | normal runtime expansion (the exported API) |
| `prte_install_dirs_expand_setup(input)` | **yes** | only during framework setup (declared in `base.h`) |

Both are thin wrappers over `prte_install_dirs_expand_internal(input,
is_setup)`. The algorithm:

1. **Optional DESTDIR offset.** If `is_setup` and the environment has a
   non-empty `$PRTE_DESTDIR`, record `destdir_offset = strlen(destdir)`.
   (Explained below.)
2. **Fast bail.** Scan the input for any `$` or `@`. If there is none,
   there is nothing to expand — skip straight to the DESTDIR step.
3. **Fixed-point substitution loop.** Repeatedly run the `EXPAND_STRING`
   macro for every field until a full pass makes no change (`while
   (changed)`). Each `EXPAND_STRING(name)` looks for **both** `${name}`
   and `@{name}` in the working string and, on the first occurrence,
   splices in `prte_install_dirs.name + destdir_offset`. The loop repeats
   so that a field whose value itself contains another `${…}` reference
   (e.g. `bindir = ${exec_prefix}/bin`) is resolved transitively.
4. **DESTDIR prepend.** If a DESTDIR was captured, the whole result is
   re-rooted under it with `pmix_os_path(false, destdir, retval, NULL)`.

The `+ destdir_offset` in step 3 is the subtle part. When staging an
install under `$PRTE_DESTDIR` (e.g. a distro build root), each field's
*value* has already been DESTDIR-prefixed during the merge/expand of the
individual directories. When one field references another (case 2 in the
source's long comment — a wrapper-compiler flag like
`-DFOO="${prefix}/share/x"`), splicing the already-prefixed value in
verbatim would double the DESTDIR. Skipping `destdir_offset` bytes drops
the leading DESTDIR from the substituted value, and the single
DESTDIR-prepend in step 4 puts exactly one back. See the extensive
comment block in the source for the three worked cases.

### Two spellings: `${field}` and `@{field}`

Both forms are accepted and mean the same thing. The `@{…}` form exists
so a value can survive being passed through Autotools `AC_SUBST` without
`m4` prematurely expanding a shell-style `${…}`. Prefer `@{…}` in
anything that flows through the build system, `${…}` elsewhere.

### Reference spellings vs. field names

Most references match the field name directly (`EXPAND_STRING(prefix)` →
field `prefix`). The three package directories do **not** — the macro
`EXPAND_STRING2(fieldname, refname)` maps a *reference spelling* onto a
*different field*:

```c
EXPAND_STRING2(pmixdatadir,    pkgdatadir);     /* @{pkgdatadir}    -> field pmixdatadir    */
EXPAND_STRING2(pmixlibdir,     pkglibdir);      /* @{pkglibdir}     -> field pmixlibdir     */
EXPAND_STRING2(pmixincludedir, pkgincludedir);  /* @{pkgincludedir} -> field pmixincludedir */
```

So in a path string you write `@{pkglibdir}` (the GNU `pkglibdir`
spelling), and it resolves from the `pmixlibdir` field. There is no
`@{pmixlibdir}` reference and no `@{prtelibdir}` reference — only
`@{pkglibdir}`. This mirrors the field-naming trap noted above.

---

## Conventions and gotchas

- **This code runs before everything.** No MCA params, no DSOs, no
  `PRTE_ERROR_LOG` (the errmgr is not up yet — the open path uses bare
  `fprintf(stderr, …)` on failure). Keep new dependencies out.
- **`prte_install_dirs` is read-only after open.** Never write to it
  outside `prte_prteinstalldirs_base_open()`. Consumers only read.
- **Fields can be `NULL`.** A directory nobody configured and nobody
  overrode is `NULL`, not `""`. Consumers must null-check (e.g.
  `check_exist(prte_install_dirs.pmixlibdir)` in `prte_init`).
- **Do not add a field on one side only.** The field list is repeated in
  five places that must stay in lockstep: the struct (PMIx's type), the
  merge (`CONDITIONAL_COPY` block), the expand-in-place block, the
  `close()` `free` block, and both components' `install_dirs_data`. A
  new directory means touching all of them (and the PMIx struct).
- **Don't hand-edit generated files.** `config/install_dirs.h` (from
  `install_dirs.h.in`) and `base/static-components.h` are generated —
  change `install_dirs.h.in` or the priorities, not the outputs.
- **Priority order is load-bearing.** `env` must stay higher-priority
  than `config` or environment overrides stop working. If you add a
  component, pick its priority relative to those two deliberately.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, constant-on-left comparisons, no new compiler warnings.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`config/AGENTS.md`](config/AGENTS.md) — compile-time baked-in paths
  (the fallback; priority 0).
- [`env/AGENTS.md`](env/AGENTS.md) — `PRTE_*` environment-variable
  overrides (priority 10, wins over `config`).
