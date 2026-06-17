.. _dvm-launch-fence-label:

DVM Launch-Fence Implementation Plan
=====================================

This document describes the implementation plan for eliminating the race
condition between a DVM extension (new-daemon launch campaign) and
concurrently-running application jobs.  For background on the race itself see
:ref:`state-machine-label`, section *DVM Extension and the Daemon-Launch Race*.

The mechanism is a **global launch fence** — a counter
(``prte_dvm_launch_fence``) that tracks the number of in-progress daemon
launch campaigns.  An app job that reaches the ``VM_READY → MAP`` transition
checks the fence; if it is nonzero the job parks itself in a held-job array
(``prte_held_jobs``) and is released when the fence reaches zero.

The state machine is single-threaded on the progress thread, so no locking
is required anywhere in this plan.

.. note::
   The app-triggered expansion path (``--add-host`` / ``--add-hostfile``)
   already sets ``prte_dvm_ready = false`` in ``add_hosts()`` before posting
   the asynchronous RAS modify request, which causes newly-arriving jobs to be
   stashed in ``prte_cache`` rather than dispatched immediately.  The launch
   fence is still required for the scheduler-push path (e.g., Slurm firing
   ``LAUNCH_DAEMONS`` directly) where ``prte_dvm_ready`` is never cleared, and
   to ensure full correctness when both paths can interleave.

Step 1 — New state constant
---------------------------

In ``src/mca/plm/plm_types.h``, add:

.. code-block:: c

   /* value 17 is currently unused in the running-state band */
   #define PRTE_JOB_STATE_WAITING_FOR_DAEMONS  17

Add a corresponding string to ``src/util/error_strings.c``.

This state is used purely as a marker so that debugging tools and verbose
output show clearly why a job is parked; no callback is registered for it.

Step 2 — New global fence and held-job arrays
---------------------------------------------

In ``src/runtime/prte_globals.c`` and ``src/runtime/prte_globals.h``, add:

.. code-block:: c

   /* counts in-progress daemon launch campaigns */
   int prte_dvm_launch_fence = 0;

   /* jobs parked at the VM_READY → MAP boundary */
   pmix_pointer_array_t *prte_held_jobs;

   /* jobs parked at the LAUNCH_APPS boundary during a shrink */
   pmix_pointer_array_t *prte_prelaunch_held_jobs;

Initialize both arrays in ``src/runtime/prte_init.c`` alongside the existing
``prte_cache`` initialization:

.. code-block:: c

   prte_held_jobs = PMIX_NEW(pmix_pointer_array_t);
   pmix_pointer_array_init(prte_held_jobs, 1, INT_MAX, 1);

   prte_prelaunch_held_jobs = PMIX_NEW(pmix_pointer_array_t);
   pmix_pointer_array_init(prte_prelaunch_held_jobs, 1, INT_MAX, 1);

Destruct both in ``src/runtime/prte_finalize.c``.

Step 3 — Increment the fence in setup_virtual_machine
------------------------------------------------------

In ``src/mca/plm/base/plm_base_launch_support.c``, inside
``prte_plm_base_setup_virtual_machine()``, the block at line 2380 already
sets ``PRTE_JOB_LAUNCHED_DAEMONS`` when ``map->num_new_daemons > 0``:

.. code-block:: c

   /* existing code */
   if (0 < map->num_new_daemons) {
       rc = prte_set_attribute(&jdata->attributes,
                               PRTE_JOB_LAUNCHED_DAEMONS, true, NULL, PMIX_BOOL);
       if (PRTE_SUCCESS != rc) { ... }
       /* ADD: increment the fence */
       prte_dvm_launch_fence++;
   }

This location is reached by all four PLM backends (ssh, slurm, pals, lsf)
through a single common code path, so a single insertion covers every
extension scenario.

Both expansion paths ultimately fire ``PRTE_JOB_STATE_LAUNCH_DAEMONS`` on
the **daemon job** (app-triggered via ``prte_ras_base_complete_request()``;
scheduler-push directly from the Slurm RAS module), so ``setup_virtual_machine()``
always operates on the daemon job when an extension is in progress.  The
``PRTE_JOB_EXTEND_DVM`` attribute is therefore set on the daemon job in both
cases.

Step 4 — Decrement the fence and release held jobs in vm_ready
--------------------------------------------------------------

In ``src/mca/state/dvm/state_dvm.c``, ``vm_ready()`` has an outer block
conditioned on ``PRTE_JOB_LAUNCHED_DAEMONS`` (line 275).  Inside that block,
a nested ``if (!PRTE_JOB_DO_NOT_LAUNCH && 1 < prte_process_info.num_daemons)``
block (lines 278–331) sends the nidmap xcast; the outer block closes at
line 332.

The fence decrement must be placed **just before the closing ``}`` of the
outer** ``PRTE_JOB_LAUNCHED_DAEMONS`` **block** (between the inner block and
line 332).  This position is reached by both the xcast path and the
``DO_NOT_LAUNCH`` / single-daemon path:

.. code-block:: c

   if (prte_get_attribute(...PRTE_JOB_LAUNCHED_DAEMONS...)) {
       if (!DO_NOT_LAUNCH && 1 < prte_process_info.num_daemons) {
           /* existing xcast code (lines 283–330) */
           /* error exits within this block each need:
            *   prte_dvm_launch_fence--;
            *   if (0 == prte_dvm_launch_fence)
            *       prte_plm_base_fence_release(false);
            * before PRTE_ACTIVATE_JOB_STATE / return          */
           PMIX_DATA_BUFFER_DESTRUCT(&buf);   /* line 330 */
       }
       /* ADD: success path (and DO_NOT_LAUNCH / single-daemon path) */
       prte_dvm_launch_fence--;
       if (0 == prte_dvm_launch_fence) {
           prte_plm_base_fence_release(true);
       }
   }   /* line 332 */

Error exits inside the inner block (the ``PRTE_ACTIVATE_JOB_STATE(NULL,
PRTE_JOB_STATE_FORCED_EXIT)`` branches at lines 288, 302, 310, 317, 327)
return without reaching the decrement above, so each one must do its own
decrement + ``prte_plm_base_fence_release(false)`` before returning.

The held jobs re-enter ``vm_ready``; at that point ``PRTE_JOB_LAUNCHED_DAEMONS``
is not set on them and the fence is zero, so they fall through to the
``preposition_files → MAP`` path normally.

Step 5 — Park jobs at the VM_READY → MAP boundary
--------------------------------------------------

In ``vm_ready()``, the code at line 360 is reached only by app jobs (the
daemon-job branch returns at line 357).  This is immediately before
``prte_filem.preposition_files()`` which leads to ``files_ready → MAP``.
Add the hold check here:

.. code-block:: c

   /* position any required files */
   if (0 < prte_dvm_launch_fence) {
       /* daemon launch in progress — park this job */
       caddy->jdata->state = PRTE_JOB_STATE_WAITING_FOR_DAEMONS;
       PMIX_RETAIN(caddy->jdata);
       pmix_pointer_array_add(prte_held_jobs, caddy->jdata);
       PMIX_RELEASE(caddy);
       return;
   }
   if (PRTE_SUCCESS !=
           prte_filem.preposition_files(caddy->jdata, files_ready, caddy->jdata)) {
       PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FILES_POSN_FAILED);
   }
   PMIX_RELEASE(caddy);

Step 6 — Handle daemon launch failure
--------------------------------------

If daemon launch fails, the normal ``vm_ready`` decrement path is never
reached.  There are two distinct failure modes, each needing its own guard.

**6a — xcast build/send errors inside** ``vm_ready``

The error-exit paths inside the ``!DO_NOT_LAUNCH`` block (lines 288, 302,
310, 317, 327) call ``PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT)``
and return before reaching the common decrement at the bottom of the outer
block.  Each of these branches must decrement and release explicitly, as
described in Step 4.

**6b — daemon crash after fence increment, before** ``vm_ready``

A daemon might crash between ``setup_virtual_machine()`` (which increments
the fence) and the ``DAEMONS_REPORTED → VM_READY`` transition.  In this case
``vm_ready`` is never reached; the crash is routed to
``errmgr_dvm.c:proc_errors()``.

In ``src/mca/errmgr/dvm/errmgr_dvm.c``, inside the ``PMIX_CHECK_NSPACE``
daemon block (line 252), within the ``PRTE_PROC_STATE_COMM_FAILED`` /
``PRTE_PROC_STATE_HEARTBEAT_FAILED`` handler, add after the "mark daemon as
gone" logic and before the early ``goto cleanup``:

.. code-block:: c

   /* if a grow campaign was in progress, fail the fence now so
    * held jobs are not parked indefinitely */
   if (0 < prte_dvm_launch_fence &&
       prte_get_attribute(&jdata->attributes,
                          PRTE_JOB_LAUNCHED_DAEMONS, NULL, PMIX_BOOL)) {
       prte_dvm_launch_fence--;
       if (0 == prte_dvm_launch_fence) {
           prte_plm_base_fence_release(false);
       }
       /* remove the attribute so a second daemon crash in the same
        * campaign does not decrement the fence again */
       prte_remove_attribute(&jdata->attributes, PRTE_JOB_LAUNCHED_DAEMONS);
   }

**6c —** ``check_complete`` **safety net**

If daemon launch fails through ``PRTE_JOB_STATE_TERMINATED`` on the daemon
job (reached via the errmgr abort path), add a fence check **inside** the
``if (NULL == jdata || PMIX_CHECK_NSPACE(...))`` block of ``check_complete``
(line 539), before the early return at line 553:

.. code-block:: c

   if (NULL == jdata || PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
       /* existing NULL / daemon-count checks ... */

       /* ADD: if the daemon job held the grow fence, release it */
       if (NULL != jdata &&
           prte_get_attribute(&jdata->attributes,
                              PRTE_JOB_LAUNCHED_DAEMONS, NULL, PMIX_BOOL) &&
           0 < prte_dvm_launch_fence) {
           prte_dvm_launch_fence--;
           if (0 == prte_dvm_launch_fence) {
               prte_plm_base_fence_release(false);
           }
       }
       /* existing early return ... */

Note: this block is the only path in ``check_complete`` that is reached by
the daemon job.  The ``jdata->state > PRTE_JOB_STATE_UNTERMINATED`` check
at line 566 is not reachable for the daemon job.

----

DVM Shrink Fence
================

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

Race 1 is covered by the existing grow-fence mechanism (the fence is also
incremented for shrink — see Step 7).  Race 2 requires a second hold point
guarded by checking the shrink campaign list (nonempty only during shrink)
so that a concurrent grow does not unnecessarily hold jobs that have already
been mapped to surviving nodes.

Multiple concurrent shrink campaigns are supported: each campaign tracks its
own pending-ACK count and is removed from the list when all its targets have
confirmed exit.

Step 7 — Shrink campaign type, list, and fence increment
---------------------------------------------------------

**7a — Campaign type**

Add the following type to ``src/runtime/prte_globals.h`` and define the
class instance in ``src/runtime/prte_globals.c``:

.. code-block:: c

   /* one entry per in-progress shrink campaign */
   typedef struct {
       pmix_list_item_t super;
       pmix_rank_t     *targets;   /* daemon ranks being terminated */
       int              ntargets;  /* initial count */
       int              pending;   /* remaining ACKs expected */
   } prte_shrink_campaign_t;
   PMIX_CLASS_DECLARATION(prte_shrink_campaign_t);

In ``src/runtime/prte_globals.c``:

.. code-block:: c

   static void campaign_destruct(prte_shrink_campaign_t *p)
   {
       free(p->targets);
   }
   PMIX_CLASS_INSTANCE(prte_shrink_campaign_t, pmix_list_item_t,
                       NULL, campaign_destruct);

**7b — Global list**

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

**7c — Populate campaign and increment fence**

In ``src/mca/ras/base/ras_base_allocate.c``, the ``PMIX_ALLOC_RELEASE``
branch of ``prte_ras_base_complete_request()`` builds the daemon rank array
(``ranks``, count ``m``) and then calls ``free(ranks)`` at line 760 before
the xcast at line 763.  Insert the campaign setup **before** ``free(ranks)``
(between lines 758 and 760):

.. code-block:: c

   /* record the campaign — must be before free(ranks) */
   {
       prte_shrink_campaign_t *_camp = PMIX_NEW(prte_shrink_campaign_t);
       _camp->targets = (pmix_rank_t *) malloc(m * sizeof(pmix_rank_t));
       memcpy(_camp->targets, ranks, m * sizeof(pmix_rank_t));
       _camp->ntargets = m;
       _camp->pending  = m;
       pmix_list_append(&prte_shrink_campaigns, &_camp->super);
       prte_dvm_launch_fence += m;
   }
   free(ranks);   /* existing line 760 */

   /* existing xcast */
   if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
       /* clean up the campaign we just added */
       prte_shrink_campaign_t *_camp =
           (prte_shrink_campaign_t *) pmix_list_remove_last(&prte_shrink_campaigns);
       prte_dvm_launch_fence -= _camp->pending;
       PMIX_RELEASE(_camp);
       PRTE_ERROR_LOG(rc);
   }

Because the campaign is appended before the xcast, any ``VM_READY`` event
that fires on the progress thread after this point will see a nonzero fence
and park the job.

Step 8 — Daemon shrink acknowledgement
---------------------------------------

A daemon that decides to exit in response to ``PRTE_DAEMON_SHRINK_CMD``
must send an acknowledgement to the HNP **before** it activates
``PRTE_JOB_STATE_DAEMONS_TERMINATED``, so the HNP can track completions.

Add a new PLM command constant in ``src/mca/plm/plm_types.h``:

.. code-block:: c

   #define PRTE_PLM_SHRINK_ACK_CMD  7   /* daemon → HNP: I am exiting due to shrink */

In ``src/prted/prted_comm.c``, in the ``PRTE_DAEMON_SHRINK_CMD`` handler
(line 469), after ``PRTE_PMIX_WAIT_THREAD(&lk)`` completes the
``PMIX_EVENT_JOB_END`` notification but *before*
``PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED)``,
insert:

.. code-block:: c

   {
       pmix_data_buffer_t _ack;
       prte_plm_cmd_flag_t _cmd = PRTE_PLM_SHRINK_ACK_CMD;
       pmix_rank_t _myrank = PRTE_PROC_MY_NAME->rank;
       PMIX_DATA_BUFFER_CONSTRUCT(&_ack);
       PMIx_Data_pack(NULL, &_ack, &_cmd, 1, PMIX_UINT8);
       PMIx_Data_pack(NULL, &_ack, &_myrank, 1, PMIX_PROC_RANK);
       prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, &_ack,
                               PRTE_RML_TAG_PLM,
                               prte_rml_send_callback, NULL);
   }

The rank is packed so the HNP can match the ACK to the correct campaign
entry.  The send is non-blocking; the daemon proceeds to shut down
immediately after posting it.

Step 9 — Second hold point at LAUNCH_APPS
------------------------------------------

In ``src/mca/plm/base/plm_base_launch_support.c``,
``prte_plm_base_launch_apps()`` (line 817), add a check after the job-state
guard but before packing any data:

.. code-block:: c

   /* if a shrink is in progress, hold this job until all targeted
    * daemons have confirmed exit, to prevent sending launch data to
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
any shrink is in progress.

Step 10 — Fence-release helper
--------------------------------

Both grow completion (``vm_ready``) and shrink ACK receipt decrement the
fence and, when it hits zero, must release two classes of held jobs.
Extract this logic into a single helper declared in
``src/mca/plm/base/plm_base_launch_support.h`` and defined in
``plm_base_launch_support.c``:

.. code-block:: c

   void prte_plm_base_fence_release(bool success)
   {
       int _hi;
       prte_job_t *_held;

       /* --- pre-map held jobs (parked at VM_READY) --- */
       for (_hi = 0; _hi < prte_held_jobs->size; _hi++) {
           _held = (prte_job_t *)
               pmix_pointer_array_get_item(prte_held_jobs, _hi);
           if (NULL == _held) continue;
           pmix_pointer_array_set_item(prte_held_jobs, _hi, NULL);
           PRTE_ACTIVATE_JOB_STATE(_held,
               success ? PRTE_JOB_STATE_VM_READY
                       : PRTE_JOB_STATE_NEVER_LAUNCHED);
           PMIX_RELEASE(_held);
       }

       /* --- pre-launch held jobs (parked at LAUNCH_APPS) --- */
       for (_hi = 0; _hi < prte_prelaunch_held_jobs->size; _hi++) {
           _held = (prte_job_t *)
               pmix_pointer_array_get_item(prte_prelaunch_held_jobs, _hi);
           if (NULL == _held) continue;
           pmix_pointer_array_set_item(prte_prelaunch_held_jobs, _hi, NULL);
           if (!success) {
               PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_NEVER_LAUNCHED);
           } else if (prte_plm_base_job_needs_remap(_held)) {
               prte_plm_base_reset_proc_map(_held);
               PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_MAP);
           } else {
               PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_LAUNCH_APPS);
           }
           PMIX_RELEASE(_held);
       }

       /* campaigns are removed individually as their last ACK arrives;
        * the list should be empty here, but do a safety sweep */
       prte_shrink_campaign_t *_camp, *_next;
       PMIX_LIST_FOREACH_SAFE(_camp, _next,
                              &prte_shrink_campaigns, prte_shrink_campaign_t) {
           pmix_list_remove_item(&prte_shrink_campaigns, &_camp->super);
           PMIX_RELEASE(_camp);
       }
   }

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

Step 11 — Handle PRTE_PLM_SHRINK_ACK_CMD in plm_base_receive.c
----------------------------------------------------------------

In ``src/mca/plm/base/plm_base_receive.c``, add a case to the PLM receive
switch (alongside ``PRTE_PLM_LOCAL_LAUNCH_COMP_CMD``, etc.).  The ACK
message carries the sending daemon's rank (packed by Step 8); unpack it to
locate and update the correct campaign:

.. code-block:: c

   case PRTE_PLM_SHRINK_ACK_CMD: {
       pmix_rank_t _drank;
       size_t _n = 1;
       prte_shrink_campaign_t *_camp;
       int _t;

       if (PMIX_SUCCESS !=
               PMIx_Data_unpack(NULL, buffer, &_drank, &_n, PMIX_PROC_RANK)) {
           PRTE_ERROR_LOG(PRTE_ERR_UNPACK_FAILURE);
           break;
       }
       PMIX_LIST_FOREACH(_camp, &prte_shrink_campaigns,
                         prte_shrink_campaign_t) {
           for (_t = 0; _t < _camp->ntargets; _t++) {
               if (_camp->targets[_t] != _drank) continue;
               _camp->pending--;
               prte_dvm_launch_fence--;
               if (0 == _camp->pending) {
                   pmix_list_remove_item(&prte_shrink_campaigns,
                                        &_camp->super);
                   PMIX_RELEASE(_camp);
               }
               if (0 == prte_dvm_launch_fence) {
                   prte_plm_base_fence_release(true);
               }
               goto shrink_ack_done;
           }
       }
       /* rank not found in any campaign — log and ignore */
       PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                            "%s shrink ACK from unknown daemon %lu",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (unsigned long) _drank));
       shrink_ack_done:
       break;
   }

Because the progress thread is single-threaded, the counter decrements and
the list manipulation are atomic with respect to all other state machine
callbacks.

Step 12 — Errmgr fallback for crashed shrink daemons
-----------------------------------------------------

A daemon might crash before it can send ``PRTE_PLM_SHRINK_ACK_CMD``.
Without a fallback, the fence would remain nonzero and held jobs would park
indefinitely.

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
               _camp->pending--;
               prte_dvm_launch_fence--;
               if (0 == _camp->pending) {
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

This ensures that a daemon crash during a shrink does not permanently stall
held jobs.  The node was being removed anyway; jobs that were mapped to it
will be detected by ``prte_plm_base_job_needs_remap()`` and re-routed to
surviving nodes.

Summary of Files Changed (Shrink Fence)
-----------------------------------------

.. list-table::
   :widths: 50 50
   :header-rows: 1

   * - File
     - Change
   * - ``src/mca/plm/plm_types.h``
     - Add ``PRTE_PLM_SHRINK_ACK_CMD = 7``.
   * - ``src/runtime/prte_globals.h``
     - Declare ``prte_shrink_campaign_t`` (type + ``PMIX_CLASS_DECLARATION``)
       and ``prte_shrink_campaigns`` (``pmix_list_t``).
   * - ``src/runtime/prte_globals.c``
     - Define ``PMIX_CLASS_INSTANCE`` for ``prte_shrink_campaign_t``
       (destructor frees ``targets``).  Define ``prte_shrink_campaigns``.
   * - ``src/runtime/prte_init.c``
     - ``PMIX_CONSTRUCT(&prte_shrink_campaigns, pmix_list_t)`` alongside
       ``prte_held_jobs`` initialization.
   * - ``src/runtime/prte_finalize.c``
     - ``PMIX_LIST_DESTRUCT(&prte_shrink_campaigns)``.
   * - ``src/mca/ras/base/ras_base_allocate.c``
     - In ``PMIX_ALLOC_RELEASE`` branch of
       ``prte_ras_base_complete_request()``: create a ``prte_shrink_campaign_t``,
       copy the rank array into it, append to ``prte_shrink_campaigns``, and
       increment ``prte_dvm_launch_fence`` by ``m`` — all before
       ``free(ranks)``.  Add xcast-failure cleanup that removes the campaign
       and decrements the fence.
   * - ``src/prted/prted_comm.c``
     - In ``PRTE_DAEMON_SHRINK_CMD`` handler: after the ``JOB_END``
       notification wait, pack ``PRTE_PLM_SHRINK_ACK_CMD`` and
       ``PRTE_PROC_MY_NAME->rank`` into a message and send to HNP on
       ``PRTE_RML_TAG_PLM`` before activating
       ``PRTE_JOB_STATE_DAEMONS_TERMINATED``.
   * - ``src/mca/plm/base/plm_base_launch_support.c``
     - Add ``prte_plm_base_fence_release()``,
       ``prte_plm_base_job_needs_remap()``, and
       ``prte_plm_base_reset_proc_map()``.
       Add hold check in ``prte_plm_base_launch_apps()`` on
       ``!pmix_list_is_empty(&prte_shrink_campaigns)``.
   * - ``src/mca/plm/base/plm_base_launch_support.h``
     - Declare the three new functions.
   * - ``src/mca/plm/base/plm_base_receive.c``
     - Handle ``PRTE_PLM_SHRINK_ACK_CMD``: unpack sender rank, find the
       owning campaign, decrement per-campaign ``pending`` and
       ``prte_dvm_launch_fence``; remove campaign when ``pending`` hits zero;
       call ``prte_plm_base_fence_release(true)`` when fence hits zero.
   * - ``src/mca/state/dvm/state_dvm.c``
     - Replace inline release loop in ``vm_ready`` and ``check_complete``
       with calls to ``prte_plm_base_fence_release()``.
   * - ``src/mca/errmgr/dvm/errmgr_dvm.c``
     - In ``proc_errors()``, daemon-comm-failure block: search
       ``prte_shrink_campaigns`` for the dead daemon's rank; if found,
       decrement campaign ``pending`` and fence; remove campaign when
       ``pending`` hits zero; call ``prte_plm_base_fence_release(true)``
       when fence hits zero.

Summary of Files Changed (Grow Fence)
--------------------------------------

.. list-table::
   :widths: 50 50
   :header-rows: 1

   * - File
     - Change
   * - ``src/mca/plm/plm_types.h``
     - Add ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS = 17``.
   * - ``src/util/error_strings.c``
     - Add string for ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS``.
   * - ``src/runtime/prte_globals.h``
     - Declare ``prte_dvm_launch_fence``, ``prte_held_jobs``, and
       ``prte_prelaunch_held_jobs``.
   * - ``src/runtime/prte_globals.c``
     - Define and initialize ``prte_dvm_launch_fence = 0``.
   * - ``src/runtime/prte_init.c``
     - Allocate and init ``prte_held_jobs`` and ``prte_prelaunch_held_jobs``.
   * - ``src/runtime/prte_finalize.c``
     - Destruct ``prte_held_jobs`` and ``prte_prelaunch_held_jobs``.
   * - ``src/mca/plm/base/plm_base_launch_support.c``
     - Increment ``prte_dvm_launch_fence`` in the
       ``0 < map->num_new_daemons`` block at the end of
       ``prte_plm_base_setup_virtual_machine()``.
       Declare and define ``prte_plm_base_fence_release()`` (Step 10).
   * - ``src/mca/state/dvm/state_dvm.c``
     - In ``vm_ready``: (a) decrement fence and call
       ``prte_plm_base_fence_release(true)`` at the bottom of the outer
       ``PRTE_JOB_LAUNCHED_DAEMONS`` block; (b) call
       ``prte_plm_base_fence_release(false)`` in all inner error-exit paths;
       (c) add hold-check before ``preposition_files``.
       In ``check_complete``: call ``prte_plm_base_fence_release(false)``
       inside the ``PMIX_CHECK_NSPACE`` daemon block when a job with
       ``PRTE_JOB_LAUNCHED_DAEMONS`` terminates abnormally.
   * - ``src/mca/errmgr/dvm/errmgr_dvm.c``
     - In ``proc_errors()``, daemon-comm-failure block: if
       ``prte_dvm_launch_fence > 0`` and ``PRTE_JOB_LAUNCHED_DAEMONS`` is
       set on the daemon job, decrement fence and call
       ``prte_plm_base_fence_release(false)`` if fence hits zero; then
       remove ``PRTE_JOB_LAUNCHED_DAEMONS`` to prevent double-decrement.

Design Invariants
-----------------

**Grow fence**

* The fence counter and both held-job arrays are only accessed on the PRRTE
  progress thread, so no locking is required anywhere.
* The grow contribution to ``prte_dvm_launch_fence`` is incremented exactly
  once per daemon launch campaign in ``setup_virtual_machine()``, which all
  PLM backends call.  Symmetric decrement is at the bottom of the outer
  ``PRTE_JOB_LAUNCHED_DAEMONS`` block in ``vm_ready`` (success or
  ``DO_NOT_LAUNCH`` path), in each inner error-exit branch of ``vm_ready``
  (failure), in the ``check_complete`` daemon block (late failure), and in
  ``errmgr_dvm.c:proc_errors()`` (daemon crash during launch).
* After ``prte_remove_attribute(...PRTE_JOB_LAUNCHED_DAEMONS...)`` is called
  in the errmgr crash path, subsequent crashes in the same campaign do not
  double-decrement the fence.
* Jobs in ``prte_held_jobs`` hold a ``PMIX_RETAIN`` reference; the release
  loop in ``prte_plm_base_fence_release()`` performs the matching
  ``PMIX_RELEASE``.
* Jobs already past ``MAP`` when a grow starts are unaffected: they were
  mapped to existing nodes whose daemons are already running.

**Shrink fence**

* ``prte_shrink_campaigns`` is a ``pmix_list_t``; each entry covers exactly
  one ``PMIX_ALLOC_RELEASE`` request.  Multiple concurrent shrink campaigns
  are supported.
* The fence is incremented by exactly ``m`` at campaign creation and
  decremented by 1 for each ``PRTE_PLM_SHRINK_ACK_CMD`` received or for
  each crash detected in the errmgr fallback.  A campaign is removed from
  the list when its ``pending`` count reaches zero.
* The ``LAUNCH_APPS`` hold uses ``!pmix_list_is_empty(&prte_shrink_campaigns)``,
  not ``prte_dvm_launch_fence > 0``, so a concurrent grow does not stall
  already-mapped jobs on surviving nodes.
* ``prte_shrink_campaigns`` is stable throughout each campaign: the targets
  array for a given campaign is valid from creation through removal, so
  ``prte_plm_base_job_needs_remap()`` can safely iterate it during release.
* Jobs in ``prte_prelaunch_held_jobs`` hold a ``PMIX_RETAIN`` reference;
  ``prte_plm_base_fence_release()`` releases it after re-activating or
  aborting the job.
* ``prte_plm_base_fence_release()`` is called only when
  ``prte_dvm_launch_fence`` reaches zero, which requires all grow campaigns
  and all shrink campaigns to have completed.  The campaign list is therefore
  empty (or nearly so — the safety sweep in ``fence_release`` handles the
  degenerate case where an xcast failure left a partially-setup campaign).
