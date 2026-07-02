.. _rml-label:

The Runtime Messaging Layer (RML)
=================================

The Runtime Messaging Layer is how PRRTE daemons talk to one another.  Every
control message that crosses the DVM — launch commands, I/O forwarding,
collective operations, PMIx data exchanges, fault notices — travels over the
RML.  It provides a single service: *deliver this buffer to that daemon, under
this tag*, asynchronously and in order per connection.

This page explains how the RML is put together and how a message flows through
it.  The code lives entirely in ``src/rml/``; a shorter, editing-oriented map
is in ``src/rml/README.md``.

Historical note: one directory, once three frameworks
-----------------------------------------------------

``src/rml`` was originally **three** MCA frameworks:

* **rml** — the messaging API,
* **routed** — pluggable routing-tree algorithms, and
* **oob** ("out of band") — pluggable transports, each with swappable modules.

PRRTE only ever had one RML, one routing algorithm (a radix tree), and one
transport (TCP), so the three frameworks were collapsed into the single
``src/rml`` directory.  The collapse merged the files but, for a long time,
left the multi-component *abstractions* in place — dead failover paths, a relay
macro nobody called, a routed-module name in every wire header, and comments
describing a world of many components, modules, and transports.  That
scaffolding has since been removed.  When you read the code today there is one
path: **RML API → radix routing → TCP transport**, plus a newer fault-tolerance
layer layered on top.

Architecture and key state
--------------------------

Two global structures hold essentially all RML state, and both are owned by the
PRRTE progress thread:

``prte_rml_base`` (``src/rml/rml.h``, defined in ``rml.c``)
    The messaging and routing state.  It holds the two matching lists —
    ``posted_recvs`` (receives the local code has posted) and
    ``unmatched_msgs`` (messages that arrived before a matching receive was
    posted) — plus the routing-tree state: the number of daemons ``n_dmns``,
    the ``failed_dmns``/``global_failed_dmns`` bitmaps, this daemon's
    ``ancestors`` and ``children`` in the tree, and its ``lifeline`` (immediate
    parent).

``prte_oob_base`` (``src/rml/oob/oob.h``, defined in ``oob_tcp.c``)
    The transport state: the list of known ``peers`` (each with its addresses,
    socket, and send queue), the local network interfaces, listener sockets,
    and the many TCP tuning MCA parameters (buffer sizes, keepalives, retry
    limits, ``max_msg_size``).

The message objects (``src/rml/rml_types.h``) are reference-counted PMIx
objects:

* ``prte_rml_send_t`` — an outbound message (destination, origin, tag, data
  buffer, retry count).
* ``prte_rml_recv_t`` — an inbound message waiting to be matched/delivered.
* ``prte_rml_posted_recv_t`` — a standing receive: a (peer, tag, persistent,
  callback) tuple.

Everything runs on one thread.  Work that originates elsewhere is moved onto
the progress thread with a *caddy* — a small heap object carrying the request
plus a ``pmix_event_t`` — posted via ``PRTE_PMIX_THREADSHIFT``.  ``PRTE_OOB_SEND``
and ``prte_rml_recv_buffer_nb`` are the two entry points that do this.

Sending a message
-----------------

The public entry point is the ``PRTE_RML_SEND(rc, dst, buffer, tag)`` macro,
which calls ``prte_rml_send_buffer_nb`` (``src/rml/rml_send.c``).  The RML takes
ownership of ``buffer``.

#. **Validation and self-sends.**  Invalid tag/rank or a known-down destination
   are rejected immediately.  A message addressed to *this* daemon never touches
   the network: it is wrapped in a ``prte_rml_recv_t`` and re-posted locally with
   ``PRTE_RML_ACTIVATE_MESSAGE``, so it enters the same delivery path as a
   received message.

#. **Hand to the transport.**  Otherwise the call builds a ``prte_rml_send_t``
   (``dst``, ``origin`` = me, ``tag``, ``dbuf``) and invokes ``PRTE_OOB_SEND``,
   which thread-shifts to ``prte_oob_base_send_nb`` (``oob_base_stubs.c``).

#. **Route and connect.**  ``prte_oob_base_send_nb`` first drops the message if
   the destination is down or the retry budget (``prte_rml_max_retries``) is
   exhausted, reporting the failure back through the send callback.  Otherwise it
   computes the **next hop** with ``prte_rml_get_route(dst)`` and looks up the
   TCP peer for that hop.  If the peer is unknown, it obtains the peer's contact
   URI (directly for the HNP, or from the PMIx store) and builds a peer object.

#. **Queue.**  If the peer is already connected, the message is queued for
   immediate transmission (``MCA_OOB_TCP_QUEUE_SEND``); otherwise it is queued as
   pending (``MCA_OOB_TCP_QUEUE_PENDING``) and the connection state machine in
   ``oob_tcp_connection.c`` is started.  Once the socket is up and the IDENT
   handshake has completed, the send handler in ``oob_tcp_sendrecv.c`` writes the
   header and then the payload.

Receiving, matching, and relaying
---------------------------------

On the wire, every message carries a ``prte_oob_tcp_hdr_t``
(``oob/oob_tcp_hdr.h``): the ``origin``, the final ``dst``, the ``tag``, a
sequence number, the payload length, and a message ``type`` (``IDENT``/``PROBE``
for the handshake, ``USER`` for a normal message).  The header is exchanged only
between daemons of the same DVM — all running the same build — so its layout is
not a stable ABI.

When a peer's socket has delivered a complete message, the recv handler
(``oob_tcp_sendrecv.c``) inspects ``hdr.dst``:

* **For us.**  It calls ``PRTE_RML_POST_MESSAGE``, which thread-shifts to
  ``prte_rml_base_process_msg`` (``rml_base_msg_handlers.c``).  That handler walks
  ``posted_recvs`` looking for a receive whose (peer, tag) matches — using
  ``PMIX_CHECK_PROCID`` so wildcard receives work — and fires its callback.  A
  non-persistent receive is removed once it fires.  If nothing matches, the
  message is parked on ``unmatched_msgs``.

* **Not for us.**  This daemon is an intermediate hop.  The handler rebuilds a
  ``prte_rml_send_t`` with the same ``origin``/``dst`` and re-enters
  ``PRTE_OOB_SEND``, which routes the message on toward the next hop.  Relaying is
  therefore just "send again from here."

The dual of ``process_msg`` is ``prte_rml_base_post_recv``: when new code posts a
receive, that handler also scans ``unmatched_msgs`` so that any message which
arrived early is delivered right away.  ``prte_rml_purge`` clears both lists for a
peer that has gone away.

Routing: the radix tree
-----------------------

Daemons are numbered ``0 .. N-1`` (rank 0 is the HNP) and arranged in a radix
tree.  The radix (fan-out) defaults to 64 and is set with
``--prtemca prte_rml_radix``.  The tree keeps the number of hops between any two
daemons small while bounding each daemon's connection count.

``prte_rml_get_route(target)`` (``routed_radix.c``) returns the rank of the next
hop:

* ``target`` itself if it is this daemon;
* this daemon's parent (its ``lifeline``) if ``target`` lies outside this
  daemon's subtree — i.e., the message must go *up* the tree;
* otherwise the child whose subtree contains ``target`` — the message goes
  *down* toward it.

All of the arithmetic — parent, child, sibling, "does this subtree contain rank
r," "which child subtree holds r," moving up and down by depth, and "next living
rank" traversals that skip failed daemons — lives as pure functions over rank
numbers in ``radix.h``.  That header is deliberately self-contained and includes
``radix_node_is_valid`` as an executable statement of the tree invariants; it is
the first thing to read if you need to modify the routing math.

Fault tolerance and reliable messaging
--------------------------------------

The routing tree is not static: daemons can fail, and (in elastic mode) the DVM
can grow and shrink.  ``prte_rml_base`` therefore tracks which daemons have
failed and repairs the tree in place.

When a connection is lost (``prte_rml_route_lost``) or a fault notice arrives,
``prte_rml_repair_routing_tree`` (``routed_radix.c``) recomputes this daemon's
ancestors and children, works out whether this daemon has been *promoted* to
fill a hole higher up, and packages the before/after difference into a
``prte_rml_recovery_status_t``.  Faults are handled twice — once **locally** as
they are discovered and once **globally** when the HNP broadcasts the confirmed
set — so handlers can distinguish "recover now" from "everyone agrees this is
final."  The recovery status is then delivered to the registered fault handlers:
the RML's own (``rml_fault_handler.c``, which drives process states and
death/adoption notices), followed by grpcomm, filem, and relm.

``relm`` (the ``src/rml/relm/`` subtree) is the **reliable messaging** layer.
``prte_rml_send_buffer_reliable_nb`` routes through it; it drives a small state
machine that re-sends messages over the repaired tree so that a message in
flight when a daemon dies is not simply lost.  It is newer than the collapsed
core and is intentionally kept as its own module.

Where to look
-------------

* Sending: ``rml_send.c`` → ``oob/oob_base_stubs.c`` → ``oob/oob_tcp_connection.c``
  → ``oob/oob_tcp_sendrecv.c``.
* Receiving/matching: ``oob/oob_tcp_sendrecv.c`` → ``rml_base_msg_handlers.c``.
* Routing math: ``routed_radix.c`` and ``radix.h``.
* Fault handling: ``routed_radix.c`` (``repair_routing_tree``),
  ``rml_fault_handler.c``, and ``relm/``.
* Editing map and gotchas: ``src/rml/README.md``.
