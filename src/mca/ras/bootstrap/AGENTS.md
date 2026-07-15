# AGENTS.md — `ras/bootstrap` (launcher-less bootstrap allocator)

Component guide for `src/mca/ras/bootstrap/`. Read the
[framework guide](../AGENTS.md) first for the module contract.

---

## Role and priority

`ras/bootstrap` supplies the node set for a **launcher-less bootstrap
DVM** — the DVM whose daemons come up from a static configuration file
rather than being spawned by a launcher/scheduler. Its `query` gates on
the global `prte_bootstrap_setup` flag: only when bootstrap mode is on
does it select (priority **20**); otherwise it returns `PRTE_ERROR` and
provides no module.

Files:

| File | Contents |
|------|----------|
| `ras_boot_component.c` | Registration; `query` gates on `prte_bootstrap_setup`, priority 20. |
| `ras_boot.c` | `allocate` (only vtable slot; no modify/finalize). |
| `ras_boot.h` | Externs for the component and module. |

---

## How `allocate()` works

`allocate` reads the **same bootstrap config file the daemons read**
(`prte_bootstrap_parse`, from `src/util/prte_bootstrap`). Because the
`DVMNodes` list is a fixed, externally-defined node set — exactly like an
RM allocation — it sets **`prte_managed_allocation = true`** so the VM
setup treats the pool as authoritative (the construct path, not
hostfile/dash-host filtering) and honors per-node slot counts as given.

It then adds each `DVMNodes` entry to the caller's list at its canonical
rank (`prte_bootstrap_rank_of`), **skipping rank 0** — the controller
node is already represented by the HNP at pool index 0. Each node is a
`prte_node_t` with `name`, `index = rank`, `state = PRTE_NODE_STATE_UP`,
`slots = 1`, and `PRTE_NODE_FLAG_SLOTS_GIVEN` set. Returns
`PRTE_SUCCESS`; config is freed with `prte_bootstrap_config_free`.

---

## Things to watch when editing

- **Node `index` is the canonical bootstrap rank**, not a pool-insertion
  ordinal — the launch/routing path depends on this correspondence.
- Skipping rank 0 avoids double-entering the controller (HNP) node;
  don't remove that guard.
- Setting `prte_managed_allocation` is intentional and load-bearing for
  the bootstrap construct path — see repo memory *Bootstrap DVM* entries.
</content>
