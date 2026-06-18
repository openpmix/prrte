.. _state-machine-label:

Job Launch State Machine
========================

PRRTE drives the full lifecycle of a job — from daemon launch through
application launch and termination — through an explicit, event-driven state
machine.  Every transition is represented as an event posted to the PRRTE
progress thread; the callback for each state runs single-threaded, performs
its work, and posts the next event when done.  Nothing blocks the calling
thread and there are no race conditions between state handlers.

There are two cooperating state machines: one for **jobs** (tracking the
lifecycle of an entire job or the DVM itself) and one for **processes**
(tracking each individual application process).

Architecture
------------

The state machine is implemented in ``src/mca/state/``.  The **DVM module**
(``src/mca/state/dvm/state_dvm.c``) is used when ``prte`` runs as a
persistent Distributed Virtual Machine; it owns the authoritative ordered
table of states and callbacks.  The **prted module**
(``src/mca/state/prted/state_prted.c``) runs inside each daemon and handles
only the small set of states relevant to a daemon's local work.

The state machine is a linked list (``prte_job_states``) of
``(state, callback)`` pairs.  The macro ``PRTE_ACTIVATE_JOB_STATE(jdata,
state)`` packages the job object and the target state into a *caddy* and
posts it to the event loop.  The matching callback is looked up and invoked
asynchronously.

Job State Definitions
---------------------

All job-state constants are defined in
``src/mca/plm/plm_types.h`` (lines 116–194).  The states relevant to daemon
launch, in numeric order, are:

.. list-table::
   :widths: 35 10 55
   :header-rows: 1

   * - Name
     - Value
     - Meaning
   * - ``PRTE_JOB_STATE_INIT``
     - 1
     - Job record created; ready to receive a job ID.
   * - ``PRTE_JOB_STATE_INIT_COMPLETE``
     - 2
     - Job ID assigned; initial setup done.
   * - ``PRTE_JOB_STATE_ALLOCATE``
     - 3
     - Ready to request resources from the scheduler/RAS.
   * - ``PRTE_JOB_STATE_ALLOCATION_COMPLETE``
     - 4
     - Resource allocation finished.
   * - ``PRTE_JOB_STATE_LAUNCH_DAEMONS``
     - 8
     - Ready to spawn ``prted`` processes.  *Not* in the DVM default table;
       registered by each PLM component at startup.
   * - ``PRTE_JOB_STATE_DAEMONS_LAUNCHED``
     - 9
     - The PLM has initiated daemon spawning; waiting for daemons to call home.
   * - ``PRTE_JOB_STATE_DAEMONS_REPORTED``
     - 10
     - All expected daemons have connected and sent their contact information.
   * - ``PRTE_JOB_STATE_VM_READY``
     - 11
     - The DVM is fully operational; node map and wireup info have been
       broadcast to all daemons.
   * - ``PRTE_JOB_STATE_MAP``
     - 5
     - Ready to map processes to nodes.
   * - ``PRTE_JOB_STATE_MAP_COMPLETE``
     - 6
     - Process mapping finished.
   * - ``PRTE_JOB_STATE_SYSTEM_PREP``
     - 7
     - Final sanity checks and environment setup before launch.
   * - ``PRTE_JOB_STATE_LAUNCH_APPS``
     - 12
     - Ready to send launch directives to daemons.
   * - ``PRTE_JOB_STATE_SEND_LAUNCH_MSG``
     - 13
     - Launch message being assembled and sent.
   * - ``PRTE_JOB_STATE_STARTED``
     - 20
     - At least one application process has been forked.
   * - ``PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE``
     - 18
     - All local processes on a daemon have attempted to launch.
   * - ``PRTE_JOB_STATE_READY_FOR_DEBUG``
     - 19
     - All local processes report ready for a debugger attach.
   * - ``PRTE_JOB_STATE_RUNNING``
     - 14
     - All processes across all daemons have been forked.
   * - ``PRTE_JOB_STATE_REGISTERED``
     - 16
     - All processes have registered with the PMIx server (called
       ``PMIx_Init``).

Termination states (values ≥ 30) and error states (values ≥ 51) are
described at the bottom of this page.

The Daemon Launch Sequence
--------------------------

The DVM module registers the following ordered table at startup
(``src/mca/state/dvm/state_dvm.c``, ``launch_states[]`` /
``launch_callbacks[]``):

.. code-block:: text

   State                          Callback
   ─────────────────────────────────────────────────────────────────────
   PRTE_JOB_STATE_INIT            prte_plm_base_setup_job
   PRTE_JOB_STATE_INIT_COMPLETE   init_complete              (dvm-local)
   PRTE_JOB_STATE_ALLOCATE        prte_ras_base_allocate
   PRTE_JOB_STATE_ALLOCATION_COMPLETE  prte_plm_base_allocation_complete
   PRTE_JOB_STATE_DAEMONS_LAUNCHED     prte_plm_base_daemons_launched
   PRTE_JOB_STATE_DAEMONS_REPORTED     prte_plm_base_daemons_reported
   PRTE_JOB_STATE_VM_READY        vm_ready                   (dvm-local)
   PRTE_JOB_STATE_MAP             prte_rmaps_base_map_job
   PRTE_JOB_STATE_MAP_COMPLETE    prte_plm_base_mapping_complete
   PRTE_JOB_STATE_SYSTEM_PREP     prte_plm_base_complete_setup
   PRTE_JOB_STATE_LAUNCH_APPS     prte_plm_base_launch_apps
   PRTE_JOB_STATE_SEND_LAUNCH_MSG prte_plm_base_send_launch_msg
   PRTE_JOB_STATE_STARTED         job_started                (dvm-local)
   PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE  prte_state_base_local_launch_complete
   PRTE_JOB_STATE_READY_FOR_DEBUG ready_for_debug            (dvm-local)
   PRTE_JOB_STATE_RUNNING         prte_plm_base_post_launch
   PRTE_JOB_STATE_REGISTERED      prte_plm_base_registered
   PRTE_JOB_STATE_TERMINATED      check_complete             (dvm-local)
   PRTE_JOB_STATE_NOTIFY_COMPLETED dvm_notify               (dvm-local)
   PRTE_JOB_STATE_NOTIFIED        cleanup_job                (dvm-local)
   PRTE_JOB_STATE_ALL_JOBS_COMPLETE prte_quit

   (plus DAEMONS_TERMINATED → prte_quit and FORCED_EXIT → force_quit,
    registered separately)

Note that ``PRTE_JOB_STATE_LAUNCH_DAEMONS`` is **not** in this table.
Each Process Launch Manager (PLM) component—ssh, slurm, pals, lsf—inserts
its own ``launch_daemons`` callback for that state during its own ``init``.

Step-by-step walk-through
~~~~~~~~~~~~~~~~~~~~~~~~~

**1. INIT → prte_plm_base_setup_job**

The job record is validated and initial app-context setup is performed.
On success the callback posts ``INIT_COMPLETE``.

**2. INIT_COMPLETE → init_complete**

The DVM-local ``init_complete`` immediately posts ``ALLOCATE`` so that a
potential DVM expansion can go through the allocation step.

**3. ALLOCATE → prte_ras_base_allocate**

The Resource Allocation Subsystem (RAS) queries the scheduler or hostfile
for available nodes and records them in the node pool.  On completion it
posts ``ALLOCATION_COMPLETE``.

**4. ALLOCATION_COMPLETE → prte_plm_base_allocation_complete**

Decision point (``src/mca/plm/base/plm_base_launch_support.c``:186):

* If ``PRTE_JOB_DO_NOT_LAUNCH`` is set (e.g., ``--map-by :display``), skip
  daemon spawning entirely and jump straight to ``DAEMONS_REPORTED``.
* Otherwise, post ``LAUNCH_DAEMONS``.

**5. LAUNCH_DAEMONS → <PLM launch_daemons>**

This state is handled by the active PLM component, not by the DVM module.
The ssh PLM's handler (``src/mca/plm/ssh/plm_ssh_module.c``:1077) is
representative:

a. Calls ``prte_plm_base_setup_virtual_machine()`` to compute which nodes
   need new daemons (nodes already hosting a daemon from a prior job are
   reused).
b. If no new daemons are needed (``map->num_new_daemons == 0``), fast-paths
   to ``DAEMONS_REPORTED``.
c. Otherwise, builds the ``prted`` command line and spawns one daemon per
   node via ssh (or pdsh, or the equivalent for slurm/pals/lsf).
d. Registers ``prte_plm_base_daemon_callback`` on
   ``PRTE_RML_TAG_DAEMON_REPORT`` to hear from daemons as they start.
e. Posts ``DAEMONS_LAUNCHED`` to indicate spawning has been initiated.

**6. DAEMONS_LAUNCHED → prte_plm_base_daemons_launched**

This callback is intentionally a no-op
(``src/mca/plm/base/plm_base_launch_support.c``:218).  The state machine
parks here and waits for daemons to call home asynchronously.

**7. Daemons call home (asynchronous)**

As each ``prted`` process starts up it:

a. Initializes via its ESS (Environment-Specific Services) component.
b. Connects to the HNP (Head Node Process) via the RML.
c. Sends a report containing its process name, RML contact URI, node name,
   and hwloc topology to the HNP on ``PRTE_RML_TAG_DAEMON_REPORT``.

The HNP receives these reports in ``prte_plm_base_daemon_callback``
(``src/mca/plm/base/plm_base_launch_support.c``:1237).  For each arriving
daemon it:

* Records the daemon's contact URI (stored via ``PMIx_Store_internal`` as
  ``PMIX_PROC_URI``).
* Records the node name and hwloc topology.
* Marks the node ``PRTE_NODE_STATE_UP``.
* Increments ``jdatorted->num_reported``.
* Calls ``progress_daemons()`` (line 1173), which fires
  ``DAEMONS_REPORTED`` once ``num_reported == num_procs``.

**8. DAEMONS_REPORTED → prte_plm_base_daemons_reported**

(``src/mca/plm/base/plm_base_launch_support.c``:118)

* If using an unmanaged allocation (e.g., a hostfile), sets the default
  slot count on each node according to ``--set-slots`` (cores, sockets,
  hwthreads, or a literal number).
* Totals up ``jdata->total_slots_alloc``.
* Posts ``VM_READY``.

At this point every daemon is up and the HNP knows how to reach each of
them.

**9. VM_READY → vm_ready**

(``src/mca/state/dvm/state_dvm.c``:261)

If new daemons were actually launched (``PRTE_JOB_LAUNCHED_DAEMONS`` is
set) and more than one daemon is running:

* Serializes the node map via ``prte_util_nidmap_create()`` into a buffer.
* Looks up each daemon's ``PMIX_PROC_URI`` and packs it into the same
  buffer.
* Broadcasts the combined nidmap + wireup buffer to all daemons via
  ``prte_grpcomm.xcast(PRTE_RML_TAG_WIREUP, &buf)``.

After the broadcast:

* Sets ``prte_dvm_ready = true``.
* If running as a persistent DVM (``prte`` without an immediate job),
  prints ``"DVM ready\n"`` to stdout or writes a ``'K'`` byte on the
  parent pipe so the caller knows the DVM is accepting work.
* Dispatches any jobs that arrived and were cached while the DVM was
  starting (``prte_cache``).

**The DVM is now fully operational.**  For a standalone ``prterun``
invocation the state machine continues immediately into the app-launch
phase below.

Application Launch (after the DVM is ready)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Once the DVM is ready, each new application job that arrives at the HNP
(via ``PRTE_PLM_LAUNCH_JOB_CMD``) goes through a fast-path re-entry into
the state machine (``plm_base_receive.c``:470).  If ``prte_dvm_ready`` is
not yet true (initial DVM startup still in progress), the job is stashed in
``prte_cache`` and flushed when ``vm_ready`` fires for the daemon job.
Otherwise the job enters the state machine immediately via
``prte_plm.spawn(jdata)``.

A DVM can run many application jobs concurrently.  Each follows the same
state machine independently.

**10. MAP → prte_rmaps_base_map_job**

(``src/mca/rmaps/base/``)

The RMAPS framework assigns each application process to a specific node and
slot.  The mapping policy (``--map-by slot``, ``--map-by node``,
``--map-by core``, ``--map-by ppr:N:L``, etc.) determines how processes
are distributed.

Key actions:

* Iterates over the node pool for the job's session.
* For each app context, calls the selected RMAPS component
  (e.g., ``rmaps_round_robin``, ``rmaps_ppr``, ``rmaps_rank_file``).
* Each component calls ``prte_rmaps_base_claim_slot()`` to assign a process
  to a node; this creates a ``prte_proc_t`` entry and links it to the node.
* Sets ``jdata->num_procs``.
* If ``--rank-by`` or ``--bind-to`` were specified, records those policies
  in the map for use during launch.

On completion, fires ``MAP_COMPLETE``.

**11. MAP_COMPLETE → prte_plm_base_mapping_complete**

(``plm_base_launch_support.c``:276)

Posts ``SYSTEM_PREP``.

**12. SYSTEM_PREP → prte_plm_base_complete_setup**

(``plm_base_launch_support.c``)

Performs pre-launch sanity checks and environment preparation:

* Validates that there are enough slots for the requested process count.
* Constructs the environment for each app context (inheriting the HNP
  environment, applying ``-x VAR``, ``--env-merge``, and PMIx-standard
  keys).
* Calls ``prte_filem.preposition_files()`` to stage any required input
  files to the compute nodes.  The ``files_ready`` callback fires on
  completion; on success it activates ``MAP`` — **wait, this is actually
  activated from** ``vm_ready`` **for the app-job path; see below**.

.. note::
   ``SYSTEM_PREP``'s callback ``prte_plm_base_complete_setup`` does the
   environment/slot validation and then fires ``LAUNCH_APPS``.  File
   staging happens earlier, inside ``vm_ready``, before MAP is activated.
   The call chain is: ``vm_ready`` → ``preposition_files`` →
   ``files_ready`` → ``MAP`` → ... → ``SYSTEM_PREP`` → ``LAUNCH_APPS``.

**13. LAUNCH_APPS → prte_plm_base_launch_apps**

(``plm_base_launch_support.c``)

Prepares the per-daemon launch data and posts ``SEND_LAUNCH_MSG``.

**14. SEND_LAUNCH_MSG → prte_plm_base_send_launch_msg**

(``plm_base_launch_support.c``)

Builds and sends an ODLS (On-node Daemon Launch Subsystem) launch message
to each daemon that has local processes for this job.  The message contains:

* The job's namespace and process list.
* Per-process slot list (cpuset, binding directives).
* Application argv and environment.
* IOF (I/O Forwarding) channel setup — which file descriptors to forward
  for each process.
* Any PMIx server info that the processes will need at init time.

Each daemon receives the message via ``PRTE_RML_TAG_LAUNCH_APPS`` and
passes it to its ODLS component.  The ODLS ``launch_local_procs()`` entry
point iterates over the local process list and ``fork``/``exec``'s each
one.  After the exec, the child process calls ``PMIx_Init`` which connects
it to the daemon's embedded PMIx server.

**15. STARTED → job_started**

Fires once the first process has been forked on any daemon (triggered by
``PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`` receipt at the HNP—see step 16).
Notifies the originating tool via a PMIx ``PMIX_EVENT_JOB_START`` event.

**16. LOCAL_LAUNCH_COMPLETE**

Each daemon sends ``PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`` back to the HNP when
all of its local processes have attempted to start, carrying each process's
PID and state.  The HNP handler (``plm_base_receive.c``:715) accumulates
``jdata->num_launched``; when the first process is counted it posts
``STARTED``; when all processes are counted it posts ``RUNNING``.

**17. READY_FOR_DEBUG → ready_for_debug**

Optional.  If the job was submitted with ``--stop-on-exec``,
``--stop-in-init``, or ``--stop-in-app``, each daemon waits until all its
local processes signal readiness and then sends
``PRTE_PLM_READY_FOR_DEBUG_CMD`` to the HNP.  When the HNP has heard from
all daemons it fires a ``PMIX_READY_FOR_DEBUG`` PMIx event to the
originating tool.

**18. RUNNING → prte_plm_base_post_launch**

All processes across the entire job are running.  Post-launch cleanup:
timeout timers, progress callbacks, and similar housekeeping.

**19. REGISTERED → prte_plm_base_registered**

All application processes have called ``PMIx_Init`` and registered with
their local PMIx server.  Each daemon accumulates its local count and
sends ``PRTE_PLM_REGISTERED_CMD`` to the HNP when all of its local
processes have registered.  The HNP handler
(``plm_base_receive.c``:675) increments ``jdata->num_reported``; when the
count reaches ``jdata->num_procs`` it fires this state.

Process State Machine
---------------------

The process state machine tracks individual application processes.  It
runs on both the HNP (via the DVM module) and each daemon (via the prted
module), with the same set of states and a single callback
``prte_state_base_track_procs`` / ``track_procs``.

.. list-table::
   :widths: 40 10 50
   :header-rows: 1

   * - Name
     - Value
     - Meaning
   * - ``PRTE_PROC_STATE_INIT``
     - 1
     - Process entry created by RMAPS.
   * - ``PRTE_PROC_STATE_RUNNING``
     - 4
     - Daemon has forked the process.
   * - ``PRTE_PROC_STATE_REGISTERED``
     - 5
     - Process called ``PMIx_Init``.
   * - ``PRTE_PROC_STATE_IOF_COMPLETE``
     - 6
     - All I/O forwarding pipes have closed.
   * - ``PRTE_PROC_STATE_WAITPID_FIRED``
     - 7
     - ``waitpid`` detected the process has exited.
   * - ``PRTE_PROC_STATE_READY_FOR_DEBUG``
     - 9
     - Process is stopped and awaiting a debugger.
   * - ``PRTE_PROC_STATE_TERMINATED``
     - 20
     - Process is fully cleaned up.

A process is considered still running if its state is less than
``PRTE_PROC_STATE_UNTERMINATED`` (15).  States ≥
``PRTE_PROC_STATE_ERROR`` (50) indicate abnormal exit.

On the daemon side (``src/mca/state/prted/state_prted.c``:314,
``track_procs``):

* ``RUNNING``: increments ``jdata->num_launched``; when all local procs
  are running, fires ``PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE`` which
  sends ``PRTE_PLM_LOCAL_LAUNCH_COMP_CMD`` to the HNP.
* ``REGISTERED``: increments ``jdata->num_reported``; when all local procs
  have registered, sends ``PRTE_PLM_REGISTERED_CMD`` to the HNP.
* ``IOF_COMPLETE`` / ``WAITPID_FIRED``: when both flags are set for a
  process, marks it ``TERMINATED`` and triggers job-completion accounting.

Termination and Error States
----------------------------

**Boundary markers** (job states):

* ``PRTE_JOB_STATE_UNTERMINATED`` (30): any state below this means the job
  is still running.
* ``PRTE_JOB_STATE_ERROR`` (50): any state at or above this is an error.

**Normal termination sequence**:

``TERMINATED`` → ``NOTIFY_COMPLETED`` → ``NOTIFIED`` → ``ALL_JOBS_COMPLETE``
→ ``prte_quit``

**Selected error states**:

.. list-table::
   :widths: 50 10
   :header-rows: 1

   * - Name
     - Value
   * - ``PRTE_JOB_STATE_KILLED_BY_CMD``
     - 51
   * - ``PRTE_JOB_STATE_ABORTED``
     - 52
   * - ``PRTE_JOB_STATE_FAILED_TO_START``
     - 53
   * - ``PRTE_JOB_STATE_NEVER_LAUNCHED``
     - 60
   * - ``PRTE_JOB_STATE_ALLOC_FAILED``
     - 68
   * - ``PRTE_JOB_STATE_MAP_FAILED``
     - 69
   * - ``PRTE_JOB_STATE_CANNOT_LAUNCH``
     - 70
   * - ``PRTE_JOB_STATE_FORCED_EXIT``
     - 64

All error states ultimately route to ``force_quit`` or ``prte_quit`` which
calls ``prte_plm.terminate_orteds()`` before exiting.

Key Source Files
----------------

.. list-table::
   :widths: 45 55
   :header-rows: 1

   * - File
     - Role
   * - ``src/mca/plm/plm_types.h``
     - All state constant definitions.
   * - ``src/mca/state/dvm/state_dvm.c``
     - DVM job and proc state tables; ``vm_ready``, ``init_complete``,
       ``check_complete``, ``dvm_notify``, ``cleanup_job``.
   * - ``src/mca/state/prted/state_prted.c``
     - Per-daemon job and proc state tables; ``track_procs``,
       ``track_jobs``.
   * - ``src/mca/state/base/state_base_fns.c``
     - ``prte_state_base_activate_job_state`` — the core dispatch function.
   * - ``src/mca/plm/base/plm_base_launch_support.c``
     - Most PLM base callbacks: ``prte_plm_base_setup_job``,
       ``prte_plm_base_allocation_complete``,
       ``prte_plm_base_daemons_launched``,
       ``prte_plm_base_daemons_reported``, ``progress_daemons``,
       ``prte_plm_base_daemon_callback``.
   * - ``src/mca/plm/base/plm_base_receive.c``
     - HNP message handler: processes ``PRTE_PLM_LOCAL_LAUNCH_COMP_CMD``
       and ``PRTE_PLM_REGISTERED_CMD`` from daemons.
   * - ``src/mca/plm/ssh/plm_ssh_module.c``
     - SSH PLM ``launch_daemons`` callback (line 1077).
   * - ``src/mca/plm/slurm/plm_slurm_module.c``
     - SLURM PLM ``launch_daemons`` callback.
   * - ``src/mca/plm/pals/plm_pals_module.c``
     - PALS PLM ``launch_daemons`` callback.
   * - ``src/mca/plm/lsf/plm_lsf_module.c``
     - LSF PLM ``launch_daemons`` callback.
   * - ``src/mca/ras/base/ras_base_allocate.c``
     - ``prte_ras_base_add_hosts()`` (thin async wrapper, line 771);
       ``prte_ras_base_complete_request()`` (grow/shrink completion, line
       586); ``prte_ras_base_modify()`` (routes requests to RAS modules,
       line 529).
   * - ``src/mca/ras/hosts/ras_hosts.c``
     - ``ras/hosts`` module ``modify()`` entry point: parses hostfiles and
       host lists and inserts nodes into the pool (line 340).
   * - ``src/mca/ras/slurm/ras_slurm_modify_extend.c``
     - Slurm ``modify()`` entry for ``PMIX_ALLOC_EXTEND``; fires
       ``LAUNCH_DAEMONS`` directly on the daemon job (line 752) instead of
       routing through ``prte_ras_base_complete_request()`` — see the
       launch-fence warning under *DVM Extension and the Daemon-Launch
       Race*.
   * - ``src/prted/prted_comm.c``
     - ``PRTE_DAEMON_SHRINK_CMD`` handler (line 469): checks daemon rank
       list and exits cleanly if listed.

Debugging
---------

Verbose output for each subsystem is controlled at runtime:

.. code-block:: sh

   # Job state machine transitions
   prte --prtemca state_base_verbose 5 ...

   # PLM (daemon launch, message receive)
   prte --prtemca plm_base_verbose 5 ...

   # Process mapping
   prte --prtemca rmaps_base_verbose 5 ...

   # Resource allocation
   prte --prtemca ras_base_verbose 5 ...

At verbosity level 5 the state machine also prints its full table at
startup via ``prte_state_base_print_job_state_machine()``.

DVM Extension and the Daemon-Launch Race
-----------------------------------------

Background
~~~~~~~~~~

A persistent DVM can have its node pool expanded at runtime in two ways:

1. **App-triggered** (``src/mca/ras/base/ras_base_allocate.c``:771):
   A job submitted with ``--add-host`` or ``--add-hostfile`` causes the RAS
   base ``add_hosts()`` function — now a thin asynchronous wrapper — to
   collect the directives into a ``prte_pmix_server_req_t`` with
   ``req->key = "hosts"`` and ``req->allocdir = PMIX_ALLOC_EXTEND``.  It
   sets ``prte_dvm_ready = false`` to block concurrent job dispatch, then
   posts the request to the event loop for ``prte_ras_base_modify()`` to
   handle.  ``prte_ras_base_modify()`` routes the request to the ``ras/hosts``
   module, whose ``modify()`` entry point
   (``src/mca/ras/hosts/ras_hosts.c``:340) parses the hostfiles and host
   lists and inserts new nodes into ``prte_node_pool``.  On success the
   common completion function ``prte_ras_base_complete_request()`` (line 586)
   marks ``PRTE_JOB_EXTEND_DVM`` on the **daemon job** and fires
   ``PRTE_JOB_STATE_LAUNCH_DAEMONS`` on the daemon job.  Any application
   jobs that arrive while ``prte_dvm_ready`` is false are stashed in
   ``prte_cache`` and flushed when ``vm_ready()`` fires.

2. **Scheduler push** (``src/mca/ras/slurm/ras_slurm_modify_extend.c``:752):
   When Slurm grants additional nodes (e.g., in response to a
   ``PMIx_Allocate`` call from an application), the Slurm RAS component
   adds the nodes to the pool and fires ``PRTE_JOB_STATE_LAUNCH_DAEMONS``
   **directly on the daemon job**, setting ``PRTE_JOB_EXTEND_DVM`` on the
   daemon job — bypassing ``prte_ras_base_complete_request()`` and leaving
   ``prte_dvm_ready`` unchanged.

In both cases ``setup_virtual_machine()`` is called (from within the PLM's
``launch_daemons`` callback) and detects the extension via the
``PRTE_JOB_EXTEND_DVM`` attribute on the daemon job.  If new daemons are
needed it sets ``PRTE_JOB_LAUNCHED_DAEMONS`` on the daemon job and returns
with ``map->num_new_daemons > 0``.  The PLM then spawns ``prted`` processes
on the new nodes and the state machine parks at ``DAEMONS_LAUNCHED`` until
they call home.

.. warning::
   A RAS component that handles a modification request (grow or shrink)
   must route its result through ``prte_ras_base_complete_request()``
   rather than activating ``PRTE_JOB_STATE_LAUNCH_DAEMONS`` directly on the
   daemon job.  ``prte_ras_base_complete_request()`` is the single point
   that performs the bookkeeping the launch fence depends on: it sets
   ``PRTE_JOB_EXTEND_DVM`` and resets ``prte_nidmap_communicated`` on the
   grow path, and on the shrink path it records the
   ``prte_shrink_campaign_t`` and raises ``prte_dvm_launch_fence`` *before*
   any daemon is asked to leave.  A component that fires
   ``PRTE_JOB_STATE_LAUNCH_DAEMONS`` itself — as the Slurm scheduler-push
   path historically does — skips this common handling and can leave the
   fence out of step with the campaign it is supposed to gate, reopening
   the daemon-launch race described below.  New RAS modules, and any
   reworking of the existing ones, should hand their results to
   ``prte_ras_base_complete_request()`` and let it activate the state.

DVM Shrink
~~~~~~~~~~

A DVM can also be **shrunk** at runtime by releasing nodes back to the
scheduler.  The path runs through the same ``prte_ras_base_complete_request()``
function, but with ``req->allocdir == PMIX_ALLOC_RELEASE``:

1. The ``PMIX_ALLOC_RELEASE`` branch extracts the node list from
   ``PMIX_ALLOC_NODE_LIST``, looks up each node's daemon rank in
   ``prte_node_pool``, and packs the ranks into a
   ``PRTE_DAEMON_SHRINK_CMD`` message.
2. The message is broadcast to all daemons via
   ``prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON)``.
3. Each daemon that receives ``PRTE_DAEMON_SHRINK_CMD``
   (``src/prted/prted_comm.c``:469) checks whether its own rank appears in
   the unpacked list.  If listed, it:

   a. Sets ``prte_abnormal_term_ordered = true``.
   b. Fires a ``PMIX_EVENT_JOB_END`` PMIx event to notify any attached tools.
   c. Activates ``PRTE_JOB_STATE_DAEMONS_TERMINATED`` and exits cleanly.

   The HNP needs no acknowledgement from the daemon: it learns that the
   daemon is gone through the normal daemon-loss (comm-failure) path, which
   is also the only event that guarantees the daemon's routes and node state
   have actually been torn down (see below).

Unlisted daemons silently discard the command and continue running.

In addition, each RAS module may implement a ``release_allocation`` entry
point (added in ``src/mca/ras/ras.h``).  The base function
``prte_ras_base_release_allocation()`` cycles active modules in priority
order (filtering by ``session->alloc_module`` when set) and is called
automatically from the ``prte_session_t`` destructor so that allocations are
released when their session object is destructed.

Shrink Synchronisation Requirement
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``PRTE_DAEMON_SHRINK_CMD`` xcast is fire-and-forget: targeted daemons
exit on their own schedule, and the HNP must determine when all of them have
actually terminated.  This creates two race windows that must be closed.

**Race 1 — new job mapping onto a shrinking node**

A job that reaches the ``VM_READY → MAP`` boundary while a shrink is in
progress may have its processes mapped onto a node whose daemon has already
received ``PRTE_DAEMON_SHRINK_CMD``.  By the time the launch message is
sent the daemon may already have exited.

**Race 2 — in-flight job at** ``LAUNCH_APPS``

A job that was fully mapped *before* a shrink started and then reaches
``LAUNCH_APPS`` (where launch data is packed and sent to each daemon) may
send to a daemon that dies in the window between MAP and the actual send.

Closing both races requires:

1. **Completion on actual daemon death** — the HNP records the targeted
   daemon ranks in a ``prte_shrink_campaign_t`` and waits for each one to
   leave the DVM.  Departure is detected through the existing daemon-loss
   (comm-failure) path in the ``errmgr/dvm`` component, which matches the
   dead daemon's rank against the campaign's target list, drives the fence
   counter down, and releases the fence once every target is gone.  The
   HNP does not rely on any acknowledgement from the daemon: the reason a
   targeted daemon dies is irrelevant, and the comm-failure event is the
   only signal that also guarantees the daemon's routes, ``num_daemons``
   count, and node state have been cleaned up.  Each target slot is stamped
   ``PMIX_RANK_INVALID`` once counted so a repeated comm event cannot
   decrement the campaign twice.

2. **Second hold point at** ``LAUNCH_APPS`` — ``prte_plm_base_launch_apps()``
   checks a dedicated ``prte_shrink_ntargets`` counter (nonzero only when a
   shrink is in progress) and if nonzero parks the job in a second held-job
   array (``prte_prelaunch_held_jobs``) rather than packing or sending any
   launch data.  This hold uses ``prte_shrink_ntargets`` rather than the
   general ``prte_dvm_launch_fence`` so that a concurrent DVM grow does not
   unnecessarily stall jobs that have already been mapped to existing nodes.

3. **Remap on release** — when ``prte_dvm_launch_fence`` returns to zero,
   jobs in ``prte_prelaunch_held_jobs`` that were mapped to any of the now-dead
   daemon nodes are reset to ``MAP`` state so they are remapped to the
   surviving nodes; jobs whose entire mapping lies on surviving nodes are
   re-activated at ``LAUNCH_APPS`` without remapping.

The full implementation plan is in :ref:`dvm-launch-fence-label`,
*DVM Shrink Fence*.

The Race Condition
~~~~~~~~~~~~~~~~~~

The app-triggered path partially mitigates the race by setting
``prte_dvm_ready = false`` in ``add_hosts()`` before the asynchronous
request is posted: any job that arrives after that point is stashed in
``prte_cache`` and is not dispatched until ``vm_ready()`` restores
``prte_dvm_ready = true``.

The scheduler-push path does **not** clear ``prte_dvm_ready``.  Because
``prte_dvm_ready`` otherwise remains ``true`` throughout DVM operation (it
is only cleared at shutdown), any application job that arrives while a
scheduler-initiated daemon launch is in flight is dispatched immediately:

.. code-block:: text

   Thread of events (time →)

   Slurm grants new nodes
   ras_slurm_modify_extend fires LAUNCH_DAEMONS on daemon job
   PLM starts spawning prted on new nodes    ← daemon launch in progress
   App job B arrives, prte_dvm_ready==true, B is dispatched
   B: INIT → ALLOCATE → VM_READY
   B: MAP ← assigns procs to new nodes ← daemons NOT UP YET
   B: SEND_LAUNCH_MSG → daemons fail to receive it

The same race exists when multiple apps are running concurrently inside the
DVM and one of them triggers an allocation expansion: the other apps'
independent state machine progressions can interleave with the daemon launch
events.

Required Change: Gate at the VM_READY → MAP Boundary
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To eliminate the race, all application jobs must be held at the
``VM_READY → MAP`` boundary whenever any daemon launch campaign is in
progress, regardless of which path (app-triggered or scheduler push)
initiated it.  Jobs that are already past ``MAP`` (i.e., already launching
or running) are unaffected — their daemons are already up.

The mechanism is a **global launch fence** — a counter
(``prte_dvm_launch_fence``) that tracks the number of in-progress daemon
launch campaigns.  An app job that reaches the ``VM_READY → MAP`` transition
checks the fence; if it is nonzero the job parks itself in a held-job array
(``prte_held_jobs``) and is released when the fence reaches zero.

The step-by-step implementation plan is in
:ref:`dvm-launch-fence-label`.
