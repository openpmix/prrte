# AGENTS.md — `prtedl/libltdl` (GNU Libtool ltdl backend)

Component guide for `src/mca/prtedl/libltdl/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the opaque
`prte_dl_handle_t`, and the build-time selection model.

---

## Role and priority

`libltdl` implements the `prtedl` contract on top of GNU Libtool's `ltdl`
library (`lt_dlopen` / `lt_dlsym` / `lt_dlclose` / `lt_dlerror`, with
optional `lt_dladvise` mode control). Its priority is **50**, *below*
`dlopen` (80), so it is only selected when the native POSIX `dlopen`
component is unavailable or a developer forces it with
`--disable-prte-dlopen`. It was the fallback for the rare platform that
lacked `<dlfcn.h>` but shipped Libtool's ltdl. Like `dlopen`, it forces
static compilation.

Files:

| File | Contents |
|------|----------|
| `prtedl_libltdl.h` | Private `struct prte_dl_handle_t`; component struct with cached `lt_dladvise` objects; module/component extern decls. |
| `prtedl_libltdl_component.c` | Registration/open/close/query; ltdl init/exit; `lt_dladvise` setup; priority 50; `have_lt_dladvise` info param. |
| `prtedl_libltdl_module.c` | The four interface functions and the module vtable. |
| `configure.m4` | Priority (50), forced static mode, `OAC_CHECK_PACKAGE` probe, `lt_dladvise` feature test, `--with-libltdl[-libdir]`. |

---

## ⚠️ This component carries a botched `dl`→`prtedl` rename

Before doing anything here, know that a global search-and-replace of the
token `dl` → `prtedl` (the one that renamed this framework from `dl` to
`prtedl`) over-reached into this component's **ltdl API references** and
left it in a state that would neither configure nor compile. The
mangling is *inconsistent* — some ltdl names were rewritten, adjacent
ones were not:

| Location | Code as written | Correct ltdl name |
|----------|-----------------|-------------------|
| `prtedl_libltdl.h` include | `#include <ltprtedl.h>` | `<ltdl.h>` |
| `prtedl_libltdl.h` handle type | `lt_prtedlhandle ltprtedl_handle;` | `lt_dlhandle ltdl_handle;` |
| `..._component.c` open | `lt_prtedlinit()` | `lt_dlinit()` |
| `..._component.c` close | `lt_prtedlexit()` | `lt_dlexit()` |
| `configure.m4` header probe | `OAC_CHECK_PACKAGE(...[ltprtedl.h],[ltprtedl],...)` | `ltdl.h` / `ltdl` |

Two consequences fall out of this:

1. **The configure probe can never succeed.** `OAC_CHECK_PACKAGE` looks
   for a header named `ltprtedl.h` and a library `-lltprtedl`, neither of
   which exists (the real names are `ltdl.h` and `libltdl`). So
   `prte_prtedl_libltdl_happy` stays `no`, the component is never built,
   and — because `dlopen` (priority 80) normally wins anyway — nobody
   notices.
2. **Even if it built, it would not compile.** The header field is
   `ltprtedl_handle`, but `prtedl_libltdl_module.c` still refers to
   `handle->ltdl_handle` (the un-renamed name), and
   `lt_prtedlhandle` / `lt_prtedlinit` / `lt_prtedlexit` are not real
   ltdl symbols. The `lt_dlopen*`, `lt_dlsym`, `lt_dlclose`, `lt_dlerror`,
   `lt_dlforeachfile`, and `lt_dladvise_*` calls in the module *were*
   left correct, so the corruption is partial.

**If you need to revive this backend, the fix is to undo the rename on
ltdl symbols only** (restore `<ltdl.h>`, `lt_dlhandle`, `ltdl_handle`,
`lt_dlinit`, `lt_dlexit`, and the `ltdl.h`/`ltdl` package probe) while
leaving the PRRTE-owned `prte_prtedl_*` names untouched. Do not "fix" it
by renaming the module's `ltdl_handle` to match the header's
`ltprtedl_handle` — that would make the mangled names self-consistent but
still wrong against the real ltdl API. The descriptions below use the
*intended* ltdl names for clarity.

---

## The handle struct

Intended shape (from `prtedl_libltdl.h`, ltdl names normalized):

```c
struct prte_dl_handle_t {
    lt_dlhandle ltdl_handle;   /* handle returned by lt_dlopen*() */
#if PRTE_ENABLE_DEBUG
    char *filename;            /* strdup'd name, debug only */
#endif
};
```

`calloc`ed in `open`, `free`d in `close`.

---

## `lt_dladvise` and the component (`prtedl_libltdl_component.c`)

Unlike POSIX `dlopen`, ltdl needs global `lt_dlinit()`/`lt_dlexit()`
bracketing, done in the component's open/close. If the linked ltdl is new
enough to have `lt_dladvise` (detected at configure time as
`PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE`), the component pre-builds **four**
advise objects covering the cross product of {private, public} ×
{ext, noext}:

- `advise_private_noext`, `advise_private_ext`,
  `advise_public_noext`, `advise_public_ext`

`open` (component) initializes each with `lt_dladvise_init` and sets
`lt_dladvise_global` (for the public ones) and/or `lt_dladvise_ext` (for
the ext ones); `close` (component) destroys all four with
`lt_dladvise_destroy`, then calls `lt_dlexit`. When `lt_dladvise` is
absent the component compiles those blocks out and the module falls back
to `lt_dlopen`/`lt_dlopenext`.

`libltdl_component_register` registers one read-only info param,
`prtedl_libltdl_have_lt_dladvise` (bool), reporting whether the advise
path is compiled in. `libltdl_component_query` returns the module and
priority 50.

---

## How the module implements each interface function

Vtable at the bottom of `prtedl_libltdl_module.c`:

```c
prte_prtedl_base_module_t prte_prtedl_libltdl_module = {
    .open = libltdl_open, .lookup = libltdl_lookup,
    .close = libltdl_close, .foreachfile = libltdl_foreachfile };
```

### `open` → `libltdl_open`
With `lt_dladvise` available, it picks one of the four cached advise
objects from the `(use_ext, private_namespace)` pair and calls
`lt_dlopenadvise(fname, advise)`. Without it, `lt_dlopenext(fname)` when
`use_ext`, else `lt_dlopen(fname)`. On success it `calloc`s the handle,
stores the ltdl handle, debug-`strdup`s `fname` (or `"(null)"`), returns
`PRTE_SUCCESS`. On failure it sets `err_msg` from `lt_dlerror()` and
returns `PRTE_ERROR`.

### `lookup` → `libltdl_lookup`
`*ptr = lt_dlsym(handle->ltdl_handle, symbol)`. Non-NULL →
`PRTE_SUCCESS`; NULL → `err_msg` from `lt_dlerror()`, `PRTE_ERROR`.

### `close` → `libltdl_close`
`lt_dlclose(handle->ltdl_handle)`, frees the debug filename and the
handle struct, returns `lt_dlclose`'s raw result.

### `foreachfile` → `libltdl_foreachfile`
A thin wrapper: `lt_dlforeachfile(search_path, func, data)` and translate
0 → `PRTE_SUCCESS`, non-zero → `PRTE_ERROR`. The `.la`/`.lo` skipping and
basename de-duplication that the `dlopen` backend hand-rolls is provided
here by ltdl itself.

---

## `err_msg` ownership — a real divergence from `dlopen`

Note the module sets `*err_msg = strdup((char *) lt_dlerror())` in
`open`, i.e. **heap the caller would own**, but sets `*err_msg =
(char *) lt_dlerror()` (borrowed) in `lookup`. This is inconsistent
within the backend *and* differs from the framework's stated "err_msg is
borrowed, do not free" contract and from the `dlopen` backend (which
always borrows). If you revive this component, reconcile it to the
framework contract (borrow, do not `strdup`) so callers can treat
`err_msg` uniformly regardless of which backend won.

---

## configure.m4 notes

Beyond the mangled package probe (see the warning above), the file adds
`--with-libltdl(=DIR)` and `--with-libltdl-libdir=DIR`, and — when the
(currently unreachable) probe would succeed — feature-tests
`lt_dladvise_init` via `AC_CHECK_FUNC` to set
`PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE`. If `--with-libltdl` is given
explicitly but the probe fails, it errors out ("Libltdl support requested
… but not found"); otherwise it silently declines and lets `dlopen` win.

---

## Gotchas when editing

- **Fix the rename before anything else** (see the warning section). Any
  edit that assumes this component compiles today is mistaken.
- **ltdl needs global init/exit** — the component's open/close own
  `lt_dlinit`/`lt_dlexit`; don't move that into the module.
- **Keep the four `lt_dladvise` objects paired with their destroys** —
  each `lt_dladvise_init` in component-open needs its
  `lt_dladvise_destroy` in component-close, all under
  `PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE`.
- **Reconcile `err_msg` ownership to "borrowed"** to match the framework
  contract and the `dlopen` backend.
- **`close` returns ltdl's raw value**, like the `dlopen` backend — not a
  translated `PRTE_*` code.
