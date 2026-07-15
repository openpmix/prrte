# AGENTS.md — `prtereachable/weighted` (the portable default)

Component guide for `src/mca/prtereachable/weighted/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`prte_reachable_t` weight matrix, and the `prte_reachable_allocate`
helper referenced throughout.

---

## Role and priority

`weighted` is the **portable, always-built fallback** reachability
component, priority **1** (the lowest). It scores every local/remote
interface pair using only information already in the `pmix_pif_t`
descriptors — address family, whether the address is public vs.
private/link-local, and whether the two addresses share a subnet — with
**no OS-specific help**. It runs anywhere PRRTE builds.

Because its priority is the lowest, it wins selection only when no
higher-priority component (i.e. `netlink`, priority 50) is present. On a
non-Linux platform, or a Linux build where libnl-route was unavailable so
`netlink` was never compiled, `weighted` is the sole component and is
selected by default. It has no availability gate of its own — it is
always compiled and always eligible.

---

## Files

| File | Contents |
|------|----------|
| `reachable_weighted.h` | Component struct type `prte_mca_prtereachable_weighted_component_t` (wraps `prte_reachable_base_component_t`); `extern` decls for the component and module. |
| `reachable_weighted_component.c` | Registration boilerplate: the component struct, `open`/`close`/`register` (all no-ops), and `query` returning the module at **priority 1**. |
| `reachable_weighted.c` | The module: `weighted_init`/`weighted_fini`, `weighted_reachable` (fills the matrix), and the scoring core `get_weights` + `calculate_weight`. |
| `Makefile.am` | Always builds `libprtemca_prtereachable_weighted` (static) or `prte_mca_prtereachable_weighted.la` (DSO). No `configure.m4` — nothing to probe. |

The module vtable is
`const prte_reachable_base_module_t prte_prtereachable_weighted_module = {weighted_init, weighted_fini, weighted_reachable};`.
`init`/`fini` just bump/decrement a static `init_cntr`; there is no real
per-module state.

---

## How it fills the matrix (`weighted_reachable`)

Straightforward double loop. It allocates the result with
`prte_reachable_allocate(pmix_list_get_size(local_ifs), pmix_list_get_size(remote_ifs))`
(returning `NULL` on allocation failure), then for each local interface
`i` and remote interface `j` stores `get_weights(local_iter, remote_iter)`
into `reachable_results->weights[i][j]`. Row/column order follows the
input-list iteration order, as the framework contract requires.

---

## The scoring rules (`get_weights`)

The heart of the component. A pair's weight is
`calculate_weight(bw_local, bw_remote, connection_quality)`, where
`connection_quality` comes from this ladder (enum `connection_quality`):

| Constant | Value | When |
|----------|-------|------|
| `CQ_NO_CONNECTION` | 0 | address-family mismatch, or one-public/one-private, or one-linklocal/one-global |
| `CQ_PRIVATE_DIFFERENT_NETWORK` | 50 | both private IPv4, different subnet |
| `CQ_PRIVATE_SAME_NETWORK` | 80 | both private IPv4 (or both IPv6 link-local), same subnet |
| `CQ_PUBLIC_DIFFERENT_NETWORK` | 90 | both public, different subnet |
| `CQ_PUBLIC_SAME_NETWORK` | 100 | both public, same subnet |

The rationale (from the source comments): same-network implies a single
hop, so it scores highest; public addresses are preferred over private
because the topology is unknown and public routability is a safer bet.
It is all heuristic — the weighted component has no way to *know* the
real topology.

Decision flow:

1. **Address family must match.** If `local` and `remote` are not the
   same `sa_family`, weight is `CQ_NO_CONNECTION` (0). This is the
   catch-all `else` branch labeled `"Address type mismatch"`.

2. **IPv4 (`AF_INET`).** Both public → same-subnet test
   (`pmix_net_samenetwork` against `local_if->if_mask`) picks
   `CQ_PUBLIC_SAME_NETWORK` vs. `CQ_PUBLIC_DIFFERENT_NETWORK`. Both
   private → `CQ_PRIVATE_SAME_NETWORK` vs. `CQ_PRIVATE_DIFFERENT_NETWORK`.
   Public/private mix → `CQ_NO_CONNECTION` (likely not a real match).
   "Public" is decided by `pmix_net_addr_isipv4public`.

3. **IPv6 (`AF_INET6`, only under `#if PRTE_ENABLE_IPV6`).** Both
   link-local (`pmix_net_addr_isipv6linklocal`) → treated as
   `CQ_PRIVATE_SAME_NETWORK`. The code deliberately assumes two
   link-local addresses are on the same network even though it cannot
   verify it, so they get paired preferentially and break the fewest
   connections (see the long comment in the source). Both non-link-local
   → same-subnet test yields `CQ_PUBLIC_SAME_NETWORK` vs.
   `CQ_PUBLIC_DIFFERENT_NETWORK`. Link-local/global mix → `CQ_NO_CONNECTION`.

Each computed pair is logged at verbosity 20 with a human string
(`"IPv4 PUBLIC SAME NETWORK"`, etc.) via the framework output.

### `calculate_weight` — the bandwidth tie-breaker

```
weight = connection_quality * (min(bw_l, bw_r) + 1/(1 + |bw_l − bw_r|))
```

`min(bw_l, bw_r)` favors the higher usable bandwidth (a link is only as
fast as its slower end); the `1/(1+|Δ|)` term slightly penalizes pairing
mismatched-bandwidth interfaces. The result is an `int`, so
`connection_quality` must be large (50–100) for the fractional term to
survive truncation. When both bandwidths are 0 (the common OOB case,
where the remote is stamped `if_bandwidth = 1` and locals may vary) the
score collapses to essentially `connection_quality`, i.e. the
same/different/public/private classification dominates.

---

## Things to watch when editing

- **Keep the address-family gate first.** A mismatched family must score
  0 before any subnet math runs — the IPv4/IPv6 branches assume both
  ends share a family.
- **All IPv6 logic stays behind `#if PRTE_ENABLE_IPV6`.** Adding an
  address-family case outside that guard breaks non-IPv6 builds.
- **`conn_type` must be assigned on every path.** It is used in the
  verbosity-20 log line; a branch that leaves it unset is a warning (and
  a bug) under `--enable-devel-check`.
- **Don't pick a winner here.** Return a *comparable* weight for every
  cell and let the OOB rank them; collapsing ties or short-circuiting to
  a single "best" would break the consumer's fail-over loop.
- **`sockaddr` copies are `sizeof(struct sockaddr)`.** `get_weights`
  memcpys into a `sockaddr_storage` for the samenetwork test; keep the
  storage zeroed first (it is) so the mask comparison is deterministic.
