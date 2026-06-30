.. _elastic-dvm-plan-label:

Elastic DVM Implementation Plan
===============================

This document describes the implementation of the **launch fence** — the
shared mechanism that serialises application-job dispatch against in-progress
DVM grow and shrink campaigns, closing the race between a DVM size change and
concurrently-running application jobs.  For background on the race itself see
:ref:`state-machine-label`, section *DVM Extension and the Daemon-Launch Race*.

The externally observable contract this implementation delivers — the
job-admission and placement guarantees, and the two-phase completion
notification — is specified in :ref:`elastic-dvm-spec-label`, which is
authoritative for observable behavior.  Where this plan and that specification
disagree about observable behavior, the specification wins and this plan must
be corrected.

This plan is the **parent** of two campaign-specific plans:

* :ref:`dvm-grow-campaign-label` — the grow (daemon-launch) path's per-campaign
  fence accounting, failure rollback, and success/failure completion events.
* :ref:`dvm-shrink-campaign-label` — the shrink (node-removal) path's campaign
  tracking, the second (``LAUNCH_APPS``) hold point, completion detection, and
  completion events.

It covers the **shared** infrastructure both paths build on: the fence counter,
the held-job arrays, the ``VM_READY → MAP`` hold point, the fence-release
helper, and the completion-event emission common to both.

The mechanism is a **global launch fence** — a counter
(``prte_dvm_launch_fence``) that tracks the number of in-progress daemon launch
campaigns.  An app job that reaches the ``VM_READY → MAP`` transition checks the
fence; if it is nonzero the job parks itself in a held-job array
(``prte_held_jobs``) and is released when the fence reaches zero.

The state machine is single-threaded on the progress thread, so no locking is
required anywhere in this plan.

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

The grow and shrink campaign lists (``prte_grow_campaigns`` and
``prte_shrink_campaigns``) that drive the fence are declared, constructed, and
destructed alongside these globals; their types and lifecycles are specified in
the two child plans.

Step 3 — Park jobs at the VM_READY → MAP boundary
--------------------------------------------------

This is the first of the two hold points and is shared by both paths: it stops
*any* in-progress campaign (grow or shrink) from letting a freshly-arriving job
map onto a node whose daemon is not ready.

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

The second hold point — at ``LAUNCH_APPS``, guarded by the shrink campaign list
rather than the fence counter — is shrink-specific and is described in
:ref:`dvm-shrink-campaign-label`.

Step 4 — Held-job release helpers
---------------------------------

There are two distinct ways a held job leaves its parked state, and they are
**not** symmetric, so they are handled by two separate helpers rather than a
single ``bool success`` flag:

* **Global success.**  When the global fence reaches zero — every grow and
  shrink campaign has completed *successfully* — both classes of held job are
  admitted.  This is ``prte_plm_base_fence_release()``.

* **Grow failure.**  When a grow campaign fails (see
  :ref:`dvm-grow-campaign-label`, *Failure drain and rollback*), the spec
  requires the whole **pre-map** held-job set to be aborted — the first-failure
  semantics of a non-elastic launch.  But a grow failure must **not** touch the
  **pre-launch** held jobs: those are parked solely on account of an in-progress
  shrink (the ``LAUNCH_APPS`` hold is gated on the shrink list, not the fence),
  so they do not wait on the grow, and the spec's conformance guarantee #4
  states that a daemon failure may affect only the jobs waiting on the campaign
  it belongs to.  This asymmetric abort is ``prte_plm_base_abort_premap_held()``.

Folding both paths into one ``fence_release(bool success)`` was the original
shape of this plan, but it could not honor the spec when a grow failed while a
shrink was still in progress.  A single global ``success`` flag cannot express
"abort the grow's waiters but leave the shrink's waiters parked," and gating the
failure abort on the fence reaching zero would let a *later* shrink-success
release **admit** a pre-map job whose grow dependency had already failed (the
last campaign to drain — the shrink — would call the release with
``success == true``).  Splitting the two release paths closes that gap: the
grow-failure abort fires immediately on the pre-map array regardless of the
fence value, and the success release is reached only when no campaign has
failed.

Both helpers are declared in ``src/mca/plm/base/plm_private.h`` (the header the
errmgr, ras, and state callers already include for the existing
``prte_plm_base_*`` launch helpers) and defined in
``plm_base_launch_support.c``:

.. code-block:: c

   /* SUCCESS release — invoked only when the global fence reaches zero, i.e.
    * every grow and shrink campaign has completed successfully.  Admits both
    * classes of held job and defensively sweeps any residual campaigns. */
   void prte_plm_base_fence_release(void)
   {
       int _hi;
       prte_job_t *_held;

       /* --- pre-map held jobs (parked at VM_READY) --- */
       for (_hi = 0; _hi < prte_held_jobs->size; _hi++) {
           _held = (prte_job_t *)
               pmix_pointer_array_get_item(prte_held_jobs, _hi);
           if (NULL == _held) {
               continue;
           }
           pmix_pointer_array_set_item(prte_held_jobs, _hi, NULL);
           PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_VM_READY);
           PMIX_RELEASE(_held);
       }

       /* --- pre-launch held jobs (parked at LAUNCH_APPS) --- */
       for (_hi = 0; _hi < prte_prelaunch_held_jobs->size; _hi++) {
           _held = (prte_job_t *)
               pmix_pointer_array_get_item(prte_prelaunch_held_jobs, _hi);
           if (NULL == _held) {
               continue;
           }
           pmix_pointer_array_set_item(prte_prelaunch_held_jobs, _hi, NULL);
           if (prte_plm_base_job_needs_remap(_held)) {
               prte_plm_base_reset_proc_map(_held);
               PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_MAP);
           } else {
               PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_LAUNCH_APPS);
           }
           PMIX_RELEASE(_held);
       }

       /* Campaigns are removed individually as their last target drains, so
        * both lists should be empty here.  Sweep each defensively anyway — so
        * a future change that can leave a residual campaign behind cannot wedge
        * the fence — and sweep *both* kinds for symmetry, not just shrink. */
       {
           prte_shrink_campaign_t *_sc, *_sn;
           PMIX_LIST_FOREACH_SAFE(_sc, _sn,
                                  &prte_shrink_campaigns, prte_shrink_campaign_t) {
               pmix_list_remove_item(&prte_shrink_campaigns, &_sc->super);
               PMIX_RELEASE(_sc);
           }
       }
       {
           prte_grow_campaign_t *_gc, *_gn;
           PMIX_LIST_FOREACH_SAFE(_gc, _gn,
                                  &prte_grow_campaigns, prte_grow_campaign_t) {
               pmix_list_remove_item(&prte_grow_campaigns, &_gc->super);
               PMIX_RELEASE(_gc);
           }
       }
   }

   /* GROW-FAILURE abort — fails every job parked at the VM_READY -> MAP
    * boundary to NEVER_LAUNCHED.  Called from the grow failure drain only, and
    * independent of the fence value, so a grow failure aborts its pre-map
    * waiters even while a concurrent shrink keeps the fence nonzero.  It
    * deliberately leaves prte_prelaunch_held_jobs untouched: those jobs wait
    * only on a shrink, never on the grow (conformance #4). */
   void prte_plm_base_abort_premap_held(void)
   {
       int _hi;
       prte_job_t *_held;

       for (_hi = 0; _hi < prte_held_jobs->size; _hi++) {
           _held = (prte_job_t *)
               pmix_pointer_array_get_item(prte_held_jobs, _hi);
           if (NULL == _held) {
               continue;
           }
           pmix_pointer_array_set_item(prte_held_jobs, _hi, NULL);
           PRTE_ACTIVATE_JOB_STATE(_held, PRTE_JOB_STATE_NEVER_LAUNCHED);
           PMIX_RELEASE(_held);
       }
   }

The pre-launch branch of ``fence_release()`` calls two shrink-specific
helpers — ``prte_plm_base_job_needs_remap()`` (does any held proc sit on a
departing daemon?) and ``prte_plm_base_reset_proc_map()`` (un-claim the previous
mapping so the job can be remapped onto survivors) — specified in
:ref:`dvm-shrink-campaign-label`.  Because shrink completion treats a clean exit
and a crash identically (a targeted daemon's departure is always a success for
its campaign), neither held-job array is ever failed on the shrink path; the
only failure disposition of a held job is the grow-failure abort above.

``prte_plm_base_fence_release()`` acts when the **global** fence reaches zero,
which requires *all* grow and shrink campaigns to have completed.  The
per-campaign **completion event** (Step 5) is distinct: it fires for an
individual request's campaign when that campaign drains, independent of whether
other campaigns are still in flight.

Step 5 — Completion-event emission (shared helper)
--------------------------------------------------

The spec's two-phase contract (see :ref:`elastic-dvm-spec-label`,
*Asynchronous size-change completion*) requires that, when an accepted DVM
operation finishes, the runtime deliver a directed event to the process that
requested the size change: ``PMIX_DVM_IS_READY`` on success or
``PMIX_ERR_DVM_MOD`` (carrying the underlying cause) on failure.

Both campaign objects therefore record the requester so the event can be
directed once the campaign drains:

.. code-block:: c

   pmix_proc_t  requester;       /* who requested the size change */
   char        *alloc_id;        /* PMIX_ALLOC_ID of the affected allocation */
   char        *req_id;          /* requester's PMIX_ALLOC_REQ_ID, or NULL */
   bool         have_requester;  /* false for a scheduler push */

These fields are populated where the campaign is created, from the allocation
request that drove the operation:

* **Shrink** — directly in the ``PMIX_ALLOC_RELEASE`` handler, from the request
  object: ``requester`` is the request's ``tproc``, and ``alloc_id`` / ``req_id``
  are read from its ``PMIX_ALLOC_ID`` / ``PMIX_ALLOC_REQ_ID`` info keys.
* **Grow** — in ``setup_virtual_machine()``, *indirectly through the session*.
  The RAS reservation machinery already records the driving request on the
  session and back-points every reserved node at it
  (``add_nodes_to_session()`` sets ``node->session``; the session carries
  ``requestor``, ``alloc_refid``, and ``user_refid``).  The grow campaign reads
  those from the first new daemon's ``node->session``, so the originating
  request need not be threaded explicitly into the launch path.

A size change initiated with no PMIx requester (a scheduler push, or the initial
DVM bring-up, where the session is the default one or its ``requestor`` rank is
``PMIX_RANK_INVALID``) leaves ``have_requester`` false and emits no event.

A single shared helper, declared in ``src/mca/plm/base/plm_private.h`` and
defined in ``plm_base_launch_support.c``, performs the emission.  Its prototype
takes a
``bool success`` rather than an event code, so that **the two new status codes
are named only inside the helper body** — call sites pass a bool and a cause and
never reference ``PMIX_DVM_IS_READY`` / ``PMIX_ERR_DVM_MOD`` themselves:

.. code-block:: c

   /* success => emit PMIX_DVM_IS_READY; otherwise emit PMIX_ERR_DVM_MOD
    * carrying `cause` (the underlying failure pmix_status_t) */
   void prte_plm_base_dvm_mod_notify(const pmix_proc_t *requester,
                                     const char *alloc_id,
                                     const char *req_id,
                                     bool success,
                                     pmix_status_t cause);

It packs ``PMIX_ALLOC_ID`` (always), ``PMIX_ALLOC_REQ_ID`` (when ``req_id`` is
non-NULL), and — on failure — the underlying ``cause`` status (carried under
``PMIX_JOB_TERM_STATUS``, the standard ``pmix_status_t``-typed info key; the PMIx
contract for ``PMIX_ERR_DVM_MOD`` asks only for "any available information
describing the cause"), then delivers the event **only** to ``requester`` as a
directed, custom-range notification — the same ``PMIX_RANGE_CUSTOM`` mechanism
used for ``PMIX_ALLOC_TIMEOUT_WARNING``.

The grow and shrink plans call this helper at their respective drain points: the
grow path inside ``prte_plm_base_grow_drain()`` (reached on success from
``vm_ready`` after the WIREUP xcast, and on failure from
``prte_plm_base_grow_target_failed()`` and the ``check_job_complete`` safety
net); the shrink path when a campaign's last target departs (success, in the
errmgr) and on the xcast-failure cleanup at campaign creation (failure).

``PMIX_DVM_IS_READY`` and ``PMIX_ERR_DVM_MOD`` are plain ``#define``\ d
``pmix_status_t`` values (PMIx status codes are preprocessor macros, not enum
constants), so their availability is decided entirely by whether the installed
PMIx headers define the symbols — **no PMIx capability flag is involved** (the
``PRTE_CHECK_PMIX_CAP`` machinery is for ``PMIX_CAP_*`` behavioral flags and
does not apply here).

To keep the project's ``#if FOO`` discipline — so a mistyped guard is a compile
error rather than a silently-false ``#ifdef`` — a probe in
``config/prte_setup_pmix.m4`` defines ``PRTE_HAVE_DVM_MOD_EVENTS`` to ``0`` or
``1`` from the presence of the two symbols:

.. code-block:: none

   AC_MSG_CHECKING([for PMIx DVM modification event codes])
   AC_PREPROC_IFELSE(
       [AC_LANG_PROGRAM([[#include <pmix.h>
   #if !defined(PMIX_DVM_IS_READY) || !defined(PMIX_ERR_DVM_MOD)
   #error DVM modification event codes not present
   #endif
   ]], [[]])],
       [AC_MSG_RESULT([yes])
        AC_DEFINE([PRTE_HAVE_DVM_MOD_EVENTS], [1],
                  [PMIx defines the DVM modification event codes])],
       [AC_MSG_RESULT([no])
        AC_DEFINE([PRTE_HAVE_DVM_MOD_EVENTS], [0],
                  [PMIx defines the DVM modification event codes])])

The macro is defined to ``0`` or ``1`` on both branches (never ``#undef``\ ed),
so it can be tested with ``#if PRTE_HAVE_DVM_MOD_EVENTS``.  Because the helper's
``bool``-based prototype keeps the two codes out of every call site, **only the
helper body needs the guard**: when ``PRTE_HAVE_DVM_MOD_EVENTS`` is ``0`` the
body compiles to a no-op (the ``bool``/``pmix_status_t`` prototype still
compiles), the call sites are unchanged, and no completion event is delivered —
exactly as the spec's backward-compatibility clause requires.  Because this
touches ``*.m4``, ``./autogen.pl`` must be re-run before configuring.

Summary of Files Changed (Shared Fence Infrastructure)
-------------------------------------------------------

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
     - Define ``prte_plm_base_fence_release()`` and
       ``prte_plm_base_abort_premap_held()`` (Step 4) and
       ``prte_plm_base_dvm_mod_notify()`` (Step 5).
   * - ``src/mca/plm/base/plm_private.h``
     - Declare ``prte_plm_base_fence_release()``,
       ``prte_plm_base_abort_premap_held()``, and
       ``prte_plm_base_dvm_mod_notify()`` (and, for the grow path,
       ``prte_plm_base_grow_drain()`` / ``prte_plm_base_grow_target_failed()`` —
       see :ref:`dvm-grow-campaign-label`).  This is the header the errmgr, ras,
       and state callers already include.
   * - ``src/mca/state/dvm/state_dvm.c``
     - In ``vm_ready``: add the ``VM_READY → MAP`` hold-check before
       ``preposition_files`` (Step 3).
   * - ``config/prte_setup_pmix.m4``
     - Add the ``AC_PREPROC_IFELSE`` probe that defines
       ``PRTE_HAVE_DVM_MOD_EVENTS`` (``0``/``1``) from the presence of
       ``PMIX_DVM_IS_READY`` / ``PMIX_ERR_DVM_MOD`` (Step 5).  Re-run
       ``autogen.pl`` afterward.

For the grow path's file changes see the "Touched files" table in
:ref:`dvm-grow-campaign-label`; for the shrink path's, the "Touched files"
table in :ref:`dvm-shrink-campaign-label`.

Design Invariants
-----------------

**Shared fence**

* The fence is a single ``int`` accessed only on the progress thread, so all
  increments, decrements, and the zero test are race-free without locking.
* A job is parked iff the fence is nonzero at the ``VM_READY → MAP`` boundary
  (Step 3); the ``LAUNCH_APPS`` hold (shrink plan) is gated on the shrink
  campaign list, not the fence, so a concurrent grow does not stall an
  already-mapped job on surviving nodes.
* ``prte_plm_base_fence_release()`` is the **success-only** release; it is
  called only when the fence reaches zero, which requires *all* grow and shrink
  campaigns to have completed successfully.  The campaign lists are therefore
  empty (or nearly so — ``fence_release`` does a defensive sweep of **both** the
  grow and shrink lists for the degenerate case where some future change leaves
  a partially-setup campaign behind).
* A grow failure is the only path that fails a held job.  It calls
  ``prte_plm_base_abort_premap_held()``, which aborts the pre-map held jobs
  (``prte_held_jobs`` → ``NEVER_LAUNCHED``) immediately and independently of the
  fence, and never touches ``prte_prelaunch_held_jobs``: those jobs wait only on
  a shrink, so per conformance #4 a grow failure must leave them parked until the
  shrink completes.
* The per-campaign completion event (Step 5) is independent of the global fence:
  it fires when an individual request's campaign drains, even if other campaigns
  keep the fence nonzero.

**Grow fence** — see the "Why this is correct" and "Design" sections of
:ref:`dvm-grow-campaign-label`.

**Shrink fence** — see the "Design Invariants" section of
:ref:`dvm-shrink-campaign-label`.
