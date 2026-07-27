# AGENTS.md — `ras/flux` (Flux allocator)

Component guide for `src/mca/ras/flux/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/flux` reads a Flux allocation by contacting the local Flux broker
and fetching the job's resource set. It is an **optional build**: its
`configure.m4` requires both Flux (`PRTE_CHECK_FLUX`) and Jansson
(`PRTE_CHECK_JANSSON`); if either is missing the component is not built.
It is also a **run-time loadable plugin** — it is in the default
`--enable-mca-dso` list in
[`config/prte_mca.m4`](../../../../config/prte_mca.m4), which keeps
`libflux-core` and `libjansson` out of `libprrte` and hence out of every
PRRTE tool. Do not take it off that list: it is what makes
`--enable-testbuild-launchers` viable (see the framework guide).

`query` gates on `FLUX_URI` (or an explicitly-set `ras_flux_broker_uri`)
and only then opens a handle (`flux_open_ex`) to confirm a broker is
really there, selecting at the configurable priority `ras_flux_priority`
(default **100**) when one answers. The env check is not just an
optimization — it is what stops the component from calling into a stub
library in a testbuild tree, and outside a Flux instance the open could
not have succeeded anyway.

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
- The parser frees `hostinfo`/`hostlist` on every exit path; mind the
  `err:` cleanup when adding branches. **Declare anything the cleanup
  block touches at the top of the function**: `root` was originally
  declared mid-body, so every early `goto err` jumped over its
  initializer and the cleanup decref'd an indeterminate pointer.
- **Use `s?o`, not `s?O`, in `json_unpack_ex`** unless you decref the
  result — the capital form takes a reference. `scheduling` is unpacked
  purely to satisfy the format string and is never used.
- `hostinfo_append_ranks` reports into a caller-supplied buffer rather
  than allocating, because the caller's `error_str` also holds string
  literals; a mixed-ownership error pointer can be neither freed nor
  safely leaked. It returns a **count**, so a caller treating `<= 0` as
  failure must also set `ret` — falling through to `err:` with `ret`
  still `PRTE_SUCCESS` returns a partially-built node list as success.
</content>
