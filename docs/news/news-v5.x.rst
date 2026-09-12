PRRTE v5.x series
=================

This file contains all the NEWS updates for the PRRTE v5.x
series, in reverse chronological order.

5.0.0 -- TBD
------------
.. important:: This is the first release in the v5 family. Where the v4
               series closed out support for the PMIx Group family of
               APIs, the work in this series has gone into making the
               DVM *elastic*, *survivable*, and *testable*. The release
               contains roughly 1,200 commits touching over 1,000 files,
               and includes the following significant changes:

               * the DVM can now grow and shrink while jobs are running,
                 driven by allocation requests passed through the RAS
                 framework to a scheduler
               * a DVM can be bootstrapped without a launcher, its
                 daemons assembling themselves into the routing tree
               * daemon failures are survivable: a reliable messaging
                 layer, tree repair, and recovery of a daemon that returns
               * processes can be mapped and bound by device (GPU, NIC)
               * each app context of an MPMD job carries its own mapping,
                 ranking, and binding directives
               * minimum PMIx required for build and execution is now
                 v7.0.0
               * Java support has been removed

Because a change list of this size is not useful read as a list of
individual commits, the notable work is summarized by area below.

New capabilities
^^^^^^^^^^^^^^^^

* **Elastic DVM.** Allocation requests now flow through RAS to a
  scheduler and back, so a running DVM can add nodes, release them
  (terminating any processes still resident on a released node), roll
  back a grow that fails partway, and emit size-change completion events
  to the requester that asked for the change. Jobs are held until the
  nodes they will run on have daemons.

* **Slurm elastic support.** A full implementation of the above for
  Slurm: expander jobs allocated with ``salloc --no-shell``, release by
  node list or by allocation id, reuse of shrunk nodes on a later
  extend, cancellation by user-provided request id, and completion of an
  extend when its ``salloc`` exits rather than by polling. The node
  hosting the HNP is identified by locality -- so that a shortened or
  fully qualified spelling of its name still matches -- and is never
  handed back, whichever session holds it. This support is gated by the
  ``prte_elastic_mode`` MCA parameter, and requires jansson and Slurm
  v24.05 or later (see ``--enable-slurm-extensions``).

* **Node reservation and session targeting.** A job may name the
  allocation onto which it is to be mapped. An allocation has exactly
  one owner, access to it is decided by permissions, and its properties
  can be queried.

* **Launcher-less DVM bootstrap.** Daemons can now assemble a DVM
  themselves -- with no ssh, srun, or other launcher -- synthesizing
  parent URIs, healing ancestors into the radix tree, and forming and
  launching jobs once the tree has converged.

* **Fault tolerance.** A reliable messaging layer (RELM) beneath the
  RML, repair of the routing tree when a daemon dies, boot-epoch stamps
  on the wire so a stale incarnation cannot rejoin, and an "unheal" path
  that re-inserts a daemon that comes back and catches it up to the
  current collective epoch. Group constructs complete on the survivors
  rather than taking down the DVM.

* **Mapping by device.** ``--map-by device=<class|name>`` places
  processes against GPUs, NICs, or fabric devices, with
  ``ppr:N:device=`` to place N processes per device, ``ndev`` to assign
  several devices to each process, and ``interleave`` and sharing
  qualifiers. Each process is told which device it was mapped against,
  and the vendor's visibility environment variable is set for it.

* **Per-app placement for MPMD jobs.** Each app context carries its own
  ``--map-by``, ``--rank-by``, ``--bind-to``, and ``pe-list`` directives,
  with ranking and binding defaults derived from that app's mapping. A
  directive given alone still belongs to the job rather than to an app,
  and the map display shows per-app policy lines.

* Support for resource usage monitoring, a ``ras/flux`` component, an
  ``rmaps/lsf`` component, PBS launch via ``pbs_tmrsh``, and an
  ``--activate`` option that starts a daemon on an allocated node.

Reworked subsystems
^^^^^^^^^^^^^^^^^^^

* **Collectives.** ``grpcomm`` has been collapsed out of MCA into plain
  code. Large broadcasts are moved by scattering and gathering them
  back, a fence may gather laterally instead of through the controller,
  and a fence's release travels a separate low-radix tree (radix 4 by
  default). Fences carry a round number so that a straggler from one
  round is not absorbed into the next.

* **RML and OOB.** Each peer's socket can be serviced on its own
  progress thread, several sends can share one payload, the message
  header carries one namespace rather than two full process identifiers,
  and a connection attempt tries the remaining addresses before giving
  up. A daemon that has died is reported as such instead of as a
  suspected firewall.

* **I/O forwarding.** The XON/XOFF protocol now actually stops the
  producer, oversized stdin writes are split across chunks rather than
  truncated, output file names can be composed by the user, output
  forwarding is inherited by spawned jobs unless refused, and a tool
  connected to a non-master daemon gets working I/O in both directions.

* **Hostfile and rankfile parsing.** Both files are line-oriented
  formats, and both are now read a line at a time by one shared reader
  instead of by a flex scanner whose token stream flattened the line
  structure each parser then rebuilt. Several long-standing defects go
  with the rewrite: a hostfile written with CRLF line endings parses
  rather than failing with a message that explained nothing, a username
  containing a dot is kept rather than silently dropped, a block comment
  may sit anywhere on a line, ``+N0`` is accepted as ``+n0`` always was,
  and a second host name written on the same line is refused by name
  rather than discarded in silence. In a rankfile, naming the same rank
  twice is refused wherever it appears rather than only when both lines
  carry a ``slot=``, a line refused part way through no longer leaves a
  half-built record in the map, and a long cpu list is no longer
  truncated at 64 bytes. Both parsers now report a syntax error by
  naming the line and the text they stopped at, rather than an internal
  token number. The rankfile reader has moved out of ``rmaps/rank_file``
  into ``libprrte``, which is what makes it testable at all; both are
  now covered by golden corpora under ``test/unit/``.

* **Data server.** Published data now honors the lifetime and
  persistence its publisher asked for, access is decided by permissions
  and then by range, what one user may hold is bounded, and an external
  data server is reached as a PMIx tool rather than over the RML.

* **PMIx server surface.** Every PMIx callback and server module upcall
  is thread-shifted onto the PRRTE progress thread before touching PRRTE
  state, closing a class of intermittent hangs. A query that a daemon
  cannot answer is relayed to the DVM master, tool registration builds
  its job object from what the tool actually sent, and ``prun``,
  ``prterun``, and ``pterm`` reflect the job's outcome in their exit
  status.

Correctness
^^^^^^^^^^^

A framework-by-framework review of the tree -- rmaps, ras, plm, odls,
iof, errmgr, filem, ess, state, grpcomm, rml, runtime, util, hwloc, the
tools, and the PMIx shim -- corrected several hundred memory leaks,
use-after-free errors, unchecked unpacks, and crashes on malformed
input, among them a heap overrun on a short ``--prefix``, a segfault a
connecting tool could provoke in its daemon, and an
async-signal-unsafe fork/exec child path. PRRTE's error codes have been
rebased onto ``PMIX_EXTERNAL_ERR_BASE``, the range PMIx reserves for
projects built on it; 46 of them previously held the value of a live
PMIx status meaning something else. Every framework now states its
interface version and checks it against the components it loads.

Two answers that depended on which daemon a client happened to reach are
now the same everywhere. ``PMIX_QUERY_PROC_TABLE`` asked of a daemon that
does not host the procs it names is relayed to the DVM master rather than
answered out of the stale states the launch message shipped, so a client
is no longer told that every off-node rank of its own job is still
starting up -- that table being the only signal PRRTE offers for "has
this proc gone away". A daemon that has never heard of a job at all now
defers to the master instead of reporting the job as not found.

Every ``show_help`` message now names the job it is about. PMIx keys
duplicate suppression on the job, and a DVM runs many jobs over its
lifetime: without the job, the first job to trip a diagnostic got the
message and every later one was left in silence, for as long as the DVM
lived.

Testing
^^^^^^^

PRRTE had no automated tests at v4.0.0. It now ships five harnesses:

* ``make check`` runs per-framework unit tests under ``test/unit/``,
  which reach components through the MCA framework rather than through
  their symbols.

* ``make -C test/offline check-offline`` drives over a thousand mapping,
  ranking, and binding cases against the synthetic topologies in
  ``test/topologies/``, checked against invariants and golden maps
  without launching anything.

* ``contrib/dockerswarm`` builds your live working tree and runs it as a
  ten-node containerized DVM, covering launch, I/O forwarding, file
  preloading, elastic grow and shrink, relay, and client churn, with
  AddressSanitizer and valgrind arms.

* ``contrib/slurmswarm`` is the same harness with a real ``slurmctld``
  and ten ``slurmd``\ s, and is the only place ``plm/slurm`` and the
  ``scontrol show job --json`` parser are exercised against a scheduler
  that can say no.

* ``contrib/scaling`` measures collective performance on a real
  allocation.

Documentation
^^^^^^^^^^^^^

New sections covering how to launch applications under each supported
resource manager (Slurm, LSF, TM, Grid Engine, ssh, localhost) with
prerequisites and troubleshooting; a "how things work" section
describing the state machine, the RML transport and reliable messaging,
file preloading, and publish/lookup; design records for the major
features under ``docs/plans``; per-directory ``AGENTS.md`` orientation
guides for contributors; and a community Code of Conduct.

Requirement changes and removals
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* The minimum PMIx version for both build and execution is now v7.0.0,
  and the minimum Python needed to build from a Git clone is v3.6.

* Flex is no longer needed to build PRRTE from a Git clone. The hostfile
  and rankfile scanners were the last flex input in the tree, and with
  them gone so are ``AC_PROG_LEX``, the version floor in ``VERSION``, and
  the per-directory compiler-flag overrides that existed to tolerate
  generated scanner output.

* Java support has been removed, as has the vestigial ``prtedl``
  dynamic-loader framework, the stale ``dist`` mapping policy, the
  remaining LIKWID references, and the ``--tmpdir`` and
  ``--test-suicide`` options, which nothing read.

* Each tool's help file is now cross-checked against that tool's schizo
  option table at build time, so a documented option the tool does not
  accept -- or an accepted option with no help entry -- fails the build.
