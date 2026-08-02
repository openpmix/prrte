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
| `prte_stdint.h` | Pointer-sized integer fallbacks and `PRIsize_t`. | no |
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
                 offsets 101 .. 156  ->  -3101 .. -3156   PRRTE's own
PRTE_ERR_MAX                  offset 157  ->  -3157       not a code: the bound
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
too: one enum in one file does not need two.

### `PRTE_ERR_MAX` — the bound, and why it is back

`PRTE_ERR_MAX` is the last member of the enum and is **not a code**. Nothing
returns it, nothing tests for it, and `prte_strerror()` deliberately has no
case for it. It is one past the last assigned offset, and it exists so that
code sweeping the list has a bound it does not have to guess.

It was removed once, on the reasoning that nothing consumed it and *a bound
with no consumer is a bound nobody maintains* — leaving each test that
needed one to define its own span. That was the wrong shape. Two of them
(`test/unit/include` and `test/unit/util`) wrote out the last offset as a
literal `155`, and **both went stale on the very first code appended after
they were written**. The consequence was worse in `test/unit/util`, whose
sweep exists precisely to catch a code added without a `prte_strerror()`
string: it stopped one short of the newest code — the code most likely to be
missing one.

So the bound is back, with the two things that were missing the first time:

- **It lives next to the append site.** Bump it in the same edit that adds a
  code. It sits at the bottom of the enum, immediately below where you are
  appending, for exactly that reason.
- **A stale bound is a test failure, not a silent loss of coverage.**
  `test_error_bound()` in `test/unit/util` uses `prte_strerror()` as the
  oracle: the offset just inside `PRTE_ERR_MAX` must be a real named code,
  and `PRTE_ERR_MAX` itself must not be. That catches both directions —
  appending a code without bumping the bound, and bumping the bound past the
  last code.

`test/unit/pmix` keeps a span of its own (`PRTE_CODE_SPAN`, 200) on purpose.
It is a deliberately loose range test — "is this value plausibly a PRRTE
code rather than a raw PMIx status?" — and wants headroom rather than the
exact end of the list.

Nothing in the tree ever tested a PRRTE code by sign, which is why the
inversion went unnoticed for so long: the house idiom is `PRTE_SUCCESS != rc`,
and both `prte_strerror()` and `prte_pmix_convert_rc()` switch on the
constants themselves. **Keep using `PRTE_SUCCESS != rc`** — it is the only
form that stays correct when the base moves.

### Adding a code

Three steps, all in the same edit:

1. Append it at the end of its group with the next unused offset (or at the
   very end — the two groups are a convention for readers, not arithmetic),
   leaving the hole between offsets 72 and 101 alone.
2. **Bump `PRTE_ERR_MAX`** to one past your new offset. It is the next line
   down; you cannot miss it, and if you do, `test/unit/util` fails.
3. **Add its string to [`src/util/error.c`](../util/error.c).**
   `test/unit/util` sweeps the whole range and fails on a code with no
   string, because `PRTE_ERROR_LOG()` prints nothing else and the user would
   see "Unknown error".

`test/unit/include` checks the range arithmetic. Uniqueness needs no test:
`prte_strerror()` switches on every code, so a repeated value is a
duplicate-`case` error at build time.

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

---

## `prte_stdint.h`

Fixed-width and pointer-sized integer types, plus `PRIsize_t`. Two notes:

- **`PRIsize_t` is unconditionally `"zu"`.** C11 is a hard requirement, so
  `"z"` is always available and is the only spelling correct by *definition*
  rather than by a size coincidence. It used to be a ladder whose `"zu"` arm
  was guarded on `ACCEPT_C99` — a symbol PRRTE's configure has never defined
  — so every build fell through to comparing `SIZEOF_SIZE_T` against
  `SIZEOF_LONG` and `SIZEOF_LONG_LONG`, landing on a working answer for the
  usual data models and on plain `"u"` for any where it did not.
- **The `intptr_t`/`uintptr_t` ladder is live** and ends in an `#error`, so
  it is one of the few things here that fails loudly. It only fires where
  the system's `<stdint.h>` does not supply the types (configure probes with
  `HAVE_INTPTR_T`/`HAVE_UINTPTR_T`), which C11 does not require it to.

There was also a 128-bit block here — `prte_int128_t`, `prte_uint128_t` and
`HAVE_PRTE_INT128_T`, selected from `HAVE_INT128_T` or `HAVE___INT128`. It
came from OPAL, whose atomics needed a 128-bit type. **Neither configure
symbol has ever been probed by PRRTE's configure**, so the types were never
declared and `HAVE_PRTE_INT128_T` was always `0` — and nothing in the tree
names any of the three. It is gone. If PRRTE ever needs one, probe for it in
`configure.ac` in the same change.

---

## What `types.h` no longer carries

`types.h` used to also carry `prte_ptr_t`, `prte_iov_base_ptr_t`,
`prte_ptr_ptol`/`ltop` and a set of `prte_swap_bytes*` functions. Nothing
used any of them.

---

## `prte_config_bottom.h`

The portability layer. Three things to know:

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
- **The printf fallbacks all come from PMIx.** Where the platform's libc is
  missing `asprintf`/`snprintf`/`vasprintf`/`vsnprintf`, this header maps the
  name onto `pmix_asprintf`/`pmix_snprintf`/`pmix_vasprintf`/`pmix_vsnprintf`
  from `src/util/pmix_printf.h`. **PRRTE has no printf replacements of its
  own and never had** — if you find yourself writing `prte_` here, that is
  the mistake. It was already made once: the `vsnprintf` arm named
  `prte_vsnprintf`, a symbol in neither tree, so a platform without
  `vsnprintf()` would have failed to build every tool — and not merely at
  link time. `pmix_printf.h` pulls in PMIx's own `pmix_config_bottom.h`,
  which maps `vsnprintf` to `pmix_vsnprintf` under the identical guard, so
  PRRTE's line would have redefined a live macro with a different body:
  `-Wmacro-redefined`, which `--enable-devel-check` makes an error. Nothing
  caught it because the whole block only compiles where the libc function is
  absent, which is nowhere anyone builds. `test/unit/include` now takes the
  address of all four, so at least the names have to exist.

Two blocks that used to live here are gone, both unreachable by
construction rather than merely unused:

- A `__func__` fallback defining it to `__FILE__`, guarded on
  `AC_CHECK_DECLS(__func__)`. C11 mandates `__func__` (6.4.2.2) and PRRTE
  requires C11.
- A VxWorks block including `<ioLib.h>`, `<sockLib.h>` and `<hostLib.h>`,
  guarded on `MCS_VXWORKS` — a symbol that appears nowhere else in the tree
  and that nothing under `config/` has ever defined.

The configure checks that existed only to feed them (`AC_CHECK_DECLS`
for `__func__`, and the three `AC_CHECK_HEADERS` entries) went with them.
When you delete a guard here, delete its probe too — an emitted symbol with
no reader is the state that made both of these survive this long. On the
same principle, `PRTE_HAVE_ATTRIBUTE_DEPRECATED_ARGUMENT` and its
`_PRTE_CHECK_SPECIFIC_ATTRIBUTE` probe are gone from
`config/prte_check_attributes.m4`: `prte_config_bottom.h` never grew a
`__prte_attribute_deprecated_argument__` wrapper for it, so the configure
result had nowhere to go. (PMIx emits the same symbol and likewise has
neither a wrapper nor a use — so "parity with PMIx" would only have meant
keeping the same dead check in two places.)

**And the whole file used to sit inside `#if OMPI_BUILDING`.** That is an
Open MPI inheritance: there, `mpi.h` set `OMPI_BUILDING` to `0` before
pulling in the config header, so none of the portability layer —
`#define snprintf pmix_snprintf`, `#define sockaddr_in6 sockaddr_in`,
`#define sin6_addr sin_addr`, the `static inline htonl` definitions, the
redefinition of Apple's `__PRI_64_LENGTH_MODIFIER__` — landed in an MPI
application's namespace, and an `#else` arm stripped the `PACKAGE_*` macros
back out for it. The fence earns its keep in Open MPI. In PRRTE it had no
"outside": there is no `mpi.h`, nothing ever set it to `0`, and with
`--with-devel-headers` gone PRRTE installs no header an external consumer
could include at all. So the block always compiled, the `#else` arm was
unreachable, and an `OMPI_`-prefixed symbol sat in PRRTE's most-included
header against the project's own symbol-prefix rule. Removing it changes
nothing: the preprocessed output of `prte_config.h` is byte-identical
before and after.

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

### PRRTE's headers are not installed, anywhere

`Makefile.am` here appends its files to `headers`, which
[`src/Makefile.am`](../Makefile.am) feeds to `libprrte_la_SOURCES`. That is
what puts them in the tarball and makes them tracked build dependencies —
**not** an install rule. There is no install rule. Do not add one.

`prte_config.h` and `version.h` are deliberately not in that list either:
they are generated into the build tree and `AC_CONFIG_HEADERS` /
`AC_CONFIG_FILES` already own them.

There used to be a `--with-devel-headers` option ("for authors doing deeper
integration") that installed all of it under `$(includedir)/prte`. It has
been removed, for the reason that settles the question: **PRRTE ships no
linkable library.** It is a set of executables. There is nothing an
installed header set could be compiled *against*, so there was never a
consumer. The option is an ORTE-era inheritance, from when ORTE lived
inside Open MPI and did have one.

It had also long since stopped working, in two independent ways worth
knowing about because both are easy to reintroduce:

- The generated headers went into a `nodist_headers` variable that **no
  `_HEADERS` variable ever referenced**, so `prte_config.h` was never
  installed — while `prte_config_top.h` and `prte_config_bottom.h`, which
  each `#error` unless `PRTE_CONFIG_H` is already defined, were. Since every
  PRRTE header opens with `#include "prte_config.h"`, nothing in the
  installed tree compiled at all.
- The `mca` framework `Makefile.am` files set `nobase_prte_HEADERS`
  *outside* the `WANT_INSTALL_HEADERS` conditional, so their headers
  installed on every build whether the option was given or not.

And even with the first repaired, it did not work: most `Makefile.am`
fragments never append to `headers`, so `src/event/`, `src/pmix/` and much
of `src/util/` were absent while installed headers included them by path.
Measured before removal: 96 headers installed, 73 of 93 failed to compile
standalone.

---

## Testing

**Unit tests — `test/unit/include/test_include.c`, run by `make check`.**
Covers the error-code band arithmetic (no code is `PRTE_SUCCESS`, the two
bands cannot collide, there is headroom to grow, and `PRTE_ERR_MAX` really
is one past the end), the rank types and their sentinels including the
parenthesization trap, `prte_hton64`/`ntoh64` round-trip *and* their actual
big-endian byte layout — a pair of no-op functions round-trips perfectly and
still puts the wrong bytes on the wire — `PRIsize_t`, the pointer-sized
integer ladder, the `PRTE_PATH_MAX`/`PRTE_MAXHOSTNAMELEN` fallbacks, the
`PRTE_ENABLE_IPV6` narrowing, and the existence of the four PMIx printf
replacements.

**The other half of the error-code coverage is in `test/unit/util`**, because
it needs `prte_strerror()`: `test_error_strings()` sweeps every offset and
fails on a code with no string, and `test_error_bound()` checks that
`PRTE_ERR_MAX` — the bound that sweep runs to — has not gone stale in either
direction. Keeping the bound in `constants.h` and the staleness check next
to the oracle is deliberate; see the `PRTE_ERR_MAX` section above for what
happened when each test kept its own copy.

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
