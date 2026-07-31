# AGENTS.md — `src/event`

Orientation for AI agents and human contributors working in `src/event/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

Two files. [`event-internal.h`](event-internal.h) is PRRTE's entire event
API — mostly macros over libevent — and [`event.c`](event.c) holds the four
functions that cannot be macros. There is no framework here, no components,
and no MCA parameters.

Small as it is, this is the substrate the rest of PRRTE runs on. PRRTE is
event-driven and single-threaded on the progress thread; *everything* —
launch, I/O forwarding, timeouts, signal handling, and every thread-shift
out of PMIx — is a callback on the base created here.

| Piece | What it is |
|-------|------------|
| `prte_sync_event_base` | The base `prte_event_base_open()` creates. |
| `prte_event_base` | **An alias for the same base**, not a second one. |
| `prte_event_base_open()` / `_close()` | Idempotent create/destroy of that base. `prte_init` opens it; `prte_finalize` closes it. |
| `prte_event_alloc()` | `calloc` of a `struct event`. The zeroing is load-bearing — see below. |
| `prte_event_assign()` | The one wrapper that has to convert a return code, and so cannot be a macro. |
| `prte_event_list_item_t` | A `struct event` that can go on a `pmix_list_t`. `prun` keeps its forwarded-signal handlers on a list of these. |
| `PRTE_PMIX_THREADSHIFT` | The macro. Over a hundred call sites. See the golden rule in the top-level `AGENTS.md`. |

The event library is **Libevent**, and there is no alternative. PRRTE and
PMIx share an event base and PMIx is Libevent-only, so a PRRTE built against
anything else could not talk to the PMIx it is compiled against. See "libev
is gone" below.

---

## GOLDEN RULE: `prte_event_base` and `prte_sync_event_base` are one object

`prte_event_base_open()` sets them equal and the comment in `event.c` says
why: *"PRTE tools block in their own loop over the event base, so no
progress thread is required."* Code that posts work to `prte_event_base`
and code that loops over `prte_sync_event_base` have to meet, and they only
do because it is the same base.

Two consequences:

- Anything that tears one down must clear **both**. `prte_event_base_close()`
  used to free the base and leave both globals pointing at it. Nothing
  called it, so nothing noticed — and the failure it was storing up would
  have looked like an ordinary `prte_event_set()` on a shutdown path, since
  PRRTE's macros take the base as an *argument* rather than looking it up.
- The waiting idiom in the top-level `AGENTS.md` — spinning
  `prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE)` instead of
  `PRTE_PMIX_WAIT_THREAD` — works *because* of this aliasing. A thread-shifted
  wakeup posted to `prte_event_base` is run by the loop over
  `prte_sync_event_base`.

Progress threads are a different matter: `prte_progress_thread_init()`
(in [`src/runtime`](../runtime/AGENTS.md)) creates its own base per named
thread. `odls` runs several. Those bases are **not** this one.

---

## GOLDEN RULE: an event is allocated here and assigned to a base later

`prte_event_alloc()` returns **zeroed** storage, and that is the whole
reason it exists rather than callers using `malloc`.

The pattern throughout the tree is: a constructor (`prte_timer_t`, the odls
launch-local caddy, the iof read/write events) allocates the event when the
object is created, but `event_assign()` happens only when the object is
*activated* — which may never happen. Every one of those destructors frees
the event unconditionally. libevent's `event_free()` calls `event_del()`,
which dereferences the event's internal fields; on raw `malloc`'d storage
those are garbage and the free crashes. A zeroed event has a NULL base,
which `event_del()` rejects cleanly (with a warning on stderr), and a later
`event_assign()` overwrites the zeros.

So: **do not replace `prte_event_alloc()` with `malloc`, and do not
"optimize away" the `calloc`.**

The asymmetry to be aware of: the allocation is plain `calloc`, but
`prte_event_free()` is libevent's `event_free()`, which frees through
libevent's allocator. They agree because PRRTE never calls
`event_set_mem_functions()`. If PRRTE ever does, this pairing breaks.

---

## Reading the header

A few things about `event-internal.h` are worth knowing before you extend
it, because they are exactly the traps it has already sprung:

- **A macro nobody expands is never compiled.** `prte_event_new()` was
  declared here and defined nowhere, with `prte_event_evtimer_new()` layered
  on top of it; the first use of either would have been an undefined-symbol
  *link* error. And `prte_event_base_init_common_timeout` was written with a
  space between the name and the parameter list, making it an object-like
  macro whose expansion was a syntax error. Both sat here indefinitely
  because nothing referenced them. If you add a wrapper, either use it or
  cover it in `test/unit/event`.

- **Route new wrappers through the `prte_event_*` spelling**, not straight
  to libevent. Today they are the same macro, so this is a consistency point
  rather than a correctness one — but it is the seam where a wrapper would
  grow a body if one ever needs to, and the signal macros were the only ones
  that bypassed it.

- **`PRTE_EVENT_SIGNAL()` recovers the signal number an event was set for.**
  `ess/base` and `prte_wait.c` use it, and `prted` forwards that number to
  its children — so getting it wrong forwards the wrong signal.

- **`PRTE_PMIX_THREADSHIFT` requires the caddy's event member to be named
  `ev`**, and requires it to be an embedded `prte_event_t`, not a pointer.
  The macro does `prte_event_set` + `event_active` with no `event_add`,
  which is what makes the handler run on the next loop pass rather than
  waiting for a real fd or timeout.

- **Do not allocate a caddy on the stack.** It has to outlive the function
  that posts it. See the caddy section of the top-level `AGENTS.md`.

---

## Where the base is opened, and by whom

| Caller | Why |
|--------|-----|
| `prte_init()` | The DVM and daemons — the general case. |
| `src/prted/prte.c` | Before `prte_init`, so a SIGTERM arriving during startup has somewhere to land (the term pipe is a `PRTE_EV_READ` event). |
| `src/tools/prun/prun.c` | `prun` is a tool: it never calls `prte_init`, but it still needs a base to run the schizo parser and its PMIx connection over. |

All three go through the same `initialized` guard, so calling it more than
once is free. `prte_finalize()` closes it, as the last thing it does —
after `PMIx_server_finalize()`, because PMIx server-module upcalls
thread-shift onto this base and can still run during that call.

`prte.c` also calls `prte_event_reinit()` after `--daemonize`: a base does
not survive a `fork()` intact. See
[`src/prted/AGENTS.md`](../prted/AGENTS.md).

---

## Testing

**Unit tests — `test/unit/event/test_event.c`, run by `make check`.**
Covers base open/close/reopen and the aliasing invariant, the zeroed
allocation and the free-a-never-assigned-event path, dispatch of activated
events and timers (including that a deleted timer does not fire), signal
events end to end, `PRTE_EVENT_SIGNAL()`, `PRTE_PMIX_THREADSHIFT` delivery,
and the `prte_event_list_item_t` class.

Everything here is single-threaded on purpose. The multi-threaded half of
PRRTE's event machinery is the progress-thread layer in `src/runtime`, and
it is tested by `test/unit/runtime`.

**Multi-node — `contrib/dockerswarm`, the `test_event` phase.** What a unit
test cannot reach is the path where an event on one node's base causes work
on another's: `prun` catches a signal on its own base, forwards it over the
RML, and each `prted` re-raises it against its local children. That crosses
`prte_event_list_item_t`, `PRTE_EVENT_SIGNAL()` and the signal wrappers, and
with a single node the forwarding step is skipped entirely.

---

## libev is gone

PRRTE used to accept libev as an alternative to Libevent, selected by
`--with-libev` and reported through a `PRTE_HAVE_LIBEV` symbol that gated a
second arm of `event-internal.h` and about 120 lines of
[`prte_progress_threads.c`](../runtime/prte_progress_threads.c) — an
`ev_async` per tracker, a mutex, a caddy list, and function-bodied
`prte_event_add`/`_del`/`_active`/`_base_loopexit` that queued each
operation onto the thread owning the loop (a libev loop is not safe to
modify from anywhere else).

It is removed, for a reason that has nothing to do with libev's merits:
**PMIx is Libevent-only**, and PRRTE shares an event base with the PMIx it
is compiled against. The two cannot disagree about which library owns that
base.

It is also a case worth remembering when weighing "keep the alternative,
it's only a few `#if`s". Nothing in CI or in `contrib/dockerswarm` ever
configured `--with-libev`, so that arm was compiled by nobody — and it had
rotted accordingly. `prte_event_new()` was declared and never defined; the
`prte_event_base_loopexit` shim did `assert(NULL != trk)` where its three
siblings tolerated a NULL tracker, so a `loopexit` on the main base (which
has no tracker) would have aborted the process. An alternative nothing
builds is not an alternative.

---

## Do not grow this directory

PRRTE gets its threading primitives, its lists, its output streams and its
progress-thread integration from **PMIx**. This directory exists only to
give PRRTE a base of its own and a house spelling for the libevent calls it
makes. Now that there is only one event library, the bar for a new wrapper
here is high: if it would just rename a libevent function, it does not earn
its place. Anything about PRRTE's lifecycle belongs in `src/runtime`;
anything generic belongs in PMIx.
