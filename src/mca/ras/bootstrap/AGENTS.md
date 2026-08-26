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
RM allocation — and it marks every node it supplies
**`PRTE_NODE_FLAG_SLOTS_GIVEN`** so the per-node slot counts are honored
as given rather than re-derived from each node's core count. (It used to
also set a global `prte_managed_allocation` to keep VM setup from
filtering the pool through hostfile/dash-host specs; that global is gone
— the pool is now the authoritative node set for every allocation.)

It then adds each `DVMNodes` entry to the caller's list at its canonical
rank (`prte_bootstrap_rank_of`), **skipping rank 0** — the controller
node is already represented by the HNP at pool index 0. Each node is a
`prte_node_t` with `name`, `index = rank`, `state = PRTE_NODE_STATE_UP`,
`slots = 1`, and `PRTE_NODE_FLAG_SLOTS_GIVEN` set. Returns
`PRTE_SUCCESS`; config is freed with `prte_bootstrap_config_free`.

---

## Things to watch when editing

- **`allocate` pre-assigns node `index` to the canonical bootstrap rank,
  and `prte_ras_base_node_insert` honors it.** This is load-bearing, not
  cosmetic: a bootstrapped daemon computes its *own* vpid from this same
  config file (`prte_bootstrap_my_identity`), so the HNP does not get to
  choose one — it has to arrive at the same answer independently.
  `node_insert` therefore places a node carrying a pre-assigned
  `index >= 0` at exactly that pool slot (falling back to an append only
  if the slot is already taken, which means a malformed config) instead
  of appending to the lowest free slot. Before that, the assignment was
  silently overwritten and the correspondence held only by accident of
  `DVMNodes` being listed in rank order.
- **plm reads that index back out as the daemon's vpid.**
  `prte_plm_base_setup_virtual_machine` uses `node->index` (not the next
  sequential vpid) when `prte_bootstrap_setup` is on, closing the loop:
  config → pool slot → daemon vpid, all from the one authority the
  daemons themselves used.
- **Ranks are contiguous, and `prte_bootstrap_parse` enforces it.** A
  host listed twice in `DVMNodes` resolves to the position of its first
  occurrence, leaving a rank no daemon will ever claim while
  `prte_bootstrap_num_daemons` still counts it — the DVM could never
  finish forming. The parser rejects that file up front
  (`bootstrap-duplicate-node`), so every daemon fails identically and
  immediately, and by the time this component runs the name↔rank mapping
  is a bijection.
- Skipping rank 0 avoids double-entering the controller (HNP) node;
  don't remove that guard.
- Marking the nodes `PRTE_NODE_FLAG_SLOTS_GIVEN` is intentional: without
  it the one slot per node this component assigns would be replaced by
  the node's core count.
