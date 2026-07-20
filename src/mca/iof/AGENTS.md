# AGENTS.md — The `iof` Framework (I/O Forwarding)

Orientation for AI agents and human contributors working in
`src/mca/iof/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`iof` (I/O Forwarding) connects the `stdin`/`stdout`/`stderr` of every
launched application process back to the user. An application proc runs
on some node, forked by that node's `prted`; its terminal is not the
user's terminal. `iof` is the machinery that:

- **captures** each local proc's `stdout`/`stderr` (the read side), and
- **delivers** `stdin` down to the proc(s) that asked for it (the write /
  sink side),

routing every byte through the HNP so it lands on the user's terminal or
output files.

The framework is **role-split** into exactly two components that never
run together in one process:

| Component | Runs in | Priority | Role |
|-----------|---------|----------|------|
| `hnp` | the HNP / DVM master (`prte`, `prun`, `mpirun`) | 100 | The hub. Collects all output (local + remote), hands it to the PMIx server for terminal/file output, and injects stdin. |
| `prted` | every per-node daemon (`prted`) | 80 | The relay. Captures its local procs' output and forwards it to the HNP; receives stdin from the HNP and writes it to local procs. |

Because the two roles are mutually exclusive (a process is either the
master or a daemon), the "priorities" never actually compete — the query
functions gate on process type, so only one ever returns a module. See
[Component selection](#component-selection).

### The end-to-end picture

```
   application proc (on node N)
        │  stdout/stderr fd            stdin fd ▲
        ▼  (pipe/pty read end)   (pipe write end) │
   ┌──────────────────────────────────────────────────┐
   │  prted iof (node N)                               │
   │  read_handler: read(fd) ──► PMIx_server_IOF_deliver (local echo)   │
   │              └──► pack+RML ─► HNP (PRTE_RML_TAG_IOF_HNP)           │
   │  recv (PRTE_RML_TAG_IOF_PROXY): stdin ─► write_output ─► proc pipe │
   └──────────────────────────────────────────────────┘
                    │ RML                        ▲ RML
                    ▼                            │
   ┌──────────────────────────────────────────────────┐
   │  hnp iof (DVM master)                             │
   │  recv (PRTE_RML_TAG_IOF_HNP): ─► PMIx_server_IOF_deliver           │
   │  read_local_handler (my own children): ─► PMIx_server_IOF_deliver  │
   │  push_stdin: ─► RML to hosting daemon / local proc pipe            │
   └──────────────────────────────────────────────────┘
                    │
                    ▼
   PMIx server library  ──►  user's terminal / --output files / pulling tools
```

**Key architectural fact:** the iof components do **not** write
`stdout`/`stderr` to the user's terminal themselves, and they do **not**
implement `--output` tagging, per-rank files, or the copy-to-a-tool
("pull") logic. They hand captured bytes to
`PMIx_server_IOF_deliver()` (via the `prte_iof_deliver_t` carrier), and
the **PMIx server library** does the terminal/file emission and honors
the user's output directives. The iof framework's own write machinery
(sinks, `prte_iof_base_write_output`, write handlers) is used almost
entirely for the **stdin** direction — writing bytes down a local proc's
stdin pipe. Keep that split in mind; it is the single biggest thing the
historical [`README.txt`](README.txt) and the stale docstrings in
[`iof.h`](iof.h) get wrong relative to today's code.

There is **no proxy-to-proxy traffic**: a daemon never sends output to
another daemon. Everything funnels HNP-ward and the HNP fans back out.

---

## Directory layout

```
iof/
  iof.h                     # the module vtable (init/push/pull/close/complete/finalize/push_stdin)
  iof_types.h               # prte_iof_tag_t + the PRTE_IOF_* stream/flow/tool tag bitmask
  README.txt                # HISTORICAL notes (2007-era design) — largely stale, read with care
  base/
    base.h                  # framework struct, the sink/read/write/proc/deliver structs, all the macros
    iof_base_frame.c        # open/close/register; MCA param; prte_iof_base_output; all class instances
    iof_base_select.c       # pick-ONE selection (highest priority query wins)
    iof_base_output.c       # prte_iof_base_write_output + prte_iof_base_write_handler (the sink engine)
    iof_base_setup.[ch]     # pre-fork pipe/pty creation, child fd dup2, parent push/pull wiring
    static-components.h      # generated: the hnp + prted component table
  hnp/                      # HNP hub component (pri 100) — see hnp/AGENTS.md
  prted/                    # daemon relay component (pri 80) — see prted/AGENTS.md
```

Read `iof_types.h` and the struct block at the top of `base.h` first —
the tag bitmask and the five core structs are the whole vocabulary of the
framework. Then read one component end-to-end (`prted` is the simpler
read→forward story).

---

## The module contract

Every component fills in a `prte_iof_base_module_t` (defined in
[`iof.h`](iof.h) as `prte_iof_base_module_2_0_0_t`). It is a small,
fixed vtable — there is no return-code "try the next component" protocol
like `rmaps` has, because only one iof module is ever selected:

```c
struct prte_iof_base_module_2_0_0_t {
    prte_iof_base_init_fn_t       init;        /* void -> int   */
    prte_iof_base_push_fn_t       push;        /* (peer, tag, fd) -> int */
    prte_iof_base_pull_fn_t       pull;        /* (peer, tag, fd) -> int */
    prte_iof_base_close_fn_t      close;       /* (peer, tag) -> int */
    prte_iof_base_complete_fn_t   complete;    /* (jdata) -> void */
    prte_iof_base_finalize_fn_t   finalize;    /* void -> int */
    prte_iof_base_push_stdin_fn_t push_stdin;  /* (dst, data, sz) -> int (hnp only) */
};
```

| Function | Meaning | Typical caller |
|----------|---------|----------------|
| `init` | Post the framework's persistent RML receive and construct the per-process `procs` list. Called by `prte_iof_base_select` right after the module is chosen. | selection |
| `push` | **Capture output.** Tie a local read fd (the read end of a proc's `stdout` or `stderr` pipe, distinguished by `src_tag`) to a read event that forwards whatever appears. | `prte_iof_base_setup_parent` (from `odls`) |
| `pull` | **Register a stdin sink.** Tie a local write fd (the write end of a proc's `stdin` pipe) to a sink so data addressed to that proc's stdin gets written down it. Only `PRTE_IOF_STDIN` is supported. | `prte_iof_base_setup_parent` |
| `close` | Tear down the read events and/or sink for the named peer for the streams in `source_tag`; drop the proc from `procs` once all three are gone. | teardown paths |
| `complete` | Job finished: purge any lingering `prte_iof_proc_t`s belonging to `jdata->nspace`. | `state` machine on `JOB`/`PROC` completion |
| `finalize` | Cancel the RML receive and destruct the `procs` list. | framework close |
| `push_stdin` | **Inject stdin.** HNP-only: route a chunk of stdin to a target proc (or wildcard) — to the hosting daemon over RML, or to a local proc's sink. `NULL` in the `prted` module. | PMIx server glue (`pmix_server_gen.c`) |

Note the naming is a little counter-intuitive and the [`iof.h`](iof.h)
docstrings are stale: **`push` handles the OUTPUT (read) side**, **`pull`
handles the STDIN (write/sink) side**. Trust the implementations, not the
header prose.

`init`/`finalize`/`push_stdin`/`complete` may legitimately be `NULL` in a
module; the base and callers all guard with `if (NULL != prte_iof.xxx)`.

---

## Component selection

Unlike `rmaps`, `iof` is a classic **pick-one** framework.
`prte_iof_base_select()` (in
[`iof_base_select.c`](base/iof_base_select.c)) calls
`pmix_mca_base_select()`, which queries every component and keeps the
single highest-priority module. The winner is copied into the global
`prte_iof` module struct and its `init()` is called immediately.

Selection is really driven by **process role**, not by the numeric
priority:

- `hnp`'s query (in
  [`iof_hnp_component.c`](hnp/iof_hnp_component.c)) returns priority `100`
  **only if `PRTE_PROC_IS_MASTER`**, else `-1`/`PRTE_ERROR`.
- `prted`'s query (in
  [`iof_prted_component.c`](prted/iof_prted_component.c)) returns priority
  `80` **only if `PRTE_PROC_IS_DAEMON`**, else `-1`/`PRTE_ERROR`.

So in any given process exactly one of them offers a module, and that one
wins. A tool that is neither master nor daemon gets no iof module and
interacts with the HNP's iof purely through the PMIx server.

---

## The base machinery in detail

The base is where all the shared data structures, event macros, and the
sink write engine live. A component is mostly glue that allocates these
structs and arms these events.

### The five core structs (`base.h`)

| Struct | What it models |
|--------|----------------|
| `prte_iof_proc_t` | Per-proc **endpoint bundle**: the proc `name`, its `stdinev` sink, and its `revstdout` / `revstderr` read events. Each component keeps a `pmix_list_t procs` of these. |
| `prte_iof_read_event_t` | One **read side**: an fd, its libevent `ev`, the `tag` (stdout/stderr), `active`/`activated` flags, `always_readable`, a back-pointer to the owning `proc`, and an optional `sink`. Destructor closes the fd. |
| `prte_iof_sink_t` | One **output endpoint**: proc `name`, owning `daemon`, `tag`, a `prte_iof_write_event_t *wev`, and `xoff`/`exclusive`/`closed` flags. |
| `prte_iof_write_event_t` | One **write side**: an fd, its libevent `ev`, `pending` (is the write event armed?), `always_writable`, and a `pmix_list_t outputs` of queued chunks. |
| `prte_iof_write_output_t` | One **queued write chunk**: a fixed `data[PRTE_IOF_BASE_TAGGED_OUT_MAX]` (8192) buffer and `numbytes`. A `numbytes == 0` chunk is the sentinel meaning "flush then close this fd." |
| `prte_iof_deliver_t` | Carrier for handing bytes to the PMIx server: a `source` proc and a `pmix_byte_object_t bo`. Freed by the `PMIx_server_IOF_deliver` completion callback. |

All are PMIx classes (`PMIX_CLASS_INSTANCE` in
[`iof_base_frame.c`](base/iof_base_frame.c)); construct/destruct them with
`PMIX_NEW`/`PMIX_RELEASE`. The `prte_iof_proc_t` destructor releases its
sink and both read events; the read-event destructor `close()`s its fd;
the write-event destructor closes fds `> 2` (never stdout/stderr of the
daemon itself).

### Read-event macros (`base.h`)

- `PRTE_IOF_READ_EVENT(&slot, proc, fd, tag, cbfunc, activate)` —
  allocate a `prte_iof_read_event_t`, retain the proc, set up its
  libevent handler on `fd` (a timer event if the fd is "always readable,"
  i.e. a regular file / non-tty char dev / block dev; a real
  `PRTE_EV_READ` fd event otherwise), and optionally activate it.
- `PRTE_IOF_READ_ACTIVATE(rev)` / `PRTE_IOF_READ_ADDEV(rev)` — mark the
  read event active and add it to the event base.

The `always_readable` branch exists because regular files never signal
readiness through the event loop; they are driven by a zero-length timer
instead. `prte_iof_base_fd_always_ready(fd)` is the predicate.

### The sink write engine (`iof_base_output.c`)

This is the heart of the **stdin / output-to-fd** path:

- **`prte_iof_base_write_output(name, stream, data, numbytes, channel)`** —
  append a copy of `data` (into a fresh `prte_iof_write_output_t`) to the
  write event's `outputs` list, and if the write event isn't already
  armed, arm it with `PRTE_IOF_SINK_ACTIVATE`. Returns the current
  backlog size (list length). A `numbytes == 0` call still enqueues a
  sentinel so the fd is flushed and closed. A `NULL` channel is a no-op
  returning `0`. Callers compare the return against
  `PRTE_IOF_MAX_INPUT_BUFFERS` (50) to detect back-pressure.

  **Fixed copy buffer — the function splits, callers need not.** The chunk
  it copies into is a fixed `data[PRTE_IOF_BASE_TAGGED_OUT_MAX]` (8192)
  array, so an input larger than that is broken across as many chunks as it
  takes rather than overrunning the buffer (a caller like the HNP's
  `push_stdin` hands over whatever the PMIx server produced and cannot be
  assumed to respect our limit). A negative `numbytes` is treated the same
  as zero — the close sentinel — since there is nothing that could be
  copied. Note the size constants still differ by role: 4096 per read
  (`PRTE_IOF_BASE_MSG_MAX`) and 8192 per queued write chunk; the daemon's
  stdin `recv` allocates to fit the message and imposes no cap of its own.

- **`prte_iof_base_write_handler(fd, event, cbdata)`** — the generic
  libevent write callback. It drains `wev->outputs`, `write()`-ing each
  chunk to `wev->fd`. It handles the three realities of non-blocking
  writes:
  - `EAGAIN`/`EINTR` → prepend the chunk back and leave the event armed to
    retry;
  - **partial write** → `memmove` the unwritten tail to the front, fix
    `numbytes`, prepend, retry;
  - `numbytes == 0` chunk → close the stream by releasing the sink.

  If the backlog ever exceeds `prte_iof_base_output_limit` it declares IOF
  hopelessly behind and fires `PRTE_JOB_STATE_FORCED_EXIT`. To avoid
  starving other fds, an "always writable" (regular-file) sink yields
  after `PRTE_IOF_SINK_BLOCKSIZE` (1024) bytes and re-arms.

  This generic handler is wired up by the **PMIx server glue**
  (`src/prted/pmix/pmix_server_gen.c`) for sinks it creates. The two iof
  components each define their **own** near-identical
  `stdin_write_handler` (with subtly different close/`xoff` semantics) and
  pass it to `PRTE_IOF_SINK_DEFINE` — so when editing write semantics,
  check all three copies.

### Sink macros (`base.h`)

- `PRTE_IOF_SINK_DEFINE(&slot, name, fd, tag, wrthndlr)` — allocate a
  `prte_iof_sink_t`, load its name/tag, and (if `fd >= 0`) set up its
  write event's libevent handler on `fd`, choosing timer vs.
  `PRTE_EV_WRITE` by `always_writable`.
- `PRTE_IOF_SINK_ACTIVATE(wev)` — mark the write event `pending` and add
  it to the event base (with a timer for always-writable fds).

### `prte_iof_base_output()` (`iof_base_frame.c`)

A convenience used by **other** frameworks (`rmaps`, `ras`, `state`) to
emit a formatted string as though it were `stdout` from a given source
proc — e.g. `--display-map` / allocation dumps. It wraps the string in a
`prte_iof_deliver_t` and calls `PMIx_server_IOF_deliver` so the output
threads through the same PMIx output path as real proc output. It does
not touch the sink engine.

**Ownership: it frees your string.** `prte_iof_base_output` stores the
passed `char *string` directly into `deliver->bo.bytes` and the
`prte_iof_deliver_t` destructor `free()`s it (on both the success path, via
the delivery completion callback, and the error path). So `string`
**must be a heap allocation you are handing off** — every current caller
passes an `pmix_asprintf`/`strdup`/`PMIx_Argv_join`/`prte_map_print`
result and does *not* free it afterward. Passing a string literal or a
stack buffer would `free()` non-heap memory. The prototype does not spell
this out; do not "fix" a caller by adding a `free`.

### Fork-time setup helpers (`iof_base_setup.[ch]`)

Called by `odls` around the `fork()` of each app proc:

- **`prte_iof_base_setup_prefork(opts)`** — before fork: create the
  `stdout` pipe (or a pty if `usepty` and PTY support is compiled in),
  the `stderr` pipe, and — only if `opts->connect_stdin` — the `stdin`
  pipe. `connect_stdin` is set true only for the proc that receives stdin
  (normally rank 0); everyone else gets `/dev/null`.
- **`prte_iof_base_setup_child(opts, env)`** — in the child after fork:
  `dup2` the pipe ends onto fds 0/1/2, disable echo on a pty, and wire
  `stdin` to `/dev/null` when not connected.
- **`prte_iof_base_setup_parent(name, opts)`** — in the daemon/HNP after
  fork: call `prte_iof.pull(name, PRTE_IOF_STDIN, p_stdin[1])` (if
  connecting stdin) and then `prte_iof.push(name, PRTE_IOF_STDOUT, …)` and
  `push(name, PRTE_IOF_STDERR, …)`. This is where the abstract module
  vtable meets real file descriptors.

### The `prte_iof` global and RML tags

- `prte_iof` (in [`iof_base_frame.c`](base/iof_base_frame.c)) is the
  selected module; everything outside the framework calls through it
  (`prte_iof.push_stdin(...)`, `prte_iof.complete(...)`).
- `PRTE_RML_TAG_IOF_HNP` — daemons → HNP (forwarded output and XON/XOFF).
- `PRTE_RML_TAG_IOF_PROXY` — HNP → daemons (stdin, and xcast stdin to all).

---

## The tag model (`iof_types.h`)

Streams and control signals share one `prte_iof_tag_t` (a `uint16_t`)
bitmask:

| Tag | Value | Meaning |
|-----|-------|---------|
| `PRTE_IOF_STDIN` | `0x0001` | stdin stream |
| `PRTE_IOF_STDOUT` | `0x0002` | stdout stream |
| `PRTE_IOF_STDERR` | `0x0004` | stderr stream |
| `PRTE_IOF_STDMERGE` | `0x0006` | stdout+stderr combined |
| `PRTE_IOF_STDDIAG` | `0x0008` | internal diagnostic stream |
| `PRTE_IOF_STDOUTALL` | `0x000e` | stdout+stderr+diag |
| `PRTE_IOF_STDALL` | `0x000f` | every stream |
| `PRTE_IOF_EXCLUSIVE` | `0x0100` | exclusive-access flag |
| `PRTE_IOF_XON` / `PRTE_IOF_XOFF` | `0x1000` / `0x2000` | flow control |
| `PRTE_IOF_PULL` / `PRTE_IOF_CLOSE` | `0x4000` / `0x8000` | tool requests |

Because tags are bit flags, tests are always `tag & PRTE_IOF_STDOUT`, and
`close`/`push` handle multiple stream bits in one call. The old
`iof.h` comment listing `STDIN=0, STDOUT=1, …` describes a **retired**
enumeration — ignore it; `iof_types.h` is authoritative.

---

## Flow control

stdin can outrun a slow reader. The framework applies simple XON/XOFF
back-pressure keyed on `PRTE_IOF_MAX_INPUT_BUFFERS` (50 queued chunks):

- On a daemon, when `prte_iof_base_write_output` reports the stdin sink
  backlog has crossed 50 (or a write errors out),
  `prte_iof_prted_send_xonxoff(PRTE_IOF_XOFF)` tells the HNP to stop
  sending, latched by `prte_mca_iof_prted_component.xoff`. When the
  backlog drains below 50, the daemon sends `PRTE_IOF_XON`.
- On the HNP, `push_stdin` to a *local* proc returns
  `PRTE_ERR_OUT_OF_RESOURCE` when its own sink passes the same threshold,
  which propagates back to stop the read that is producing stdin.

`prte_iof_base_output_limit` (MCA param `iof_base_output_limit`, default
`INT_MAX`) is the harder ceiling: if a sink's backlog exceeds it, the
write handler concludes something is permanently wedged and forces the
job to exit.

---

## Threading

Everything here runs on the **progress thread** via libevent fd/timer
handlers — read handlers, write handlers, and the RML receives are all
event callbacks. There is no locking inside the framework because there is
no other thread touching this state. Consequences:

- All `read()`/`write()` calls are on **non-blocking** fds (the components
  `fcntl(O_NONBLOCK)` every fd before arming its event). Never issue a
  blocking I/O call from a handler.
- `PMIX_ACQUIRE_OBJECT` / `PMIX_POST_OBJECT` bracket handler entry/exit so
  the object's memory is coherent when the event fires — keep them when
  you add a handler.
- Reads are bounded to `PRTE_IOF_BASE_MSG_MAX` (4096) bytes per fire; the
  handler re-arms itself (`PRTE_IOF_READ_ACTIVATE`) to come back for more,
  rather than looping the fd dry, so one chatty proc can't starve the
  progress thread.

---

## Conventions and gotchas

- **`push` is output, `pull` is stdin.** Repeat it until it sticks. The
  docstrings in [`iof.h`](iof.h) and the [`README.txt`](README.txt) are
  historical and describe a design that no longer matches the code.
- **The PMIx server does the actual output.** `stdout`/`stderr` bytes are
  handed to `PMIx_server_IOF_deliver`; terminal writing, `--output`
  tagging, per-rank files, and tool "pull" copies are the PMIx server's
  job, not this framework's. Do not try to add tagging here.
- **`numbytes == 0` is a sentinel, not a no-op.** A zero-byte chunk /
  zero-byte read means "flush and close this stream." Preserve that
  meaning on every write path.
- **Activate both read events together.** `push` defines `revstdout` and
  `revstderr` separately but only *activates* them once both exist —
  otherwise one firing early (e.g. immediate EOF) can drive the proc to
  `IOF_COMPLETE` before the other stream is even wired. The `activated`
  flag guards double-activation.
- **IOF completion drives proc state.** When both `revstdout` and
  `revstderr` are gone (EOF or close), the read handler fires
  `PRTE_PROC_STATE_IOF_COMPLETE` — the state machine waits on this before
  fully reaping a proc. Don't null a read-event slot without going through
  that check.
- **Stdin does not come from a terminal read here.** Older trees carried
  declarations (`prte_iof_base_flush`, `prte_iof_hnp_stdin_cb`,
  `prte_iof_hnp_stdin_check`, the `stdinsig` event) from when `mpirun` read
  its own terminal stdin directly; they were never defined and have been
  removed. Stdin arrives via the PMIx server calling
  `prte_iof.push_stdin` — don't reintroduce a direct-read path.
- **The version macro is `PRTE_IOF_BASE_VERSION_2_0_0`.** Match it in any
  new component's struct.
- Standard PRRTE rules still apply: `prte_config.h` first, braces on every
  block, `NULL ==`/constant-on-left comparisons, no new compiler warnings,
  `PRTE_ERROR_LOG` for unexpected errors.

---

## Testing

Self-contained unit coverage lives in
[`test/unit/iof/test_iof.c`](../../../test/unit/iof/) and is wired into
`make check`. Because the read/forward/write paths need a live DVM, real
fds, and a running progress thread, the unit test deliberately stops at
what *is* exercisable without them:

- the tag-bitmask invariants (`STDMERGE`/`STDOUTALL`/`STDALL` equal the OR
  of their parts; control flags don't collide with stream bits);
- the module vtable contract (HNP wires all seven slots; `prted` leaves
  `push_stdin` `NULL`);
- the component name strings (`"hnp"`, `"prted"`);
- constructor defaults / destructor safety for the five core classes;
- the **producer side** of the sink engine —
  `prte_iof_base_write_output`'s backlog accounting, byte copy, zero-byte
  sentinel, and `NULL`-channel no-op — driven with the write event
  pre-marked `pending` so no event base is needed;
- the chunk-splitting of an oversized write (every chunk within
  `PRTE_IOF_BASE_TAGGED_OUT_MAX`, the pieces reassembling to the original
  bytes) and the negative-count degradation to the close sentinel;
- the `prte_iof_base_fd_always_ready` predicate (pipe vs. regular file vs.
  `/dev/null`).

The end-to-end capture/relay/inject behavior is covered by the integration
harness (`prte --daemonize` → `prun` → `pterm`), not by `make check`.

## Debugging

```sh
prte --prtemca iof_base_verbose 5 ...   # trace read/forward/write decisions
prun --output tag ...                    # prefix each line with its source rank (PMIx server)
prun --output timestamp ...              # timestamp each line
prun --output-filename DIR ...           # per-rank files instead of the terminal
```

`iof_base_verbose` ≥1 already narrates every fd read, byte count, sink
activation, and forward; ≥20 traces fd closes and sink teardown. Because
the split between "captured by iof" and "emitted by the PMIx server" is
where output bugs usually hide, correlate iof verbosity with the fact that
the actual terminal write happens downstream in the PMIx server.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`hnp/AGENTS.md`](hnp/AGENTS.md) — the HNP hub: collects all output,
  emits it, injects stdin. Read this first.
- [`prted/AGENTS.md`](prted/AGENTS.md) — the per-daemon relay: captures
  local output and forwards it; delivers stdin locally.
