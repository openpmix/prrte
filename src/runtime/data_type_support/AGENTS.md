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
it mapped (`prte_map_pack` sends `req_mapper`/`last_mapper`/mapping/ranking/
binding/`num_nodes` and nothing else). Node procs, topologies, session
backpointers, the session dir, `cli`, and the counters are all left behind.

The two sides are hand-written mirrors with no format version and no
self-description. **Every field added to the packer must be added to the
unpacker in the same position, in the same PMIx type.** There is nothing
that will catch a mismatch for you: a type of the same width (`PMIX_INT32`
against `PMIX_UINT32`) silently produces wrong values, and a type of a
different width desynchronizes everything after it.

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
  are `uint16_t`, `prte_job_state_t`/`prte_exit_code_t` are `int32_t`. See
  [`src/include/types.h`](../../include/types.h),
  [`src/util/attr.h`](../../util/attr.h), and
  [`src/mca/plm/plm_types.h`](../../mca/plm/plm_types.h).
- **`PMIx_Data_unpack`'s count is in/out.** Reset it to 1 before each call
  rather than relying on the previous call having left it there.
- **An unpack that fails mid-object must release the partially built
  object.** All of these do; keep it that way when adding a field.
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
