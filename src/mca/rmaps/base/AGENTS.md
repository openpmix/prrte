# AGENTS.md — `rmaps/base` (the machinery under the mappers)

Guide to `src/mca/rmaps/base/`. Read the [framework guide](../AGENTS.md)
first: it covers the module contract, `prte_rmaps_base_map_job()`, the
`prte_rmaps_options_t` scratch struct, the support helpers every mapper
leans on, ranking and binding. It is the authority for everything in this
directory; this file holds what is specific to one file and too detailed
for the framework map.

```
base/
  base.h                    # framework globals + the base API other code may call
  rmaps_private.h           # base API used only by the components
  rmaps_base_frame.c        # open/close/register; the --map-by/--rank-by/--bind-to parsers
  rmaps_base_select.c       # keeps ALL components, priority-sorted
  rmaps_base_map_job.c      # the orchestrator
  rmaps_base_support_fns.c  # get_target_nodes, check_avail, setup_proc, ...
  rmaps_base_ranking.c      # compute_vpids
  rmaps_base_binding.c      # bind_proc and friends
  rmaps_base_devices.c      # the device list a --map-by device= job is placed against
  rmaps_base_print_fns.c    # policy -> string
  help-prte-rmaps-base.txt
```

Everything here runs on the **HNP's progress thread**, inside
`prte_rmaps_base_map_job()`, which the state machine fires as an event.
Nothing in this directory is a PMIx callback, and nothing needs a thread
shift — but equally nothing here may block.

---

## `rmaps_base_devices.c` — mapping by device

The framework guide's "Mapping by device" section says what the feature is
and the two facts that govern it (a device uuid names its node; a GPU
nothing can name is refused). This is the file-level detail.

### The API and who calls it

Two mappers place against devices and share this one enumeration, so they
cannot disagree about what the devices are:

- `round_robin` (`--map-by device=`) wraps it in a
  `prte_rmaps_target_enum_t` and hands it to the shared `map_targets` loop
  — see [`../round_robin/AGENTS.md`](../round_robin/AGENTS.md).
- `ppr` (`--map-by ppr:N:device=`) calls the functions directly.

Per node, in this order:

| Call | What it does |
|------|--------------|
| `prte_rmaps_base_devices_begin(jdata, node, opts, &ctx)` | Enumerates, applies `interleave`, groups `ndev` devices per proc, resolves each group's locality, and makes the refusals. On any non-success `*ctx` is NULL. |
| `prte_rmaps_base_devices_count(node, opts, ctx)` | How many groups — i.e. procs, before `shared` — the node offers. Zero is not an error here; the caller reports it. |
| `prte_rmaps_base_devices_locale(node, opts, ctx, j)` | The object group *j* is placed against. |
| `prte_rmaps_base_devices_record(proc, opts, ctx, j)` | Publishes group *j* on the proc as `PRTE_PROC_DEVICE_ID`. **Returns a status, and a failure fails the map.** |
| `prte_rmaps_base_devices_end(ctx)` | Releases the context; NULL is fine. Every exit from a node's body has to reach it. |

`prte_rmaps_base_devices_total(node_list, opts, &total)` is separate: the
"are there enough?" pre-count a mapper makes before placing anything.

### What the context owns

`prte_rmaps_device_map_t` owns the device array PMIx returned (release with
`pmix_hwloc_release_devices()`, never `free()` alone — each entry owns
strings) and the `grouploc` array. The `locality` pointers inside both are
**borrowed from `node->topology`**: valid because the node's list holds a
reference for the whole of the node's placement, and meaningless once it
does not. `record()` copies what it publishes, so a proc never points into
the context.

`interleave_devices()` reorders the array with a shallow copy and a
`memcpy` back. That is safe because each entry is moved exactly once — the
per-group counts are exact — so ownership of every string moves with it.

### Traps

- **`interleave=<level>` finds the level object by cpuset, never by walking
  up.** A device's locality is usually a Package or a Group, and in hwloc 2
  a NUMA node is a memory child — nobody's ancestor.
  `hwloc_get_ancestor_obj_by_type(NUMANODE, …)` therefore answers NULL for
  every device, every device lands in one group, and `interleave=numa`
  reproduces the input order: silently, because that is also the documented
  answer for a level that does not partition the devices. `interleave_key()`
  asks which object of the level *contains the locality's cpuset*, through
  the PRRTE wrappers so NUMA means CPU NUMA
  ([`src/hwloc/AGENTS.md`](../../../hwloc/AGENTS.md)). For package and the
  caches that is the same answer the walk gave.
- **The GPU refusal is decided by each device's type**, not by the class the
  user asked for. `device=renderD129` names one GPU; it is not a class
  request, and it used to slip past the refusal and hand the process exactly
  the unactionable assignment the refusal exists to prevent.
- **Diagnostics name the job.** `begin()` takes `jdata` for its three
  `show_help` calls and nothing else. PMIx scopes duplicate suppression by
  the nspace a message names and purges it when that nspace ends; named with
  the DVM's own nspace — which never ends — a persistent DVM showed the
  first job that hit a refusal and folded every later job's into "N more
  processes have sent help message". Those jobs failed with nothing said
  about why. See `src/util/prte_show_help.h`.
- **`total()` counts; it does not judge.** It used to run `begin()` per
  node, so a node `begin()` refused was explained, skipped and left out of
  the count — and the user was then *also* told there were too few devices,
  on a node list that had plenty. It enumerates only, and the refusals
  happen once, when the mapper reaches the node. What it does return is an
  enumeration that fails outright: a count that quietly leaves a node out is
  that same wrong answer.
- **`record()` failing is not cosmetic.** The proc is already placed, and a
  proc launched without its assignment runs, looks right in
  `--display map`, and computes on whichever device its runtime picks by
  default.
- **A named device is shared by definition.** A class is a set of devices
  to hand out, one process each unless `shared` says otherwise. A name is
  "put every process near this device" — the old `dist` policy in one
  directive — so `prte_rmaps_base_devices_named()` makes round_robin treat
  it as shared whatever the qualifier says. Applying the class rule to it
  refused the documented `prun -n 8 --mapby device=mlx5_0` as "too few
  devices", which is the only thing the name form is for.

### What is deliberately not checked

`grouploc[j]` is never NULL in practice, and `map_targets` treats a NULL
target as "no more targets on this node" rather than an error. PMIx gives a
device's locality as the nearest ancestor carrying a cpuset, and the root
object always carries one, so a NULL could only come from a topology hwloc
itself would not load.

---

## Testing

`test/unit/rmaps/test_devices.c` loads the two GPU topologies in
`test/topologies/` (`turin-4gpu-nvml.xml`, read with the vendor backends;
`turin-4gpu.xml`, the same machine without them) onto hand-built nodes and
drives this API directly — uuid naming, the refusals by class and by name,
the network synonyms, and `interleave=numa`. It needs the PMIx server the
test main brings up, because `record()` copies through `PMIx_Data_copy`.

Placement against those topologies — plain, interleaved, shared, `ndev`,
`ppr` — is pinned by the golden maps in `test/offline/golden/`; run
`make -C test/offline check-offline` for any change to this file.
