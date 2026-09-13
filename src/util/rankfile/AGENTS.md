# AGENTS.md — `src/util/rankfile`

Orientation for AI agents and human contributors working in
`src/util/rankfile/`. This is a map, not the rulebook: the authoritative
guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md), under
[`docs/`](../../../docs/), and in [`src/util/AGENTS.md`](../AGENTS.md). When
this file and those disagree, **the docs win** — and please fix this file.

---

## What lives here

The rankfile parser, and the record it produces:

```c
PRTE_EXPORT int prte_util_parse_rankfile(const char *rankfile,
                                         pmix_hash_table_t *rankmap,
                                         int *num_ranks);
PRTE_EXPORT void prte_util_rankfile_clear(pmix_hash_table_t *rankmap);
```

The caller supplies `rankmap` already constructed and initialized, and each
line's record is filed under the rank it describes, as a `uint32` key. The
table is the caller's on every outcome, failure included — the parser does
not reclaim what it filed, because the caller is what owns it.
`prte_util_rankfile_clear()` releases every record and empties the table, and
is how both the mapper and the corpus test give them back.

## The map is keyed by rank, not indexed by it

It was a `pmix_pointer_array_t` indexed by rank, and a pointer array is as
large as its largest index: `pmix_pointer_array_set_item()` grows the table
to reach the slot and writes NULL into every entry below it. The ranks a file
names need not be dense, and nothing the parser knows bounds them — the
file's own line count does not, because a rank the file leaves out may be
placed by a default cpu list. So one line reading `rank 1000000000=nodeA` had
the head node allocate and zero eight gigabytes before the mapper looked at
the job, and the `set_item` that could fail was not checked. Do not put the
array back to save a lookup; a job's rank count is what bounds the mapper's
walk over the table, and it is the mapper that knows it.

## Why it is here and not in `rmaps/rank_file`

A rankfile is a file the user wrote. Turning its text into records is not a
mapping policy, and the mapping component keeps all of the policy.

The practical half of the reason is that a parser inside a component cannot
be tested. It was `static`, it wrote into module statics, and under
`--enable-mca-dso` the component is a separate shared object that nothing
outside it can link against — so the rankfile format had no coverage of its
own, only whatever fell out of an end-to-end map. It has 48 goldens now
(`test/unit/util/test_rankfile_corpus.c`), and writing them found four
defects.

Taking the output as parameters rather than module statics also settled a
wart: because the parse wrote into statics, the component had to reclaim
that state on two separate paths with two near-identical loops.

---

## The format

```
rank <N> = <hostname> [ slot = <cpu list> ]
```

Every line is exactly that, and the file's documentation
([`docs/prrte-rst-content/detail-placement-rankfiles.rst`](../../../docs/prrte-rst-content/detail-placement-rankfiles.rst))
says so: *"Each line of a rankfile specifies the location of one process."*
Spaces around the `=` are optional on both sides, `slots` is accepted for
`slot`, the hostname may carry a `user@` prefix, and `+n<K>` names a node by
its position in the allocation. Comments are `#`, `//` and `/* */`.

Reading is done by [`src/util/textfile.h`](../textfile.h), which hands back
one line already stripped of comments and split into fields with `=` always
a field of its own — so `rank 0=nodeA` and `rank 0 = nodeA` arrive as the
same four fields. There is no lexer.

---

## A line contributes its record only when the whole line is accepted

The parser this replaced filed a record the moment it read the rank and
filled it in as the rest of the line arrived. So a line refused after that
point left the map holding a record with no node name, or no slot list,
counted in `num_ranks` — a phantom rank the caller would have mapped had the
refusal not stopped it. Build the record on the stack, file it last.

## A duplicate rank is refused wherever it appears

The check used to live on the `slot=` path alone, so two lines naming rank 0
and then stopping were accepted in silence: the second record overwrote the
first in the map — leaking it, since `pmix_pointer_array_set_item()` replaces
a slot without releasing what was there — `num_ranks` counted both, and the
file's answer was quietly the last one. The test is now "is this rank
already filed", made before anything else on the line is done, and the
record already there is left alone.

## `slot_list` is allocated, and there is no maximum

It was a fixed `char[64]` in the record, copied into up to its length and no
further with nothing said. `slot=0,1,2,...,25` is already over that, so a
rank given a long explicit cpu list was bound to a prefix of what the user
asked for. NULL means the line named a node and stopped.

## A relative node is `+n<K>` or `+N<K>`, and the mapper must read both

The parser stores a relative name exactly as written, and it accepts either
case of the letter. The mapper used to read the index with
`atoi(strtok(name, "+n"))`, whose delimiter set skips a lower-case `n` and not
an upper-case one — so `+N3` became `atoi("N3")`, which is zero, and the rank
went to the first node without a word. `rmaps_rank_file.c` now reads the
digits after the two-character prefix with `strtol()` and range-checks them.
If you change what the parser accepts here, look at that consumer too.

## `user@host` takes exactly one `@`, with something on both sides

`node_from_entry()` finds the `@` itself. `PMIx_Argv_split()` drops empty
fields, so the version that used it took `someone@` as a node named
`someone`, and `@nodeA` and `someone@@nodeA` as `nodeA`. The same trap was in
the hostfile parser; see [`../hostfile/AGENTS.md`](../hostfile/AGENTS.md).

## A read that fails is not the end of the file

`prte_textfile_next()` returns NULL for both, and the parser checks
`tf.failed` once the loop ends. Taking an early NULL for the end of the file
accepted a partly-read rankfile as the whole of it, and an app that gave no
`-n` took its process count from however many lines had arrived.

## The username is dropped, and `username=` is refused

Those are two different answers to the same question, and it is worth
knowing before you change one of them. The hostfile parser keeps a `user@`
prefix as a `PRTE_NODE_USERNAME` attribute on the node; a rankfile record
has nowhere to put one, so the prefix is parsed off and discarded — while
the explicit `username=` spelling is refused as unsupported. Making these
agree means giving the record somewhere to keep it.

---

## Testing

- `test/unit/util/test_rankfile_corpus.c` (`make check`) is the golden
  corpus: 48 rankfile bodies against a canonical rendering of the return
  code, the rank count and every record filed. Add a case here for anything
  you change. A golden that moves is a decision, and belongs in the commit
  message — never a quiet re-baseline.
- `test/unit/util/test_textfile.c` covers the line reader underneath.
- `test/unit/rmaps/test_rank_file.c` covers the component's dispatch guards.
- The offline mapper harness (`make -C test/offline check-offline`) has **no**
  rankfile cases. To see a rankfile's map end to end without a DVM, run
  `prterun --rtos donotlaunch --display map --prtemca hwloc_use_topo_file
  <topo> -H nodeA:4,nodeB:4 --map-by rankfile:file=<path> -n 2 hostname` by
  hand. `contrib/dockerswarm/run-tests.sh` places a rankfile across real
  nodes.
