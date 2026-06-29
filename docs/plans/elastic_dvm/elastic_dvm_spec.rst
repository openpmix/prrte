.. _elastic-dvm-spec-label:

Elastic DVM Job Admission: Specification
========================================

Purpose
-------

This document specifies the externally observable behavior of job
admission while the DVM changes size — that is, what an application,
tool, or scheduler may rely on when a job is submitted during a DVM
**grow** (a new-daemon launch campaign) or **shrink** (a node-removal
campaign).  It defines *what* the runtime guarantees, not *how* it
achieves it.  The companion design plans —
:ref:`dvm-launch-fence-label` (the shared fence mechanism and the shrink
path) and :ref:`dvm-grow-campaign-label` (the grow path's per-campaign
accounting) — describe the internal data structures, code paths, and
implementation order.  Where this specification and those plans disagree
about observable behavior, **this specification is authoritative** and
the plan must be corrected.

The guarantees below are stated purely in terms of job lifecycle
outcomes.  This feature introduces **no** new command-line options,
environment variables, or PMIx attributes: the grow and shrink triggers
that already exist (``--add-host`` / ``--add-hostfile``, a
scheduler-driven daemon launch, and a ``PMIX_ALLOC_RELEASE`` that removes
nodes) acquire correct concurrency semantics, and nothing else about a
caller's interface changes.

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
  and the distinction between a failure that belongs to the in-progress
  campaign and an unrelated one.
* The behavior when grow and shrink campaigns, or multiple campaigns of
  the same kind, overlap in time.
* The single observable artifact this feature adds: the
  ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` job state reported for a parked
  job in verbose and debugging output.

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
  campaign completes, the grow is treated as failed as a whole, and every
  job parked on account of the grow is aborted (it never launches) rather
  than being admitted onto an incomplete DVM.  This matches the
  first-failure semantics of a non-elastic launch.

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

Failure semantics
-----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Event
     - Observable outcome
   * - A grow-target daemon fails before its grow completes
     - The grow fails as a whole; all jobs parked on account of the grow
       are aborted and never launch.
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

The only externally visible artifact this feature introduces is a job
state.  A parked job is reported as ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS``
in verbose and debugging output (for example under
``--prtemca state_base_verbose``), so an operator can see precisely why a
job has not yet been mapped or launched.  The state is a passive marker:
it triggers no callback and changes no other behavior.  Once the size
change the job is waiting on completes, the job advances out of this state
on its own.

Backward compatibility and transparency
----------------------------------------

This feature is transparent to every caller:

* No new command-line option, environment variable, or PMIx attribute is
  defined or required.  A tool, application, or scheduler issues the same
  requests it always has.
* On a DVM that never grows or shrinks, no job is ever parked and behavior
  is identical to a non-elastic DVM.
* The only difference a caller can observe when a size change *is* in
  progress is a launch delay for an affected job and, in debugging output,
  the ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` state — never a spurious
  failure and never a launch onto a node that is not ready or is leaving.

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
#. Shrink completion is driven by the actual departure of every targeted
   daemon — clean exit and crash being indistinguishable — and is
   idempotent against a daemon that reports its departure more than once.
#. Concurrent campaigns of either kind never deadlock a parked job: it is
   admitted once, and only once, every campaign it is waiting on has
   completed.
#. The feature adds no new caller-visible interface; its only observable
   artifact is the ``PRTE_JOB_STATE_WAITING_FOR_DAEMONS`` state shown for
   a parked job in verbose and debugging output.
