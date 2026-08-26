# AGENTS.md — The `ras` Framework (Resource Allocation Subsystem)

Orientation for AI agents and human contributors working in
`src/mca/ras/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`ras` (Resource Allocation Subsystem) answers one question at DVM/job
startup: **which nodes, and how many slots on each, has this DVM been
given to work with?** It runs only on the HNP (DVM master) — non-HNP
procs are never allowed to allocate resources, which is why the
framework needs no proxy (see the note in `base/ras_base_frame.c`). It
is the first substantive step of the job-launch state machine:

```
PRTE_JOB_STATE_INIT → ALLOCATE → MAP → LAUNCH_DAEMONS → RUNNING → TERMINATED
                        ▲
                        └── ras runs here
```

The `state` framework fires `prte_ras_base_allocate()` when a job enters
`PRTE_JOB_STATE_ALLOCATE`. On success the job advances to
`PRTE_JOB_STATE_ALLOCATION_COMPLETE`; every failure path advances it to
`PRTE_JOB_STATE_ALLOC_FAILED`. The product of a successful allocation is
a populated **global node pool** (`prte_node_pool`, a
`pmix_pointer_array` of `prte_node_t`) — one entry per allocated node,
each carrying `slots`, `slots_max`, `slots_inuse`, a `state`, and flags
such as `PRTE_NODE_FLAG_SLOTS_GIVEN`. `prte_ras_base.total_slots_alloc`
holds the sum, and it is copied into `jdata->total_slots_alloc`. The
mapper (`rmaps`) consumes this pool at the next state.

Two situations the framework must serve (see the block comment atop
`ras.h`):

1. **Managed allocation** — the user already asked a scheduler (SLURM,
   PBS, LSF, …) for nodes before running `prte`/`prun`; the allocation
   arrives via environment variables. The right component recognizes its
   RM's env and reads the nodelist.
2. **Unmanaged allocation** — no scheduler; nodes come from
   `--host`/`--hostfile`/default-hostfile, or from nothing (fall back to
   the local host as a 1-slot node).

The framework also owns **dynamic allocation changes** at runtime
(reservations, add-host, elastic grow/shrink) via the `modify` path —
see below.

---

## Directory layout

```
ras/
  ras.h                    # module/component vtable + PRTE_MCA_BASE_VERSION(ras)
  base/
    base.h                 # framework-global struct (prte_ras_base) + all base API prototypes
    ras_base_frame.c       # framework open/close/register; ras_base MCA params; globals
    ras_base_select.c      # priority-ordered component selection (keeps ALL, like rmaps)
    ras_base_allocate.c    # THE driver + node-pool display + modify/reservation machinery
    ras_base_node.c        # prte_ras_base_node_insert: dedup nodes into the global pool
    help-ras-base.txt      # user-facing error text
  slurm/                   # SLURM (SLURM_NODELIST) — pri 50; full elastic modify support
  pbs/                     # PBS/Torque/Cobalt (PBS_NODEFILE) — pri 100 (param)
  lsf/                     # LSF (lsb_getalloc) — pri 75
  gridengine/              # SGE/Grid Engine (PE_HOSTFILE) — pri 100 (param)
  flux/                    # Flux (resource.R via KVS) — pri 100 (param); optional build
  pmix/                    # query allocation from a host PMIx scheduler — pri 20
  hosts/                   # dash-host / hostfile / default localhost — pri 1 (catch-all)
  bootstrap/               # launcher-less bootstrap DVM node set — pri 20 (gated)
  simulator/               # fabricate a synthetic allocation for testing — pri 1000 (gated)
  testrm/                  # fake RM: read a fixed hostfile — pri 1000 (gated)
```

Read `ras.h` for the vtable, then `base/ras_base_allocate.c` (the
driver) and `base/ras_base_node.c` (where nodes actually land in the
pool). Then read `hosts/` (the default) and one RM component (`slurm/`).

---

## The module contract

Every component exposes a `prte_ras_base_module_t`
(`struct prte_ras_base_module_2_0_0_t` in `ras.h`) with up to six entry
points; only `allocate` is universally implemented:

```c
typedef int (*prte_ras_base_module_init_fn_t)(void);
typedef int (*prte_ras_base_module_allocate_fn_t)(prte_job_t *jdata, pmix_list_t *nodes);
typedef pmix_status_t (*prte_ras_base_module_modify_fn_t)(prte_pmix_server_req_t *req);
typedef void (*prte_ras_base_module_shrink_complete_fn_t)(prte_shrink_campaign_t *campaign);
typedef int (*prte_ras_base_module_release_fn_t)(prte_session_t *session);
typedef int (*prte_ras_base_module_finalize_fn_t)(void);
```

| vtable slot | Meaning |
|-------------|---------|
| `init` | Optional one-time setup at selection (SLURM allocates its session stack here; most components leave it NULL). |
| `allocate` | Discover nodes and **append `prte_node_t` objects to `nodes`** (a `pmix_list_t`). Does *not* touch the global pool — the base does that. |
| `modify` | Serve a runtime allocation change (`PMIX_ALLOC_NEW`/`EXTEND`/`RELEASE`/`REQ_CANCEL`) carried in a `prte_pmix_server_req_t`. |
| `shrink_complete` | Called once an elastic shrink campaign has drained, so the RM can hand freed nodes back to the scheduler. |
| `release_allocation` | Called when a `prte_session_t` is destructed, so the RM can release that session's allocation. |
| `finalize` | Teardown at framework close. |

### The `allocate()` return protocol

The driver (`prte_ras_base_allocate`) walks the selected modules in
priority order and interprets the return code — it is a protocol, not
just success/failure:

| Return | Meaning in the driver |
|--------|----------------------|
| `PRTE_SUCCESS` | Got an allocation; **stop** cycling modules and insert the nodes. |
| `PRTE_ERR_TAKE_NEXT_OPTION` | "Not my environment / nothing to contribute" — try the next module. **Not an error.** |
| `PRTE_ERR_ALLOCATION_PENDING` | An async allocation request is underway; do nothing and return (a later event resumes). |
| `PRTE_EXISTS` | A fixed allocation was already discovered (e.g. SLURM sees the jobid's session already exists); skip insertion, jump to display. |
| anything else | A real error: `PRTE_ERROR_LOG` it and activate `PRTE_JOB_STATE_ALLOC_FAILED`. |

Because of `TAKE_NEXT_OPTION`, **the first thing a component does is
decide whether the job is for it**. RM components gate on their env var
and bail with `TAKE_NEXT_OPTION` (or a hard error) when absent.

The `modify()` path has its own protocol (`prte_ras_base_modify`):
`PMIX_SUCCESS`/`OPERATION_IN_PROGRESS` → module owns the callback;
`PMIX_OPERATION_SUCCEEDED` → done atomically, base completes it;
`PMIX_ERR_TAKE_NEXT_OPTION`/`PMIX_ERR_NOT_SUPPORTED` → try next module.
`modify` requests can be keyed to a single component via `req->key`
(matched case-insensitively against the component name).

A module that grows the DVM answers `OPERATION_IN_PROGRESS` and lets the grow
campaign's `PMIX_DVM_IS_READY` be the result, since the nodes are unusable until
daemons are up on them. `setup_virtual_machine` resolves that event's recipients
**from the new nodes** — `node->session`, else the session named by the node's
`PRTE_NODE_ALLOC_ID` — so a module whose nodes join the general pool must record
`session->requestor` on whatever session tracks the allocation, or neither the
completion nor a launch failure reaches anyone. A campaign collects every
distinct requester among its targets: one campaign can cover several grows.

---

## An allocation has exactly one owner

`prte_ras_base_select()` (in `ras_base_select.c`) queries every component and
builds a **priority-ordered candidate list**, then takes the first candidate
whose `init()` succeeds. That one module is stored in
`prte_ras_base.selected_modules` — still a `pmix_list_t` of
`prte_ras_base_selected_module_t`, holding exactly one entry, so that every
consumer (allocate, modify, release, shrink-complete, finalize) iterates
unchanged. Only the winner is initialized, which is why `ras/slurm` no longer
stands up its elastic bookkeeping in a DVM it is not allocating for. With
`ras_base_verbose > 4` the selector prints the candidates and names the winner.

Query priorities (higher wins):

```
simulator 1000  =  testrm 1000  >  pbs 100  =  gridengine 100  =  flux 100
   >  lsf 75  >  slurm 50  >  bootstrap 20  =  pmix 20  >  hosts 1
```

`hosts` is the catch-all at priority **1**: always available, it handles
`--host`/`--hostfile`/the default hostfile and (via the base) the ultimate
fall-back to a 1-slot local node. The RM components make themselves available
only when their environment is detected (e.g. `slurm` requires a Slurm job id
— see [`common/slurm`](../common/slurm/AGENTS.md)), so at most one RM answers
on a given machine. `pmix` answers only when it has actually been pointed at a
scheduler (a URI, nspace, pid, host, connection order, or the
system-scheduler switch); it used to answer unconditionally, which was
harmless only while every module was kept, and would now shadow `hosts` in
every unmanaged environment — its `allocate()` always returns
`TAKE_NEXT_OPTION`, so nothing would read a hostfile.
`simulator`/`testrm` sit at 1000 so that when explicitly configured they
pre-empt everything.

**This was multi-select until recently, and it never delivered what it was
for.** The idea was a mixed allocator — a scheduler allocation plus extra
hosts from a hostfile — but `prte_ras_base_allocate()` breaks at the first
module that succeeds, so there is no union step and the second allocator never
contributed a node. (Under a scheduler a hostfile is a *filter*; one naming a
node outside the allocation is refused outright.) What the extra modules did
do was serve `prte_ras_base_modify()`: a request the real allocator declined
fell through to a module with no authority over the allocation. `ras/slurm`
answers `PMIX_ERR_NOT_SUPPORTED` to anything that is not EXTEND/RELEASE/CANCEL
and the driver reads that as "ask the next module", so a `PMIX_ALLOC_NEW`
naming hostnames inside a Slurm allocation was served by `ras/hosts` — answered
`PMIX_SUCCESS` with an allocation id minted, while Slurm was never asked and
had granted nothing. The daemon launch on the un-allocated node then failed
and took the DVM down. `docs/todo.rst` records what supporting mixed
allocators would actually take.

## Who may add a node

Each module states, in `prte_ras_base_module_t::scheduler_owned`, whether an
external resource manager owns what it allocated. Slurm, PBS, LSF, Flux,
gridengine and the PMIx scheduler say yes; hosts, bootstrap, simulator and
testrm say no. The base copies the selected module's answer into
`prte_ras_base.scheduler_owned`.

Where it is yes, **PRRTE may select from the allocation but must never add to
it.** `prte_ras_base_add_hosts()` refuses `--add-host`/`--add-hostfile`
outright (`ras-base:add-host-managed`), and refuses when no active component
can serve the directive at all (`ras-base:add-host-unsupported`, which a
bootstrapped DVM reaches). The refusal is issued **before** the request is
posted, and that placement is load-bearing: serving it is asynchronous, the
DVM is marked not-ready and the job parked in the cache, and only the grow's
`VM_READY` re-entry releases it — so a request nothing answers leaves the job
waiting on a DVM that never becomes ready again.

Note what already handles the other direction: a node a scheduler has taken
back is marked `PRTE_NODE_STATE_NOT_INCLUDED` by
`prte_ras_slurm_exclude_shrunk_nodes()`, and both `setup_virtual_machine`
branches and the mapper skip that state. Its pool entry deliberately survives
— the pool index is `PMIX_NODEID` and must never be reused. The only thing
that ever brought such a node back was `--add-host`, because
`prte_ras_base_node_insert()` carries `PRTE_NODE_STATE_ADDED` onto an existing
pool entry; refusing add-host under a scheduler closes that.

**What the refusal took away, and `--activate` gives back.** Refusing
add-host under a scheduler also closed the *legitimate* case it had been
serving by accident: a node the scheduler **did** grant, sitting in the pool
with `node->daemon` NULL because a `--host`/`--hostfile` given at DVM startup
narrowed which pool entries got daemons, or because a released reservation
handed it back without one. `prte_ras_base_activate_hosts()` (the
`--activate` cmd line option, `PRTE_APP_ACTIVATE_HOSTS`) is that operation
and nothing more: it resolves a `--host`-syntax specification against the
node pool, marks the resolved entries `PRTE_NODE_STATE_ADDED` and calls
`prte_ras_base_activate_dvm_grow()`. It inserts no node, changes no slot
count, and calls no module — so `scheduler_owned` does not gate it, and it
cannot name anything the allocation does not already contain. A `:N` slot
extension is **refused** rather than ignored for the same reason: activate
has no authority to set slot counts.

**The same operation, asked for programmatically.** `PMIX_ALLOC_ACTIVATE`
is the allocation directive that says exactly this, naming its nodes with
`PMIX_HOST` and/or `PMIX_HOSTFILE`. `prte_ras_base_modify()` serves it
**itself**, before the module loop, for the same reason `scheduler_owned`
does not gate the command line: there is nothing for a module to do, and
routing it to whichever component owns the allocation would make the answer
depend on the RM in play — refusing under a scheduler the one size change
that is always safe under one. Both forms resolve through
`prte_ras_base_activate_nodes()`, so the syntax, the refusals and the
treatment of an already-joined node are one implementation and cannot drift
apart. The two differ only in how the hostfile arrives: the command line
folds it into the host list as `file=<path>`, the PMIx form carries it as
its own attribute, which is why the shared entry point takes a host
specification and a hostfile path as two arguments.

The programmatic form answers in **two phases**, as an extend does, because
there is no job behind it to make completion observable: the grant is
returned at once and the daemons are reported by the directed
`PMIX_DVM_IS_READY` (or `PMIX_ERR_DVM_MOD`) when the grow campaign drains.
The requester is recorded with `prte_plm_base_add_grow_requester()` rather
than on a session — an activation creates none — and the campaign adopts it
when it is recorded. Where no campaign will exist (outside elastic mode, or
when every named node is already in the DVM) the grant stays the answer and
`PMIX_SUCCESS` is returned at phase one, so nothing waits on an event no one
will send.

Two things about it are load-bearing:

- It runs **after** `prte_ras_base_add_hosts()` in `plm_base_receive.c`, and
  when the same request carries both it deliberately does **not** activate a
  grow of its own. Add-host's grow is asynchronous — the request has to be
  served before the nodes exist — and it launches on every node marked
  `ADDED`, so it sweeps up activate's too. A second grow posted here would
  race ahead of the insertion.
- Resolution is separated from commitment. A specification that fails
  anywhere marks nothing, so a bad token in a later app segment cannot leave
  pool entries marked `ADDED` for a grow that never runs — which is how a
  node gets relaunched later by some entirely unrelated request.

`+e` is **refused** here, with its own message rather than the generic
"invalid relative node syntax" — it is valid `--host` syntax, so a user has
every reason to try it. For `--host` it means "no application process running
on this node", which says nothing about DVM membership: most of what it picks
is already in the DVM, so honoring it would start no daemon and report
success.

The "every one of them" form is `+all` (the `+` prefix is what already marks
a token as *not* a hostname in this grammar). It selects every allocated node
that is not in the DVM, and finding none is success, not an error — the same
rule that makes naming an already-joined node a no-op.

The other indirect form is `file=<hostfile>`, read by
`prte_util_add_hostfile_nodes()` — the real parser, so `^host` exclusions,
aliases and its refusal of relative syntax inside a file all come along. Only
the **names** are used; a `slots=` in the file is not applied, which is both
consistent with the `:N` refusal and with what a hostfile handed to a tool
already does (it selects, it does not resize). Note the tool side matters:
`create_app()` in `prte_app_parse.c` absolutizes a relative `file=` path
against the *tool's* cwd, because the HNP is the process that opens it.

---

## An allocation a spawn brings with it

`PMIX_SPAWN_ALLOC` puts an entire allocation request **on** a spawn: the
value is an array of `pmix_info_t` beginning with the request's directive
(`PMIX_ALLOC_REQ_DIRECTIVE`) and continuing with exactly the info a
standalone `PMIx_Allocation_request` would have carried. The host obtains the
allocation, waits for it, and only then launches — what a launcher does for
itself when invoked outside an allocation, and what a caller otherwise has to
write by hand as "request, wait, read the id, spawn with
`PMIX_SPAWN_TARGET`".

The request itself is nothing new: `prte_ras_base_spawn_alloc()` splits the
array into its directive and its info and hands it to `prte_ras_base_modify`
like any other, so it reaches the same modules with the same semantics. Four
things about the *surroundings* are worth knowing.

- **Who asks.** The requester is the process that asked for the spawn
  (`PRTE_JOB_LAUNCH_PROXY`), not the job being spawned — that job has no
  namespace yet, and an allocation must be owned by somebody who exists. It
  also puts the reservation under the ordinary ownership rules, so the same
  process can target and release it.
- **Where the job waits.** The job is held by the *request* — not parked in
  `prte_cache`, which is drained by whatever DVM-ready event comes next and
  would launch it while its own allocation was still being obtained. Only
  once the answer is in does `prte_plm_base_spawn_alloc_granted()` decide:
  cache it if a grow is now in flight (`prte_ras_base_dvm_is_growing()`), or
  launch it at once if nothing is coming to make the DVM ready again.
- **Where the job runs.** A grant that names a session is resolved through
  the same `resolve_spawn_targets()` a `PMIX_SPAWN_TARGET` goes through, so
  the job maps onto what it was just given. Without that it would map onto
  everything *except* those nodes — a reservation is withheld from general
  use. A grant that names no session (ras/slurm's extend puts its nodes in
  the general pool) needs no target and gets none.
- **The two failures, kept apart.** A refused allocation fails the spawn with
  `PMIX_ERR_JOB_ALLOC_FAILED` and launches nothing; a *malformed* request —
  no directive, a value that is not an info array — is `PMIX_ERR_BAD_PARAM`,
  because nothing was asked of any allocator. And a spawn that fails *after*
  the grant hands the allocation back before its own error is delivered
  (`prte_ras_base_release_spawn_alloc()`, driven from
  `prte_plm_base_spawn_response`): a caller told its spawn failed is entitled
  to conclude it is not holding resources for it, and nothing else would ever
  release them — the job they were obtained for does not exist and never
  will. `PRTE_JOB_SPAWN_ALLOC_ID` is what records that debt; it is dropped
  once the job is running, from which point the allocation is the job's and
  its disposition is the ordinary one for a reservation.

---

## `prte_ras_base_allocate()` — the driver

This state callback in `ras_base_allocate.c` is the heart of the
framework. Its phases:

1. **Reuse guard.** The DVM's initial (daemon-job) discovery is the
   fixed base allocation for the whole session, whoever provided it. If
   `prte_ras_base.allocation_established` is set and this is **not** the
   DVM's own daemon job, the base *reuses* the existing pool and jumps to
   display. Re-running discovery would re-read a hostfile, overwrite
   established per-node slot counts and clear
   `PRTE_NODE_FLAG_SLOTS_GIVEN`, hiding genuine oversubscription from the
   mapper — and re-reading an RM is no better, since a component that has
   already recorded its allocation must spend a return code saying so
   (`ras/slurm` answers `PRTE_EXISTS`) and one that does not would insert
   it twice. (The sanctioned way to grow an allocation is
   add-host/add-hostfile or an allocation request →
   `prte_ras_base_modify`.)
2. **Ask the allocator.** Walk `selected_modules` — one entry — calling
   `mod->module->allocate(jdata, &nodes)`, honoring the return protocol
   above.
3. **Empty-list handling.** If no module contributed and
   `prte_allocation_required` is set → fatal (`ras-base:no-allocation`).
   Otherwise fabricate a single node from `prte_process_info.nodename`
   with `slots = 1`, `state = PRTE_NODE_STATE_UP`, and set
   `prte_hnp_is_allocated`.
4. **Insert.** `prte_ras_base_node_insert(&nodes, jdata)` drains the
   list into the global pool (see below).
5. **Establish + display.** Set `allocation_established = true`; if
   `ras_base_verbose > 4`, dump the allocation
   (`prte_ras_base_display_alloc`). Honor `PRTE_JOB_DISPLAY_TOPO`.
6. **Report + advance.** Copy `total_slots_alloc` into the job, fire the
   `PMIX_NOTIFY_ALLOC_COMPLETE` event if `prte_report_events`, then
   activate `PRTE_JOB_STATE_ALLOCATION_COMPLETE`.

---

## `prte_ras_base_node_insert()` — nodes into the global pool

`ras_base_node.c` is where a component's working list becomes the
authoritative `prte_node_pool`. **NOTE: it removes every item from the
input list.** Walk it carefully before touching allocation code:

- **Multiplier.** `ras_base_multiplier` (default 1) fabricates N copies
  of every node via `prte_node_copy`, to simulate a large cluster from a
  small one; it also stamps `PRTE_JOB_MULTI_DAEMON_SIM` on the job.
- **HNP dedup.** The HNP's own node is already at pool index 0. Any
  incoming node that `prte_check_host_is_local()` matches updates that
  entry in place (slots, `slots_max`, aliases, `rawname`, attributes)
  instead of adding a duplicate, and sets `prte_hnp_is_allocated`.
  `launch_orted_on_hn` + `NO_USE_LOCAL` handling can instead rename the
  HNP node to `"prte"`, flag it `PRTE_NODE_NON_USABLE`, and skip the
  dedup so the head node is left out of mapping.
- **General dedup.** For every other node, an exhaustive `prte_nptr_match`
  scan of the pool decides add-vs-update. `PRTE_NODE_ADD_SLOTS` means
  "adjust the existing slot count" (clamped to `[0, slots_max]`) rather
  than replace it. **On a match the incoming object is released and the
  loop moves on**: the pool entry is authoritative, so the duplicate must
  not be added, must not have its slots counted into
  `total_slots_alloc` a second time, and must not be multiplied. This is
  the hot path on every elastic **re-grow** — a shrink removes the
  daemon but leaves the pool entry, so the regrant always arrives as a
  duplicate. Only `PRTE_NODE_STATE_ADDED` is carried across, because the
  DVM extension keys off it to decide where to launch.
- **Pre-assigned pool slots.** A component may choose a node's pool slot
  by setting `node->index` before handing it over; `node_insert` places
  it there rather than appending to the lowest free slot (falling back
  to an append if the slot is occupied). `ras/bootstrap` is the only
  component that does this, and it needs it — see its guide.
- **`PRTE_NODE_FLAG_SLOTS_GIVEN` is the slot authority, and it is
  per node.** The component that supplies a node says whether its count
  is a given — every RM component sets the flag, the hostfile/dash-host
  parsers set it for an explicit `slots=`/`:N`, and nobody sets it on
  the bare local-host fallback. `node_insert` carries the flag into the
  pool untouched; at launch `plm_base_launch_support` re-sizes only the
  nodes that lack it (from the topology, as `prte_set_slots` directs)
  and sums the job's session nodes into `jdata->total_slots_alloc`,
  which the PMIx server hands out as `PMIX_UNIV_SIZE` and
  `PMIX_MAX_PROCS`. A component that forgets the flag silently hands the
  job its nodes' core counts instead of its allocation.
  `prte_set_slots_override` is the deliberate escape hatch: it re-sizes
  even the given ones.
- **FQDN normalization.** `normalize_node()` truncates an FQDN to the
  short name, keeping the full name as `rawname` and an alias (unless
  `prte_keep_fqdn_hostnames` or the name is an IP). Sets
  `prte_have_fqdn_allocation`.
- **`DO_NOT_LAUNCH` daemons.** When the daemon job carries
  `PRTE_JOB_DO_NOT_LAUNCH` (offline mapper tests), a synthetic
  `prte_proc_t` daemon is attached to each node so the mapper sees a
  daemon without a live launch.
- Bumps `prte_ras_base.total_slots_alloc` by each node's slots — but only
  for a node it actually adds. A node the pool already held is skipped, so a
  `PRTE_NODE_ADD_SLOTS` adjust applied in place does not reach the total.
  That matters less than it looks, and it is worth knowing why before
  "fixing" it: **the framework total is not what a job reports.**
  `prte_ras_base_allocate` copies it into `jdata->total_slots_alloc` at
  `ALLOCATION_COMPLETE`, and `prte_plm_base_daemons_reported` then
  *recomputes* that field from the job's own session nodes at
  `DAEMONS_REPORTED` — which is before anything reads it, so
  `PMIX_UNIV_SIZE` / `PMIX_MAX_PROCS` and the packed launch message all
  carry the recomputed figure. `prte_ras_base.total_slots_alloc` is the
  framework's running description of the pool and nothing else.
- **An error return does not drain the list.** The contract is "removes all
  items", and it holds only on success: an error leaves what it had not
  reached still on the list, so a caller must `PMIX_LIST_DESTRUCT` rather
  than `PMIX_DESTRUCT`. The node in hand is released by `node_insert`
  itself, since by then it is off the list and not yet in the pool and no
  one else could.

---

## `--display-allocation` and cpus

- `prte_ras_base_display_alloc(jdata)` prints the whole `prte_node_pool`
  (name, `slots`, `max_slots`, `slots_inuse`, `state`, flags, aliases),
  in plain or `<allocation>` XML form (`PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT`).
  Guarded by `PRTE_JOB_ALLOC_DISPLAYED` so it prints once.
- `prte_ras_base_flag_string(node)` renders the node flag bitmask
  (`DAEMON_LAUNCHED`, `LOCATION_VERIFIED`, `OVERSUBSCRIBED`, `MAPPED`,
  `SLOTS_GIVEN`, `NONUSABLE`) for that display.
- `prte_ras_base_display_cpus(jdata, nodelist)` prints available
  processors per package for the requested nodes (`--display-cpus`).
  It resolves each requested name with `prte_node_match()`, which is the one
  place that knows how a name reaches a pool entry — the local-host aliases,
  the per-node alias list, and the fact that an entry may carry no name at
  all. It used to spell that walk out for itself and had already drifted:
  it compared `node->name` with no NULL check.
  **Its parseable form has to parse.** It used to emit
  `<processors node=x>` — an unquoted attribute value — wrapping
  `<pkg=0 cpus=0-7>`, which is not an element at all, while
  `--display map:parseable` writes the identical fact as
  `<package id="0" cpus="0-7"/>` in `prte_node_print()`. Two spellings of
  one fact, and the one here could not be read by any XML parser. Keep the
  two in step; the cpu list itself comes from
  `prte_hwloc_base_cpuset2ranges()` for the reason given in
  [`src/hwloc/AGENTS.md`](../../hwloc/AGENTS.md).

---

## Dynamic allocation: modify, reservations, elastic grow/shrink

The bulk of `ras_base_allocate.c` beyond the driver serves runtime
allocation changes. Understand these entry points before touching
elastic-DVM or PMIx_Allocation code:

| Base function | Role |
|---------------|------|
| `prte_ras_base_modify()` | The `modify` driver (also a state callback). Cycles modules (keyed by `req->key`) to serve a `prte_pmix_server_req_t`; on `PMIX_SUCCESS` calls `prte_ras_base_complete_request`, then invokes the requester's `infocbfunc`. |
| `prte_ras_base_add_hosts()` | Collect `PRTE_APP_ADD_HOSTFILE`/`PRTE_APP_ADD_HOST` directives across apps into a `PMIX_ALLOC_EXTEND` request and hand it to `prte_ras_base_modify`. Sets `prte_dvm_ready = false` until processed. |
| `prte_ras_base_activate_hosts()` | Collect `PRTE_APP_ACTIVATE_HOSTS` directives across apps, resolve them against the node pool, mark the resolved entries `PRTE_NODE_STATE_ADDED` and activate a grow. Adds nothing to the allocation, so it is permitted under a scheduler; must run after `prte_ras_base_add_hosts()`. Sets `prte_dvm_ready = false` when it activates a grow of its own. |
| `prte_ras_base_spawn_alloc()` | Serve the allocation request carried by a spawn (`PMIX_SPAWN_ALLOC`), in the name of the spawn's requester. Sets `*posted` when one is in flight - the request then holds the job, and the outcome arrives at `prte_plm_base_spawn_alloc_granted()`/`_failed()`. |
| `prte_ras_base_release_spawn_alloc()` | Give back an allocation obtained for a job that then failed to launch, calling `prte_plm_base_spawn_alloc_released()` when it resolves so the spawn's own error can follow. |
| `prte_ras_base_activate_nodes()` | Resolve one activation — a host specification, a hostfile, or both — against the node pool and mark the resolved entries `PRTE_NODE_STATE_ADDED`, reporting how many entries that changed. Shared by `--activate` and `PMIX_ALLOC_ACTIVATE`. Resolves and marks only: activating the grow is the caller's. |
| `prte_ras_base_complete_request()` | The heavy reservation router. For `PMIX_ALLOC_NEW`/`EXTEND` it resolves the destination `prte_session_t` (honoring `PMIX_ALLOC_TARGET`/`SHARE`/`INHERITANCE`/`ID`/`REQ_ID`), parses `PMIX_ALLOC_NODE_LIST`, inserts the nodes, and attaches them to the reservation (`add_nodes_to_session`). For `PMIX_ALLOC_RELEASE` it tears down a named reservation or xcasts a `PRTE_DAEMON_SHRINK_CMD`. Marks `PRTE_JOB_EXTEND_DVM` and re-launches daemons for grows. |
| `prte_ras_base_release_allocation()` | Session-destruct hook: cycles modules whose `release_allocation` matches `session->alloc_module`. |
| `prte_ras_base_shrink_complete()` | Offers a drained `prte_shrink_campaign_t` to every module's `shrink_complete`. |
| `prte_ras_base_teardown_reservation()` | Drop a reservation's hold on its nodes (clear `node->session` back to the default pool), deregister it, and — if `return_to_scheduler` — shrink its daemon-carrying nodes out of the DVM. |
| `prte_ras_base_check_reservations_on_term()` | On namespace termination, fire each reservation's inheritance disposition (`PMIX_ALLOC_INHERIT_NONE`/`CHILD`/`CHILD_DEFAULT`/`DEFAULT`). |

### A request's directives arrive from a client and are typed by a client

`req->info` is the requester's own array: `pmix_server_alloc_fn` hands the
client's `pmix_info_t[]` straight through, and nothing between there and here
inspects it. So **never read `value.data.<member>` without first establishing
the type**. A `PMIX_ALLOC_ID` sent as an integer, taken as `.string`, is a
wild pointer, and the first `strdup`/`strcmp`/`PMIX_LOAD_NSPACE` of it faults
the **HNP** — i.e. any process or tool that can connect to the DVM could take
it down with a one-line allocation request. `ras_base_get_string()` is the
string reader every directive in `ras_base_allocate.c` goes through, and it
answers `PMIX_ERR_BAD_PARAM` rather than ignoring a mistyped key, because a
caller that mistyped one needs to be told and not quietly handed a
reservation it cannot name. `prte_ras_base_parse_node_list()` does the same
for `PMIX_ALLOC_NODE_LIST`.

`PMIX_ALLOC_INHERITANCE` has **two** legitimate spellings and both must be
accepted: `PMIX_ALLOC_INHERIT`, the attribute's own type and the one the PMIx
documentation's example uses, and a plain integer of some width.
`PMIx_Value_get_number()` knows only the second, so it cannot be the whole
answer on its own. `prte_pmix_value_get_inheritance()`
([`src/pmix/pmix-internal.h`](../../pmix/pmix-internal.h)) is the single
reader — the disposition decides whether a reservation's nodes go back to the
scheduler, so it must be the one the caller sent and not one a type confusion
produced.

### What a failed request must leave behind

- **A `PMIX_ALLOC_NEW` that fails after `ras_base_prepare_grow()` created its
  reservation unwinds it** (`prte_ras_base_teardown_reservation` +
  `PMIX_RELEASE`, exactly as `pmix_server_session.c` unwinds a half-built
  session). Otherwise the requester is told the request failed, never learns
  the allocation id, and can therefore never release a reservation that is
  still registered and still holding a reference on the owner job. Nodes an
  earlier `PMIX_ALLOC_NODE_LIST` already contributed stay in the pool — a
  pool index is a `PMIX_NODEID` and is never reused — and revert to the
  general pool still marked `PRTE_NODE_STATE_ADDED`, so the next grow adopts
  them, which is the right answer for nodes the allocator did grant.
- **A shrink that names no daemon is never broadcast.** A release may
  legitimately name only nodes that carry none — one a previous shrink handed
  back, one the DVM was never extended onto. Sending the command anyway is
  not harmless: the receiver sizes its target array from the packed count and
  PMIx refuses an unpack of zero values, so *every* daemon in the DVM logs
  `PMIX_ERR_UNPACK_INADEQUATE_SPACE` for a command that asked nothing of it.
  The guard lives in `ras_base_send_dvm_shrink`, which is the one place every
  caller passes through.

### Tearing a reservation down does not free it

`prte_ras_base_teardown_reservation()` gives the nodes back, drops the owners
and the retained owner job, disarms the session's time limit, and removes the
session from `prte_sessions` — but it does **not** release the
`prte_session_t`. It cannot: `prte_job_t::session` and `::target_sessions`
are borrowed pointers taken without a reference, so a job still running in
the reservation would be left holding a dangling one. Deregistering also puts
the object beyond `prte_finalize`'s sweep over `prte_sessions`, so it is
genuinely leaked, one small object per reservation ever torn down. `docs/todo.rst`
records what fixing it properly needs. Disarming the timer *is* done here and
is load-bearing: the limit is on a lifetime that has just ended, and a timer
left armed fires `session_timeout_cb()` on a reservation that no longer
exists and terminates whatever jobs are still recorded in `session->jobs`.

The two callers that *can* release — `pmix_server_session.c`'s `error:` path
and the grow unwind above — do so because the session they are discarding was
built moments earlier and no job has ever seen it.

**A reservation requested by a tool lives and dies with that tool**, and
that hangs on the daemon being told when the tool goes. It is not a child,
so no waitpid fires for it; the connection drop that follows a clean
`PMIx_tool_finalize` raises no lost-connection event either, because PMIx
has already marked the peer finalized. The only notice is PMIx's
`client_finalized` upcall, which reaches tools from
`PMIX_CAP_TOOL_FINALIZED` onwards — `_client_finalized()`
([`src/prted/pmix/pmix_server_gen.c`](../../prted/pmix/pmix_server_gen.c))
turns it into `PRTE_PROC_STATE_TERMINATED` for a job flagged
`PRTE_JOB_FLAG_TOOL`, which is what eventually walks the table above.
Without that the disposition never runs at all, and a grow driven from a
command line strands its nodes for the life of the DVM: out of the general
pool, and unreachable through the reservation as well, since
`prte_session_is_owned_by` will only admit a namespace that no longer
exists.

Elastic shrink is a two-phase collective: `PMIX_ALLOC_RELEASE` records a
`prte_shrink_campaign_t`, xcasts the shrink command with a completion
callback (`shrink_xcast_complete` → thread-shifted
`shrink_campaign_complete`), which does a single batch routing-tree
repair and per-target HNP teardown. This only engages under
`prte_elastic_mode`; outside it the release is fire-and-forget.

---

## Sessions, pools, and the SLURM node→session deviation

A `prte_node_t` carries a `session` backpointer. **`session == NULL`
means the node belongs to the default (unreserved) pool**; a non-NULL,
non-default session means the node is *reserved* and withheld from
general use. The mapper filters nodes by session, so this is how
reservation and elastic-DVM node pools keep jobs on their intended
nodes.

A subtle but deliberate deviation lives in `slurm/ras_slurm_module.c`
(`prte_ras_slurm_assign_new_session`): the SLURM RAS migration creates a
`prte_session_t` per SLURM jobid **to track the allocation group**, and
retains a reference to each real (pool-bound, daemon-carrying) node in
that session — but it **intentionally leaves `node->session == NULL`**.
Those nodes form the DVM's *startup/default* session and must stay in the
general pool; the session object is only a tracking handle for later
identification/release, not a reservation. Do not "fix" this by setting
`node->session` — it would withhold the whole base allocation from
mapping. (This is captured in repo memory: *Node-reservation SLURM
deviation*.)

---

## Globals and MCA params

`prte_ras_base` (`base.h`, defined in `ras_base_frame.c`):

| Field | Meaning |
|-------|---------|
| `selected_modules` | The selected component. A one-entry list so every consumer can iterate it. |
| `scheduler_owned` | Does an external RM own our allocation? Copied from the selected module. |
| `total_slots_alloc` | Sum of `slots` across the pool. |
| `multiplier` | `ras_base_multiplier` — fabricate N daemons/node to simulate scale (default 1). |
| `launch_orted_on_hn` | `ras_base_launch_orted_on_hn` — run a daemon on the head node. |
| `allocation_established` | Latched true once the first allocation completes; drives the reuse guard. |

MCA params: `prte_ras_base_multiplier`,
`prte_ras_base_launch_orted_on_hn` (framework), plus per-component params
documented in each component's guide.

---

## Conventions and gotchas

- **`allocate` appends to the list; the base owns the pool.** Never add
  to `prte_node_pool` from a component — build `prte_node_t`s and append
  to the `nodes` list; `node_insert` places them.
- **Slot conventions.** `slots` = usable slots; `slots = -1` (in the
  `hosts` hostfile parser) is a marker for "not specified, compute from
  topology later". Set `PRTE_NODE_FLAG_SLOTS_GIVEN` only when the count
  is authoritative. `slots_max = 0` means "no max".
- **Return `TAKE_NEXT_OPTION`, not an error, when the env is absent** —
  it is what lets `hosts` (and the local-node fallback) run.
- **The reuse guard is load-bearing** for spawn/child jobs — don't
  bypass it.
- **The framework's `close` hook lives in `ras_base_frame.c`.**
  `prte_ras_base_close` is defined there and wired into the framework
  DECLARE; there is no separate close file. (An orphaned
  `ras_base_close.c` — never in `base/Makefile.am`, referencing
  long-gone `active_module`/`ras_opened` fields — was removed.)
- **`ess/hnp` opens the framework; `prte_finalize` closes it.** That
  split is deliberate and is the one thing to know before moving either
  call. `session_des` → `prte_ras_base_release_allocation` walks
  `selected_modules`, and the sessions are torn down *after*
  `prte_ess.finalize()` has returned, so the close has to wait for them;
  `prte_finalize` does it immediately after that teardown, guarded by
  `PRTE_PROC_IS_MASTER` (a daemon never opens `ras` — only the HNP may
  allocate — so a daemon must never close it either). Closing it is what
  gives every selected module its `finalize`: `ras/slurm` frees its
  session stack, its shrink trackers and its pending extend requests
  there, and until the close existed none of that ever ran. Get the
  order backwards and nothing crashes — `PMIX_LIST_DESTRUCT` leaves the
  list empty but walkable — the allocation is just silently never
  released.
- **A `prte_node_t` only acquires a `topology` when its daemon reports
  in.** Any pool walk that dereferences `node->topology` must NULL-check
  first — the pool routinely holds allocated-but-not-yet-launched nodes
  (a node just added by a grow, or the whole allocation before
  `LAUNCH_DAEMONS`). Both `--display-topo` and `--display-cpus` are pool
  walks.
- **`PMIx_Argv_split()` answers "nothing" with NULL, not with an empty
  array.** A string that is empty or all delimiters — `""`, `";;"` — splits
  to a NULL `char**`, so every caller must check before indexing it. This is
  reachable from user input: `--display topo=';'` used to fault the HNP in
  `prte_ras_base_allocate`'s topology dump.
- **A parser reading an RM's file or env must tolerate malformed input.**
  These run on the HNP, so a NULL deref on a blank line takes the whole
  DVM down. `strtok_r` returns NULL for a short line; `fgets` does not
  guarantee a trailing newline; and every `ctype.h` call needs its
  argument cast to `unsigned char` (a plain `char` is UB for any byte
  with the high bit set).
- Standard PRRTE rules apply: `prte_config.h` first, braces on every
  block, `NULL ==`/constant-on-left, no new warnings,
  `PRTE_ERROR_LOG`/`PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED)`
  on failure.

---

## `--enable-testbuild-launchers` — compile-only, and it requires a DSO

Several ras components are gated on third-party headers that most
machines do not have, so they are silently skipped by nearly every
developer build and every CI job — which is exactly where an edit can sit
broken indefinitely. `ras/flux` needs Flux *and* jansson; `ras/slurm`
compiles a ~1000-line `scontrol --json` parser only when jansson is
found, and **jansson defaults to `--with-jansson=no`**, so the stub is
what almost everyone builds.

`--enable-testbuild-launchers` builds them anyway, against
declaration-only stand-ins:

| Header | Stands in for | Used by |
|--------|---------------|---------|
| [`base/testbuild_jansson.h`](base/testbuild_jansson.h) | `<jansson.h>` | `ras/slurm` (`ras_slurm_jansson.c`), `ras/flux` |
| [`flux/testbuild_flux.h`](flux/testbuild_flux.h) | `<flux/core.h>`, `<flux/hostlist.h>`, `<flux/idset.h>` | `ras/flux` |
| [`lsf/testbuild_lsf.h`](lsf/testbuild_lsf.h) | `<lsf/lsbatch.h>` | `ras/lsf` |

### A stubbed component MUST be a DSO

Nothing behind those declarations is implemented, so the object files
come out with unresolved `lsb_*`/`flux_*`/`json_*`/`hostlist_*`/`idset_*`
references. Where they land decides whether the tree even builds:

- **As a run-time loadable plugin** the unresolved references are fine.
  Creating a shared object does not require them to resolve; the loader
  refuses the `dlopen` later and the framework simply never sees the
  component.
- **Linked statically into `libprrte`** they are fatal. Every PRRTE tool
  links that library, and the link fails on the first one:

  ```
  /usr/bin/ld: ../../../src/.libs/libprrte.so: undefined reference to `flux_open_ex'
  ```

So **every component with a testbuild stub must be in the default
`--enable-mca-dso` list** in [`config/prte_mca.m4`](../../../config/prte_mca.m4)
— today `plm-lsf`, `plm-tm`, `ras-lsf`, `ras-flux` and `ras-slurm`
(`ras/slurm` is on the list because it compiles the jansson parser). That
is also the right layout regardless of the stubs: it keeps a third-party
dependency out of `libprrte` and therefore out of every PRRTE tool.
Adding a stub without adding the component to that list breaks the build
for everyone.

### It must also not *call* a stub

`dlopen` failing is a Linux-and-friends guarantee, not a universal one.
On macOS a plugin is a bundle built with a flat namespace: it loads
happily and the unresolved call becomes a jump to address 0 the moment
someone makes it. So a stubbed component's `query` must decide whether
this machine is even its environment **before** it touches the library —
`getenv("LSB_JOBID")` for `ras/lsf`, `getenv("FLUX_URI")` for `ras/flux`.
That is the framework convention anyway (see the `allocate()` return
protocol above); with a stub it is also what keeps `prte_init` from
segfaulting.

Given both, a tree configured this way builds, links and runs — CI
depends on that, since every build job configures
`--enable-testbuild-launchers` and then runs `make check`, `make install`
and a live `prterun`. What it cannot do is any actual work: nothing is
parsed and no broker is contacted, so re-check such a tree after touching
`ras/flux`, `ras/lsf` or `ras_slurm_jansson.c` (a normal build will not
tell you that you broke them), and do not install one over a good
installation.

Adding a stub is deliberate work, not boilerplate: declare only what the
component actually calls, and keep the signatures faithful. A wrong
prototype here compiles and then mismatches the real library.

---

## Testing

| Layer | What it covers |
|-------|----------------|
| [`test/unit/ras/test_ras.c`](../../../test/unit/ras/) (`make check`) | `prte_ras_base_node_insert` (dedup, drain, slot accounting, `ADD_SLOTS` clamping, FQDN normalization, HNP dedup, pre-assigned pool slots), the module vtable contract for every static component, `prte_ras_base_select` priority ordering, `prte_ras_base_flag_string`, and `ras/slurm`'s detect-and-report half — query gating on `SLURM_JOBID` at priority 50, then `allocate()` expanding a compressed `SLURM_NODELIST` and refusing a tainted jobid or an over-length nodelist. That last part is driven **through the framework** (find the component, query it, call the module it returns) rather than by naming its symbols, because `ras-slurm` is a plugin and has none in `libprrte`; keep it that way. It skips with a printed reason when the component is not there to be found. |
| [`contrib/dockerswarm/run-tests.sh`](../../../contrib/dockerswarm/) (`linux`) | The multi-node paths: grow/shrink/re-grow leaving exactly one daemon per node (a duplicated pool entry launches two), `--add-hostfile` growing a live DVM through `add_hosts → ras/pmix defer → ras/hosts` including the `slots=+N` in-place adjust, `--activate` bringing an allocated-but-idle node into the DVM (one left out by `prte_max_vm_size`, and two a shrink handed back, named through `file=`) while refusing a host the allocation does not contain and refusing to apply the hostfile's `slots=`, the same operation asked for through `PMIX_ALLOC_ACTIVATE` (`elastic activate`, both the `PMIX_HOST` and the `PMIX_HOSTFILE` form, including its two-phase completion), and **`ras/slurm`'s whole modify surface** against a faked scheduler (below). |
| Live RM | PBS/LSF/Flux discovery still needs a real scheduler; there is no substitute. |

**`ras/slurm`'s `modify` surface is covered in `contrib/dockerswarm`, not
in the unit test.
** Extend, release and cancel shell out to
`sbatch`/`scontrol` and only mean anything across several nodes, and each
of them returns `PRTE_ERR_NOT_AVAILABLE` outright unless the component's
**extensions** were built — which needs jansson *and* SLURM 24.05 or later
at configure time (`PRTE_HAVE_SLURM_EXTENSIONS`, see
[`slurm/configure.m4`](slurm/configure.m4) and
[`slurm/AGENTS.md`](slurm/AGENTS.md)). A `make check` build, which by
default has no jansson, cannot reach a line of it. Both container
harnesses pass `--with-jansson` deliberately and are the only automated
builds anywhere that even compile `ras_slurm_jansson.c` — but only
[`contrib/slurmswarm`](../../../contrib/slurmswarm/) *runs* it, against ten
containers holding a real `slurmctld`. `validate_hostname`,
`prte_ras_slurm_drain_cmd_output` and the JSON parser live on that path only
and are exercised there, as are the paths that exist purely to survive a
misbehaving scheduler (a failing `scancel`, unparsable JSON, a request
cancelled while its job is still `PENDING`) — the last of which a live
scheduler produces by itself when asked for more nodes than exist, while the
first two are armed by the wrapper described in
[slurmswarm AGENTS.md §14](../../../contrib/slurmswarm/AGENTS.md).

The unit test builds the global job/node/session arrays by hand (the
real ones come from `prte_init()`, which wants a live ESS) — follow that
pattern rather than trying to bring up a DVM in `make check`.

---

## Debugging

```sh
prte --prtemca ras_base_verbose 5 ...     # trace selection + allocation + node_insert
prun --display allocation ...             # print the job's node list
prte --prtemca ras_base_multiplier 8 ...   # fake an 8x-larger cluster
```

Verbosity ≥5 prints the final component priority list, each module's
allocate attempt, and every node inserted with its slot count — start
there.

Note that `prun --display allocation` renders the **job's** node list
(`rmaps_base_support_fns.c`), not the global pool: nodes held in a
reservation created by an elastic grow are withheld from a general job
and will not appear. The pool dump proper
(`prte_ras_base_display_alloc`) only fires on the daemon job under
`ras_base_verbose > 4`.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`hosts/AGENTS.md`](hosts/AGENTS.md) — the default; read this second.
- [`slurm/AGENTS.md`](slurm/AGENTS.md) — SLURM, with full elastic modify.
- [`pbs/AGENTS.md`](pbs/AGENTS.md) — PBS/Torque/Cobalt nodefile.
- [`lsf/AGENTS.md`](lsf/AGENTS.md) — LSF `lsb_getalloc`.
- [`gridengine/AGENTS.md`](gridengine/AGENTS.md) — SGE `PE_HOSTFILE`.
- [`flux/AGENTS.md`](flux/AGENTS.md) — Flux `resource.R`.
- [`pmix/AGENTS.md`](pmix/AGENTS.md) — query a host PMIx scheduler.
- [`bootstrap/AGENTS.md`](bootstrap/AGENTS.md) — launcher-less bootstrap DVM.
- [`simulator/AGENTS.md`](simulator/AGENTS.md) — synthetic allocation for testing.
- [`testrm/AGENTS.md`](testrm/AGENTS.md) — fixed-hostfile fake RM.
</content>
</invoke>
