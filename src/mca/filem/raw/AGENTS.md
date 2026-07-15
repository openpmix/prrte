# AGENTS.md — `filem/raw` (the xcast staging component)

Component guide for `src/mca/filem/raw/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
preposition/link entry points, and where this sits in the launch flow
(`VM_READY` → preposition → `MAP`).

---

## Role and priority

`raw` is the **only** `filem` component in the tree and the one that
actually stages files. Its `query` returns priority **0**
(`filem_raw_component.c`), so it wins by default whenever it is built;
if it is absent, the framework falls back to the no-op "none" module.

"raw" describes its transport: it does **not** shell out to `scp`/`rsync`
or negotiate remote paths. It reads each source file on the HNP, chops it
into fixed-size chunks, and **`xcast`-broadcasts** the raw bytes to every
daemon over the RML. Each daemon reassembles the file into its node's
session directory, auto-extracts archives, and later symlinks the result
into each local process's session directory.

`raw` implements exactly two of the framework's vtable slots —
`preposition_files` and `link_local_files` — plus `filem_init`,
`filem_finalize`, and `fault_handler`. Every put/get/rm/wait slot is
deliberately wired to the base `prte_filem_base_none_*` no-ops
(`filem_raw_module.c`, module struct at the top).

---

## When/why selected

Selection is automatic (priority 0, single-winner). The component only
*does* anything when a job's app contexts carry preload attributes:

- `PRTE_APP_PRELOAD_BIN` — stage the executable itself (set by
  `--preload-binary`).
- `PRTE_APP_PRELOAD_FILES` — a comma-separated list of files to stage
  (set by `--preload-files`).

With neither attribute set, `raw_preposition_files` finds nothing to do,
immediately fires the completion callback with `PRTE_SUCCESS`, and the
job proceeds to mapping.

---

## Files

| File | Contents |
|------|----------|
| `filem_raw_component.c` | Registration, the single `flatten_directory_trees` MCA param, `query` (priority 0). |
| `filem_raw.h` | The four private classes, `PRTE_FILEM_RAW_CHUNK_MAX` (16384), the `flatten_trees` flag. |
| `filem_raw_module.c` | Everything: the module vtable, HNP send path, daemon receive path, symlinking, and class instances. |

### MCA parameter

`filem_raw_flatten_directory_trees` (bool, default false). When true, a
staged file's remote target is just its basename — all files land flat in
the working directory instead of recreating their directory tree.

---

## Private data structures (`filem_raw.h`)

| Class | Lives on | Role |
|-------|----------|------|
| `prte_filem_raw_outbound_t` | HNP | One preposition request. Holds the list of `xfers`, the aggregate `status`, and the caller's `cbfunc`/`cbdata`. When its `xfers` list drains, the callback fires. |
| `prte_filem_raw_xfer_t` | HNP | One file being sent. Carries the read `fd`, the libevent `ev` (the caddy field — **named `ev`** as required), `src` (local path, for dup detection), `file` (remote-relative path), `type`, `nchunk` (next chunk index), and `nrecvd` (how many daemons have acked). |
| `prte_filem_raw_incoming_t` | daemon | One file being received. Carries the write `fd`, `ev`, `file`/`top`/`fullpath`, `type`, the `outputs` list of pending write buffers, and `link_pts` (paths to symlink for each proc). |
| `prte_filem_raw_output_t` | daemon | One received chunk: `numbytes` + a `PRTE_FILEM_RAW_CHUNK_MAX` data buffer, queued on an incoming file's `outputs` list for the write handler. |

`xfer` and `incoming` both embed `ev` and use `PRTE_PMIX_THREADSHIFT` /
`prte_event_active` to drive their I/O on the progress thread — they are
long-lived caddies, released only when the transfer finishes.

---

## `raw_init` / `raw_finalize`

`raw_init` constructs the `incoming_files` list and posts a **persistent
RML recv** on `PRTE_RML_TAG_FILEM_BASE` bound to `recv_files` (this fires
on every node — HNP and daemons — since the HNP is also a target of its
own broadcast). If it is the HNP, it also constructs `outbound_files` and
`positioned_files` and posts a second persistent recv on
`PRTE_RML_TAG_FILEM_BASE_RESP` bound to `recv_ack`. `raw_finalize` drains
and destructs those lists.

Three file-scoped lists hold all state:
`outbound_files`/`positioned_files` (HNP) and `incoming_files` (every
node).

---

## HNP send path

### `raw_preposition_files(jdata, cbfunc, cbdata)`

The framework entry point on the master. Steps:

1. **Scan app contexts** for preload attributes and build a temporary
   `fsets` list of `prte_filem_base_file_set_t`:
   - `PRTE_APP_PRELOAD_BIN`: mark the file `PRTE_FILEM_TYPE_EXE`, and
     **rewrite the app** to run `./<basename>` from the session dir
     (`app->app`, `app->argv[0]`, and `PRTE_APP_SSNDIR_CWD` are all
     updated) so the staged copy is what actually executes.
   - `PRTE_APP_PRELOAD_FILES`: split on `,`; infer the `target_flag` from
     the suffix (`.tar`→TAR, `.bz`→BZIP, `.gz`→GZIP, else FILE); compute
     the `remote_target` (basename if flattening, else the path made
     relative — absolute paths have their leading `/` stripped); then
     strip any leading `./`/`../` components so nothing escapes above the
     session dir. The app's `PRTE_APP_PRELOAD_FILES` list is rewritten to
     the cleaned relative names so the daemon side can match them.
2. If nothing was collected, fire the callback and return `PRTE_SUCCESS`.
3. Create one `outbound` object, stash `cbfunc`/`cbdata`, append it to
   `outbound_files`.
4. For each file set, **de-duplicate**: skip anything whose `src` already
   appears in `positioned_files` (already sent) or in any in-flight
   `outbound->xfers` (already queued). This is why the same file
   referenced by multiple apps is broadcast only once.
5. `open()` the source `O_RDONLY`, set it `O_NONBLOCK`, build a
   `prte_filem_raw_xfer_t`, and `PRTE_PMIX_THREADSHIFT` it to
   `send_chunk`.
6. If every file turned out to be a duplicate (empty `xfers`), release
   the outbound and fire the callback immediately.

Note the return value only reports whether the *setup* succeeded; actual
completion is signalled later through the callback.

### `send_chunk(fd, argc, xfer)` — the read/broadcast pump

Runs on the progress thread, re-arming itself until EOF:

1. `read()` up to `PRTE_FILEM_RAW_CHUNK_MAX` (16 KB) bytes. On `EAGAIN`/
   `EINTR`, re-add the event and retry. On a hard error, force
   `numbytes = 0` to flush an EOF downstream.
2. If `prte_dvm_abort_ordered`, drop the xfer and stop.
3. Pack a buffer `{file(string), nchunk(int32), data(numbytes bytes)}`;
   on the **first chunk** (`nchunk == 0`) also append the `type` so the
   receiver knows how to handle it.
4. `prte_grpcomm.xcast(PRTE_RML_TAG_FILEM_BASE, &chunk)` — broadcast to
   **all daemons at once**. Increment `nchunk`.
5. If `numbytes == 0` this was the EOF chunk: close the fd and stop.
   Otherwise re-arm the read event (`prte_event_active(..., PRTE_EV_WRITE,
   1)`) to pump the next chunk.

So a file of *N* chunks produces *N* payload broadcasts plus one final
zero-byte broadcast that tells receivers to close and finalize.

### `recv_ack` + `xfer_complete` — completion accounting

Each daemon sends an ack `{file, status}` per file. `recv_ack` finds the
matching `xfer` in `outbound_files`, records any non-success status, and
bumps `xfer->nrecvd`. When `nrecvd == prte_process_info.num_daemons` the
file is fully positioned: `xfer_complete` moves the xfer from
`outbound->xfers` to `positioned_files`. When an outbound's `xfers` list
is empty, its `cbfunc` fires (this is the state machine's `files_ready`)
and the outbound is released.

---

## Daemon receive path

### `recv_files` — reassemble chunks

Fires on `PRTE_RML_TAG_FILEM_BASE` for each broadcast chunk:

1. Unpack `{file, nchunk}`; if `nchunk < 0` treat as EOF (`nbytes = 0`),
   else unpack the byte payload; on chunk 0 also unpack `type`.
2. Find or create the matching `prte_filem_raw_incoming_t` in
   `incoming_files`.
3. **On chunk 0**: compute `top` (first path component), build `fullpath`
   under `prte_process_info.top_session_dir`, create the parent
   directory, and `open()` the target for writing — `O_RDWR|O_CREAT|
   O_TRUNC`, mode `S_IRWXU` for an EXE (so it stays executable) else
   `S_IRUSR|S_IWUSR`. Then threadshift the incoming to `write_handler`.
4. Copy the payload into a fresh `prte_filem_raw_output_t`, append it to
   `incoming->outputs`, and (if not already pending) activate the write
   event.

Any failure is reported back to the HNP via `send_complete(file, err)`.

### `write_handler` — drain to disk, then finalize

Runs on the progress thread; consumes `incoming->outputs`:

- For each output with `numbytes > 0`, `write()` it to the fd. Short/`EAGAIN`
  writes push the remainder back onto the front of the list and re-arm.
- When it hits the **zero-byte** output (EOF), it closes the fd and
  finalizes by `type`:
  - `FILE`/`EXE`: register `top` as the single link point, then
    `send_complete(file, PRTE_SUCCESS)`.
  - `TAR`/`BZIP`/`GZIP`: `chdir` into the target dir, run
    `tar xf`/`tar xjf`/`tar xzf` via `system()`, `chdir` back, then call
    `link_archive` and ack.

### `link_archive` — enumerate archive contents

Runs `tar tf <fullpath>` via `popen`, reads each path, skips directories
and `.deps` trees, and appends every real file path to `inbnd->link_pts`.
Because different apps may share a directory tree but need different
files, each individual file becomes its own link point.

### `send_complete(file, status)`

Packs `{file, status}` and `PRTE_RML_SEND`s it to the HNP on
`PRTE_RML_TAG_FILEM_BASE_RESP` — the ack that `recv_ack` counts.

---

## `raw_link_local_files(jdata, app)` — the daemon-side link phase

Called by `odls` at fork time, **synchronously**, once per app context:

1. Gather the app's wanted files: the `PRTE_APP_PRELOAD_FILES` list plus,
   if `PRTE_APP_PRELOAD_BIN`, the executable basename.
2. For every local child in this job/app that is not yet alive, compute
   its per-proc session dir (`<jdata->session_dir>/<rank>`).
3. For each wanted file, find the matching `incoming` entry and, for each
   of its `link_pts`, call `create_link` to `symlink()` the file from the
   job session dir into the proc's session dir (creating intermediate
   dirs, tolerating an already-existing link).

This is what makes a staged file appear at the relative path the app
expects, in each rank's own directory.

---

## Fault handling

`raw_fault_handler` is intentionally minimal (marked TODO for real
resilience): if a daemon fails while `incoming_files` or `outbound_files`
is non-empty — i.e. a transfer is in flight — it activates
`PRTE_JOB_STATE_COMM_FAILED`. It relies on the fact that `xcast` is
already resilient for the not-in-flight case.

---

## Things to watch when editing

- **`ev` must stay named `ev`** in `xfer` and `incoming` — libevent/the
  threadshift macros require it. These objects are caddies that outlive
  the function that created them; never stack-allocate them.
- **Always ack, always callback.** Every receive error path must
  `send_complete` so the HNP's ack count can complete, and the HNP's
  outbound callback must fire on every path — a dropped ack or missed
  callback hangs the job at `VM_READY`. This is the classic `raw` bug.
- **De-duplication depends on `src`/`positioned_files`.** The
  already-sent checks in `raw_preposition_files` compare against both
  `positioned_files` and in-flight `outbound->xfers`; keep both, or the
  same file gets broadcast repeatedly across successive jobs in a DVM.
- **Chunk-0 carries the metadata.** File `type` (and the fd-open
  decision) rides only on the first chunk; the zero-byte final chunk
  triggers finalize. Don't reorder or coalesce these.
- **Paths are forced relative** on both the send side (strip leading
  `/`, `./`, `../`) and the write side (rooted at `top_session_dir`).
  This is a security property — staging must never let a user overwrite
  an absolute path on a remote node. Preserve it.
- **`raw` owns `PRTE_RML_TAG_FILEM_BASE`.** It posts its own recv in
  `raw_init` rather than using the base `filem_base_receive.c` service;
  don't start the base comm service alongside it or the two will collide
  on the tag.
