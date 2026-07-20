# AGENTS.md — `iof/hnp` (the HNP I/O hub)

Component guide for `src/mca/iof/hnp/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the
sink/read/write/proc structs, the tag model, and the base sink engine
referenced throughout.

---

## Role and selection

`hnp` is the **hub** of I/O forwarding. It runs **only in the HNP / DVM
master** — the `prte`, `prun`, or `mpirun` process that owns the user's
terminal. *All* IOF traffic converges here: output from the HNP's own
local children, and output relayed over the RML from every `prted`. There
is no proxy-to-proxy path — a daemon that wants another daemon's output
gets it as `prted → HNP → tool`.

Selection is role-gated. `prte_iof_hnp_query()` in
[`iof_hnp_component.c`](iof_hnp_component.c) returns priority **100** and
the module **only when `PRTE_PROC_IS_MASTER`**; otherwise it returns
`-1`/`PRTE_ERROR` and declines. Since a process is either master or
daemon, this module and `prted` never compete for real.

The component keeps one piece of state (in
[`iof_hnp.h`](iof_hnp.h)'s `prte_mca_iof_hnp_component_t`): a
`pmix_list_t procs` of `prte_iof_proc_t` endpoint bundles.

---

## Files

| File | Contents |
|------|----------|
| `iof_hnp_component.c` | Registration + `query` (gate on `PRTE_PROC_IS_MASTER`, priority 100). |
| `iof_hnp.c` | The module vtable: `init`, `hnp_push`, `hnp_pull`, `hnp_close`, `hnp_complete`, `finalize`, `push_stdin`, and the local `stdin_write_handler`. |
| `iof_hnp_read.c` | `prte_iof_hnp_read_local_handler` — reads the HNP's *own* children's stdout/stderr and emits them via the PMIx server. |
| `iof_hnp_receive.c` | `prte_iof_hnp_recv` — the `PRTE_RML_TAG_IOF_HNP` handler: unpacks daemon-forwarded output and emits it via the PMIx server. |
| `iof_hnp_send.c` | `prte_iof_hnp_send_data_to_endpoint` — packs stdin and sends it to a daemon (or xcasts to all) over `PRTE_RML_TAG_IOF_PROXY`. |
| `iof_hnp.h` | Component struct + prototypes for the above. |

The module vtable (`prte_iof_hnp_module`) sets `push_stdin` — the HNP is
the **only** component that implements it, because injecting stdin into
the job is inherently a master-side operation.

---

## The two output paths converge on the PMIx server

Whether output originates locally or remotely, the HNP's job is to hand
it to `PMIx_server_IOF_deliver()`, which performs the actual terminal /
`--output`-file / tool-pull emission. Both paths build a
`prte_iof_deliver_t` (source proc + `pmix_byte_object_t`) and translate
the iof stream bits into PMIx channel bits (`PMIX_FWD_STDOUT_CHANNEL`,
`PMIX_FWD_STDERR_CHANNEL`, `PMIX_FWD_STDDIAG_CHANNEL`).

### Path 1 — local children (`iof_hnp_read.c`)

`hnp_push` (called from `prte_iof_base_setup_parent` for procs the HNP
forked itself) arms a `PRTE_IOF_READ_EVENT` on the read end of the child's
stdout/stderr pipe, with `prte_iof_hnp_read_local_handler` as the callback.
On each fire the handler:

1. `read(fd, …)` up to `PRTE_IOF_BASE_MSG_MAX` (4096) bytes.
2. `numbytes < 0` with `EAGAIN`/`EINTR` → re-arm and return; other
   `numbytes <= 0` → EOF, jump to `CLEAN_RETURN`.
3. Otherwise wrap the bytes in a `prte_iof_deliver_t` and call
   `PMIx_server_IOF_deliver` — the data goes **straight out**, never over
   the RML, because these are the HNP's own children.
4. Re-arm the read event.

At `CLEAN_RETURN` (EOF/error) it releases the finished read event
(`revstdout` or `revstderr`) and, when both are gone, fires
`PRTE_PROC_STATE_IOF_COMPLETE` for the proc. It carefully `PMIX_RETAIN`s
the proc across the release to avoid a recursive free.

### Path 2 — remote daemons (`iof_hnp_receive.c`)

`prte_iof_hnp_recv` is the persistent `PRTE_RML_TAG_IOF_HNP` receive posted
by `init()`. A daemon forwards output as a packed buffer of
`{ tag (uint16), origin proc, numbytes (int32), bytes }`. The handler
unpacks those, finds-or-creates the `prte_iof_proc_t` for `origin`, maps
the tag to PMIx channel bits, and calls `PMIx_server_IOF_deliver`. Same
destination as Path 1 — the PMIx server — just sourced from a remote node.
XON/XOFF flow-control messages arrive on this same tag (tag-only buffers);
they are consumed here as part of the stdin back-pressure protocol.

---

## Stdin injection (`push_stdin`, `hnp_pull`, `iof_hnp_send.c`)

The HNP is where stdin *enters* the DVM. The PMIx server calls
`prte_iof.push_stdin(dst_name, data, sz)` (from
`src/prted/pmix/pmix_server_gen.c`) when the user's terminal (or a tool)
produces input. `push_stdin` routes it:

- **Wildcard rank** (`PMIX_RANK_WILDCARD`) → `prte_iof_hnp_send_data_to_endpoint`
  with a wildcard host, which `xcast`s the buffer to every daemon over
  `PRTE_RML_TAG_IOF_PROXY`.
- Otherwise it looks up the daemon hosting `dst_name`
  (`prte_get_proc_daemon_vpid`). If that daemon **isn't** the HNP, it
  sends the buffer to that daemon over `PRTE_RML_TAG_IOF_PROXY` (a
  zero-byte payload tells the daemon to close the proc's stdin).
- If the target proc is **local** to the HNP, it writes directly into that
  proc's stdin sink via `prte_iof_base_write_output(&name, PRTE_IOF_STDIN,
  data, sz, proct->stdinev->wev)`. Crossing `PRTE_IOF_MAX_INPUT_BUFFERS`
  (50) returns `PRTE_ERR_OUT_OF_RESOURCE` to apply back-pressure.

`hnp_pull` is how a local proc's stdin *sink* gets registered: called from
`prte_iof_base_setup_parent` with `PRTE_IOF_STDIN` and the write end of the
proc's stdin pipe, it `PRTE_IOF_SINK_DEFINE`s a `prte_iof_sink_t` on
`proct->stdinev` (handler = the local `stdin_write_handler`), tags it with
the HNP as the owning `daemon`, and activates it. Only `PRTE_IOF_STDIN` is
accepted; anything else returns `PRTE_ERR_NOT_SUPPORTED`.

`iof_hnp_send.c` also short-circuits: if the destination is a daemon in
the HNP's own job family and `prte_dvm_abort_ordered` is set, it drops the
send (but still forwards to non-daemon tools that may be watching an abort).

### `stdin_write_handler` (in `iof_hnp.c`)

The HNP's own sink write callback drains `wev->outputs` to the proc's
stdin fd with the standard non-blocking dance (EAGAIN/EINTR → prepend and
re-arm; partial write → `memmove` + prepend + re-arm; `numbytes == 0` →
close). It differs from the base `prte_iof_base_write_handler` in two
ways: it dumps pending data immediately if `prte_abnormal_term_ordered`
(the DVM is aborting), and it honors the sink's `closed` flag, releasing
the sink once the last queued byte is written.

---

## Close and completion

- `hnp_close(peer, source_tag)` releases the sink and/or read events named
  by the tag bits, and drops the `prte_iof_proc_t` from `procs` once all
  three (`stdinev`, `revstdout`, `revstderr`) are gone.
- `hnp_complete(jdata)` sweeps `procs` for any entry whose nspace matches
  the finished job and releases it — a safety net for endpoints that
  outlived their proc.
- `finalize()` destructs the `procs` list. (`init()` had posted the
  `PRTE_RML_TAG_IOF_HNP` receive and constructed the list.)

---

## Gotchas when editing

- **Everything routes through the PMIx server for output.** Don't add
  terminal-writing or tagging logic here; emit via `PMIx_server_IOF_deliver`
  and let the server honor `--output`. The `prte_iof_deliver_t` you pass is
  freed by the delivery completion callback (`lkcbfunc`) — on a failed
  submit you must `PMIX_RELEASE` it yourself, as the code does.
- **Retain-before-release on completion.** `read_local_handler`'s
  `CLEAN_RETURN` and the several places that null `revstdout`/`revstderr`
  can recursively free the proc; keep the `PMIX_RETAIN(proct)` guard.
- **Both read events must be defined before either is activated.** `hnp_push`
  only flips the `activated` flags once `revstdout && revstderr` exist,
  so an immediate EOF on one stream can't declare the proc IOF-complete
  before the other is wired.
- **The `query` gate is a single `!PRTE_PROC_IS_MASTER` test.** Keep it
  that way: `PRTE_PROC_MASTER` is its own bit in `proc_type` (it does not
  include `PRTE_PROC_DAEMON`), so this one predicate is exactly the
  "am I the HNP" question and needs no companion test.
- **Zero-byte stdin means close.** `push_stdin` deliberately forwards
  zero-length payloads so a preceding buffer is flushed and the proc's
  stdin fd is then closed. Preserve that.
- **`push_stdin` to a *local* proc hands `bo->size` straight to
  `prte_iof_base_write_output`**, whatever size the PMIx server produced.
  That is safe because `write_output` splits an oversized push across
  chunks (see the framework guide's sink-engine note) — don't "optimize"
  by copying into a fixed buffer here instead.
- **`prte_iof_hnp_recv` must not trust the wire `numbytes`.** It screens
  `<= 0` before `malloc(numbytes)` and checks the allocation; keep both
  guards if you touch the unpack sequence. It's internal RML traffic
  today, but don't widen the trust.
- **Let the `prte_iof_proc_t` destructor free the stream slots.**
  `hnp_complete` just removes the proc from `procs` and
  `PMIX_RELEASE`s it — the destructor releases `stdinev`, `revstdout`, and
  `revstderr`. Don't reintroduce hand-releases of individual slots.
