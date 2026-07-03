.. _rml-oob-label:

The OOB TCP Transport
=====================

The OOB ("out of band") transport is the layer beneath the RML API that moves a
message's bytes across a socket to the next daemon in the routing tree.  Where
the RML core (:ref:`rml-label`) decides *what* to send, *to whom*, and *which
way through the tree*, the OOB owns the sockets, the connection handshake, the
on-wire framing, and the retry logic.  The code lives in ``src/rml/oob/``; the
editing-oriented map is in ``src/rml/oob/AGENTS.md``.

One transport, not a framework
------------------------------

The OOB was once an MCA framework with pluggable transport *components*, each
with swappable *modules* — TCP was one, with notional room for others.  PRRTE
only ever shipped TCP, so the framework was collapsed into the single
``src/rml/oob`` directory.  Any language in the code about "trying the next
component," "another transport module," or a selection loop is historical
cruft.  There is exactly one transport: TCP.

Key state
---------

All transport state hangs off one global, ``prte_oob_base``
(``src/rml/oob/oob.h``, defined in ``oob_tcp.c``), and is owned by the progress
thread:

* the list of known **peers** (``prte_oob_tcp_peer_t``), each with its
  addresses, socket, connection state, and send queue;
* the local network **interfaces** the daemon may bind and connect from;
* the **listener** sockets;
* the many TCP tuning MCA parameters — buffer sizes, keepalives, retry limits,
  ``max_msg_size``, and the connection-retry knobs described below.

Each peer is a ``prte_oob_tcp_peer_t`` (``oob_tcp_peer.h``): the peer's name, a
list of candidate addresses, the active address, the socket, a state enum, the
send queue, and retry bookkeeping (``num_retries`` and ``first_attempt``).

The wire header
---------------

Every message on the wire is preceded by a ``prte_oob_tcp_hdr_t``
(``oob_tcp_hdr.h``): the ``origin``, the final ``dst``, the ``tag``, a sequence
number, the payload length, and a message ``type``:

* ``IDENT`` / ``PROBE`` — the connection handshake, and
* ``USER`` — a normal message.

The header is exchanged **only** among daemons of the same DVM, all running the
same build, so it is **not** a stable ABI.  Its layout may change freely, but
every daemon in a build must agree; there is no versioning.

Sending
-------

``PRTE_OOB_SEND(msg)`` (a macro in ``oob.h``) thread-shifts the send onto the
progress thread, where ``prte_oob_base_send_nb`` (``oob_base_stubs.c``) runs:

#. **Give-up checks.**  If the destination is known down, or the message's retry
   budget (``prte_rml_max_retries``) is exhausted, the message is dropped and
   the failure is reported back through its send callback.

#. **Route.**  ``prte_rml_get_route(dst)`` returns the rank of the next hop.

#. **Find or build the peer.**  If a ``prte_oob_tcp_peer_t`` for that hop already
   exists, use it.  Otherwise obtain the hop's contact URI — directly for the
   HNP, from the PMIx store otherwise, or (in a bootstrapped DVM) synthesized
   from configuration — and build the peer with ``process_uri`` / ``set_addr``.

#. **Queue.**  If the peer is connected, the message is queued for immediate
   transmission (``MCA_OOB_TCP_QUEUE_SEND``).  Otherwise it is queued as pending
   (``MCA_OOB_TCP_QUEUE_PENDING``) and the connection state machine is started.

Once the socket is up and the IDENT handshake has completed, the send handler in
``oob_tcp_sendrecv.c`` writes the header and then the payload.

The connection state machine
----------------------------

``oob_tcp_connection.c`` drives a peer from unconnected to usable.  The daemon
that initiates a connection sends an ``IDENT`` naming itself; the acceptor
(``recv_handler`` in ``oob_tcp.c``, plus ``oob_tcp_listener.c`` for the accept
socket) checks the identity and either acks or nacks.  Because two daemons can
try to connect to each other simultaneously, the handshake resolves which socket
survives so a pair of peers does not end up with two half-open connections.

Retry and backoff.  When a connect attempt finds no listener yet — a common race
during startup — ``prte_oob_tcp_peer_try_connect`` schedules a retry.  The base
behavior is a fixed ``retry_delay``-second wait, bounded by
``max_recon_attempts``.  Two MCA parameters modify this, both defaulting to the
original behavior:

* ``prte_retry_max_delay`` — when larger than ``retry_delay``, the delay backs
  off exponentially (``retry_delay``, 2×, 4×, …) capped at ``retry_max_delay``,
  so a daemon waiting on an absent peer polls quickly at first and then settles
  onto a steady rate rather than busy-spinning.  The exponent is clamped before
  the shift so an unbounded retry count cannot overflow.

* ``prte_connect_max_time`` — caps how long a **non-lifeline** peer is chased
  (measured from the peer's ``first_attempt`` timestamp) before giving up so the
  routing tree can heal to an ancestor.  ``0`` means retry forever.  The HNP is
  never subject to this cap; it is always retried forever.

Receiving and relaying
----------------------

When a peer's socket has delivered a complete message, the recv handler in
``oob_tcp_sendrecv.c`` inspects ``hdr.dst``:

* **For us.**  It calls ``PRTE_RML_POST_MESSAGE`` to hand the message up to the
  RML matching layer (:ref:`rml-label`).

* **Not for us.**  This daemon is an intermediate hop.  The handler rebuilds a
  ``prte_rml_send_t`` with the same ``origin`` / ``dst`` and re-enters
  ``PRTE_OOB_SEND``, so relaying is simply "send again from here," routed on
  toward the destination.

Connection loss.  When a socket drops, ``oob_tcp_component.c`` fires its
``lost_connection`` / ``failed_to_connect`` handlers, which turn a transport
event into a routing event: the RML is told to treat the peer as lost so the
tree can be repaired (see :ref:`rml-label`).

Bootstrap specifics
-------------------

In a launcher-less (bootstrapped) DVM the daemons boot independently, so the
transport cannot rely on a launcher having pre-distributed everyone's contact
information:

* **URIs are synthesized on demand.**  With ``prte_bootstrap_setup`` set and no
  peer object present, ``prte_oob_base_send_nb`` derives the next hop's URI from
  configuration via ``prte_ess_base_bootstrap_peer_uri`` — the same synthesis a
  prted uses for its original parent — rather than reading a nidmap that was
  never distributed.  This also covers a healed lifeline whose new parent (a
  former grandparent) was never pre-synthesized.

* **A missing interface mask is tolerated.**  A synthesized URI cannot know the
  peer's interface mask, so ``set_addr`` treats a missing or empty mask as
  ``/0`` (universally reachable) instead of rejecting the address.

* **A not-yet-present parent is not fatal.**  Because a parent may simply not be
  up when a child times out on it,
  ``prte_mca_oob_tcp_component_failed_to_connect`` (in bootstrap mode) heals the
  tree the way a lost *live* parent would: ``prte_rml_route_lost`` promotes the
  daemon to the next ancestor and returns a ``COMM_FAILED`` recovery, instead of
  raising a fatal ``FAILED_TO_CONNECT``.  The HNP is the exception — it is
  retried forever and never allowed to time out.

Where to look
-------------

* Send entry and routing: ``oob_base_stubs.c``.
* Connection handshake, retry, backoff: ``oob_tcp_connection.c``,
  ``oob_tcp_listener.c``, and ``recv_handler`` in ``oob_tcp.c``.
* Socket I/O and the deliver-vs-relay decision: ``oob_tcp_sendrecv.c``.
* Transport-to-routing fault events: ``oob_tcp_component.c``.
* The wire header: ``oob_tcp_hdr.h``.
