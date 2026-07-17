# AGENTS.md — The `grpcomm` Framework (Group Communication)

Orientation for AI agents and human contributors working in
`src/mca/grpcomm/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`grpcomm` (Group Communication) provides the **collective communication
services that span the DVM's daemons**: scalable broadcast (`xcast`),
allgather/barrier (`fence`), and PMIx group operations (`group`). Its own
header says it plainly:

> The PRTE Group Comm framework provides communication services that span
> entire jobs or collections of processes. It is not intended to be used
> for point-to-point communications (the RML does that), nor should it be
> viewed as a high-performance communication channel for large-scale data
> transfers.

grpcomm runs **on every daemon** (`prted` and the HNP). It is layered on
top of the RML (Runtime Messaging Layer) and the routing tree: it does not
open its own connections — it sends RML messages along the radix routing
tree that `src/rml/` maintains (`prte_rml_base.children`,
`PRTE_PROC_MY_PARENT`, `PRTE_PROC_MY_HNP`). The framework is the machinery
behind almost every DVM-wide action:

- The `plm`/`state` code broadcasts launch messages, wireup (nidmap), and
  DAEMON commands with `prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, …)` /
  `PRTE_RML_TAG_WIREUP`.
- The PMIx server shim satisfies `PMIx_Fence` /
  `PMIx_server_register_resources` for local clients via
  `prte_grpcomm.fence(...)` (see `src/prted/pmix/pmix_server_fence.c`).
- The PMIx server shim satisfies `PMIx_Group_construct/destruct/cancel`
  via `prte_grpcomm.group(...)` (see
  `src/prted/pmix/pmix_server_group.c`).
- The RML fault handler re-broadcasts `PRTE_RML_TAG_DAEMON_DIED` /
  `PRTE_RML_TAG_DAEMON_REVIVED` through `xcast` as part of DVM recovery.

Everything reaches the framework through a single global module instance,
`prte_grpcomm` (declared in `grpcomm.h`, defined in
`base/grpcomm_base_frame.c`). Callers never talk to a component directly;
they call `prte_grpcomm.<fn>(...)`.

---

## Directory layout

```
grpcomm/
  grpcomm.h                    # THE module/component vtable + prte_pmix_grp_caddy_t
  base/
    base.h                     # framework-global struct (context_id) + select() proto
    grpcomm_base_frame.c       # open/close/register; prte_pmix_grp_caddy_t class
    grpcomm_base_select.c      # single-winner component selection
    static-components.h        # generated: the built-in component list
  direct/                      # the only component (pri 5): RML-tree collectives
```

Read `grpcomm.h` first — the entire framework contract is the one vtable
struct it defines. Then read the `direct` component, which is where all the
real work lives.

---

## The module contract

Unlike most PRRTE frameworks there is no separate "component vtable" and
"module vtable": `grpcomm.h` defines one struct,
`prte_grpcomm_base_module_t`, tagged **`Ver 4.0`**, and the selected
component fills it in. Every function pointer **MUST** be provided.

| Field | Signature | Meaning / return protocol |
|-------|-----------|---------------------------|
| `init` | `int (*)(void)` | Called once on the winning module right after selection. Set up trackers, register RML receives. Returns `PRTE_SUCCESS`. |
| `finalize` | `void (*)(void)` | Tear down trackers and cancel RML receives. Called by the framework close. |
| `fault_handler` | `void (*)(const prte_rml_recovery_status_t *status)` | Invoked by the RML/routed layer (`src/rml/routed_radix.c`) when the routing tree changes — a daemon died, revived, or the local node was re-parented/promoted. Repair or abort in-flight collectives. |
| `xcast` | `int (*)(prte_rml_tag_t tag, pmix_data_buffer_t *msg)` | Scalably broadcast `msg` to **every** daemon in the DVM, to be delivered at `tag`. Non-destructive to `msg` (caller still owns it). Returns `PRTE_SUCCESS` when the broadcast has been *accepted*, not when it completes. |
| `xcast_nb` | `int (*)(prte_rml_tag_t tag, pmix_data_buffer_t *msg, prte_grpcomm_xcast_complete_fn_t cbfunc, void *cbdata)` | Same as `xcast`, but when `cbfunc != NULL` it fires on the **master** once the whole DVM has confirmed receipt (all ACKs have rolled back up the tree). `cbfunc`/`cbdata` are ignored on non-master daemons. `xcast` is just `xcast_nb(tag, msg, NULL, NULL)`. |
| `fence` | `int (*)(const pmix_proc_t procs[], size_t nprocs, const pmix_info_t info[], size_t ninfo, char *data, size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata)` | Non-blocking allgather/barrier across the daemons hosting `procs`. Barrier == NULL data. `cbfunc` is invoked with the gathered buffer on completion. Returns `PRTE_SUCCESS` once queued. |
| `group` | `int (*)(pmix_group_operation_t op, char *grpid, const pmix_proc_t procs[], size_t nprocs, const pmix_info_t directives[], size_t ndirs, pmix_info_cbfunc_t cbfunc, void *cbdata)` | PMIx group construct/destruct/cancel. Basically a fence with enough differences (context-id assignment, membership assembly, bootstrap) to warrant its own path. |

`prte_grpcomm_cbfunc_t` (`void (*)(int status, pmix_data_buffer_t *buf,
void *cbdata)`) is declared in `grpcomm.h` for collective completion, and
`prte_grpcomm_xcast_complete_fn_t` (`void (*)(void *cbdata)`) is the
xcast-completion callback.

The version macro components must stamp is
**`PRTE_GRPCOMM_BASE_VERSION_4_0_0`** (`grpcomm.h`), chained to
`PRTE_MCA_BASE_VERSION_3_0_0("grpcomm", 4, 0, 0)`.

---

## Component selection is "pick one"

`prte_grpcomm_base_select()` (in `grpcomm_base_select.c`) is a **standard
single-winner** selection — the opposite of `rmaps`. It calls
`pmix_mca_base_select("grpcomm", …)`, copies the winning module into the
global `prte_grpcomm`, and calls its `init()`. If no component is selected
it returns `PRTE_ERR_NOT_FOUND`.

Today there is exactly one component, `direct`, with query priority **5**
(it is "always available"). Any replacement would just need to return a
higher priority from its `query`.

---

## What `base/` provides

The base is deliberately thin — all the collective algorithms live in the
component. The base offers only:

### Framework plumbing (`grpcomm_base_frame.c`)

- `PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, grpcomm, "GRPCOMM", …)` wires up
  open/close/register. `prte_grpcomm_base_open()` opens all components;
  `prte_grpcomm_base_close()` calls `prte_grpcomm.finalize()` (if set),
  then closes the components.
- The global `prte_grpcomm` module (zero-initialized until selection) and
  the global `prte_grpcomm_base` struct.

### The framework-global struct (`base.h`)

```c
typedef struct {
    uint32_t context_id;   /* initialized to UINT32_MAX */
} prte_grpcomm_base_t;
```

`context_id` is the **group context-id pool**. When a group construct asks
for `PMIX_GROUP_ASSIGN_CONTEXT_ID`, the HNP hands out
`prte_grpcomm_base.context_id` and **decrements** it (see
`grpcomm_direct_group.c`). It counts *down* from `UINT32_MAX` so that these
DVM-assigned context ids do not collide with ids assigned from the bottom
of the range elsewhere.

### The group caddy class (`grpcomm_base_frame.c` + `grpcomm.h`)

`prte_pmix_grp_caddy_t` is the thread-shift caddy that carries a
`PMIx_Group*` request from the PMIx-server callback thread onto the
progress thread. It embeds the mandatory caddy fields (`ev`, `lock`,
`cbfunc`/`cbdata`) plus the group request parameters (`op`, `grpid`,
`procs`/`nprocs`, `directives`/`ndirs`, `info`/`ninfo`). Its constructor/
destructor (`grpcon`/`grpdes`) manage the lock and free `grpid`/`info`.
The `direct` component allocates and posts these caddies.

### Selection (`grpcomm_base_select.c`)

`prte_grpcomm_base_select()` as described above.

That's the whole base API. There is **no** base-level collective tracking,
signature packing, or xcast plumbing in the live build. If you are looking
for the signature/tracker model, it is component-private (see the `direct`
guide).

---

## The collective model (implemented in the component)

Because all algorithms live in `direct`, the concepts below are documented
in depth in [`direct/AGENTS.md`](direct/AGENTS.md); here is the shape so
the framework makes sense:

- **Routing tree.** Collectives ride the radix routing tree owned by
  `src/rml/` (`prte_rml_base.children` / `n_children`,
  `PRTE_PROC_MY_PARENT`, `PRTE_PROC_MY_HNP`). The HNP is the root of every
  collective.
- **Signatures.** A collective is identified not by a global counter but
  by a *signature* — for `fence`, the sorted set of participating procs;
  for `group`, the `groupID` + operation; for `xcast`, an HNP-assigned,
  globally-unique `op_id`. A signature lets independently-arriving pieces
  of the same collective find each other.
- **Trackers.** Each daemon keeps a per-collective tracker (a
  `prte_grpcomm_fence_t` / `prte_grpcomm_group_t` / xcast `op_t`) on a
  component list, counting how many contributions it `nexpected` vs.
  `nreported`. When a tracker completes locally it rolls up to the parent;
  when the HNP's tracker completes, it broadcasts the release via `xcast`.
- **Two-phase collective.** `fence`/`group` are an **up-tree allgather**
  (children → parent → … → HNP) followed by a **down-tree release**
  (`xcast` from the HNP to everyone). `xcast` itself is the down-tree half,
  made reliable with an ACK rollup.

---

## Conventions, threading, and gotchas

- **Everything runs on the single progress thread.** Every entry point
  (`fence`, `group`, `xcast_nb`) immediately thread-shifts: it allocates a
  heap caddy/op, `prte_event_set(prte_event_base, &cd->ev, …)`,
  `PMIX_POST_OBJECT`, `prte_event_active`. The framework-global data
  (trackers, `context_id`, xcast op lists) is only touched inside those
  handlers. Follow that pattern — never touch tracker lists from a caller
  thread. See the top-level guide's *Thread-shifting with caddies*
  section; the caddy's event member must be named `ev`.
- **RML receives are persistent.** The component registers
  `PRTE_RML_PERSISTENT` receives for each collective tag in `init()` and
  cancels them in `finalize()`. The tags are `PRTE_RML_TAG_XCAST`,
  `..._XCAST_ACK`, `..._FENCE`, `..._FENCE_RELEASE`, `..._GROUP`,
  `..._GROUP_RELEASE`.
- **`xcast` returns on acceptance, not completion.** If you need to know
  the whole DVM received a broadcast, use `xcast_nb` with a callback — that
  is the hook the elastic DVM-shrink path uses to run its single
  routing-tree repair.
- **Non-destructive to the caller's buffer.** `xcast`/`fence` copy the
  payload; the caller keeps ownership of the `pmix_data_buffer_t` it
  passed.
- **Capability-guarded FT code.** Group fault-tolerance (the
  `PMIX_GROUP_CANCEL` operation and cancel routing) is compiled only when
  `#if PRTE_PMIX_HAVE_GROUP_FT`. That macro is defined by
  `config/prte_setup_pmix.m4` via `PRTE_CHECK_PMIX_CAP([GROUP_FT], …)`,
  which succeeds when the installed PMIx advertises `PMIX_CAP_GROUP_FT`.
  Any new group-FT code must live behind that guard, and you must build
  against a new-enough PMIx (and re-run `autogen.pl`) to exercise it.
- **Every entry-point handler owns its caddy/op — release it on *all*
  paths.** `fence`/`group`/`xcast_nb` each allocate a heap object
  (`prte_pmix_fence_caddy_t`, `prte_pmix_grp_caddy_t`, or the xcast `op_t`),
  thread-shift it to the handler, and cache only the *callback pointers*
  (`cbfunc`/`cbdata`) into the long-lived tracker. The caddy/op itself is
  therefore consumed by the handler and must be `PMIX_RELEASE`d before it
  returns — on the success path just as much as the error paths. It is not
  parked on any list and the tracker does not own it, so a missing release
  is a per-collective leak in a hot path (this exact bug existed in
  `begin_xcast`, the non-cancel `group()` returns, and `fence_recv`'s
  `info` array). When you add a return to one of these handlers, ask "what
  did I allocate that nothing else owns yet?" and free it.
- **Unpack failures must bail, not fall through.** The recv handlers start
  by unpacking a signature into a NULL-initialized pointer; on failure the
  unpack helper leaves that pointer NULL. Every such failure must `return`
  immediately — the very next step dereferences the signature, so a logged
  error without a return is a NULL-deref crash on a truncated/corrupt RML
  message.
- **Standard PRRTE rules apply:** `prte_config.h` first, constant-on-left
  comparisons, braces on every block, `PRTE_ERROR_LOG`/`PMIX_ERROR_LOG`
  for unexpected errors, no new compiler warnings.

---

## Testing

A structural unit test lives at
[`test/unit/grpcomm/test_grpcomm.c`](../../../test/unit/grpcomm/) and is
wired into `make check`. It cannot drive a real collective (that needs a
live DVM — use the integration/dockerswarm harnesses), but it guards the
invariants that hold with no DVM: the direct module's vtable is fully
wired, the component is named `direct`, `prte_grpcomm_base.context_id`
starts at `UINT32_MAX`, and every signature/tracker/caddy class
constructs with the documented defaults (all rollup counters zero, the
group tracker's info-lists opened) and destructs without leaking or
crashing. Extend it when you add a class or change a constructor default.

---

## Debugging

```sh
prte --prtemca grpcomm_base_verbose 5 ...   # trace collective progress
prte --prtemca plm_base_verbose 5 ...       # daemon launch / xcast of launch msg
prte --prtemca state_base_verbose 5 ...     # job-state transitions that drive fences
prte --prtemca routed_base_verbose 5 ...    # routing-tree (children/parent) view
```

Framework verbosity ≥1 already narrates each `xcast`, `fence`, and `group`
call and its rollup counts (`nexpected` vs `nreported`); ≥5 adds per-child
relay/ack traffic. Because collectives ride the routing tree, a "collective
hang" is almost always a routing-tree problem — check `routed_base_verbose`
and the fault handlers first.

---

## Where to go next

- [`direct/AGENTS.md`](direct/AGENTS.md) — the one and only component;
  read it for the actual xcast/fence/group algorithms, the op-id
  sequencing and ACK rollup, and the group construct/cancel/FT handling.
