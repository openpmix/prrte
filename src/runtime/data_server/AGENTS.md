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
| `ds_main.c` | `prte_data_server()` — the RML receive that unpacks the room number and command and dispatches; `prte_data_server_check_range()`; all the class instances. |
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
- **The requesting process is carried in `PMIX_REQUESTOR`.** Otherwise the far
  end attributes the operation to the relaying daemon's *tool* identity, and
  every ownership rule — who may unpublish, what `PMIX_RANGE_NAMESPACE`
  admits, what a job-end purge takes — is answered about the wrong process.
  `prte_ds_check_requestor()` honors the claim **only from a tool**; an
  application process making one is trying to act under a peer's identity, and
  the claim is dropped.
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

## Ranges — the access-control rules

`prte_data_server_check_range()` decides whether a requestor may see a
published item. It is the only access control here, so it is worth reading
before changing:

| `data->range` | Admits |
|---------------|--------|
| `SESSION`, `GLOBAL`, `UNDEF` | anyone |
| `NAMESPACE` | any rank of the publisher's namespace |
| `LOCAL` | requestors behind the same daemon (`req->proxy` vs `data->proxy`) |
| `PROC_LOCAL` | the publishing process itself |
| `RM` | our own (the host server's) namespace |
| `CUSTOM` | **nobody** — the accessor list is not implemented |

`CUSTOM` denying is deliberate, not an oversight in the tests: the function
falls through to `return PMIX_ERROR`, which is the safe direction for an
unimplemented rule.

Separately from the range, both publish and lookup compare `uid` — data is
only visible to the user that posted it. That check is in the callers, not
in `check_range`.

`data->proxy` and `req->proxy` are what the `LOCAL` rule compares, and
`PMIX_NEW` does not zero its allocation, so both constructors have to
`PMIX_PROC_CONSTRUCT` them. They did not, and the `LOCAL` check was reading
uninitialized memory.

---

## Persistence and parked requests

`PMIX_PERSIST_FIRST_READ` removes an item from `data->info` as soon as it is
returned. Both `ds_lookup` (returning from the store) and `ds_publish`
(satisfying a parked request) implement it, and both then have to notice
that an object whose `info` list is now empty must leave the store.

A lookup carrying `PMIX_WAIT` that cannot be fully satisfied is parked on
`prte_data_store.pending` with **only the keys it is still missing**. When a
later publish resolves the rest, `complete_resolved` is what says the
request is finished — do not re-derive it by counting `req->keys`, because
that array has already been swapped for the unresolved remainder.

`ds_purge` drops both the departing process's published items *and* any
lookup it left parked; a request that outlives its requestor would otherwise
have a later publish trying to reply to a process that no longer exists.

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
- **No locking, and none is wanted.** Everything here runs inside the RML
  receive on the progress thread.
- **`prte_data_store.output` is `-1` until a verbosity is registered**, so
  `pmix_output_verbose` on it is a no-op. That is fine — do not "fix" it by
  reaching for `pmix_output(0, ...)`.

---

## Testing

**Unit — `test/unit/runtime/test_runtime.c`.** `prte_data_server_check_range`
against every range value in both directions, and the object constructors'
initialization contract. Neither needs the RML.

**Multi-node — `contrib/dockerswarm`, the `test_runtime` phase.** The store
is on the HNP and the clients are elsewhere, so the interesting paths only
exist with real daemons: a publish on one node found by a lookup on another,
the range and userid rules between real processes, unpublish, and the
`PMIX_WAIT` path where a lookup parks until a later publish satisfies it.

The **external** data server is multi-node coverage by construction — the
whole point is a namespace boundary, and one DVM has none. `test_runtime`
stands up three DVMs at once (a server and two clients pointed at it) and
asserts that data crosses, that the answer names the publishing process
rather than the relay, that a parked `PMIX_WAIT` lookup in one client is woken
by a publish in the other, that an ended job's data is purged from the server
and that the purge takes *only* that job's data — plus the control, that a
DVM which was not given the URI sees none of it.

**Not covered:** `PMIX_RANGE_CUSTOM` (no accessor list exists to test),
`PMIX_PERSIST_FIRST_READ` end-to-end, and `ds_purge` (which is driven by
process termination rather than by a client call).

A note on the partial-lookup case, because it took both code bases to make
it work. PRRTE returning the status *and* the values it found is only half
of it: the PMIx client used to answer its lookup callback with
`(status, NULL, 0)` for anything other than `PMIX_SUCCESS`, so a correct
server still produced an empty answer. That is fixed upstream
(`pmix_client_pub.c`), which means the swarm case needs a PMIx built from
source — `PMIX_SRC=... ./build.sh` — to pass.
