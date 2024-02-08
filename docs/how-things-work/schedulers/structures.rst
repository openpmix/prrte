Objects and Definitions
=======================

The following objects and definitions are used in PRRTE for support of
scheduler operations. Where applicable, modifications to previously
existing structures is highlighted.


prte_session_t
--------------
PMIx "sessions" equate to a scheduler's allocation - i.e., a session consists
of a collection of assigned resources within which the RTE will execute one
or more jobs. Sessions can be related to one another - i.e., a session can
be defined as a result of a ``PMIx_Allocation_request`` from a process within
an existing session. This relationship must be tracked as termination of the
parent session mandates termination of all child sessions, unless the user
requested that child sessions continue on their own.

PRRTE defines the following structure for tracking sessions:

.. code:: c

	typedef struct{
	    pmix_object_t super;
	    uint32_t session_id;
	    char *user_refid;
	    char *alloc_refid;
	    pmix_pointer_array_t *nodes;
	    pmix_pointer_array_t *jobs;
	    pmix_pointer_array_t *children;
	} prte_session_t;

with the following fields:

* super: the usual PRRTE object header

* session_id: the numerical ID assigned by the scheduler to
  the session. Schedulers can assign both a numerical ID and
  a string ID for a session - the two do not necessarily relate
  to one another. Both IDs are provided as the numerical ID
  can provide a faster lookup of the session, while the string ID
  is considered more user-friendly. Note that the scheduler is
  required to provide the numerical ID, but the string ID is
  optional. The session ID is included in both the response to
  the allocation request and in the ``PMIx_Session_control``
  directive given to the RTE via the ``PMIX_SESSION_ID`` attribute.

* user_refid: the user's string ID provided to the allocation request
  that generated this session via the ``PMIX_ALLOC_REQ_ID``. This will
  be ``NULL`` if the user chose not to provide the ID.

* alloc_refid: the string ID assigned by the scheduler to the session.
  Note that a scheduler is not required to assign such an ID. It is
  communicated in the ``PMIx_Session_control`` directive to the RTE
  via the ``PMIX_ALLOC_ID`` attribute.

* nodes: a pointer array containing pointers to `prte_node_t` objects
  describing the individual nodes included in this session. Nodes can
  belong to multiple sessions, although this isn't common in HPC (most
  facilities configure their schedulers for sole-occupancy of resources).

* jobs: a pointer array containing pointers to `prte_job_t` objects
  describing the individual jobs executing within the session. Note that
  jobs are not allowed to operate across sessions, though they may have
  child jobs (i.e., jobs spawned by a process from within the parent job)
  executing in another session.

* children: a pointer array containing pointers to `prte_session_t` objects
  that describe sessions resulting from a call to ``PMIx_Allocation_request``
  by a process executing within this session. Child sessions are terminated
  by default upon completion of their parent session - this behavior can be
  overridden by including the ``PMIX_ALLOC_CHILD_SEP`` attribute when calling
  ``PMIx_Allocation_request``, or by issuing a ``PMIx_Session_control`` request
  with the ``PMIX_SESSION_SEP`` attribute.

The session object is created upon receiving a ``PMIx_Session_control``
directive from the scheduler - this occurs in ``src/prted/pmix/pmix_server_session.c``
as an upcall from the PMIx server library. Session objects are stored in the
``prte_sessions`` global pointer array.



prte_job_t
----------
Extended to include pointer to the session within which this job is executing.
Note addition of ``PMIX_SPAWN_CHILD_SEP`` and ``PMIX_JOB_CTRL_SEP`` attributes.

.. code:: c

	typedef struct{
	    pmix_list_item_t super;
	    int exit_code;
	    char **personality;
	    struct prte_schizo_base_module_t *schizo;
	    pmix_nspace_t nspace;
	    char *session_dir;
	    int index;
	    pmix_rank_t offset;
	    prte_session_t *session;   <========  ADDED
	    pmix_pointer_array_t *apps;
	    prte_app_idx_t num_apps;
	    pmix_rank_t stdin_target;
	    int32_t total_slots_alloc;
	    pmix_rank_t num_procs;
	    pmix_pointer_array_t *procs;
	    struct prte_job_map_t *map;
	    prte_node_t *bookmark;
	    prte_job_state_t state;
	    pmix_rank_t num_mapped;
	    pmix_rank_t num_launched;
	    pmix_rank_t num_reported;
	    pmix_rank_t num_terminated;
	    pmix_rank_t num_daemons_reported;
	    pmix_rank_t num_ready_for_debug;
	    pmix_proc_t originator;
	    pmix_rank_t num_local_procs;
	    prte_job_flags_t flags;
	    pmix_list_t attributes;
	    pmix_data_buffer_t launch_msg;
	    pmix_list_t children;
	    pmix_nspace_t launcher;
	    uint32_t ntraces;
	    char **traces;
	    pmix_cli_result_t cli;
	} prte_job_t;

with the following fields:

* super: the usual PRRTE list item header so the object can be
  included on a PRRTE list

* exit_code: the exit code for the job. This is usually taken as
  the exit code from the first process to exit with a non-zero
  status

* personality: a string indicating the schizo component to be
  used for parsing this job's command line (if applicable),
  harvesting envars, and generally setting up the job

* schizo: a pointer to the schizo module itself

* nspace: the namespace of the job

* session_dir: the job-level session directory assigned to the job

* index: the position of this job object in the global ``prte_job_data``
  pointer array

* offset: offset to the total number of procs so shared memory
  components can potentially connect to any spawned jobs

* session: (**ADDED**) pointer to the session within which this job
  is executing. This is provided to accelerate lookup operations when
  referencing the session behind a given job.

  .. warning::

    One must `not`
    ``PMIX_RETAIN`` the ``prte_session_t`` object before assigning it to
    this field. Session objects clean up `after` all of their included
    jobs terminate and clean up - a circular dependency can be created
    that prevents job and session objects from executing their destructors.
    The ``prte_job_t`` destructor will `not` release the ``session`` field.

* apps: a pointer array containing pointers to the ``prte_app_context_t``
  objects describing the applications executing within this job.

* num_apps: the number of applications executing within this job

* stdin_target: the rank of the process that is to receive forwarded stdin
  data. A rank of ``PMIX_RANK_WILDCARD`` indicates that all processes in
  the job are to receive a copy of the data.

* total_slots_alloc: the sum total of all available slots on the nodes
  assigned to this job. Note that a job does not necessarily have access
  to all resources assigned to the session within which the job is executing.
  The job's resources can be modified by hostfile, add-hostfile, dash-host,
  and add-host directives.

* procs: a pointer array containing pointers to the ``prte_proc_t`` objects
  describing the individual processes executing as part of the job

* num_procs: the number of processes executing within the job

* map: a pointer to the job map detailing the location and binding of
  each process within the job

* bookmark: bookmark for where we are in mapping - this indicates the last
  node used to map the job. Should a process within the job initiate a
  "spawn" request, mapping of the spawned job will commence from this
  point, assuming that the resource list for the new job includes the
  bookmark location.

* state: the PRRTE state of the overall job. This is the state within the
  PRRTE state machine within which the job is currently executing.

* num_mapped: bookkeeping counter used in the mapper subsystem

* num_launched: bookkeeping counter used during job launch

* num_reported: number of processes that have called ``PMIx_Init``

* num_terminated: bookkeeping counter of process termination

* num_daemons_reported: bookkeeping counter of number of daemons
  spawned in support of the job that have reported "ready"

* num_ready_for_debug: bookkeeping counter of number of processed
  that have registered as ready for debug

* originator: ID of process that requested spawn of this job

* num_local_procs: bookkeeping counter of the number of processes
  from this job on the local node

* flags: set of bit-mapped flags used internally by PRRTE

* attributes: list of job attributes controlling the job behavior

* launch_msg: copy of the message sent to all daemons to launch
  the job's processes

* children: list of `prte_job_t` describing the jobs that have been
  started by processes executing within this parent job.

* launcher: the namespace of the tool that requested this job be
  started

* ntraces: number of stacktraces collected when PRRTE is asked to
  collect stacktraces from failed processes

* traces: the actual collected stacktraces from failed processes

* cli: the results of parsing the command line used to generate
  this job - only valid when the job is started from the ``prterun``
  command line


The job object is created in two places in the DVM system controller
(a.k.a, "master" daemon):

* upon directly receiving a ``PMIx_Spawn`` request in the PMIx server
  library, which is then upcalled in ``src/prted/pmix/pmix_server_dyn.c``.
  In this case, the job object is used to assemble the job description
  based on the PMIx attributes passed up to the PRRTE function. The
  object is subsequently packed and sent to the DVM master for processing - if
  the server itself is the DVM master, then it will just be sent to itself.
  This object is a temporary holding place for the job description and
  will be released upon completion of the spawn.

* while unpacking a relayed spawn request from another daemon in the
  DVM (who received the request from a local client or tool) or a
  "send-to-self" from the above function, in
  ``src/mca/plm/base/plm_base_receive.c``. The job object is assigned
  the relevant ``prte_session_t`` object based on the following (in
  order of priority):

  * the session ID, if specified
  * the allocation ID, if given
  * the user's allocation reference ID, if given
  * the session of the parent job, if the spawn requestor is an
    application process (and therefore has a parent job)
  * the default session, which is composed of the global node pool, if
    the spawn requestor is a tool

A job is required to be assigned to a session - if no session is found,
or the specified session is unknown to PRRTE, then the spawn request
will be denied with an appropriate error code.


