PRRTE v5.x series
=================

This file contains all the NEWS updates for the PRRTE v5.x
series, in reverse chronological order.

5.0.0 -- TBD
------------
.. important:: This is the first release in the v5 family. Where the v4
               series closed out support for the PMIx Group family of
               APIs, the work in this series has gone into making the
               DVM *elastic*, *survivable*, and *testable*: a running DVM
               can grow and shrink under a scheduler's direction, daemon
               failures no longer take it down, and PRRTE now ships
               automated test harnesses from unit tests up to a
               multi-node suite that runs on real clusters. A DVM run
               as root can also be shared between users, each job
               running as the user it belongs to. A systematic review
               of the tree corrected several hundred defects along the
               way.

               The release requires PMIx v7.0.0 or later for both build
               and execution.

Highlights
^^^^^^^^^^

.. rst-class:: open

* **Elastic DVM.** A running DVM can add and release nodes through
  allocation requests passed via RAS to a scheduler. A grow that fails
  is rolled back, jobs are held until their nodes have daemons, and the
  requester is told when the change completes.

* **Slurm elastic support.** Expander jobs via ``salloc --no-shell``,
  release by node list or allocation id, and cancellation by request id.
  Gated by ``prte_elastic_mode``; requires jansson and Slurm v24.05 or
  later (see ``--enable-slurm-extensions``). ``scontrol show job
  --json`` output is streamed, so the HNP's memory no longer grows with
  the size of the job. An expander inherits the parent job's GRES,
  reservation, required features, and excluded nodes
  (``ras_slurm_propagate_extra`` names further job record members), and
  a queued extend is named by its allocation id, so it can be cancelled
  from another tool. ``srun`` reads its node list from a file, so a
  launch is no longer bounded by the 128 KiB argument limit, and grows
  work under Slurm step management.

* **Node reservation and session targeting.** A job may name the
  allocation it is to be mapped onto. Each allocation has one owner,
  access is decided by permissions, and its properties can be queried.
  An allocation's id is the one its grantor gives it, a request id names
  an allocation only to the job that made it, and the time an allocation
  request asks for is applied.

* **Sharing a DVM between users.** A DVM run as root with
  ``prte_pmix_no_foreign_tools 0`` accepts other users' tools at every
  daemon, starts each job's processes as the user the job belongs to,
  and opens the files a job names as that user. Who may act on a job,
  session, reservation, process set, or allocation, and who may grow
  the DVM, is decided by user and group through PMIx's access rule. See
  :doc:`/launching-apps/multi-user`.

* **Authenticated daemon connections.** Each daemon proves it holds the
  DVM key when it connects, and every message on the connection carries
  a Poly1305 tag that is checked before the message is acted on.

* **Launcher-less bootstrap.** Daemons can assemble a DVM themselves,
  with no ssh, srun, or other launcher.

* **Survivable daemon failures.** A reliable messaging layer (RELM)
  beneath the RML, routing-tree repair when a daemon dies, and
  boot-epoch stamps so a stale incarnation cannot rejoin. A daemon that
  returns is re-inserted and caught up. Group constructs complete on the
  survivors. A lost daemon's node is marked down and leaves the
  allocation, fences complete once every process on a daemon has
  departed, and a restarted process takes part in them again.

* **Mapping by device.** ``--map-by device=<class|name>`` places
  processes against GPUs, NICs, or fabric devices, with ``ppr:N:device=``,
  ``ndev``, and ``interleave``. Each process is told its device, and the
  vendor's visibility environment variable is set. An ``ndev`` group
  stays under one PCI root complex, and ``--map-by device=gpu:nic``
  gives each process the RDMA NICs under its GPU's root complex, ranked
  by PCIe link speed.

* **Memory binding.** ``--membind`` binds each process's memory to the
  local NUMA node(s) that are best by ``CAPACITY``, ``LOCALITY``,
  ``BANDWIDTH``, or ``LATENCY`` -- for example, to on-package HBM -- and
  is carried on PMIx's ``PMIX_MEMBIND``.

* **Per-app placement for MPMD jobs.** Each app context carries its own
  ``--map-by``, ``--rank-by``, ``--bind-to``, ``--membind``, and
  ``pe-list`` directives.

* **Collectives reworked.** ``grpcomm`` is now plain code rather than an
  MCA framework. Large broadcasts are scattered and gathered, fences can
  gather laterally, and releases travel a separate low-radix tree. A
  group ID already in use is refused, contributions that name different
  members fail the operation, and a fence or group operation that times
  out names the members it was waiting for.

* **Connected jobs.** Departures from a connected set of jobs are
  announced as sets rather than with one broadcast per process. A
  spawned job inherits its parent's error handling, inheritance passes on
  to the spawned job's own children, and a recoverable job is no longer
  terminated because a job connected to it failed.

* **Messaging reworked.** Per-peer progress threads and shared payloads
  in the RML/OOB. ``prte_if_include`` and ``prte_if_exclude`` now decide
  what is listened on, so a loopback-only DVM works (and avoids the
  macOS firewall prompt). A silent peer can no longer stall a daemon.

* **I/O forwarding.** XON/XOFF actually stops the producer, large stdin
  writes are split rather than truncated, output file names can be
  composed by the user, and tools connected to a non-master daemon get
  working I/O.

* **Data server.** Published data honors its requested lifetime and
  persistence, access is by permission and then range, per-user holdings
  are bounded, and an external data server is reached as a PMIx tool.

* **Command-line parsing and mapper options.** Numerous fixes to how
  the values of ``--map-by``, ``--rank-by``, ``--bind-to``, ``--output``,
  ``--display``, and ``--rtos`` are parsed and applied, including
  refusing ambiguous abbreviations. See the DIRECTIVES AND QUALIFIERS
  section of :ref:`prterun(1) <man1-prterun>` and the
  :doc:`/placement/index` documentation.

* **Hostfile and rankfile parsing.** Both are now read by one line
  reader, with numerous fixes and error messages that name the line at
  fault. Node lookups are indexed, so reading a large hostfile no longer
  takes time quadratic in its length. See :doc:`/hosts/hostfiles` and
  :doc:`/placement/rankfiles`.

* **Fewer hangs.** Races between kill and launch, late launch reports,
  daemons failing during DVM formation, ``MPI_Abort``, the elastic
  launch fence, DVM shutdown while a spawned child was still dying, and
  a ``pterm`` whose halt was refused have all been closed.

* **Hardening.** Several hundred leaks, use-after-free errors, unchecked
  unpacks, and crashes on malformed input were fixed; command lines that
  used to crash a tool are now refused with a message. Session
  directories, logs, and staged files resist planted links and foreign
  ownership.

* **PMIx server surface.** Every PMIx callback and upcall is
  thread-shifted onto PRRTE's progress thread. Queries a daemon cannot
  answer, including ``PMIX_QUERY_PROC_TABLE``, are relayed to the DVM
  master, so the answer no longer depends on which daemon was asked.
  ``prun``, ``prterun``, and ``pterm`` reflect the job's outcome in
  their exit status. With PMIx v7, ``--report-uri`` takes ``tcp-`` and
  ``tcp+`` for the TCP URI that clients of earlier PMIx releases use;
  :doc:`/launching-apps/troubleshooting` says how to run those clients
  with legacy peers.

* **prun matches prterun.** ``prun`` keeps quotes in application
  arguments, forwards exported ``PMIX_MCA_*`` / ``OMPI_MCA_*`` settings
  under a persistent DVM, and honors ``--dvm`` under the ompi
  personality. A ``prun`` in a ``prterun`` job script has its output
  shown once and keeps its step's exit status to itself, and a ``prun``
  on any node of a DVM receives the master's ``--display`` reports.

* **MCA parameter routing.** A generic ``--mca`` is routed by the same
  prefix list whatever else is on the command line, and ``if_*``
  parameters reach PMIx (as ``pif_*``) and Open MPI.

* **Options that were ignored now work.** ``--tune`` under the native
  personality, ``--xterm``, ``prte_info --param``, and comments in
  ``prun`` appfiles. Diagnostics are printed once, and every
  ``show_help`` message names its job so a later job's is not
  suppressed. A message about a tool's request goes only to that tool,
  and the build checks every ``show_help`` call against its topic and
  argument count.

* **Also new:** resource usage monitoring, ``ras/flux``, ``rmaps/lsf``,
  PBS launch via ``pbs_tmrsh``, and ``--activate`` to start a daemon on
  an allocated node.

* **Documentation.** Full man pages for ``prte``, ``prterun``, ``prun``,
  ``pterm``, and ``prted``, built from the same source as ``--help``; a
  Testing chapter; launch guides for each supported resource manager; a
  "how things work" section; design records under ``docs/plans``; a Code
  of Conduct; and a security policy in ``SECURITY.md``.

* **Testing.** Five harnesses, where v4.0.0 had none: ``make check``
  unit tests; ``make -C test/offline check-offline`` for over a thousand
  mapping, ranking, and binding cases; ``contrib/dockerswarm``, a
  ten-node multi-node suite that also runs on a real cluster over ssh;
  ``contrib/slurmswarm``, the same suite under a real Slurm; and
  ``contrib/scaling`` for collective performance.

Compatibility notes
^^^^^^^^^^^^^^^^^^^

.. rst-class:: open

* **PMIx v7.0.0 or later** is required to build and run, including
  ``pmix_cli_match()`` (the ``CLI_MATCH`` capability). Building from a
  Git clone needs Python v3.6 or later and no longer needs flex.

* **Every process in a DVM must come from the same PRRTE build.** There
  is no compatibility between versions of ``prte``, ``prted``, and
  ``prun``.

* **Out-of-tree MCA components must be rebuilt**; framework interface
  versions are now checked at load time.

* **PRRTE error codes have new values,** now based at
  ``PMIX_EXTERNAL_ERR_BASE``. 46 of them previously collided with live
  PMIx status codes.

* **Ambiguous abbreviations are refused.** Spellings that were resolved
  silently before -- ``--bind-to n``, ``:s``, ``:i``, ``--rank-by s``,
  ``--output ta`` -- now produce an error naming the candidates.

* **Daemon authentication cannot be turned off.**
  ``prte_oob_authenticate`` and ``prte_oob_authenticate_messages`` are
  gone. The DVM key is handed to each daemon through the launch agent,
  so ``rsh`` and ``remsh`` agents are refused and the default agent list
  is ``ssh``; under Grid Engine, ``qrsh`` now forwards the key.

* **A host may not name an account to reach it as.** ``user@host``, in
  any host list, hostfile, rankfile, or allocation request, and the
  hostfile ``username=`` key are refused.

* ``prte.conf``, a ``DVMNodes=file:`` list, and the data server's URI
  file are read only when their ownership and permissions meet the rule
  in :doc:`/configuration`.

* On a DVM not run as root, a job belonging to another user is refused.
  A scheduler that instantiates a session must name its owner with
  ``PMIX_ALLOC_UID`` and ``PMIX_ALLOC_GID``.

* The processes a node may hold, even when oversubscribed, are capped
  at ``prte_rmaps_base_max_procs_per_slot`` (default 64) times its
  slots, but never below ``prte_rmaps_base_max_procs_per_node_floor``
  (default 1024). Set the first to 0 to remove the cap.

* ``configure`` now refuses hwloc older than v2.1.0, which it had
  accepted.

* The release scripts moved from ``contrib/dist`` to ``contrib``, and
  ``make_dist_tarball`` takes the same options as PMIx's.

* A DVM not started in elastic mode refuses ``--add-host``,
  ``--add-hostfile``, ``--activate``, and PMIx allocation requests.

* ``round_robin`` mapping is held to each node's ``max_slots``, which no
  oversubscribe directive lifts.

* With ``--uniform-nodes``, a node whose topology differs fails the
  launch rather than being mapped as something it is not.

* Each top-level session directory is now
  ``<tmpdir>/<prefix>.XXXXXX``, made by ``mkdtemp()``, so a directory
  already at a fixed name is never reused.

* Group membership keeps the order the participants gave it;
  ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` is gone.

* Removed: Java support, the ``prtedl`` framework, the ``dist`` mapping
  policy, LIKWID references, the ``--tmpdir``, ``--test-suicide``, and
  ``--alloc-refid`` options (name an allocation with ``--alloc-id``), and
  the ``prte_pmix_remote_connections`` and
  ``prte_pmix_allow_client_clones`` parameters.

A full list of individual changes will not be provided here,
but will commence with the v5.0.1 release.
