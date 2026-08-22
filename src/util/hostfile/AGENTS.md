# AGENTS.md — `src/util/hostfile`

Orientation for AI agents and human contributors working in
`src/util/hostfile/`. This is a map, not the rulebook: the authoritative
guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md), under
[`docs/`](../../../docs/), and in [`src/util/AGENTS.md`](../AGENTS.md). When
this file and those disagree, **the docs win** — and please fix this file.

---

## What lives here

The hostfile parser: a flex scanner (`hostfile_lex.l`) and three entry points
over it (`hostfile.c`). A hostfile is one node per line, optionally with
keyword modifiers.

| Entry point | Used for | Behavior |
|-------------|----------|----------|
| `prte_util_add_hostfile_nodes()` | building an allocation (`prte --hostfile`, `--add-hostfile`) | Adds every named node to the caller's list, merging duplicates and dropping the excluded ones. |
| `prte_util_filter_hostfile_nodes()` | selecting for a job (`prun --hostfile`) | Removes from the caller's list every node the hostfile does not name, and caps the ones it keeps (see below). Returns `PRTE_ERR_TAKE_NEXT_OPTION` if the hostfile was empty, and refuses a hostfile naming a node the allocation does not have. |
| `prte_util_get_ordered_host_list()` | `rmaps/seq`, `rmaps/rank_file` | Keeps duplicates and order: the list *is* the sequence of placements. |

`hostfile_lex.c` is **generated** by flex from `hostfile_lex.l` and is not in
git. Change the `.l` file, then `rm src/util/hostfile/hostfile_lex.c` and
rebuild — the Make rule will not notice on its own in every configuration.

---

## Syntax the lexer accepts

```
# comment                    // also a comment, and /* block */ comments
hostname                                 one slot
hostname slots=4                         explicit count (also cpu=, count=)
hostname slots=4 max_slots=8             also slots-max=, max-slots=, cpu_max=
user@hostname                            username recorded as PRTE_NODE_USERNAME
hostname port=2222                       ssh port
^hostname                                EXCLUDE this node
192.168.1.10   /  fe80::1                IPv4 and IPv6, with or without user@
+n3                                      relative: the 3rd node of the allocation
+e   /  +e:2                             relative: all / N currently-empty nodes
rank 0=hostname                           rankfile form (the rank is ignored here)
```

The `^` exclusion is easy to break and was in fact broken for a long time:
the `^` was allowed only inside the *optional* `user@` group, and the `@`
makes that group mandatory, so a bare `^host` line never matched the hostname
rule at all — it fell through to the catch-all and the **whole hostfile** was
rejected as a parse error. `hostfile_parse_line()` had always known how to
strip the `^` and move the node to the exclude list; it simply never saw one.
If you touch the hostname pattern, keep the leading `\^?` **outside** the
`user@` group, and keep the swarm case that reads a hostfile with a `^` line.

---

## Every refusal names the file and the line, and says so only once

A hostfile is user input, and every way of getting it wrong has to come back
as a `prte_show_help()` message out of `help-hostfile.txt` carrying
`cur_hostfile_name` and `prte_util_hostfile_line`. Two failures did not:

- A value with more than one `@` — `hostfile_parse_username()` takes one
  field as a hostname and two as `user@hostname`, and anything else is a
  typo. It used to print `WARNING: Unhandled user@host-combination` through
  `pmix_output` at two sites (the plain host entry and the `rank N=<host>`
  form), naming neither the file nor the line, and it is the failure a user
  is most likely to reach by typo. Both sites now go through the one helper
  and the `user-host` topic.
- A `rank N` whose `=` never arrives. The loop that skips to the `=` runs to
  the end of the file, and the `done` arm returned a bare error with no
  message at all.

The other half of "says so only once": a path that has already shown help
must return **`PRTE_ERR_SILENT`**, not `PRTE_ERROR`. `PRTE_ERROR_LOG()`
drops `PRTE_ERR_SILENT` and prints everything else, and the callers
(`prte_ras_base_allocate`, `prte_rmaps_base_filter_nodes`) log whatever they
are handed — so a `PRTE_ERROR` return put `PRTE ERROR: Error in file
ras_base_allocate.c at line 408` above the user's own diagnostic, pointing
at our source for their typo.

---

## The parser state is process-global

`prte_util_hostfile_in` (the `FILE*`), `prte_util_hostfile_line` (the line
counter), `prte_util_hostfile_value` and the flex buffer are all globals, and
`cur_hostfile_name` is a file-scope static. Consequences:

- **Close and destroy on every exit path**, not just the clean one. The error
  paths used to `goto` straight past `fclose()` and
  `prte_util_hostfile_lex_destroy()`, leaking a descriptor per failed parse
  and leaving the flex buffer live for the next one.
- **Reset the line counter per file.** It is only ever incremented, so every
  hostfile after the first reported its parse errors at a line number carried
  over from the ones before it.
- The parser is **not reentrant** and must not be called from two threads.
  Everything that calls it runs on the PRRTE progress thread.
- A top-level token the parser does not understand must set an error return.
  It used to call `pmix_show_help()` and then fall out of the loop with
  whatever `rc` the previous line left behind — reporting a parse error and
  returning success.

---

## Slot counts, and what "given" means

`PRTE_NODE_FLAG_SLOTS_GIVEN` is the difference between "this node has 4 slots
because the user said so" and "this node has 4 slots because that is what we
found on it". It matters in three places here:

- A name repeated on several lines **increments** the count and sets the flag —
  that is the "one line per slot" form.
- `slots=` **sets** the count and sets the flag, and a second `slots=` for the
  same node is an error (`slots-given`), not a silent overwrite.
- The relative forms (`+n<K>`, `+e[:N]`) are **placeholders**: they name a node
  without saying anything about its size. A placeholder's `slots` is the
  constructor's zero, so `prte_util_get_ordered_host_list()` has to check the
  flag before treating it as a subdivision request — otherwise every node a
  bare `+n0` or `+e` resolved to came back with zero slots and the launch was
  refused for lack of resources.

"Empty", for `+e`, means `slots_inuse == 0` — not `num_procs == 0`, which is
the count the mapper is still *building* for the job being mapped. The
dash-host parser uses the same definition; keep them the same.

---

## Relative node syntax indexes the ALLOCATION, not the pool

`+n<K>` means "the K'th node of the allocation, counting from zero". The head
node always occupies `prte_node_pool` slot 0, so when it is **not** part of
the allocation the pool is offset by one and the index has to be adjusted:

```c
if (!prte_hnp_is_allocated) {
    nodeidx++;
}
```

All three places that resolve `+n<K>` — here, `prte_util_get_ordered_host_list()`,
and dash-host's `parse_dash_host()` — must make the same adjustment, or the
same index means a different node depending on which one the user typed it at.

---

## A `slots=` that selects is a cap on the *job*, and it must be given back

`prte_util_filter_hostfile_nodes()` does more than select: a `slots=` count
smaller than the node's own replaces it, which is how a user subdivides an
allocation somebody else built. The nodes on the caller's list are the node
**pool's own objects**, so that write is to the DVM's idea of the node, and
what the hostfile stated is only what *this job* may take there — exactly what
a `-host node:N` states, and the opposite of `--add-hostfile`, which is how an
allocation is changed.

So the count has to come back. Record it with
`prte_rmaps_base_record_resize()` before writing, and
`prte_rmaps_base_restore_resized()` at `map_job`'s `cleanup` puts it back on
both outcomes of the map. Left unrecorded (issue #2698), one job's hostfile
shrank the node for every job the DVM ran afterwards — jobs that never named
the hostfile — and since the write only ever lowers the count, the allocation
could only shrink, with nothing short of restarting the DVM to undo it.

**Only the `remove == true` caller may do this**, because that is the one
running inside `prte_rmaps_base_map_job()`, which restores what it recorded
before it returns. `prte_rmaps_base.resized_nodes` is a framework global, not
a per-job list, and a DVM maps one job while another is still forming its
daemons — so an entry made outside a map is one that some unrelated job's map
would put back. The other caller, `prte_plm_base_setup_virtual_machine()`, is
marking which nodes are to host a daemon: it never maps and reads no slot
count, so it has nothing to resize for.

---

## Ownership, and the `keep` list

`prte_util_filter_hostfile_nodes()` moves the caller's nodes onto a local
`keep` list and puts them back at the end so the result is in hostfile order.
Every early return therefore has to either restore or release that list —
those are the **caller's** nodes, and dropping them on the floor leaks node
objects. Route every exit through the single `cleanup:` label.

The same applies to walking a list while removing from it: save the successor
*before* releasing the item. The exclusion pass in
`prte_util_get_ordered_host_list()` kept iterating from the item it had just
released, which is a use-after-free on every hostfile that excludes a host
appearing more than once.

---

## Testing

- `test/unit/rmaps/test_resize.c` (`test_hostfile_cap`) covers the section
  above: that the cap applies and is recorded when selecting for a map, that
  it is restored, that VM-setup marking leaves the node alone, and that a
  count *larger* than the node changes nothing. It lives under `rmaps`
  because the record/restore list is a framework global that needs the
  framework opened.
- `test/unit/util/test_util.c` writes temporary hostfiles and parses them:
  slot counts, `max_slots`, comments, the `^` exclusion (including a duplicated
  excluded name), a `user@host` entry and the refusal of a second `@` in both
  the plain and the `rank N=` form, a `rank` entry with no host, a malformed
  file, and a good file parsed straight after a failed one.
- `contrib/dockerswarm/run-tests.sh` `test_util` covers what a single node
  cannot: that a `^host` line removes *that* machine and no other, and that a
  failed parse does not poison the next one across a real launch.
- The offline mapper harness consumes what this produces; run
  `make -C test/offline check-offline` after changing slot accounting.
