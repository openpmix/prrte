# AGENTS.md — `iof/base` (the shared I/O-forwarding machinery)

Guide to `src/mca/iof/base/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the tag
model, and the end-to-end picture; this file is the close-up on the code
the two components share, and on the ownership rules that are easy to get
wrong here.

---

## What lives here

| File | Contents |
|------|----------|
| `base.h` | The framework struct, the six reference-counted structs, the read/write event macros, the size constants, and the `prte_iof_base_fd_always_ready` predicate. |
| `iof_base_frame.c` | Framework open/close/register, the `iof_base_output_limit` MCA param, `prte_iof_base_output()`, and **every** `PMIX_CLASS_INSTANCE` — the constructors and destructors that define who owns what. |
| `iof_base_select.c` | Pick-one component selection; copies the winning module into the global `prte_iof` and calls its `init()`. |
| `iof_base_output.c` | The sink write engine: `prte_iof_base_write_output` (producer), `prte_iof_base_write_handler` (consumer), `prte_iof_base_adjust_short_write` (the shared short-write re-base). |
| `iof_base_setup.[ch]` | The fork-time descriptor dance: `setup_prefork`, `setup_child`, `setup_parent`. |

Nothing here decides *policy*. It allocates the objects, arms the events,
moves the bytes, and hands the descriptors around; the components decide
what to do with them.

---

## Ownership — the part that keeps biting

Every struct in `base.h` is a PMIx class, so lifetime is refcounting. Four
rules cover essentially all of it.

### 1. A chunk the handler is looking at is off the list

Each write handler begins its iteration with

```c
while (NULL != (item = pmix_list_remove_first(&wev->outputs))) {
```

so from that instant the `prte_iof_write_output_t` belongs to the handler,
not to the write event. Releasing the sink or the write event frees only
what is **still queued**. Every exit from the loop body must therefore
either `PMIX_RELEASE(output)` or `pmix_list_prepend` it back — there is no
third option, and "we are closing the stream anyway" is not one of them.

All three handlers (this one, plus the near-identical copies in
`hnp/iof_hnp.c` and `prted/iof_prted.c`) used to return on the zero-byte
sentinel without releasing it. That leaked one chunk — a fixed
`PRTE_IOF_BASE_TAGGED_OUT_MAX` (8192) byte buffer plus header — for every
stdin stream that was ever closed, which on a persistent DVM means per
proc, per job, forever.

### 2. A read event owns its fd; a write event owns its fd only above 2

`prte_iof_base_read_event_destruct` `prte_event_free`s the event and
`close()`s `rev->fd`. `prte_iof_base_write_event_destruct` does the same
but guards the close with `2 < wev->fd`, so a sink pointed at the daemon's
own `stdout`/`stderr` cannot close it. Both fall back to a plain
`free(ev)` when the fd is `-1`, because an event that was never assigned
to a base must not be handed to `prte_event_free`.

### 3. The proc and its read events form a reference **cycle**

A `prte_iof_proc_t` owns its read events, and `PRTE_IOF_READ_EVENT` does
`PMIX_RETAIN(p)` for each one it builds. So a proc with both streams wired
sits at refcount 3 — one for the component's `procs` list, one per read
event — and **releasing the proc alone can never free either side.**

Two consequences, and both have been live bugs:

- **`complete()` has to break the cycle explicitly.** Both components'
  `complete()` used to remove the proc from `procs` and release it, which
  for a proc whose streams are still open frees nothing: it produces an
  *orphan* — a proc nothing can reach, still holding its pipe descriptors,
  with its read events still armed and pointing back at it. A job that
  reached IOF completion normally has closed its streams by then; a job
  that was **killed** has not (`failed_start()` in `errmgr/prted` flags
  `PRTE_PROC_FLAG_IOF_COMPLETE` directly without the iof ever tearing
  anything down), and that is precisely the case `complete()` exists for.
  Both now release `stdinev`/`revstdout`/`revstderr` first, the way
  `close()` does.

- **Order matters, and only one order is safe.** Release the streams
  *while a reference to the proc is still held*: each stream release drops
  the read event's reference as a side effect, so the owner's release is
  the last one and runs the destructor at a safe moment. The other order —
  drop the proc's last reference, then release the read event — frees the
  proc from inside the read event's destructor, while `PMIX_RELEASE` is
  still about to write `NULL` into the slot that lives in it. That is why
  both read handlers bracket their teardown with `PMIX_RETAIN(proct)` /
  `PMIX_RELEASE(proct)`: the read event they are dropping may be holding
  the only remaining reference. The HNP's handler always did; the daemon's
  did not. `test/unit/iof` pins the whole sequence.

### 4. `PMIX_RELEASE` nulls the pointer it was handed — when it frees

The PMIx macro assigns `object = NULL` inside the `if (0 == refcount)`
arm. So `PMIX_RELEASE(proct->revstdout)` clears the slot **provided** that
release was the last one, which for a read event it is. The components'
explicit `proct->revstdout = NULL;` afterwards is belt and braces, and
worth keeping — the "all streams gone" test right below it is what fires
`PRTE_PROC_STATE_IOF_COMPLETE`, and relying on a macro side effect for
that is not a thing to make a reader verify.

### 5. `prte_iof_base_output()` takes ownership of your string

It stores the `char *` straight into `deliver->bo.bytes`, and the
`prte_iof_deliver_t` destructor `free()`s it — on the success path via the
delivery completion callback, and on the error path directly. So the
string must be a heap allocation you are handing off. Every caller
(`rmaps`, `ras`, `state`) passes an
`pmix_asprintf`/`strdup`/`PMIx_Argv_join`/`prte_map_print` result and does
not free it. A literal or a stack buffer would `free()` non-heap memory;
do not "fix" a caller by adding a `free`.

---

## The sink write engine (`iof_base_output.c`)

### Producer: `prte_iof_base_write_output`

Appends a **copy** of the caller's bytes to `channel->outputs` and arms
the write event if it is not already `pending`. Returns the resulting
backlog length so a caller can notice back-pressure (see the framework
guide's *Flow control* section for what that is worth today).

Three behaviors that are contract, not implementation detail:

- **It splits; callers need not.** The chunk buffer is a fixed
  `PRTE_IOF_BASE_TAGGED_OUT_MAX` array, and an input larger than that is
  broken across as many chunks as it takes. The HNP's `push_stdin` hands
  over whatever the PMIx server produced and cannot be assumed to respect
  our limit, so the split belongs here rather than at each call site.
- **Zero bytes is a sentinel, not a no-op.** It enqueues a `numbytes == 0`
  chunk meaning "flush what precedes me, then close this fd." A negative
  count degrades to the same thing, since there is nothing that could be
  copied and handing the count to `write()` would be catastrophic.
- **A `NULL` channel is a documented no-op returning 0.** Callers rely on
  this: a proc that never pulled stdin has no sink, and a sink whose
  stream has already closed has `wev == NULL`.

The size constants differ by role and that is deliberate:
`PRTE_IOF_BASE_MSG_MAX` (4096) bounds one *read*, `PRTE_IOF_BASE_TAGGED_OUT_MAX`
(8192) bounds one queued *write* chunk, and the daemon's stdin `recv`
allocates to fit the message and imposes no cap of its own.

### Consumer: `prte_iof_base_write_handler`

The generic libevent write callback, wired up by the PMIx server glue
(`src/prted/pmix/pmix_server_gen.c`) for the sinks it creates. It drains
`wev->outputs`, `write()`-ing each chunk, and handles the three realities
of a non-blocking write:

| Outcome | Action |
|---------|--------|
| `EAGAIN` / `EINTR` | prepend the chunk back, leave the event armed, re-activate |
| short write | `prte_iof_base_adjust_short_write`, prepend, re-activate |
| full write | release the chunk and continue |
| `numbytes == 0` | release the chunk, then release the sink (closing the fd) |
| any other error | release the chunk and abandon this pass |

An "always writable" (regular-file) sink yields after
`PRTE_IOF_SINK_BLOCKSIZE` (1024) bytes and re-arms, so one file-backed
stream cannot starve the other descriptors. If the backlog ever exceeds
`prte_iof_base_output_limit` the handler declares IOF hopelessly behind
and fires `PRTE_JOB_STATE_FORCED_EXIT`; the default is `INT_MAX`, so that
is off unless someone sets the MCA param.

### `prte_iof_base_adjust_short_write` — one copy, three callers

Both the count and the data have to move. Sliding the unwritten tail to
the front while leaving `numbytes` alone leaves the last `num_written`
bytes of the chunk holding a stale copy of what already went out, and the
next write emits them again — so any stream that takes short writes (any
pipe whose reader falls behind, i.e. any input larger than the pipe's
capacity) arrives duplicated, once per retry. That was
[#2220](https://github.com/openpmix/prrte/issues/2220), fixed in the HNP
copy and missed in the daemon copy, which is
[#2579](https://github.com/openpmix/prrte/issues/2579). The adjustment now
lives in exactly one place and all three handlers call it. Keep it that
way rather than re-inlining the `memmove`.

---

## Fork-time descriptors (`iof_base_setup.c`)

`odls` calls these three around the `fork()` of every application proc.
They are the only place in the framework that creates descriptors rather
than being handed them.

- **`setup_prefork(opts)`** — in the parent, before fork. Creates the
  `stdout` pipe (or a pty when `opts->usepty` and PTY support is compiled
  in), then the `stdin` pipe **only if `opts->connect_stdin`**, then the
  `stderr` pipe. `connect_stdin` is true only for the proc that receives
  stdin — normally rank 0; everyone else gets `/dev/null` in
  `setup_child`.

  **On a partial failure it closes what it already made.** The caller
  reads `opts->p_*` only after a success, so nobody else can clean them
  up, and two descriptors leaked per failed launch is what turns one
  transient `EMFILE` into every later launch on that node failing too.
  `test/unit/iof` drives this by burning the table down to three free
  slots.

- **`setup_child(opts, env)`** — in the child, after fork. Closes the
  parent ends, `dup2`s the child ends onto 0/1/2, disables echo on a pty,
  and opens `/dev/null` onto stdin when not connected.

- **`setup_parent(name, opts)`** — in the parent, after fork. Where the
  abstract vtable meets real descriptors: `prte_iof.pull(name,
  PRTE_IOF_STDIN, p_stdin[1])` when connecting stdin, then
  `prte_iof.push(name, PRTE_IOF_STDOUT, p_stdout[0])` and the same for
  `PRTE_IOF_STDERR`.

**Return PRRTE codes.** These functions return into `odls`, which tests
against `PRTE_SUCCESS` — so they must speak `PRTE_ERR_SYS_LIMITS_PIPES`
and `PRTE_ERR_PIPE_SETUP_FAILURE`, not their PMIx namesakes. They used to
return the PMIx ones, which is the boundary-crossing mistake the top-level
guide warns about: the value is meaningless when a PRRTE caller prints it.

---

## The event macros (`base.h`)

`PRTE_IOF_READ_EVENT` / `PRTE_IOF_SINK_DEFINE` both branch on
`prte_iof_base_fd_always_ready(fd)` — true for a regular file, a non-tty
character device, or a block device. Such a descriptor never signals
readiness through the event loop, so it is driven by a zero-length
**timer** instead of a real fd event. Get that branch wrong and the
stream silently never fires.

`PRTE_IOF_SINK_ACTIVATE(wev)` sets `pending` and adds the event (with the
timer for the always-writable case). `PRTE_IOF_READ_ACTIVATE(rev)` is the
read-side twin. Both post `PMIX_POST_OBJECT` so the object is coherent
when the handler picks it up on the progress thread.

Note `PRTE_IOF_SINK_DEFINE` **overwrites** the slot you point it at. It
does not release what was there. Nothing calls `pull` twice for one proc
today; if you make it possible, release the old sink first.

---

## Unused state — know it before you build on it

Four struct fields and one constant here were declared and never used —
`prte_iof_read_event_t.sink` (never assigned, yet released by the
destructor), `prte_iof_read_event_t.active` (assigned by
`PRTE_IOF_READ_ACTIVATE`, never read), `prte_iof_sink_t.xoff` and
`.exclusive`, and `PRTE_IOF_BASE_TAG_MAX`. They have been removed. If you
find one in an old commit, that is why.

What remains declared-but-dormant, and deliberately so:

| Symbol | Status |
|--------|--------|
| `prte_iof_sink_t.closed` | never assigned today, but the HNP's `stdin_write_handler` honors it — releasing the sink once the last queued byte is out. That is the graceful counterpart to the zero-byte close sentinel, and it is a hook worth keeping rather than a leftover. |
| `prte_iof_sink_t.daemon` | set by the HNP's `pull`, read by nobody |
| `PRTE_IOF_EXCLUSIVE`, `PRTE_IOF_PULL`, `PRTE_IOF_CLOSE` | tag values nothing sends or tests; kept because the tag word is a wire vocabulary, and the unit test pins them clear of the stream bits so a future user of one cannot collide |

---

## Testing

[`test/unit/iof/test_iof.c`](../../../../test/unit/iof/test_iof.c) covers
this directory more than either component, because this is where the
event-loop-independent logic lives: the class constructors/destructors,
the producer accounting and chunk splitting, `adjust_short_write`, a real
`prte_iof_base_write_handler` drain against a live pipe (payload out,
sentinel closes the fd), the `fd_always_ready` predicate, and
`setup_prefork`'s descriptor recovery under a lowered `RLIMIT_NOFILE`. See
the framework guide's *Testing* section for the full list and for what is
left to the swarm harness.
