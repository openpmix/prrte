# AGENTS.md — `grpcomm` (Group Communication)

Orientation for AI agents and human contributors working in
`src/grpcomm/`. This is a map, not the rulebook: the authoritative project
guidance lives in the top-level [`AGENTS.md`](../../AGENTS.md) and under
[`docs/`](../../docs/). When this file and those disagree, **the docs
win** — and please fix this file.

---

## What this code does

`grpcomm` provides the **collective communication services that span the
DVM's daemons**: scalable broadcast (`xcast`), allgather/barrier (`fence`),
and PMIx group operations (`group`). It is not intended for
point-to-point communication (the RML does that), nor as a
high-performance channel for large-scale data transfer.

grpcomm runs **on every daemon** (`prted` and the HNP). It is layered on
top of the RML and its routing tree: it opens no connections of its own —
it sends RML messages along the radix routing tree that `src/rml/`
maintains (`prte_rml_base.children`, `PRTE_PROC_MY_PARENT`,
`PRTE_PROC_MY_HNP`). It is the machinery behind almost every DVM-wide
action:

- `plm`/`state` broadcast launch messages, wireup (nidmap), and DAEMON
  commands with `prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, …)` /
  `PRTE_RML_TAG_WIREUP`. The receive side of the wireup lives here, in
  `process_wireup()` (`grpcomm_xcast.c`): after the nidmap it reads the
  **job catch-up** (`prte_util_decode_job_catchup` — the jobs already
  running in the DVM, which a daemon that just joined has never heard of;
  they used to lead the launch message instead), and then a
  **three-field record per daemon** — name, `PMIX_PROC_URI` (RML contact
  info), `PMIX_SERVER_URI` (that node's PMIx server rendezvous,
  redistributed only so any daemon can answer a tool's query; PRRTE never
  connects to it). The sender is `vm_ready()` in `state/dvm`. The loop
  skips storing what it already knows, so mind the invariant: **every
  field of a record must be unpacked before any `continue`**, or the next
  iteration reads the leftover string as a `pmix_proc_t`.
- The PMIx server shim satisfies `PMIx_Fence` /
  `PMIx_server_register_resources` via `prte_grpcomm_fence(...)` (see
  `src/prted/pmix/pmix_server_fence.c`).
- The PMIx server shim satisfies `PMIx_Group_construct/destruct/cancel`
  via `prte_grpcomm_group(...)` (see `src/prted/pmix/pmix_server_group.c`).
- The RML fault handler re-broadcasts `PRTE_RML_TAG_DAEMON_DIED` /
  `PRTE_RML_TAG_DAEMON_REVIVED` through `xcast` as part of DVM recovery.

---

## This is not an MCA framework — do not make it one again

grpcomm **was** an MCA framework, with a `base/` and a single component
called `direct`. It was collapsed into plain code for the same reasons
`src/rml` was (which had been *three* frameworks — rml, routed and oob):

- There was only ever one component. `direct` returned priority 5 and
  declared itself "always available"; the historical `bmg` component had
  long since been deleted.
- More importantly, **the choice that actually matters is not expressible
  as a component**. What varies is which algorithm a collective uses, and
  that is a per-operation, per-message-size decision: a barrier and a
  modex are both `fence` but want opposite algorithms, and the launch
  message and a shutdown command are both `xcast` on the same tag six
  orders of magnitude apart in size. A component is chosen once, for the
  whole interface, for the life of the process. Wrong axis.

So: if an algorithm ever needs to vary, it varies **inside the
operation** — not by adding a component. Do not reintroduce the
component/module abstraction unless a genuine second implementation of
the *whole* interface turns up; that abstraction is precisely what was
removed.

**Today nothing varies.** Both collectives use the tree and only the tree:
`xcast` forwards the whole payload down the routing tree, `fence` rolls up
to the controller which broadcasts the answer back. A lateral
scatter/allgather pair for each was built and then removed — see
[`docs/plans/scalable_collectives/`](../../docs/plans/scalable_collectives/)
for what it was and why it went. If you are reintroducing one, read that
first: the framing it needed (an out-of-order op hold, a partial-payload
gate, a movement id on the wire and a disagreement interlock for the
fence) went with it, and is the expensive part.

---

## Directory layout

```
grpcomm/
  grpcomm.h            # the public API - what the rest of the tree calls
  grpcomm_internal.h   # trackers, signatures, globals, cross-file prototypes
  grpcomm.c            # init/finalize/fault_handler, globals, MCA params
  grpcomm_classes.c    # PMIX_CLASS_INSTANCE for every signature/tracker/caddy
  grpcomm_xcast.c      # reliable, fault-tolerant broadcast
  grpcomm_fence.c      # allgather / barrier
  grpcomm_group.c      # PMIx group construct/destruct/cancel
```


Read `grpcomm.h` first — it is the whole external contract. Then
`grpcomm_internal.h` for the object model.

---

## The API

Plain functions, all `PRTE_EXPORT`, declared in `grpcomm.h`:

| Function | Meaning / return protocol |
|----------|---------------------------|
| `prte_grpcomm_register()` | Register MCA parameters and open the verbosity stream. Called from `prte_register_params()`, long before init. |
| `prte_grpcomm_init()` | Construct the trackers, register the persistent RML receives. Called once per daemon from the ess. Failure is **fatal** — no trackers and no receives means every later collective quietly does nothing. |
| `prte_grpcomm_finalize()` | Tear down trackers, cancel the RML receives. |
| `prte_grpcomm_fault_handler(status)` | Invoked on **every** daemon by `src/rml/routed_radix.c` when the routing tree changes. Called twice per death (LOCAL scope then GLOBAL scope); which scope a collective keys on depends on what it needs, so read *Surviving a daemon failure* before adding a third. |
| `prte_grpcomm_xcast(tag, msg)` | Broadcast to **every** daemon, delivered at `tag`. Non-destructive to `msg`. Returns `PRTE_SUCCESS` on *acceptance*, not completion. |
| `prte_grpcomm_xcast_nb(tag, msg, cbfunc, cbdata)` | As above, but `cbfunc` fires on the **master** once the whole DVM has confirmed receipt — or, if the broadcast is abandoned before it is ever emitted, as soon as that is known (`abandon_xcast`), because a caller waiting on it would otherwise wait forever. It carries no status, so it means only "stop waiting". `cbfunc`/`cbdata` are ignored on non-master daemons. `xcast` is just `xcast_nb(tag, msg, NULL, NULL)`. |
| `prte_grpcomm_fence(procs, nprocs, info, ninfo, data, ndata, cbfunc, cbdata)` | Non-blocking allgather/barrier across the daemons hosting `procs`. A barrier supplies no data. Returns `PRTE_SUCCESS` once queued. |
| `prte_grpcomm_group(op, grpid, procs, nprocs, directives, ndirs, cbfunc, cbdata)` | PMIx group construct/destruct/cancel. |

### The one indirection: `prte_grpcomm_release_bcast`

`grpcomm_internal.h` declares a single function pointer, initialised to
`prte_grpcomm_xcast`, through which fence and group emit their **release**
broadcasts. It exists for two reasons, and neither of them is "keep the
framework alive in spirit":

- The release is the part any different data movement would change: a
  rollup-to-controller collective must broadcast the answer back, while an
  allgather would leave every daemon already holding it with nothing to
  release. Nothing exercises that today, but the seam is one line and it
  is where such a change would land.
- It is the only way the unit test can observe the release a controller
  would emit without standing up an RML.

Production code sets it once, at startup, and never again.

---

## Global state

One struct, `prte_grpcomm_globals` (`grpcomm_internal.h`), defined in
`grpcomm.c`. This was two structs — the framework's "base" and the
component's own — until grpcomm stopped being a framework; they were
always one thing.

| Field | Meaning |
|-------|---------|
| `output` | Verbosity stream. `-1` until the `grpcomm_base_verbose` MCA parameter opens it. The parameter deliberately **keeps its old framework spelling**, because it is in every debugging recipe and in the guides. |
| `context_id` | The group context-id pool. A construct asking for `PMIX_GROUP_ASSIGN_CONTEXT_ID` gets this value, and it **decrements** — it counts *down* from `UINT32_MAX` so DVM-assigned ids cannot collide with ids assigned from the bottom of the range elsewhere. Spend it only through `prte_grpcomm_assign_context_id()`, which refuses a non-master caller: the group collective is no longer its only consumer (see below), and two doors onto one counter is how two callers end up with one id. |
| `xcast_ops` | In-flight broadcasts, the `pending_completions` FIFO, and the three op-id sequence counters. |
| `fence_ops` | List of `prte_grpcomm_fence_t`. A tracker is found here by its signature alone, so it must come **off** this list before its result is delivered — see *Retire before you deliver* below. |
| `group_ops` | List of `prte_grpcomm_group_t`. |
| `completed_group_ops` | Bounded memo of already-released group ops (see below). |
| `recovery_epoch` | The collective recovery epoch, shared by fence and group: one failure, one restart, one epoch. Issued by the master, absolute on the wire, adopted by highest value seen — see "The epoch" below. |

Note the verbosity variable that backs the MCA parameter is at **file
scope** in `grpcomm.c`, not a local: the MCA layer keeps the pointer it is
handed and writes through it whenever the variable is set, so a stack slot
would be a dangling write the moment registration returns.

### Retire before you deliver

**A fence tracker comes off `fence_ops` before its completion callback runs,
not after.** The fence is over the moment its result is in hand; everything
after that is delivery, and delivery is precisely when the next fence can
start.

The reason is that a fence signature is *only its participant list*. It
carries nothing to distinguish one fence over a set of procs from the next
fence over that same set. So when `fence_release()` runs the callback, the
local clients' `PMIx_Fence` completes, and any of them may call `PMIx_Fence`
again over the same participants immediately — and that lookup would find the
tracker that has just been answered and is about to be released, joining a
collective that is already finished.

Both completion paths honor this, by different means:

- `fence_release()` unlinks the tracker first, then delivers. It still holds
  the reference that keeps the object alive across the callback.
- the entry point's failure path leaves the tracker in place deliberately (a
  later release from the controller still has to find it) and instead clears
  `cbfunc`/`cbdata` so the participants cannot be completed twice.

Do not "tidy" the unlink back down next to `PMIX_RELEASE(coll)`. It reads like
teardown belonging together and it is not: one of those two is the end of the
collective and the other is just freeing memory.

---

## The collective model

- **Routing tree.** Collectives ride the radix routing tree owned by
  `src/rml/`. The HNP is the root of every operation.
- **Signatures.** A collective is identified not by a global counter but
  by a *signature* — for `fence`, the array of participating procs
  (matched **byte-for-byte** with `memcmp`, so the order PMIx hands down
  is load-bearing); for `group`, the `groupID` + operation; for `xcast`,
  an HNP-assigned globally-unique `op_id`. A signature lets
  independently-arriving pieces of the same collective find each other.
- **Trackers.** Each daemon keeps a per-collective tracker on a global
  list, counting `nexpected` vs `nreported`. When a tracker completes
  locally it rolls up to the parent; when the HNP's tracker completes it
  broadcasts the release.
- **Two-phase collective.** `fence`/`group` are an **up-tree allgather**
  followed by a **down-tree release**. `xcast` itself is the down-tree
  half, made reliable with an ACK rollup.

`create_dmns()` (in both `grpcomm_fence.c` and `grpcomm_group.c`) turns a
signature's proc set into the daemon vpids that must participate;
`prte_rml_get_num_contributors()` then tells the tracker how many child
contributions to expect.

---

## `xcast` — reliable fault-tolerant broadcast (`grpcomm_xcast.c`)

The most intricate file. `prte_grpcomm_xcast()` is just
`xcast_nb(tag, msg, NULL, NULL)`.

### Message flow

1. **Originate (`xcast_nb`).** Any daemon can originate. Builds an `op_t`,
   stashes the (possibly compressed) message and tag, records the optional
   completion callback, thread-shifts to `begin_xcast`.
2. **`begin_xcast`.** Packs the op and reliably sends it **to the HNP**.
   The initiating op is then discarded — it is not the tracked op. A
   master originator also enqueues one entry on `pending_completions`.
3. **HNP assigns the op-id.** `sig.op_id == 0` becomes
   `++op_id_inited` — globally unique and monotonic. A non-zero op-id
   arriving at the HNP is a bug (`PRTE_ERR_DUPLICATE_MSG`).
4. **Forward down the tree** to each routing-tree child, then process
   locally.
5. **Process locally (`process_msg`).** Decompresses and delivers to
   *itself* at the user tag via `PRTE_RML_POST_MESSAGE` — not a real send.
   `PRTE_RML_TAG_WIREUP` is special-cased to `process_wireup()`.
6. **ACK rollup.** A leaf `finish_op`s immediately; an interior daemon
   only once all children have ACKed. `finish_op` ACKs upward, advances
   `op_id_completed`, fires the completion callback (master only), and
   releases the op.

### One movement: the tree

`xcast` forwards the **whole payload to each routing-tree child**. That is
the only way a broadcast travels, and `forward_op()` calls
`tree_whole_forward()` directly — there is no selection, nothing on the wire
saying how the payload moved, and no per-message decision to get wrong.

Nothing else about the operation is about movement, and all of it is the hard
part: op-id assignment, the `XCAST_ACK` rollup with its `is_request` re-poll,
the `process_first` ordering set, late-joiner catch-up, the promotion replay
hold, the `pending_completions` FIFO, and the fault-handler reactions to
parent/children changes. Keep that separation in mind if a second movement is
ever reintroduced — it is data transport that would be added, not a second
copy of the reliability machinery.

The cost of the tree is `d*r*M*beta`: a daemon with `r` children puts `r` full
copies of the payload on its outbound link at every level. That is the term a
lateral movement was built to remove, and
[`docs/plans/scalable_collectives/`](../../docs/plans/scalable_collectives/)
records both the attempt and why it was withdrawn.

### Compression, and why the threshold is not a size

`xcast_nb()` compresses the payload **once, at the originator**, via
`PMIx_Data_compress`; the compressed bytes are what every hop forwards, and
only the local delivery in `process_msg()` inflates. So the DVM pays one
deflate and one inflate per daemon, the latter off the forwarding path. The
`msg_compressed` bool travels beside the byte object so the receiver knows
which it got.

Whether that is worth doing is not a property of the payload's size alone.
Five quantities decide it, and it is worth naming them before the arithmetic:

| | meaning |
|---|---|
| `M` | the payload, in bytes |
| `rho` | the fraction the compressor leaves — `rho = 0.5` means it halved it |
| `B` | bandwidth of **one** link, in bytes/sec |
| `d` | depth of the routing tree (levels below the HNP) |
| `k` | radix of the tree — how many children a daemon forwards to (64 by default) |
| `R` | the compressor's throughput, in bytes/sec |

Shrinking the payload saves `(1 - rho) * M / B` on **every link the broadcast
crosses**, and the critical path holds `d * k` of them, because each of the `d`
levels serialises `k` full copies onto one outbound link. So the saving carries
a `d * k` multiplier that the compressor knows nothing about. Deflate is linear
in `M` above a few KB, so writing its cost as `M / R` makes `M` cancel from
both sides:

```
compress iff   R  >  B / (d * k * (1 - rho))
```

A comparison of two rates, not a size. **This is why there is no
PRRTE-side size threshold and should not be one.** A size limit buys only
protection from the compressor's fixed start-up cost and from the poor ratios
of tiny inputs — and that is the compressor's own business: every `pcompress`
component already declines an input below its configured `pcompress_base_limit`
and declines any result that is not actually smaller. There is nothing for this
layer to add. Hand the payload over and let the component judge it.

**Scale is what decides it, so do not calibrate this on a small DVM.** `d * k`
is 39 on a 40-node DVM but 128 at 1000 nodes and 192 at 10000 (radix 64), and
the payloads grow with the process count on top of that. The answer at 40 nodes
is "barely, maybe not"; the answer at 10000 is "overwhelmingly yes".

The launch message measures **8.1 bytes per process** (`grpcomm_base_verbose 1`
against `prterun -n N`, slope taken at N = 800 to 1600), so 1.28M processes is a
~10 MB broadcast. A full-data fence release carries the aggregated modex, which
is the application's size rather than PRRTE's — at a modest 200 B/proc that is
25 MB at 1000 nodes and 256 MB at 10000.

At 10000 nodes and 128 processes per node (`d = 3`, `k = 64`, so `d * k = 192`),
broadcasting that 256 MB release over 10 Gb/s (`B` = 1.25 GB/s), with zstd's
measured `R` = 434 MB/s and `rho` = 0.63:

| | deflate | wire raw | wire compressed | net |
|---|---|---|---|---|
| zstd | 0.6 s | 39.3 s | 24.8 s | **+13.9 s** |
| zlib (`R` = 103 MB/s, `rho` = 0.65) | 2.5 s | 39.3 s | 25.5 s | **+11.3 s** |

So compression is not marginal here — it is the difference between a
40-second broadcast and a 25-second one. Two things that decision rests on:

- **`B` is the assumption to check.** At 100 Gb/s (`B` = 12.5 GB/s) the same
  broadcast needs `d * k > 78` with zstd to stay ahead — which 1000- and
  10000-node DVMs still clear, but a small one does not. PRRTE's OOB is TCP and
  on most clusters rides the management network rather than the compute fabric,
  which is the case worth designing for.
- **The deflate blocks the progress thread.** `xcast_nb()` compresses *before*
  it thread-shifts to `begin_xcast`, and every caller (`fence_recv`, the plm
  launch path, the state machine) is already on the progress thread. At 256 MB
  that is a sub-second stall with zstd but several seconds with zlib at a high
  level, during which the HNP services no RML message, no PMIx connection and no
  tool request. The wire time it buys back is larger, so the trade is right, but
  the cost lands as one stall rather than as evenly spread bandwidth.

### The fence path compresses twice, and that is correct — do not "fix" it

The collect blob is compressed **before** xcast ever sees it:
`pmix_server_fence.c` deflates each daemon's bucket, PRRTE concatenates those
opaque blocks up the tree, and the HNP deflates the concatenation again in
`xcast_nb()`. That looks redundant and the obvious cleanup — drop the
per-bucket pass and compress once over the whole aggregate, where all the
cross-rank redundancy is — has been measured and it is a **regression on both
axes**:

| 1000 daemons x 128 ppn, 200 B/proc | broadcast | rollup |
|---|---|---|
| per-bucket + xcast (today) | 15.40 MB | 15.51 MB |
| xcast only | 15.62 MB (**+1.4%**) | 25.60 MB (**+65%**) |

The reason is mechanical: **deflate's sliding window is 32 KB**. A bucket of
128 ranks at 200 B/proc is 25.6 KB — already inside one window — so the
per-bucket pass finds essentially everything zlib is capable of finding, and
compressing the aggregate in one shot gives it no matches it could not already
see. Chunking only costs where a bucket is far *below* the window (at 8 ppn the
single pass wins 1.5%, still against a 46% larger rollup).

And there is no long-range redundancy waiting for a bigger window either. On
the same corpus, `xz -9` with a 64 MB dictionary reaches 0.576 against `gzip
-9`'s 0.600, and `zstd -19 --long=27` (128 MB window) manages only 0.591. The
floor is set by the opaque per-rank value bytes — endpoints, keys, addresses —
which do not compress at all. Roughly 60% of a modex is incompressible no
matter how it is framed.

So the per-bucket pass is free ratio-wise and buys a 65% smaller rollup. Leave
it alone.

**What does move the needle is the compressor, not the layering.** Measured on
the same 25.6 MB aggregate, single-threaded: `zstd -1` matches `gzip -9`'s
ratio (0.600) at **640 MB/s against 64 MB/s**, and `zstd -3` beats it (0.588)
at 427 MB/s. That is the lever on the progress-thread stall above — the same
256 MB release that costs 5.5 s of blocked HNP under `gzip -9` would cost
around 0.4 s. PMIx's `pcompress` framework is where such a component would go;
it already carries `zlib` and `zlibng`.

### Measuring it

`grpcomm_base_verbose 1` reports raw size, on-wire size and ratio for **every**
broadcast, compressed or not, so the line is a complete census of what a DVM
broadcasts. Adding `grpcomm_enable_timing 1` appends what the deflate cost the
originator:

```sh
prterun --prtemca grpcomm_base_verbose 1 --prtemca grpcomm_enable_timing 1 ...
```

Timing is off by default because the clock reads it needs sit directly in the
broadcast path. There is no separate size gate to sweep — the compressor's own
`pcompress_base_limit` and `pcompress_*_level` are the knobs, so vary those:

```sh
--pmixmca pcompress zstd --pmixmca pcompress_zstd_level 1
```

### How long a broadcast took: three stamps, and which pair to use

`grpcomm_enable_timing` also stamps absolute microseconds at three points, so
that the quantity a change to the fanout tree is actually about — how long the
payload took to reach the whole DVM — can be measured. An end-to-end fence
cannot resolve it: the rollup's noise is larger than the effect.

```text
grpcomm:xcast:timing started   op_id N tree K at <sec>.<usec>   originator only
grpcomm:xcast:timing processed op_id N tag T tree K at ...      every daemon
grpcomm:xcast:timing completed op_id N tag T tree K at ...      master only
```

Pair them on **(tree, op_id)**, never on the tag: the `started` line cannot
name the payload tag, because at that point `tag` is the tag the message
*arrived* on (`PRTE_RML_TAG_XCAST`) and the real one is still packed inside.
Several broadcasts are in flight at once and they complete out of order, so
the pairing key is load-bearing rather than a convenience.

**Which span to measure depends on whose clock you have.**

- `started` → last `processed` is the coverage itself, and it is only
  meaningful where the clocks are common: a container swarm sharing one
  kernel, yes; a real cluster, no. NTP holds a cluster to about a
  millisecond and the whole broadcast is shorter than that, so the answer
  there is clock skew with a broadcast buried in it.
- `started` → `completed` is the same span measured **at one end**, on the
  master, and is what to use anywhere else. It reads high by the ack tree's
  return path, which is separable: a zero-byte broadcast is very nearly a
  measurement of it on its own.

Pair either with the census line's own `raw`/`wire` sizes, printed by
`grpcomm_base_verbose 1` just before `started` on the same thread. Do not
average a tag's spans without doing that — **a fence emits two releases per
iteration on the same tag**, the allgather's (large) and the barrier's (tens
of bytes), so a median over all of them is a median of the barrier.

A `--daemonize`d DVM discards the HNP's output, so any of this needs a
foreground `prte` or `prterun`.

### Timing runs want an optimized build — a debug build measures itself

**Do not draw conclusions about sizes or times from a `--enable-debug` build.**
The overhead is not a uniform tax that cancels out of a comparison; it changes
the very quantities being measured.

The one that matters most here is that **a debug PMIx describes its buffers**.
`pmix_bfrops_globals.default_type` is `PMIX_BFROP_BUFFER_FULLY_DESC` under
`PMIX_ENABLE_DEBUG` and `PMIX_BFROP_BUFFER_NON_DESC` otherwise, so every value
packed into a buffer carries a type descriptor it would not carry in
production. A launch message, a nidmap and a fence bucket are all built that
way, so in a debug build they are **larger** — and their compression ratio is
flattered too, because those descriptors are highly repetitive and deflate eats
them. Both halves of `raw` and `wire` in the census line move, in opposite
directions from the truth.

Note this is **PMIx's** debug flag, not PRRTE's: the buffer type is compiled
into `libpmix`. A PRRTE built `--enable-debug` against a PMIx built without it
still packs production-sized buffers, which is the usual development setup and
is fine to measure. Check what you actually have before trusting a number:

```sh
grep PMIX_ENABLE_DEBUG <pmix-build>/src/include/pmix_config.h
```

The rest of a debug build — assertions, `PRTE_ERROR_LOG` paths, unoptimized
code — inflates times everywhere and is the ordinary reason not to benchmark
one.

**`scaletest` cannot resolve a small effect either**, whatever the build. At 40
nodes the collect fence's wall clock has a run-to-run coefficient of variation
of 35-50%; a four-arm sweep (compression on/off crossed with compressible and
incompressible payloads) put every ON/OFF pair fully inside the other's range,
with the *sign* flipping between radix 4 and radix 64. That is consistent with
the model — the swarm's wire is loopback, so `B` is enormous and no `d * k` a
40-container swarm can reach compensates — but it settles nothing on its own.

`scaletest`'s default payload is a repeating 256-byte ramp that deflate squashes
by ~250:1. Any compression number read off it is fiction; `--entropy` fills with
an xorshift stream instead, which is the floor. Real modex data — endpoints,
keys, addresses — sits much closer to the floor than to the ramp.

### Ordering and fault tolerance

The in-file comments are the real spec — read them. The load-bearing ideas:

- **`process_first` set.** Most xcasts forward before processing (to
  preserve ordering), but `PRTE_RML_TAG_WIREUP` and
  `PRTE_RML_TAG_DAEMON_DIED` are processed *first* because they change the
  child set: a death *grows* our subtree (orphans promote to us), so we
  must repair before forwarding. `PRTE_RML_TAG_DAEMON_REVIVED`
  deliberately stays on the forward-first path (a return *shrinks* our
  subtree) — the comment explicitly says do not move it.
- **Late joiners.** A daemon that has never seen an xcast
  (`op_id_inited == 0`) but is handed op N>1 is a grown/rebooted/bootstrap
  daemon; it adopts ops `1..N-1` as complete so `finish_op` does not raise
  `PRTE_ERR_OUT_OF_ORDER_MSG` and force-exit.
- **Promotion / re-parenting.** `xcast_fault_handler` (local scope only)
  invalidates upward ack-ids, resets `nexpected`, starts new ack rounds,
  and holds replays (`replay_pending_parent`) after a promotion until the
  parent replays in order.
- **`op_id_completed_at_promotion`** stops a promoted daemon wrongly
  assuming its *newly-acquired* subtree finished ops it completed before
  promotion.

### Turning the release onto its own tree takes TWO parameters

`grpcomm_low_radix_release` (bool, **default false**) is the switch: it alone
decides whether a fence's release leaves the routing tree. Off, every release
goes down the routing tree and none of the derived-tree code below runs.

`rml_base_radix2` (int, **defaults to `rml_base_radix`**) only gives the
release tree its shape, and is not consulted at all while the switch is off.

Setting the switch and leaving the radix alone does nothing useful, and it is
the obvious mistake: at equal radices the release tree *is* the routing tree,
identical parent and children for every rank and every failure pattern
(`test_release_tree_matches_routing` in `test/unit/rml`). The master emits the
`release-radix-noop` help topic for that combination and carries on. Note
which way round the two want to be: the rollup wants a **wide** tree (a
gathering daemon receives r messages and sends one aggregate, so width is
free) and the release wants a **narrow** one (a broadcasting daemon receives
one and sends r, so width is the whole cost).

### A derived tree's edges must be sent DIRECT

`prte_rml_get_route()` answers on the **routing** tree, so an edge of any other
tree that is not also a routing edge gets *relayed*. At a high routing radix
the relay is the controller itself, which means the bytes cross the very link
the second tree exists to keep them off, the controller handles them twice,
and the fanout is not reduced at all. Measured on eight daemons at routing
radix 64 with release radix 2: seven of nine release edges went back through
rank 0, which is worse than not having a second tree.

So `forward_payload_to()` and `send_ack_msg()` route their sends through
`edge_is_direct()`: the routing tree keeps the routed send (there every edge
*is* the route), and every other tree gets
`prte_rml_send_payload_direct_cb_nb()` / `prte_rml_send_buffer_direct_cb_nb()`.
Both fall back to a routed send on their own when contact information is
missing, so no caller handles "no direct route".

A peer reached that way and not also a tree neighbour is registered with
`prte_rml_lateral_register()`. That is about faults, not delivery: losing a
lateral link means the tree has **not** changed shape, and repairing on the
strength of it would end every in-flight collective in the DVM.
`prte_rml_route_lost()` consults `prte_rml_is_lateral_only()`, deregisters,
and calls the lateral-lost callback instead of repairing.

grpcomm installs that callback (`prte_grpcomm_xcast_lateral_lost`), and it
closes a hole that only exists once edges are direct: an *undeliverable*
forward is covered by `forward_lost`, but a link that drops **after** the
forward landed produces no send completion at all, and the operation then
waits on an ack that is never coming. The handler re-derives and re-forwards,
and deliberately concludes nothing about the peer being dead — the RML reached
it precisely because it decided the loss was not a tree fault.

**Check this when measuring.** `--prtemca rml_base_verbose 2` names the macro
each send used; a release-tree forward from a non-controller must appear as
`RML-SEND-PAYLOAD-DIRECT-CB`.

### A derived tree repairs by different rules, and they are not optional

An op may travel a tree other than the routing one — today a fence release
under `grpcomm_low_radix_release`, tomorrow anything else added to
`prte_grpcomm_topology_t`. Such a tree is **derived**: every daemon computes
it from the daemon count, the failed set and `rml_base_radix2`, all of which
they hold in step, so there is no protocol and nothing to keep synchronized.
That is what makes it cheap. It also means **none of the routing tree's
recovery inputs describe it** — `status->promoted`, `->children_changed`,
`->prev_children` and `prte_rml_base.n_children` are all facts about the
routing tree, and applying them to an op travelling another one is not a near
miss but the wrong tree in every particular. With `rml_base_radix 64` a
non-master daemon has *no* routing children at all, and the handler read that
as "my subtree is empty, this op is complete" and retired a release it had
forwarded nowhere.

`release_tree_fault()` is the separate path, and five rules hold it up. All
five are properties of deriving a tree rather than being told one, so anything
added here inherits them:

- **Both halves of the derivation must agree.** `prte_rml_release_tree()`
  computes a parent *and* children, and they have to describe the same tree
  for every failure pattern. Walking up past dead ancestors to name a parent
  while replacing a dead child in place is two different promotion rules and
  orphans daemons outright. `test_release_tree` (`test/unit/rml`) brute-forces
  every failure subset and requires one tree rooted at 0 covering exactly the
  living daemons — run it before believing any change here.
- **Do not screen a forward on "is the sender my parent".** That screen is
  sound on the routing tree, whose repair notice travels the routing tree, so
  a daemon has processed the notice that gave it a new parent before anything
  that parent relays can arrive. A derived tree has no such ordering: a
  repaired parent replays to a child that has not heard yet. Both the forward
  and the ack-request screens are therefore asked only of
  `PRTE_GRPCOMM_TOPO_ROUTING`.
- **Ack whoever asked** (`send_ack_to`, `op->upstream`), not whoever this
  daemon's own derivation currently calls its parent. Answering the wrong
  daemon costs nothing; failing to answer the right one hangs the broadcast.
- **Nobody defers.** `replay_pending_parent` is a routing-tree device; here a
  daemon deferring to a parent that has already replayed waits for a second
  replay that never comes, and strands its subtree. It is also unnecessary: an
  op a child is missing is still in flight, hence held and replayed by someone
  in the same pass, in op order.
- **A forward must be read under the view it was computed in.** Every forward
  carries `prte_rml_tree_version()` — a monotone count of the departures and
  returns the sender has learned of. A receiver that is behind it **and** was
  addressed by a daemon it does not call its parent parks the op
  (`awaiting_news`): it takes the payload and delivers it locally, which does
  not depend on the tree, but derives nothing until the notice lands. Without
  this, a daemon promoted into a dead relay's slot still reads itself as a
  leaf, retires the op as complete with zero children, and strands everything
  beneath it. Both conditions are required — the version is a count of events
  learned rather than an agreed number, and daemons legitimately hold
  different ones for a while (a grown daemon is seeded from the departed set
  it was launched with), so the "not my parent" test is what keeps ordinary
  traffic from ever being parked.

`grpcomm_xcast_delay_ms` / `_vpid` is the instrument: it holds one daemon's
*forward* on a non-routing tree, so a broadcast that is otherwise over in
microseconds can have a daemon killed underneath it on purpose. Its two
siblings (`grpcomm_fence_delay_ms`, `grpcomm_release_delay_ms`) both act after
the forward has gone and cannot reach this path at all. It ships compiled in,
for the same reason they do. `contrib/dockerswarm`'s
"a release tree repairs itself when a relay dies" is the case, and its first
assertion is a canary on the hold being live — without it every assertion
below passes vacuously.

### The forward is shared, and that changes what a send completion means

`tree_whole_forward()` packs the forward **once** and hands the same
`prte_rml_payload_t` to every child, because the bytes do not depend on the
destination. Two consequences are easy to get wrong:

- **A send takes its own reference, and only once it has accepted the
  message.** `prte_rml_send_payload_cb_nb()` retains after every early
  return, so a *refused* send leaves the caller's count exactly as it found
  it — which is why `forward_payload_to()` has nothing to unwind on failure
  and `tree_whole_forward()` drops exactly one reference of its own after
  the loop.
- **The completion callback is handed a NULL buffer.** For an ordinary
  buffer send the callback owns the buffer; for a shared payload the buffer
  belongs to the payload and the other destinations may still be
  transmitting it, so `PRTE_RML_SEND_COMPLETE` passes NULL instead. That is
  what lets `forward_lost()` pass its arguments straight through to
  `prte_rml_send_callback()` without freeing a buffer `k-1` other sends are
  still using.

**A forward that fails synchronously ends the DVM; one that fails
asynchronously only costs the op that subtree.** `forward_payload_to()`
force-exits on a non-success return, while `forward_lost()` deliberately
just drops `nexpected` — the same event, opposite reactions. What makes
that safe is an invariant that lives in `src/rml`: `update_descendants()`
replaces any child found in `failed_dmns` with its next living descendant,
and it runs *after* `prte_rml_repair_routing_tree()` has marked the new
failures — so a rank in `prte_rml_base.children` is never one
`prte_rml_is_node_up()` calls down, and `PRTE_ERR_NODE_DOWN` is not a
return this loop can actually see. A change on either side of that (a
child set that can hold a failed rank, or a marking that moves after the
repair) turns an ordinary daemon loss into a DVM teardown.

### Completion callbacks (the `pending_completions` FIFO)

The op the master ends up *tracking* is a fresh one built on receipt, not
the initiating op — so a callback cannot ride the initiating op. Instead
`begin_xcast` enqueues one entry per **master-originated** broadcast (NULL
callback included, to keep alignment), in send order; on receipt the
master pops the FIFO head and attaches it. `finish_op` fires it, master
only, where a completed op means the *entire DVM* has the broadcast. This
is the hook the elastic DVM-shrink path uses.

---

## `fence` — allgather / barrier (`grpcomm_fence.c`)

`prte_grpcomm_fence()` rejects a NULL `procs` array
(`PRTE_ERR_NOT_SUPPORTED`), builds a `prte_pmix_fence_caddy_t`, and
thread-shifts to the static `fence()` handler.

1. **`fence` handler.** Computes the signature, gets-or-creates the
   tracker, names the operation from the directives, packs signature +
   operation + info + payload, and **sends it to itself** on
   `PRTE_RML_TAG_FENCE` — funnelling the local contribution through the
   same receive path everything else uses.  A barrier packs no payload.
2. **`fence_recv`.** Checks the epoch, unpacks the signature, finds the
   tracker, merges the operation (see *Two operations* below), merges info
   (`PMIX_TIMEOUT` takes the max; a non-success `PMIX_LOCAL_COLLECTIVE_STATUS`
   is sticky), copies the payload into `coll->bucket` **if this is an
   allgather**, bumps `nreported` either way. At `nreported == nexpected`:
   - **HNP:** broadcast the result via `prte_grpcomm_release_bcast`.
   - **non-HNP:** forward the bucket up to `PRTE_PROC_MY_PARENT`.
3. **`fence_release`.** Finds the tracker (missing tracker == "I had no
   local participants", not an error) and fires `coll->cbfunc` to hand the
   gathered data back to the PMIx server. Removes and releases the tracker.
   A barrier is handed a NULL payload rather than an empty one, which is what
   tells PMIx to skip its store outright. The release message itself does not
   carry the operation and does not need to: a daemon only reaches this path
   by holding a tracker, and a tracker knows which collective it is.

**`create_dmns()`'s answer is a pair, and NULL means two different
things.** A signature naming the daemon job is "every daemon in the DVM",
reported as a count with a **NULL array**; `set_nexpected()` answers that
as `n_children + 1`. A NULL array with a **zero** count is the opposite
statement, "nothing resolved". Both readers used to index the array
regardless, which dereferenced NULL for any fence naming the daemon job.

**A signature that cannot be read is refused, and refusal leaves nothing
behind.** `create_dmns()` returns `PRTE_ERR_BAD_PARAM` for an empty
signature or one whose first entry has no nspace — both of which are what
a truncated RML message unpacks to, and the second is worse than it looks,
because `PMIX_CHECK_NSPACE` answers *yes* for an empty nspace against
anything, so it would be taken for a daemon-job fence and sized to expect
the whole DVM. `get_tracker()` then removes the half-built tracker before
returning NULL. That removal is not tidiness: a tracker with no daemon set
can never complete, and the next fence with the same signature would
*find* it, see a rollup expecting nothing, and answer immediately with
data it never gathered.

**Every exit from the `fence()` handler destructs the signature it built
and completes the caller.** The signature is a stack object with a
malloc'd proc array that `get_tracker()` copies rather than adopts, so
walking away from it leaks it once per `PMIx_Fence`. And a failure there
is invisible to everyone else: the entry point answered `PRTE_SUCCESS` the
moment the request was queued, so no release is coming. The handler
completes its own participants with the reason, after clearing the
tracker's `cbfunc` so a later abort cannot complete them twice.

**The fence's deadline is the DVM's to keep.** A participant can put a
`PMIX_TIMEOUT` on a fence; the controller arms a timer on the first
contribution carrying one (largest offered wins), disarms on convergence,
and ends the fence with `PMIX_ERR_TIMEOUT` through `abort_fence_op()`.
Nothing else is watching: the PMIx server library arms a timeout while it
gathers the *local* contributions, then deletes it the instant the request
is handed to the host — deliberately, so a late host answer cannot reach a
tracker it already released.

**A contribution can outlive the release that ended its fence, and a
generation on the wire is what tells that from the next round.**  A fence
signature is only its participant list, so nothing about a contribution says
which round it belongs to.  In the normal flow that costs nothing, because a
daemon converges only when everything it expects has arrived, so nothing
*can* arrive afterwards.  But `abort_fence_op()` ends a fence early — on a
`PMIX_TIMEOUT`, and on a participant lost to a failed daemon — and a
contribution still climbing the tree then reaches a daemon whose tracker the
release already retired.  Without a round number `fence_recv()` builds a new
tracker for it, and the next fence over those same participants *finds* that
tracker, inherits its `nreported` and its bucket, and converges early
carrying the previous round's data.

**It is a counter, not a memo, and that is why `completed_group_ops` could
not be copied.**  The group memo works because a group is keyed by `groupID`
+ operation and `group()` drops the entry when a local client starts one.  A
daemon relaying a fence for its subtree has no local client and would never
drop it, so the next fence's legitimate contribution would be discarded — a
hang, worse than the wrong answer it was meant to prevent.  What works is a
per-signature *release count*: `prte_grpcomm_globals.fence_generations` holds
one `prte_grpcomm_fence_memo_t` per signature giving the number of the **next**
fence over it, one past the last released here.

The rules, and each earns its place:

- **The count is driven by releases**, and `prte_grpcomm_fence_gen_record()`
  **adopts** the released generation rather than incrementing.
- **Every contribution is stamped** with its tracker's generation, and the
  **release carries one too** — up *and* down, so a daemon can learn a number
  it never counted.
- **`fence_recv()` screens before `get_tracker()`.**  A stamp below what we
  expect is dropped there, deliberately ahead of building anything: creating
  a tracker and then discarding the message would leave exactly the wreck the
  mechanism exists to prevent.
- **The counter has to bootstrap, and that means round 0 must be a real
  round.**  A daemon present since the DVM started takes "no entry for this
  signature" as round 0 and stamps 0.  Without that nothing ever establishes
  a first round: every contribution is stamped "unknown", nothing is ever
  recognized as stale, and the mechanism is **inert** — which is exactly what
  the first version of this did, and it passed every unit test while doing
  nothing, because the tests drove the counter directly instead of through
  the path that has to start it.

- **`PRTE_GRPCOMM_FENCE_GEN_UNKNOWN` is for a daemon a grow added, and only
  for one.**  It has released none of the earlier rounds, so it cannot claim
  0 — every daemon that has been present is past it and would drop a 0 as
  ancient, hanging the fence.  It says it does not know instead, which a
  receiver takes into whatever round is current.  Safe because a joiner has
  no earlier round over that signature to have straggled from, so its first
  contribution cannot be one: the window is **one contribution per signature
  per joiner**, and after its first release it holds a real number.

- **Which of the two a daemon is, it is told rather than derives**
  (`prte_grpcomm_fence_note_join()`), on the first wireup it receives, and
  never revised afterwards — a later wireup describes a DVM it is already
  part of.  What travels is a **flag, not a count**, and that is the whole
  reason it works: a count would be stale on arrival, because the master goes
  on answering fences over other signatures while the grow completes and
  there is no moment at which a number handed over is still true.  A flag is
  true exactly as long as it is true.  Deriving the count locally is not
  sufficient on its own either, which is why it is checked on arrival rather
  than only counted; Slurm's `kvs_seq` carries it in both directions for the
  same reason.
- **The memo is bounded** (`PRTE_GRPCOMM_FENCE_MEMO_MAX`).  Eviction is a
  graceful loss: that signature returns to the pre-generation behaviour, where
  a straggler and a new round are indistinguishable.  It does not corrupt
  anything, and an entry only has to outlive its own fence's in-flight
  messages.

The other half of this was already in place: `0d9dde1c8a` retires the tracker
*before* delivering, at both sites, because a client may fence again over the
same participants the moment the callback returns.

**Reproducing the race.**  The window is a timing accident no test can
arrange from outside, so `grpcomm_fence_delay_ms` (with
`grpcomm_fence_delay_vpid`) holds one daemon's own contribution back.  Paired
with a `PMIX_TIMEOUT` on the fence, the controller ends the round without
that daemon and the held contribution lands afterwards — the straggler.  The
knob is **compiled in always**, deliberately: a race hook that exists only
under `PRTE_ENABLE_DEBUG` cannot reproduce a race on the build that shows it.
`contrib/dockerswarm`'s *"a straggler from an aborted fence is not the next
round"* case drives it, and it has been watched failing with the drop removed
— a regression test for a race that has never been red proves nothing.

**A test that asserts a collective *delivered* something must use
`PMIX_OPTIONAL` on the readback.**  Without it a `PMIx_Get` for a key the
fence did not carry falls through to a direct modex and fetches it from the
owning daemon anyway; the value turns up and the assertion proves nothing.
That cost two full build-and-run cycles of false green here.  (It is also a
neat demonstration that the on-demand path works: the fence had genuinely
lost the key and the application never noticed.)

**The early contribution, and why it is not a corner case.**
`PRTE_RML_TAG_FENCE_RELEASE` is not in `xcast`'s `process_first` set, so a
daemon forwards a release to its children *before* processing it itself.  A
child can therefore be released, start the next round, and have its
contribution reach the parent while the parent is still in the previous one.
That is what `(signature, generation)` keying is for: the early contribution
gets a tracker of its own instead of landing in a bucket the release is about
to discard.

Adopting the higher number onto the live tracker instead — which is what the
first attempt did — is worse than dropping, because it relabels a tracker
holding the previous round's data.  The symptom is not wrong data but a
**hang**: the early contribution is consumed by the wrong round, and the round
it belonged to then waits for something that has already arrived.  Measured,
with the generation removed from the key: the second fence completes on 0 of 4
ranks while the first still succeeds and the DVM survives.

`grpcomm_release_delay_ms` (with `grpcomm_release_delay_vpid`) is what makes
that reachable — it holds one daemon's own *processing* of a release back
while the forward to its children goes on time, widening a window that is
otherwise microseconds.  Drive it at `rml_base_radix 1` so the tree is a chain
and the delayed daemon is genuinely interior; hanging it off the HNP as a leaf
tests nothing.  See the *"a contribution for the next round does not join this
one"* case in `contrib/dockerswarm`.

**A release with no local callback still has data to free.** A daemon
holding a tracker only because it relayed for its subtree has no `cbfunc`
— the common case on any interior daemon — so that branch frees the
payload itself.

**Only a fence that was in flight when the failure landed is ended.** A
fence *started afterwards* completes among the survivors, because
`prte_rml_get_num_contributors()` skips failed daemons when the tracker is
built. That is deliberate, and it is the opposite of where the group
policy sits (at completion, in `check_complete`). The failed-daemon set is
permanent, so applying the test at completion would make every later fence
naming that namespace fail for the life of the DVM, leaving a recoverable
job unable to fence at all after its first loss. If you move this check,
that is the regression to expect.

A fence whose participants all survive lost only a message path and
re-converges at the new epoch — so losing a pure relay is invisible to it.
A fence that genuinely lost a participant cannot produce its answer,
because an allgather has no opt-in to running degraded, so the controller
ends it with `PMIX_ERR_LOST_CONNECTION`. Note the difference from a group
construct, which *can* complete on survivors when asked: a fence has no
equivalent of `PMIX_GROUP_FT_COLLECTIVE`.

### Two operations, one movement

**`PMIX_COLLECT_DATA` names the operation, and nothing else may.** False or
absent is a **barrier**; true is an **allgather**. `prte_grpcomm_fence_op_from_info()`
reads it out of the info array PMIx hands to the upcall, `fence()` records it
on the tracker, and every contribution carries it as a byte of its own ahead
of the info array — `fence_op_pack()` / `fence_op_unpack()`.

**Do not derive it from the payload.** The bytes vary from daemon to daemon
while the operation must not: a participant with nothing to publish is fully a
participant in an allgather, and since PMIx learned to contribute only what
changed, a zero-byte contribution is the ordinary case for any fence after the
first rather than a degenerate one. Deriving the operation from the payload
would have daemons disagree about which collective they are in, and a fence
has no originator to settle it — the same failure that withdrew the lateral
movements. The directive is safe precisely because it is a property of the
*call*: every participant passes the same value, and PMIx has already forced
the local participants to agree before the upcall (disagreement there becomes
`PMIX_COLLECT_INVALID` and the fence is refused locally).

**What the operation gates is the payload, in three places** — the
contribution `fence()` packs, the bucket `tree_gather_answer()` sends up or
broadcasts, and the unload `fence_release()` performs. It gates *nothing*
about participation: `nreported`, `reported_slots` and `self_reported` are
counted, never weighed, so an empty contribution advances the rollup exactly
as far as a large one. That is the invariant that lets an allgather stay an
allgather when a participant has nothing to add, and any future exchange
schedule will depend on it.

**A barrier has no data path at all.** PMIx still builds a blob for one — a
lone `PMIX_COLLECT_NO` flag byte, compressed and wrapped — but it never leaves
the node: rolling one of those up from every daemon and broadcasting the
concatenation back to all of them spends the whole round trip on bytes that
say only "there is nothing here". The receiving side wants it no more than we
do, because PMIx skips its store outright when the host returns no data, where
a present-but-empty payload makes it walk the blobs to find that out.

**A disagreement is reported, not resolved.** `prte_grpcomm_fence_op_merge()`
adopts on the first answer and requires agreement after that; a mismatch emits
`help-prte-grpcomm.txt`'s `fence-op-mismatch` and makes `coll->status` sticky
at `PMIX_ERR_INVALID_ARG` — the status PMIx itself answers for the local form
of the same user error — which the rollup carries to the controller and the
release carries back out to every participant. The contribution is still
counted: convergence is what delivers the failure, so refusing to count it
would hang instead. **This check is load-bearing rather than belt-and-braces.**
PMIx compares a collect-flag byte per contribution inside `store_modex` and
raises `collection-mismatch`, but a barrier no longer puts anything on the wire
for it to compare, so PRRTE is now the only thing that can see the two answers
together.

### One movement: rollup and release

A fence rolls its contributions **up the routing tree** to the controller,
which broadcasts the gathered result back down. Both operations travel that
way — the operation decides what rides along, not where it goes — and no
movement id is on the wire.

The seam that a different movement would use is still visible in the shape of
the code — `tree_gather_converged()` / `_contribute()` / `_answer()` are
separate functions because "has it got everything" and "what to do once it
has" are the two things an allgather would replace. They are called directly.

**If you reintroduce a lateral movement, the hard part is not the schedule.**
A broadcast has one originator who decides for everybody; a fence has none,
so every participant must reach the same answer independently or the
collective hangs with half the daemons waiting for a release the other half
will never send. That is why the withdrawn implementation carried a movement
id purely to *detect disagreement*, and why its deadline had to become
per-participant. It also could not use the recovery restart, because an
exchange's shape is the participant list rather than the routing tree.


## `group` — PMIx group operations (`grpcomm_group.c`)

The rollup/release skeleton mirrors `fence`, with a much richer signature
and payload.

- **Cancel short-circuit** (`#if PRTE_PMIX_HAVE_GROUP_FT`): a
  `PMIX_GROUP_CANCEL` op is not a rollup collective — it routes straight
  to the HNP via `request_group_cancel()` and returns.
- Otherwise it builds the signature from `grpid` + `procs` (a NULL `procs`
  marks a bootstrap **follower**), scans the directives
  (`prte_grpcomm_group_parse_directives()`), gets-or-creates the tracker,
  and relays.
- **Bootstrap** ops send **directly to the HNP** (no rollup tree — each
  daemon reports straight to the controller); non-bootstrap ops send to
  self on `PRTE_RML_TAG_GROUP`.

**A signature owns its arrays — always.** Two directives carry a proc
array (`PMIX_GROUP_ADD_MEMBERS`, `PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`), and
the array inside a directive belongs to the **PMIx server** that delivered
the upcall: PMIx frees the directives, arrays and all, once the operation
completes. The signature's destructor frees what the signature holds, so
the parse takes a *copy* of each. Pointing at the caller's array instead
is a double free of live heap, and it is not theoretical. If you add a
directive that carries an array, copy it in `copy_directive_procs()` like
the other two; do not add a "clear it again before the destructor runs"
step anywhere.

**Completion** is `nleaders_reported >= nleaders && nfollowers_reported >=
nfollowers` for bootstrap, else `nreported >= nexpected` — `>=` rather
than `==` because a fault can lower `nexpected` under a tracker that has
already counted more than it now needs; the `converged` latch keeps that
from answering twice. A bootstrap additionally requires `0 < nleaders`:
both counts are zero until the first *leader* arrives to supply them, so
without that guard a tracker created by a follower would satisfy `>= 0` on
its own and converge on one contribution with an empty membership.

**`grp_release` is a continuation, not a wait.** The
`PMIx_server_register_resources` call used to be waited on with
`PMIX_WAIT_THREAD`. Nothing raced, but the wait parked the **progress
thread** — and unlike the teardown waits converted for
[#2534](https://github.com/openpmix/prrte/issues/2534), this one runs on
every daemon for every group construct. It is now chained through
`grp_release_regcbfunc` → `grp_release_resume` → `grp_release_complete`.
Three things about the shape are load-bearing:

- **A file-local caddy owns everything that has to survive the gap**, and
  frees all of it in its destructor.
- **The tracker is re-looked-up in the continuation, not carried.** The
  progress thread is free while PMIx works, so an abort or duplicate
  release can delete the tracker in the interim; a cached pointer would
  dangle.
- **`PMIX_OPERATION_SUCCEEDED` means it already finished**, so that path
  falls straight through rather than waiting for a callback that will
  never come.

`abort_group_op()` broadcasts a release carrying only signature +
status; the normal non-success path then completes each daemon's local
participants and deletes the tracker — so a cancel/abort tears down the
collective **without tearing down the DVM**, which is the whole point.
Only the controller may call it — it is the sole xcast source — and every
caller sits behind a `PRTE_PROC_IS_MASTER` test for that reason.

**The entry point owes the participant an answer on every path.**
`prte_grpcomm_group()` has already returned `PMIX_SUCCESS` to the PMIx
server by the time `group()` runs, so nothing upstream will fail the
client if the handler bails out: a silent `return` leaves it blocked in
`PMIx_Group_construct` with no collective in flight to release it. Every
failure therefore leaves by the `error:` label. That label does two things
and both are load-bearing — it invokes `cd->cbfunc` with the reason, and it
first clears `coll->cbfunc`/`coll->cbdata`, because the tracker itself
*stays* (a release from the controller, which can still abort the
operation, has to find it) and a release arriving later would otherwise
complete the same client a second time with the same `cbdata`. This is the
same rule the fence entry point follows — see *Retire before you deliver*.

**A controller that cannot build the release must abort, not return.** By
the time `check_complete()` starts packing, `converged` is latched, so
nothing will drive that tracker again — and on the controller the recovery
restart deliberately skips a converged tracker, because its release is
supposed to be on the wire already. A bare `return` out of the packing
therefore hangs *every* participant in the DVM, permanently. The `failed:`
label instead falls back to `abort_group_op()`, whose message is only a
signature plus a status and so is by far the smallest thing still worth
trying to build.

**`ft_collective` means "some *surviving* participant asked for it."** It
is accumulated by sticky-OR as contributions merge, so a participant that
requested it and then died before rolling up is not visible. This is a
deliberate superset of PMIx's own rule, which takes the first
`PMIX_GROUP_FT_COLLECTIVE` in the aggregated block info and ignores the
rest — do not "fix" it to match.

---

## Surviving a daemon failure

This applies to **both** fence and group; they differ only in what they do
about a collective that genuinely lost a participant.

A daemon failure invalidates every in-flight rollup: `nexpected` is derived
from the routing tree, and the tree just changed shape. Recovery is a
**restart**, not a repair — each daemon discards what it gathered,
recomputes what the repaired tree owes it, and re-offers
`coll->my_contribution`, kept for exactly this.

**The epoch.** One `recovery_epoch` per daemon, shared by both
collectives, stamped on every `PRTE_RML_TAG_GROUP` and
`PRTE_RML_TAG_FENCE` message as `[epoch][body]`. A contribution stamped
older than the receiver's epoch belongs to a round that no longer exists
and is dropped before it is merged.

**The value is the master's, and absolute.** The DVM master issues it —
`prte_grpcomm_issue_epoch()`, called once per global failure notice it
emits — and packs it into that notice; every daemon adopts what it is
given (`prte_grpcomm_advance_epoch(status->epoch)`), taking the highest
value it has seen. Each daemon counting the notices *it* received would be
simpler and is wrong: the number would then be a function of delivery, so
a daemon that missed one broadcast is a step behind for the rest of the
DVM's life, with every contribution it offers dropped as stale and every
collective over a job placed on it hung. That is not a hypothetical — a
daemon launched by an elastic grow has missed **all** of them: the routing
tree holds its vpid from the moment the grow records it, so a broadcast
sent while its launch is in flight is addressed to a daemon with no
contact info and is dropped by design (`prte_oob_base_send_nb`).

An absolute value is also what makes the epoch **tellable**, which is the
other half of the same problem. The `PRTE_RML_TAG_WIREUP` broadcast — sent
once every expected daemon has reported in, and the first message that can
reach a daemon which has just joined — carries `prte_grpcomm_current_epoch()`,
and `process_wireup()` adopts it. Because adoption is by highest value
seen, the seed and any notice still in flight commute: a daemon already at
or past that epoch is unaffected, and a late wireup cannot walk anyone
back. The issued counter is deliberately separate from the applied one:
the master's own epoch does not move until its broadcast is relayed back
to it, and a second failure inside that window would otherwise reissue the
number the first notice is already carrying, collapsing two restarts into
one — a hang, not a wrong answer.

A daemon that joins a **bootstrapped** DVM, with no HNP-built wireup
behind it, still starts at zero. `rml_base_dead_dmns` and the vpid holes
the nidmap encodes carry the same limitation, and for the same reason:
everything that repairs a late joiner's view of the DVM is something the
HNP sends it.

**Why a per-link round does not work here, and why one epoch does.** The
obvious model is xcast's — `ack_id_down` chosen by the parent, echoed by
the child. It does not transfer. xcast's payload flows *down*, so the
parent always holds the op and can re-poll; a rollup flows *up*, so the
parent may hold nothing at all (a pass-through daemon's tracker is created
lazily, on the first arriving contribution). A round cannot invalidate a
contribution the parent has **already absorbed** — that invalidation has
to travel up, and rounds travel down.

What makes a single epoch work is that the restart is *simultaneous*. The
GLOBAL fault scope reaches every daemon from one broadcast, in the same
order everywhere, after the tree is repaired. `PRTE_RML_TAG_DAEMON_DIED`
is in xcast's `process_first` set, so a daemon hands the notice to its own
delivery before forwarding, and libevent runs that active event before its
next round of socket reads — so a daemon has advanced its epoch before it
can read a contribution from a child that advanced first. A **newer**
stamp should therefore be unreachable; the recv handlers handle one anyway
by adopting it and logging, so the ordering argument stays falsifiable.

**A partial restart is the failure mode to fear**, not a hang: a daemon
that resets and re-sends into a parent that did not would have its subtree
counted twice and another not at all — a quietly wrong result. That is why
`prte_grpcomm_advance_epoch()` walks *every* tracker.

**A converged collective is finished, on the controller.** Neither the
restart nor the fault handler touches a tracker the controller has already
answered: the release is on the wire, ordered ahead of anything a restart
could send. Re-running the rollup there answers twice — a second release, a
second context id consumed, the same group registered twice everywhere.
The test is *only* valid on the controller: `converged` on any other daemon
means it rolled its aggregate up to its parent, and re-sending that
aggregate is precisely what recovery is for.

**Still aborted, deliberately:** bootstrap operations (no resolved daemon
set, and `nleaders` is a count with no identities) and anything in flight
across a **revival** (`PRTE_RML_TAG_DAEMON_REVIVED` is forward-first by
design, so it cannot give the ordering an epoch advance needs). The
revival path is discriminated by LOCAL scope with an empty `failed_ranks`.

Two supporting pieces: `nreported` is backed by `reported_slots`, a bitmap
keyed on `prte_rml_get_subtree_index()` of the sender, so a replayed
contribution is idempotent rather than double-counted (the info-list
accumulation appends with no key matching, so a double-count is also a
duplicated payload). And `completed_group_ops` is a bounded memo of
already-released operations, consulted by `grp_recv` so a straggler cannot
build a tracker nothing will ever complete or delete; `group()` clears the
matching entry, because a local client starting an operation is proof the
previous one of that name is over.

---

## Things to watch when editing

- **Everything runs on the single progress thread.** Every entry point
  (`fence`, `group`, `xcast_nb`) immediately thread-shifts: heap caddy/op,
  `prte_event_set(prte_event_base, &cd->ev, …)`, `PMIX_POST_OBJECT`,
  `prte_event_active`. The global data — trackers, `context_id`, xcast
  counters — is only touched inside those handlers. The caddy's event
  member must be named `ev`.
- **RML receives are persistent**, registered in `prte_grpcomm_init()` and
  cancelled in `prte_grpcomm_finalize()`: `PRTE_RML_TAG_XCAST`,
  `..._XCAST_ACK`, `..._FENCE`, `..._FENCE_RELEASE`, `..._GROUP`,
  `..._GROUP_RELEASE`.
- **`xcast` returns on acceptance, not completion.** If you need to know
  the whole DVM received a broadcast, use `xcast_nb` with a callback.
- **Non-destructive to the caller's buffer.** `xcast`/`fence` copy the
  payload; the caller keeps ownership.
- **xcast ordering is a correctness invariant, not a nicety.** The
  `process_first` set, the late-joiner catch-up and the promotion replay
  hold are what keep the reliable broadcast correct across DVM
  grow/shrink/unheal. Read the in-file comments before changing any of it;
  a mistake here manifests as `PRTE_ERR_OUT_OF_ORDER_MSG` force-exits or
  silent message loss during recovery.
- **A collective's own state is copied, never borrowed.** The arrays a
  caller hands an entry point belong to the PMIx server that delivered the
  upcall, which frees them once the completion callback fires. The caddy
  may point at them for the length of the handler, but anything that
  outlives it (a signature, a tracker) has to hold a copy. Both halves of
  that rule have been broken historically.
- **Every entry-point handler owns its caddy/op — release it on *all*
  paths.** The tracker caches only `cbfunc`/`cbdata`, never the caddy, so
  the handler is the last owner. Historic leaks here — `begin_xcast` never
  releasing the initiating `op_t`, the `group()` success returns never
  releasing `cd`, `fence_recv` never freeing its unpacked `info` — were all
  "return added, free forgotten" mistakes. Trace each new `return` back to
  what it strands.
- **Unpack failures must `return`, not fall through.** The recv handlers
  unpack a signature into a NULL-initialised pointer and the code
  immediately below dereferences it. Logging without returning turns a
  corrupt RML message into a daemon crash.
- **Signatures must round-trip exactly.** Fence trackers match on a
  byte-for-byte `memcmp`; group trackers on groupID + op. If you add a
  field, update *both* pack/unpack and constructor/destructor, or trackers
  fail to coalesce (hang) or leak.
- **Fence and group share one recovery epoch, advanced in one place** —
  `prte_grpcomm_fault_handler()` in `grpcomm.c`. Do not give a collective
  its own counter: the restart is only safe because it is simultaneous,
  and two counters racing to advance would break exactly that.
- **Group-FT code must stay behind `#if PRTE_PMIX_HAVE_GROUP_FT`** (from
  `PRTE_CHECK_PMIX_CAP([GROUP_FT])`), so PRRTE still builds against a
  pre-FT PMIx. The fault-handler abort loop itself is *unguarded*; the
  `PMIX_GROUP_CANCEL` handling is guarded — preserve that split.
  **The wire format is the exception**: the signature's `ft_collective`
  field is packed and unpacked unguarded, so every daemon in a DVM agrees
  on the message layout no matter what its PMIx advertised. Guard the
  *behaviour*, never the bytes.
- **The trackers' `pmix_bitmap_init(&reported_slots, 1)` is deliberately
  unchecked.** `pmix_bitmap_set_bit()` grows the bitmap on demand, and an
  `init` that fails leaves `array_size` at zero behind a NULL pointer, so the
  first slot recorded allocates and every accessor before that reads zero -
  which is the right answer for a tracker nothing has reported to. A
  constructor cannot fail anyway; do not add an abort here.
- **One allocator per array.** The proc arrays on a signature are built
  with `PMIX_PROC_CREATE` and freed with `PMIX_PROC_FREE` everywhere —
  those allocate and free *inside the PMIx library*, so a plain `free()`
  crosses the library boundary.
- **Never call `PMIx_Info_list_convert()` and reach for the array without
  reading the status.** An empty list — the ordinary case for a construct
  carrying no `PMIX_GROUP_INFO` and no endpoints — answers
  `PMIX_ERR_EMPTY`, and on that early return PMIx is under no obligation to
  have touched the `pmix_data_array_t` you handed it. Callers here pass an
  *uninitialised stack* variable, so an unchecked call reads whatever was on
  the stack for `.array`/`.size`, packs that many `pmix_info_t` from that
  pointer, and then hands the same pointer to `PMIX_DATA_ARRAY_DESTRUCT`.
  Current PMIx master initialises the argument before its first failure
  return, but no *released* PMIx in the supported range does, and depending
  on that is depending on the callee to clean up after the caller. Go
  through `convert_info_list()` in `grpcomm_group.c`, which initialises the
  array itself and answers an empty one on any failure, so what comes back
  is always safe to pack from and to destruct.
- **Three numbering schemes meet in `grpcomm_xcast.c`, and the mixture is
  deliberate.** The `DIRECT_XCAST_PACK`/`_UNPACK` packers hand back
  `PMIx_Data_pack`'s status, so `pack_sig`, `pack_msg`, `pack_relay_msg` and
  `pack_forward_msg` answer in **PMIx** statuses; every `PRTE_RML_SEND*`
  answers in **PRTE** codes. A function that does both — `send_ack_msg()`
  is the clearest — needs two checks logged through two decoders, and
  collapsing them into one "tidier" test logs half its failures against the
  wrong table. `process_wireup()` looks worst of all, testing
  `prte_util_decode_nidmap()` against `PMIX_SUCCESS` and
  `prte_util_decode_job_catchup()` against `PRTE_SUCCESS` two lines apart:
  both are right, because those two functions genuinely answer in different
  numbering. Check the callee before making them agree.
  The entry points themselves answer in **PRTE** codes, as the API table
  above says — callers log them with `PRTE_ERROR_LOG` and at least one
  (`pmix_server_monitor.c`) runs the result back through
  `prte_pmix_convert_rc()`, which mistranslates a PMIx status.
- Standard PRRTE rules: `prte_config.h` first, constant-on-left, braces
  everywhere, `PMIX_ERROR_LOG`/`PRTE_ERROR_LOG`, no new warnings.

---

## Testing

A structural unit test lives at
[`test/unit/grpcomm/test_grpcomm.c`](../../test/unit/grpcomm/) and is
wired into `make check`. It cannot drive a real collective (that needs a
live DVM) but it guards the invariants that hold with no DVM:

- `prte_grpcomm_release_bcast` defaults to the real broadcast. Nothing
  else establishes that, and a NULL or wrongly-aimed default would
  silently drop every release a controller emits — a DVM-wide hang with no
  diagnostic.
- `prte_grpcomm_globals.context_id` starts at `UINT32_MAX`, and every
  signature/tracker/caddy class constructs with the documented defaults
  and destructs without leaking or crashing.

### The pool has a second consumer, and it is not in this directory

A group formed by `PMIx_Group_invite` runs **no collective at all** — it is
realized entirely through PMIx event notification, so nothing ever reaches
`prte_grpcomm_group()` and there is no signature to carry an `assignID`. Its
leader asks for a context id through `PMIx_Job_control` instead, which lands
on whichever daemon hosts that leader; see `assign_group_ctxid()` in
[`src/prted/pmix/pmix_server_job_ctrl.c`](../prted/pmix/pmix_server_job_ctrl.c)
and the `PRTE_PMIX_GROUP_CTXID` relay beside it.

That is why the pool is now reached through `prte_grpcomm_assign_context_id()`
rather than touched directly: the two paths spend from one counter, and the
accessor is where "only the master may mint" is enforced once instead of at
each caller. A leader is an application process and sits wherever it was
mapped, so the job-control path is usually *not* on the master and has to
relay — which is the half a single-host run cannot reach.
`contrib/dockerswarm/groupinv.c` makes the highest rank the leader for exactly
that reason.
- **Building a fence tracker**: what a signature naming the daemon job, a
  signature naming nobody, and an unresolvable signature each produce —
  the last of which must leave *nothing* on the tracker list.
- **Parsing a group's directives**: that the signature ends up owning
  copies of the proc arrays, so the caller's arrays survive the
  signature's destructor intact (the test asserts on the *pointers*, which
  is the only way to see the difference before something double-frees).
- **Daemon-failure decisions**: the departed-member predicate, and the
  fence fault handler's choice between re-converging a fence that merely
  lost a message path and ending one that lost a participant. The test
  stands the failed-daemon set and job map up by hand and stubs
  `prte_grpcomm_release_bcast` to capture the release the controller would
  emit. That is where to pin a change to the *policy*; the recovery itself
  needs the dockerswarm harness.

Cases that reach at internals are compiled only when
`PRTE_TEST_GRPCOMM_INTERNALS` is set — grpcomm is compiled straight into
`libprrte` now, so the only thing that can put them out of reach is
`-fvisibility=hidden` (`PRTE_HIDE_INTERNALS`).

The live collectives are covered by the
[dockerswarm harness](../../contrib/dockerswarm/AGENTS.md): `xcast` and
`fence` fall out of nearly every phase, and `test_grpcomm` drives
`PMIx_Group_construct`/`_destruct` and the fence FT cases across the
node containers with the `groupcon` client. That phase exists because
`grp_release()` runs on **every** daemon, and a daemon that merely
*received* the down-tree broadcast — rather than originating it as the
HNP — does not exist on one host.

---

## Debugging

```sh
prte --prtemca grpcomm_base_verbose 5 ...   # trace collective progress
prte --prtemca plm_base_verbose 5 ...       # daemon launch / xcast of launch msg
prte --prtemca state_base_verbose 5 ...     # job-state transitions that drive fences
prte --prtemca rml_base_verbose 5 ...       # routing-tree (children/parent) view
```

Verbosity ≥1 narrates each `xcast`, `fence` and `group` call and its
rollup counts (`nexpected` vs `nreported`); ≥5 adds per-child relay/ack
traffic. Because collectives ride the routing tree, a "collective hang" is
almost always a routing-tree problem — check the RML and the fault
handlers first.

Note that `prte --daemonize` detaches the HNP's stderr, so verbosity given
that way goes nowhere you can read. Use `prterun` for a foreground DVM, or
`--leave-session-attached` with a hand-started daemon.
