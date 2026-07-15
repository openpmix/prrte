# AGENTS.md — `prtedl/dlopen` (native POSIX libdl backend)

Component guide for `src/mca/prtedl/dlopen/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the opaque
`prte_dl_handle_t`, and the build-time (not runtime) selection model.

---

## Role and priority

`dlopen` implements the `prtedl` contract directly on the POSIX dynamic
linker — `dlopen(3)` / `dlsym(3)` / `dlclose(3)` / `dlerror(3)` from
`<dlfcn.h>`. Its priority is **80**, higher than `libltdl` (50), so on
any platform that has a working `libdl` it is the component `configure`
selects and the only one built. In practice that is essentially every
platform PRRTE targets, so `dlopen` is the de-facto backend.

Files:

| File | Contents |
|------|----------|
| `prtedl_dlopen.h` | The private `struct prte_dl_handle_t` and the component struct type; extern decls for the module and component. |
| `prtedl_dlopen_component.c` | Registration/open/close/query; the `filename_suffixes` MCA param; priority 80. |
| `prtedl_dlopen_module.c` | The four interface functions plus the `do_dlopen` helper; the module vtable. |
| `configure.m4` | Priority (80), forced static compile mode, `OAC_CHECK_PACKAGE` for `dlfcn.h`/`-ldl`/`dlopen`, `--disable-prte-dlopen` back-door. |

---

## When/why selected

`configure.m4` runs `OAC_CHECK_PACKAGE([dlopen], …, [dlfcn.h], [dl],
[dlopen], …)` — it needs the `<dlfcn.h>` header and a `dlopen` symbol
(in `libc` or `-ldl`). If found, the component is "happy" and, being the
highest priority, wins the framework's `STOP_AT_FIRST` selection. The
`--disable-prte-dlopen` switch is a developer-only back-door to force the
`libltdl` component instead (users who want *no* DL support use the
framework-level `--disable-dlopen`). The component always compiles
static (`MCA_prte_prtedl_dlopen_COMPILE_MODE` → `"static"`).

---

## The handle struct

From `prtedl_dlopen.h`:

```c
struct prte_dl_handle_t {
    void *dlopen_handle;      /* the void* returned by dlopen() */
#if PRTE_ENABLE_DEBUG
    void *filename;           /* strdup'd name, for debugging only */
#endif
};
```

Allocated with `calloc` in `open`, released with `free` in `close`. The
`filename` copy exists only in debug builds and is freed alongside.

---

## The component (`prtedl_dlopen_component.c`)

- **`dlopen_component_register`** sets the default of the one MCA param,
  `prtedl_dlopen_filename_suffixes` — a comma-delimited string defaulting
  to `".so,.dylib,.dll,.sl"` — registers it, then splits it into a
  `char **filename_suffixes` argv the module walks when `use_ext` is
  true.
- **`dlopen_component_open`** is a no-op (`PRTE_SUCCESS`) — libdl needs no
  global init.
- **`dlopen_component_close`** frees the split `filename_suffixes` argv.
- **`dlopen_component_query`** hands back `&prte_prtedl_dlopen_module.super`
  and the priority. The comment notes the priority is "somewhat
  meaningless" because configure already guaranteed at most one component
  exists.

---

## How the module implements each interface function

The module vtable at the bottom of `prtedl_dlopen_module.c`:

```c
prte_prtedl_base_module_t prte_prtedl_dlopen_module = {
    .open = dlopen_open, .lookup = dlopen_lookup,
    .close = dlopen_close, .foreachfile = dlopen_foreachfile };
```

### `open` → `dlopen_open`
Builds the `flags`: always `RTLD_LAZY`, plus `RTLD_LOCAL` when
`private_namespace` is true, else `RTLD_GLOBAL`.

- **`use_ext` && `fname != NULL`:** loops the component's
  `filename_suffixes`, `pmix_asprintf`-ing `"<fname><ext>"` for each. It
  `stat()`s each candidate; on a miss it optionally fills `err_msg`
  ("File %s not found") and continues; on the first hit it calls the
  `do_dlopen` helper and **breaks** (whether or not the dlopen itself
  succeeded — one existing file is tried, not every suffix).
- **otherwise:** a single `do_dlopen(fname, …)` on the name as given
  (this is also the `fname == NULL` "open the main image" path).

On success it `calloc`s a `prte_dl_handle_t`, stores the raw
`dlopen_handle`, and (debug builds) `strdup`s `fname` (or `"(null)"`).
Returns `PRTE_SUCCESS` if a handle was obtained, else `PRTE_ERROR`
(`PRTE_ERR_IN_ERRNO` / `PRTE_ERR_OUT_OF_RESOURCE` on allocation
failures).

The `do_dlopen` helper wraps `dlopen(fname, flags)` and, if `err_msg` is
non-NULL, sets it to `NULL` on success or to `dlerror()` on failure.
**Ownership note:** `dlerror()` returns a pointer into libdl's internal
storage — the caller must not free it, and it is only valid until the
next libdl call. This matches the framework's "err_msg is borrowed"
contract.

### `lookup` → `dlopen_lookup`
`*ptr = dlsym(handle->dlopen_handle, symbol)`. Non-NULL → `PRTE_SUCCESS`;
NULL → set `err_msg` from `dlerror()` and return `PRTE_ERROR`. Asserts
that handle, inner handle, symbol, and `ptr` are all non-NULL.

### `close` → `dlopen_close`
`ret = dlclose(handle->dlopen_handle)`; frees the debug `filename` and
the handle struct; returns `dlclose`'s raw result (0 on success — note
this is the libdl convention, which happens to equal `PRTE_SUCCESS`, not
a translated PRRTE code).

### `foreachfile` → `dlopen_foreachfile`
This is the only substantial function. It splits `search_path` on
`PRTE_ENV_SEP` and, per directory, `opendir`/`readdir`s entries:

1. `stat` each entry; skip anything that is not a regular file
   (`S_ISREG`).
2. Strip the suffix at the last `.`; **skip Libtool bookkeeping files**
   (`.la`, `.lo`) entirely.
3. De-duplicate: track basenames already seen in a `good_files` argv and
   append only new ones — so `foo.la` + `foo.so` yield one `"foo"`.
4. After scanning, invoke the caller's `func(basename, data)` on each
   collected basename; if any callback returns non-`PRTE_SUCCESS`, stop
   and propagate that code.

Cleanup at the `error:` label closes any open `DIR`, frees the `dirs`
split and the `good_files` argv. Uses `PMIx_Argv_*` for all argv
handling and `pmix_asprintf` for path construction.

---

## Gotchas when editing

- **`use_ext` tries the first *existing* file, not the first that
  dlopens.** Once `stat` finds a candidate the loop `break`s after a
  single `do_dlopen`, even on failure. If you need "try every suffix
  until one loads", that is a behavior change — weigh it carefully.
- **`err_msg` from this backend is borrowed libdl storage** (`dlerror`),
  not heap you own — never `free` it, and treat it as valid only until
  the next `prtedl`/libdl call. (The `libltdl` backend historically
  `strdup`ed some messages, so a caller that frees `err_msg` would be
  wrong here — keep the borrowed contract.)
- **`close` returns `dlclose`'s value verbatim.** It is the raw libc
  return (0/-1), not a `PRTE_*` code that happens to coincide with
  success at 0. Don't "fix" it to `PRTE_SUCCESS` without checking callers.
- **`foreachfile` must keep skipping `.la`/`.lo` and de-duplicating
  basenames** — that contract is what callers rely on to iterate a plugin
  directory once per component.
- **The `filename` field is `void *` and debug-only.** It is `strdup`ed
  (a `char *`) but typed `void *`; keep it under `#if PRTE_ENABLE_DEBUG`
  and remember it is freed in `close`.
