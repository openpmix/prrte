# AGENTS.md — `src/runtime/data_type_support`

Orientation for AI agents and human contributors. Read
[`../AGENTS.md`](../AGENTS.md) first — the ownership rules there are what
these functions have to honor. The project rules are in the top-level
[`AGENTS.md`](../../../AGENTS.md); when this file and the docs disagree,
**the docs win**.

---

## What this is

Four operations over the runtime's core objects, one file each:

| File | Provides |
|------|----------|
| `prte_dt_packing_fns.c` | `prte_{job,app,proc,node,map}_pack` |
| `prte_dt_unpacking_fns.c` | `prte_{job,app,proc,node,map}_unpack` |
| `prte_dt_copy_fns.c` | `prte_{job,app,proc,node,map}_copy` |
| `prte_dt_print_fns.c` | `prte_{job,app,proc,node,map}_print` |

All of it is plain C over `PMIx_Data_pack`/`PMIx_Data_unpack`; there is no
framework, no registration, and no type-id table. The prototypes live in
[`../prte_globals.h`](../prte_globals.h).

---

## Packing is deliberately partial

The comment at the top of `prte_job_pack` is the important one:

> We do not pack all of the job object's fields as many of them have no
> value in sending them to another location. The only purpose in packing and
> sending a job object is to communicate the data required to dynamically
> spawn another job — so we only pack that limited set of required data.

So this is **not** serialization of a `prte_job_t`. It is the launch
message. `job->map` is packed for the policies it carries, not for the nodes
it mapped (`prte_map_pack` sends mapping/ranking/binding/`num_nodes` and
nothing else - deliberately no mapper name: mapping happens only on the HNP,
so the identity of the component that did it was read by nothing at the far
end). Node procs, topologies, session
backpointers, the session dir, `cli`, and the counters are all left behind.

The two sides are hand-written mirrors with no format version and no
self-description. That is workable only because **mixed-version DVMs are
strictly forbidden** — every process in a DVM comes from the same build, so
the two sides are always the same source (see the top-level
[`AGENTS.md`](../../../AGENTS.md) and [`docs/versions.rst`](../../../docs/versions.rst)).
It also means you may freely add, remove or retype a field, and need keep
nothing for compatibility — but **every field added to the packer must be
added to the unpacker in the same position, in the same PMIx type, in the
same commit.** There is nothing
that will catch a mismatch for you: a type of the same width (`PMIX_INT32`
against `PMIX_UINT32`) silently produces wrong values, and a type of a
different width desynchronizes everything after it.

### The placement travels as maps, not as a per-proc array

`prte_job_pack` does **not** pack a record per process describing where it
went. It packs the job's **node map** — the names of the nodes in
`job->map`, in map order — and then **one proc map per app**: for each of
those nodes, in the same order, the ranks of that app resident on it, with
`"-"` where the app has none there so the two lists stay in step. Both go
through `PMIx_generate_regex2`, so they compress with the number of *nodes*
rather than growing with the number of *processes*.

**A proc map is data, so build it with `snprintf`, never with
`PRTE_NAME_PRINT`/`PRTE_VPID_PRINT`.** Those are display helpers over a
rotating static buffer, and they render each of PMIx's five sentinel ranks
as a *word* — `PMIX_RANK_INVALID` as `"INVALID"`. The decoder reads the
field with `strtoul`, which turns any such word into a **zero**, so a proc
the mapper left unranked — it is on `node->procs`, which is what the map is
built from, but not in the rank-indexed `job->procs`, so it has no proc
record — quietly handed rank 0 its node, its app index and its local rank on
every daemon. Nothing failed and nothing was logged. `"%u"` is byte for byte
what the helper emits for every real rank, so the wire is unchanged; a
sentinel now arrives above `num_procs` and is refused. `unpack_layout`
checks `strtoul`'s end pointer as well, because a field that is not a number
at all is otherwise indistinguishable from rank 0.

Five fields are therefore derived by `prte_job_unpack` rather than
transmitted, and each is derivable for a specific reason:

| field | why the maps already say it |
|-------|------------------------------|
| `name.rank` | it *is* the map — the proc maps list ranks by node |
| `parent` | the node's daemon, which the node pool already holds |
| `app_idx` | which app's proc map the rank appeared in |
| `app_rank` | `compute_app_rank()` walks `jdata->procs`, which is indexed by rank, so an app's ranks take app ranks in ascending rank order. The decoder sees them in *node* order, so it sorts before numbering |
| `local_rank` | `compute_local_rank()` walks each node's procs in array order, apps outermost — exactly the order the packer emits them in |

That last one is only safe because nothing else ever assigns a local rank.
`prte_rmaps_base_update_local_ranks()` did assign one by a different rule
("lowest unused on the new node"), which would have broken the
correspondence after a relocation — but it had no callers, and it has been
removed rather than left as a trap.

`prte_proc_pack` is what remains: **only what the maps cannot say.** The
node rank counts procs of *every* job on that node, so one job's map cannot
produce it. The cpuset is the binding the mapper computed. The state is the
proc's own. Its **attribute list is not on the wire at all** — see the long
comment in `prte_proc_pack`, and the `#if PRTE_ENABLE_DEBUG` check beside it
that shouts if a proc ever acquires a `PRTE_ATTR_GLOBAL` attribute. That is
~13 bytes a proc against the ~25 the full record cost, on top of the ~46 it
cost before the namespace was hoisted out of it.

Two things the unpacker deliberately does **not** do, both of which look
like omissions:

- **It does not set `proc->node`,** and does not put the proc on
  `node->procs`. The odls does that, in `construct_child_list`, by looking
  `proc->parent` up in the daemon job — which is the only place that knows
  which `prte_node_t` this daemon's copy of the pool holds for that vpid.
  Setting it here would duplicate that and get it wrong on the HNP, which
  throws the unpacked copy away and keeps its own.
- **It does not retain `jptr->bookmark`.** The bookmark travels as the
  node's **index in `prte_node_pool`**, which is meaningful on the receiver
  only because the nidmap ships pool slots along with the names; the
  resulting pointer is borrowed, and `prte_job_destruct` simply NULLs it.

### The buffer says which shape it is: `prte_job_pack_mode_t` and `devices`

Not every caller packs the same record. The launch message is broadcast to
every daemon, but a proc's **cpuset** is read only by the daemon that forks
it, so the launch path packs `PRTE_JOB_PACK_NO_CPUSETS` and sends each
daemon its own bindings point to point
(`prte_odls_base_send_cpuset_slices()`, see
[`src/mca/odls/AGENTS.md`](../../mca/odls/AGENTS.md)). Everything else packs
`PRTE_JOB_PACK_ALL`.

The mode is **one byte at the head of the buffer**, ahead of the nspace, and
`prte_job_unpack` hands it back through an out parameter. Two reasons it is
on the wire rather than agreed out of band:

- The decoder must know before it reads the first per-proc record. Getting
  it wrong does not fail — it reads the next proc's node rank as this one's
  cpuset and desynchronizes everything after.
- The receiver has to tell **"not sent" from "not bound"**. A NULL cpuset
  means the mapper bound nothing when the mode is `ALL`, and means "this is
  not my proc" when it is `NO_CPUSETS`, and the two have different right
  answers everywhere downstream — see
  [`src/prted/pmix/AGENTS.md`](../../prted/pmix/AGENTS.md).

**The mode byte is not the only discriminator.** A `bool` follows it saying
whether the per-proc records carry a device assignment, and it is set from
`PRTE_MAPPING_BYDEVICE` on the job's mapping policy. It has to travel
separately even though the policy itself is on the wire, because
`prte_map_pack` runs *after* the proc array: by the time the decoder could
read the policy it has already had to decide how wide a proc record is. Any
further optional per-proc field needs the same treatment — a job-level
discriminator packed ahead of the array, never a flag inside the record it
governs.

A *conditional trailing field* was tried instead and does not work:
`prte_proc_pack` runs in a loop and more job-level fields follow the array,
so an absent field is ambiguous against both, and `PMIX_ERR_UNPACK_PAST_END`
never fires because there is always more buffer. Any optional field needs a
discriminator the decoder has already read.

**`prte_util_pack_job_catchup` also packs `NO_CPUSETS`,** and that is not an
optimization by analogy: the catchup decoder *discards* a job it already
knows, so the only daemon that keeps what that message says is one that was
not in the DVM when the job launched — which hosts none of its procs.

Two consequences to respect:

- **`prte_proc_unpack` no longer creates the proc.** `prte_job_unpack` has
  already built it from the maps and set its identity; the proc belongs to
  the job, so an error there leaves it to be released with everything else.
- **The decoder needs a populated `prte_node_pool`,** because that is what
  it resolves node names against. Every caller has one: a daemon decodes
  the nidmap out of the same wireup message first, and the HNP has the pool
  natively. An **unmapped** job — a spawn request on its way to the HNP,
  which has not been through `rmaps` — packs a zero node count and nothing
  else, so that path needs nothing.

### The GLOBAL/LOCAL attribute split

Only attributes marked `PRTE_ATTR_GLOBAL` are packed; the unpacker marks
everything it reads back as `PRTE_ATTR_GLOBAL` ("obviously not a local
value"). This asymmetry is load-bearing and is a recurring source of bugs
elsewhere in the tree: **the mapper runs against an unpacked copy of the
job**, so any attribute the mapper has to read must be set GLOBAL at the
point it is created, or it simply is not there by the time anyone looks.

---

## Copying

`prte_job_copy` and `prte_proc_copy` are `PMIX_RETAIN` + assign — aliases,
not copies. `prte_node_copy`, `prte_app_copy`, and `prte_map_copy` are real
deep copies. Only `prte_node_copy` has a caller today (the `ras`
`multiplier`, which fabricates duplicate nodes for mapper testing); the rest
are dead but exported.

Three rules the copies have to obey, all of which were being broken:

1. **A copy is only useful if it carries identity.** A node answers to its
   `name`, its `rawname`, and every entry in `aliases`; it belongs to a
   `session`; and its `attributes` carry the per-node settings that decide
   how it is launched (`PRTE_NODE_USERNAME`, `PRTE_NODE_PORT`). A copy
   missing any of those is a node that cannot be found by the names the
   allocation used, or cannot be launched the same way.
2. **Attributes are `prte_attribute_t`.** `prte_app_copy` walked
   `app->attributes` as a list of `prte_value_t` — similar enough to
   compile, different enough that the value came from the wrong offset and
   the key (which `prte_value_t` does not have) was dropped, leaving every
   copied attribute unfindable.
3. **Copy into a pointer array through `pmix_pointer_array_set_item`.**
   `prte_map_copy` used to blit the source array's `size`/`max_size`/
   `block_size` over the destination's and then assign `addr[i]` directly.
   The destination's `addr` is whatever `prte_job_map_construct` allocated —
   `PRTE_GLOBAL_ARRAY_BLOCK_SIZE` (64) entries — so any map spanning more
   nodes than that wrote off the end of the heap block, and the copied-in
   metadata then told every later reader the array was bigger than it is.
   And because `prte_job_map_destruct` releases every node it holds, a copy
   that takes no reference leaves the two maps dropping one refcount too
   many between them.

---

## Printing

`prte_map_print` (called from `rmaps_base_map_job.c` for `--display map`)
and `prte_app_print` (called from the odls) are the live entry points;
`prte_job_print` and `prte_node_print`/`prte_proc_print` are reached from
them.

Three output shapes share the code, selected by job attributes:
`PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT` (XML), `PRTE_JOB_DISPLAY_DEVEL_MAP`
(the developer dump), and neither (the short user form). They are not
symmetric — a guard added to one arm is not present in the others unless
you put it there. `prte_proc_print` had NULL checks on
`src->node->topology` in two of its three arms and none in the third, and
none of the three checked `src->node` itself, which is NULL for an unmapped
proc and for a proc that outlived its node.

The XML arm sizes a buffer for `prte_hwloc_get_binding_info()` itself, and
has to size it from the **elements** it will hold, not from a round number:
each site is 20 spaces of indent plus `<core>%d</core>\n`, so ~34 bytes for a
single-digit core index and more as indices grow, and each package the
process touches costs an opening and a closing element on top. The estimate
used to be 20 bytes per PU and nothing per package, which silently truncated
the site list for any process bound to more than about half the cores of a
non-SMT node — and, before `src/hwloc` bounded its writes, overran the buffer
outright. See [`src/hwloc/AGENTS.md`](../../hwloc/AGENTS.md), "a cpuset's
bits are PU OS indices".

**The `<package>` elements come from the renderer, not from here.** This arm
used to wrap the returned site list in a single `<package id="%d">` built
from a `pkgnum` out parameter, which meant a process bound across two
packages was reported as belonging to one — the short form of the same
binding (`prte_hwloc_base_cset2str`) named both. Emit what the renderer
returns verbatim.

The string building is `pmix_asprintf`-and-free chaining: `tmp` always owns
the accumulated string, each step builds `tmp2` from it, frees `tmp`, and
reassigns. Any `continue` inside such a loop that skips the
`free(tmp1); tmp1 = tmp2;` step both leaks the new string and drops that
iteration's contribution from the output.

---

## Gotchas before you edit

- **Pack and unpack are edited together, always.** There is no version and
  no check.
- **Match the PMIx type to the C type.** `prte_app_idx_t` is `uint32_t`,
  `prte_node_state_t` is `int8_t`, `prte_local_rank_t`/`prte_node_rank_t`
  are `uint16_t`, `prte_proc_state_t` is `uint32_t`,
  `prte_job_state_t`/`prte_exit_code_t` are `int32_t`,
  `prte_job_flags_t` is `uint16_t`,
  `prte_app_context_flags_t`/`prte_node_flags_t` are `uint8_t`. See
  [`src/include/types.h`](../../include/types.h),
  [`src/util/attr.h`](../../util/attr.h),
  [`src/runtime/prte_globals.h`](../prte_globals.h), and
  [`src/mca/plm/plm_types.h`](../../mca/plm/plm_types.h).
- **`PMIX_PROC_RANK` is four bytes, and not everything called a count of
  procs is a rank.** PMIx packs and unpacks it as a `PMIX_UINT32`, so using
  it on a narrower field reads and *writes* past that field. `prte_node_t`'s
  `num_procs` is a `prte_node_rank_t` — two bytes — and was packed as a
  `PMIX_PROC_RANK` from the day it stopped being a `prte_vpid_t`: the pack
  put two bytes of the struct's padding on the wire and the unpack wrote
  four bytes into two. It round-tripped, because both sides were wrong in
  the same way, which is exactly why nothing found it. A mismatch of this
  kind survives a round-trip test; only reading the field's declaration
  catches it.
- **`PMIx_Data_unpack`'s count is in/out.** Reset it to 1 before each call
  rather than relying on the previous call having left it there.
- **An unpack that fails mid-object must release the partially built
  object.** All of these do; keep it that way when adding a field.
- **These functions answer in PRRTE codes, not PMIx statuses.** Every one of
  them converts at its own `PMIx_Data_*` calls and returns the result of
  `prte_pmix_convert_status()`. So a caller — including one of these
  functions calling another — tests `PRTE_SUCCESS`, logs with
  `PRTE_ERROR_LOG`, and returns the code unchanged. Converting a second time
  is not harmless: PRRTE codes sit at `PMIX_EXTERNAL_ERR_BASE`, which
  `prte_pmix_convert_status()` does not recognise, so every specific failure
  collapses into a bare `PRTE_ERROR` and `PMIX_ERROR_LOG` prints the wrong
  name for it. `pmix_server_dyn.c` keeps the two spaces in separate
  variables and says why; follow that.
- **The wire trusts `job->num_apps`.** The packer writes it and then walks
  `job->apps` packing every non-NULL entry, while the unpacker reads exactly
  that many; the proc maps are keyed the same way. The two agree only
  because every `jdata->num_apps++` in the tree sits beside a
  `pmix_pointer_array_add(jdata->apps, ...)` and nothing ever removes an
  app. If you ever make an app removable, this is the first thing that
  breaks, and it breaks as a desynchronized buffer rather than as a wrong
  app count.
- **`prte_node_pack`/`prte_node_unpack` have no callers.** They are
  exported, and `test/unit/runtime` round-trips them, but no message in the
  tree carries a bare `prte_node_t` — nodes reach a daemon through the
  nidmap. Keep them correct (they are the obvious thing to reach for), but
  do not assume a live path is exercising them.
- **Do not add a new attribute to the packer and forget its disposition.**
  If the receiving side needs it, it has to be `PRTE_ATTR_GLOBAL` where it
  is set — packing is filtered on that flag, not chosen per call site.

---

## Testing

**Unit — `test/unit/runtime/test_runtime.c`.** Round trips for job (with
apps, procs, a map, personality, and both a GLOBAL and a LOCAL attribute,
asserting the LOCAL one does *not* cross) and for node; the mapless-job
case; and all three deep copies, including a map deliberately built with
more nodes than one array block and a refcount check after both maps are
released.

**Multi-node — `contrib/dockerswarm`.** The packers' real exercise is any
launch: `prte_job_pack` is what the odls sends and `prte_job_unpack` is what
every daemon reads, so the whole existing suite covers them implicitly. A
dynamic spawn (`test_runtime`'s spawn case) drives the
`pmix_server_dyn.c` path specifically.

**Not covered:** the print functions' XML arm, and `prte_job_copy` /
`prte_proc_copy` (which have no callers to test through).
