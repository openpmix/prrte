# AGENTS.md — `rmaps/seq` (sequential mapper)

Component guide for `src/mca/rmaps/seq/`. Read the
[framework guide](../AGENTS.md) first.

---

## Role and priority

seq implements **`--map-by seq`**: lay ranks down **one process per line
of a hostfile**, in file order. Line *k* of the file names the node for
rank *k* (and optionally a cpuset). It is priority **60** — above
round_robin, below ppr/rank_file/lsf. Claimed only when the mapping
policy is `PRTE_MAPPING_SEQ`.

This is the mapper to reach for when a user wants precise, ordered,
node-by-node placement without the full ceremony of a rankfile — the
node list *is* the placement.

Files:

| File | Contents |
|------|----------|
| `rmaps_seq_component.c` | Registration, `priority` param (default 60), `query`. |
| `rmaps_seq.c` | `prte_rmaps_seq_map()` plus the `seq_node_t` list type and `process_file()`. |
| `rmaps_seq.h` | Component/module externs. |

---

## The sequence source (precedence)

A `seq_node_t` is just `{ hostname, cpuset }`. Per app context, the
mapper builds the ordered list from the first of these that is present:

1. **Per-app map file** — `PRTE_APP_MAP_FILE` (`--map-by seq:FILE=…` on
   that app), parsed by `process_file()`.
2. **Job-level file** — `PRTE_JOB_FILE`.
3. **Per-app `-host`** — `PRTE_APP_DASH_HOST`, expanded via
   `prte_util_get_ordered_dash_host_list()` (dash-host entries carry no
   cpuset).
4. **Per-app `-hostfile`** — `PRTE_APP_HOSTFILE`.
5. **Default hostfile** — `prte_default_hostfile`, parsed once up front
   into `default_seq_list`.

`process_file()` reads a hostfile where each line is `hostname [cpuset
[numa mempolicy]]`; it keeps hostname and cpuset and drops the trailing
NUMA/mempolicy fields (the LSB affinity-file format is accepted so the
same parser serves both). A line that names a cpuset says where the rank
goes *and* which cpus it gets there — see "Things to watch" below.

---

## What the mapper does

Gate: defer (`TAKE_NEXT_OPTION`) on restart, or when `options->map` isn't
`PRTE_MAPPING_SEQ`. The base records the component on acceptance; the mapper
does not stamp it.

Per app (honoring `options->app_idx`):

1. Build `seq_list` from the precedence chain above.
2. **NOLOCAL:** if `--map-by :NOLOCAL`, remove the head node from the
   list (checked with `prte_check_host_is_local`, since FQDN forms may
   differ).
3. **Proc count:** if the app gave no `-n`, `app->num_procs =
   #entries`; if it gave more procs than entries → `not-enough-resources`
   error. So, like ppr, seq can *derive* the count — here from the
   number of hostfile lines.
4. **Placement:** starting at the right position (for the shared default
   list, continue where the previous app left off via `save`), for each
   of `app->num_procs`:
   - Find the named host in the global `prte_node_pool` (`prte_quickmatch`)
     — the hostfile objects are temporaries, so the mapping must be
     recorded against the real node object.
   - `get_cpuset` + `check_avail`; `setup_proc` on that node. **`check_avail`
     is given a NULL node list here.** `seq_list` holds `seq_node_t`
     entries, not the `prte_node_t` being placed on, so letting the helper
     try to drop the node from it removed an item of the wrong type from a
     list it was never on. NULL says "just tell me yes or no."
   - **Assign the rank sequentially** (`proc->name.rank = vpid++`) and
     set `app_rank` directly, then insert into `jdata->procs`. `vpid` starts
     at `options->start_vpid` — zero for a whole job, and in per-app dispatch
     the first rank no earlier app has taken.
   - `check_oversubscribed`, advance to the next list entry.
5. When not in per-app dispatch, call `compute_vpids` — but since seq
   already set the ranks, this only back-fills local/app ranks.

---

## Things to watch when editing

- **seq assigns its own vpids.** It does not use the by-slot/node/fill/
  span ranking schemes; ranks follow file order. Keep `proc->name.rank =
  vpid` and the `jdata->procs` insertion consistent, and don't route seq
  through the object-based `compute_vpids` paths.
- **The default list is shared across apps, and the cursor can run out.**
  `save` carries the position so successive apps continue through the
  default hostfile rather than restarting at line 0. That also means the
  "enough entries for `num_procs`" check against the *whole* list is not
  enough for the second app onwards: entering at the cursor there may be
  fewer entries left than the count promises, and stepping past the end
  reads the list's sentinel as if it were an entry. The loop checks for the
  sentinel and errors out. Per-app lists (`sq_list`) are freed each app; the
  default list is destructed once, on both the success and the error path.
- **A per-line cpuset IS the binding for that rank.** `process_file()`
  understands `hostname [cpuset [numa mempolicy]]`, and when an entry carries
  a cpuset `bind_to_entry_cpuset()` applies it the way rank_file applies a
  slot list: parse it against that node's topology, write `proc->cpuset`, and
  take those cpus out of `node->available` so a later entry naming the same
  node cannot claim them too (unless `--bind-to <x>:overload-allowed` says it
  may). The ordinary object-based binder is held off for that proc —
  `options->bind` is set to `BIND_TO_NONE` across the `setup_proc` call —
  because a proc bound twice keeps only the second answer and leaks the
  first. Entries with no cpuset are unaffected and bind by policy as before.
- **Host lookup must go through `prte_node_pool`.** Mapping onto the
  temporary hostfile node object instead of the pooled one loses the
  placement — this is a subtle, recurring class of bug.
- Honor `options->app_idx` and the standard defer-don't-error gate.
