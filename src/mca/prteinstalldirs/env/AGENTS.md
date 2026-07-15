# AGENTS.md — `prteinstalldirs/env` (environment-variable overrides)

Component guide for `src/mca/prteinstalldirs/env/`. Read the
[framework guide](../AGENTS.md) first for the component contract, the
`prte_install_dirs_t` field set, the keep-all-and-merge model, and the
expansion engine referenced throughout.

---

## Role and priority

`env` lets a user or a launch environment **override any install
directory at runtime** by exporting a `PRTE_*` environment variable —
the classic use being a relocated tree (`export PRTE_PREFIX=/new/root`)
or a launcher that stages PRRTE under a job-specific path. Its
configure-time priority is **10** (higher than `config`'s 0), so in the
generated `static-components.h` it is listed first and the framework's
merge visits it **first**. Because the merge is first-non-NULL-wins per
field, any directory `env` supplies **shadows the compiled-in `config`
value**; any it leaves `NULL` falls through to `config`.

Unlike `config`, `env` cannot be a static initializer — the environment
is only knowable at runtime — so it does its work in an
`pmix_mca_open_component` callback.

---

## Files

| File | Contents |
|------|----------|
| `prte_installdirs_env.c` | The component: a mostly-`NULL` `install_dirs_data`, plus `prteinstalldirs_env_open()`, which reads the `PRTE_*` variables into the struct via the `SET_FIELD` macro. |
| `configure.m4` | Sets this component's priority to **10**, forces static compile mode, registers its `Makefile`. |
| `Makefile.am` | Builds `libprtemca_prteinstalldirs_env.la` from the single source file. |

There is no generated header and no `.h.in` — `env` has nothing to bake
in at build time.

---

## How it participates in the merge

The component's `install_dirs_data` is declared with every field
explicitly `NULL` (designated initializers). The real values are filled
by the registered open callback:

```c
.component = {
    PRTE_INSTALLDIRS_BASE_VERSION_2_0_0,
    .pmix_mca_component_name = "env",
    ...
    .pmix_mca_open_component = prteinstalldirs_env_open
},
```

When `pmix_mca_base_framework_components_open()` runs during framework
open, it invokes `prteinstalldirs_env_open()`, which reads each variable:

```c
#define SET_FIELD(field, envname)                                        \
    do {                                                                 \
        char *tmp = getenv(envname);                                     \
        if (NULL != tmp && 0 == strlen(tmp)) {                           \
            tmp = NULL;                                                  \
        }                                                                \
        prte_mca_prteinstalldirs_env_component.install_dirs_data.field = tmp; \
    } while (0)
```

An unset variable yields `NULL` (nothing to override); a variable set to
the empty string is **treated as unset** and also collapses to `NULL`.
So `env` contributes a field only when the corresponding `PRTE_*`
variable is present and non-empty. The component closes with
`PMIX_MCA_BASE_COMPONENT_INIT(prte, prteinstalldirs, env)`.

---

## The field → environment-variable mapping

`prteinstalldirs_env_open()` calls `SET_FIELD` once per field:

| Struct field | Environment variable |
|--------------|----------------------|
| `prefix` | `PRTE_PREFIX` |
| `exec_prefix` | `PRTE_EXEC_PREFIX` |
| `bindir` | `PRTE_BINDIR` |
| `sbindir` | `PRTE_SBINDIR` |
| `libexecdir` | `PRTE_LIBEXECDIR` |
| `datarootdir` | `PRTE_DATAROOTDIR` |
| `datadir` | `PRTE_DATADIR` |
| `sysconfdir` | `PRTE_SYSCONFDIR` |
| `sharedstatedir` | `PRTE_SHAREDSTATEDIR` |
| `localstatedir` | `PRTE_LOCALSTATEDIR` |
| `libdir` | `PRTE_LIBDIR` |
| `includedir` | `PRTE_INCLUDEDIR` |
| `infodir` | `PRTE_INFODIR` |
| `mandir` | `PRTE_MANDIR` |
| `pmixdatadir` | `PRTE_PKGDATADIR` |
| `pmixlibdir` | `PRTE_PKGLIBDIR` |
| `pmixincludedir` | `PRTE_PKGINCLUDEDIR` |

The three package fields keep the framework's naming trap: the
PMIx-named struct fields `pmixdatadir`/`pmixlibdir`/`pmixincludedir` are
driven by the `PRTE_PKG*` variables (PRRTE's package dirs), *not* by any
`PRTE_PMIX*` variable.

An override value may itself contain `${field}`/`@{field}` references
(e.g. `PRTE_BINDIR='${prefix}/bin'`); the framework's expansion pass
resolves them at open time just as it does for `config`'s templates. A
bare relocation like `PRTE_PREFIX=/opt/prte2` works because the other
fields' compiled-in templates reference `${prefix}` and re-expand
against the overridden value.

---

## Gotchas when editing

- **`SET_FIELD` stores the raw `getenv` pointer** into
  `install_dirs_data` — it does **not** `strdup`. This is safe *only*
  because the framework's open path copies the value out with
  `CONDITIONAL_COPY` and then immediately replaces it with a heap copy
  via `prte_install_dirs_expand_setup()` (before anyone frees it). The
  framework's `close()` then `free()`s that owned copy, never the
  `getenv` buffer. If you change the base merge/expand flow, preserve
  this invariant or you will either leak or free `getenv`'s storage.
- **Empty means unset — keep it that way.** The `0 == strlen(tmp)` check
  is deliberate: a launcher that exports `PRTE_LIBDIR=` should not blank
  out the compiled-in libdir. Don't drop it.
- **Designated initializers here, positional in `config`.** `env` names
  each field, so reordering is harmless; but if you add a field, add it
  to both this initializer *and* the `SET_FIELD` list, and mind that
  `config` is positional. Keep the two components' field sets in sync.
- **No expansion happens in this component.** `env` only captures raw
  strings; every `${…}`/`@{…}`/`$PRTE_DESTDIR` transformation is the
  base's job. Don't add expansion logic here.
- **This runs before the MCA param system.** Like the rest of the
  framework, `env` executes at the very start of `prte_init`, so it reads
  the process environment directly with `getenv` rather than any MCA
  parameter machinery.
