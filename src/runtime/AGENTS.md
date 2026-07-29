# AGENTS.md — `src/runtime`

Orientation for AI agents and human contributors working in `src/runtime/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

`src/runtime` is the foundation the rest of PRRTE is built on. It is not a
framework and has no components: it is the process lifecycle, the global
state, and the object model. Practically everything else in the tree
includes [`prte_globals.h`](prte_globals.h).

Five separable things share the directory:

| Area | Files | What it is |
|------|-------|------------|
| **Object model & globals** | `prte_globals.[ch]` | The three central data structures (`prte_job_t`, `prte_node_t`, `prte_proc_t`), plus `prte_session_t`, `prte_app_context_t`, `prte_topology_t`, the launch-fence campaign objects — their class instances, and the global registries (`prte_job_data`, `prte_node_pool`, `prte_sessions`, ...) with the lookup functions over them. |
| **Lifecycle** | `prte_init.c`, `prte_finalize.c`, `prte_quit.[ch]`, `prte_locks.[ch]` | The three-stage startup (`prte_init_minimum` → `prte_init_util` → `prte_init`), teardown, the "stop looping the event base" path, and the one-shot mutexes that keep re-entrant shutdown from bouncing. |
| **MCA parameters** | `prte_mca_params.c` | `prte_register_params()` — every `prte_*` MCA variable, registered once. |
| **Threads & timers** | `prte_progress_threads.[ch]`, `prte_wait.[ch]` | Named progress threads (event base + engine thread + refcount), the SIGCHLD/waitpid plumbing, and the `PRTE_DETECT_TIMEOUT`/`PRTE_TIMER_EVENT` macros. |
| **Sub-directories** | [`data_server/`](data_server/AGENTS.md), [`data_type_support/`](data_type_support/AGENTS.md) | The PMIx publish/lookup service, and the pack/unpack/copy/print functions for the objects above. Each has its own AGENTS.md. |

---

## The three-stage init, and why it is three

Tools do not all need the same amount of runtime, and some of them need a
*part* of it before they can decide what else to do. Hence:

```
prte_init_minimum()   PMIx version check, installdirs, MCA var system,
                      prte_register_params(), MCA param files preloaded.
                      No event base, no frameworks, no globals.
        │
        ▼
prte_init_util(flags) + malloc/output init, hostname, stack handlers,
                      system limits, prtebacktrace.  Records proc_type.
        │
        ▼
prte_init(argc,argv,flags)
                      + event base, locks, proc_info, hwloc, the global
                        arrays, prte_default_session, schizo + ess
                        selection, prte_ess.init(), the cache and the
                        launch-fence arrays.
```

Each stage is idempotent behind its own `static bool` (`min_initialized`,
`util_initialized`, `prte_initialized`). `prte_register_params()` has its
own `passed_thru` guard because `mpirun` reaches it twice.

**Consequences for anything you add here:**

- Something registered in `prte_register_params()` is available to *tools*
  that never call `prte_init()`. Something set up in `prte_init()` is not.
- MCA variables read their environment exactly once, on first registration.
  That is why the bootstrap daemon has to publish the DVM-wide parameters it
  reads out of `prte.conf` (`prte_ess_base_bootstrap_params()`) *before*
  `prte_register_params()` runs, and why that call sits where it does.
- The global arrays (`prte_job_data`, `prte_node_pool`, `prte_sessions`,
  `prte_node_topologies`) are **NULL** until `prte_init()`. Any function
  reachable from a tool or a parser has to tolerate that.

---

## The object model

### Class hierarchy — get the declared parent right

Every object here is a PMIx reference-counted object. The parent named in
`PMIX_CLASS_INSTANCE` **must be the type actually embedded as the struct's
first member**. The class system runs the whole ancestor chain's
constructors and destructors, so naming a wider parent than the struct
embeds makes those run over the object's own leading fields.

`prte_session_t` embeds `pmix_object_t` and was registered against
`pmix_list_item_t`. The constructor damage was invisible (`session_con` runs
afterwards and rewrites the same bytes), but `pmix_list_item_destruct`
asserts that the item is not still on a list, and under a debug PMIx it read
that "refcount" out of the middle of the session's own data and aborted the
process. It survived for as long as it did only because
`prte_default_session` — the one session that always exists — is never
released.

Current parents: `prte_job_t`, `prte_node_t`, `prte_proc_t`,
`prte_attribute_t`, and the two campaign objects are `pmix_list_item_t`
(they really do go on lists); `prte_app_context_t`, `prte_job_map_t`,
`prte_topology_t`, `prte_timer_t`, and `prte_session_t` are `pmix_object_t`.

### Who owns whom

This is the part that bites. The rules, and the reason for each:

| Reference | Counted? | Why |
|-----------|----------|-----|
| `job->procs[]`, `job->apps[]` | yes | the job owns them |
| `node->procs[]`, `node->daemon` | yes | the node owns them |
| `proc->node` | **no** — borrowed | retaining it would close a cycle with `node->procs`/`node->daemon`; neither side could ever reach zero, so nothing would be freed. `prte_node_destruct` clears the backpointer on every proc it knows about, so a proc that outlives its node points at NULL rather than at freed memory. |
| `node->session` | **no** — borrowed | same reason, against `session->nodes`. `prte_ras_base_release_allocation` clears it when a reservation is torn down. |
| `session->nodes[]` | yes | a reservation withholds its nodes |
| `session->jobs[]` | **no** — borrowed | a job's lifetime is governed by the global job pool, not by the session it ran in |
| `session->children[]`, `session->owner_job` | yes | |
| `job->target_sessions[]` | **no** — borrowed | owned via `prte_set_session_object`; the destructor frees only the array |
| `node->topology` | yes | a topology outlives any one node pointing at it |
| `map->nodes[]` | yes | `prte_job_map_destruct` releases every node it holds — so anything that *puts* a node in a map must retain it |

`prte_job_destruct` also has to reach into the job's attributes and release
the pointers hiding in them (`PRTE_JOB_TIMEOUT_EVENT`,
`PRTE_SPAWN_TIMEOUT_EVENT`, `PRTE_JOB_ABORTED_PROC`, `PRTE_JOB_INFO_CACHE`):
an attribute holding `PMIX_POINTER` is invisible to the generic attribute
teardown.

### The global registries

`prte_job_data` and `prte_sessions` are **slot maps, not append-only logs**:
`prte_set_job_data_object` / `prte_set_session_object` scan for the first
free slot and reuse it, and the destructors NULL their own slot out. Two
things follow — a destroyed object silently vacates its index, and the index
you were handed can be handed to somebody else later.

The lookups are linear scans over the whole array. That is fine at DVM
scale, but do not put one inside a per-proc loop on a launch path.

---

## Sessions (reservations)

A `prte_session_t` is an allocation. `prte_default_session` is the general,
unreserved pool; it is special-cased everywhere:

- its `nodes` array **is** `prte_node_pool` (`prte_init` releases the array
  the constructor made and substitutes the global one);
- it is owned by everyone — `prte_session_is_owned_by` and
  `prte_session_add_owner` both short-circuit on it;
- `prte_ras_base_release_allocation` returns immediately for it.

`prte_session_is_owned_by` is the ownership gate for spawning into a
reservation. Note the trap it stepped in: **`PMIX_CHECK_NSPACE` answers
"true" when either side is an empty nspace** — that is its wildcard rule.
`prte_pmix_server_globals.scheduler.nspace` is empty until a scheduler
connects, so testing it unguarded declared every namespace to be the
scheduler on every DVM running without one, and the whole check collapsed
into an unconditional yes. Guard any such comparison with
`PMIX_NSPACE_INVALID` first.

---

## Progress threads

`prte_progress_thread_init(name)` is refcounted by name and returns an
existing base for a name already in use. Each tracker owns an event base, a
`pmix_thread_t`, and a persistent long-timeout event that exists purely so
the base is never empty (an empty base makes `prte_event_loop` return
immediately and the engine spin).

`prte_progress_thread_pause(NULL)` means "pause them all" and is what
`prte_finalize` uses. With a name, `pause`/`resume`/`finalize` all report
`PRTE_ERR_NOT_FOUND` for a name that does not exist — the header has always
said so.

`prte_progress_thread_parse_cpus()` expands the `prte_progress_thread_cpus`
specification. It is deliberately outside the `HAVE_PTHREAD_SETAFFINITY_NP`
guard its only caller sits behind, so it can be tested on a platform without
`pthread_setaffinity_np`; ranges are **inclusive** of their upper bound.

---

## Gotchas before you edit

- **`prte_config.h` first.** Every `.c` file in the tree, no exceptions.
- **Logical macros are `#if`, not `#ifdef`.** `configure` emits things like
  `PRTE_PICKY_COMPILERS` and `PRTE_WANT_HOME_CONFIG_FILES` with
  `AC_DEFINE_UNQUOTED` as `0` or `1`, so they are *always defined* and
  `#ifdef` never excludes anything. `prte_finalize`'s entire teardown sat
  behind `#ifdef PRTE_PICKY_COMPILERS` and had therefore always run — and
  "correcting" it to `#if` would have silently deleted the teardown from
  every normal build. If you find one of these, work out which behavior is
  the intended one before changing the spelling.
- **The `#else` arms get compiled somewhere.** `--disable-per-user-config-files`
  turns `PRTE_WANT_HOME_CONFIG_FILES` off, and that arm had been calling a
  function that does not exist. If you add a variant arm, build it.
- **`PMIX_NEW` does not zero the allocation.** Anything a destructor
  `free()`s, or any field a comparison reads, has to be set by the
  constructor. The campaign objects and the data-server objects both learned
  this the hard way.
- **A pointer array's `size` is not the caller's to set.** Copy through
  `pmix_pointer_array_set_item` and let the array grow itself; blitting the
  bookkeeping across and then writing `addr[i]` directly overruns whatever
  the destination constructor allocated (`PRTE_GLOBAL_ARRAY_BLOCK_SIZE`
  entries).
- **`strtoul`'s end pointer is never NULL.** It points at the first
  unconsumed character, so a "did it stop at a delimiter" test has to look
  at `*end`, not at `end`.
- **Attributes are `prte_attribute_t`, not `prte_value_t`.** They look
  similar enough to walk with the wrong type and still compile; the value
  then comes from the wrong offset and the key is dropped entirely.
- **Only `PRTE_ATTR_GLOBAL` attributes go on the wire.** That asymmetry is
  load-bearing — the mapper works on an unpacked *copy* of the job, so an
  attribute it must read has to be GLOBAL. See
  [`data_type_support/AGENTS.md`](data_type_support/AGENTS.md).
- **Everything runs on the PRRTE progress thread.** Nothing in this
  directory may be touched from a PMIx callback without a thread-shift; see
  the golden rule in the top-level `AGENTS.md`.
- **Warnings are errors.** Debug builds enable `--enable-devel-check`.

---

## Session teardown at finalize

`prte_finalize` releases the sessions, and the node pool goes with them.
Three constraints fix the order, and all three have to hold:

1. **Sessions before the pool.** A reservation holds a *counted* reference
   on each of its nodes, so it has to give those back while the nodes are
   still alive.
2. **The default session last.** Its node array *is* `prte_node_pool`, so
   releasing it performs the pool teardown — every other session must
   already have let go by then. `prte_finalize` keeps a fallback loop for
   the case where there is no default session at all (a process that failed
   partway through `prte_init`).
3. **Before the `ras` framework closes.** `session_des` calls
   `prte_ras_base_release_allocation`, which walks
   `prte_ras_base.selected_modules`. Today nothing closes that framework —
   neither `ess/hnp`'s `rte_finalize` nor `ess/base`'s `prted_finalize` — so
   the list is still valid at finalize. **If you ever add that close, this
   block has to move ahead of it**, or the walk runs over a destructed list.

Reaching `release_allocation` here is deliberate, not collateral: it only
cancels an allocation PRRTE itself created dynamically and still tracks, so
a DVM that exits still holding a sub-allocation gives it back, while a
user's own resource-manager allocation is left alone.

`session->children` needs no separate walk. Nothing in the tree ever
populates that array — which also means `prte_sessions_related()` can only
ever report identity, and its parent/child branch is unreachable today.

## Known gaps

- **`prte_job_copy` and `prte_proc_copy` have no callers.** They are
  `PMIX_RETAIN` + assign, i.e. aliases rather than copies, which is a
  surprising thing for a function named `copy` to be. `prte_app_copy` and
  `prte_map_copy` have no callers either but are real deep copies.

---

## Testing

**Unit tests — `test/unit/runtime/test_runtime.c`, run by `make check`.**
Covers what needs no DVM, which is most of this directory: the job/session
registries (including slot reuse and duplicate rejection), session ownership,
proc lookups, node matching and its aliases, the copy functions (including a
map with more nodes than one array block, and the refcount arithmetic when
both maps are released), the pack/unpack round trips for job/app/proc/node/map
and the GLOBAL-vs-LOCAL attribute split, object lifetimes and the borrowed
backpointer contract, the exit-status macros, the data server's range checks
and object construction, and the progress-thread cpu parser and lifecycle.

It builds the global arrays by hand (`prte_init_util` does not) and calls
`PMIx_server_init` because `PMIx_Data_pack` refuses to run until PMIx is up.

**Multi-node — `contrib/dockerswarm`, the `test_runtime` phase.** The data
server is a genuinely distributed service — the store lives on the HNP and
every participant reaches it over the RML — so publish/lookup/unpublish
across nodes, the range and userid access checks between real processes, and
the `PMIX_WAIT` path where a lookup parks until a later publish satisfies it
all need real daemons. So does DVM teardown, which is where the object
destructors actually run.

**What is not covered anywhere:** the `prte_init`/`prte_finalize` sequence
itself beyond the smoke test, and the `#else` arm of
`PRTE_WANT_HOME_CONFIG_FILES` (nothing in CI configures with
`--disable-per-user-config-files`).
