.. _dvm-grow-campaign-label:

DVM Grow-Campaign Fence Tracking
================================

This document describes the implementation that makes the DVM **grow**
(daemon-launch) path account for the launch fence on a per-daemon, rank-tracked
basis, mirroring the design already used by the DVM **shrink** path.  For the
fence mechanism itself and the race it closes, see
:ref:`dvm-launch-fence-label` and :ref:`state-machine-label`, section
*DVM Extension and the Daemon-Launch Race*.

The state machine is single-threaded on the progress thread, so no locking is
required anywhere in this plan.

The observable job-admission and placement guarantees that the grow path
upholds are specified in :ref:`elastic-dvm-spec-label`, which is
authoritative for observable behavior; this document describes the
implementation that delivers them.

Motivation
----------

The launch fence (``prte_dvm_launch_fence``) holds application jobs at the
``VM_READY → MAP`` boundary while a daemon-launch campaign is in progress, so
that no job is mapped onto a node whose daemon is not yet up and wired.  The
shrink path tracks the specific daemon ranks it is removing in a
``prte_shrink_campaign_t`` and resolves the fence one rank at a time as each
targeted daemon actually departs.

The grow path, by contrast, originally encoded "a grow is in progress" as a
single boolean — ``PRTE_JOB_LAUNCHED_DAEMONS`` — set on the one daemon job,
together with a ``prte_dvm_launch_fence++`` performed once per campaign in
``prte_plm_base_setup_virtual_machine()``.  The single decrement happened in
``vm_ready`` on success, or in the ``errmgr/dvm`` comm-failure handler if a
daemon died first.  Because that boolean carries no identity, two defects
followed:

#. **An unrelated daemon death consumed the campaign's token.**  The
   comm-failure handler decremented the fence and cleared the boolean whenever
   *any* daemon died while a grow was in progress — there is only one daemon
   job, and it carried the token.  A pre-existing daemon dying mid-grow would
   therefore release the held jobs early (reopening the very race the fence
   exists to close) and clear the token, after which ``vm_ready`` skipped the
   WIREUP xcast (it is gated on the same attribute), so the genuinely new
   daemons could come up without ever receiving the nidmap/wireup buffer.

#. **Concurrent campaigns could wedge the fence.**  Two overlapping grows would
   raise the fence to two but share the single boolean token, which can only be
   cleared once.  A daemon failure would clear it, leaving the fence stuck
   above zero and the held jobs parked indefinitely.

Both defects trace to the same root cause: the grow path tracked *that* a grow
was happening, not *which* daemons it was launching.

Design
------

Track each grow campaign explicitly, recording the ranks being launched, and
hold the whole campaign's fence contribution until a single safe drain point.

New campaign object
~~~~~~~~~~~~~~~~~~~~

In ``src/runtime/prte_globals.h`` / ``prte_globals.c``:

.. code-block:: c

   typedef struct {
       pmix_list_item_t super;
       pmix_rank_t     *targets;   /* daemon ranks being launched */
       int              ntargets;  /* == this campaign's fence contribution */
   } prte_grow_campaign_t;
   PMIX_CLASS_DECLARATION(prte_grow_campaign_t);

   PRTE_EXPORT extern pmix_list_t prte_grow_campaigns;

The list is constructed in ``prte_init.c`` and destructed in
``prte_finalize.c`` alongside ``prte_shrink_campaigns``.  A separate list (as
opposed to unifying with the shrink list) is used deliberately: the
``LAUNCH_APPS`` hold and the remap-on-release logic key off the *shrink* list's
non-emptiness, and a grow must **not** stall jobs that are already mapped onto
existing nodes.  Keeping the lists separate leaves the working shrink path
untouched.

Fence is campaign-granular, not per-rank
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Unlike shrink, the grow fence contribution is **held in full until the
campaign is drained as a unit**.  This is the key correctness point: the
fence must not reach zero (for a successful grow) until *after* the WIREUP
xcast in ``vm_ready``, otherwise an application job arriving in the window
between "last daemon reported" and "wireup sent" would see a zero fence and
map onto daemons that are up but not yet wired.  A naive per-rank decrement at
daemon-report time would reopen exactly that window.  Holding the contribution
until ``vm_ready`` drains it preserves the original ordering guarantee.

The per-rank ``targets`` array therefore exists for one purpose: to decide
whether a *failure* event belongs to this grow.

Lifecycle
~~~~~~~~~

#. **Create** — in ``prte_plm_base_setup_virtual_machine()``, when
   ``map->num_new_daemons > 0``: build a ``prte_grow_campaign_t`` recording the
   ``num_new_daemons`` consecutive vpids starting at ``map->daemon_vpid_start``,
   append it to ``prte_grow_campaigns``, and add ``num_new_daemons`` to the
   fence.  ``PRTE_JOB_LAUNCHED_DAEMONS`` is still set on the daemon job for its
   unrelated uses (the WIREUP gate in ``vm_ready`` and the odls path); it is no
   longer consulted for fence accounting.

#. **Success drain** — ``vm_ready`` fires only once every expected daemon has
   reported (``num_reported == num_procs``), which means any in-progress grow
   campaigns have fully succeeded.  After performing the WIREUP xcast, it calls
   ``prte_plm_base_grow_drain(true)``, which removes every grow campaign,
   subtracts each ``ntargets`` from the fence, and — if the fence has reached
   zero — releases the held jobs with ``success == true``.

#. **Failure drain** — in the ``errmgr/dvm`` comm-failure /
   ``FAILED_TO_START`` handler, the dead daemon's rank is passed to
   ``prte_plm_base_grow_target_failed()``.  That function acts **only** if the
   rank is a member of an in-progress grow campaign; an unrelated daemon loss
   matches nothing and leaves the fence untouched (fixing defect 1).  If the
   rank is a grow target, the grow is compromised, so it calls
   ``prte_plm_base_grow_drain(false)`` — failing the held jobs.  Mirroring the
   original single-token behavior, any grow failure fails the whole held-job
   set.

#. **Safety net** — ``check_job_complete``'s "received NULL job" branch drains
   any still-pending grow campaigns with ``success == false`` so held jobs are
   never parked across a daemon-job teardown.

Because every live campaign contributes exactly its ``ntargets`` to the fence
until drained as a unit, the fence's grow contribution is always the sum of the
``ntargets`` of the campaigns on the list, and ``prte_plm_base_grow_drain()``
zeroes that contribution in one pass — independent of how many concurrent
campaigns exist (fixing defect 2).

Why this is correct
-------------------

* **Unrelated daemon death during a grow.**  ``grow_target_failed()`` scans the
  campaign target arrays; a non-target rank matches nothing, so the fence is
  not touched, the held jobs are not released early, and the WIREUP xcast is
  not skipped.

* **Concurrent campaigns.**  Each campaign is an independent object with its own
  contribution.  ``grow_drain()`` removes them all and the fence reaches zero
  only when no grow contribution remains; there is no single token to exhaust.

* **Wireup ordering.**  The fence stays at its full value throughout the grow
  and is dropped only when ``vm_ready`` drains it after the WIREUP xcast (on
  success) or when a target dies (on failure).  Jobs held at ``VM_READY → MAP``
  are thus admitted only once the new daemons are wired up.

* **Partial failure.**  A grow in which any target dies is failed as a whole:
  the dying daemon triggers ``grow_drain(false)`` and the held jobs are
  activated to ``NEVER_LAUNCHED``.  This matches the original first-failure
  semantics.

Touched files
-------------

.. list-table::
   :widths: 45 55
   :header-rows: 1

   * - File
     - Change
   * - ``src/runtime/prte_globals.{h,c}``
     - Add ``prte_grow_campaign_t``, ``prte_grow_campaigns`` list, and class.
   * - ``src/runtime/prte_init.c`` / ``prte_finalize.c``
     - Construct / destruct ``prte_grow_campaigns``.
   * - ``src/mca/plm/base/plm_base_launch_support.c``
     - Create the campaign in ``setup_virtual_machine``; add
       ``prte_plm_base_grow_drain()`` and ``prte_plm_base_grow_target_failed()``.
   * - ``src/mca/plm/base/plm_private.h``
     - Declare the two new helpers.
   * - ``src/mca/errmgr/dvm/errmgr_dvm.c``
     - Replace the coarse ``PRTE_JOB_LAUNCHED_DAEMONS`` fence block with a call
       to ``prte_plm_base_grow_target_failed()``.
   * - ``src/mca/state/dvm/state_dvm.c``
     - Drain on success in ``vm_ready`` after WIREUP; drop the per-error fence
       manipulation (the DVM is force-exiting); convert the
       ``check_job_complete`` safety net to a drain.

Follow-up
---------

The grow and shrink campaign objects are structurally similar and could be
unified into a single ``prte_launch_campaign_t`` with a ``kind`` discriminator
in a future cleanup.  That was intentionally deferred here to avoid disturbing
the ``LAUNCH_APPS`` hold and remap-on-release logic, which must remain
shrink-only.
