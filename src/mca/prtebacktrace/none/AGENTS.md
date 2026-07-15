# AGENTS.md — `prtebacktrace/none` (no-op fallback)

Component guide for `src/mca/prtebacktrace/none/`. Read the
[framework guide](../AGENTS.md) first for the two-function contract and
the link-time-selection model.

---

## Role and priority

`none` is the **guaranteed fallback**. Priority **0** — the lowest,
so it is chosen only when no platform-specific strategy is available. It
implements neither real backtrace path; it exists so that the two
framework symbols (`prte_backtrace_print`, `prte_backtrace_buffer`)
*always resolve at link time*, on any platform, even one with no
usable backtrace facility. On such a platform a crash simply produces no
stack trace rather than a link error or a missing feature.

---

## When and why it is selected

`none/configure.m4` has **no availability check** — its `_CONFIG` macro
does nothing but `AC_CONFIG_FILES([.../none/Makefile])`, so the component
is always buildable:

```m4
AC_DEFUN([MCA_prte_prtebacktrace_none_PRIORITY], [0])
...
AC_DEFUN([MCA_prte_prtebacktrace_none_CONFIG],[
    AC_CONFIG_FILES([src/mca/prtebacktrace/none/Makefile])
])
```

Under the framework's `STOP_AT_FIRST` mode, configure walks components in
priority order; `none` (priority 0) is reached only after both
`execinfo` and `printstack` (priority 30) have failed their checks. So it
lands in `static-components.h` exactly when the platform has neither
glibc `backtrace()` nor Solaris `printstack()`. It builds `static`
(forced by its `COMPILE_MODE`).

---

## Files

| File | Contents |
|------|----------|
| `backtrace_none.c` | Both interface functions, each a no-op returning `PRTE_ERR_NOT_IMPLEMENTED`. |
| `backtrace_none_component.c` | The bare component struct `prte_mca_prtebacktrace_none_component` (name/version header only) + `PMIX_MCA_BASE_COMPONENT_INIT(prte, prtebacktrace, none)`. No query, no module. |
| `configure.m4` | Priority 0, no availability check, forced static compile. |
| `Makefile.am` | Build wiring. |

---

## How it implements the interface

Both functions are stubs:

```c
int prte_backtrace_print(FILE *file, char *prefix, int strip)
{
    return PRTE_ERR_NOT_IMPLEMENTED;
}

int prte_backtrace_buffer(char ***message_out, int *len_out)
{
    *message_out = NULL;
    *len_out = 0;
    return PRTE_ERR_NOT_IMPLEMENTED;
}
```

`prte_backtrace_buffer` zeroes its out-params before returning so callers
never read uninitialized pointers. Callers already check the return code
(the crash handler prints an "unable to print backtrace" message when
`prte_backtrace_print` returns non-`PRTE_SUCCESS`), so `none` degrades
gracefully to "no trace available."

---

## Gotchas when editing

- **This component is the safety net** — its whole point is to *always*
  compile and *always* define both symbols. Do not add an availability
  check or otherwise make it conditionally unbuildable, or platforms with
  no backtrace facility would fail to link.
- **Keep both stubs returning `PRTE_ERR_NOT_IMPLEMENTED`** and keep the
  buffer path zeroing its out-params. Callers depend on that contract.
- `backtrace_none_component.c` carries a spurious forward `extern` for a
  `prte_mca_backtrace_none_component` (note: no `prte`-doubled framework
  segment) that does not match the actual struct name
  `prte_mca_prtebacktrace_none_component`; it is unused. Leave it or
  tidy it as an incidental fix, but do not rename the real struct — the
  generated `static-components.h` references the real name.
