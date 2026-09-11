# AGENTS.md — `src/util/rankfile`

Orientation for AI agents and human contributors working in
`src/util/rankfile/`. This is a map, not the rulebook: the authoritative
guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md), under
[`docs/`](../../../docs/), and in [`src/util/AGENTS.md`](../AGENTS.md). When
this file and those disagree, **the docs win** — and please fix this file.

---

## What lives here

The rankfile parser, and the record it produces. One entry point:

```c
PRTE_EXPORT int prte_util_parse_rankfile(const char *rankfile,
                                         pmix_pointer_array_t *rankmap,
                                         int *num_ranks);
```

The caller supplies `rankmap` already constructed, and each line's record is
filed at the index of the rank it describes. The array is the caller's on
every outcome, failure included — the parser does not reclaim what it filed,
because the caller is what owns it.

## Why it is here and not in `rmaps/rank_file`

A rankfile is a file the user wrote. Turning its text into records is not a
mapping policy, and the mapping component keeps all of the policy.

The practical half of the reason is that a parser inside a component cannot
be tested. It was `static`, it wrote into module statics, and under
`--enable-mca-dso` the component is a separate shared object that nothing
outside it can link against — so the rankfile format had no coverage of its
own, only whatever fell out of an end-to-end map. It has 41 goldens now
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
  corpus: 41 rankfile bodies against a canonical rendering of the return
  code, the rank count and every record filed. Add a case here for anything
  you change. A golden that moves is a decision, and belongs in the commit
  message — never a quiet re-baseline.
- `test/unit/util/test_textfile.c` covers the line reader underneath.
- `test/unit/rmaps/test_rank_file.c` covers the component's dispatch guards.
- The offline mapper harness (`make -C test/offline check-offline`) runs the
  whole path from file to printed map.
