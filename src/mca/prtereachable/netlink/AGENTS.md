# AGENTS.md — `prtereachable/netlink` (Linux route-based reachability)

Component guide for `src/mca/prtereachable/netlink/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`prte_reachable_t` weight matrix, and the `prte_reachable_allocate`
helper referenced throughout.

---

## Role and priority

`netlink` is the **Linux-only, higher-fidelity** reachability component,
priority **50**. Instead of guessing from address family and subnet like
`weighted`, it asks the kernel directly: for each local/remote pair it
issues an `RTM_GETROUTE` query over an `AF_NETLINK`/`NETLINK_ROUTE`
socket and inspects the returned route — is there a route to the remote
address out of *this* local interface, and does it go through a gateway
(a different network) or not (same network)?

Because its priority (50) beats `weighted` (1), netlink wins selection
wherever it is built. It targets Linux exclusively.

**It runs on the main progress thread, and it blocks there.** Every entry
into `prte_oob_tcp_peer_try_connect()` arrives through
`PRTE_ACTIVATE_TCP_CONN_STATE` or `PRTE_RETRY_TCP_CONN_STATE`
([`src/rml/oob/oob_tcp_connection.h`](../../../rml/oob/oob_tcp_connection.h)),
and both post to `prte_event_base` — not to the peer's worker base. So the
whole N×M sweep of socket-open / query / receive / socket-close happens
inline on the thread that runs everything else, once per peer the daemon
dials. Nothing here may touch PRRTE globals (it does not), and nothing
here may acquire a long wait: a component that blocked for the full
`SO_RCVTIMEO` on every pair would stall the DVM, which is why the reply
handling below matters more than it looks.

---

## When/why selected — the availability gate

netlink is **conditionally compiled**. Its `configure.m4`
(`MCA_prte_prtereachable_netlink_CONFIG`) requires:

- **libnl-route-3** — via `OAC_CHECK_PACKAGE([libnl_route], …,
  [netlink/route/route.h], [nl-route-3 nl-3], [rtnl_route_get], …)`. The
  m4 includes an "ugly hack" to find libnl3 headers under
  `/usr/include/libnl3` or `/usr/local/include/libnl3` when pkg-config
  doesn't locate them.
- **`linux/netlink.h`** — an `AC_CHECK_HEADER` guard that fails the
  component on non-Linux systems.

If either check fails, the whole component is dropped from the build and
`weighted` becomes the only component. So "netlink is preferred on Linux
when available; weighted is the portable fallback" is enforced at
configure time (component present-or-absent) plus run time (priority 50
vs. 1), not by any MCA parameter.

---

## Files

| File | Contents |
|------|----------|
| `reachable_netlink_component.c` | Registration boilerplate: the `prte_reachable_base_component_t` struct, no-op `open`/`close`/`register`, and `query` returning the module at **priority 50**. |
| `reachable_netlink_module.c` | The module: `netlink_init`/`netlink_fini`, `netlink_reachable` (fills the matrix), and the scoring core `get_weights` + `calculate_weight`. |
| `reachable_netlink_utils_common.c` | The rtnetlink machinery: socket alloc/free, query send, reply parse callback, and the `prte_reachable_netlink_rt_lookup` / `_rt_lookup6` entry points. Adapted from libfabric. |
| `libnl_utils.h` | The socket struct `prte_reachable_netlink_sk` and the `_rt_lookup` / `_rt_lookup6` prototypes. |
| `libnl3_utils.h` | libnl3 shims: `NL_HANDLE` typedef, the `NL_*` macro wrappers (`NL_HANDLE_ALLOC`, `NL_RECVMSGS`, …), and the callback-arg struct `prte_reachable_netlink_rt_cb_arg`. |
| `reachable_netlink.h` | `extern` decls for the component and module. |
| `configure.m4` | The libnl-route-3 + `linux/netlink.h` availability gate. |
| `Makefile.am` | Builds the component only when the gate passed. |

The module vtable is
`const prte_reachable_base_module_t prte_prtereachable_netlink_module = {netlink_init, netlink_fini, netlink_reachable};`.
As with weighted, `init`/`fini` only bump/decrement a static
`init_counter`.

---

## How it fills the matrix (`netlink_reachable`)

Same double-loop shape as weighted: allocate with
`prte_reachable_allocate(local_ifs->pmix_list_length, remote_ifs->pmix_list_length)`
(returns `NULL` on failure), then store `get_weights(local, remote)` into
`weights[i][j]` in list order.

### `get_weights` — per-pair scoring

Connection quality here is a 3-level ladder (a simpler enum than
weighted's):

| Constant | Value | When |
|----------|-------|------|
| `CQ_NO_CONNECTION` | 0 | family mismatch, or the route lookup found no route out this interface |
| `CQ_DIFFERENT_NETWORK` | 50 | route exists but goes through a gateway |
| `CQ_SAME_NETWORK` | 100 | route exists with no gateway (directly connected) |

Flow:

1. **Family must match** (`local_if->af_family == remote_if->af_family`),
   else `CQ_NO_CONNECTION`.
2. **Identical addresses short-circuit to `CQ_SAME_NETWORK`.** If local
   IP == remote IP, assume loopback reachability — done "artificially due
   to historical reasons" to match older implementations, skipping the
   kernel query entirely (`goto out`).
3. **Otherwise query the kernel.** IPv4 calls
   `prte_reachable_netlink_rt_lookup(local_ip, remote_ip, if_kernel_index, &has_gateway)`;
   IPv6 (under `#if PRTE_ENABLE_IPV6`) calls `_rt_lookup6` with the
   `in6_addr`s. The `outgoing_interface` passed in is the local
   interface's `if_kernel_index`. On return:
   - `0 == ret && 0 == has_gateway` → `CQ_SAME_NETWORK`
   - `0 == ret && has_gateway` → `CQ_DIFFERENT_NETWORK`
   - `ret != 0` (e.g. `EHOSTUNREACH`) → `CQ_NO_CONNECTION`

Final weight is `calculate_weight(bw_local, bw_remote, connection_quality)`
— the **identical** formula to weighted:
`connection_quality * (min(bw_l, bw_r) + 1/(1 + |bw_l − bw_r|))`. Each
pair is logged at verbosity 20.

---

## The rtnetlink query flow (`reachable_netlink_utils_common.c`)

`prte_reachable_netlink_rt_lookup` (and its IPv6 twin `_rt_lookup6`) is
the actual kernel conversation. Both return `0` if reachable,
`EHOSTUNREACH` if not, other non-zero on error, and set `*has_gateway`.
Steps:

1. **Alloc a netlink socket** — `prte_reachable_netlink_sk_alloc(&unlsk,
   NETLINK_ROUTE)`: `nl_socket_alloc` → `nl_connect(NETLINK_ROUTE)` →
   disable sequence checking → set a 1-second `SO_RCVTIMEO` receive
   timeout (`prte_reachable_netlink_set_rcvsk_timer`). The socket's
   starting sequence number is seeded from `time(NULL)`.
2. **Build the route message** — a `struct rtmsg` with `rtm_family`
   (`AF_INET`/`AF_INET6`) and `rtm_dst_len`/`rtm_src_len` set to the full
   address bit-width; wrap it in an `RTM_GETROUTE` netlink message
   (`nlmsg_alloc_simple`), then append `RTA_DST` and `RTA_SRC` attributes
   for the remote and local addresses.
3. **Send** — `prte_reachable_netlink_send_query(...)` stamps the header's
   pid/seq and sends with `NLM_F_REQUEST`.
4. **Receive + parse** — register `prte_reachable_netlink_rt_raw_parse_cb`
   as an `NL_CB_MSG_IN`/`NL_CB_CUSTOM` callback, then
   `NL_RECVMSGS` (a macro that calls `nl_recvmsgs_default` and maps a
   `-NLE_AGAIN` timeout to `EHOSTUNREACH`).
5. **Interpret** — after receive, `arg.found` says whether a matching
   route was seen; if so `*has_gateway = arg.has_gateway`, else
   `EHOSTUNREACH`.
6. **Free the socket** (`prte_reachable_netlink_sk_free`) on the way out.

### The parse callback

`prte_reachable_netlink_rt_raw_parse_cb` is where "reachable" is
actually decided:

- validates the reply is expected (pid/seq — under `PRTE_ENABLE_DEBUG`),
  not an error (`NLMSG_ERROR`), and is an `RTM_NEWROUTE` of a supported
  family;
- `nlmsg_parse`s the route attributes against `route_policy`;
- **the key test:** if `RTA_OIF` (outgoing interface index) equals the
  `lookup_arg->oif` we asked for, `found = 1`. If the kernel returns a
  route through a *different* interface, that counts as **not reachable
  via this local interface** (there may be a route, but not out the pair
  we're scoring) — logged at verbosity 20.
- if `found` and an `RTA_GATEWAY` attribute is present,
  `has_gateway = 1` (→ different network).

The callback returns `NL_STOP` after the first route, `NL_SKIP` to
ignore an uninteresting/invalid message.

**`NL_SKIP` on the kernel's error reply does not cost the receive
timeout**, which is the thing to check before "optimizing" it. The
ordinary "no route to that address" answer is an `NLMSG_ERROR` message,
and the callback answers it `NL_SKIP` — which reads as "wait for more",
and with a one-second `SO_RCVTIMEO` on the socket that would be a second
of the main progress thread per unreachable pair. It is not, because
libnl's `recvmsgs()` re-reads the socket only when it has seen
`NLM_F_MULTI`: `NL_SKIP` advances to the next message *already in the
buffer*, and with none left the loop ends and `nl_recvmsgs_default()`
returns 0. Our query is a plain `NLM_F_REQUEST` (never a dump), so the
reply is never multipart and the second read never happens. The
`-NLE_AGAIN` arm of `NL_RECVMSGS` therefore only fires when the kernel
says nothing at all, which is why it is allowed to be loud.

---

## Key structs

| Struct | Where | Purpose |
|--------|-------|---------|
| `prte_reachable_netlink_sk` | `libnl_utils.h` | Wraps the libnl socket handle (`NL_HANDLE *nlh`) and the running sequence number `seq`. |
| `prte_reachable_netlink_rt_cb_arg` | `libnl3_utils.h` | Threads state through the parse callback: requested `oif`, result flags `found`/`has_gateway`/`replied`, and a back-pointer to the socket. |
| `route_policy[]` | `reachable_netlink_utils_common.c` | `nla_policy` table describing the expected route attributes for `nlmsg_parse`. |

---

## Things to watch when editing

- **The `RTA_OIF` match is the whole point.** A route existing is not
  enough — it must exit the *specific* local interface being scored, or
  the pair is not reachable via that interface. Don't relax that test.
- **The two `pmix_pif_t` are always distinct objects.** They come off two
  different lists — the local one is `prte_oob_base.local_ifs`, the remote
  one is synthesized per peer in `oob_tcp_connection.c` — so "is this the
  same address" has to compare the address bytes. The IPv4 branch does
  (`local_ip` and `remote_ip` are `uint32_t` copies); the IPv6 branch holds
  `struct in6_addr *` into the two objects, where `==` is a pointer test
  that is never true.
- **Keep IPv6 behind `#if PRTE_ENABLE_IPV6`.** `_rt_lookup6`,
  `sockaddr_in6` casts, and the `AF_INET6` branch in `get_weights` are
  all conditional; adding IPv6 code outside the guard breaks non-IPv6
  builds.
- **Every lookup allocs and frees its own socket.** `rt_lookup` is called
  once per interface pair, so an N×M matrix opens N×M short-lived netlink
  sockets — and the matrix itself is built once per peer the daemon dials,
  so this *already* scales with daemon count. N and M are small (the
  interfaces on one machine), which is what keeps it affordable; the cost
  to watch is not the socket but anything that can wait, since all of it
  is on the main progress thread.
- **This code is adapted from libfabric.** The BSD license block at the
  top of `reachable_netlink_utils_common.c`, `libnl_utils.h`, and
  `libnl3_utils.h` must be preserved (it is third-party-derived). Keep
  edits minimal and in PRRTE style only where PRRTE-specific.
- **Debug-only diagnostics.** Several `pmix_output`/`nl_msg_dump` calls
  are wrapped in `#if PRTE_ENABLE_DEBUG`; keep noisy netlink dumps behind
  that guard so production builds stay quiet.
- **`query` claims priority 50 without checking that netlink works.** It
  answers "yes, and I outrank weighted" on the strength of having been
  compiled, and the first `socket(AF_NETLINK, ...)` does not happen until
  the first peer is dialed. So in an environment that compiles against
  libnl but refuses `AF_NETLINK` at run time — a sandbox or a seccomp
  profile that filters it — every `rt_lookup` fails, every pair scores 0,
  and the OOB concludes it can reach no peer at all, where `weighted`
  (which asks the kernel nothing) would have worked. This was considered
  and deliberately left alone: no such environment has been seen, the
  failure is loud rather than silent (`sk_alloc` reports the refusal at
  verbosity 0, once per pair), and the escape hatch already exists —
  `--prtemca prtereachable weighted`. If one ever *is* seen, the fix is a
  probe in `query`: open and close one `NETLINK_ROUTE` socket, and decline
  selection if it fails, the same shape as `common/slurm`'s ask-once.
- **`init`/`fini` are counters only.** No real teardown happens, and the
  framework never calls `finalize` anyway — keep the module free of
  process-global state that would leak.
