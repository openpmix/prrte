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
| `iof_hnp_send.c` | `prte_iof_hnp_send_data_to_endpoint` — packs a payload and sends it to a daemon (or xcasts to all) over `PRTE_RML_TAG_IOF_PROXY` — plus `prte_iof_hnp_relay_to_tool`, which uses it to send a job's output to the daemon hosting the tool that launched it. |
| `iof_hnp.h` | Component struct + prototypes for the above. |

The module vtable (`prte_iof_hnp_module`) sets `push_stdin`. The `prted`
module sets it too, but only to relay: *routing* stdin — resolving which
daemon hosts the target, or xcasting a wildcard — is inherently a
master-side operation, so a daemon asked to inject stdin sends it here.

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
unpacks those, maps the tag to PMIx channel bits, and calls
`PMIx_server_IOF_deliver`. Same destination as Path 1 — the PMIx server —
just sourced from a remote node.

**It deliberately does not record `origin` in `procs`.** That list holds
*endpoint bundles* — a stdin sink and the read events for a proc the HNP
itself forked — and a proc on another node has none of those here. The
find-or-create this path used to do produced an entry carrying a name and
three `NULL` slots that no code ever read, at the cost of one object and
one list node on the DVM master for **every remote process that ever
produced output**, held until the job completed, plus a linear walk of that
list on every stdin fragment and every close. Do not put it back to "keep
track of" a remote proc; nothing here can use the entry.

**Two message kinds on this tag are not output, and both must be screened
before the proc unpack.** The order in the handler is deliberate:

1. **Flow control.** `PRTE_IOF_XON` / `PRTE_IOF_XOFF` from a daemon whose
   stdin sink has backed up. Such a buffer holds *nothing but the tag* —
   no proc, no count, no payload — so falling through to
   `PMIx_Data_unpack(…PMIX_PROC)` fails and reports the daemon's XOFF to
   the user as a corrupted message, which is what happened every time a
   process read its stdin slowly. We recognize it by mask (the control
   bits are disjoint from every stream bit), trace it, and hand it to
   `PMIx_server_IOF_flow_control` — wildcard, because the message says
   only *that* this daemon is behind and never which producer filled it.
   See the framework guide's *Flow control* section, and note the pairing
   obligation it describes: an XOFF we never release hangs the producer.
2. **Relayed stdin.** A leading `PRTE_IOF_STDIN` is stdin a daemon is
   relaying on behalf of a tool attached to *it* rather than to us, and
   the proc that follows is the intended **recipient**, not a source. That
   branch unpacks the same trailing count and bytes and hands them to
   `push_stdin` below. It is screened ahead of the "nothing to do" test on
   a zero-length payload, because a zero-byte stdin push is the sentinel
   that closes the target's stdin. See
   [`../prted/AGENTS.md`](../prted/AGENTS.md), "Relaying a tool's stdin".

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

The wildcard branch `xcast`s and then releases the buffer — `xcast` copies
what it needs, so the buffer is the caller's either way. Its return is
reported, not discarded: nothing retries stdin, so a failed broadcast loses
that fragment outright and returning `PRTE_SUCCESS` over it would make the
loss silent.

### `stdin_write_handler` (in `iof_hnp.c`)

The HNP's own sink write callback drains `wev->outputs` to the proc's
stdin fd with the standard non-blocking dance (EAGAIN/EINTR → prepend and
re-arm; partial write → `prte_iof_base_adjust_short_write` + prepend +
re-arm; `numbytes == 0` → release the sentinel chunk and close). It differs
from the base `prte_iof_base_write_handler` in two
ways: it dumps pending data immediately if `prte_abnormal_term_ordered`
(the DVM is aborting), and it honors the sink's `closed` flag, releasing
the sink once the last queued byte is written. (Nothing currently *sets*
`closed`, so that second branch is dormant — see the framework guide's
note on the sink's unused fields.)

The chunk in hand is off the backlog list, so the sentinel branch owns it:
releasing the write event frees only what is still queued. Forgetting that
leaked one 8 KiB chunk per closed stdin stream here, in the daemon copy,
and in the base handler alike.

---

## Relaying output to a tool on another daemon (`relay_to_tool`)

`PMIx_server_IOF_deliver` reaches the tools connected to **our** server. A
tool that attached to some other daemon — a `prun` started on a compute node
of a persistent DVM — registered its IOF request there, and only that
server can deliver to it. We are the only process that sees all of the job's
output, so we send a copy back out.

`prte_iof_hnp_relay_to_tool(source, stream, data, numbytes, already_delivered)`
is called from **both** output paths, just before the local delivery: from
`prte_iof_hnp_recv` (passing the forwarding daemon's rank) and from
`prte_iof_hnp_read_local_handler` (passing our own). It sends on
`PRTE_RML_TAG_IOF_PROXY` with the stream tag leading, which is how the
daemon tells it from stdin.

Three tests decide whether a copy is owed, and each is there for a reason:

- **`jdata->originator` must be in the daemon namespace.** On the master that
  field is the daemon that relayed the spawn (`plm_base_receive` overwrites
  the tool's own procID with the sender). An originator in any other
  namespace is a tool that spawned through *us* and is a client of our own
  server — already served by the local delivery.
- **...and must not be us.** Same reason.
- **...and must not be `already_delivered`.** A daemon hands its own procs'
  output to its own PMIx server before forwarding it to us, so if that
  daemon is also the tool's, the tool already has this chunk and a relayed
  copy would duplicate it. This is why a tool sees the ranks on its own node
  correctly even with no relay at all — and why the bug looked like partial
  output rather than none.

The receiving daemon must not *emit* the copy; see
[`../prted/AGENTS.md`](../prted/AGENTS.md), "Relayed output". The whole
function is compiled out when `PRTE_PMIX_IOF_DELIVER_LOCAL` is 0, because
without that PMIx capability the relayed copy cannot be marked
non-emitting and relaying would make two daemons write one output file.

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
