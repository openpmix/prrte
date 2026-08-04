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
   │            ...and relayed output ─► PMIx_server_IOF_deliver        │
   │  push_stdin (from an attached tool): ─► RML relay to the HNP       │
   └──────────────────────────────────────────────────┘
                    │ RML                        ▲ RML
                    ▼                            │
   ┌──────────────────────────────────────────────────┐
   │  hnp iof (DVM master)                             │
   │  recv (PRTE_RML_TAG_IOF_HNP): ─► PMIx_server_IOF_deliver           │
   │  read_local_handler (my own children): ─► PMIx_server_IOF_deliver  │
   │  relay_to_tool: ─► RML to the daemon hosting the launching tool    │
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
a since-removed `README.txt` and the docstrings in [`iof.h`](iof.h) used
to get wrong relative to today's code. Both have now been corrected — the
README was a 2007 e-mail thread describing `prte_iof_base_endpoint_t` and
`prte_iof_svc_sub_t`, structures this tree has not had for many years, and
it was the origin of the "STDIN=0, STDOUT=1" enumeration every guide here
had to warn readers off; it has been removed, and `iof.h`'s prose now
describes the code that is actually there.

There is **no proxy-to-proxy traffic**: a daemon never sends output to
another daemon. Everything funnels HNP-ward and the HNP fans back out —
stdin to the daemon hosting the target, and output to the daemon hosting a
tool that asked for it. See [A tool is not always on the
master](#a-tool-is-not-always-on-the-master).

---

## Directory layout

```
iof/
  iof.h                     # the module vtable (init/push/pull/close/complete/finalize/push_stdin)
  iof_types.h               # prte_iof_tag_t + the PRTE_IOF_* stream/flow/tool tag bitmask
  base/                     # the shared machinery — see base/AGENTS.md
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
    prte_iof_base_push_stdin_fn_t push_stdin;  /* (dst, data, sz) -> int */
};
```

| Function | Meaning | Typical caller |
|----------|---------|----------------|
| `init` | Post the framework's persistent RML receive and construct the per-process `procs` list. Called by `prte_iof_base_select` right after the module is chosen. | selection |
| `push` | **Capture output.** Tie a local read fd (the read end of a proc's `stdout` or `stderr` pipe, distinguished by `src_tag`) to a read event that forwards whatever appears. | `prte_iof_base_setup_parent` (from `odls`) |
| `pull` | **Register a stdin sink.** Tie a local write fd (the write end of a proc's `stdin` pipe) to a sink so data addressed to that proc's stdin gets written down it. Only `PRTE_IOF_STDIN` is supported. | `prte_iof_base_setup_parent` |
| `close` | Tear down the read events and/or sink for the named peer for the streams in `source_tag`; drop the proc from `procs` once all three are gone. | teardown paths |
| `complete` | Job finished: purge any lingering `prte_iof_proc_t`s belonging to `jdata->nspace` — releasing each one's stream slots **before** the proc, because the proc and its read events reference each other (see [`base/AGENTS.md`](base/AGENTS.md)). | `state` machine on `JOB`/`PROC` completion |
| `finalize` | Cancel the RML receive and destruct the `procs` list. | framework close |
| `push_stdin` | **Inject stdin.** Deliver a chunk of stdin to a target proc (or wildcard). In the `hnp` module that means routing it — to the hosting daemon over RML, or to a local proc's sink. In the `prted` module it means relaying to the HNP, which is the only process that can do the routing. Implemented in **both**; a tool can attach to any daemon. | PMIx server glue (`pmix_server_gen.c`) |

Note the naming is a little counter-intuitive and the [`iof.h`](iof.h)
docstrings are stale: **`push` handles the OUTPUT (read) side**, **`pull`
handles the STDIN (write/sink) side**. Trust the implementations, not the
header prose.

`init`/`finalize`/`push_stdin`/`complete` may legitimately be `NULL` in a
module; the base and callers all guard with `if (NULL != prte_iof.xxx)`.
Both shipped modules do fill all seven — `push_stdin` was `NULL` in `prted`
until a tool attaching to a non-master daemon turned that into a segfault
([#2568](https://github.com/openpmix/prrte/issues/2568)) — but the guards
are the contract, so keep them.

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
  - **partial write** → re-base the chunk with
    `prte_iof_base_adjust_short_write` (slide the unwritten tail to the
    front **and** drop `numbytes` by what went out), prepend, retry;
  - `numbytes == 0` chunk → release the sentinel chunk, then close the
    stream by releasing the sink.

  **A chunk the handler is looking at is off the list.** Every handler
  starts with `pmix_list_remove_first`, so from that moment the chunk
  belongs to the handler and nothing else will free it — in particular,
  releasing the sink/write event frees only what is *still queued*. All
  three handlers used to return on the zero-byte sentinel without
  releasing it, leaking one `PRTE_IOF_BASE_TAGGED_OUT_MAX`-sized chunk per
  closed stdin stream, which on a persistent DVM is per proc per job
  forever. Either release the chunk or put it back; there is no third
  option.

  If the backlog ever exceeds `prte_iof_base_output_limit` it declares IOF
  hopelessly behind and fires `PRTE_JOB_STATE_FORCED_EXIT`. To avoid
  starving other fds, an "always writable" (regular-file) sink yields
  after `PRTE_IOF_SINK_BLOCKSIZE` (1024) bytes and re-arms.

  This generic handler is wired up by the **PMIx server glue**
  (`src/prted/pmix/pmix_server_gen.c`) for sinks it creates. The two iof
  components each define their **own** near-identical
  `stdin_write_handler` (with subtly different close/`xoff` semantics) and
  pass it to `PRTE_IOF_SINK_DEFINE` — so when editing write semantics,
  check all three copies. That triplication has already cost once: the
  short-write adjustment was fixed in the HNP copy (2025,
  [#2220](https://github.com/openpmix/prrte/issues/2220)) and missed in the
  daemon copy, so piping a large file into a remote rank still duplicated
  it ([#2579](https://github.com/openpmix/prrte/issues/2579)). The
  adjustment itself now lives in **one** place,
  `prte_iof_base_adjust_short_write`, and all three handlers call it — keep
  it that way rather than re-inlining the `memmove`.

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
  then — only if `opts->connect_stdin` — the `stdin` pipe, then the
  `stderr` pipe. `connect_stdin` is set true only for the proc that
  receives stdin (normally rank 0); everyone else gets `/dev/null`.
  On a failure partway through it **closes the descriptors it already
  created** and returns `PRTE_ERR_SYS_LIMITS_PIPES`: the caller only reads
  those fields on success, so nobody else could close them, and leaking
  two per failed launch is what turns one transient `EMFILE` in a daemon
  into every later launch on that node failing too.
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
- `PRTE_RML_TAG_IOF_HNP` — daemons → HNP (forwarded output, XON/XOFF, and
  stdin a daemon is relaying on behalf of a tool attached to it; the leading
  stream tag is what tells the three apart, and it is the **only** thing
  that can — an XON/XOFF message is nothing *but* that tag, so it has to be
  screened before the receiver reaches for a proc).
- `PRTE_RML_TAG_IOF_PROXY` — HNP → daemons (stdin, xcast stdin to all, and
  output relayed to the daemon hosting a launching tool; again the leading
  stream tag distinguishes them).

---

## A tool is not always on the master

A tool attaches to whichever PMIx server it found, and for a `prun` started
on a compute node of a persistent DVM with no `--dvm-uri` that is the
**local daemon**, not the HNP. PMIx registers the tool's IOF request with
that daemon's server (`pmix_server_process_iof` on the spawn), and that
server is the only one that can deliver to it. Both directions therefore
need a relay through the master, which is the only process that can route:

| Direction | Path |
|-----------|------|
| tool's stdin → the job | tool → its daemon's `push_stdin` → RML (`IOF_HNP`, stream `STDIN`) → HNP's `push_stdin` → hosting daemon → proc's stdin pipe |
| the job's output → tool | hosting daemon → RML (`IOF_HNP`) → HNP → `relay_to_tool` → RML (`IOF_PROXY`, stream `STDOUT`/`STDERR`) → tool's daemon → `PMIx_server_IOF_deliver` → tool |

Two things make the output half less obvious than it looks:

- **Part of it already works, which is what hides the rest.** A daemon
  hands its own procs' output to its own PMIx server as well as forwarding
  it, so a tool always sees the ranks that happen to land on its own node.
  Only the other ranks go missing, so the symptom is partial output, not
  none.
- **The relayed copy must not be emitted where it lands.** Every daemon
  registers the job's namespace with the same output directives, so a
  daemon handed another node's output would write it out again — and with
  `--output file=` that is two daemons writing one file. The delivery
  therefore carries `PMIX_IOF_LOCAL_OUTPUT=false`, which tells the PMIx
  server "deliver this to your registered requestors; it is not yours to
  write." That needs `PMIX_CAP_IOF_DELIVER_LOCAL`; without it the HNP does
  not relay at all (`PRTE_PMIX_IOF_DELIVER_LOCAL`, set by
  [`config/prte_setup_pmix.m4`](../../../config/prte_setup_pmix.m4)) rather
  than relay unsafely.

Who the tool's daemon is comes from `jdata->originator`. On the master that
field is the **daemon that relayed the spawn** — `plm_base_receive`
overwrites the requesting tool's own procID with the sender — so an
originator in the daemon namespace names the daemon to relay to, and an
originator in any other namespace is a tool that spawned through the master
and is already a client of its server. Deduplication is by the same handle:
the daemon that forwarded a chunk has already delivered it locally, so the
master skips the relay when that daemon is the tool's.

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
`close`/`push` handle multiple stream bits in one call. `iof_types.h` is
authoritative; a `STDIN=0, STDOUT=1, …` enumeration you may find in an old
tree or an old commit is **retired**.

---

## Flow control

stdin can outrun a slow reader, so the framework applies XON/XOFF
back-pressure keyed on `PRTE_IOF_MAX_INPUT_BUFFERS` (50 queued chunks).
**It reaches all the way back to the producer, and it did not always** —
until PMIx grew `PMIx_server_IOF_flow_control` the whole mechanism was a
signal the HNP logged and discarded. If you are reading an older tree, or
an older copy of this guide, that is what it is describing.

The producer is never ours. stdin originates in a PMIx server's read of
its own stdin, or in a tool's `PMIx_IOF_push`, so the only way to slow it
is to ask PMIx to stop it at the source. That is what
`PMIx_server_IOF_flow_control` does: it leaves the read un-armed, so the
bytes stay in the producer's own input stream and the OS applies the
back-pressure. **Nothing is buffered on behalf of a suspended stream and
nothing is dropped** — an XOFF is not permission to lose data.

The two halves, which are separate mechanisms that happen to share a
vocabulary:

- **A daemon** whose stdin sink crosses 50 (or whose write errors out)
  calls `prte_iof_prted_send_xonxoff(PRTE_IOF_XOFF)`, latched by
  `prte_mca_iof_prted_component.xoff`, and sends `PRTE_IOF_XON` when the
  backlog drains. `prte_iof_hnp_recv` screens the
  `PRTE_IOF_XON | PRTE_IOF_XOFF` mask ahead of everything else and turns
  it into a wildcard `PMIx_server_IOF_flow_control` call. Wildcard because
  the message says only *that* this daemon is behind, never which producer
  filled it — so every process feeding us stdin is suspended, which is the
  conservative reading.
- **The HNP's own local procs** never involve the RML at all.
  `push_stdin` returns `PRTE_ERR_OUT_OF_RESOURCE` when a local sink passes
  the same threshold, latched by `prte_mca_iof_hnp_component.xoff`, and
  the glue in
  [`src/prted/pmix/pmix_server_gen.c`](../../prted/pmix/pmix_server_gen.c)
  turns that into `PMIX_ERR_IOF_XOFF` on the `push_stdin` completion —
  which PMIx reads as "I have the data, suspend the stream". The matching
  release is `release_flow_control()` in
  [`hnp/iof_hnp.c`](hnp/iof_hnp.c).

**Every XOFF must be paired with an XON, and that is on us.** PMIx has no
status meaning "resume": a suspension persists until somebody calls the
API with `xoff` false. So a release has to run on *every* path a
backed-up sink can leave that state by — not just the one where it
drains, but the ones where it is torn down. `release_flow_control()` is
called from the `check:` and `finish:` arms of `stdin_write_handler`
**and** from `hnp_close`/`hnp_complete`, which release a stdin sink
directly and never reach the write handler at all — which is exactly the
case that matters, since a sink is backed up precisely when its proc
stopped reading, and that is the proc a teardown is likely to be
retiring. It is a no-op when no XOFF is outstanding, so it is safe to
call from anywhere, which is what lets it be called from everywhere it
must be.

**The blast radius is the job, not the DVM** — know why, because it is
not obvious and it is what makes the failure tolerable. PMIx keeps no
sticky suspension state: `pmix_iof_flow_control` walks the peers that
exist *at the moment of the call* and pushes the request to them, and
the only durable state is the `xoff` flag on the read event inside the
producer itself. The producer is `prun`, which is per-job, so a
suspension it is still carrying dies when it does. A `prun` that
connects afterwards has never been told anything and starts reading
normally. So the worst an unreleased XOFF can do is hang **that** job;
the next one is unaffected, and the first sink to drain clears the stale
latch. Do not read that as license to skip a release — a hung job is
still a bug, and the reasoning above is a property of PMIx's current
design rather than a guarantee it owes us. The dockerswarm suite counts
the two and fails on a mismatch rather than merely checking that flow
control happened.

The oscillation is expected. Several procs take stdin at different rates,
so one draining proc can turn the producers back on while another is
still behind; the one still behind asserts XOFF again on its next write.
That is the intended failure mode — the alternative to oscillating is
stalling, and nothing is dropped either way. The prted module's `CHECK`
block carries the same caveat in its own comment.

`prte_iof_base_output_limit` (MCA param `iof_base_output_limit`, default
`INT_MAX`) is still the harder ceiling underneath all of this: if a
sink's backlog exceeds it, the write handler concludes something is
permanently wedged and fires `PRTE_JOB_STATE_FORCED_EXIT`. With flow
control working, reaching it means the producer ignored the suspension,
not merely that the reader is slow.

**Against a PMIx that predates the capability** (`PRTE_PMIX_IOF_FLOW_CONTROL`
is 0, from `PRTE_CHECK_PMIX_CAP([IOF_FLOW_CONTROL])` in
`config/prte_setup_pmix.m4`) every one of these paths compiles away and
the old behavior returns: the message is consumed quietly, the sink
queues, and `iof_base_output_limit` is the only bound. What is **not**
conditional is that the HNP screen the tag first — a flow-control message
carries no proc and no payload, so falling through to the output unpack
reports the daemon's XOFF to the user as a corrupted message, which is
what used to happen every time a process read its stdin slowly.

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
  names are the historical ones and they read backwards; [`iof.h`](iof.h)
  now says so explicitly at both prototypes.
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
- **The proc and its read events reference each other.** Releasing the
  proc alone frees neither; the streams have to go first, and only while a
  reference to the proc is still held. Every teardown path in this
  framework obeys that, and it is the single easiest thing to get wrong
  here — see [`base/AGENTS.md`](base/AGENTS.md), *Ownership*.
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
- the module vtable contract — **both** components wire all seven slots,
  including `push_stdin` (it was `NULL` in `prted` until #2568);
- the component name strings (`"hnp"`, `"prted"`), asked of the framework
  rather than of a module symbol, so a `--enable-mca-dso` build still runs
  the case;
- constructor defaults / destructor safety for the five core classes;
- the **producer side** of the sink engine —
  `prte_iof_base_write_output`'s backlog accounting, byte copy, zero-byte
  sentinel, and `NULL`-channel no-op — driven with the write event
  pre-marked `pending` so no event base is needed;
- the chunk-splitting of an oversized write (every chunk within
  `PRTE_IOF_BASE_TAGGED_OUT_MAX`, the pieces reassembling to the original
  bytes) and the negative-count degradation to the close sentinel;
- the **consumer side** for real: `prte_iof_base_write_handler` driven
  against a live pipe, asserting that the payload arrives intact and that
  the zero-byte sentinel behind it closes the fd (the reader sees EOF).
  This is the only case that runs a write handler rather than a piece of
  one, and it is what holds the sentinel branch — where all three handlers
  leaked their chunk — still;
- `prte_iof_base_adjust_short_write` in isolation, driven by draining a
  chunk through a run of short writes and comparing the bytes that came out
  with the bytes that went in (the #2579 failure mode is duplication, so
  the byte count is the assertion that matters);
- the proc/read-event reference cycle: dropping the list's reference frees
  nothing while a stream is open, and the stream-first teardown order is
  what makes the proc's own release safe;
- the flow-control message's shape: the `XON`/`XOFF` tags share no bit with
  any stream tag, and a flow-control buffer really does end after the tag —
  which is why the HNP must branch on the mask before unpacking a proc;
- the `prte_iof_base_fd_always_ready` predicate (pipe vs. regular file vs.
  `/dev/null`);
- `prte_iof_base_setup_prefork`'s descriptor bookkeeping, driven by burning
  the descriptor table down to three free slots so the stdin pipe cannot be
  created: the call must fail *and* hand back the stdout pipe it already
  made. It runs last because it lowers `RLIMIT_NOFILE` for its duration.

The test opens `prte_event_base` (the sink macros assign their libevent
events to it) and `PMIx_server_init`s (`PMIx_Data_pack` refuses to run
before PMIx is up) — both are what a real daemon has when this code runs.

The end-to-end capture/relay/inject behavior is covered by the integration
harness (`prte --daemonize` → `prun` → `pterm`), not by `make check`.
Note that the *interesting* stdin behavior needs a **slow** reader as well
as a live DVM: a proc that drains its pipe as fast as the daemon fills it
never produces a short write, which is why a large-payload `cat` test
passed throughout the life of #2579. The swarm harness
([`contrib/dockerswarm`](../../../contrib/dockerswarm/)) has a `slowcat`
client for exactly this.

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

Each subdirectory has its own `AGENTS.md`:

- [`hnp/AGENTS.md`](hnp/AGENTS.md) — the HNP hub: collects all output,
  emits it, injects stdin. Read this first.
- [`prted/AGENTS.md`](prted/AGENTS.md) — the per-daemon relay: captures
  local output and forwards it; delivers stdin locally.
- [`base/AGENTS.md`](base/AGENTS.md) — the shared machinery: who owns
  what, the sink write engine up close, the fork-time descriptor rules,
  and an inventory of the fields here that are declared but dead. Read it
  before editing anything under `base/`.
