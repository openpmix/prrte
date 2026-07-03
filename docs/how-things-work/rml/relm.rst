.. _rml-relm-label:

Reliable Messaging (RELM)
=========================

A normal RML send is fire-and-forget.  It rides the radix routing tree
(:ref:`rml-label`), and if a daemon on the path dies while a message is in
flight, that message is simply lost — the tree repairs itself, but the bytes are
gone.  For most runtime traffic that is acceptable, because the higher-level
protocol will retry or the information is reconstructible.  Some traffic is not
so forgiving.  **RELM** ("reliable messaging") is the layer that guarantees a
message is delivered exactly once to its destination even if daemons on the path
fail and the tree is rebuilt underneath it.

RELM lives in ``src/rml/relm/``; its editing map is ``src/rml/relm/AGENTS.md``
and the concrete implementation's map is ``src/rml/relm/base/AGENTS.md``.

How RELM is invoked
-------------------

Application code sends reliably with ``prte_rml_send_buffer_reliable_nb``
(wrapped by the ``PRTE_RML_RELIABLE_SEND`` macro).  That call routes through the
``prte_relm`` module to ``prte_relm_start_msg``.  Everything else in the
subsystem runs in reaction to two things: RELM control messages received from
neighbors, and fault notices from the routing layer.

RELM is shaped like a small framework — the ``prte_relm_module_t`` interface and
a state machine (``prte_relm_state_machine_t``) full of function pointers — so a
different reliability strategy could be substituted.  Today there is exactly one
implementation, the *base* module, which ``relm.c`` installs at open time by
copying ``prte_relm_base_module`` into the global ``prte_relm`` and wiring every
state-machine callback to a ``prte_relm_base_*`` function.

Identifying a message
---------------------

Every reliable message has a stable, globally-unique identity that all daemons
on its path agree on:

* the **origin** ``src`` (the rank that called ``reliable_send``) and a
  per-source **UID** together form a globally-unique id;
* adding the destination ``dst`` yields the message **signature**
  (``prte_relm_signature_t``);
* the pair ``<src, uid>`` is hashed into a 64-bit **GUID**
  (``prte_relm_guid_t``) used as the hash-table key.

The UID (``prte_relm_uid_t``) is a 32-bit counter that is allowed to wrap; the
design assumes a message is globally complete and unreferenced long before its
UID could be reused.  The top of the range is reserved for sentinels
(``UNKNOWN`` / ``NONE`` / ``INVALID``), so valid UIDs run up to
``PRTE_RELM_UID_MAX``.

State is kept per destination.  ``prte_relm_state_machine_t`` holds a hash table
of ``prte_relm_rank_t`` objects keyed by destination rank; each of those holds a
hash table of ``prte_relm_msg_t`` objects keyed by GUID.  A ``prte_relm_msg_t``
records the message's ``src`` / ``uid`` / ``dst``, its current state, the
message data (while it may still be needed), and ``prev_uid`` / ``next_uid``
links that chain together the messages this origin has sent to this destination.

Ordering
--------

Messages from one origin to one destination are delivered **in order**.  The
``prev_uid`` / ``next_uid`` chain enforces this: a message may only be posted to
the application once its predecessor has been posted, and an acknowledgement of a
message implicitly acknowledges everything before it in the chain.  A message
whose predecessor has not yet arrived is parked in the ``PENDING`` state and
released to ``SENDING`` when the predecessor is posted.

The message lifecycle
---------------------

Each ``prte_relm_msg_t`` moves through a small set of *lasting* states
(``types.h``).  Because the same message object exists on every daemon along the
path, "the state" means *this daemon's* view of the message:

``SENDING``
    The message (with its data) is queued to go one hop further downstream,
    toward ``dst``.
``SENT``
    This daemon has handed the message to the next hop.  An intermediate daemon
    caches the data at this point in case a replay is needed.
``REQUESTED``
    A replay has been requested — the data was lost from somewhere downstream
    and must be re-sent.
``PENDING``
    The message is held for ordering, waiting on its predecessor.
``ACKED``
    The destination has posted the message; an acknowledgement is travelling
    back upstream toward ``src``.
``ACKACKED``
    The origin has seen the ACK and is releasing state; the ACK-of-ACK travels
    back downstream so every daemon can drop the message.

A handful of *ephemeral* states (``NEW``, ``ACKACKED`` when used to trigger,
``CACHED``, ``EVICTED``) drive transitions but are **never stored** as a
message's ``state``; the engine enforces this.

Steady-state flow.  When ``reliable_send`` starts a message, the origin
constructs it in ``NEW``, records the data, links it after the previous message
to that destination, and moves it to ``SENDING``.  The data is forwarded one hop
at a time; each hop transitions ``SENDING`` → ``SENT`` and (if it is neither
source nor destination) caches the data.  When the data reaches ``dst``, the
destination posts it to the application — respecting the ``prev_uid`` ordering,
holding it ``PENDING`` if its predecessor has not yet posted — and moves it to
``ACKED``.  The ACK walks back upstream hop by hop; at the origin it becomes an
ACK-of-ACK, which walks back downstream so every daemon on the path releases its
copy.

State updates on the wire
-------------------------

State changes propagate as small RELM control messages carrying the message's
signature, its ``prev_uid``, its state, and (for ``SENDING``) the data.  They
are sent with the ordinary RML over a single ``prte_rml_get_route`` hop, tagged
``PRTE_RML_TAG_RELM_STATE``.  ``state_machine.c`` provides the two emitters —
``prte_relm_send_state_downstream`` (toward ``dst``) and
``prte_relm_send_state_upstream`` (toward ``src``) — and a receiver,
``prte_relm_message_handler``, that unpacks an update, finds or creates the local
message object, and feeds it to the state machine.

The interesting logic is ``prte_relm_base_update_state`` (``state_updates.c``),
which routes an incoming update by **who sent it**:

* ``local_update`` — a change requested by this daemon itself;
* ``downstream_update`` — a change reported by the neighbor toward ``dst``
  (typically an ACK coming back);
* ``upstream_update`` — a change reported by the neighbor toward ``src``
  (typically data coming forward, or a replay request).

Each of those is a switch over the incoming state that decides what to do given
the message's current state: forward the data, post to the application, cache
the data, request or replay a lost message, emit an ACK, or tear the message
down.  Updates that arrive from a rank that is no longer this message's upstream
or downstream neighbor are simply ignored — a natural consequence of the tree
having changed.

Caching and replay
------------------

An intermediate daemon holds a message's data after forwarding it (state
``SENT``) so it can replay the data if a downstream daemon lost it during a
fault.  Cached data is bounded two ways, both configurable:

* by **time** — ``relm_base_cache_ms`` (default 500 ms), after which an eviction
  timer fires and drops the data; and
* by **count** — ``relm_base_cache_max_count`` (default 30), beyond which the
  oldest cached message is evicted.

When data is needed but no longer cached anywhere reachable, a daemon issues a
``REQUESTED`` update upstream; the first daemon that still holds the data (or the
origin, which always can) replays it as a fresh ``SENDING``.

Surviving a fault: link updates
-------------------------------

The steady-state protocol assumes the path is stable.  When a daemon fails, the
routing layer repairs the tree and notifies the registered fault handlers (RML,
grpcomm, filem, then relm — see :ref:`rml-label`).  RELM's handler
(``link_updates.c``) is where in-flight messages are stitched back onto the new
tree.

The handler acts only on the **local**-scope recovery notice.  (Faults are
reported twice: once locally as they are discovered and once globally when the
HNP confirms them.  By the time the global notice arrives, RML recovery has
already run and the tree shows no delta, so a component must save whatever it
needs between the two calls.)  On the local notice, the handler:

#. **Purges** dead paths — messages to or from a failed rank, and messages this
   daemon is, after the repair, no longer on the direct path of.

#. **Exchanges link updates** with each neighbor whose identity changed.  A link
   update (tagged ``PRTE_RML_TAG_RELM_LINK``) carries the full state of every
   in-flight message that routes through that link, so a new neighbor learns
   exactly what is outstanding and can resume it.

Two mechanisms keep this correct under cascading or concurrent promotions:

* **Depth stamping.**  Each link update carries the sender's tree depth.  A
  receiver ignores an update whose depth does not match the expected
  parent/child depth, so lingering updates from *before* a promotion are
  discarded rather than misapplied.

* **Update gating.**  The ``upstream_links_updated`` and
  ``downstream_links_updated`` bitmaps track which neighbors this daemon has
  exchanged state with since its last promotion.  A daemon will not forward
  updates onward until it has gathered enough neighbor state to do so correctly;
  ``try_send_pending_link_updates`` releases pending updates as the missing
  pieces arrive.

Because a promoted daemon may have inherited children that briefly believed they
had a different parent, the recovery code treats all of a promoted daemon's
children as new — see the commentary on ``prte_rml_recovery_status_t`` in
``rml_types.h``.

Where to look
-------------

* Entry point and message identity: ``state_machine.c`` (``start_msg``,
  ``find``/``get``, GUID math in ``types.h``).
* Steady-state transitions: ``base/state_updates.c``.
* Fault recovery and link updates: ``base/link_updates.c``.
* Pack/unpack and helper macros: ``util.c`` / ``util.h``.
* Module wiring and MCA parameters: ``base/base.c``.
