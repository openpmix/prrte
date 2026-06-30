.. _elastic-dvm-spec-label:

Elastic DVM: Specification
==========================

Purpose
-------

This document specifies the externally observable behavior of the DVM
while it changes size — what an application, tool, or scheduler may rely
on when the DVM **grows** (a new-daemon launch campaign) or **shrinks** (a
node-removal campaign).  It covers two distinct audiences and contracts:

* **Job admission** — what happens to an application job that is submitted
  *while* a grow or shrink is in progress (the bulk of this document).
* **Size-change completion** — how the process that *requested* the size
  change learns, asynchronously, whether the DVM operation eventually
  succeeded or failed (see `Asynchronous size-change completion`_).

It defines *what* the runtime guarantees, not *how* it achieves it.  The
companion design plans — :ref:`elastic-dvm-plan-label` (the shared fence
mechanism and completion-event helper), :ref:`dvm-grow-campaign-label`
(the grow path's per-campaign accounting), and
:ref:`dvm-shrink-campaign-label` (the shrink path's campaign tracking) —
describe the internal data structures, code paths, and implementation
order.  Where this specification and those plans disagree about observable
behavior, **this specification is authoritative** and the plan must be
corrected.

The whole of this behavior is gated on the DVM being in **elastic mode**,
selected by the pre-existing ``prte_elastic_mode`` MCA parameter (off by
default).  When it is not set the DVM is fixed-size: none of the
job-admission deferral, parking, or completion-event machinery is active,
and the runtime behaves exactly as it did before this feature — a daemon
loss, for instance, is handled by the ordinary error path rather than
absorbed as a campaign event.  Everything below describes the behavior
**when elastic mode is enabled**.

Within elastic mode, the job-admission guarantees are stated purely in
terms of job lifecycle outcomes and introduce **no** new command-line
options, environment variables, or PMIx attributes: the grow and shrink
triggers that already exist (``--add-host`` / ``--add-hostfile``, a
scheduler-driven daemon launch, and a ``PMIX_ALLOC_RELEASE`` that removes
nodes) simply acquire correct concurrency semantics.  The completion
contract does introduce two new PMIx event (status) codes —
``PMIX_DVM_IS_READY`` and ``PMIX_ERR_DVM_MOD`` — used to notify the
requester when the asynchronous DVM operation finishes; these are the only
new caller-visible interface this feature adds, and both are optional (see
`Backward compatibility and transparency`_).

Scope
-----

In scope
~~~~~~~~

* The admission guarantee for an application job that reaches the
  map-eligible boundary while a grow or shrink is in progress.
* The placement guarantee for a job launched across a size change — onto
  which set of nodes (pre- or post-change) it is permitted to map.
* The disposition of a job that was already mapped onto a node that is
  about to depart in a shrink.
* The outcome of a daemon failure that occurs during a grow or a shrink,
  the distinction between a failure that belongs to the in-progress
  campaign and an unrelated one, and the rollback of a failed grow to the
  DVM's pre-grow membership.
* The behavior when grow and shrink campaigns, or multiple campaigns of
  the same kind, overlap in time.
* The two-phase model by which a dynamic allocation request that drives a
  size change is answered: a synchronous response when the request is
  *accepted*, and a later asynchronous event when the DVM operation
  *completes* or *fails*.
* The two new PMIx event codes — ``PMIX_DVM_IS_READY`` and
  ``PMIX_ERR_DVM_MOD`` — that carry the asynchronous completion result to
  the requester, and the allocation-identifying payload each delivers.
* The ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` job state reported for a
  parked job in verbose and debugging output.

Out of scope / non-goals
~~~~~~~~~~~~~~~~~~~~~~~~~~

* The *policy* that decides when the DVM grows or shrinks.  That decision
  is driven entirely by the existing triggers (a tool or application
  expansion request, a scheduler action, or an allocation release); this
  specification governs only how concurrent job submissions behave around
  such a change, never whether the change happens.
* Node selection — which physical nodes are added or removed — is the
  responsibility of the resource manager and the existing allocation and
  mapping machinery, and is unchanged.
* The wire encoding of internal messages, the names and types of internal
  counters and lists, and the precise timing of internal state
  transitions are implementation details and are not specified here.
* This document does not redefine the meaning of any DVM size-change
  trigger; it specifies only the admission and placement guarantees that
  now hold while one is in flight.

Definitions
-----------

Grow campaign
   A single in-progress launch of one or more new daemons that extends
   the DVM onto additional nodes.  A grow campaign is *complete* when
   every new daemon has reported in and the head node has distributed the
   wireup (nidmap) information to the DVM.

Shrink campaign
   A single in-progress removal of one or more nodes from the DVM,
   initiated by an allocation release.  A shrink campaign targets a fixed
   set of daemon ranks and is *complete* when every targeted daemon has
   actually departed the DVM.

Size change
   A grow campaign or a shrink campaign.  More than one may be in
   progress at the same time.

Map-eligible boundary
   The point in a job's lifecycle at which it is ready to be assigned to
   nodes (the ``VM_READY → MAP`` transition).  A job that has not yet
   crossed this boundary has no node assignments.

Launch boundary
   The point at which a job's mapping is complete and the runtime is
   ready to send launch data to the daemons that will host its processes
   (the ``LAUNCH_APPS`` transition).  A job at this boundary already has
   node assignments.

Parked job
   A job that has been held at the map-eligible boundary or the launch
   boundary because a size change is in progress.  A parked job is
   reported in the ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` state.  Parking
   is invisible to the submitting tool beyond a delay in launch; the job
   is neither failed nor restarted.

Surviving node
   A node that remains in the DVM after a shrink campaign completes.

Departing node
   A node whose daemon is a target of an in-progress shrink campaign.

Size-change requester
   The process whose request initiated a grow or shrink — for a dynamic
   allocation this is the process that issued the ``PMIX_ALLOC_NEW`` /
   ``PMIX_ALLOC_EXTEND`` (grow) or ``PMIX_ALLOC_RELEASE`` (shrink) request.
   A size change initiated without a PMIx requester (for example a
   scheduler pushing daemons directly) has no requester.

Request acceptance
   The point at which the runtime has finished *processing* a size-change
   request — validated it and initiated the corresponding DVM operation —
   and returns its synchronous response.  Acceptance is **phase one**; it
   does not assert that the operation has finished.

Operation completion
   The point at which the initiated DVM operation actually finishes — the
   grow's new daemons are launched and wired, or the shrink's targeted
   daemons have departed and the routing tree is repaired — or
   definitively fails.  Completion is **phase two** and is reported
   asynchronously by event (see `Asynchronous size-change completion`_).

Admission contract
-------------------

The central guarantee is one of **non-destructive deferral**:

   A job submitted while a size change is in progress is never failed
   merely because the DVM is changing size.  It is held until the change
   completes and then admitted onto the post-change set of nodes — except
   that a job whose admission depends on a grow that *fails* is aborted
   rather than launched onto an incomplete DVM.

Two distinct placement hazards are closed by this contract, and a
conforming implementation must close both:

#. **A job must never be mapped onto a node whose daemon is not ready.**
   During a grow this means a node whose daemon has started but has not
   yet received its wireup information; during a shrink it means a node
   whose daemon is a departure target.

#. **A job must never have launch data sent to a daemon that is
   departing.**  A job that completed mapping *before* a shrink began, and
   was placed on a node now targeted for removal, must not transmit its
   launch message to that daemon.

Behavior during a grow
~~~~~~~~~~~~~~~~~~~~~~~~

While a grow campaign is in progress:

* A job that reaches the map-eligible boundary is parked.  It is admitted
  only once **every** in-progress grow campaign has completed — that is,
  only after the new daemons are not merely running but fully wired into
  the DVM.  This guarantees hazard 1: the job cannot be mapped onto a
  node whose daemon is up but not yet wired.

* A job that had already crossed the map-eligible boundary before the
  grow began is **unaffected**: it continues to launch on the nodes it
  was assigned, and a grow never stalls it.

* When the grow completes successfully, every job parked at the
  map-eligible boundary is admitted and proceeds to mapping, now able to
  consider the newly added nodes.

* If a daemon belonging to the grow campaign **fails** before the
  campaign completes, the grow is treated as failed as a whole and is
  **rolled back**: every daemon that the same campaign had already
  started is terminated, and the nodes the campaign was adding leave the
  DVM, so the DVM is restored to exactly the membership it had before that
  campaign began.  A failed grow never leaves the DVM half-extended with a
  partial, un-wired set of new daemons.  Every job parked on account of
  the grow is then aborted (it never launches) rather than being admitted
  onto an incomplete DVM.  This matches the first-failure semantics of a
  non-elastic launch.  The rollback is scoped to the failed campaign: an
  unrelated grow campaign running concurrently keeps its own daemons and
  completes normally, and pre-existing daemons and nodes are untouched.

* A daemon failure that does **not** belong to any in-progress grow
  campaign (for example, a pre-existing daemon dying for an unrelated
  reason) does not release parked jobs early and does not abort them; the
  grow proceeds unaffected.

Behavior during a shrink
~~~~~~~~~~~~~~~~~~~~~~~~~~

While a shrink campaign is in progress:

* A job that reaches the map-eligible boundary is parked, exactly as for
  a grow, and is admitted only once every in-progress size change has
  completed.  This closes hazard 1 for the shrink case: a newly arriving
  job cannot be mapped onto a node that is in the act of leaving.

* A job that had already completed mapping and reaches the launch
  boundary is parked **if and only if a shrink is in progress**, until
  every targeted daemon has departed.  This closes hazard 2.  A grow in
  progress does *not* park a job at the launch boundary, because a grow
  removes no node and so cannot invalidate an existing mapping.

* When the shrink completes, each parked job is admitted according to
  whether the shrink invalidated its placement:

  - A job parked at the launch boundary whose processes were **not**
    assigned to any departing node proceeds directly to launch on its
    existing mapping.
  - A job parked at the launch boundary that had one or more processes
    assigned to a departing node is **remapped** onto the surviving
    nodes and then launched.  Its placement after the change reflects the
    smaller DVM; the job is not failed.
  - A job parked at the map-eligible boundary is admitted to mapping and
    naturally considers only the surviving nodes.

* Completion of a shrink is driven by the **actual departure** of each
  targeted daemon, not by any advance announcement of intent to leave.  A
  daemon that exits cleanly in response to the shrink and a daemon that
  crashes during the shrink are indistinguishable to the contract: in
  both cases the node is leaving, and any job mapped onto it is remapped
  onto survivors as described above.  No job is admitted until the
  departing daemons are genuinely gone and the DVM's view of its
  membership has been updated to match.

Concurrency
-----------

* Any number of grow campaigns, any number of shrink campaigns, or a
  mixture, may be in progress simultaneously.  Parked jobs are admitted
  only when **all** in-progress campaigns have completed; no campaign can
  release another campaign's held jobs, and no campaign's completion is
  consumed by an unrelated daemon event.

* A grow campaign in progress concurrently with a shrink does not stall a
  job at the launch boundary that has already been mapped onto surviving
  nodes; only an in-progress shrink holds jobs there.

* Each campaign is accounted for independently, so an overlapping or
  partially-overlapping pair of campaigns cannot leave a job parked
  indefinitely once the last campaign it is waiting on completes.

Asynchronous size-change completion
-----------------------------------

A dynamic allocation request that grows or shrinks the DVM is answered in
**two phases**, and the runtime separates the point at which the
*allocation is complete* from the point at which the *runtime is ready*.

Two-phase model
~~~~~~~~~~~~~~~

#. **Acceptance (synchronous).**  When the runtime has finished
   *processing* the request — validated it, decided the resulting
   session/reservation, and initiated the corresponding grow or shrink —
   it returns the allocation response (status plus ``PMIX_ALLOC_ID``, as
   specified in the companion allocation contract
   ``node-reservation-spec.rst``).  This response confirms only that the
   request was **accepted** and the DVM operation has **begun**.  It does
   *not* assert that a grow's new daemons are up and wired, or that a
   shrink's targeted daemons have departed.

#. **Completion (asynchronous, by event).**  When the DVM operation later
   finishes — or fails — the runtime delivers a directed PMIx event to the
   **size-change requester**.  This is the signal that the *runtime is
   ready* (or that the change will not happen), as distinct from the
   acceptance in phase one.

The two phases are decoupled because the DVM operation is inherently
asynchronous and unbounded in time (daemon launch and wireup, or daemon
termination and tree repair).  Blocking the allocation response until the
operation finished would stall the requester and entangle the request's
validity with unrelated launch/teardown failures.  Decoupling lets the
requester learn promptly that its request was accepted and then act only
when the runtime actually reflects the new size.

Success event — ``PMIX_DVM_IS_READY``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When the DVM operation completes successfully — for a grow, the new
daemons are launched and wired into the DVM; for a shrink, every targeted
daemon has departed and the routing tree is repaired — the runtime
delivers a ``PMIX_DVM_IS_READY`` event to the requester.  After this event
the DVM reflects the requested size: a grow's new nodes are available to
spawn onto, a shrink's removed nodes are gone.  The event payload carries:

* ``PMIX_ALLOC_ID`` (``char*``) — the allocation whose operation
  completed; always present.
* ``PMIX_ALLOC_REQ_ID`` (``char*``) — the requester's own request id,
  included whenever one was supplied on the original request, so the
  recipient can match the event by either identifier.

Failure event — ``PMIX_ERR_DVM_MOD``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the accepted DVM modification fails — a grow cannot launch or wire its
new daemons (and is rolled back per `Failure semantics`_), a shrink cannot
be carried out, or any other condition leaves the requested change
unrealized — the runtime delivers a ``PMIX_ERR_DVM_MOD`` event to the
requester.  The event states that **no (further) DVM modification will be
made** for this request and that the DVM has been returned to a stable
state (for a failed grow, its pre-grow membership).  The payload carries:

* ``PMIX_ALLOC_ID`` (``char*``) — always present.
* ``PMIX_ALLOC_REQ_ID`` (``char*``) — when one was supplied.
* The **underlying cause** — the specific ``pmix_status_t`` that prevented
  the modification (for example a daemon-launch failure versus a resource
  error), conveyed in the event's info array so the requester can
  distinguish what went wrong rather than only that *something* did.

Both events are **directed to the requesting process only** — they are not
broadcast — mirroring the delivery of ``PMIX_ALLOC_TIMEOUT_WARNING``
specified in ``node-reservation-spec.rst``.

Delivery guarantees
~~~~~~~~~~~~~~~~~~~

* **Exactly one terminal event per operation.**  Each accepted request
  that initiates a DVM grow or shrink yields exactly one of
  ``PMIX_DVM_IS_READY`` or ``PMIX_ERR_DVM_MOD`` to its requester.
* **No event for a phase-one rejection.**  A request rejected during
  *processing* (the error cases in the companion
  ``node-reservation-spec.rst``, e.g. a malformed or unauthorized request)
  fails synchronously in the allocation response and produces **no**
  completion event; the phase-two events report only the outcome of an
  *accepted* request's DVM operation.
* **No event when nothing changes.**  A request that is accepted but
  initiates no actual DVM size change (for example an extend that adds no
  new daemons) is fully complete at acceptance and emits no asynchronous
  event.
* **No requester, no directed event.**  A size change with no PMIx
  requester (a scheduler-driven push) updates the runtime's own state but
  has no specific process to direct a completion event to; none is sent.

Proposed PMIx status codes
~~~~~~~~~~~~~~~~~~~~~~~~~~~

This contract requires two PMIx event codes that PRRTE cannot define on
its own (they belong to the PMIx standard and headers):

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Code
     - Meaning
   * - ``PMIX_DVM_IS_READY``
     - Non-error event: an accepted DVM size change has completed and the
       runtime now reflects the new size.  Carries ``PMIX_ALLOC_ID`` and,
       when supplied, ``PMIX_ALLOC_REQ_ID``.
   * - ``PMIX_ERR_DVM_MOD``
     - Error event: an accepted DVM size change failed and will not be
       made; the DVM has returned to a stable state.  Carries
       ``PMIX_ALLOC_ID``, the underlying failure ``pmix_status_t``, and —
       when supplied — ``PMIX_ALLOC_REQ_ID``.

Because PMIx status codes are plain preprocessor ``#define``\ s, their use is
guarded at build time by a simple check for the codes' presence in the
installed PMIx headers — no PMIx *capability* flag is required.  A PRRTE built
against a PMIx that defines neither code simply omits the completion
notification (see `Backward compatibility and transparency`_).

Failure semantics
-----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Event
     - Observable outcome
   * - A grow-target daemon fails before its grow completes
     - The grow fails as a whole and is rolled back: every daemon the same
       campaign had already started is terminated and its nodes leave the
       DVM, restoring the pre-grow membership; all jobs parked on account
       of the grow are aborted and never launch.
   * - A daemon unrelated to any in-progress grow fails during a grow
     - The grow is unaffected; parked jobs are neither released early nor
       aborted.
   * - A shrink-target daemon exits cleanly
     - Counts as that target's departure; when the last target departs the
       shrink completes and parked jobs are admitted (remapped if they
       were on a departing node).
   * - A shrink-target daemon crashes during the shrink
     - Handled identically to a clean exit: the node is leaving regardless,
       and jobs mapped onto it are remapped onto survivors.
   * - The same departing daemon emits more than one failure event
     - Counted once; a repeated event for an already-departed target has no
       further effect on admission.
   * - The controlling daemon job is torn down with grow campaigns still
       pending
     - The pending grows are drained as failures so that no job is left
       parked across the teardown.

In every case a parked job has no partial effect: until it is admitted it
has launched no processes, and a job aborted because its grow failed
leaves nothing running.

Observability
-------------

This feature introduces two externally visible artifacts.

The first is a **job state**.  A parked job is reported as
``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` in verbose and debugging output (for
example under ``--prtemca state_base_verbose``), so an operator can see
precisely why a job has not yet been mapped or launched.  The state is a
passive marker: it triggers no callback and changes no other behavior.
Once the size change the job is waiting on completes, the job advances out
of this state on its own.

The second is the pair of **completion events** described under
`Asynchronous size-change completion`_: ``PMIX_DVM_IS_READY`` on success
and ``PMIX_ERR_DVM_MOD`` on failure, each directed to the requester of the
size change and carrying the allocation identifiers (and, on failure, the
underlying cause).  Unlike the job state, these are an active interface a
requester registers an event handler for; they are the requester's only
signal that the asynchronous DVM operation has finished.

Backward compatibility and transparency
----------------------------------------

The **job-admission** contract is transparent to every caller:

* It is inert unless the DVM is in elastic mode (``prte_elastic_mode``, off
  by default).  A DVM started without that parameter is fixed-size and runs
  exactly as it did before this feature — the launch fence is never raised,
  no job is ever parked, and daemon losses follow the ordinary error path.
* No new command-line option, environment variable, or PMIx attribute is
  defined or required for it (``prte_elastic_mode`` already existed).  A
  tool, application, or scheduler issues the same requests it always has.
* On a DVM that never grows or shrinks, no job is ever parked and behavior
  is identical to a non-elastic DVM.
* The only difference a submitting caller can observe when a size change
  *is* in progress is a launch delay for an affected job and, in debugging
  output, the ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` state — never a
  spurious failure and never a launch onto a node that is not ready or is
  leaving.

The **completion** contract adds the two new PMIx event codes, which are
optional and degrade cleanly:

* ``PMIX_DVM_IS_READY`` and ``PMIX_ERR_DVM_MOD`` are delivered only to a
  requester that registers a handler for them; a requester that ignores
  them is unaffected beyond losing the completion signal.
* When the underlying PMIx defines neither code, a PRRTE built against it
  (the call sites are guarded by a preprocessor check for the two
  ``#define``\ d status codes) omits the asynchronous completion
  notification entirely.  The allocation response
  (phase one) is unchanged, so a request is still accepted and the DVM
  still grows or shrinks; the requester simply receives no event-based
  signal that the operation finished or failed, exactly as before this
  feature existed.  This is a functional gap, not merely a cosmetic one:
  without the event a requester cannot reliably know when the runtime is
  ready and must fall back to whatever coarse means it used previously.

Conformance summary
-------------------

A conforming implementation guarantees that:

#. A job submitted while a size change is in progress is held, not failed,
   solely on account of the change — and is then admitted onto the
   post-change node set, with the sole exception of a job whose grow
   dependency fails, which is aborted.
#. A job is never mapped onto a node whose daemon is not yet wired into
   the DVM (grow) or is a departure target (shrink).
#. A job is never sent launch data destined for a departing daemon; a job
   already mapped onto a departing node is remapped onto surviving nodes
   before it launches.
#. A daemon failure affects only the jobs waiting on the campaign that
   failure belongs to; an unrelated daemon loss neither releases nor
   aborts parked jobs.
#. A grow that fails is rolled back to the DVM's pre-grow membership: the
   campaign's already-started daemons are terminated and its nodes leave
   the DVM, so a failed grow never leaves the DVM half-extended.  The
   rollback is scoped to the failed campaign and leaves concurrent
   campaigns and pre-existing daemons untouched.
#. Shrink completion is driven by the actual departure of every targeted
   daemon — clean exit and crash being indistinguishable — and is
   idempotent against a daemon that reports its departure more than once.
#. Concurrent campaigns of either kind never deadlock a parked job: it is
   admitted once, and only once, every campaign it is waiting on has
   completed.
#. A dynamic allocation that drives a size change is answered in two
   phases: a synchronous response on acceptance (which does not assert the
   operation has finished), and exactly one asynchronous terminal event —
   ``PMIX_DVM_IS_READY`` on success or ``PMIX_ERR_DVM_MOD`` (carrying the
   underlying cause) on failure — directed to the requester when the DVM
   operation completes.  A phase-one rejection, a request that changes
   nothing, and a requester-less scheduler push each produce no such event.
#. The job-admission contract adds no caller-visible interface; the
   completion contract adds only the two optional PMIx event codes above,
   and when the underlying PMIx lacks them the runtime omits the
   completion notification while leaving every other guarantee intact.
   The job state ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` remains observable
   for a parked job in verbose and debugging output.
