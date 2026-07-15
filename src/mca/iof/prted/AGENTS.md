# AGENTS.md — `iof/prted` (the per-daemon I/O relay)

Component guide for `src/mca/iof/prted/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the
sink/read/write/proc structs, the tag model, and the base sink engine
referenced throughout.

---

## Role and selection

`prted` is the **relay**. It runs in **every per-node daemon** (`prted`),
where the application procs actually live. Its two jobs are:

1. **Capture** each local proc's `stdout`/`stderr` and forward the bytes
   to the HNP over the RML (while also handing a copy to the local PMIx
   server), and
2. **Deliver** `stdin` that arrives from the HNP down to the local
   proc(s) that pulled it.

A daemon never talks IOF to another daemon — output goes up to the HNP,
stdin comes down from the HNP.

Selection is role-gated. `prte_iof_prted_query()` in
[`iof_prted_component.c`](iof_prted_component.c) returns priority **80**
and the module **only when `PRTE_PROC_IS_DAEMON`**; otherwise it declines
with `-1`/`PRTE_ERROR`.

Component state (in [`iof_prted.h`](iof_prted.h)'s
`prte_mca_iof_prted_component_t`): a `pmix_list_t procs` of endpoint
bundles and a single `bool xoff` latch for stdin flow control toward the
HNP.

---

## Files

| File | Contents |
|------|----------|
| `iof_prted_component.c` | Registration + `query` (gate on `PRTE_PROC_IS_DAEMON`, priority 80). |
| `iof_prted.c` | The module vtable: `init`, `prted_push`, `prted_pull`, `prted_close`, `prted_complete`, `finalize`, and the local `stdin_write_handler`. **No `push_stdin`** — that's HNP-only. |
| `iof_prted_read.c` | `prte_iof_prted_read_handler` — reads a local proc's stdout/stderr, echoes locally via the PMIx server, and forwards to the HNP. |
| `iof_prted_receive.c` | `prte_iof_prted_recv` (the `PRTE_RML_TAG_IOF_PROXY` handler for incoming stdin) + `prte_iof_prted_send_xonxoff` (flow control back to the HNP). |
| `iof_prted.h` | Component struct + prototypes. |

The vtable leaves `push_stdin` unset (`NULL`): a daemon receives stdin
passively over the RML rather than being asked to inject it.

---

## Output capture and forward (`prted_push`, `iof_prted_read.c`)

`prted_push` (called from `prte_iof_base_setup_parent` after the daemon
forks an app proc) sets the fd non-blocking, finds-or-creates the
`prte_iof_proc_t` for the proc, verifies the proc's job data exists
locally (`prte_get_job_data_object`), and arms a `PRTE_IOF_READ_EVENT` on
the read end of the stdout or stderr pipe (per `src_tag`) with
`prte_iof_prted_read_handler` as the callback. As in the HNP, both read
events are *defined* first and only *activated* (guarded by `activated`)
once `revstdout && revstderr` both exist, so an early EOF on one stream
can't prematurely trip IOF completion.

On each fire, `prte_iof_prted_read_handler`:

1. `read(rev->fd, …)` up to `PRTE_IOF_BASE_MSG_MAX` (4096) bytes.
2. `numbytes < 0` with `EAGAIN`/`EINTR` → re-arm and return; other
   `numbytes <= 0` → EOF, jump to `CLEAN_RETURN`.
3. **Local echo:** wrap the bytes in a `prte_iof_deliver_t`, map the tag
   to PMIx channel bits, and call `PMIx_server_IOF_deliver` — this lets a
   local PMIx server / tool see the output without a round-trip.
4. **Forward to HNP:** pack `{ tag (uint16), proc name, numbytes (int32),
   bytes }` into a `pmix_data_buffer_t` and
   `PRTE_RML_RELIABLE_SEND(..., PRTE_PROC_MY_HNP->rank, buf,
   PRTE_RML_TAG_IOF_HNP)`. The tag is packed first so a pure flow-control
   message can be just the tag.
5. Re-arm the read event.

At `CLEAN_RETURN` (EOF/error) it releases the finished read event and,
when both `revstdout` and `revstderr` are gone, fires
`PRTE_PROC_STATE_IOF_COMPLETE` for the proc — the same completion signal
the HNP uses, so the state machine can reap the proc.

Note the daemon **does not buffer** output waiting for HNP acks: each read
is immediately reliable-sent. The wire format is decoded by the HNP's
`prte_iof_hnp_recv`.

---

## Stdin delivery (`prted_pull`, `iof_prted_receive.c`)

`prted_pull` registers where local stdin goes. Called from
`prte_iof_base_setup_parent` with `PRTE_IOF_STDIN` and the write end of the
proc's stdin pipe, it sets the fd non-blocking, finds-or-creates the
proc's endpoint (matching by full name via
`prte_util_compare_name_fields`), and `PRTE_IOF_SINK_DEFINE`s a
`prte_iof_sink_t` on `proct->stdinev` with the local `stdin_write_handler`.
Only `PRTE_IOF_STDIN` is supported; anything else returns
`PRTE_ERR_NOT_SUPPORTED`.

`prte_iof_prted_recv` is the persistent `PRTE_RML_TAG_IOF_PROXY` receive
posted by `init()`. Incoming buffers carry `{ stream (uint16), target
proc, bytes }`:

1. Unpack the stream; if it isn't `PRTE_IOF_STDIN` it's a protocol error
   (`PRTE_ERR_COMM_FAILURE`). (Flow-control tags are handled by the HNP,
   not here.)
2. Unpack the target proc and the data.
3. Walk `procs`, matching the target by nspace and by rank
   (`PMIX_CHECK_RANK` honors wildcard, so a broadcast reaches every local
   proc that pulled stdin). For each match with a live `stdinev`, call
   `prte_iof_base_write_output(&target, stream, data, numbytes,
   proct->stdinev->wev)`.
4. If that write backs up past `PRTE_IOF_MAX_INPUT_BUFFERS` (50) and we
   haven't already, latch `xoff = true` and
   `prte_iof_prted_send_xonxoff(PRTE_IOF_XOFF)` to tell the HNP to stop
   sending stdin.

A zero-byte payload is forwarded through `write_output` too, so it flushes
preceding data and then closes the proc's stdin fd.

### `stdin_write_handler` (in `iof_prted.c`)

The daemon's sink write callback drains `wev->outputs` to the local proc's
stdin fd with the usual non-blocking handling (EAGAIN/EINTR → prepend +
re-arm; partial write → `memmove` + prepend + re-arm; `numbytes == 0` →
release the write event and null `sink->wev` to close). Its distinctive
behavior is **flow-control recovery**: on a fatal write error it sends
`PRTE_IOF_XOFF`, and at the `CHECK` label, whenever `xoff` is latched and
the backlog has fallen below `PRTE_IOF_MAX_INPUT_BUFFERS`, it clears the
latch and sends `PRTE_IOF_XON` to resume stdin from the HNP. The inline
`RHC:` comment flags the unsolved case of several procs fighting over
XON/XOFF at different consumption rates.

### `prte_iof_prted_send_xonxoff` (`iof_prted_receive.c`)

Builds a tag-only `pmix_data_buffer_t` (just the `PRTE_IOF_XON`/`XOFF`
tag) and reliable-sends it to the HNP on `PRTE_RML_TAG_IOF_HNP` — the same
tag used for forwarded output, distinguished by the leading tag value.

---

## Close, completion, teardown

- `prted_close(peer, source_tag)` releases the sink and/or read events for
  the tag bits and drops the proc from `procs` once all three streams are
  gone. For a daemon this "just" closes local fds — there is no remote
  state to unwind.
- `prted_complete(jdata)` sweeps `procs` and releases any endpoint whose
  nspace matches the finished job.
- `finalize()` `PMIX_LIST_DESTRUCT`s `procs` **and**
  `PRTE_RML_CANCEL`s the `PRTE_RML_TAG_IOF_PROXY` receive — unlike the HNP
  module, the daemon explicitly cancels its RML receive on shutdown.

---

## Gotchas when editing

- **Two consumers per read: local PMIx server + the HNP.** Every
  successful read both delivers locally (`PMIx_server_IOF_deliver`) and
  reliable-sends to the HNP. Dropping either breaks a use case (local
  tools attached to the daemon vs. the user's terminal). The
  `prte_iof_deliver_t` is freed by the delivery callback; on a failed
  submit, `PMIX_RELEASE` it as the code does.
- **Forwarded output is never acked/buffered here.** The daemon fires and
  forgets over the RML; backpressure only exists on the *stdin* side via
  XON/XOFF. Don't assume symmetric flow control.
- **Guard the `xoff` latch.** It's a single component-wide bool, so only
  toggle it through the send-XOFF (on backup) / send-XON (on drain) pair.
  Spurious toggles cause stdin stalls or floods.
- **`prte_get_job_data_object` must succeed in `prted_push`.** A proc
  whose job data isn't present locally is an error (`PRTE_ERR_NOT_FOUND`);
  don't relax that — it means the daemon was asked to forward for a proc
  it doesn't own.
- **Match on full name for stdin, nspace+rank for delivery.** `prted_pull`
  uses `prte_util_compare_name_fields(PRTE_NS_CMP_ALL, …)`; `recv` uses
  `PMIX_CHECK_NSPACE` + `PMIX_CHECK_RANK` (wildcard-aware). Keep those
  matching rules — they're what make wildcard-stdin fan-out work.
- **Zero-byte stdin means close.** Preserve the zero-length forward path
  through `write_output`.
