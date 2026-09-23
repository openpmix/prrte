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

## `rmaps_base_map_job.c` — traps in the orchestrator

The framework guide describes the seven phases and the `cleanup:` contract.
This is what the file's shape invites that the phase list does not show.

### `options` is a copy, and it is the copy that places the job

`prte_rmaps_base_map_job()` reads `jdata->map->{mapping,ranking,binding}`
into `options.{map,rank,bind}` once, at the `ranking:` label, and everything
downstream consults the copy: `prte_rmaps_base_bind_proc()` dispatches on
`options->bind`, `prte_rmaps_base_compute_vpids()` on `options->rank`,
`bind_generic()` on `options->hwb`. **Writing a policy onto `jdata->map`
after that point changes nothing.** `map_colocate()` did exactly that to say
"daemons are never bound and always rank by-slot"; the daemons were bound to
a core anyway, taking cpus from the very processes they were colocated with
and failing the colocation outright on a node with none to spare. Set both,
or set the one the reader uses.

The framework guide states the mirror-image rule for reads ("a gate reads
`options`, never `jdata->map`"). They are the same rule.

### The colocation path skips most of the setup

`colocate`/`colocate_daemons` jumps straight to `ranking:`, past the block
that reads `PRTE_JOB_PES_PER_PROC`, `PRTE_JOB_CPUSET`, `PRTE_JOB_MAP_DEVICE`
and friends, and past the loop that sums the apps' proc counts. So on that
path `options.cpus_per_rank` is **0**, not 1; `options.nprocs` is 0; and
`options.cpuset` is NULL. The zero proc count is why the derived binding
comes out `BIND_TO_CORE` (`prte_hwloc_base_set_default_binding()` reads
`nprocs <= 2` as "a couple of procs, bind to a cpu"), and the zero
`cpus_per_rank` is a divisor in `prte_rmaps_base_check_avail()` — which the
colocation path happens never to call. Anything new on this path has to
assume those fields are unset rather than defaulted.

### The map has to exist before the first `goto cleanup`

`cleanup:` walks `jdata->map->nodes` to reset the flags the map set, so
`jdata->map` is created at the very top — ahead of the personality check,
which is the one guard that can fire before any mapping work. A job reaching
`PRTE_JOB_STATE_MAP` need not have a map: `prte_job_construct()` leaves it
NULL, and only some creation paths fill it in.

### `map_colocate()` reads a tool's request, not our own data

The `pmix_data_array_t` behind `PRTE_JOB_COLOCATE_PROCS` comes straight off a
`PMIx_Spawn` request's `PMIX_COLOCATE_PROCS` value with nothing in between,
so neither its element type nor its contents have been established. Reading
a differently-typed array as `pmix_proc_t` walks off the end of the caller's
allocation, and a target named with `PMIX_RANK_WILDCARD` may name a job that
was never mapped — **a tool's job tracker has `map == NULL`** and is in
`prte_job_data` like any other. Both are checked; keep them checked.

The other half of that function's state is `PRTE_NODE_FLAG_MAPPED`, which it
uses twice for two different questions ("already collected into `targets`?"
and "already added to `map->nodes`?"). Every exit has to clear it on *both*
the target list and the map, because an error exit from the target scan
leaves it set on live pool nodes and the next colocation then reads those
nodes as already collected and silently leaves them out.

### The binding `limit` counters outlive the job

`bind_generic()` keeps a per-object proc count on the hwloc object's
`userdata`, and it lives for the life of the DVM. Only a job that set a
limit bumps it, so only such a job has to clear it — but **`limit` has two
spellings**, `PRTE_JOB_BINDING_LIMIT` and the per-app
`PRTE_APP_BINDING_LIMIT`, and both have to trigger
`prte_hwloc_base_reset_counters()`. Resetting on the job-level one alone
left a second per-app `--bind-to <obj>:limit=N` finding every object already
standing at its limit. See [`src/hwloc/AGENTS.md`](../../../hwloc/AGENTS.md).

### A count that comes from a string is not a count yet

The `N` of a `ppr:N:<object>` pattern is never validated by the `--map-by`
parser — `prte_rmaps_base_set_mapping_policy()` only splits the spec and
stores it — so `ppr_count()` here is the one place it is checked, for the
job-level and the per-app spelling alike. A bare `strtoul()` is not enough:
`ppr:-2:core` came back as a huge unsigned that truncated to a **negative**
process count, a value past `INT_MAX` wrapped to whatever its low bits said,
and `ppr:abc:core` became zero — which the mapper reads as "this app named
no pattern" and quietly maps by the job's.

---

## `rmaps_base_frame.c` — the parsers

### A qualifier's value is checked here or nowhere

`--map-by`, `--rank-by` and `--bind-to` values are strings, and this file is
the only place between the command line and the mapper that looks at them.
The three value-bearing mapping qualifiers now agree on the shape of that
check — `strtol` with an end pointer, `errno`, and an explicit range — and
they have to, because each one is either a `uint16_t` attribute, a divisor,
or both:

| Qualifier | Why the range matters |
|---|---|
| `PE=n` | A `uint16_t` attribute *and* the divisor in `prte_rmaps_base_check_avail()`'s `ncpus / cpus_per_rank`. `PE=0` is an integer division by zero in the HNP — silently zero on aarch64, **SIGFPE on x86-64**. `PE=70000` used to become 4464 and `PE=-1` became 65535. |
| `LIMIT=n` | A `uint16_t` attribute, and `bind_generic` reads a limit of zero as "no limit at all" (`0 < options->limit`) rather than as what the user wrote. |
| `NDEV=n` | A `uint16_t` attribute. |

The `ppr:N:<object>` count is *not* checked here — this parser only splits
the spec and stores it — which is why `ppr_count()` in `rmaps_base_map_job.c`
is where that one is validated, for both the job-level and the per-app
spelling.

`check_pe_list()` has its own trap: **`PMIx_Argv_split()` keeps no empty
tokens**, so an entry that is nothing but delimiters comes back NULL rather
than as an empty array, and walking it as an array is a NULL dereference.
`--map-by pe-list=-` segfaulted before the job was ever described. The outer
split is guarded; so is the inner one now.

### A directive only the job's policy word carries must be hoisted

`prte_rmaps_base_hoist_job_directives()` exists because a per-app mapping
spec may carry a qualifier that describes the whole job. Two different
reasons put a qualifier on that list, and it is worth keeping them apart:

- **OVERSUBSCRIBE/NOOVERSUBSCRIBE and INHERIT/NOINHERIT** are *meaningless*
  per app — one app cannot oversubscribe its nodes while its siblings do not
  — so they are hoisted, and apps that disagree are refused.
- **NOLOCAL** is hoisted for a blunter reason: it is simply *unreadable* per
  app. It travels in the mapping policy word's directive bits, and
  `prte_rmaps_base_get_target_nodes()` is handed `jdata->map->mapping`
  whichever app it is placing. Left on the app it was read by nothing at all
  and the app ran on the head node anyway.

That second test — "does anything downstream read this from the app?" — is
the one to apply to any directive bit added here. `SPAN` and `ORDERED` pass
it (`prte_rmaps_base_resolve_app_options()` lifts both onto `options`);
`NO_USE_LOCAL` did not.

The hoist also has to notice when what it removed was *all* the app had to
say. An app left carrying nothing but `PRTE_MAPPING_GIVEN` names no mapping
policy, and the attribute's mere presence is what puts the whole job on the
per-app dispatch path — to be placed by a policy of zero.

### What `NULL == jdata` means, and why the globals are safe

The three-way `(NULL, NULL)` / `jdata` / `app` dispatch in `check_modifiers()`
is not symmetric in one respect: the `NULL == attrs` arm writes **DVM-wide
globals** (`prte_rmaps_base.default_pes`, `.inherit`, `.hwthread_cpus`,
`.file`, `.mapping`, `.ranking`). That is only correct because the NULL-jdata
entry point is reachable from exactly one place — `prte_rmaps_base_open()`,
parsing the `mapby`/`rankby` MCA parameters before any job exists. Every
other caller (`pmix_server_dyn.c`, schizo) passes a real job or app. Keep it
that way: a spawn request that reached the NULL arm would rewrite the DVM's
defaults for every later job.

`prte_rmaps_base.ppr` and `.file` are allocated by this file and released in
`prte_rmaps_base_close()`. `.default_mapping_policy` and
`.default_ranking_policy` look the same but are **not** — the MCA variable
system owns those, and freeing them is a double free.

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
strings), the `grouploc` array, and `groupcpus` - per group, the union of
its members' localities. Every exit from `begin()` after allocation goes
through `prte_rmaps_base_devices_end()`, which releases all of it. The
`locality` pointers inside the device and `grouploc` arrays are
**borrowed from `node->topology`**: valid because the node's list holds a
reference for the whole of the node's placement, and meaningless once it
does not. `record()` copies what it publishes, so a proc never points into
the context.

`interleave_devices()` reorders the array with a shallow copy and a
`memcpy` back. That is safe because each entry is moved exactly once — the
per-group counts are exact — so ownership of every string moves with it.

### Traps

- **A group's locality decides how coarse a binding may be; its devices
  decide where a finer one goes.** With `ndev`, a group is placed against
  the common ancestor of its devices' localities - which is what makes
  `--bind-to package` legal for two GPUs on different NUMA domains of one
  package. But binding picks its object from the cpus of whatever it was
  placed against, and the first core or NUMA domain of that package is
  local to neither GPU: `device=gpu:ndev=2 --bind-to numa` bound to NUMA
  domain 0 on a machine whose GPUs are on 1 and 2. `devices_locale()`
  therefore also copies the group's `groupcpus` into `options->devcpus`,
  and `bind_generic()`/`bind_multiple()` narrow their candidates to it
  (`narrow_to_devices()`). It is a copy, owned by the options like
  `target`, because the context is released at the end of each node.
- **"All devices equally close" is a statement about the devices.** The
  degenerate-locality warning compares the devices' localities, not the
  groups'. With `interleave` and `ndev` together every group spans the
  packages and so resolves to the whole node, on a machine whose four GPUs
  sit in four different NUMA domains - and comparing groups told the user
  their machine hung every device off one PCI complex.
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
