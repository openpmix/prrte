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
               contains roughly 1,300 commits touching over 1,000 files,
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
  nodes they will run on have daemons. A DVM that was not started in
  elastic mode holds none of that machinery, and now refuses a size
  change -- an ``--add-host``, ``--add-hostfile``, ``--activate``, or a
  PMIx allocation request -- with a message saying so, rather than
  launching a daemon nothing can roll back. A grow that fails releases
  the jobs parked behind it instead of leaving the DVM not-ready for the
  rest of its life.

* **Slurm elastic support.** A full implementation of the above for
  Slurm: expander jobs allocated with ``salloc --no-shell``, release by
  node list or by allocation id, reuse of shrunk nodes on a later
  extend, cancellation by user-provided request id, and completion of an
  extend when its ``salloc`` exits rather than by polling. The node
  hosting the HNP is identified by locality -- so that a shortened or
  fully qualified spelling of its name still matches -- and is never
  handed back, whichever session holds it. This support is gated by the
  ``prte_elastic_mode`` MCA parameter, and requires jansson and Slurm
  v24.05 or later (see ``--enable-slurm-extensions``). The version probe
  also reads the version Debian and Ubuntu print -- their clients answer
  ``slurm-wlm 23.11.4`` under their own package name -- which used to
  read as no Slurm at all, so a DVM started inside a live allocation fell
  back to ssh and one node without a word.

  A job record from ``scontrol show job --json`` is no longer parsed
  whole. It is walked through a fixed-size window, keeping only the
  members PRRTE reads and discarding the rest as they arrive, and a
  job's node allocation is taken one node at a time. A large job's
  record -- which grows with every socket and core of every node, and
  runs to hundreds of megabytes of JSON -- therefore costs the HNP the
  same as a small one, where it used to cost that much again several
  times over as a parsed document, and the 1MB ceiling on the record
  read by a shrink is gone. A record whose node list disagrees with the
  count Slurm prints ahead of it is refused rather than acted on, a
  record refused part way through adds none of its nodes, and a member
  too large for the window is refused by name. A read refused for
  exceeding its limit now reaches the requester as
  ``PMIX_ERR_OUT_OF_RESOURCE`` rather than a bare ``PMIX_ERROR``.

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
  round is not absorbed into the next, and group operations now carry
  the same numbering: a group ID used twice -- which is what
  ``MPI_Comm_create_from_group`` produces from a repeated string tag --
  completes the second time rather than hanging on a daemon that
  remembered the first. A group's final membership comes back in the
  order its participants gave it rather than sorted, and every
  participant's completion is discharged rather than only the last
  one's. A broadcast is identified by its tree as well as its number, so
  a routing-tree operation and a release-tree operation that share a
  number are no longer taken for one another.

* **RML and OOB.** Each peer's socket can be serviced on its own
  progress thread, several sends can share one payload, the message
  header carries one namespace rather than two full process identifiers,
  and a connection attempt tries the remaining addresses before giving
  up. A daemon that has died is reported as such instead of as a
  suspected firewall.

  ``prte_if_include`` and ``prte_if_exclude`` now decide what is listened
  on, not merely what is advertised. The OOB opens one socket per
  selected address rather than binding the wildcard in both families, the
  PMIx tool listener is given the same selection when remote connections
  are enabled, and an include list naming only the loopback interface
  brings up a single-node DVM that never reaches the network -- which is
  how a macOS user keeps ``mpirun`` from raising a firewall prompt, and
  which used to fail to start at all. Each address family keeps its own
  list of interface masks, so reachability is no longer scored from an
  IPv4 address paired with an IPv6 prefix, two identical IPv6 addresses
  compare equal as the IPv4 pair always did, and the IPv6 listening
  socket sets ``IPV6_V6ONLY``. An accept that fails for a reason
  belonging to the connection being accepted no longer closes the
  listener for the life of the daemon, and the connect handshake is read
  as its bytes arrive instead of in a blocking loop on the progress
  thread, where one byte sent to the OOB port followed by silence stalled
  everything the daemon was doing.

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
  now covered by golden corpora under ``test/unit/``. A hostfile that is
  not text -- one saved as UTF-16, or truncated -- is refused rather than
  read as fragments of the lines it holds; an exclusion written
  ``^user@host`` excludes, where the ``^`` used to be looked for after
  the user had been split away; an ``@`` with nothing on one side of it
  is refused instead of becoming a node name; a hostfile holding nothing
  but exclusions excludes, where it was taken for an empty file and the
  job ran everywhere; and a ``slots=`` written on a relative entry
  (``+n0``, ``+e``) is applied rather than parsed and dropped. The
  rankfile map is keyed by rank instead of indexed by it, so a rankfile
  naming rank 1000000000 no longer has the head node allocate eight
  gigabytes before the mapper looks at the job, and ``+N3`` is read the
  way ``+n3`` always was.

* **Data server.** Published data now honors the lifetime and
  persistence its publisher asked for, access is decided by permissions
  and then by range, what one user may hold is bounded, and an external
  data server is reached as a PMIx tool rather than over the RML. An
  unpublish of a ``NAMESPACE``-, ``LOCAL``- or ``PROC_LOCAL``-range key
  reaches only its own set's copy, where either of two jobs belonging to
  one user used to take the other's out from under a job still running. A
  parked ``PMIX_WAIT`` lookup is answered once, with everything it waited
  for, and is finished when its requestor dies rather than consuming a
  first-read value on a dead process's behalf. A ``PMIX_TIMEOUT`` given
  to ``PMIx_Publish`` is read as the directive it is rather than stored
  as published data, where it collided with the same user's next timed
  publish. A range or persistence sent as an ordinary integer is read as
  what it means rather than as whatever the union member happened to
  hold.

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

A second pass over the tree since the release candidate concentrated on
what a user or a tool can reach directly. Ten command lines killed the
tool before it could say anything -- ``--map-by=``, ``--output=``,
``--rtos :foo`` and their kin -- and are now refusals with a message;
``--map-by slot:PE=0`` was an integer division by zero inside the HNP;
``--map-by pe-list=-`` faulted before the job had been described; a blank
or commented line in an ``-app`` appfile crashed the parse of the line
behind it; and a colocation request, the one mapper input that comes
straight off a tool's ``PMIx_Spawn``, was read past the end of what the
tool sent. The session directory is refused if another user owns it or if
it is writable by group or other, closing a predictable name planted
under a world-writable ``/tmp``. A query's qualifiers are screened rather
than read out of whichever union member was asked for, so a spawning
process can no longer fault the daemon answering it.

Several races that produced a hang rather than a crash are closed. A kill
that crossed a launch marked a child terminated while its fork was still
queued, so the process ran on past the job that had been aborted; a late
launch report could write ``RUNNING`` over a job already ``TERMINATED``
and keep ``prterun`` alive for ever; a daemon that failed to start while
the DVM was still forming left the launch waiting instead of ending it;
RELM dropped the very message it exists to deliver when the daemon
holding it died; and a job held by the elastic launch fence was caught up
to newly grown daemons as though it were already running, so its own
launch message reached them as a duplicate and forked nothing.

Placement now matches what the mapper documents. ``round_robin`` is held
to a node's ``max_slots``, which no oversubscribe directive lifts, and
``--map-by node`` no longer lets a small node at the head of the list
shrink the share of every node behind it. ``--map-by device`` refuses a
device that carries no vendor identity whatever spelling named it, rather
than handing processes an assignment no GPU runtime can act on, and
``interleave=numa`` interleaves. A ``pe-list`` proc allowed to overload
binds to its list rather than to an empty cpuset. A ``nolocal`` written
on one app of an MPMD job is hoisted to the job, where it can be read at
all -- it travels in the mapping policy word, which the mapper is handed
one of per job, so before this the app simply ran on the head node.
``--uniform-nodes`` asserts that every node is
identical and stops PRRTE asking, so a node that then reports a different
topology now fails the launch and is named, instead of being described to
the mapper as something it is not. ``PMIX_NOTIFY_COMPLETION`` is honored:
the spawn path wrote one attribute and the notifier read another, so a
tool that declined notification was notified anyway.

Every command-line diagnostic ``prte`` and ``prterun`` produced was
printed twice -- the direct write to stderr that exists because a
daemon's log would otherwise be dropped fires before PMIx exists to drop
it -- and is now printed once. A launch that fails on every node exits
with the status ``PMIx_Spawn`` reported to the tool, rather than
whichever writer reached the exit status first.

Two answers that depended on which daemon a client happened to reach are
now the same everywhere. ``PMIX_QUERY_PROC_TABLE`` asked of a daemon that
does not host the procs it names is relayed to the DVM master rather than
answered out of the stale states the launch message shipped, so a client
is no longer told that every off-node rank of its own job is still
starting up -- that table being the only signal PRRTE offers for "has
this proc gone away". A daemon that has never heard of a job at all now
defers to the master instead of reporting the job as not found.

A singleton is recognized by its identity rather than by PMIx having
handed it no server object. A PMIx that adopts a registration for a
client that connected ahead of it (openpmix/openpmix#4279) hands the
singleton its object like any other client, and with
``pmix_require_pid_match`` set the old inference refused the singleton's
connection.

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
  that can say no. It can grow a job record past anything the DVM
  holds, and checks that the HNP's peak memory does not move when it
  does.

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
guides for contributors; a community Code of Conduct; and a security
policy (``SECURITY.md``, rendered into the documentation) describing how
to report a vulnerability privately and what is supported.

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

* ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` is gone. It existed to let a
  caller undo the sort a group construct applied to its final
  membership; the sort is gone, so the membership comes back in the
  order the participants gave it and there is nothing left to undo.

* Each tool's help file is now cross-checked against that tool's schizo
  option table at build time, so a documented option the tool does not
  accept -- or an accepted option with no help entry -- fails the build.
  A help file that gives the same topic twice fails it as well: the
  second section silently replaced the first, and a ``show_help`` call
  written against the first then printed the second's text formatted
  with the first's arguments. A job attribute that is written but never
  read, or read but never written, fails the build too.

* ``autogen.pl`` no longer applies four Libtool patches that matched
  nothing in any supported Libtool, fixes the macOS Big Sur patch that
  had never matched at all, and now reports any remaining patch that
  matched nothing rather than announcing it as applied.
