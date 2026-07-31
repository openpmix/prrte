# AGENTS.md — `src/pmix`

Orientation for AI agents and human contributors working in `src/pmix/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

Two files, about 900 lines, compiled straight into `libprrte`. There is no
framework here and no components.

| File | What it is |
|------|------------|
| `pmix-internal.h` | The header essentially every `.c` file in PRRTE includes. It pulls in the PMIx public headers, defines the lock type and its macros, the small list-item classes, and declares the translators. |
| `pmix.c` | The translators between PRRTE's and PMIx's integer spaces, the thread-shifted lock wakeup, and the class instantiations. |

### What this directory is *not*

It is easy to confuse three different things that all have "pmix" in the name:

| | Where | What it is |
|---|---|---|
| **The shim** | `src/pmix/` (here) | Translation and glue. No PMIx server, no upcalls, no state. |
| **The server host module** | [`src/prted/pmix/`](../prted/pmix/AGENTS.md) | The ~10k lines that answer PMIx's upcalls. This is what people usually mean by "the PMIx code". |
| **PMIx itself** | An external installation | The library. PRRTE uses a great deal of its *internal* API (`pmix_list_t`, `pmix_output_*`, the MCA base) — not just the public one. |

If you are looking for `spawn`, `fence`, `query`, or anything that talks to a
client, you want `src/prted/pmix/`, not here.

---

## GOLDEN RULE: the two integer spaces overlap, so nothing may pass through

This is the whole reason `pmix.c` exists, and the source of every defect it
has had.

- A PRRTE **error code** is `PRTE_ERR_BASE - n`, and `PRTE_ERR_BASE` is
  `PMIX_EXTERNAL_ERR_BASE` (-3000) — see
  [`src/include/constants.h`](../include/constants.h).
- A PRRTE **proc/job state** is a small positive integer, and so is PMIx's —
  see [`src/mca/plm/plm_types.h`](../mca/plm/plm_types.h).
- A PMIx status runs from `-1` down past `-160`.

The error codes were moved out of PMIx's range only recently. Until then
they were based at `0` and `-100`, and **46 PRRTE codes had the value of a
live PMIx status meaning something else** — `PRTE_ERR_SLURM_SHRINK_FAILURE`
was `PMIX_OPERATION_SUCCEEDED`, and `PRTE_ERR_TAKE_NEXT_OPTION`, a
control-flow signal rather than a failure, was `PMIX_ERR_NOT_FOUND`.
Returning an unconverted status did not produce a recognisably foreign
number that someone would notice; it produced a confidently wrong PRRTE code
that `prte_strerror()` would happily name. `prte_pmix_convert_status()` used
to end in `default: return status;`, which meant, among others:

| PMIx status | value | arrived as |
|---|---|---|
| `PMIX_ERR_PACK_FAILURE` | -21 | `PRTE_ERR_FILE_OPEN_FAILURE` |
| `PMIX_ERR_UNPACK_FAILURE` | -20 | `PRTE_ERR_FILE_WRITE_FAILURE` |
| `PMIX_ERR_NOMEM` | -32 | `PRTE_ERR_DATA_OVERWRITE_ATTEMPT` |
| `PMIX_ERR_EMPTY` | -60 | `PRTE_ERR_NODE_DOWN` |
| `PMIX_ERR_NOT_AVAILABLE` | -64 | `PRTE_ERR_PROC_CHECKPOINT` |

The pack/unpack ones are not hypothetical: PRRTE routes essentially every
bfrops return through this function — there are ~270 call sites, most of them
in `src/runtime/data_type_support/`.

**So: every one of these functions must be total.** Every `case` lands on a
constant in the target space, and so does `default`. A code you have no name
for is `PRTE_ERROR` / `PMIX_ERROR`, never the input.

The one remaining identity is `PRTE_SUCCESS == PMIX_SUCCESS == 0`.
`PRTE_ERROR == PMIX_ERROR == -1` used to be a second one, and it was the
worst-placed of the lot: it made a passthrough bug invisible in exactly the
case a test would most naturally construct. Now that PRRTE's codes hang off
`PMIX_EXTERNAL_ERR_BASE` the two error spaces are disjoint, and
`test/unit/pmix` sweeps both ranges to prove it — with no exemptions left.

## GOLDEN RULE: never spell a state as a bare integer

The four state translators are the same trap in a second dimension. The two
proc-state spaces share a *scheme* but not a *numbering*:

```
   PRRTE                     PMIx
   UNDEF          0          UNDEF             0
   INIT           1          PREPPED           1     <-- PRRTE has no PREPPED,
   RESTART        2          LAUNCH_UNDERWAY   2         so everything below the
   TERMINATE      3          RESTART           3         error boundary is
   RUNNING        4          TERMINATE         4         shifted by one
   REGISTERED     5          RUNNING           5
                             CONNECTED         6
   ...
   UNTERMINATED  15          UNTERMINATED     15    <-- these do line up
   TERMINATED    20          TERMINATED       20
   ERROR         50          ERROR            50    <-- and from here the
   KILLED_BY_CMD 51          KILLED_BY_CMD    51        offsets agree, name
   ...                       ...                       for name, to +13
```

`prte_pmix_convert_state()` was written with bare integer cases. Because the
error range *does* line up name-for-name, `case 59` looked harmless — and
returned `PMIX_PROC_STATE_MIGRATING` for `PRTE_PROC_STATE_HEARTBEAT_FAILED`,
while the real `MIGRATING` (60) fell through the switch. `TERMINATED` was
absent altogether, so `PMIX_QUERY_PROC_TABLE` reported every proc of a
finished job as `UNDEF`.

Nothing about that is visible at the point of failure: `UNDEF` is a legal
answer, so no error is logged anywhere. **Use the `PRTE_PROC_STATE_*` and
`PMIX_PROC_STATE_*` names, always**, and when you add a state to
[`plm_types.h`](../mca/plm/plm_types.h), add it here and to
`test/unit/pmix`'s table in the same commit.

---

## The six translators

| Function | Direction | Used by |
|----------|-----------|---------|
| `prte_pmix_convert_rc()` | PRRTE error → PMIx status | ~38 sites, mostly `src/prted/pmix/` returning from an upcall |
| `prte_pmix_convert_status()` | PMIx status → PRRTE error | ~270 sites, mostly pack/unpack |
| `prte_pmix_convert_state()` | PRRTE proc state → PMIx proc state | `pmix_server_queries.c`, the proc-table queries |
| `prte_pmix_convert_pstate()` | PMIx proc state → PRRTE proc state | nothing today — it is the declared inverse of the above and is kept, and tested, so the pair stays consistent |
| `prte_pmix_convert_job_state_to_error()` | PRRTE job state → PMIx status | `errmgr/dvm`, building the spawn response for a failed job |
| `prte_pmix_convert_proc_state_to_error()` | PRRTE proc state → PMIx status | the proc-failure notification path |

Two properties to preserve:

- **Collapses must agree.** PRRTE has six ways to say "lost touch with a
  peer" (`COMM_FAILED`, `UNABLE_TO_SEND_MSG`, `LIFELINE_LOST`,
  `NO_PATH_TO_TARGET`, `FAILED_TO_CONNECT`, `PEER_UNKNOWN`). PMIx has one
  bucket for each of the two questions — `PMIX_PROC_STATE_COMM_FAILED` and
  `PMIX_ERR_COMM_FAILURE`. Both translators must collapse the *same* six, or
  a tool asking twice about one event gets two stories.
- **`PRTE_ERR_SILENT` must survive.** It means "this failure has already been
  reported to the user, do not report it again". `convert_rc` used to have no
  case for it, so every silent abort became a second, redundant error at the
  tool.

The round-trip tests in `test/unit/pmix` are what enforce both.

---

## The lock, and the thread rule

`prte_pmix_lock_t` and its macros live in `pmix-internal.h`. The lock itself
is ordinary — a mutex, a condvar, an `active` flag, a status, and an optional
message — but **who may wake it is not**.

The top-level [`AGENTS.md`](../../AGENTS.md) has the full golden rule. The
part this directory owns is the helper:

```c
PRTE_EXPORT void prte_pmix_shifted_wakeup(prte_pmix_lock_t *lock,
                                          pmix_status_t status,
                                          char *msg);
```

A `prte_pmix_lock_t` is a **PRRTE** object. A callback running on the PMIx
progress thread must not touch it — not even to set `active = false`. The
reason is specific and was found the hard way: a waiter on the thread that
*drives* `prte_event_base` waits by re-checking `lock.active` between
`prte_event_loop(PRTE_EVLOOP_ONCE)` iterations, not by blocking in the
condvar. A `pthread_cond_signal` delivered while that loop is parked inside
the event backend is never observed, and the process hangs. Posting the
wakeup as an event both wakes the loop and serializes the flag update.

So:

- **From a PMIx callback**, call `prte_pmix_shifted_wakeup()`. It takes
  ownership of `msg`, which must be NULL or heap-allocated.
- **From the PRRTE progress thread**, `PRTE_PMIX_WAKEUP_THREAD()` is correct
  and is what the shifted handler itself uses.
- **Waiting on the thread that drives `prte_event_base`** — `prte()` itself,
  `prted`, `prte_daemon_recv` — uses the loop-driving idiom, *never*
  `PRTE_PMIX_WAIT_THREAD`:

  ```c
  while (prte_event_base_active && lock.active) {
      prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
  }
  ```

- **`PRTE_PMIX_WAIT_THREAD` is for threads that do not drive the loop** —
  `prun` (a tool, no PRRTE event base) and code inside `prte_init()` (the
  loop does not exist yet). Anywhere else it is almost certainly a bug.

There used to be `PRTE_PMIX_ACQUIRE_THREAD` and `PRTE_PMIX_RELEASE_THREAD`
here too. Nothing used them, and `RELEASE`'s non-debug branch asserted on a
`pmix_mutex_trylock()` — a side effect inside `assert()`, which vanishes
under `NDEBUG` and, when it did run, tried to lock a non-recursive mutex the
caller was required to already hold. They are gone; do not reintroduce that
shape.

---

## `PMIX_SERVER_URI` is collected, and it is not an RML thing

Worth stating because the name invites exactly the wrong assumption.

`PMIX_SERVER_URI` is the rendezvous address of a node's **PMIx server** — how
a *client or tool* connects to it. It is **not** how daemons reach each
other; that is the RML, using `PMIX_PROC_URI`. No daemon ever opens a PMIx
connection to another daemon, and nothing in PRRTE consumes this key
internally.

It exists for one consumer: a **tool** that asks the DVM "where is the PMIx
server on node X?" so it can connect there directly —
`PMIx_Query(PMIX_SERVER_URI)` qualified by `PMIX_HOSTNAME` or `PMIX_NODEID`,
as in [`examples/tool.c`](../../examples/tool.c) (`--uri <nodename>`).

The value travels the **same two hops as `PMIX_PROC_URI`**, one field later
in each message — collect to the master, then hand the whole set back out:

1. **Collect.** Each daemon fetches its own server URI from its PMIx server
   and packs it into its `PRTE_RML_TAG_PRTED_CALLBACK` rollup, right after
   `PMIX_PROC_URI` — [`src/tools/prted/prted.c`](../tools/prted/prted.c).
   The master unpacks it in `prte_plm_base_daemon_callback()` and does
   `PMIx_Store_internal(&dname, PMIX_SERVER_URI, ...)` against that daemon's
   name — [`plm_base_launch_support.c`](../mca/plm/base/plm_base_launch_support.c).
2. **Distribute.** `vm_ready()` builds the nidmap, appends every daemon's
   name + `PMIX_PROC_URI` + `PMIX_SERVER_URI` to that same buffer, and
   xcasts it on `PRTE_RML_TAG_WIREUP` —
   [`state_dvm.c`](../mca/state/dvm/state_dvm.c). Every daemon stores what
   it receives in `process_wireup()` —
   [`grpcomm_direct_xcast.c`](../mca/grpcomm/direct/grpcomm_direct_xcast.c).
3. **Serve.** The query fetches it with a `PMIx_Get` keyed on exactly that
   name, which is what `PRTE_MODEX_RECV_VALUE_OPTIONAL` does — so the query
   code needed no change at all.

Consequences to keep in mind:

- **Every daemon can answer for every node**, and that uniformity is the
  point: a tool must not get a different answer depending on which daemon it
  happened to connect to. Collecting only at the master would have made the
  query's result depend on the tool's attachment point.
- **Grow is covered by the same path; shrink needs nothing.** `vm_ready()`
  runs on `VM_READY`, which fires again whenever the daemon set changes, and
  it re-sends the *whole* set — so a grow redistributes without any code of
  its own. A shrink NULLs `node->daemon`, and the query resolves
  hostname → node → daemon, so a departed node simply cannot be answered
  for; its store entry is orphaned under a vpid the DVM will never reuse.
- **A daemon that cannot report one packs a NULL, and that is not an
  error.** This is auxiliary information; it must never fail a launch or a
  wireup.
- **In `process_wireup()` the server URI must be unpacked before the
  `continue`s.** It is a per-record field, so skipping it for a daemon whose
  `PMIX_PROC_URI` we already have would leave it in the buffer and the next
  iteration would read that string as a `pmix_proc_t`.
- **The URI is only useful to a remote tool if the server accepts remote
  connections** (`--prtemca pmix_remote_connections 1`,
  `prte_pmix_server_globals.remote_connections`). Off — the default — every
  server binds loopback, so the answer is truthful but only usable by a tool
  on that node. That is the requester's business, not the DVM's; serve what
  we have.
- This closes the half-built feature from commit `6e481fbb95` (2019), whose
  message promised the collection but whose diff only ever contained the
  query side and the example.

---

## Everything else in the header

- **The list-item classes.** `prte_info_item_t` (a `pmix_info_t` on a list),
  `prte_info_array_item_t` (a list of those), `prte_value_t`, and
  `prte_pmix_app_t`. They exist because PMIx's arrays are fixed-size and the
  command-line parsers accumulate before they know the count.
  `prte_pmix_app_t` additionally holds `mapby`/`rankby`/`bindto` aside until
  the whole command line is parsed, because a directive given *once* belongs
  to the job rather than to any app — `prte_parse_locals()` decides and
  distributes them.
- **`PRTE_MODEX_RECV_VALUE_OPTIONAL`.** A `PMIx_Get` with `PMIX_OPTIONAL`.
  **It yields a `pmix_status_t`, not a PRRTE code** — this has caught people:
  `pmix_server_queries.c` fed its result to `prte_pmix_convert_rc()`, the
  PRRTE→PMIx direction, turning every failure on the `PMIX_SERVER_URI` query
  into a bare `PMIX_ERROR`. It works by accident when compared against
  `PRTE_SUCCESS`, since both spaces agree on 0.
- **`prte_attribute_t`** is *declared* here but instantiated in
  [`src/runtime/prte_globals.c`](../runtime/prte_globals.c) and used through
  [`src/util/attr.h`](../util/attr.h). That split is historical, not
  principled.

---

## Conventions

Everything in the top-level [`AGENTS.md`](../../AGENTS.md) applies. Two
things bite here in particular:

- **No `PMIX_`/`pmix_`-prefixed names for PRRTE symbols.** This header once
  defined `PMIX_PROC_NTOH`/`PMIX_PROC_HTON` and two `pmix_proc_*_intr()`
  inlines in PRRTE's own header — squarely in PMIx's namespace, and a
  collision waiting for PMIx to define the same names. They are gone.
- **No compatibility shims.** The header used to `#ifndef` a fallback
  definition of `PMIX_DATA_BUFFER_STATIC_INIT` for PMIx versions that lacked
  it. PRRTE requires PMIx ≥ 6.1.0, which has it; a local copy is a second
  maintenance path. Raise the requirement in `configure` instead.

---

## Testing

**Unit — `test/unit/pmix/test_pmix` (`make check`).** Everything in `pmix.c`
is a pure integer mapping with no state and no I/O, so the coverage is
essentially total and should stay that way:

- every named `PRTE_PROC_STATE_*`, as an explicit table, in both directions,
  plus a round-trip requirement so the two converters cannot drift apart
  (which is exactly how the 59/60 transposition survived — `convert_pstate`
  had `MIGRATING → 60` all along);
- a sweep of the whole PMIx status range proving nothing passes through
  unconverted and no error status can produce `PRTE_SUCCESS`;
- the same sweep outbound over both PRRTE code families;
- round trips for the codes that must survive one, `PRTE_ERR_SILENT` first;
- the two `*_to_error()` tables.

**Add to the table in the same commit as any new state or code.** A
translator that answers `UNDEF`/`PMIX_ERROR` for an input it simply forgot is
indistinguishable from one answering correctly.

**Multi-node — `contrib/dockerswarm`, the `test_pmix` phase.** The table test
cannot show that the mapping is *reached*, with real proc states, on a daemon
that is not the one you are standing on. The `proctable` client covers:

- `PMIX_QUERY_PROC_TABLE` over a job spread across four nodes — every proc
  must report a state PMIx defines, and none may report `UNDEF`;
- `PMIX_QUERY_LOCAL_PROC_TABLE`, whose local-vs-global distinction has no
  meaning at all on one host;
- the master serving **any** node's `PMIX_SERVER_URI`, and a failure
  reporting a real status rather than a flattened `PMIX_ERROR` — the direct
  regression test for the wrong-direction conversion described above.

  Note who the consumer is: **not a daemon**. Daemons reach each other over
  the RML and never form PMIx connections to one another. This query exists
  for a *tool* — see `examples/tool.c --uri <nodename>` — which asks the DVM
  where a particular node's PMIx server is so the tool can connect to it
  directly. See the section below for how that value gets to the master.

What is deliberately **not** tested anywhere automatically: the lock macros
and `prte_pmix_shifted_wakeup()`. Their contract is a threading one — it
needs a live `prte_event_base` and a second thread to say anything
meaningful — and it is exercised implicitly by every live smoke test, since a
violation manifests as a hang.

---

## Where to go next

- [`../prted/pmix/AGENTS.md`](../prted/pmix/AGENTS.md) — the PMIx server host
  module: the upcalls, the request trackers, and the relay pattern. This is
  where the translators are called from.
- [`../runtime/AGENTS.md`](../runtime/AGENTS.md) — `prte_job_t` /
  `prte_node_t` / `prte_proc_t`, and where `prte_attribute_t` really lives.
- [`../mca/plm/plm_types.h`](../mca/plm/plm_types.h) — the job and proc state
  definitions the state translators mirror.
- [`../include/constants.h`](../include/constants.h) — the PRRTE error codes
  the status translators mirror.
