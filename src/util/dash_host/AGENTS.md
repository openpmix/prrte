# AGENTS.md — `src/util/dash_host`

Orientation for AI agents and human contributors working in
`src/util/dash_host/`. This is a map, not the rulebook: the authoritative
guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md), under
[`docs/`](../../../docs/), and in [`src/util/AGENTS.md`](../AGENTS.md). When
this file and those disagree, **the docs win** — and please fix this file.

---

## What lives here

The parser for `-host` / `--host` / `--add-host`, and its sibling
`PRTE_APP_DASH_HOST` / `PRTE_APP_ADD_HOST` app attributes. One file,
four entry points:

| Entry point | Used for | Behavior |
|-------------|----------|----------|
| `prte_util_add_dash_host_nodes()` | building an allocation | Adds the named nodes to the caller's list, merging duplicates. Relative tokens are **ignored** here: they select from an allocation and cannot contribute to one. |
| `prte_util_filter_dash_host_nodes()` | selecting for a job | Removes from the caller's list every node the specification does not name, in the order the user gave, and reports by name any host the allocation does not have. |
| `prte_util_get_ordered_dash_host_list()` | `rmaps/seq`, `rmaps/rank_file` | Keeps order and duplicates: the list *is* the sequence. |
| `prte_util_dash_host_compute_slots()` | the mapper | How many slots one node contributes under this specification. |

---

## Syntax

```
nodeA,nodeB            one slot each; a repeated name accumulates slots
nodeA:4                four slots
nodeA:*   nodeA:auto   however many the node was discovered to have
nodeA:+2  nodeA:-2     adjust the discovered count (records PRTE_NODE_ADD_SLOTS)
15                     a bare launch id: selects the node whose name ends in 15
+n3                    the 3rd node of the allocation, counting from zero
+e   +e:2              all / N currently-empty nodes
```

A token made **entirely of digits** is a launch id, not a hostname — no real
machine is called `15` — so it is matched against the trailing run of digits
in each node's name, which is what lets `--host 15` select `nid0015`. That
decision is made from the shape of the token, not from the kind of allocation,
so the shorthand works wherever such names are used and costs nothing where
they are not (a name ending in no digit simply does not match).

---

## GOLDEN RULE: per-token state must be reset per token

`prte_util_add_dash_host_nodes()` walks the comma-separated tokens and derives
`slots`, `slots_given` and `add_slots` from each. All three describe **that
token** — declaring them outside the loop and only resetting some of them is
how `--host nodeA:*,nodeB` came to hand nodeB the `*` auto-detect marker
(`slots = -1`), which the branch below reads as "this node has no slots at
all", so nodeB was allocated zero and a job asking for one process on it was
refused. The same leak turned `--host nodeA:+2,nodeB` into an increment request
against nodeB's discovered count.

The same class of bug bit `parse_dash_host()` differently: its `+e:N` branch
scanned the node pool with `j`, **the index of the outer loop over the tokens**,
and left it past the end — so every token after a `+e:N` was silently
discarded. `--host +e:2,node07` asked for two empty nodes and forgot node07.
Give an inner loop its own index.

---

## Relative node syntax indexes the ALLOCATION, not the pool

Identical to the hostfile parser, and it has to stay identical:

```c
if (!prte_hnp_is_allocated) {
    nodeidx++;    /* the head node always holds pool slot 0 */
}
```

Bounds-check against `prte_node_pool->size` with `>=`, not `>`.

"Empty", for `+e`, means `slots_inuse == 0`. Not `num_procs == 0`: that is the
count the mapper is *building* for the job it is mapping, so a node stops
answering to `+e` the moment the mapper places its first process there, and
the slot count computed for the rest of that same map comes out as zero. The
filter path and the hostfile parser both use `slots_inuse`; keep all three the
same.

---

## `compute_slots` is why relative syntax works at all

The mapper asks `prte_util_dash_host_compute_slots()` how many slots each node
offers under the user's specification. A relative token names a node without
saying anything about how big it is — the colon in `+e:N` is a *node* count,
not a slot count — so a node it designates contributes what it was discovered
to have, exactly as for a job that named no hosts at all. Without that case,
the raw `+n1` text was matched against each node's *name*, matched nothing,
and every node came out with zero available slots: `--host +n1` was refused
for lack of resources even though the node list had been resolved correctly,
and only ever "worked" under `--map-by :OVERSUBSCRIBE`, which skips the check.

The answer is a *cap*, not a slot count: when the `-host` selects within an
allocation that already exists, the node keeps its own `slots` and the cap
lands in `node->slots_available` — which is what the mappers have to place
against, and what they did not all do.  A cap *larger* than the node is
reconciled there too: refused under a resource manager, and otherwise taken
as a re-description of the node for the duration of that map.  See
[`src/mca/rmaps/AGENTS.md`](../../mca/rmaps/AGENTS.md).

---

## No allocation check in the "add" path

`prte_util_add_dash_host_nodes()` deliberately does **not** ask "is this host
in the allocation?". Every caller runs it while the allocation is being built,
so there is nothing yet to check against. Naming a host the allocation does
not contain is reported and refused by
`prte_util_filter_dash_host_nodes()`, which is also where the "not all mapped"
diagnostic names the offending host — an entry that did not match has to be
**left in place** for that loop to find, which is why the match loop does not
free as it goes.

---

## Testing

- `test/unit/util/test_util.c` covers plain names, repeated names, explicit
  counts, the `:*` and `:+N` markers not leaking between tokens, and
  `compute_slots` for explicit / auto / bare / launch-id / no-match tokens.
- `contrib/dockerswarm/run-tests.sh` covers the relative syntax against a real
  multi-node DVM (`+n1` resolving to the second node and being held to its
  slot count, `+e:2` spreading over two empty nodes) and, in `test_util`, that
  a token following `+e:N` is not dropped and that `:*` on one token does not
  starve the next. A single-node pool makes every one of those look correct.
- Run `make -C test/offline check-offline` after changing slot accounting.
