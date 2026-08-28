Data Store Retention: Implementation Plan
=========================================

Build order for :doc:`datastore-retention`, which is the design, against
:doc:`spec`, which is the contract.  Each phase is a commit or a small
series of them, each leaves the tree building warning-free and passing
``make check``, and each is useful on its own.

The ordering is chosen so that the reported bug is fixed early and the
larger machinery lands behind it, and so that nothing depends on the
PMIx change until the phase that needs it.

Phase 0 — the reported bug
--------------------------

``PMIX_PERSIST_FIRST_READ`` moves out of ``expires_by()``'s "shortest
lifetimes" case, so a job-end purge no longer takes an unread item.
This is `issue #2733 <https://github.com/openpmix/prrte/issues/2733>`_
and it is four lines.

Land it with the end-to-end ``FIRST_READ`` test the suite does not have,
and with the two-generation swarm case from the issue's reproducer.

The item is now unbounded until Phase 4 gives it a timeout.  That is a
knowingly temporary state and it is the state ``INDEF`` has been in
since the beginning, so it makes nothing worse.

Phase 1 — the multi-node purge bug
----------------------------------

``job_teardown()`` in ``state_prted.c`` sends a namespace-wide purge to
the *master's* store when only the local daemon's share of a job has
finished.  Scope the daemon-side call to the daemon's own store, and
leave the namespace horizon to the master.

Standalone commit: it is a live defect, it is independent of everything
below, and it wants its own bisect point.  Its swarm case is a job
spanning nodes where one node's procs exit well before another's.

Phase 2 — removal by uid
------------------------

#. ``ds_unpublish.c``: read ``PMIX_USERID`` / ``PMIX_GRPID`` from the
   directives and replace the procid comparison with the uid/gid
   predicate.
#. ``ds_relay.c``: pack ``prte.pub.ruid`` / ``prte.pub.rgid`` beside
   ``PMIX_REQUESTOR``; ``ds_publish.c`` and ``ds_unpublish.c`` honor
   them under the same tool-only rule ``check_requestor`` applies.
#. ``ds_publish.c``: widen ``PRTE_PUBLISH_REPLACE`` to the same
   predicate.

Unit tests for the predicate carry this phase, including the two
directions the specification calls out: an accessor list does not confer
removal, and an owner its own accessor list excludes may still remove.

This phase alone resolves the wedged-name problem the issue describes,
so it is worth having complete before the retention work starts.

Phase 3 — the horizons
----------------------

Depends on the PMIx constant (below), which should be in flight from the
start of this work.

#. ``ds.h`` / ``ds_main.c``: ``app_idx`` and ``session_id`` on the object,
   constructed properly.  (``last_access`` waits for Phase 4, which is
   where the first thing that reads it lands.)
#. ``ds_publish.c``: resolve the publisher's app and session; stamp
   ``last_access``.
#. ``ds_purge.c``: the new ``expires_by()`` table; match on the app
   index and the session id; unpack the two new directives.
#. State machine: the ``PROC``, ``APP``, ``NSPACE`` and ``SESSION``
   senders, and the per-app termination count in
   ``prte_state_base_track_procs``.  The existing job-end purge changes
   its horizon from ``APP`` to ``NSPACE``.
#. ``prted_comm.c``: purge the daemon's own store on
   ``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD``; a local ``PROC`` purge where the
   daemon reaps a child.
#. The default persistence becomes ``PMIX_PERSIST_NSPACE``.

Packer and unpacker change together.  The swarm case is two jobs under
one persistent DVM: a ``PERSIST_APP`` item gone at job end, a
``SESSION`` one still there.

Phase 4 — the timeout
---------------------

``last_access`` on the object, ``prte_data_server_timeout``, the sweep
event, and the arm/disarm rule.

Swarm coverage sets the timeout to a few seconds by MCA parameter — not
a debug-only knob, and not a ``PRTE_ENABLE_DEBUG`` build.

Phase 5 — the cap
-----------------

``prte_data_server_max_size``, ``nbytes``, the per-uid usage records,
eviction, the refusal path, and the first-eviction ``show_help``
warning.

The ``show_help`` text is new, so this phase carries the regeneration
step: ``rm src/util/prte_show_help_content.* && make``.  Skipping it
leaves the binary serving the old message set even though the ``.txt``
looks right.

Phase 6 — documentation
-----------------------

The two MCA parameters and the retention rules belong in the user-facing
RST under ``docs/``, not only in this plan.  The deferred app
notification goes in ``docs/todo.rst`` in prose, as that file is the
single reference for outstanding work.

In parallel: PMIx
-----------------

``PMIX_PERSIST_NSPACE`` as value 5 in ``include/pmix_common.h.in``, with
its man-page entry, plus the proposal to the Standard.  Phase 3 is
blocked on the constant existing; nothing earlier is.

Verification at each phase
--------------------------

* ``--enable-debug`` build, warning-free.
* ``make check``.
* ``prte --daemonize`` → ``prun -n 4 hostname`` → ``pterm``, for any
  phase touching the state machine (1, 3).
* ``contrib/dockerswarm`` ``test_runtime``, for every phase from 0 on;
  the suite baseline is green, so a failure is real.
* The partial-lookup swarm cases need a PMIx built from source
  (``PMIX_SRC=... ./build.sh``), and from Phase 3 so does everything
  else here, since the new constant will not be in an installed
  release yet.
