# AGENTS.md — `src/include`

Orientation for AI agents and human contributors working in `src/include/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

Everything the rest of the tree is compiled *against*: the return codes, the
base typedefs, and the configure-driven portability layer. No subsystem, no
components, no functions of consequence.

That makes this directory unusual in one specific way: **a defect here is
invisible at the point of the mistake.** Nothing in this directory has
behavior of its own, so a wrong constant or a mis-parenthesized macro shows
up as strange behavior in whatever subsystem happened to use it.

| File | What it is | Generated? |
|------|------------|------------|
| `constants.h` | PRRTE's return codes. | no |
| `types.h` | Rank/app-index typedefs, `prte_socklen_t`, the 64-bit byte-order helpers. | no |
| `prte_config_top.h` | Undefs the autoconf `PACKAGE_*` macros. Included at the *top* of `prte_config.h`. | no |
| `prte_config_bottom.h` | The portability layer: the `__prte_attribute_*__` wrappers, `PRTE_EXPORT`, `PRTE_PATH_MAX`, the `sockaddr`/`AF_*` fallbacks. Included at the *bottom* of `prte_config.h`. | no |
| `prte_stdint.h` | Fixed-width types and `PRIsize_t` where the system's are inadequate. | no |
| `prte_stdatomic.h` | `prte_atomic_bool_t`. That is the whole file. | no |
| `prte_socket_errno.h` | `#define prte_socket_errno errno`. | no |
| `prte_portable_platform.h`, `_real.h` | Compiler-identification macros, also read by `config/prte_check_compiler_version.m4`. | vendored — do not edit |
| `prte_config.h.in` | autoheader output. | **yes** |
| `version.h.in` | configure substitution. | template |
| `prte_frameworks.[ch]` | The framework list. | **yes** — `autogen.pl` |

`prte_config_top.h` and `prte_config_bottom.h` are included *only* from
`prte_config.h` and both `#error` if you try otherwise. `prte_config.h` in
turn must be the first `#include` in every `.c` file in the tree — that is a
top-level golden rule and it exists so this directory's definitions are in
place before anything else is parsed.

---

## GOLDEN RULE: the error codes hang off `PMIX_EXTERNAL_ERR_BASE`

`constants.h` numbers its codes as negative offsets from one base:

```
PRTE_ERR_BASE  PMIX_EXTERNAL_ERR_BASE  (-3000)
                 offsets   1 ..  72  ->  -3001 .. -3072   mirrored from PMIx
                 offsets 101 .. 155  ->  -3101 .. -3155   PRRTE's own
```

`PRTE_SUCCESS` is `0` and is the only code that is not an offset. Everything
else is negative.

**The base is not arbitrary.** PMIx reserves everything below
`PMIX_EXTERNAL_ERR_BASE` for projects layered on top of it — *"negative
values larger than this are guaranteed not to conflict with PMIx values"* —
and PRRTE is such a project. PRRTE and PMIx codes cross paths constantly,
and a code that escapes `prte_pmix_convert_rc()` / `prte_pmix_convert_status()`
does not look foreign if the two schemes overlap: it looks like some other,
real code. Define new codes relative to the constant, never to a specific
value — PMIx says it may move.

This was not hypothetical. PRRTE used to base its codes at `0`, and **46 of
them were numerically identical to a live PMIx status meaning something
else**: `PRTE_ERR_SLURM_SHRINK_FAILURE` was `PMIX_OPERATION_SUCCEEDED`,
`PRTE_ERR_NO_PATH_TO_TARGET` was `PMIX_ERR_EVENT_REGISTRATION`, and
`PRTE_ERR_TAKE_NEXT_OPTION` — a control-flow signal, not a failure — was
`PMIX_ERR_NOT_FOUND`. `PRTE_ERROR` and `PMIX_ERROR` were both `-1`, which is
why a converter bug in this area could never show up in the case a test
would most naturally construct.

The other thing to know is that the codes were, for several years, not all
negative. There used to be a second base, `PRTE_ERR_SPLIT`, for the lower
group. It is an ORTE inheritance: the two groups belonged to two separate
projects — OPAL owned the first, ORTE started where OPAL's allocation ended
— so each could append without coordinating with the other. ORTE's base was
`OPAL_ERR_MAX` = `-100`, and the minus sign was dropped when the two layers
were flattened into `src/` in 2019. So the whole lower group became
**positive** error codes (+99 down to +43), and a `PRTE_ERR_MAX` defined as
`(PRTE_ERR_SPLIT - 100)` was `0` — the value of `PRTE_SUCCESS`. OPAL and
ORTE have been one code base since that merge, so the second base is gone
too: one enum in one file does not need two. `PRTE_ERR_MAX` is gone rather
than corrected — nothing in the tree ever used it, and a bound with no
consumer is a bound nobody maintains. If you need one, the range is
`PRTE_ERR_BASE` downwards and the test that cares defines its own span.

Nothing in the tree ever tested a PRRTE code by sign, which is why the
inversion went unnoticed for so long: the house idiom is `PRTE_SUCCESS != rc`,
and both `prte_strerror()` and `prte_pmix_convert_rc()` switch on the
constants themselves. **Keep using `PRTE_SUCCESS != rc`** — it is the only
form that stays correct when the base moves.

### Adding a code

Append it at the end of its group with the next unused offset (or at the very
end — the two groups are a convention for readers, not arithmetic), leave the
hole between offsets 72 and 101 alone, and **add its string to
[`src/util/error.c`](../util/error.c)**. `test/unit/util` sweeps the whole
range and fails on a code with no string, because `PRTE_ERROR_LOG()` prints
nothing else and the user would see "Unknown error". `test/unit/include`
checks the range arithmetic. Uniqueness needs no test: `prte_strerror()`
switches on every code, so a repeated value is a duplicate-`case` error at
build time.

Job and process states are numbered similarly but live somewhere else:
[`src/mca/plm/plm_types.h`](../mca/plm/plm_types.h). They have **not** been
moved out of PMIx's space — they are a different kind of value, converted by
`prte_pmix_convert_state()`, and the top-level rule about converting at the
boundary still governs them. See [`src/pmix/AGENTS.md`](../pmix/AGENTS.md).

---

## `types.h`

What is left in here is small and all of it is live:

- `prte_local_rank_t` / `prte_node_rank_t` (both `uint16_t`) with their `MAX`
  and `INVALID` values. **Parenthesize any new `MAX`**: `PRTE_LOCAL_RANK_MAX`
  was written as `UINT16_MAX - 1` bare, which reads correctly alone and binds
  wrongly inside any larger expression.
- `prte_app_idx_t`, `prte_socklen_t`.
- `prte_hton64()` / `prte_ntoh64()`. These are the OOB's 64-bit wire
  conversion — [`oob_tcp_hdr.h`](../rml/oob/oob_tcp_hdr.h) runs every message
  header's origin epoch through them — which makes them the one thing in this
  directory a *heterogeneous* DVM depends on being right.

Note the `#ifdef HAVE_UNIX_BYTESWAP` in those two. That symbol is
`AC_DEFINE`'d only in the success branch, so it is absent rather than `0`
when the check fails, and `#ifdef` is the correct test — the opposite of the
project's usual `#if FOO` rule, which applies to the `AC_DEFINE_UNQUOTED`
0-or-1 symbols. Where it is absent, `prte_config_bottom.h` supplies identity
`htonl`/`htons` stubs and these follow suit.

`types.h` used to also carry `prte_ptr_t`, `prte_iov_base_ptr_t`,
`prte_ptr_ptol`/`ltop` and a set of `prte_swap_bytes*` functions. Nothing
used any of them.

---

## `prte_config_bottom.h`

The portability layer. Two things to know:

- **The `__prte_attribute_*__` macros are the only correct way to write a
  compiler attribute** in this tree (`__prte_attribute_unused__`,
  `__prte_attribute_noreturn__`, `__prte_attribute_format__`, …). Each
  expands to the real `__attribute__` where configure found support and to
  nothing elsewhere. Whether one expanded or silently vanished is not
  observable from inside the program, so a build with warnings-as-errors is
  the only check that exists for them.
- **`PRTE_ENABLE_IPV6` is narrowed here**, from what configure was asked for
  to what the platform can honor. The narrowing is written as a guarded
  `#undef` + `#define` rather than an unconditional re-`#define`, because an
  unconditional one emits `-Wmacro-redefined` on precisely the platform the
  narrowing exists for — and `--enable-devel-check` makes that an error.
  This is the one place in the tree that legitimately `#undef`s a logical
  macro.

---

## Atomics are C11, full stop

`prte_stdatomic.h` declares exactly one type, `prte_atomic_bool_t`, with the
`_Atomic` keyword and no alternative. PMIx's `pmix_stdatomic.h` says the same
thing the same way, and the two projects share a threading model.

There used to be a `volatile`-typed fallback here selected by
`PRTE_ATOMIC_C11 == 0`, reachable through `--disable-c11-atomics`.
`volatile` is not an atomic type, so that arm was a way to configure a build
that could not be correct; the `PRTE_ATOMIC_GCC_BUILTIN` and
`PRTE_ATOMIC_X86_64` symbols it emitted had no readers anywhere in the tree
either. `config/prte_setup_cc.m4` now fails configure outright when the
compiler cannot supply C11 atomics, so the requirement is enforced where it
is detected rather than discovered at compile time.

The one user is the OOB listener thread's stop flag
([`oob_tcp_listener.c`](../rml/oob/oob_tcp_listener.c)) — set on the main
thread, spun on by the listener.

---

## Do not duplicate PMIx

PRRTE builds against PMIx's *internal* headers, and PMIx installs its own
`pmix_prefetch.h`, `pmix_hash_string.h`, `pmix_stdatomic.h`,
`pmix_stdint.h`, `pmix_socket_errno.h` and `pmix_portable_platform.h`. Every
one of those has (or had) a PRRTE twin in this directory.

Three of the twins were pure dead weight and are gone:

- `align.h` — no includer, no user of any of its five macros.
- `hash_string.h` — a duplicate of `pmix_hash_string.h`; four files
  `#include`d it and none expanded either macro.
- `prefetch.h` — a duplicate of `pmix_prefetch.h` that defined
  **`PMIX_LIKELY` and `PMIX_UNLIKELY`**, in violation of the project's own
  symbol-prefix rule, guarded on PRRTE's `PRTE_C_HAVE_BUILTIN_EXPECT` rather
  than PMIx's. Nothing included it — the one `PMIX_LIKELY` use in the tree
  (`oob_tcp_sendrecv.c`) resolves to PMIx's — but had anything done so on a
  build where the two configure results disagreed, the differing bodies would
  have been a `-Wmacro-redefined` error.

The twins that remain are load-bearing and must stay: `prte_config_bottom.h`
needs `prte_stdint.h` before any PMIx header has been seen, and
`prte_socket_errno.h`/`prte_stdatomic.h` are one line and one typedef
respectively. **Do not add a fourth.** If PRRTE needs a generic helper PMIx
does not have, add it to PMIx and require the PMIx that has it via
`PRTE_CHECK_PMIX_CAP` — see the top-level `AGENTS.md`.

---

## Generated files

`prte_config.h.in` (autoheader), `prte_frameworks.[ch]` (`autogen.pl`) and
`version.h` (configure) are generated. Do not edit them, and remember that
adding a framework or a `config/*.m4` change means re-running `./autogen.pl`
and reconfiguring — a plain `make` will not do it and can wedge the tree.

---

## Testing

**Unit tests — `test/unit/include/test_include.c`, run by `make check`.**
Covers the error-code band arithmetic (no code is `PRTE_SUCCESS`, the two
bands cannot collide, and there is headroom to grow), the rank types and
their sentinels including the parenthesization trap, `prte_hton64`/`ntoh64`
round-trip *and* their actual big-endian byte layout — a pair of no-op
functions round-trips perfectly and still puts the wrong bytes on the wire —
`PRIsize_t`, the `PRTE_PATH_MAX`/`PRTE_MAXHOSTNAMELEN` fallbacks, and the
`PRTE_ENABLE_IPV6` narrowing.

That test deliberately does **not** call `prte_init_util()`. Everything here
has to be correct before any of PRRTE is running — `prte_init_util` is
itself compiled against these headers — so a test that needed the runtime up
would be testing the wrong thing.

**Multi-node — `contrib/dockerswarm`, the `test_include` phase.** A unit
test can prove the numbering is self-consistent; it cannot prove that a code
a *daemon* produces still reaches the user as a sentence. Renumbering does
not break the build — it breaks a real failure path into "Unknown error", or
into somebody else's error. So that phase provokes genuine failures on a
*remote* node (a missing executable, a missing working directory, an
impossible slot request) and requires each to come back named. The
byte-order helpers get their multi-node coverage for free: every message
between daemons runs its header epoch through `prte_hton64`.

**Not unit-testable, by construction:** whether an attribute macro expanded
or silently vanished, and whether `PRTE_EXPORT` produced a visible symbol.
Only the build can answer those, which is one more reason a warning-free
`--enable-debug` build is part of the checklist.
