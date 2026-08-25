Reaching a data server in another DVM
=====================================

What this is for
----------------

PRRTE's data server backs PMIx's publish/lookup service.  Normally the store
lives on this DVM's own master and every participant reaches it over the RML
— see `src/runtime/data_server/AGENTS.md
<https://github.com/openpmix/prrte/blob/master/src/runtime/data_server/AGENTS.md>`_.

``prte_pmix_server_uri`` asks for something else: that the store live in a
**different DVM**, so that jobs launched by *different invocations* can find
each other's data.  That is what an MPI application is doing when one
``mpirun`` calls ``MPI_Publish_name`` and another calls ``MPI_Comm_accept``.

What crosses is the data an application publishes, and only that.
``pmix_server_register_fns.c`` used to also publish each job's registration
info whenever an external server was configured, on the stated grounds that
"any subsequent connect has to be able to retrieve it" — but nothing ever
retrieved it.  Every ``PMIx_Lookup`` caller in PRRTE, PMIx and Open MPI takes
a key the user supplied, and no reader keyed by a namespace has existed at
any point in the history.  It could not have served that purpose in any case:
the key was ``prte_process_info.myproc.nspace``, the *daemon* job's namespace
and so one constant string per DVM, while the value was the registration info
for whichever job was being registered — so every daemon, and every job, wrote
a different payload under the one key, and a reader would have got an
arbitrary daemon's copy of an arbitrary job.  It is an ORTE-era carry-over,
from when cross-``mpirun`` accept/connect really did fetch the remote job's
info by name; under PMIx that travels through ``PMIx_Connect`` and the
server's own machinery.  The write has been removed.


Why it could not work over the RML
----------------------------------

The RML addresses a peer **by rank**, and stamps the sender's own namespace
on it.  Every send entry point takes a ``pmix_rank_t``
(``prte_rml_send_buffer_nb`` and its variants), ``send_buffer()`` builds the
destination as ``PMIX_LOAD_PROCID(&snd->dst, PRTE_PROC_MY_NAME->nspace,
rank)``, and ``prte_oob_base_send_nb`` resolves the next hop the same way.  A
daemon of another DVM is therefore not nameable: the namespace parsed out of
the server's URI was discarded and the request went to *this* DVM's rank 0.

That has been so since the March 2022 RML rework
(``17368b1b60``), which rewrote the one line that mattered::

    -    PRTE_RML_SEND(rc, target, xfer, PRTE_RML_TAG_DATA_SERVER);
    +    PRTE_RML_SEND(rc, target->rank, xfer, PRTE_RML_TAG_DATA_SERVER);

It is not something the transport can be talked into.  Below the API,
everything is indexed by rank within this DVM — ``prte_rml_is_node_up()``,
``prte_rml_get_route()``, the boot-epoch table — so a foreign rank collides
with a local one at every layer, not just in the wire header.  Two further
gaps made the feature unreachable even in principle: the reply was addressed
with ``sender->rank`` in the *server's* namespace, and nothing published a
URI in the form the OOB parses (``--report-uri`` emits the **PMIx** server
URI, ``nspace.rank;tcp4://host:port``, while ``process_uri`` wants PRRTE's
own ``tcp://ip:port:ifmask``).


The approach: a PMIx tool connection
------------------------------------

Rather than teach the RML to carry a second namespace, the crossing is made
where PMIx already supports one.  **The DVM master attaches to the remote
DVM's PMIx server as a tool** and reissues the operation as an ordinary
``PMIx_Publish``/``Lookup``/``Unpublish``.  The remote DVM's own upcalls then
reach its data server exactly as a local client's would.

This is not a new mechanism in PRRTE: ``prte_pmix_set_scheduler()`` already
attaches the master to a scheduler the same way.

Consequences, all of them wanted:

* **The RML and the wire header stay as they are.**  Nothing has to name a
  foreign process, so the OOB header can go on carrying one namespace — see
  the entry in :doc:`../../todo`.
* **The URI problem disappears.**  What ``PMIx_tool_attach_to_server`` wants
  under ``PMIX_SERVER_URI`` is exactly what ``prte --report-uri`` writes.
* **One connection per DVM.**  Only the master attaches; every other daemon
  relays to it over the RML, as it already does for a locally-hosted data
  server.

::

    app proc ──PMIx──▶ its prted ──RML(DATA_SERVER)──▶ this DVM's master
                                                              │
                                                     prte_ds_relay()
                                                              │
                                             PMIx tool connection
                                                              ▼
                                              the server DVM's master
                                                              │
                                            its own publish/lookup upcall
                                                              ▼
                                                   its data server


Two things the design has to get right
--------------------------------------

**Whose request is it.**  A relayed publish arrives at the far end as an
operation by *this daemon's tool identity*, so every item would be owned by
the relay rather than by the process that asked.  Ownership is not cosmetic:
it decides who may unpublish an item, what ``PMIX_RANGE_NAMESPACE`` admits,
and which items a purge takes when a job ends.  The requesting process is
therefore carried in ``PMIX_REQUESTOR``, which the PMIx Standard defines for
precisely this — *"used when relaying a request to the PMIx library on
behalf of someone else where the API doesn't include a requestor
parameter"*.

The far end honors it **only from a tool** (``prte_ds_check_requestor()``).
Relaying is what a tool does; an application process claiming to act for
another has no such standing, and allowing it would let any process publish —
and unpublish — under a peer's identity.

**Which server is primary.**  PMIx directs a tool's client-side calls at
whichever attached server is currently *primary*, and only one may be primary
at a time.  A master can hold two connections — a scheduler and a data server
— so an operation may not assume the primary is whatever the last operation
left in place.  ``prte_pmix_set_primary_server()`` is the single point that
designates one, and both the data-server relay and
``prte_pmix_set_scheduler()`` call it before every operation.  It is a no-op
when the named server is already primary, and it is race-free because the
PMIx client calls pack and send synchronously, on the same thread, before
returning.

That replaces the sticky ``scheduler_set_as_server`` flag, which recorded
"the scheduler has been made primary" once and would have been wrong the
moment a second connection existed.


What changed
------------

============================================  =====================================================
file                                          change
============================================  =====================================================
``src/prted/pmix/pmix_server_pub.c``          ``init_server()`` attaches as a tool instead of
                                              parsing an RML URI; only the master attaches;
                                              ``execute()`` sends to the master when a server is
                                              external; the purge command packs its directives
``src/runtime/data_server/ds_relay.c``        new — the relay itself
``src/runtime/data_server/ds_main.c``         dispatch to the relay; ``prte_ds_check_requestor()``
``src/runtime/data_server/ds_*.c``            honor ``PMIX_REQUESTOR`` in the directive scan
``src/prted/pmix/pmix_server_allocate.c``     ``prte_pmix_set_primary_server()``
``src/mca/state/{dvm,base}``                  purge senders match the new packing, and address
                                              the master when the server is external
============================================  =====================================================

The purge command's wire format gained a directive array.  That is allowed
without ceremony — the message is exchanged inside one DVM, whose processes
all come from one build — but both ends change in the same commit: three
senders (``pmix_server_unpublish_fn``, ``state_dvm.c``,
``state_base_fns.c``) and one reader (``ds_purge.c``).


How it is tested
----------------

Nothing about this exists on a single DVM, so the coverage is in
``contrib/dockerswarm``'s ``test_runtime`` phase: three DVMs at once — a
server and two clients pointed at it — asserting that a key published in one
client DVM is found in the other, that the answer names the **publishing
process** and not the relay, that a ``PMIX_WAIT`` lookup parked from one DVM
is woken by a publish from the other, that an ended job's data is purged from
the external server, that the purge takes **only** that job's data, and — the
control — that a DVM which was not given the URI sees none of it.
