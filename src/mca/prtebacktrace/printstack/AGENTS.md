# AGENTS.md — `prtebacktrace/printstack` (Solaris/illumos)

Component guide for `src/mca/prtebacktrace/printstack/`. Read the
[framework guide](../AGENTS.md) first for the two-function contract, the
link-time-selection model, and the async-signal-safety constraint.

---

## Role and priority

`printstack` is the **Solaris/illumos** strategy. Priority **30**. It
implements the print path with the platform's `printstack(3C)` routine,
which walks the current thread's stack (via `walkcontext`) and writes a
symbolic trace to a file descriptor. It is selected on SunOS-family
systems, which lack the glibc `backtrace(3)` family that `execinfo`
needs.

---

## When and why it is selected

`printstack/configure.m4` gates the component:

```m4
AC_DEFUN([MCA_prte_prtebacktrace_printstack_PRIORITY], [30])
...
AC_CHECK_HEADERS([ucontext.h])
AC_CHECK_FUNCS([printstack],
               [prtebacktrace_printstack_happy="yes"],
               [prtebacktrace_printstack_happy="no"])
```

It builds only when `<ucontext.h>` is present **and** the `printstack`
function links — effectively only on Solaris/illumos. On those systems
`execinfo`'s `backtrace` check fails, so under the framework's
`STOP_AT_FIRST` mode `printstack` (priority 30) is the one that survives.
It builds `static` (forced by its `COMPILE_MODE`).

---

## Files

| File | Contents |
|------|----------|
| `backtrace_printstack.c` | The implementation: `prte_backtrace_print` (real) and `prte_backtrace_buffer` (stub). |
| `backtrace_printstack_component.c` | The bare component struct `prte_mca_prtebacktrace_printstack_component` (name/version header only) + `PMIX_MCA_BASE_COMPONENT_INIT(prte, prtebacktrace, printstack)`. No query, no module. |
| `configure.m4` | Priority 30, `ucontext.h`/`printstack` availability check, forced static compile. |
| `Makefile.am` | Build wiring. |

---

## How it implements the interface

### `prte_backtrace_print(FILE *file, char *prefix, int strip)`

```c
int fd = prte_stacktrace_output_fileno;
if (NULL != file) {
    fd = fileno(file);
}
printstack(fd);
return PRTE_SUCCESS;
```

It resolves the output descriptor the same way `execinfo` does
(`prte_stacktrace_output_fileno`, overridden by `fileno(file)`), then
hands it to `printstack(fd)`, which writes the full symbolic trace
itself. `printstack` is documented as async-signal-safe on Solaris, so
it is legitimate to call from the crash handler.

Note the two parameters it **ignores**: `prefix` (there is no per-line
hook into `printstack`, so no host/pid banner is prepended to each
frame) and `strip` (`printstack` always prints from the current frame;
the handler frames appear in the output). These are accepted for
interface compatibility but not honored.

### `prte_backtrace_buffer(char ***message_out, int *len_out)`

A **stub**: it sets `*message_out = NULL`, `*len_out = 0`, and returns
`PRTE_ERR_NOT_IMPLEMENTED`. The source carries a long-standing "BWB"
comment noting it *could* be implemented on top of `walkcontext` the way
`printstack` is, but nobody has done it. Callers that need buffered
trace strings (e.g. `prte_stackframe_output_string`) therefore get
nothing on this platform and must tolerate the failure — they already
check the return code.

---

## Async-signal-safety notes / gotchas when editing

- **`printstack()` is the async-signal-safe engine** here; keep the print
  path a thin wrapper around it. Do not add allocation, stdio, or
  locking.
- **`prefix` and `strip` are silently dropped.** If you make them work,
  do it without breaking async-signal-safety — you would have to
  reimplement on top of `walkcontext(3C)` with a signal-safe per-frame
  callback, not post-process `printstack` output.
- **Both symbols must remain defined.** Even though `prte_backtrace_buffer`
  is unimplemented, it must still exist and return
  `PRTE_ERR_NOT_IMPLEMENTED` — dropping it would break the link, since
  the framework's callers reference the symbol unconditionally.
- This component is hard to test off a Solaris/illumos box; changes here
  are effectively untested on the CI platforms and warrant extra care.
