# AGENTS.md — `src/hwloc`

Orientation for AI agents and human contributors working in `src/hwloc/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

`src/hwloc` is PRRTE's integration layer over the [hwloc](https://www.open-mpi.org/projects/hwloc/)
library. It is **not** a framework and has no MCA components: it is two
source files compiled straight into `libprrte`.

| File | What it is |
|------|------------|
| `hwloc-internal.h` | The whole public surface. Binding-policy word and its macros, the two cached-data classes, the globals, and every prototype. |
| `hwloc.c` | MCA parameter registration, open/close, the `--bind-to` parser, the default-binding chooser, and the print-buffer ring. |
| `hwloc_base_util.c` | Everything that takes a topology as an argument: topology acquisition, the NUMA summary, object counting/lookup, cpu-set and slot-list parsing, and the binding/topology renderers. |
| `help-prte-hwloc-base.txt` | `show_help` content for this directory. Remember the [GOLDEN RULE](../../AGENTS.md) about regenerating `prte_show_help_content.*` after touching it. |

Everything here is a **pure function of a topology plus a string**. There is
no state beyond the globals listed below and the data cached on a topology's
`userdata` pointers. That is what makes the directory unit-testable, and it
is the property to preserve.

### Who calls it

| Caller | What it wants |
|--------|---------------|
| `ess/hnp`, `plm/base` | `prte_hwloc_base_get_topology()`, `_filter_cpus()`, `_setup_summary()` — acquiring the local topology and receiving remote ones |
| `rmaps/*` | `_get_nbobjs_by_type()`, `_get_obj_by_type()`, `_get_npus()`, `_generate_cpuset()`, `_cpu_list_parse()`, `_reset_counters()`, `prte_hwloc_obj_data_t` |
| `odls/base` | `prte_hwloc_base_map` / `_mbfa` (the memory-policy globals) and `_cset2str()` |
| `runtime/data_type_support`, `ras/base` | `_cset2str()`, `prte_hwloc_get_binding_info()`, `prte_hwloc_print()`, `prte_hwloc_build_map()` — every `--display` variant |
| `schizo`, `tools` | `_set_binding_policy()`, `_print_binding()` |

---

## GOLDEN RULE: the topology you are handed is usually not this machine's

`prte_hwloc_topology` is the *local* topology, and it is the exception. In
the HNP, almost every call in this directory is made against a topology that
arrived as XML from another node's daemon and lives in
`prte_node_topologies`. So:

- **Take the topology as an argument and use that one.** Do not consult
  `prte_hwloc_topology` inside a function that has a `topo` parameter.
  `prte_hwloc_base_cpu_list_parse()` used to check the *global* for NULL and
  then operate on its argument — which meant the check passed on every
  daemon (each has sensed its own topology) and a NULL argument sailed
  straight into the parsers.
- **Nothing about a remote topology is guaranteed to have been set up.**
  In particular the cached summary (below) is built for the local topology
  when it is sensed, and for a remote one only where `plm/base` remembers
  to.
- **Do not assume homogeneity.** Two nodes in one DVM can have different
  package counts, different core counts, and different allowed cpusets.
  A cpu-set expansion computed against node A is not valid for node B.

---

## GOLDEN RULE: NUMA is not a normal hwloc level, and PRRTE filters it

Two things are true at once, and both surprise people:

1. **In hwloc 2.x, NUMA nodes are not in the depth hierarchy.** They live at
   the special depth `HWLOC_TYPE_DEPTH_NUMANODE`. A loop over
   `0..hwloc_topology_get_depth()` never visits one — which is why
   `prte_hwloc_base_release_userdata()` sweeps that special depth
   separately, and why it must keep doing so: binding attaches placement
   counters to NUMA nodes exactly as it does to packages and cores.

2. **PRRTE deliberately counts only *CPU* NUMA domains.** Modern packages
   carry GPU/fabric memory that hwloc also reports as NUMA nodes, and those
   are not places to put a process. The filter is the `numa_cutoff` in
   `prte_hwloc_topo_data_t`, computed once per topology by
   `prte_hwloc_base_setup_summary()` and cached on the **root object's
   `userdata`**: CPU NUMA domains have low OS indices counting up from 0,
   non-CPU ones have high indices counting down from 255, so the cutoff is
   the first OS index at which a NUMA node's cpuset intersects one already
   seen.

Consequences:

- **Always ask NUMA questions through `prte_hwloc_base_get_nbobjs_by_type()`
  and `prte_hwloc_base_get_obj_by_type()`,** never through
  `hwloc_get_nbobjs_by_type()`/`hwloc_get_obj_by_type()` directly. Only the
  PRRTE wrappers apply the cutoff. (For every other object type they are
  thin pass-throughs, so using them everywhere costs nothing and keeps the
  NUMA case from being special-cased by accident.)
- **Those two wrappers build the summary if it is missing.** They used to
  return `0`/`NULL` instead, which does not look like an error to anybody:
  it says "this node has no NUMA domains", and a `--map-by numa` job
  believes it and places nothing.
- **The cutoff is an OS-index bound, and the scan that computes it is
  bounded by the largest OS index actually present.** A topology whose NUMA
  nodes carry no OS index (`HWLOC_UNKNOWN_INDEX` — hand-written XML, or XML
  from an old hwloc) is the case that used to make the computation walk to
  `UINT_MAX`. It now terminates, but such a topology reports zero CPU NUMA
  domains; if that ever needs to work, the cutoff has to stop being
  expressed in OS indices.
- **`numa_cutoff` must never be left at `UINT_MAX`,** on any error path.
  The consumers scan `0..cutoff`.

---

## Two kinds of `userdata`, hanging off the same pointer

PRRTE stores two *different* PMIx objects on hwloc `userdata` pointers:

| Where | Type | Set by |
|-------|------|--------|
| the topology's **root** object | `prte_hwloc_topo_data_t` (the summary: `computed`, `numa_cutoff`) | `prte_hwloc_base_setup_summary()` |
| any other object | `prte_hwloc_obj_data_t` (the per-object `nprocs` placement counter) | `rmaps/base/rmaps_base_binding.c` |

hwloc does not know these pointers are ours, so:

- **`prte_hwloc_base_release_userdata()` must run before
  `hwloc_topology_destroy()`.** `prte_hwloc_base_close()` and
  `prte_topology_t`'s destructor both do it. So does the XML-loading path,
  which replaces the local topology.
- **`prte_hwloc_base_reset_counters()` starts at depth 1, not depth 0.**
  Depth 0 is the root, whose `userdata` is a `prte_hwloc_topo_data_t`;
  reinterpreting it as a counter would scribble on `numa_cutoff`. Release
  code can treat both uniformly (`PMIX_RELEASE` is correct for either), but
  anything that *reads a field* cannot.
- `prte_hwloc_base_reset_counters()` walks `prte_node_topologies`, which is
  NULL until `prte_init()`.

---

## The binding-policy word

`prte_binding_policy_t` is a `uint16_t` split in half:

```
 0x00ff   the policy itself      PRTE_BIND_TO_{NONE,PACKAGE,NUMA,L3CACHE,
                                 L2CACHE,L1CACHE,CORE,HWTHREAD}
 0xff00   the qualifiers         PRTE_BIND_OVERLOAD_GIVEN  0x0100
                                 PRTE_BIND_IF_SUPPORTED    0x1000
                                 PRTE_BIND_ALLOW_OVERLOAD  0x2000
                                 PRTE_BIND_GIVEN           0x4000
```

That split is what lets `PRTE_SET_BINDING_POLICY` replace the policy while
preserving the qualifiers (`(pol) | ((target) & 0xff00) | PRTE_BIND_GIVEN`).
**A new qualifier bit must go in `0xff00` and a new policy value in
`0x00ff`,** or the macro will drop it. The policy values are also mirrored
in `src/mca/rmaps/rmaps.h` — keep them in step.

`PRTE_SET_DEFAULT_BINDING_POLICY` deliberately does *not* set
`PRTE_BIND_GIVEN`: `PRTE_BINDING_POLICY_IS_SET()` has to keep answering
"no" so a later explicit request still wins.

### `prte_hwloc_base_set_binding_policy(jdata, spec)`

One parser for two callers, distinguished by `jdata`:

- **`jdata == NULL`** — the DVM-wide default from the `bindto` MCA
  parameter. Result lands in `prte_hwloc_default_binding_policy`. The
  qualifiers that need somewhere to record a value (`report`, `limit=N`)
  are *refused* here, because there is no job to record them against.
- **`jdata != NULL`** — one job's `--bind-to`. Result lands in
  `jdata->map->binding`; `report` and `limit=N` become job attributes.

Qualifier names may be abbreviated to any unambiguous prefix, so **never
index past a qualifier's name to find its `=value`** — use
`pmix_cli_qualifier_value()`. `limit=N` lands in a `uint16_t` attribute, so
a value that does not fit has to be rejected rather than truncated.

---

## Rendering a binding: three functions, three buffer contracts

This is where the sharp edges are, because the output is built into
fixed-size buffers whose size the *caller* chooses.

| Function | Output | Buffer |
|----------|--------|--------|
| `prte_hwloc_base_cset2str()` | `package[0][core:L0,2-3]` | allocates and returns; internally builds through a 2048-byte scratch |
| `prte_hwloc_get_binding_info()` | one `<core>N</core>` element per bound core, plus the package number | **caller's** buffer and size |
| `prte_hwloc_print()` | the whole topology as indented text | allocates and returns |

Rules that were all being broken:

- **Charge every byte written against the remaining room.** The XML element
  writer advanced its cursor through a run of set bits without reducing the
  budget it handed `snprintf`, so a process bound to more than a few cores
  wrote past the end of the caller's buffer — in the HNP, which is holding
  every node's topology, so the corruption surfaced as a crash somewhere
  unrelated.
- **A caller sizing a buffer per PU must use the real element size.** Each
  element is 20 spaces of indent plus `<core>%d</core>\n` — about 34 bytes
  for a single-digit index, more as indices grow. The one caller in
  `runtime/data_type_support/prte_dt_print_fns.c` budgeted 20.
- **`hwloc_bitmap_snprintf()` gets `sizeof(buf)`, not a constant that
  happens to be nearby.** `print_hwloc_obj()` declared 1024 bytes and
  claimed 2048.
- **`*pkgnum` is always set.** Its only caller declares it uninitialized
  and prints it.
- **`hwloc_bitmap_isfull()` is never true for a topology's cpuset.** It
  means "infinitely set". A dead `isequal(cpuset, avail) && isfull(avail)`
  "unbound" test lived in two of these functions for years. Deciding a
  process is unbound is the *caller's* job — each one already prints
  `UNBOUND` when the proc carries no cpuset — so don't reintroduce it here.

---

## The cpu-set (`--cpu-set` / `hwloc_default_cpu_list`)

`prte_hwloc_base_generate_cpuset()` takes a comma-separated list of logical
cpu ids and ranges, resolves each against a topology, and **rewrites the
caller's string with the fully expanded list**. `prte_hwloc_base_filter_cpus()`
wraps it and is what produces `prte_node_t.available`.

- **Every id is user input; validate it.** The ids used to go through a bare
  `strtoul()`, which reports `0` for `foo` — so a typo did not fail, it
  confined the entire DVM to cpu 0. `parse_cpu_id()` is the gate.
- **`-` is the range delimiter**, so a leading `-` is not a sign: `"-1"`
  tokenizes to the single id `1`. The grammar cannot express a negative id.
- **`prte_hwloc_base_filter_cpus()` never returns NULL.** Its result goes
  straight into `prte_node_t.available`, which the mapper copies and
  intersects without checking; a NULL there took the HNP down inside hwloc.
  An unresolvable cpu-set yields an **empty** set, so the node offers
  nothing and the mapper reports it through its normal path — the user has
  already been told which entry failed.
- The expansion mutates the *global* `prte_hwloc_default_cpu_list` as it
  walks the nodes. It is idempotent for a homogeneous DVM; for a
  heterogeneous one, whichever node was processed first decides the
  expanded form. This is a known wart, not a designed behavior.
- **Applying the set is `rmaps`' job, and it applies at every binding
  level.** `filter_cpus()` narrows `prte_node_t.available`; the mapper then
  has to keep a proc inside that set whatever object it binds to. It did not
  — `--bind-to package` handed the rank every core of the package — which is
  why the `cpu_list` comment in `hwloc.c` describing the package case reads
  like a specification of behavior that did not exist. It does now; see
  `set_proc_cpuset()` in
  [`rmaps_base_binding.c`](../mca/rmaps/AGENTS.md).

`prte_hwloc_base_cpu_list_parse()` is the richer slot-list grammar
(`P0:0-3`, `S1:*`, bare core lists). Every package and core id in it is
user input too, and every lookup can legitimately fail — `--cpu-set P99`
used to be a segfault.

---

## The print-buffer ring

`prte_hwloc_base_print_binding()` returns a pointer into a **ring of 16
per-thread buffers**, obtained from `prte_hwloc_get_print_buffer()`. That is
what makes

```c
pmix_output(0, "%s -> %s", prte_hwloc_base_print_binding(a),
                           prte_hwloc_base_print_binding(b));
```

correct, and the mappers are full of it. **Every function that takes a slot
must advance `ptr->cntr`.** It also means the pointer is only good until the
same thread has cycled through the other 15 — never store one.

---

## Gotchas before you edit

- **`prte_config.h` first.** Every `.c` file in the tree, no exceptions.
- **An MCA string parameter's storage belongs to the MCA layer**, which
  frees it at finalize. Never assign a string literal to one — the
  deprecated `bind_to_core`/`bind_to_socket` shortcuts did, which is a
  `free()` of read-only memory. And **give every parameter its own
  variable**: two parameters sharing one `static char *` means each reports
  the other's value.
- **Match `pmix_show_help()`'s argument list to the `%s` count in the
  topic.** Two call sites passed one argument to a two-`%s` message, and one
  passed a `char **` where the format wanted a `char *`.
- **A failure path that has destroyed the topology must NULL
  `prte_hwloc_topology`.** `prte_hwloc_base_close()` tests it for NULL and
  destroys it again otherwise.
- **`hwloc_bitmap_first()`/`_last()` return `-1` for an empty bitmap.**
  Do not feed that to `hwloc_bitmap_isset()`. `hwloc_bitmap_weight()` is
  what you want for "how many" and "exactly one".
- **Comparing unsigneds by subtracting into an `int`** gets the order wrong
  for values far apart. `compare_unsigned()` learned this.
- **Warnings are errors.** Debug builds imply `--enable-devel-check`.

---

## Testing

**Unit — [`test/unit/hwloc/test_hwloc.c`](../../test/unit/hwloc/), run by
`make check`.** hwloc can synthesize a topology on demand
(`hwloc_topology_set_synthetic`), so nearly everything here is testable with
no DVM: the NUMA summary and both object lookups (including the
summary-not-yet-built case), `_get_npus()` in core and hwthread terms,
`_generate_cpuset()` expansion and rejection, the whole
`_cpu_list_parse()` grammar including every id that does not exist,
`_cset2str()` range collapsing, `prte_hwloc_get_binding_info()` against a
**guarded** buffer (a canary past `sz` — this is how the element-writer
overflow is pinned), `prte_hwloc_build_map()`, the print-buffer ring, the
`--bind-to` parser, and userdata release across the normal hierarchy *and*
the special NUMA depth.

Add to it rather than around it. If a change here is not reachable from a
synthetic topology plus a string, say why in the test file.

**Offline mapper harness — `make -C test/offline check-offline`.** Any change
to how a binding is chosen or rendered shows up here across ~1200
map/rank/bind combinations. Run it.

**Multi-node — [`contrib/dockerswarm`](../../contrib/dockerswarm/AGENTS.md),
the `test_hwloc` phase.** What the unit test cannot reach is the case PRRTE
actually runs in: the topology being queried or rendered came from *another*
machine. That phase covers rendering a remote node's binding in the HNP, a
package-wide binding on a non-SMT node (the shape that overran the element
buffer — those containers are 8 cores, one package, no SMT, no sysfs NUMA
node, which is exactly the shape that trips it), `--map-by numa` against
topologies the HNP never sensed, a DVM cpu-set applied to every node rather
than just the first, a malformed cpu-set being refused without taking the
HNP down, and `--display topo` over several topologies at once.

**Not covered anywhere:** `prte_hwloc_base_get_topology()`'s sensing path
(it reads the real machine), and `prte_hwloc_print()` against a machine wide
enough to fill its cpuset buffer — a few thousand PUs.

---

## Known gaps and things deliberately absent

- **PRRTE no longer computes locality.** PMIx does, via
  `PMIx_server_generate_locality_string()`, which
  `prted/pmix/pmix_server_register_fns.c` calls. The `prte_hwloc_locality_t`
  enum, the `PRTE_PROC_ON_LOCAL_*` macros, `_get_relative_locality()`,
  `_compute_relative_locality()`, `_get_locality_string()` and
  `_get_location()` were all dead and are gone. If you need locality, ask
  PMIx — do not grow a second implementation here (see
  ["Generic CLI code lives in PMIx"](../../AGENTS.md)).
- **Memory affinity is applied by the odls, not from here.**
  `hwloc_base_maffinity.c` (`_set_process_membind_policy()`,
  `_memory_set()`, `_membind()`, `_node_name_to_id()`) had no callers at
  all; `odls/base/odls_base_bind.c` reads `prte_hwloc_base_map` and
  `prte_hwloc_base_mbfa` and does the binding itself, in the forked child,
  async-signal-safely. Those two globals and their MCA parameters
  (`hwloc_default_mem_alloc_policy`, `hwloc_default_mem_bind_failure_action`)
  are therefore still live; the file is gone.
- **Intel Phi coprocessor detection is gone** (`_find_coprocessors()`,
  `_check_on_coprocessor()`), along with the topology-signature builder,
  `_single_cpu()`, `_get_obj_idx()` and `_topology_export_xmlbuffer()` —
  none had callers.
- **`prte_hwloc_get_binding_info()`'s output is undefined for a process
  bound across more than one package.** It renders the last matching package
  and reports that package's number. The caller wraps the result in a single
  `<package>` element, so the shape of the fix is an API change, not a
  one-liner.
