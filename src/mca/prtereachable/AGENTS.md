# AGENTS.md — The `prtereachable` Framework (Interface Reachability)

Orientation for AI agents and human contributors working in
`src/mca/prtereachable/`. This is a map, not the rulebook: the
authoritative project guidance lives in the top-level
[`AGENTS.md`](../../../AGENTS.md) and under [`docs/`](../../../docs/).
When this file and those disagree, **the docs win** — and please fix
this file.

---

## What this framework does

`prtereachable` answers one narrow question: **given a list of my local
network interfaces and a list of a peer daemon's remote interfaces,
which local/remote pairs can actually talk to each other, and how good
is each pairing?** It does *not* open sockets, and it does *not* pick the
one winning pair — it produces a **scoring matrix** and hands it back to
the caller, which does the picking.

The single consumer today is the OOB TCP transport
([`src/rml/oob/`](../../../src/rml/oob/)). When a daemon needs to open a
TCP connection to a peer, `prte_oob_tcp_peer_try_connect()`
(`oob_tcp_connection.c`) builds a `pmix_list_t` of local `pmix_pif_t`
interfaces and a `pmix_list_t` of the peer's remote `pmix_pif_t`
addresses, then calls:

```c
results = prte_reachable.reachable(local_list, remote_list);
```

The returned `prte_reachable_t` carries a `num_local × num_remote`
integer weight matrix. The OOB then simply scans for the highest-scoring
`[i][j]` cell, uses local interface `i` to bind and remote address `j` to
connect, and — if that connection attempt fails — zeroes that cell and
picks the next-highest, until it connects or the matrix is exhausted (a
best-of `0` means "no reachable pair"). That selection loop lives in the
consumer, not here; this framework's whole job is to fill the matrix.

**Where it runs.** The framework is opened and a module selected during
`ess` init for the two daemon roles that route OOB traffic: the HNP
(`ess/hnp/ess_hnp_module.c`) and every prted
(`ess/base/ess_base_std_prted.c`). Both call
`prte_reachable_base_select()` right after opening the framework, and
close it at finalize. There is no public library API — like all of
PRRTE, this is internal machinery.

---

## Directory layout

```
prtereachable/
  prtereachable.h                    # the prte_reachable_t matrix + module/component vtable types
  base/
    base.h                           # framework global + prte_reachable_base_select / _allocate protos
    reachable_base_frame.c           # framework open/close/register; defines the prte_reachable global
    reachable_base_select.c          # prte_reachable_base_select: single-winner component pick + init
    reachable_base_alloc.c           # prte_reachable_t class (construct/destruct) + prte_reachable_allocate
    static-components.h              # generated: array of built-in components
  weighted/                          # PORTABLE fallback (pri 1): score by address family + subnet match
  netlink/                           # LINUX ONLY (pri 50): score by rtnetlink kernel route lookup
```

Read `prtereachable.h` first — it is short and defines every type the
framework trades in. Then `reachable_base_alloc.c` (how the matrix is
built) and `reachable_base_select.c` (how the module is chosen).

---

## The module contract

Every component supplies one `prte_reachable_base_module_t` (declared in
`prtereachable.h`) — a three-function vtable:

```c
typedef struct {
    prte_reachable_base_module_init_fn_t     init;
    prte_reachable_base_module_fini_fn_t     finalize;
    prte_reachable_base_module_reachable_fn_t reachable;
} prte_reachable_base_module_t;
```

| Function | Signature | Meaning / return protocol |
|----------|-----------|---------------------------|
| `init` | `int (*)(void)` | Called once by `prte_reachable_base_select()` on the winning module. Returns `PRTE_SUCCESS` or an error (which fails select). Both shipped components just bump a static init counter. |
| `finalize` | `int (*)(void)` | Tear-down counterpart to `init`. Returns `PRTE_SUCCESS`. Note: nothing in the framework currently calls `finalize` — it exists in the vtable but the base never invokes it. |
| `reachable` | `prte_reachable_t *(*)(pmix_list_t *local_ifs, pmix_list_t *remote_ifs)` | **The real work.** Build and return the scoring matrix, or `NULL` if it could not be allocated. |

The `reachable` inputs are lists of `pmix_pif_t` (PMIx's network-interface
descriptor). Per `prtereachable.h`, the **local** interfaces must be
fully populated; the **remote** interfaces only need `af_family`,
`if_addr` (a `struct sockaddr_storage`), `if_mask`, and `if_bandwidth`
set. (The consumer in `oob_tcp_connection.c` synthesizes the remote list
from the peer's advertised addresses and stamps `if_bandwidth = 1`
because it carries no real bandwidth data across the wire.)

The `reachable` function **does not choose** the best pairing — the
header comment is explicit about this. It only assigns a *comparable*
weight to every `(local, remote)` cell so the caller can rank them.

---

## The output: `prte_reachable_t` and its weight matrix

Defined in `prtereachable.h`:

```c
struct prte_reachable_t {
    pmix_object_t super;
    int    num_local;   /* rows: # local interfaces passed in  */
    int    num_remote;  /* cols: # remote interfaces passed in */
    int  **weights;     /* weights[local_idx][remote_idx]      */
    void  *memory;      /* \internal single backing allocation */
};
```

`weights[i][j]` is the connectivity of local interface `i` to remote
interface `j`, a value in `[0, INT_MAX]` where **higher is better** and
**`0` means "no connection possible."** Row/column indices correspond to
list-iteration order of the `local_ifs` / `remote_ifs` lists — the
consumer walks the same lists in the same order to map a winning `[i][j]`
back to the actual `pmix_pif_t` / peer address.

It is a reference-counted PMIx object (`PMIX_CLASS_DECLARATION`), so
create with `PMIX_NEW` (or via `prte_reachable_allocate`, below) and
dispose with `PMIX_RELEASE`.

---

## What `base/` provides

The base is deliberately tiny — three source files, two public helpers,
and the framework boilerplate. There is no orchestration layer here (all
the intelligence is in the components); the base only selects a component
and owns the matrix's memory management.

### `reachable_base_frame.c` — framework plumbing

Standard MCA framework scaffolding via
`PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, prtereachable, …)`:
`open` opens all available components, `close` closes them, `register` is
a no-op. This file also **defines the framework-global module**:

```c
prte_reachable_base_module_t prte_reachable = {0};
```

`prte_reachable` is the vtable the rest of the tree calls through (e.g.
`prte_reachable.reachable(...)`). It stays zeroed until
`prte_reachable_base_select()` copies the winning module into it.

### `reachable_base_select.c` — `prte_reachable_base_select()`

Unlike `rmaps` (which keeps *all* components), this framework does a
**classic single-winner selection**. It calls `pmix_mca_base_select()`
over the open components; the highest-`priority` component whose `query`
succeeds wins. Then it:

1. copies the winner's module struct into the `prte_reachable` global
   (`prte_reachable = *best_module;`), and
2. calls `prte_reachable.init()` and returns its result.

If no component is available it returns `PRTE_ERR_NOT_FOUND`. The `ess`
callers treat a failed select as a fatal init error.

### `reachable_base_alloc.c` — the matrix allocator + class

`prte_reachable_allocate(num_local, num_remote)` is the helper both
components use to build their result. It `PMIX_NEW`s a `prte_reachable_t`,
sets `num_local`/`num_remote`, and then does **one** `malloc` for the
entire jagged 2-D array — the `int *` row-pointer vector *and* all the
`int` cells live in a single block pointed to by `reachable->memory`:

```c
memory = malloc(sizeof(int *) * num_local + num_local * (sizeof(int) * num_remote));
```

The row pointers are then fixed up to index into that block. This is a
deliberate single-allocation design to avoid a storm of little mallocs.
The class `construct` nulls `weights`; the `destruct` frees `memory`
(and only `memory` — never the individual rows, because they are not
separately allocated). **Gotcha:** if you ever change the matrix layout,
keep allocation and free in lockstep in this one file — freeing a row
pointer would corrupt the heap.

`prte_reachable_allocate` returns `NULL` if the backing `malloc` fails;
both components propagate that `NULL` straight out of `reachable`.

---

## Component selection (priority)

`pmix_mca_base_select` picks the single highest-priority component whose
`query` returns a module. Priorities are **hard-coded in each
component's `query` function** — note there is **no** per-component
`priority` MCA parameter (both components' `component_register` are
no-ops), so you cannot retune this from the command line except by
including/excluding components with the framework-level
`prtereachable` MCA param.

```
netlink 50   >   weighted 1
```

So on a Linux box where the `netlink` component was built (its
`configure.m4` found libnl-route-3), netlink wins. Everywhere else — and
on Linux builds where libnl is unavailable, so netlink was never
compiled in — the portable `weighted` component (priority 1) is the only
one present and wins by default. `weighted` is the universal fallback and
is always built.

---

## Conventions, data structures, gotchas

- **The matrix is scores, not decisions.** Do not add "pick the best
  pair" logic into a component. The consumer owns pairing; a component
  that pre-decides would break the OOB's fail-over-to-next-best loop.
- **Higher weight = better; `0` = unreachable.** Both shipped components
  compute weight as
  `connection_quality * (min(bw_l, bw_r) + 1/(1 + |bw_l − bw_r|))`
  (see `calculate_weight` in each). The bandwidth term gently penalizes
  pairing a fast interface with a slow one; `connection_quality` is the
  large multiplier that encodes "same network vs. different vs. none."
  Because weights are integers, `connection_quality` is deliberately
  large so the fractional bandwidth term survives truncation.
- **Row/column order is list order.** `weights[i][j]` follows the
  iteration order of the input lists. The consumer relies on this to map
  indices back to interfaces — don't reorder inside a component.
- **`finalize` is defined but never called.** Don't rely on it running;
  keep components stateless enough that a missing finalize is harmless
  (both just decrement a counter).
- **IPv6 paths are conditional.** All IPv6 handling in both components is
  wrapped in `#if PRTE_ENABLE_IPV6`. Keep new address-family code behind
  the same guard so non-IPv6 builds stay warning-free.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on
  every block, `NULL ==`/constant-on-left comparisons, no new compiler
  warnings, `PRTE_EXPORT` only where cross-unit visibility is needed.
- **The version macro is `PRTE_REACHABLE_BASE_VERSION_2_0_0`**
  (`PRTE_MCA_BASE_VERSION_3_0_0("prtereachable", 2, 0, 0)`). Every
  component's `base_version` must use it.

---

## Debugging

```sh
prte --prtemca prtereachable_base_verbose 20 ...   # trace per-pair scoring
```

Both components emit a per-pair line at verbosity **20** through
`prte_prtereachable_base_framework.framework_output`, e.g.
`reachable:weighted: path from <local> to <remote>: IPv4 PRIVATE SAME NETWORK`
or `reachable:netlink: path from <local> to <remote>: IPv4 DIFFERENT NETWORK`.
That tells you which connection-quality bucket each pair landed in, which
is usually enough to explain why the OOB chose a given interface. To see
which component won selection, raise the framework verbosity and watch
the `pmix_mca_base_select` output.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`weighted/AGENTS.md`](weighted/AGENTS.md) — the portable default;
  read this second. Scores from address family + public/private + subnet
  match, no OS help needed.
- [`netlink/AGENTS.md`](netlink/AGENTS.md) — the Linux-only component
  that asks the kernel's routing table whether a pair is actually
  reachable.
