.. _dvm-shrink-campaign-label:

DVM Shrink-Campaign Fence Tracking
==================================

This document describes the implementation of the DVM **shrink** (node-removal)
path: how a ``PMIX_ALLOC_RELEASE`` that removes daemons is tracked against the
launch fence, the second hold point that protects in-flight jobs, how campaign
completion is detected, and the completion event delivered to the requester.
For the shared fence mechanism it builds on — the counter, the held-job arrays,
the ``VM_READY → MAP`` hold point, the fence-release helper, and the
completion-event helper — see the parent plan :ref:`elastic-dvm-plan-label`.
The externally observable contract is specified in :ref:`elastic-dvm-spec-label`,
which is authoritative for observable behavior.

The state machine is single-threaded on the progress thread, so no locking is
required anywhere in this plan.

Background
----------

The ``PRTE_DAEMON_SHRINK_CMD`` xcast is fire-and-forget: daemons exit
asynchronously and the HNP has no built-in notification when all targeted
daemons have terminated.  Two race windows must be closed:

**Race 1 — new job maps onto a shrinking node.**  A job that checks the
``VM_READY → MAP`` fence while a shrink is in progress may pass the fence
(if it was raised after the check), get mapped to a node whose daemon is
dying, and then send a launch message to a daemon that has already exited.

**Race 2 — in-flight job at LAUNCH_APPS.**  A job that completed MAP before
the shrink started and then enters ``prte_plm_base_launch_apps()`` may pack
and send launch data to a daemon that dies between MAP and the send.  The
existing VM_READY fence does not protect this window because the job already
passed the fence before the shrink was initiated.

Race 1 is covered by the shared fence (the fence is incremented for shrink — see
Step 1).  Race 2 requires a second hold point guarded by checking the shrink
campaign list (nonempty only during shrink) so that a concurrent grow does not
unnecessarily hold jobs that have already been mapped to surviving nodes.

Multiple concurrent shrink campaigns are supported: each campaign tracks its
own count of still-living targets and is removed from the list when all of
them have departed the DVM.

Design Decision — Complete on Death, Not on Acknowledgement
-----------------------------------------------------------

An earlier revision of this plan had each targeted daemon send an explicit
``PRTE_PLM_SHRINK_ACK_CMD`` to the HNP just before it exited, and the HNP
decremented the campaign on *receipt of the ACK*.  The errmgr comm-failure
path existed only as a *fallback* for a daemon that crashed before it could
send its ACK.  That design was abandoned for the following reasons.

**The ACK is the wrong signal.**  An ACK announces a daemon's *intent* to
leave; it is sent while the daemon is still alive and still a participant in
the DVM.  But the state the fence protects against — a job being mapped onto,
or having launch data sent to, a departing daemon — is only safe once the
daemon's routes, its ``num_daemons`` count, and its node state have actually
been torn down.  That teardown happens on the comm-failure path
(``errmgr_dvm.c``), *not* when the ACK is sent.  Releasing held jobs on ACK
receipt could therefore unpark them into a DVM that still believed the
departing daemon was present.

**The reason for departure carries no information.**  The HNP only needs to
know that a target is gone, not *why*.  A clean shrink exit and a crash have
identical consequences for the campaign: the node is being removed either
way, and the application processes beneath the daemon are killed (or die)
when it terminates.  Distinguishing the two cases buys nothing, so the
"clean ACK vs. crash fallback" split was pure complexity.

**Two decrement paths caused double-counting.**  With both the ACK handler
and the errmgr fallback live, each target could be counted twice — once when
its ACK arrived (daemon still alive) and again when its subsequent death was
detected — because nothing marked a target as already counted and the
campaign was only removed once ``pending`` hit zero.  Worked through for a
two-target campaign:

.. code-block:: text

   camp: ntargets=2, pending=2
     daemon A acks   -> pending=1, fence-=1     (A still alive)
     daemon A dies   -> errmgr matches A (still in targets, camp still listed)
                     -> pending=0 -> camp removed+released, fence-=1
     daemon B        -> never counted; campaign already "complete"

The campaign completed and the fence released while daemon B was still
present, re-opening exactly the race the fence was meant to close.

**Resolution.**  The ACK was removed entirely (the daemon-side send, the
``PRTE_PLM_SHRINK_ACK_CMD`` constant, and the HNP-side handler).  Campaign
completion is driven solely by actual daemon departure on the comm-failure
path, which is both the authoritative event and the point at which the
relevant cleanup has occurred.  To make the single decrement idempotent
against a daemon that emits more than one failure event, each matched target
slot is stamped ``PMIX_RANK_INVALID`` once counted (Step 5).

Step 1 — Shrink campaign type, list, and fence increment
---------------------------------------------------------

**1a — Campaign type**

Add the following type to ``src/runtime/prte_globals.h`` and define the
class instance in ``src/runtime/prte_globals.c``:

.. code-block:: c

   /* one entry per in-progress shrink campaign */
   typedef struct {
       pmix_list_item_t super;
       pmix_rank_t     *targets;        /* daemon ranks being terminated */
       int              ntargets;       /* initial count */
       int              pending;        /* targets not yet known to have departed */
       /* requester recorded for the spec's phase-two completion event */
       pmix_proc_t      requester;      /* who issued the PMIX_ALLOC_RELEASE */
       char            *alloc_id;       /* PMIX_ALLOC_ID of the allocation */
       char            *req_id;         /* PMIX_ALLOC_REQ_ID, or NULL */
       bool             have_requester; /* false for a scheduler-driven release */
   } prte_shrink_campaign_t;
   PMIX_CLASS_DECLARATION(prte_shrink_campaign_t);

In ``src/runtime/prte_globals.c``:

.. code-block:: c

   static void campaign_destruct(prte_shrink_campaign_t *p)
   {
       free(p->targets);
       free(p->alloc_id);
       free(p->req_id);
   }
   PMIX_CLASS_INSTANCE(prte_shrink_campaign_t, pmix_list_item_t,
                       NULL, campaign_destruct);

**1b — Global list**

In ``src/runtime/prte_globals.h`` declare, and in ``src/runtime/prte_globals.c``
define:

.. code-block:: c

   pmix_list_t prte_shrink_campaigns;

Initialize in ``src/runtime/prte_init.c``:

.. code-block:: c

   PMIX_CONSTRUCT(&prte_shrink_campaigns, pmix_list_t);

Destruct in ``src/runtime/prte_finalize.c``:

.. code-block:: c

   PMIX_LIST_DESTRUCT(&prte_shrink_campaigns);

**1c — Populate campaign and increment fence**

In ``src/mca/ras/base/ras_base_allocate.c``, the ``PMIX_ALLOC_RELEASE``
branch of ``prte_ras_base_complete_request()`` builds the daemon rank array
(``ranks``, count ``m``) and then calls ``free(ranks)`` at line 760 before
the xcast at line 763.  Insert the campaign setup **before** ``free(ranks)``
(between lines 758 and 760), recording the requester (from the originating
request) so the completion event can be directed at it:

.. code-block:: c

   /* record the campaign — must be before free(ranks) */
   {
       prte_shrink_campaign_t *_camp = PMIX_NEW(prte_shrink_campaign_t);
       _camp->targets = (pmix_rank_t *) malloc(m * sizeof(pmix_rank_t));
       memcpy(_camp->targets, ranks, m * sizeof(pmix_rank_t));
       _camp->ntargets = m;
       _camp->pending  = m;
       /* requester / ids captured from the PMIX_ALLOC_RELEASE request;
        * have_requester stays false for a scheduler-driven release */
       if (have_requester) {
           PMIX_XFER_PROCID(&_camp->requester, &requester);
           _camp->alloc_id = (NULL != alloc_id) ? strdup(alloc_id) : NULL;
           _camp->req_id   = (NULL != req_id)   ? strdup(req_id)   : NULL;
           _camp->have_requester = true;
       }
       pmix_list_append(&prte_shrink_campaigns, &_camp->super);
       prte_dvm_launch_fence += m;
   }
   free(ranks);   /* existing line 760 */

   /* existing xcast */
   if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
       /* clean up the campaign we just added, and tell the requester the
        * DVM modification failed (spec phase-two failure event) */
       prte_shrink_campaign_t *_camp =
           (prte_shrink_campaign_t *) pmix_list_remove_last(&prte_shrink_campaigns);
       prte_dvm_launch_fence -= _camp->pending;
       if (_camp->have_requester) {
           /* success == false => PMIX_ERR_DVM_MOD carrying rc */
           prte_plm_base_dvm_mod_notify(&_camp->requester, _camp->alloc_id,
                                        _camp->req_id, false, rc);
       }
       PMIX_RELEASE(_camp);
       PRTE_ERROR_LOG(rc);
   }

Because the campaign is appended before the xcast, any ``VM_READY`` event
that fires on the progress thread after this point will see a nonzero fence
and park the job.

Step 2 — Daemon exit (no acknowledgement)
------------------------------------------

A daemon that decides to exit in response to ``PRTE_DAEMON_SHRINK_CMD``
does **not** send any acknowledgement to the HNP.  After firing its
``PMIX_EVENT_JOB_END`` notification it simply activates
``PRTE_JOB_STATE_DAEMONS_TERMINATED`` and exits.

The HNP tracks campaign completion through the daemon's *actual departure*,
not through a message announcing its intent to leave.  An acknowledgement
sent before the daemon dies would be premature: the HNP cares only that the
daemon is gone — the reason is irrelevant, and the application processes
under it are killed when it terminates regardless.  More importantly, the
acknowledgement would arrive *before* the daemon's routes, ``num_daemons``
count, and node state have been torn down, so acting on it could release
held jobs into a DVM that still believes the departing daemon is present.
The comm-failure event (Step 5) is the only signal that coincides with that
cleanup, so it is the sole completion trigger.

Step 3 — Second hold point at LAUNCH_APPS
------------------------------------------

In ``src/mca/plm/base/plm_base_launch_support.c``,
``prte_plm_base_launch_apps()`` (line 817), add a check after the job-state
guard but before packing any data:

.. code-block:: c

   /* if a shrink is in progress, hold this job until all targeted
    * daemons have departed the DVM, to prevent sending launch data to
    * a dying daemon */
   if (!pmix_list_is_empty(&prte_shrink_campaigns)) {
       jdata->state = PRTE_JOB_STATE_WAITING_FOR_DAEMONS;
       PMIX_RETAIN(jdata);
       pmix_pointer_array_add(prte_prelaunch_held_jobs, jdata);
       PMIX_RELEASE(caddy);
       return;
   }

Using ``!pmix_list_is_empty(...)`` rather than a counter means the check
automatically handles concurrent campaigns: the list is nonempty as long as
any shrink is in progress.  Keying on the **shrink** list specifically (not the
shared fence counter) ensures a concurrent grow does not stall a job that has
already been mapped onto surviving nodes.

Step 4 — Remap helpers
----------------------

The pre-launch branch of the shared ``prte_plm_base_fence_release()`` (parent
plan, Step 4) calls two shrink-specific helpers, both defined in
``plm_base_launch_support.c`` and declared in ``plm_base_launch_support.h``.

**``prte_plm_base_job_needs_remap(jdata)``** iterates over ``jdata->procs``
and returns ``true`` if any proc's assigned node has a daemon rank appearing
in any active campaign:

.. code-block:: c

   bool prte_plm_base_job_needs_remap(prte_job_t *jdata)
   {
       prte_shrink_campaign_t *camp;
       prte_proc_t *proc;
       int p, t;

       PMIX_LIST_FOREACH(camp, &prte_shrink_campaigns, prte_shrink_campaign_t) {
           for (p = 0; p < jdata->procs->size; p++) {
               proc = (prte_proc_t *)
                   pmix_pointer_array_get_item(jdata->procs, p);
               if (NULL == proc || NULL == proc->node ||
                   NULL == proc->node->daemon) continue;
               for (t = 0; t < camp->ntargets; t++) {
                   if (camp->targets[t] == proc->node->daemon->name.rank) {
                       return true;
                   }
               }
           }
       }
       return false;
   }

**``prte_plm_base_reset_proc_map(jdata)``** un-claims all slot assignments
made during the previous MAP pass so that the job can be remapped cleanly.
Mirror the mapper's ``prte_rmaps_base_claim_slot()`` accounting, which does
``node->num_procs++`` and ``++node->slots_inuse`` for each non-tool proc:

.. code-block:: c

   void prte_plm_base_reset_proc_map(prte_job_t *jdata)
   {
       int p, np;
       prte_proc_t *proc;
       prte_node_t *node;
       prte_app_context_t *app;

       for (p = 0; p < jdata->procs->size; p++) {
           proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, p);
           if (NULL == proc) continue;
           node = proc->node;
           if (NULL != node) {
               /* remove from node's proc list */
               for (np = 0; np < node->procs->size; np++) {
                   if (pmix_pointer_array_get_item(node->procs, np) == proc) {
                       pmix_pointer_array_set_item(node->procs, np, NULL);
                       node->num_procs--;
                       /* mirror claim_slot: tool procs do not count
                        * against slots_inuse */
                       app = (prte_app_context_t *)
                           pmix_pointer_array_get_item(jdata->apps,
                                                       proc->app_idx);
                       if (NULL == app ||
                           !PRTE_FLAG_TEST(app, PRTE_APP_FLAG_TOOL)) {
                           node->slots_inuse--;
                       }
                       break;
                   }
               }
           }
           pmix_pointer_array_set_item(jdata->procs, p, NULL);
           PMIX_RELEASE(proc);
       }
       jdata->num_procs = 0;
       jdata->num_launched = 0;
   }

After remapping, the job re-enters ``prte_rmaps_base_map_job()`` which
re-creates proc objects on the surviving nodes using the original
``app->num_procs`` counts.

Step 5 — Detect target departure in the errmgr and notify completion
--------------------------------------------------------------------

Campaign completion is driven entirely by the daemon-loss path: when a
targeted daemon leaves the DVM, the HNP's comm-failure handler matches its
rank against the active campaigns and drives the fence down.  This is the
same event whether the daemon exited cleanly in response to the shrink
command or crashed, so a single code path covers both — there is no separate
"acknowledgement" message and no fallback to reconcile.

In ``src/mca/errmgr/dvm/errmgr_dvm.c``, inside the ``PMIX_CHECK_NSPACE``
daemon-proc block of ``proc_errors()`` (line 252), within the
``PRTE_PROC_STATE_COMM_FAILED`` / heartbeat-failed handler, add after the
"mark daemon as gone" logic:

.. code-block:: c

   /* check if this daemon was a pending shrink target */
   {
       prte_shrink_campaign_t *_camp, *_next;
       int _t;
       PMIX_LIST_FOREACH_SAFE(_camp, _next,
                              &prte_shrink_campaigns, prte_shrink_campaign_t) {
           for (_t = 0; _t < _camp->ntargets; _t++) {
               if (_camp->targets[_t] != proc->rank) continue;
               /* stamp this slot so a repeated comm event for the same
                * daemon cannot decrement the campaign twice */
               _camp->targets[_t] = PMIX_RANK_INVALID;
               _camp->pending--;
               prte_dvm_launch_fence--;
               if (0 == _camp->pending) {
                   /* this request's shrink is complete — notify the
                    * requester that the DVM now reflects the new size */
                   if (_camp->have_requester) {
                       /* success == true => PMIX_DVM_IS_READY */
                       prte_plm_base_dvm_mod_notify(&_camp->requester,
                                                    _camp->alloc_id,
                                                    _camp->req_id,
                                                    true, PMIX_SUCCESS);
                   }
                   pmix_list_remove_item(&prte_shrink_campaigns,
                                        &_camp->super);
                   PMIX_RELEASE(_camp);
               }
               if (0 == prte_dvm_launch_fence) {
                   prte_plm_base_fence_release(true);
               }
               goto errmgr_shrink_done;
           }
       }
       errmgr_shrink_done: ;
   }

Because the progress thread is single-threaded, the counter decrements and
the list manipulation are atomic with respect to all other state machine
callbacks.  Stamping the matched slot ``PMIX_RANK_INVALID`` makes the
decrement idempotent: should the daemon generate more than one failure event,
only the first is counted.  A daemon that crashes during a shrink is handled
identically to one that exits cleanly — the node was being removed anyway,
and jobs mapped to it are detected by ``prte_plm_base_job_needs_remap()`` and
re-routed to surviving nodes.

The ``PMIX_DVM_IS_READY`` notification is **per campaign**: it fires when *this*
request's last target departs, regardless of whether other (grow or shrink)
campaigns keep the shared fence nonzero.  The fence-release of held jobs, by
contrast, waits for the global fence to reach zero.  The failure counterpart
(``PMIX_ERR_DVM_MOD``) is emitted only on the xcast-failure cleanup in Step 1 —
once the shrink command is on the wire, every targeted daemon's departure is a
*success* for the campaign, since clean exit and crash are indistinguishable
and both remove the node as requested.

Summary of Files Changed (Shrink Fence)
-----------------------------------------

.. list-table::
   :widths: 50 50
   :header-rows: 1

   * - File
     - Change
   * - ``src/runtime/prte_globals.h``
     - Declare ``prte_shrink_campaign_t`` (type + ``PMIX_CLASS_DECLARATION``,
       including the requester fields) and ``prte_shrink_campaigns``
       (``pmix_list_t``).
   * - ``src/runtime/prte_globals.c``
     - Define ``PMIX_CLASS_INSTANCE`` for ``prte_shrink_campaign_t``
       (destructor frees ``targets``, ``alloc_id``, ``req_id``).  Define
       ``prte_shrink_campaigns``.
   * - ``src/runtime/prte_init.c``
     - ``PMIX_CONSTRUCT(&prte_shrink_campaigns, pmix_list_t)`` alongside
       ``prte_held_jobs`` initialization.
   * - ``src/runtime/prte_finalize.c``
     - ``PMIX_LIST_DESTRUCT(&prte_shrink_campaigns)``.
   * - ``src/mca/ras/base/ras_base_allocate.c``
     - In ``PMIX_ALLOC_RELEASE`` branch of
       ``prte_ras_base_complete_request()``: create a ``prte_shrink_campaign_t``,
       copy the rank array into it, record the requester / ``PMIX_ALLOC_ID`` /
       ``PMIX_ALLOC_REQ_ID``, append to ``prte_shrink_campaigns``, and increment
       ``prte_dvm_launch_fence`` by ``m`` — all before ``free(ranks)``.  Add
       xcast-failure cleanup that removes the campaign, decrements the fence,
       and emits ``PMIX_ERR_DVM_MOD`` to the requester.
   * - ``src/prted/prted_comm.c``
     - In ``PRTE_DAEMON_SHRINK_CMD`` handler: after the ``JOB_END``
       notification wait, activate ``PRTE_JOB_STATE_DAEMONS_TERMINATED`` and
       exit.  No acknowledgement is sent; the HNP detects departure via the
       comm-failure path.
   * - ``src/mca/plm/base/plm_base_launch_support.c``
     - Add ``prte_plm_base_job_needs_remap()`` and
       ``prte_plm_base_reset_proc_map()``.  Add hold check in
       ``prte_plm_base_launch_apps()`` on
       ``!pmix_list_is_empty(&prte_shrink_campaigns)``.
   * - ``src/mca/plm/base/plm_base_launch_support.h``
     - Declare the two remap helpers.
   * - ``src/mca/errmgr/dvm/errmgr_dvm.c``
     - In ``proc_errors()``, daemon-comm-failure block: search
       ``prte_shrink_campaigns`` for the dead daemon's rank; if found, stamp
       the matched target slot ``PMIX_RANK_INVALID``, decrement campaign
       ``pending`` and fence; when ``pending`` hits zero emit
       ``PMIX_DVM_IS_READY`` to the requester and remove the campaign; call
       ``prte_plm_base_fence_release(true)`` when the fence hits zero.  This is
       the sole shrink-completion trigger.

The shared infrastructure this path relies on — the fence counter, held-job
arrays, ``VM_READY → MAP`` hold point, ``prte_plm_base_fence_release()``, and
the ``prte_plm_base_dvm_mod_notify()`` completion-event helper — is listed in
the "Shared Fence Infrastructure" table in :ref:`elastic-dvm-plan-label`.

Design Invariants
-----------------

* ``prte_shrink_campaigns`` is a ``pmix_list_t``; each entry covers exactly
  one ``PMIX_ALLOC_RELEASE`` request.  Multiple concurrent shrink campaigns
  are supported.
* The fence is incremented by exactly ``m`` at campaign creation and
  decremented by 1 for each targeted daemon whose departure is detected on
  the errmgr comm-failure path (clean exit and crash are indistinguishable
  and handled identically).  Each target slot is stamped ``PMIX_RANK_INVALID``
  once counted, so a repeated comm event cannot decrement twice.  A campaign
  is removed from the list when its ``pending`` count reaches zero.
* The ``LAUNCH_APPS`` hold uses ``!pmix_list_is_empty(&prte_shrink_campaigns)``,
  not ``prte_dvm_launch_fence > 0``, so a concurrent grow does not stall
  already-mapped jobs on surviving nodes.
* ``prte_shrink_campaigns`` is stable throughout each campaign: the targets
  array for a given campaign is valid from creation through removal, so
  ``prte_plm_base_job_needs_remap()`` can safely iterate it during release.
* Jobs in ``prte_prelaunch_held_jobs`` hold a ``PMIX_RETAIN`` reference;
  ``prte_plm_base_fence_release()`` releases it after re-activating or
  aborting the job.
* The completion event is per campaign and fires from the campaign-removal
  point (``pending == 0``), so each accepted release yields exactly one
  ``PMIX_DVM_IS_READY`` (success) or, on an xcast failure at creation, exactly
  one ``PMIX_ERR_DVM_MOD`` — never both, and never for a scheduler-driven
  release with no requester.

Follow-up — collective shrink completion
-----------------------------------------

The shrink design above drains a campaign **one daemon at a time**: every
targeted daemon exits on its own and the HNP discovers each departure
independently through the errmgr comm-failure path (Step 5), decrementing the
campaign's ``pending`` count and the fence once per death.  This is correct,
but each individual departure drives a separate routing-tree repair —
``prte_rml_repair_routing_tree()`` runs ``handle_promotion()`` and
``update_descendants()`` for that single rank.  Shrinking ``m`` daemons that
sit along one branch of the radix routing tree therefore triggers up to ``m``
sequential promotions/descendant rewrites, which review of PR `#2472
<https://github.com/openpmix/prrte/pull/2472>`_ flagged as potentially
expensive for a large shrink (unprofiled).

A **collective** completion scheme would repair the tree once per campaign
instead of once per daemon:

#. Broadcast the ``PRTE_DAEMON_SHRINK_CMD`` as today.
#. Each targeted daemon records that it is leaving and does any local
   processing, but **does not exit yet**.
#. The HNP hooks the *broadcast's* completion.  The reliable xcast in
   ``src/mca/grpcomm/direct/grpcomm_direct_xcast.c`` already tracks completion
   via ACKs flowing up the tree (``finish_op`` on the master means every daemon
   received the op); a per-op completion callback would be added, or the shrink
   xcast special-cased, to fire a handler at that point.
#. That handler reports **all** of the campaign's targets as failed in a single
   batch via ``prte_rml_repair_routing_tree(failed_ranks, /*global=*/false)``,
   which already accepts a rank array and performs one promotion/descendant
   pass for the whole set (``src/rml/routed_radix.c``).
#. Each doomed daemon exits once its lifeline disconnects — driven by the batch
   failure report — rather than self-exiting.

This is **not** the per-daemon acknowledgement that the
*Design Decision — Complete on Death, Not on Acknowledgement* section above
rejected.  That ACK announced a daemon's *intent* to leave and, acted upon,
would have released held jobs while the HNP still believed the daemon present.
Here the authoritative HNP-side teardown (route removal, ``num_daemons``, node
state) still happens at the batch failure report, and held jobs are released
only after it — so the invariant "act once teardown has occurred, not on
intent" is preserved.  The doomed daemons exiting slightly later is harmless:
they are already excluded from routing and mapping, so released jobs remap onto
survivors correctly.  Completion would collapse to a single event per campaign,
which also retires the per-rank ``PMIX_RANK_INVALID`` idempotency stamping and
the double-count analysis that the per-death path requires.  The completion
event (``PMIX_DVM_IS_READY``) would then fire from that single batch point
rather than from the last individual departure.

This is an **optimization, not a correctness fix**, so it is deliberately
deferred out of the launch-fence work:

* It needs a new xcast-completion callback hook (or a shrink special case).
* It moves the daemon-side ``PRTE_DAEMON_SHRINK_CMD`` handler from self-exit to
  record-and-wait-for-lifeline.
* It moves the fence-completion trigger from the errmgr comm-failure path to the
  batch-repair callback, and must verify that all daemon-loss bookkeeping that
  currently rides on the comm-failure event still runs when the loss is
  declared proactively.
* The collective / whole-branch failure path is far less exercised than
  individual daemon deaths and may surface bugs in the tree-repair and
  fault-handling code, so it warrants validation against large multi-daemon and
  single-branch shrinks.
