# AGENTS.md — `src/runtime/data_server`

Orientation for AI agents and human contributors. Read
[`../AGENTS.md`](../AGENTS.md) first for the runtime's object model and
ownership rules, and the top-level [`AGENTS.md`](../../../AGENTS.md) for the
project rules. When this file and the docs disagree, **the docs win**.

---

## What this is

The data server backs PMIx's publish/lookup/unpublish service — the
key/value rendezvous MPI applications use for `MPI_Publish_name` and
friends. It is a **single store on one process** (normally the HNP) that
every other participant reaches over the RML.

```
app proc ──PMIx──▶ its prted ──RML(PRTE_RML_TAG_DATA_SERVER)──▶ HNP
                                                                 │
                                                       prte_data_server()
                                                                 │
                          ┌──────────────┬──────────────┬────────┴─────┐
                     ds_publish     ds_lookup     ds_unpublish     ds_purge
                                                                 │
app proc ◀─PMIx── its prted ◀──RML(PRTE_RML_TAG_DATA_CLIENT)─────┘
```

| File | Role |
|------|------|
| `prte_data_server.h` | The public surface: init/finalize, the RML receive callback, and the four command codes. |
| `ds.h` | The internal objects — `prte_data_object_t` (one published item), `prte_data_req_t` (one parked lookup), `prte_ds_info_t`, and the `prte_data_store` singleton. |
| `ds_main.c` | `prte_data_server()` — the RML receive that unpacks the room number and command and dispatches; the access check and the two range checks; all the class instances. |
| `ds_publish.c` | Store an item, then satisfy any parked lookups it answers. |
| `ds_lookup.c` | Answer from the store, or park the request if the caller asked to wait. |
| `ds_unpublish.c` | Remove the caller's own items by key. |
| `ds_purge.c` | Remove everything a (departing) process owns. |
| `ds_relay.c` | Reissue a request to an **external** data server — one living in another DVM — over a PMIx tool connection, and answer the requesting daemon when it replies. |

State is `prte_data_store`: a `pmix_pointer_array_t` of published objects
plus a `pmix_list_t` of parked requests. There is no locking — everything
runs on the PRRTE progress thread, inside the RML receive.

---

## The answer-buffer contract

This is the single easiest thing to get wrong here, so it is written down.

`prte_data_server()` creates `answer`, packs the room number and the command
into it, and hands it to a sub-handler. From that point:

> **Returning `PMIX_SUCCESS` means the handler has disposed of `answer`** —
> sent it, or released it. **Returning anything else means the handler left
> it alone**, and `prte_data_server()` packs the error into it and sends it.

Both halves matter. `prte_ds_lookup`'s wait path returns without sending
anything (the request is parked until a publish satisfies it); it therefore
has to *release* the buffer, or every waiting lookup leaks one. And a
handler that has already sent the buffer must not return an error, or the
caller sends a freed buffer.

The room number leads every reply because it is how the requesting PMIx
client matches an answer to its outstanding request. A reply that omits it,
or that carries a status without the payload the status implies, wedges the
client rather than failing it.

A related trap, seen in three of these files: `PMIx_Data_pack(NULL, buf,
&rc, 1, PMIX_STATUS)` assigned back to `rc` packs the right value but then
discards it, so the error being reported is replaced by the pack's own
status. Keep the two in separate variables.

---

## An external data server — the store in another DVM

`prte_pmix_server_uri` names a data server living in a **different DVM**, so
that jobs launched by different invocations can find each other's data
(`MPI_Publish_name` / `MPI_Comm_accept` across `mpirun`s). When it is set,
this DVM stores nothing of its own: `prte_data_server()` hands every request
straight to `prte_ds_relay()`.

**It is not reachable over the RML, and no amount of work here would make it
so.** The RML addresses a peer by *rank*, stamping the sender's own namespace
on it (`rml_send.c`, `oob_base_stubs.c`), so a daemon of another DVM cannot be
named at all — the namespace parsed out of the server's URI was simply
discarded and the request went to the local master. The crossing is therefore
a PMIx **tool** connection: the DVM master attaches to the remote DVM's PMIx
server (`init_server()` in `../../prted/pmix/pmix_server_pub.c`) and reissues
the operation as an ordinary `PMIx_Publish`/`Lookup`/`Unpublish`, which the
remote DVM's own upcalls deliver to its data server.

Three things follow, and each is load-bearing:

- **Only the master attaches.** Every other daemon relays to it over the RML
  exactly as it does for a local store, so a DVM holds one connection however
  many daemons it has. At the master itself the RML send is a send to self.
- **The requesting process is carried in `PMIX_REQUESTOR`, and its uid and
  gid in `prte.pub.ruid` / `prte.pub.rgid`.** Otherwise the far end
  attributes the operation to the relaying daemon's *tool* identity, and
  every ownership rule — who may unpublish, what `PMIX_RANGE_NAMESPACE`
  admits, what a job-end purge takes — is answered about the wrong process.
  The uid half matters for the same reason and is newer: PMIx appends the
  *relay's* `PMIX_USERID` and `PMIX_GRPID` to the call the relay makes, so
  without a separate claim the far end would store the item under the
  relaying daemon's user and test every later removal against it. They are
  PRRTE-private keys rather than a second `PMIX_USERID` precisely because
  PMIx adds its own, and two entries under one key make the answer depend on
  array order. `prte_ds_check_requestor()` reads all three in one pass and
  honors them **only from a tool**; an application process making the claim
  is trying to act under a peer's identity, and it is dropped. Call it
  *after* your own directive scan, or the relay's `PMIX_USERID` lands on top
  of the claim.
- **The primary server must be named per operation.** PMIx sends a tool's
  client-side call to whichever attached server is currently primary, and a
  master may also be attached to a scheduler. `prte_pmix_set_primary_server()`
  is the one place that designates one; call it immediately before the PMIx
  call, never once at startup.

The purge command carries a directive array for this reason — it is the only
way `PMIX_REQUESTOR` can reach `ds_purge`. Three senders pack it
(`pmix_server_unpublish_fn`, `state_dvm.c`, `state_base_fns.c`) and one reader
unpacks it; they change together.

---

## Who may read, and who may remove

Two different questions, and conflating them is what went wrong here
before. **Reading** is decided by the publisher's access permissions and
then by the range. **Removal** is decided by ownership alone.

### Retrieval: permissions first, then range

`prte_data_server_check_access()` is the first gate. Absent an accessor
list, published data belongs to its publisher: the requestor must present
the publisher's own uid **and** gid. A publisher that names a list —
`PMIX_ACCESS_USERIDS`, `PMIX_ACCESS_GRPIDS`, either inside a
`PMIX_ACCESS_PERMISSIONS` array or at the top level — replaces that
default, and each list it gives is a **requirement**, not a grant: a
requestor must satisfy every list present. (So a publisher whose own uid is
absent from its own `PMIX_ACCESS_USERIDS` cannot look its own data up
either. It can still unpublish it — see below.) A refusal is
`PMIX_ERR_NO_PERMISSIONS`, and `ds_lookup` reports that status rather than
`NOT_FOUND` when the key existed and only permission was lacking.

A restriction the publisher gave and we cannot parse **fails the publish**.
Storing it anyway would store the data unrestricted, which is the one
outcome nobody asked for.

This depends on PMIx handing us both ids. The library appends `PMIX_USERID`
and `PMIX_GRPID` to the info array of every publish, lookup and unpublish —
the gid took an openpmix change to be handed over at all (it comes from the
peer's connection record, not from the message), and where it is missing
both sides read `UINT32_MAX` and the rule degrades to uid-only rather than
locking everyone out.

### Then the range, in both directions

The PMIx retrieval rules apply the range test twice: the requestor must
fall within the range the *publisher* named, and the publisher must fall
within the range the *requester* named. One predicate answers both —
`range_admits()` in `ds_main.c`, "does `subject` fall within `range` as seen
from `anchor`" — and the two entry points differ only in which process
plays which role:

| Entry point | Range applied | Anchor → subject |
|-------------|---------------|------------------|
| `prte_data_server_check_range()` | `data->range` | publisher → requestor |
| `prte_data_server_check_search_range()` | `req->range` | requestor → publisher |

| range | Admits the subject when |
|-------|-------------------------|
| `SESSION`, `GLOBAL`, `UNDEF` | always |
| `NAMESPACE` | it shares the anchor's namespace |
| `LOCAL` | it sits behind the anchor's daemon (`req->proxy` vs `data->proxy`) |
| `PROC_LOCAL` | it *is* the anchor |
| `RM` | it is the host environment — our own namespace |
| `CUSTOM` | the publisher's accessor list admits it |

`CUSTOM` is the one range that is not a relation between two processes: the
accessor list *is* the range, so `check_range` answers it directly — admit
when a list was given (the access check has already applied it), refuse when
the publisher named nobody, since there is no other reading of a custom
range with no custom in it. In the requester's direction there is no list to
consult and `range_admits` refuses it.

The requester's half used to be missing entirely: `ds_lookup` unpacked
`PMIX_RANGE` and put it only on a *parked* request, where nothing read it,
so a lookup that asked to search its own namespace searched everything its
publishers would let it see. Both checks now run in `ds_lookup` and in
`ds_publish`'s pending-request loop, and the default on both sides is
`PMIX_RANGE_SESSION` — the constructors say so, which is why `rqcon` sets a
range at all.

### Removal: ownership, and nothing else

`ds_unpublish` applies neither rule. **An owner may unpublish what it
published on any range**, and the range the unpublish itself names does not
narrow that.

**The owner is the publishing USER**, not the publishing process:
`prte_data_server_owns()` compares the requestor's effective uid against the
one recorded at publish, and the gid where both are known (either side
reading `UINT32_MAX` degrades the test to uid alone, exactly as the read
rule does). The same predicate scopes `PRTE_PUBLISH_REPLACE`.

It used to be the process — namespace *and* rank — which sounds stricter and
is, in the way that made it useless: a process takes no data with it when it
exits, so an item published by a job that had ended was removable by nobody
at all. Its own user's next job could read it, could not publish over it
(that is a duplicate) and could not remove it, so the name was wedged for
the life of the DVM. A predecessor that *died* before it could unpublish is
the case that matters, and it is the one a checkpoint/restart handover hits.
Keying on the user moves no boundary that matters: nothing here crosses
between users, and there is still no administrative override.

**Ownership is not access, in either direction.** A `PMIX_ACCESS_USERIDS`
list widens who may *read* an item and confers no removal; and the sharper
case, a publisher whose own accessor list excludes it may still remove what
it cannot read, because it owns it. `prte_data_server_owns()` reads
`data->uid` and `data->gid` and never touches `auids`/`agids`, which is what
keeps those two questions apart.

Gating removal on the read rule is exactly the confusion this section
exists to prevent: it left a `PMIX_RANGE_RM` or `PMIX_RANGE_CUSTOM` item
impossible to remove — the owner falls outside its own item's range — while
still answering `PMIX_SUCCESS`, so the item sat in the store until its job
ended.

`data->proxy` and `req->proxy` are what the `LOCAL` rule compares, and
`PMIX_NEW` does not zero its allocation, so both constructors have to
`PMIX_PROC_CONSTRUCT` them. They did not, and the `LOCAL` check was reading
uninitialized memory.

---

## Duplicate keys: same key, same *set of processes*

`ds_publish` refuses a key that is already published on the range being
published to, with `PMIX_ERR_DUPLICATE_KEY` and nothing stored. The Standard
requires that, and the alternative is worse than it sounds: `ds_lookup`
answers a key from the **first match it finds**, so a stored duplicate is
unreachable — a write that reported success and did nothing.

Nor is the loser reliably the newcomer. The store is a
`pmix_pointer_array_t` and `pmix_pointer_array_add()` fills the **lowest
free slot**, so a duplicate landing in a slot that some earlier unpublish
freed sits *ahead* of the original and displaces it instead. Which value a
lookup resolves to was a function of unrelated publish/unpublish history.

**"Same range" is a set of processes, not the `pmix_data_range_t` word.**
`PMIX_RANGE_NAMESPACE` published by two processes of different namespaces
names two disjoint sets; refusing the second would refuse a publish the
Standard permits. So `same_data_range()` tests the range word **and** asks
whether the new publisher could itself have looked the stored item up
(`prte_data_server_check_access` then `prte_data_server_check_range`) —
which for `NAMESPACE`, `LOCAL` and `PROC_LOCAL` is exactly set equality, is
trivially true for `SESSION`, `GLOBAL` and `RM`, and separates two users'
identically-keyed items because neither can see the other's.

The scan runs in two passes and the order is the point: `count_duplicates()`
decides, `drop_prior()` acts. A publish that is going to be refused must
leave the store exactly as it found it.

The consequence worth knowing before you field a bug report: two *concurrent*
publishers of one name no longer both appear to succeed. The second is
refused, and since removal is a question of ownership, only the first can let
the name go. Sequential generations of a job are unaffected — the previous
one's `PERSIST_APP` data goes when it ends (see below) — but overlapping ones
must now handle `PMIX_ERR_DUPLICATE_KEY` where they used to get a write that
quietly went nowhere.

`PRTE_PUBLISH_REPLACE` (`"prte.pub.replace"`, in `prte_data_server.h`) lets
a publisher take back its **own** prior publication of those keys instead of
failing — otherwise updating a value you published yourself needs an
intervening `PMIx_Unpublish`. It is deliberately owner-scoped: if any
colliding item belongs to somebody else the publish is refused whether or
not the directive was given, so it is a republish and never a way to take a
live name away. Only the republished keys go; an object holding others keeps
them.

"Own" here is the same `prte_data_server_owns()` the removal rule uses — the
publishing **user**. It has to be: once a same-uid process may unpublish a
key and then publish its own, the two-step is available anyway, and refusing
the one-step form would only make the same outcome take two calls.

Everything those checks read — the owner (which `PMIX_REQUESTOR` may have
replaced), the range, the uid and gid — is final only *after* the directive
scan, so the gate has to sit between that scan and
`pmix_pointer_array_add()`.

---

## Persistence and parked requests

`PMIX_PERSIST_FIRST_READ` removes an item from `data->info` as soon as it is
returned. Both `ds_lookup` (returning from the store) and `ds_publish`
(satisfying a parked request) implement it, and both then have to notice
that an object whose `info` list is now empty must leave the store —
`ds_lookup` did not, so an item read by an ordinary lookup stayed in the
store as an empty shell, matching nothing and removed only by a purge.
`data` there is the loop variable of the scan over the store, so nothing may
touch it after the drop; the `break` is what makes that safe.

A lookup carrying `PMIX_WAIT` that cannot be fully satisfied is parked on
`prte_data_store.pending` with **only the keys it is still missing**. When a
later publish resolves the rest, `complete_resolved` is what says the
request is finished — do not re-derive it by counting `req->keys`, because
that array has already been swapped for the unresolved remainder.

A `PMIX_TIMEOUT` given with the wait bounds it: the parked request carries
an event armed for that many seconds (`lookup_timeout` in `ds_lookup.c`),
which takes it off `pending` and answers `PMIX_ERR_TIMEOUT` with no payload.
Anything that disposes of the request earlier — a satisfying publish, a
purge, finalize — releases it, and the destructor is what disarms the
timer, so every path is covered by `PMIX_RELEASE`. Without the timer a wait
for a key nobody publishes never returned: the timeout reached the daemon's
caddy (`req->timeout` in `pmix_server_pub.c`) and went no further.

**`PMIX_PERSISTENCE` is enforced by the purge, and only because the state
machine says a lifetime ended.** `expires_by()` decides what a given
horizon takes:

| horizon | removes |
|---------|---------|
| `INVALID` (an explicit unpublish-all) | everything the target owns |
| `PROC` | `PROC` |
| `APP` | `PROC`, `APP` |
| `NSPACE` | `PROC`, `APP`, `NSPACE` |
| `SESSION` | `PROC`, `APP`, `NSPACE`, `SESSION` |

The values are *not* a numeric ladder — `PMIX_PERSIST_INDEF` is 0 and
outlives all of them, and `PMIX_PERSIST_NSPACE` is 5 while ending before
`SESSION`'s 4 — so the ordering is spelled out rather than compared.

Two of the horizons need something a `pmix_proc_t` cannot say. An
**application** is not a process and not a namespace either: every app of a
job shares the one namespace assigned to the job, so the target names them
all and `prte.purge.appidx` says which one ended. A **session** outlives the
jobs that ran in it and their job objects, so its target admits anybody (an
empty namespace is a wildcard to `PMIX_CHECK_PROCID`) and `PMIX_SESSION_ID`
selects. Each item records both at publish, in `app_idx` and `session_id`,
because neither is derivable later — the purge arrives after the publisher
has gone.

`FIRST_READ` is in that list of what no horizon takes, and it took
issue #2733 to get there. Its criterion is the first access and nothing
else, so an item published for a reader that has not started yet is not
the publisher's to lose when the publisher's job ends — `PROC` and `APP`
are how a publisher says "remove this when I go away". Purging it made
the one conforming handover between successive generations of a job
impossible: the successor found nothing. The cost of not purging it is an
unread item that nothing expires on its own, which is what `INDEF`
already carries; a retention timeout for both is planned (see
`docs/plans/datastore/`).

A purge with **no** horizon (`PMIX_PERSIST_INVALID`) means an explicit
`PMIx_Unpublish(NULL, ...)`: a live publisher taking back everything it
published, regardless of persistence. That case must *not* drop the
requestor's parked lookups — cancelling the lookups a process is waiting on
is no part of taking its published data back.

**There is a store on every daemon, not just the master.**
`pmix_server_start()` calls `prte_data_server_init()` unconditionally, and it
runs in `ess/hnp` and `ess/base/ess_base_std_prted` alike. What decides which
store a request reaches is the RANGE, in `execute()`
(`src/prted/pmix/pmix_server_pub.c`) — one routing decision, no local-first
search and no fallback:

| Range | Target |
|-------|--------|
| `PMIX_RANGE_SESSION` | the global server (`prte_pmix_server_globals.server`; the HNP relays when it is external) |
| `PMIX_RANGE_LOCAL` | `PRTE_PROC_MY_NAME` — **this daemon's own store** |
| everything else | `PRTE_PROC_MY_HNP` |

So a prted's store holds *only* local-range items published by its own local
procs, and a lookup that misses gets `PMIX_ERR_NOT_FOUND` rather than trying
somewhere else. On the master the two stores are the same object, which is
why a single-node run cannot tell them apart — and why the purge going only
to the global store left local-range data unreclaimed until it also went to
`PRTE_PROC_MY_NAME`.

**A purge is a call, not a message.** The store that has to act is in every
case one the acting process already holds, so `state/base` offers four
entry points and each is called by whoever can see that lifetime end:

| call | called by | when |
|------|-----------|------|
| `prte_state_base_purge_proc` | the master, and the daemon that hosted it | a process terminates (both `track_procs` implementations) |
| `prte_state_base_purge_app` | the master | an app's terminated count reaches its `num_procs` |
| `prte_state_base_purge_nspace` | the master; every daemon | the job ends (`state_dvm.c`); `PRTE_DAEMON_DVM_CLEANUP_JOB_CMD` |
| `prte_state_base_purge_session` | the master | `prte_ras_base_teardown_reservation()` |

Each purges **its own** store. That is what makes the `PROC` horizon
affordable — it fires once per terminating process, and a store nothing was
ever published into is an array of one empty slot, so the scan costs
nothing. The **only** thing that still needs a message is an *external*
data server, which lives in another DVM behind the PMIx tool connection the
master holds; a daemon never relays, since its own store is local-range
data that never leaves this DVM.

Two things that had to be got right here, both of which were once wrong:

- **A daemon's share of a job finishing is not a lifetime ending.**
  `state_prted.c`'s `job_teardown()` runs when the procs of a job that
  *this daemon hosted* have terminated (`num_terminated ==
  num_local_procs`), which says nothing about the rest of the job. It used
  to send the DVM-wide purge from there, so the first node to finish its
  share purged the entire namespace's `APP` and `PROC` data out of the
  **master's** store while other nodes were still publishing into it. It
  now sends nothing: each of its procs was purged at the `PROC` horizon as
  it died, and the namespace horizon arrives from the master.
- **The purge used to be gated on `prte_pmix_server_globals.server.nspace`
  being set** — "nobody local to us has used the data server" — which is
  true of *our* store and false of the global one, whose publishers are
  anywhere in the DVM. On the master that guard held exactly when the
  publishers ran on other nodes, which is the arrangement that most needs
  the purge. It went unnoticed only because every daemon was sending the
  DVM-wide purge as well.

All three call sites were once gated on `NULL != prte_data_server_uri`, so
the **built-in** data server — the usual case — was never told a job had
ended at all; nothing was reclaimed from it short of the DVM shutting down,
and `PERSIST_APP` and `PERSIST_PROC` both behaved as `PERSIST_INDEF`.

**Two persistences name no lifetime, and a timeout is what bounds them.**
`PMIX_PERSIST_INDEF` is "retained until specifically deleted" and only its
publisher may delete it, so in a DVM that outlives the publisher it is a
permanent allocation made by a process that no longer exists;
`PMIX_PERSIST_FIRST_READ` is the same shape when the read it waits for never
comes. `prte_data_server_timeout` (default 300 s) removes either once it has
been **idle** that long — `last_access` is stamped at publish and restamped
by every lookup that returns one of the item's keys, in *both* places that
answer a lookup (`ds_lookup.c`, and `ds_publish.c` where a publish satisfies
a parked request).

Idle rather than a lifetime, deliberately: a rendezvous name in active use
must not be pulled out from under its readers. Nothing else is swept — a
persistence that names a lifetime has a criterion a running system reaches,
and cutting it short would break the retention its publisher was promised
while it is still alive to rely on it.

One sweep event serves the whole store (`ds_purge.c`), armed only while the
store holds something the timeout applies to and disarmed when it does not.
The interval is `timeout / 4`, bounded to [1 s, 60 s], which is what makes
"removed no earlier than the timeout, and normally within a sweep interval
after it" a bound worth stating. A timer per item would be exact, at the
cost of an armed libevent timer per published item and a re-arm on every
read; the parked-lookup timeout in `ds_lookup.c` is per-request because a
request is a one-shot with a caller waiting on it, which is not this case.

## The cap: per user, and nobody else's data

Retention alone leaves the store unbounded — a job publishes what it likes
and exits, and what it published outlives it. `prte_data_server_max_size`
(default 16 MiB) bounds that, **per publishing uid**, and eviction never
crosses a uid boundary: a user who floods the store evicts only their own
data. Without that scoping the cap is an abuse primitive in its own right,
since publishing junk in bulk becomes a way to push somebody else's
rendezvous name out.

`prte_ds_usage_t` holds one running byte total per uid, on a list in the
store — as many records as there are users publishing here, which is a small
number. Three rules keep it honest:

- **Every removal goes through `prte_ds_drop()`.** There are seven paths —
  the duplicate drop, an unpublish, a `FIRST_READ` read that empties an item
  (in *both* places that answer a lookup), each purge horizon, the expiry
  sweep, and eviction itself — and one that forgets to uncharge leaves a uid
  unable to publish anything ever again. That is why it is one function and
  not a line repeated seven times.
- **Every shrink calls `prte_ds_charge()`.** An item that loses a key to a
  `FIRST_READ` read is smaller than what its publisher is charged for.
- **The cap gate runs last**, after the duplicate scan and the directive
  scan, immediately before `pmix_pointer_array_add()`. It is the only gate
  that *modifies* the store, so a publish that is going to be refused must
  not have cost anybody their data on the way — which is also why a publish
  larger than the whole cap is refused before anything is evicted, rather
  than emptying the user's store and failing anyway
  (`PMIX_ERR_OUT_OF_RESOURCE`).

Eviction takes the uid's own least-recently-used item, by the same clock the
retention timeout reads, and ignores persistence — a user's own data is that
user's own to budget. A per-uid list in LRU order would make the choice
O(1); it is deliberately not built, because the store is a
`pmix_pointer_array_t` whose objects are reached by index on several paths
and a second membership is another thing every removal path has to keep in
step. The scan runs only when a uid is at its cap. If a store is ever
*measured* spending real time there, the list is the answer.

The first eviction for a uid emits a `show_help` warning
(`help-prte-data-server.txt`) naming the limit and the parameter that raises
it: eviction is the store protecting itself, not a policy anyone asked for.
A reader whose item was evicted gets `PMIX_ERR_NOT_FOUND` and cannot tell
that from a key nobody published — which is deliberate, since a publisher
cannot distinguish eviction from a reader that never arrived either.

## Persistence and the default

The object's default is `PMIX_PERSIST_NSPACE`, and PMIx adds none of its
own before handing a publish to the host, so the value in `ds_main.c`'s
constructor is the one that governs. It is deliberately **not** the
Standard's `PMIX_PERSIST_APP`: `APP` means the publishing process's
*application*, an MPMD job's applications need not end together, and
applying that default literally would shorten the retention every unmarked
publish has been getting — as a side effect of reading `APP` correctly.
`NSPACE` is that same lifetime, said out loud. A publisher that wants app
lifetime asks for `APP` and gets it.

A lifecycle purge drops both the departing process's published items *and*
any lookup it left parked; a request that outlives its requestor would
otherwise have a later publish trying to reply to a process that no longer
exists.

---

## The external server: attach, and answer

Two things about the relay path are easy to get wrong, and both were.

**A local-range request must not be relayed.** `prte_data_server()` used to
hand *everything* to `prte_ds_relay()` whenever `prte_data_server_uri` was set.
But `PMIX_RANGE_LOCAL` data belongs to the store of the daemon that relayed
it, so forwarding it stored a publish where its own publisher could never look
it up, and answered a local-range lookup out of a store that cannot hold
local-range data. By the time the dispatch runs, the range is buried in a
payload whose shape depends on the command — so the *sender* says which store
it means by choosing the tag, `PRTE_RML_TAG_DATA_SERVER_LOCAL` rather than
`PRTE_RML_TAG_DATA_SERVER`. `execute()` picks it for a local range, the
lifecycle purge picks it for our own store, and the receive is registered for
both.

**The master must attach even when nothing local asks it to.** The attach
(`init_server()`, now behind `prte_pmix_server_init_pubsub()`) used to happen
only inside `execute()` in `pmix_server_pub.c` — that is, only when a **local
client of that daemon** published or looked something up. But relaying is the
*master's* job: every other daemon sends its request to the master, which is
the only one holding the tool connection to the far end. A master with no
publishing client of its own therefore never attached, and every relayed
request failed `PMIX_ERR_UNREACH`. Nothing noticed for as long as each job's
nspace registration also published — that went through `execute()` and
attached as a side effect. Remove the accidental attach and the feature stops
working, which is exactly what happened. `prte_ds_relay()` now forces it.

**A relay that cannot take the request must still answer.** The dispatch in
`prte_data_server()` used to log a relay failure and `return`, sending
nothing. The daemon that asked stayed parked on its room number, and the
process behind it hung for good. An unreachable data server is something a
caller can be told about; a hang is not. The relay branch now falls into the
same error reply every other failure uses, and the contract is unchanged:
`prte_ds_relay()` returning `PMIX_SUCCESS` means it owns the request and will
answer — including when it has already sent a failure.

---

## Gotchas before you edit

- **The answer-buffer contract above.** Every new exit path has to say which
  side of it you are on.
- **`PMIX_RELEASE(req)` while `req` is still on `pending` leaves a freed
  item on the list.** Remove it first.
- **Success paths free too.** `ds_publish` unpacks a `pmix_info_t` array,
  copies what it keeps into the data object, and has to `PMIX_INFO_FREE` the
  array — that was happening only on the unpack-failure path, so a
  successful publish leaked.
- **A stack `PMIX_CONSTRUCT`ed object needs `PMIX_DESTRUCT` on *every*
  return.** `ds_lookup` and `ds_unpublish` both build a `prte_data_req_t` on
  the stack to carry the requestor into `check_range`.
- **An access rule is not an ownership rule.** See the section above: what
  may be read and what may be removed are different questions, and the
  answer to the first one refused owners their own data.
- **Decide before you remove.** The duplicate scan is two passes for a
  reason: a refused publish must leave the store untouched, so nothing may
  be dropped until every collision has been found and attributed.
- **A range word is not a range.** Two publications can carry the same
  `pmix_data_range_t` and still name disjoint sets of processes. Never
  compare `data->range` alone and call it "the same range".
- **A parked request can own an armed timer.** Remove it from `pending` and
  `PMIX_RELEASE` it; never `free` around it, and never leave the list
  holding one you have released.
- **Every exit from the dispatch must answer somebody.** A caller is parked
  on a room number; a path that returns silently is a hang, not an error.
  See the section above.
- **`rc` in `prte_data_server()` is a `pmix_status_t`.** Format it with
  `PMIx_Error_string()`; `PRTE_ERROR_NAME()` knows only PRRTE's codes and
  renders every PMIx status as "Unknown error".
- **No locking, and none is wanted.** Everything here runs inside the RML
  receive on the progress thread.
- **`prte_data_store.output` is `-1` until a verbosity is registered**, so
  `pmix_output_verbose` on it is a no-op. That is fine — do not "fix" it by
  reaching for `pmix_output(0, ...)`.

---

## Testing

**Unit — `test/unit/runtime/test_runtime.c`.** Both range checks against
every range value in both directions — `prte_data_server_check_range` from
the publisher's side and `prte_data_server_check_search_range` from the
requester's — and the object constructors' initialization contract,
including the `PMIX_RANGE_SESSION` default a request carries. Neither needs
the RML.

**Multi-node — `contrib/dockerswarm`, the `test_runtime` phase.** The store
is on the HNP and the clients are elsewhere, so the interesting paths only
exist with real daemons: a publish on one node found by a lookup on another,
the range and access rules between real processes, a namespace-scoped lookup
that must *not* reach another job's data, an accessor list that admits or
refuses a real reader (and is answered with `PMIX_ERR_NO_PERMISSIONS`),
unpublish — including an owner removing data published on a range it does
not itself fall within — and the `PMIX_WAIT` path, both where a later
publish satisfies it and where `PMIX_TIMEOUT` ends it. The `dataserver`
helper takes the publish range, the unpublish range and an access spec as
separate arguments for those cases.

The **external** data server is multi-node coverage by construction — the
whole point is a namespace boundary, and one DVM has none. `test_runtime`
stands up three DVMs at once (a server and two clients pointed at it) and
asserts that data crosses, that the answer names the publishing process
rather than the relay, that a parked `PMIX_WAIT` lookup in one client is woken
by a publish in the other, that an ended job's data is purged from the server
and that the purge takes *only* that job's data — plus the control, that a
DVM which was not given the URI sees none of it.

The swarm covers the timeout with `--prtemca prte_data_server_timeout` set
to a few seconds — no debug-only knob and no `PRTE_ENABLE_DEBUG` build. Note
that a `prun` takes seconds to get a process running, so a case that reads a
key repeatedly to show the clock restarting has to leave room for that in
both directions: gaps comfortably inside the timeout, and a last read
comfortably outside it, or the case passes without showing anything.

The `SESSION` horizon lives in the **`test_session`** phase rather than
this one, because that is where the tooling is: a reservation is
instantiated with the publisher running inside it (which is what gives its
data a session to belong to — the job carries `PRTE_JOB_SESSION_ID`), the
key is read back while the session stands, and it is gone once
`sessionctrl terminate` tears the reservation down. Its control is a key of
the *same* persistence published outside any reservation, which must
survive: what separates the two is the session id recorded on each item and
nothing else. Nothing but the session purge can remove either — no job-end
horizon takes `SESSION` data, and the sweep does not touch it — so the case
cannot pass by accident.

**Not covered:** anything that needs a second *user*. The harness runs as
one uid in every container, so what the swarm can show of the ownership rule
is the same-user half — a later job taking back its predecessor's name, and
replacing it in one publish. That a different uid is refused is unit-tested
(`test_data_server_ownership`) and nowhere else. Access permissions have the
same limit: the swarm proves that a list naming *somebody else* keeps us
out, not that a genuinely different uid gets in.

A note on the partial-lookup case, because it took both code bases to make
it work. PRRTE returning the status *and* the values it found is only half
of it: the PMIx client used to answer its lookup callback with
`(status, NULL, 0)` for anything other than `PMIX_SUCCESS`, so a correct
server still produced an empty answer. That is fixed upstream
(`pmix_client_pub.c`), which means the swarm case needs a PMIx built from
source — `PMIX_SRC=... ./build.sh` — to pass.
