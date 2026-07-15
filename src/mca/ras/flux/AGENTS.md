# AGENTS.md — `ras/flux` (Flux allocator)

Component guide for `src/mca/ras/flux/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/flux` reads a Flux allocation by contacting the local Flux broker
and fetching the job's resource set. It is an **optional build**: its
`configure.m4` requires both Flux (`PRTE_CHECK_FLUX`) and Jansson
(`PRTE_CHECK_JANSSON`); if either is missing the component is not built.
Its `query` actually opens a Flux handle (`flux_open_ex`) to test
availability, selecting at the configurable priority `ras_flux_priority`
(default **100**) when a broker answers.

Files:

| File | Contents |
|------|----------|
| `ras_flux_component.c` | Registration; `query` probes the broker; `priority`, `broker_uri`, `open_flags` MCA params. |
| `ras_flux_module.c` | `init`, `allocate`, `modify` (unsupported), `finalize`, and the `resource.R` JSON parser. |
| `ras_flux.h` | Component struct and externs. |
| `configure.m4` | Flux + Jansson build gate. |
| `help-ras-flux.txt` | Broker/KVS/JSON error text. |

---

## How `allocate()` works

1. `flux_open_ex` to the broker (`flux-broker-not-found` on failure).
2. `flux_attr_get(h, "jobid")` — saved to `prte_job_ident`.
3. `flux_kvs_lookup(h, NULL, 0, "resource.R")` then
   `flux_kvs_lookup_get` to fetch the R (resource set) JSON string.
4. `json_loads` + `parse_json_payload`.

`parse_json_payload` handles **R version 1** only. It unpacks
`execution.nodelist` (a Flux hostlist) and `execution.R_lite` (per-broker
rank → core idsets). `hostlist_from_R_nodelist` expands the hostlist;
`hostinfo_array_create` allocates a per-node `R_hostinfo`; and
`hostinfo_append_ranks` decodes each R_lite entry's rank/core idsets
(via `idset_decode`) to set each node's `broker_rank` and `nslots` (=
core count). Each node becomes a `prte_node_t` at `PRTE_NODE_STATE_UP`
with `slots = nslots`. Errors map to `PRTE_ERR_NOT_AVAILABLE`.

`modify()` is **not implemented** for Flux — it returns
`PMIX_ERR_NOT_SUPPORTED`.

---

## Things to watch when editing

- Depends on external libs (`flux/core.h`, `flux/hostlist.h`,
  `flux/idset.h`, `jansson.h`); any new API must stay inside the
  `configure.m4` gate so non-Flux builds still compile the tree.
- Only R **version 1** is parsed; a different version is a clean
  `PRTE_ERR_NOT_AVAILABLE`, not a crash — preserve that.
- The parser carefully frees `hostinfo`/`hostlist` on every exit path;
  mind the `err:` cleanup when adding branches.
</content>
