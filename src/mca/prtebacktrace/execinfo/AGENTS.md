# AGENTS.md — `prtebacktrace/execinfo` (glibc/BSD backtrace)

Component guide for `src/mca/prtebacktrace/execinfo/`. Read the
[framework guide](../AGENTS.md) first for the two-function contract, the
link-time-selection model, and the async-signal-safety constraint that
governs everything here.

---

## Role and priority

`execinfo` is the **mainstream default** on Linux (glibc), FreeBSD, and
macOS. Priority **30**. It implements the trace with the standard
`backtrace(3)` family from `<execinfo.h>`. On the vast majority of the
systems PRRTE runs on, this is the component that ends up in
`static-components.h`.

---

## When and why it is selected

`execinfo/configure.m4` gates the component:

```m4
AC_DEFUN([MCA_prte_prtebacktrace_execinfo_PRIORITY], [30])
...
AC_CHECK_HEADERS([execinfo.h])
# FreeBSD has backtrace in -lexecinfo, usually in libc
PRTE_SEARCH_LIBS_COMPONENT([backtrace_execinfo], [backtrace], [execinfo],
               [prtebacktrace_execinfo_happy="yes"],
               [prtebacktrace_execinfo_happy="no"])
```

The component is "happy" (and thus buildable) only when `<execinfo.h>`
exists **and** the `backtrace` symbol links — from libc, or from
`-lexecinfo` on FreeBSD, which `PRTE_SEARCH_LIBS_COMPONENT` locates and
records for the link line. Because the framework runs `STOP_AT_FIRST`
and this component ties `printstack` at priority 30, it is selected on
any platform where `printstack` is absent (i.e. everywhere that is not
Solaris/illumos). It builds `static` (forced by its `COMPILE_MODE`).

---

## Files

| File | Contents |
|------|----------|
| `backtrace_execinfo.c` | The implementation: `prte_backtrace_print` and `prte_backtrace_buffer`. |
| `backtrace_execinfo_component.c` | The bare component struct `prte_mca_prtebacktrace_execinfo_component` (name/version header only) + `PMIX_MCA_BASE_COMPONENT_INIT(prte, prtebacktrace, execinfo)`. No query, no module. |
| `configure.m4` | Priority 30, `execinfo.h`/`backtrace` availability check, forced static compile. |
| `Makefile.am` | Build wiring. |

---

## How it implements the interface

### `prte_backtrace_print(FILE *file, char *prefix, int strip)`

The **async-signal-safe** path, and the one the crash handler calls:

1. Resolve the output descriptor: `fd = prte_stacktrace_output_fileno`,
   overridden by `fileno(file)` when `file != NULL`. If `fd == -1`,
   return `PRTE_ERR_BAD_PARAM`.
2. `trace_size = backtrace(trace, 32)` — capture up to 32 return
   addresses into a fixed on-stack array (no allocation).
3. For each frame from index `strip` to `trace_size`: write `prefix`
   (if non-NULL) via `pmix_fd_write()`, then a `"[%2d] "` frame-number
   label via `snprintf` into a small stack buffer + `pmix_fd_write()`,
   then `backtrace_symbols_fd(&trace[i], 1, fd)` to emit that one frame's
   symbolized line straight to the descriptor.
4. Return `PRTE_SUCCESS`.

The key async-signal-safety choice is **`backtrace_symbols_fd`**, which
writes directly to a descriptor and does **not** call `malloc` — unlike
`backtrace_symbols`. Output goes through `pmix_fd_write()` (a raw
`write()` wrapper), not buffered stdio. Frames are emitted one at a time
so the per-line `prefix` can be interleaved.

### `prte_backtrace_buffer(char ***message_out, int *len_out)`

The convenience path for non-handler callers:

1. `backtrace(trace, 32)` as above.
2. `backtrace_symbols(trace, trace_size)` — this **allocates** the
   returned string array with `malloc`.
3. Hand the array back in `*message_out` (caller frees) and the count in
   `*len_out`. Returns `PRTE_SUCCESS`.

Because step 2 allocates, this function is **not** async-signal-safe —
matching the framework header's warning. Keep it off the signal-handler
path.

---

## Async-signal-safety notes / gotchas when editing

- **Do not introduce `malloc`, stdio, or locks into `prte_backtrace_print`.**
  It runs from a `SIGSEGV`/`SIGBUS` handler. Stick to `backtrace()`,
  `backtrace_symbols_fd()`, `snprintf` into a stack buffer, and
  `pmix_fd_write()`. Never "improve" it by switching to
  `backtrace_symbols` + `fprintf`.
- **The 32-frame cap is fixed** (`void *trace[32]`). Deep stacks are
  truncated. Enlarging it grows the handler's stack footprint — a real
  concern when the crash is a stack overflow — so change it only
  deliberately.
- **`buf[6]` sizes exactly the `"[%2d] "` label** (`[`, two digits,
  `]`, space, NUL). If you widen the format, widen the buffer.
- **Guard on `HAVE_EXECINFO_H`/`HAVE_UNISTD_H`** as the source already
  does — the file is only ever compiled when the header check passed, but
  the guards keep it honest.
