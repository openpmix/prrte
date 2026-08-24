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
session directory, auto-extracts archives, and later copies the result
into the working directory of each app it is about to launch.

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
| `filem_raw.h` | The four private classes, `PRTE_FILEM_RAW_CHUNK_MAX` (16384), `PRTE_FILEM_RAW_COPY_MAX` (8192), the `flatten_trees` flag. |
| `filem_raw_module.c` | Everything: the module vtable, HNP send path, daemon receive path, placement, and class instances. |
| `help-prte-filem-raw.txt` | The daemon-side collision message. |

### MCA parameter

`filem_raw_flatten_directory_trees` (bool, default false). When true, a
staged file's remote target is just its basename — all files land flat in
the working directory instead of recreating their directory tree.

---

## Private data structures (`filem_raw.h`)

| Class | Lives on | Role |
|-------|----------|------|
| `prte_filem_raw_outbound_t` | HNP | One preposition request. Holds the list of `xfers`, the aggregate `status`, and the caller's `cbfunc`/`cbdata`. When its `xfers` list drains, the callback fires. |
| `prte_filem_raw_xfer_t` | HNP | One file being sent. Carries the read `fd`, the libevent `ev` (the caddy field — **named `ev`** as required), `src` (local path, for dup detection), `file` (remote-relative path), `type`, `mode` (the source file's permission bits), `nchunk` (next chunk index), and the delivery accounting (`nexpected`, `nrecvd`, the `acked` bitmap, `horizon`) described under *completion accounting*. |
| `prte_filem_raw_incoming_t` | daemon | One file being received. Carries the write `fd`, `ev`, `file`/`fullpath`, `type`, `mode`, the `outputs` list of pending write buffers, and `link_pts` (the paths, relative to the session dir, to place). |
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
`PRTE_RML_TAG_FILEM_BASE_RESP` bound to `recv_ack`. `raw_finalize`
**cancels both recvs** and then drains and destructs those lists — in an
`--enable-mca-dso` build the framework close that follows unloads this
component, so a recv left posted is an RML callback pointing into memory
that is about to be unmapped.

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
     the suffix at the **end** of the name (`.tar.gz`/`.tgz`/`.gz`→GZIP,
     `.tar.bz2`/`.tbz`/`.bz2`→BZIP, `.tar`→TAR, else FILE — reading from
     the *first* dot instead left `run.v2.tar.gz` classified as a plain
     file that was never unpacked); compute
     the `remote_target` — the basename if flattening **or if the file was
     named by an absolute path**, else the relative path as given; then
     strip any leading `./`/`../` components so nothing escapes above the
     destination. The app's `PRTE_APP_PRELOAD_FILES` list is rewritten to
     the cleaned relative names so the daemon side can match them.

     The absolute case is not arbitrary: the file is going to be placed in
     the app's **working directory**, and reproducing an absolute source
     tree there (`/home/me/f.dat` → `./home/me/f.dat`) both puts the file
     somewhere the app never asked for and scatters directories through the
     user's own directory. A relative specification is kept as-is because
     that is exactly the name the app will open.
2. **Validate every delivered name**, then check for clashes:
   - A name that is empty, `.`, or contains a `..` component anywhere is
     refused (`preload-bad-path`, callback status `PRTE_ERR_BAD_PARAM`).
     Stripping the *leading* dot directories does not make a name safe:
     `a/../../f` has none at the front and still resolves two levels above
     the session directory, where the receive path would create it with
     `O_TRUNC` over whatever is there — on every node.
   - Two *different* files that would land under the same relative name
     (`/data/a/mesh.dat` and `/data/b/mesh.dat` both basename to
     `mesh.dat`). Only one could ever be delivered, so this fires the
     callback with `PRTE_ERR_PRELOAD_CONFLICT` instead of silently letting
     the second overwrite the first in the session dir.

   Both checks run over `fs->remote_target` **after** it has been
   normalized, which is why normalization happens once when the file set is
   built rather than later when the xfer is created: the clash check would
   otherwise miss `./foo` against `foo`.
3. If nothing was collected, fire the callback and return `PRTE_SUCCESS`.
4. Create one `outbound` object, stash `cbfunc`/`cbdata`, append it to
   `outbound_files`.
5. For each file set, **de-duplicate**: skip anything whose `src` already
   appears in `positioned_files` **and reached at least as many daemons as
   the DVM has now**, or appears in any in-flight `outbound->xfers`
   (already queued). This is why the same file referenced by multiple apps
   is broadcast only once — and why an **elastic** DVM that grew since the
   last broadcast sends it again: the daemons that joined afterwards never
   saw it, and skipping the resend would launch procs there with the file
   simply absent, with no error anywhere. Any stale `positioned_files`
   entry for a file being resent is dropped so the list cannot accumulate
   one dead record per grow.
6. `open()` the source `O_RDONLY`, set it `O_NONBLOCK`, build a
   `prte_filem_raw_xfer_t`, and `PRTE_PMIX_THREADSHIFT` it to
   `send_chunk`.
7. If every file turned out to be a duplicate (empty `xfers`), release
   the outbound and fire the callback immediately.

Note the return value only reports whether the *setup* succeeded; actual
completion is signalled later through the callback.

### `send_chunk(fd, argc, xfer)` — the read/broadcast pump

Runs on the progress thread, re-arming itself until EOF:

1. `read()` up to `PRTE_FILEM_RAW_CHUNK_MAX` (16 KB) bytes. On `EAGAIN`/
   `EINTR`, re-add the event and retry. On a hard error, record
   `PRTE_ERR_FILE_READ_FAILURE` on the xfer and force `numbytes = 0` to
   flush an EOF downstream — the receivers cannot tell a truncated file
   from a complete one and will ack it as staged, so this is the only
   place that knows the bytes never left.
2. If `prte_dvm_abort_ordered`, retire the xfer and stop.
3. Pack a buffer `{file(string), nchunk(int32), data(numbytes bytes)}`;
   on the **first chunk** (`nchunk == 0`) also append the `type` and the
   source file's `mode`, which is everything the receiver is told about
   the file. The mode matters: without it the receiver had to guess, and
   it guessed `0600` for anything not flagged as the job's executable, so
   `--preload-files helper.sh` delivered a script the ranks could not run.
4. `prte_grpcomm.xcast(PRTE_RML_TAG_FILEM_BASE, &chunk)` — broadcast to
   **all daemons at once**. Increment `nchunk`.
5. If `numbytes == 0` this was the EOF chunk: close the fd and stop.
   Otherwise re-arm the read event (`prte_event_active(..., PRTE_EV_WRITE,
   1)`) to pump the next chunk.

So a file of *N* chunks produces *N* payload broadcasts plus one final
zero-byte broadcast that tells receivers to close and finalize.

### `recv_ack` + `xfer_complete` — completion accounting

Each daemon sends an ack `{file, status}` per file. `recv_ack` finds the
matching `xfer` in `outbound_files`, records any non-success status, sets
the sender's bit in `xfer->acked` and bumps `xfer->nrecvd`, then calls
`xfer_check_complete()`. When `nrecvd >= xfer->nexpected` the file is
fully positioned: `xfer_complete` moves the xfer from `outbound->xfers` to
`positioned_files`. When an outbound's `xfers` list is empty, its `cbfunc`
fires (this is the state machine's `files_ready`) and the outbound is
released.

**Four fields, maintained together, and none of them is a live global.**

| Field | Meaning |
|-------|---------|
| `nexpected` | How many daemons still owe an ack. Seeded from `prte_process_info.num_daemons` when the transfer starts; the fault handler decrements it as daemons depart. |
| `nrecvd` | How many daemons have the file **and are still in the DVM** — a departure takes its ack back out again. |
| `acked` | A `pmix_bitmap_t` of *which* daemons those are. |
| `horizon` | The daemon job's `num_procs` when the transfer started, i.e. the vpid line above which a rank must have joined later. |

The counting used to be a bare `nrecvd >= prte_process_info.num_daemons`
re-read on every ack, and the reasons it cannot be are worth keeping:

- **A daemon that dies owing an ack never sends one, and nothing revisits
  the transfer.** Completion is driven only by arriving acks, so the
  outbound hangs and the job wedges at `VM_READY`. The fault handler is
  what settles this, which is only possible if the arithmetic does not
  depend on a global it cannot trust at that instant — the shrink path
  repairs the routing tree (and so calls the handler) *before* it
  decrements `num_daemons`.
- **A repeated ack from one daemon is not another daemon reporting in.**
  Without `acked`, two acks for one file complete a transfer that some
  daemon still has no copy of. `acked` also lets the handler tell "it died
  before answering" from "it answered and then died", which need opposite
  adjustments.
- **A daemon that joins mid-transfer owes nothing.** `xcast` hands a late
  joiner the ops it missed as already complete, so those chunks never
  reach it and no ack will ever come. `horizon` is what keeps such a rank
  from being credited (or expected) by either path — vpids are never
  reused and a grow appends them, so "rank ≥ horizon" is exactly "joined
  after this broadcast began".

`xfer_check_complete()` also refuses to retire a transfer whose `fd` is
still open. Under normal delivery that cannot trigger — a receiver acks
only at EOF, which is broadcast in the same `send_chunk` call that closes
the fd — but an *error* ack can arrive at chunk 0, and retiring on it would
leave `send_chunk` pumping bytes for a file the job has been told is in
place.

**Every exit from a transfer goes through `xfer_retire()`**, which is the
one place that unlinks the xfer from `outbound->xfers` and, when that list
empties, fires the completion callback and releases the outbound. Its
`positioned` argument says whether the transfer actually reached every
daemon: `true` parks the xfer on `positioned_files` so a later job in this
DVM does not resend the same bytes, `false` releases it, because a file
that failed on the way out was never delivered and recording it as
positioned would make the next job skip a file that is not there.
`xfer_complete()` is just `xfer_retire(..., true)`.

A bailout that only closes the fd and returns — which every pack and
`xcast` failure in `send_chunk` used to do — leaves the xfer on the
outbound's list with **no further event scheduled for it** and no ack ever
coming, so the completion callback never fires and the job wedges at
`VM_READY` forever. Releasing it in place instead (the old
`prte_dvm_abort_ordered` path) is worse: the outbound's list is left
walking freed memory.

---

## Daemon receive path

### `recv_files` — reassemble chunks

Fires on `PRTE_RML_TAG_FILEM_BASE` for each broadcast chunk:

1. Unpack `{file, nchunk}`; if `nchunk < 0` treat as EOF (`nbytes = 0`),
   else unpack the byte payload; on chunk 0 also unpack `type` and `mode`.
   A name carrying a `..` component is refused here too, even though the
   sender is supposed to have refused it already — this is the side that
   would do the `O_TRUNC`.
2. Find or create the matching `prte_filem_raw_incoming_t` in
   `incoming_files`.
3. **On chunk 0**: build `fullpath` under
   `prte_process_info.top_session_dir`, create the parent directory, and
   `open()` the target for writing — `O_RDWR|O_CREAT|O_TRUNC` with the
   sender's `mode` (plus `S_IRUSR|S_IWUSR` so we can write what we are
   about to write), followed by an `fchmod` because `open()` applies the
   mode only on create and trims it by our umask. Then threadshift the
   incoming to `write_handler`.
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
  - `FILE`/`EXE`: register the file's own relative path as the single link
    point, then `send_complete(file, PRTE_SUCCESS)`.
  - `TAR`/`BZIP`/`GZIP`: `chdir` to `top_session_dir`, run
    `tar xf`/`tar xjf`/`tar xzf` on the archive's **full path** via
    `system()`, `chdir` back, then call `link_archive` and ack.

**Link points are always relative to `top_session_dir`**, which is why the
archive is unpacked *there* rather than beside itself: an archive staged as
`sub/bundle.tar` must still deliver its contents at the paths the archive
names them by, not under `sub/`. (Unpacking beside itself and naming the
archive by its staged relative name were both broken for any archive with a
directory component — `tar` was run from a directory the relative name no
longer resolved in, and the link points then pointed at a tree that was one
level up from where the files actually were.)

### `link_archive` — enumerate archive contents

Runs `tar tf <fullpath>` via `popen`, reads each path, skips directories,
`.deps` trees, and any member that is absolute or steps up through `..`
(tar refuses to extract those, so there is nothing there to place), and
appends every remaining file path to `inbnd->link_pts`. Because different
apps may share a directory tree but need different files, each individual
file becomes its own link point. A non-zero `pclose` status means the
listing failed, which is reported rather than acked as a delivery.

**Both archive commands go through a shell** (`system` for the extract,
`popen` for the listing), so the path is passed through
`prte_filem_base_shell_quote()`. Without it a perfectly ordinary
`my data.tar.gz` reaches `tar` as two arguments and the extract fails.

### `send_complete(file, status)`

Packs `{file, status}` and sends it to the HNP on
`PRTE_RML_TAG_FILEM_BASE_RESP` — the ack that `recv_ack` counts.

It goes out with `PRTE_RML_RELIABLE_SEND`, not a plain send. The ack is the
only thing that retires a transfer on the master, and a plain send in flight
through a daemon that then dies is simply dropped — leaving the master
waiting forever for an ack this daemon believes it has already given. RELM
re-drives it over the repaired tree.

---

## `raw_link_local_files(jdata, app)` — the daemon-side placement phase

Called by `odls` at fork time, **synchronously**, once per app context,
immediately after `setup_path` has resolved and `chdir`'d to the app's
working directory:

1. Gather the app's wanted files: the `PRTE_APP_PRELOAD_FILES` list plus,
   if `PRTE_APP_PRELOAD_BIN`, the executable basename.
2. Confirm there is actually a local child of this job/app still to launch
   (`INIT`/`RESTART`, not `ALIVE`). If not, return without touching
   anything — the user's directory is not ours to write in speculatively.
3. For each wanted file, find the matching `incoming` entry and, for each
   of its `link_pts`, `place_file()` it from `top_session_dir` into
   **`app->cwd`** (creating intermediate dirs as needed).
4. Then check that every wanted file *had* an entry. It always should —
   the HNP does not advance the job past pre-positioning until every daemon
   has acked every file — but when the elastic dedup was wrong (see step 5
   of `raw_preposition_files`) this is precisely where a job arrived with a
   file that was never sent, and the only symptom was the app failing to
   open it. It now names the missing file (`preload-not-staged`) and fails
   the launch.

`app->cwd` is the whole point: it is the directory every one of this app's
procs will start in, whatever put them there — the user's cwd, `--wdir`, or
the session dir a `--preload-binary`/`--set-cwd-to-session-dir` job runs
from. Placement is therefore per-**app**, not per-proc: one copy serves
every local rank of the app, and an MPMD job whose apps have different
working directories gets each app's files in its own.

---

## Fault handling

`raw_fault_handler` has exactly one job: **settle the ack accounting for
daemons that are never going to answer.** Everything else a transfer needs
in order to survive a daemon loss is supplied by the layers underneath it.

- The chunks ride `xcast`, which re-drives its in-flight ops over the
  repaired tree, so a daemon whose parent died still receives the rest of
  the file, in order, with no help from here.
- The acks ride RELM (`send_complete` uses `PRTE_RML_RELIABLE_SEND`), so an
  ack in flight through the daemon that died is re-driven rather than lost.
- Neither can produce an ack from a daemon that is *gone*. That is the one
  gap, and it is fatal on its own: completion is driven only by arriving
  acks, so a transfer still owing one to a departed daemon holds its
  outbound forever and the job wedges at `VM_READY`.

So the handler walks `outbound_files` (and `positioned_files`, whose counts
answer the "does this file still cover the whole DVM?" question the dedup
check asks), drops each failed rank from what the transfer is owed via
`credit_departures()`, and calls `xfer_check_complete()` on anything still
in flight. It is master-only — `outbound_files` exists nowhere else — and
does nothing at all on a daemon, whose incoming transfers need no repair.

Three filters keep it from acting on things that are not daemon deaths:

- **Non-master returns immediately.**
- **`PRTE_RML_FAULT_SCOPE_LOCAL` only.** The local pass carries the news
  first and is the *only* pass the elastic shrink path drives at all
  (`shrink_campaign_complete` calls `prte_rml_repair_routing_tree` with
  `global = false`), so keying on it runs this exactly once per departure
  on both routes.
- **An empty `failed_ranks` returns immediately.** A *revival* reaches this
  same entry point (`prte_rml_revive_routing_tree` calls
  `prte_filem.fault_handler` on the way back up) with nothing marked
  failed, and so does a duplicate fault notice for a rank already recorded
  as gone.

### What this replaced, and why the old shape was worse than the TODO said

The handler used to activate `PRTE_JOB_STATE_COMM_FAILED` whenever
`incoming_files` or `outbound_files` was non-empty, on the theory that a
non-empty list meant a transfer was in flight. It does not: **nothing ever
removes an `incoming` entry on success.** It cannot — `raw_link_local_files`
reads that list at fork time to find each file's link points — so after one
preload job every daemon, the HNP included (it receives its own broadcast),
holds a non-empty `incoming_files` for the life of the DVM.

The consequence was not "staging fails" but "the DVM ends". On a prted,
`PRTE_JOB_STATE_COMM_FAILED` kills every local proc and calls `prted_abort`
(`errmgr/prted`); on the HNP it terminates the daemon job
(`errmgr/dvm`, `job_errors`). So in any DVM that had *ever* staged a file,
the next daemon loss — an event the rest of PRRTE is built to absorb — took
the whole DVM down, as did an ordinary elastic **shrink** (which repairs the
routing tree, and so calls this handler, on every survivor) and, on a
bootstrap DVM, a daemon **revival**, where nothing had failed at all.

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
  The comparison is `same_source()` — `stat` identity, not `strcmp` —
  because `./mesh.dat` and `mesh.dat` are the same file and two apps of one
  job may name it each way. Read as different files they dedup as two
  transfers *and*, worse, trip the clash check as "two different files
  under one delivered name" and refuse a launch that was fine.
- **Chunk-0 carries the metadata.** File `type` (and the fd-open
  decision) rides only on the first chunk; the zero-byte final chunk
  triggers finalize. Don't reorder or coalesce these.
- **Paths are forced relative** on both the send side (basename for an
  absolute spec, leading `./`/`../` stripped from a relative one, a `..`
  anywhere else refused) and the write side (rooted at `top_session_dir`,
  then at `app->cwd`). This is a security property — staging must never
  let a user overwrite an arbitrary path on a remote node. Preserve it,
  and express it through the base helpers
  (`prte_filem_base_strip_leading_dots` / `..._has_dotdot`) rather than
  open-coding the walk: it was open-coded twice here and the copies had
  drifted, which is how the clash check came to compare `./foo` against
  `foo` and see two different files.
- **`raw` owns `PRTE_RML_TAG_FILEM_BASE`.** It posts its own recv in
  `raw_init`. The base used to carry a second, never-posted service on that
  same tag; it has been removed rather than left as a collision waiting to
  happen. If a future component needs its own receive, give it its own tag.
- **`raw_fault_handler` runs on daemons too**, where `outbound_files` was
  never constructed (`raw_init` only builds it on the master). Guard any
  new use of the HNP-only lists with `PRTE_PROC_IS_MASTER` —
  `raw_fault_handler` returns immediately off the master, and
  `raw_preposition_files` refuses outright, for the same reason: appending
  to a list that was never constructed is not a failure that announces
  itself.
- **A non-empty `incoming_files` does not mean a transfer is in flight.**
  Nothing removes an entry on success; the list is what `link_local_files`
  reads at fork time, so it is non-empty from the first staged file until
  the daemon exits. Any new "is staging active?" test must ask the
  outbound side, on the master.
- **The fault handler is also reached by a revival and by a shrink**, not
  only by a death. Read `status->failed_ranks` and `status->scope` before
  acting on anything.
- **A PMIx status is not a PRTE status.** The receive path acks failures
  back to the HNP, where the value becomes the completion callback's
  status and then a job state; convert with `prte_pmix_convert_status()`
  before it goes on the wire.
- **The archive path blocks the progress thread.** `write_handler` runs
  `tar` through `system()` (and `link_archive` through `popen()`) while the
  daemon's event loop waits. It also `chdir`s the whole process for the
  duration. Nothing else can run meanwhile, which is exactly why nothing
  else observes the changed cwd — but a very large archive stalls the
  daemon. If that ever matters, move the extraction off the progress
  thread rather than trying to make the `chdir` safe.
- **On an error, unlink an object from its list *before* releasing it.**
  Several receive-side error paths add an `incoming` (or `xfer`) to a
  file-scoped list and, on a later failure, must both
  `pmix_list_remove_item` it and `PMIX_RELEASE` it — releasing without
  removing leaves a dangling pointer that the next chunk walks. The
  chunk-0 `recv_files` failure paths (dirpath-create and fd-open) now do
  this consistently; keep new bailouts consistent too. Likewise
  `recv_ack` must `free()` the unpacked filename on the no-match fall-off,
  and the `write_handler` EOF marker output must be `PMIX_RELEASE`d before
  finalize — both were leaks.

- **On open failure, `raw_preposition_files` records the error and lets
  the async path report it — it must NOT free the `outbound`.** Files are
  threadshifted to `send_chunk` as the loop walks them, so an already-
  queued `xfer` has a live `send_chunk` event on the progress thread and
  an open fd. If a *later* file fails to `open()`, the fix sets
  `outbound->status = PRTE_ERR_FILE_OPEN_FAILURE` and `break`s; the queued
  xfers finish and the completion callback delivers that status (or, if no
  xfer was queued, the empty-`xfers` tail fires the callback with it).
  **Do not restore the old behavior of `PMIX_RELEASE`-ing the outbound and
  returning `PRTE_ERROR` mid-loop** — its destructor drains the queued
  xfers, so the pending events fire against freed memory (use-after-free,
  reproducible with `--preload-files good.dat,/nonexistent`), the xfers'
  fds leak, and the synchronous error return *plus* the eventual callback
  both drive `PRTE_JOB_STATE_FILES_POSN_FAILED`.

## Placement: copy the data, link only the binary

`place_file()` **copies** a staged data file into `app->cwd`; only a
preloaded *binary* is symlinked (`create_link`). That split is deliberate:

- A symlink into the working directory points into this node's session
  directory, which is a path **no other node can resolve** and one that
  does not outlive the DVM. Where the working directory is on a shared
  filesystem — the ordinary HPC case — every rank on every other node
  would get a dangling link. A copy is also what "preload the files to the
  remote machine's working directory" has always claimed to happen.
- The **binary** is the exception because `--preload-binary` sets
  `PRTE_APP_SSNDIR_CWD`, so the working directory resolved by `setup_path`
  *is* this job's own session directory on this node — never shared, never
  outliving the job. A link there costs nothing, where a second copy of a
  large executable can cost a great deal. If the cwd model ever changes,
  this exception has to move with it.

The copy goes to `<dest>.prte-tmp.<pid>` and is `rename()`d into place, so
a proc never sees a half-written file — which matters precisely in the
shared-working-directory case, where several daemons may be placing the
identical file at the same moment.

### Refusing to overwrite, without refusing ourselves

`place_file` fails with `PRTE_ERR_PRELOAD_CONFLICT` (and a `show_help`
naming the file, directory and node) when something of that name is already
there. The test for "already there" is `same_contents()` — a **byte-for-byte
comparison against what we were about to write** — and it has to stay that,
because every legitimate repeat looks like a collision otherwise:

- every proc of an app shares one working directory (we place once per app,
  but several apps or several jobs in a DVM may share the directory);
- a working directory on a shared filesystem is written by one daemon and
  found by all the others, each of whose session-dir paths is different;
- the user's own copy of the file they asked to preload is very often
  sitting in the directory they launched from.

An identical file is left alone. Anything else — different contents, a
directory, a dangling symlink — is somebody else's and aborts the launch.
The error is reported twice by design: the daemon's `show_help` names the
file (and is the only place that can), while `PRTE_ERR_PRELOAD_CONFLICT`
travels back as the failing proc's exit code so
`prte_render_launch_failure()` can say it again in the **tool's** voice —
a daemon's stderr on a remote node usually reaches nobody.

The placed copy carries the **staged file's permission bits**, `fchmod`'d
after creation so the daemon's umask cannot trim a mode the user chose.
Those bits are the source file's, carried across in chunk 0 — before they
were sent, the staged copy was `0600` and a preloaded script arrived
unrunnable.

### Never hand the user's directory to `pmix_os_dirpath_create`

It **chmods a path that already exists** to the mode it was given. Calling
it unconditionally on the working directory silently reduced the user's own
cwd to `0700`. `place_file` therefore `stat`s first and only creates a
directory that is missing, with `S_IRWXU|S_IRWXG|S_IRWXO` so the user's
umask decides. The copied *file* is the other way round — it takes the
staged file's exact permission bits and is `fchmod`'d so the umask cannot
touch them, because those bits are the user's own and were carried across
the wire for exactly that reason.

## `create_link` returns SUCCESS only when it means it

`raw_link_local_files` aborts the whole launch if `create_link` returns
non-success, so two things there are load-bearing:

- **Reset `rc` after tolerating `PMIX_ERR_EXISTS`.** `pmix_os_dirpath_create`
  returns `PMIX_ERR_EXISTS` (== `-11`) whenever the proc session dir
  already exists — the normal case. The code tolerates that in the guard,
  but must then set `rc = PRTE_SUCCESS` before the `symlink()`; otherwise a
  perfectly good link returns the stale `-11` (which surfaces as
  `PRTE_ERR_IN_ERRNO`) and every preload launch fails.
- **The symlink *source* is `top_session_dir`, not `jdata->session_dir`.**
  `recv_files` writes staged bytes under `prte_process_info.top_session_dir`
  (per-node, shared across jobs), so `raw_link_local_files` passes that as
  `create_link`'s `my_dir`. Passing the job dir as the source builds a
  dangling link one level too deep.

Both were live bugs that made `--preload-files`/`--preload-binary` fail
outright.

### Verifying true delivery

A single-host run **cannot** prove staging works: the source file is
already present locally, so the app runs even if `filem` did nothing. The
real cross-daemon path is covered by the dockerswarm harness
(`contrib/dockerswarm/run-tests.sh`): `--preload-binary` compiles a marker
binary on node1 only and runs it on node2+node3; `--preload-files` writes a
data file on node1 only and has the remote ranks read it back by its bare
relative name out of their working directory; a relative `sub/pf.dat`
proves the directory component survives and is recreated; a `0755` script
proves the mode crossed with the bytes; an archive named
`"my run.v2.tar.gz"` proves both the shell quoting and the end-of-name
suffix classification; the collision case plants a *different* file of that
name on one node and requires the launch to be refused with that node's
data intact; and a `..` inside the delivered name must be refused rather
than resolved. None of them can pass unless the bytes were actually staged
and placed.

Two further cases cover the fault path, and neither can be reproduced on
one host: a daemon killed **after** a completed staging job, where the HNP,
the daemon holding the staged file, and the DVM itself must all survive and
a second staging job must still run; and a daemon killed **while** a large
file is in flight, where the job must finish rather than park at `VM_READY`
and the surviving ranks must still get their file. Run them (or an
equivalent multi-node test) after touching the send/receive/placement paths
or the fault handler — the `test/unit/filem` unit test covers the
base classes, the "none" module, and the naming rules, but not delivery.
