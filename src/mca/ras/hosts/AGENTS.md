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
   app-contexts via `prte_util_add_dash_host_nodes(..., true)`.
3. **Hostfile** (`PRTE_APP_HOSTFILE`), a comma-list per app parsed with
   `prte_util_add_hostfile_nodes`; the result is the UNION across apps.
4. **Default hostfile** (`prte_default_hostfile`), if set.

Node objects are appended to the caller's `nodes` list; the base's
`node_insert` then dedups them into `prte_node_pool`. Slot counts come
from the hostfile/dash-host parsers (a bare hostname yields the
parser's default; `slots=N` is honored).

Hard parse errors on a hostfile activate `PRTE_JOB_STATE_ALLOC_FAILED`
and return the error.

---

## `modify()` — schedulerless elastic authority

When no scheduler is reachable (`ras/pmix` deferred with
`PMIX_ERR_TAKE_NEXT_OPTION`), `hosts.modify` is the local resource
authority for runtime DVM changes:

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
  flex parser can't. Keep the two in sync in spirit.
- Because this is the lowest-priority component, returning a hard error
  (rather than `TAKE_NEXT_OPTION`) from `allocate` will fail the whole
  allocation — reserve hard errors for genuine parse failures.
</content>
