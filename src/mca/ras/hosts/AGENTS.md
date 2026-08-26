# AGENTS.md — `ras/hosts` (the default allocator)

Component guide for `src/mca/ras/hosts/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
`allocate()` return protocol, and `prte_ras_base_node_insert`.

---

## Role and priority

`hosts` is the **catch-all default allocator**, query priority **1**
(lowest of all ras components), so the driver tries it last. It has no
scheduler to talk to: it assembles the allocation from user-supplied
sources — a rank/seq file, `--host` (dash-host), `--hostfile`, or the
default hostfile — and it is the DVM's local resource authority for
elastic operations when no external scheduler is present. It is always
available (its `query` unconditionally returns the module).

Files:

| File | Contents |
|------|----------|
| `ras_hosts_component.c` | Registration; `query` returns the module at priority 1 (no params). |
| `ras_hosts.c` | `allocate`, `modify`, `finalize`, plus the local `process_hostfile` parser. |
| `ras_hosts.h` | Extern declarations for the component and module. |

---

## How `allocate()` works

`allocate()` tries sources in order and returns `PRTE_SUCCESS` the moment
one yields nodes; if all are empty it returns `PRTE_ERR_TAKE_NEXT_OPTION`
(letting the base fall back to the local host):

1. **Rank/seq file** (`PRTE_JOB_FILE` on the job). Parsed with
   `prte_util_add_hostfile_nodes`. If it produced nodes, the module
   stamps the job map with `PRTE_MAPPING_BYUSER` + `PRTE_MAPPING_GIVEN`
   (and `NO_OVERSUBSCRIBE` unless the user set a subscribe directive) —
   the rankfile *is* the allocation and the mapping.
2. **Dash-host** (`PRTE_APP_DASH_HOST`), aggregated across all
   app-contexts via `prte_util_add_dash_host_nodes()`.
3. **Hostfile** (`PRTE_APP_HOSTFILE`), a comma-list per app parsed with
   `prte_util_add_hostfile_nodes`; the result is the UNION across apps.
4. **Default hostfile** (`prte_default_hostfile`), if set.

Node objects are appended to the caller's `nodes` list; the base's
`node_insert` then dedups them into `prte_node_pool`. Slot counts come
from the hostfile/dash-host parsers (a bare hostname yields the
parser's default; `slots=N` is honored).

Hard parse errors return the error code and **nothing else** — reporting a
hard error is the *driver's* job. `prte_ras_base_allocate()` reads anything
outside its return protocol as a real error and answers it with
`PRTE_ERROR_LOG` plus `PRTE_ACTIVATE_JOB_STATE(PRTE_JOB_STATE_ALLOC_FAILED)`,
so a module that activates the state as well runs the DVM's whole failure
teardown twice.

A hostfile value that splits to nothing — `prte --hostfile ''`,
`prte --hostfile ,` — is refused here rather than indexed.
`PMIx_Argv_split()` answers "nothing" with **NULL**, not with an empty array,
so the loop over the result faulted the DVM master before it had a node pool.
That is a live command line: the persistent branch of `prte.c` joins
`--hostfile`'s values raw, while the `prterun` branch absolutizes each one
first — which is why only `prte` ever arrives here with an empty string.

---

## `modify()` — schedulerless elastic authority

`hosts.modify` is the DVM's local resource authority for runtime changes.
Selection keeps exactly one module, and `ras/pmix` declines the query unless
it has been pointed at a scheduler — so in a schedulerless DVM `hosts` *is*
the selected allocator and is asked directly. (It is not reached by falling
through `ras/pmix` any more; that component now answers `PMIX_ERR_UNREACH`
when it is selected and its scheduler is out of touch.) It serves:

- `PMIX_ADD_HOSTFILE` — comma-list of hostfiles parsed by the local
  `process_hostfile` (a hand parser, *not* the flex hostfile code,
  because it must accept `slots=+N`/`-N` adjustment syntax). Matches
  existing pool nodes by name/alias and adjusts slots, or appends new
  `PRTE_NODE_STATE_ADDED` nodes. Rejects a new node given negative slots.
- `PMIX_ADD_HOST` — comma-list of hosts via
  `prte_util_add_dash_host_nodes`.
- It also claims `PMIX_ALLOC_NEW`/`EXTEND`/`RELEASE` so the base's
  `prte_ras_base_complete_request` runs with the original request info
  intact (preserving the node list and allocation ids for reservation
  routing).

New nodes clear `prte_nidmap_communicated` and are inserted via
`prte_ras_base_node_insert`. Returns `PMIX_OPERATION_SUCCEEDED` when it
handled something, else `PMIX_ERR_TAKE_NEXT_OPTION`.

---

## Things to watch when editing

- **Order matters and is first-match-wins** — a rankfile short-circuits
  dash-host/hostfile, which short-circuit the default hostfile. Don't
  reorder without understanding the mapping-policy side effects of the
  rankfile branch.
- **`process_hostfile` is deliberately a separate parser** from
  `src/util/hostfile` — it supports the `+N`/`-N` slot-adjust syntax the
  flex parser can't. Keep the two in sync in spirit. Being a hand parser,
  it owns its own hygiene:
  - every `isspace()` call must cast its argument to `unsigned char` (a
    plain `char` is UB for bytes ≥ 0x80);
  - the slot value goes through `parse_slots()`, not a bare `strtol()`.
    A bare one answers **0** for anything that is not a number, and 0 is a
    meaningful count here — the node would join the DVM able to run nothing
    *and* carry `PRTE_NODE_FLAG_SLOTS_GIVEN`, so its real size would never be
    worked out from its topology. It also truncated a `long` into an `int`,
    and the sign is what the caller acts on: a value too large to fit could
    arrive with the sign the user did not write, and the refusal of negative
    slots for a new node would then not fire.
  - node lookup goes through **`prte_node_match()`**, which is the one place
    that knows how a name reaches a pool entry — local-host resolution, the
    per-node alias lists, and an entry that carries no name at all. This
    file used to spell that walk out for itself and had already drifted from
    it, comparing `nptr->name` with no NULL check.
- **A `slots=+N` match adjusts the pool entry in place and appends nothing
  to the working list.** Two consequences that look like oversights and are
  not: `prte_ras_base.total_slots_alloc` is updated by `adjust_slots()`
  itself rather than by `node_insert`, and `prte_nidmap_communicated` is
  *not* cleared — the nidmap carries node names and daemon placement, never
  slot counts, so an adjustment gives the other daemons nothing new to
  learn.
- **`modify()` must validate the info values it splits.** A request can
  arrive over the wire carrying any type; `PMIX_ADD_HOST`/
  `PMIX_ADD_HOSTFILE` are checked for `PMIX_STRING` with a non-NULL
  value before `PMIx_Argv_split`. It also destructs its working list on
  every error return — `prte_ras_base_node_insert` drains only what it
  reached.
- **A `modify()` that fails is what un-parks the DVM, and the base does it
  for us.** `prte_ras_base_add_hosts()` clears `prte_dvm_ready` and parks the
  requesting job in `prte_cache`; only the grow's `VM_READY` re-entry sets
  the flag again and drains the cache. A request that fails before it reaches
  a grow therefore used to leave that job — and every job cached behind it —
  waiting on a DVM that would never be ready again, with nothing to time it
  out. One mistyped `--add-hostfile` path was enough to hang a tool for the
  life of the DVM. The request now carries `dvm_held`, and
  `prte_ras_base_modify()`'s respond tail fails the requesting job and gives
  the DVM back. So refusing a request from here is safe; silently succeeding
  on one you could not serve is what is not.
- Because this is the lowest-priority component, returning a hard error
  (rather than `TAKE_NEXT_OPTION`) from `allocate` will fail the whole
  allocation — reserve hard errors for genuine parse failures.
