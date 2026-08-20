Deferred work
=============

Work that is knowingly incomplete, with enough of a pointer to pick it up.

This is **not** the issue tracker: anything a user has reported belongs at
https://github.com/openpmix/prrte/issues.  What is collected here is the
other kind — a path the code takes deliberately today because the better one
was left for later, a test that cannot exist yet, a measurement nobody has
been able to take.  Each entry says where the evidence is, so that the next
person can decide whether it is still true before acting on it.

Add to it when you leave something undone, and delete from it when you do
the work.  *Every marker* left in PRRTE's own sources — a ``TODO``, a
``FIXME``, an ``XXX``, an arm deliberately left empty — belongs here in
prose; **Every marker in the code** is the table that makes that
correspondence checkable, and a marker missing from it means one of the two
is stale.

The two sections after that table are of a different kind.  **Review status**
records how far the file-by-file review pass has reached, and — the part that
matters more — which subsystems have been rebuilt since their review, so a
guide's authority can be judged before it is trusted.  **Decided against** is
the other outcome: work that was investigated and deliberately not done, kept
so nobody spends the effort a second time.

Runtime behavior
----------------

**``ras/flux`` has no ``modify()``.**  It returns ``PMIX_ERR_NOT_SUPPORTED``
(``src/mca/ras/flux/ras_flux_module.c``), so the elastic extend/release
surface exists for SLURM only.  Everything above the component is
RM-agnostic; what is missing is the Flux-side conversation.

**``filem/raw`` does not survive losing a daemon mid-staging.**  Its fault
handler is deliberately minimal (``src/mca/filem/raw/filem_raw_module.c``);
the note in place observes that the real thing should be straightforward,
since the transport it stages over (xcast) is already resilient.  Until then,
a daemon lost while files are in flight fails the staging rather than
re-driving it.

**A TCP peer that cannot be reached at one address is closed, not
retried.**  In ``src/rml/oob/oob_tcp_connection.c`` the final
connect-failure arm closes the peer and returns ``PRTE_ERR_UNREACH``
where it should force the next address in the peer's list to be tried.
A peer with several interfaces therefore gets fewer chances than the
address list implies.

**Routing-tree state is not preserved across a DVM resize.**
``prte_rml_compute_routing_tree`` re-initializes the failure bitmaps on every
grow and restores only the permanent sets (``dead_dmns``, ``absent_dmns``);
whether anything else should survive the recompute is an open question marked
in ``src/rml/routed_radix.c``.

**Only a command line can ask for a daemon on an allocated node.**  The
command-line half of this is done: ``--activate`` (``prun`` and
``prterun``) takes node names, ``+all``, ``+n<K>`` and ``file=<hostfile>``,
resolves them against the node pool, marks the chosen entries
``PRTE_NODE_STATE_ADDED`` and calls ``prte_ras_base_activate_dvm_grow()``
(``prte_ras_base_activate_hosts``).  It needs no ``ras`` module, touches no
scheduler, and is safe by construction since it can only name nodes the
allocation already contains - which is why it is allowed where
``--add-host`` is refused.  A *programmatic* requester has no equivalent:
the directive travels as a PRRTE-private spawn key
(``prte.activate.hosts``) precisely because PMIx has no standard way to say
"start a daemon on a node I already hold".

The intended shape is a new **allocation directive**, not an extension of
some existing attribute's meaning.  Add ``PMIX_ALLOC_ACTIVATE`` to
``pmix_alloc_directive_t`` (``pmix_common.h``, next unused value after
``PMIX_ALLOC_REQ_CANCEL``), naming its nodes with ``PMIX_HOST`` and/or
``PMIX_HOSTFILE`` - the attributes that already mean "these hosts" and
"this hostfile" everywhere else - and parsed **identically** to the command
line, so a request and a ``--activate`` spec cannot drift apart.  That
means factoring the resolver out of ``ras_base_activate_hosts`` rather than
writing a second one; note the command line folds the hostfile into its own
list as ``file=<path>`` while the PMIx form carries it as a separate
attribute, so the shared entry point wants a host-syntax string and a
hostfile path as two arguments.  On PRRTE's side the directive is another
arm in ``prte_ras_base_complete_request()``/``ras/hosts``' ``modify()``
beside ``NEW``/``EXTEND``/``RELEASE``, and - as with the command line - it
must **not** be gated on ``prte_ras_base.scheduler_owned``, since it can
name nothing the scheduler has not already granted.

The second half is more general than activation and is worth having on its
own: **there is no way to put an allocation directive into a**
``PMIx_Spawn`` **call.**  Today a caller that needs resources before it can
launch has to issue ``PMIx_Allocation_request`` itself, wait for it, and
then spawn - the library equivalent of running ``salloc`` before ``srun``.
An attribute carrying an allocation request on the spawn would let the host
do what ``srun`` does when invoked outside an allocation: obtain the
allocation per the directive, wait for it to complete, and only then
execute the spawn.  PRRTE already has the mechanism this needs - the
add-host path marks the DVM not-ready, parks the job in ``prte_cache``, and
lets the grow's ``VM_READY`` re-entry release it (``prte_ras_base_add_hosts``,
``plm_base_receive.c``) - so what is missing is the attribute and the
routing, not the machinery.  Getting the failure semantics right is the
part that needs thought: an allocation request that is refused, or that
times out, has to fail the spawn with something the caller can tell apart
from a launch failure.

**Mixed allocators are not supported, and would need more than the ``ras``
framework.**  The motivating case is a cloud/local combination: an allocation
from a scheduler plus a set of unmanaged nodes outside it.  The ``ras``
framework was made multi-select for this, and it never delivered it -
``prte_ras_base_allocate`` breaks at the first module that succeeds, so there
is no union step and the second allocator never contributed a node.  That
selection is now single, which loses nothing that worked.  Supporting the case
properly is a larger piece of work than re-admitting multiple ``ras`` modules:
``plm`` is not multi-select either, so nothing tracks which launcher owns
which nodes, and the daemon-launch path would have to fan out through more
than one.  At minimum it needs a launcher affinity recorded per node and
honored by ``prte_plm_base_setup_virtual_machine``.

**RELM has one module and no way to choose another.**
``prte_relm_register`` registers the base implementation unconditionally
(``src/rml/relm/relm.c``); the MCA variable that would select between modules
does not exist.  This is only worth doing if a second module does.

**The post-fork child still calls into hwloc once.**  ``odls`` was converted
to async-signal-safe operations between ``fork`` and ``exec`` except for
``hwloc_set_membind``, which allocates internally; replacing it with a bare
``set_mempolicy``/``mbind`` means reproducing hwloc's NUMA nodeset handling
and was left for later (``src/mca/odls/AGENTS.md``).

**A hostfile line with more than one ``@`` stops the parse without saying
where.**  ``hostfile_parse_line`` splits the value on ``@`` and takes one
field as a hostname and two as ``user@host``; anything else prints
``WARNING: Unhandled user@host-combination`` through ``pmix_output`` and
returns ``PRTE_ERROR``, which abandons the rest of the file (two sites in
``src/util/hostfile/hostfile.c``, one per token class that can carry a
name).  Every other parse failure in that file goes through
``hostfile_parse_error`` and ``help-hostfile.txt``, which name the file and
the line number.  This one names neither, and it is the failure a user is
most likely to reach by typo.

**A peer whose socket cannot be created keeps its queued messages.**  In
``prte_oob_tcp_peer_try_connect`` (``src/rml/oob/oob_tcp_connection.c``), a
failed ``tcp_peer_create_socket`` activates ``PRTE_JOB_STATE_COMM_FAILED``,
which is right — the failure spans every interface, so there is no other
address to try.  What the note in place also asks for is that the peer's
queued messages be marked and returned as unreachable, and they are not.
This is a reconnect path as well as a first-connect one, so what is queued
can be real work rather than a handshake.

**Nothing records who is "connected".**  ``pmix_server_connect_fn`` and
``pmix_server_disconnect_fn`` (``src/prted/pmix/pmix_server_dyn.c``)
implement both operations as a fence across the participants, which is
enough to make them return.  The bookkeeping is what is missing: the set of
processes a ``PMIx_Connect`` joined is recorded nowhere, so when one of them
terminates or fails there is nothing to consult for who was promised a
notification.

**The bootstrap configuration parses two options it deliberately does not
publish.**  ``SessionTmpDir`` and the ``Log*`` options are read into the
bootstrap configuration and then left there
(``prte_ess_base_bootstrap_params``, ``src/mca/ess/base/ess_base_bootstrap.c``):
the facilities they would drive — a dedicated session-directory override, and
DVM state logging — do not exist, so publishing them as MCA envars would
promise behavior the daemon does not have.  The parse stays because the file
format is the specification; the plumbing waits on the facilities.

**A job cannot ask for a transport.**  The network allocation request the
odls builds for each job names ``<nspace>.net`` and a security key and
otherwise takes whatever transport the resource manager offers by default
(``prte_odls_base_default_get_add_procs_data``,
``src/mca/odls/base/odls_base_default_fns.c``).  There is no command-line
surface for saying which one, and the note there is the record that one was
always intended.

**Stack traces assume ``siginfo_t``.**  ``show_stackframe``
(``src/util/stacktrace.c``) is installed as an ``sa_sigaction`` handler and
prints ``si_code``, ``si_addr`` and their neighbors unconditionally.
``configure`` probes for two *members* (``si_fd``, ``si_band``) but nothing
probes for the structure itself, so a platform without it would fail to
build rather than degrade to the plain handler.  Every platform PRRTE
currently supports has it, which is why this has never been forced.

Test coverage
-------------

**Init and finalize.**  Nothing covers the ``prte_init``/``prte_finalize``
sequence itself beyond a smoke test, and nothing configures with
``--disable-per-user-config-files``, so the ``#else`` arm of
``PRTE_WANT_HOME_CONFIG_FILES`` is never compiled in CI
(``src/runtime/AGENTS.md``).

**Heterogeneous DVMs.**  The byte-order helpers (``prte_hton64`` /
``prte_ntoh64``) are exercised by every inter-daemon message, but the case
they exist for — two daemons that disagree about endianness — cannot be built
from a container swarm on one host.  No test can currently fail if they are
wrong.

**The resource managers nobody has.**  ``ras``/``plm``/``ess`` components for
PBS, LSF, gridengine, Flux and PALS are compile-only in CI
(``--enable-testbuild-launchers``, and for the ones needing third-party
headers, against declaration-only stubs).  A build proves they still compile;
only a real allocation on such a system proves anything else.  SLURM is the
exception — ``contrib/slurmswarm`` runs a real one.

**macOS.**  ``contrib/dockerswarm/run-tests.sh macos`` is a single-host
subset by construction.  Everything multi-node on that platform is untested.

**``--enable-mca-dso`` beyond one node.**  Building every component as a
run-time loadable DSO instead of linking it into ``libprrte`` is now built,
unit-tested and smoke-launched on Linux by the ``ubuntuMcaDso`` job in
``.github/workflows/builds.yaml``, and the same build was verified by hand
on macOS (26 components, ``make check`` clean, ``prterun -n 2 hostname``).
That is a single node, so it proves the components load and that the ones a
local launch needs work; it does not exercise a component only reached
across daemons.  ``contrib/dockerswarm`` can now be built that way —
``PRTE_SWARM_MCA_DSO=1 ./build.sh``, after which the ordinary suite is the
test — but the arm is **opt-in**, so what is still missing is anyone
running it as a matter of course.  Note that the CI job asserts on a
*named* component being loadable rather than on a count: the default
build already installs a DSO or two of its own, so a count cannot tell
the two configurations apart, and a silent fallback to static linking
would otherwise leave every step passing while testing nothing.

**``SLURM_TASKS_PER_NODE`` in its single-node spelling.**  The suite asserts
the ``2(xN)`` form that a multi-node allocation produces; the ``2(x1)`` form
goes through the same parser and is not separately covered.

**A revived daemon, and RELM's depth-stamped link updates.**  The unheal
path (``docs/plans/bootstrap/unheal_plan.rst``) is implemented and
harness-verified for the routing recompute, for the xcast op-id stream a
late joiner rejoins, and for the nidmap span the handoff encodes.  One item
in it is argued rather than tested: RELM stamps link updates with the tree
depth and ``update_link`` drops one whose depth does not match, while a
revival changes depths and rides the xcast forward-first.  The static
argument is that every daemon recomputes synchronously right after
forwarding, so both ends have settled before any link update — a later,
separate message — is processed.  The case that would settle it is cheap:
kill an interior node on the Docker harness, restart it, then launch a job
across the whole DVM and check that nothing was lost.

**The topology sensing path, and a cpuset wide enough to fill the buffer.**
``prte_hwloc_base_get_topology()``'s sensing arm reads the real machine, so
nothing drives it — every unit test and the offline harness hand it a
topology file instead.  Nor is ``prte_hwloc_print()`` exercised against a
machine of a few thousand PUs, which is the width its cpuset buffer is sized
for (``src/hwloc/AGENTS.md``).

**The XML arm, and the copy constructors.**  In
``src/runtime/data_type_support/``, the print functions' XML output has no
test, and ``prte_job_copy`` / ``prte_proc_copy`` have no callers to be tested
through.

Every marker in the code
------------------------

The list above is meant to be the whole of it: a ``TODO``, ``FIXME`` or
``XXX`` left in PRRTE's own sources should have an entry here in prose — and
so should an arm deliberately left empty, which carries no keyword at all —
and this table is what makes that checkable.  If you leave a marker, add the
entry; if you find a marker this table does not name, either the entry or
the marker is stale.

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - marker
     - entry above
   * - ``util/hostfile/hostfile.c``, ``hostfile_parse_line`` (two sites)
     - a hostfile line with more than one ``@``
   * - ``util/stacktrace.c``, ``show_stackframe``
     - stack traces assume ``siginfo_t``
   * - ``rml/oob/oob_tcp_connection.c``,
       ``prte_oob_tcp_peer_try_connect``
     - a peer whose socket cannot be created
   * - ``rml/oob/oob_tcp_connection.c``,
       ``prte_oob_tcp_peer_recv_connect_ack``
     - a TCP peer closed rather than retried at its next address
   * - ``rml/routed_radix.c``, ``prte_rml_compute_routing_tree``
     - routing-tree state is not preserved across a DVM resize
   * - ``rml/relm/relm.c``, ``prte_relm_register``
     - RELM has one module and no way to choose another
   * - ``mca/filem/raw/filem_raw_module.c``, ``raw_fault_handler``
     - ``filem/raw`` does not survive losing a daemon mid-staging
   * - ``mca/odls/base/odls_base_bind.c``, ``prte_odls_base_set``
     - the post-fork child still calls into hwloc once
   * - ``mca/odls/base/odls_base_default_fns.c``,
       ``prte_odls_base_default_get_add_procs_data``
     - a job cannot ask for a transport
   * - ``mca/ess/base/ess_base_bootstrap.c``,
       ``prte_ess_base_bootstrap_params``
     - two bootstrap options are parsed and not plumbed
   * - ``prted/pmix/pmix_server_dyn.c``, ``pmix_server_disconnect_fn``
     - nothing records who is "connected"
   * - ``mca/ras/flux/ras_flux_module.c``, ``modify``
     - ``ras/flux`` has no ``modify()``

Two families of marker are **not** ours, and are deliberately absent from
that table: the ``TODO`` comments in ``hostfile_lex.c`` and
``rmaps_rank_file_lex.c``, which come from flex's generated skeleton, and the
``FIXME`` comments throughout ``config/libtool.m4`` and ``config/ltmain.sh``,
which are vendored Autotools.

Review status
-------------

PRRTE's source has been going through a file-by-file review pass: read the
code, verify each finding adversarially, fix what survives, and write what
the review established into the directory's ``AGENTS.md``.  This section
records where that pass has reached, because "has anyone actually looked at
this?" should be a question with an answer, and because a review is only as
good as the code it was a review *of* — several subsystems have been rebuilt
since theirs.

The churn figures are commits and diff lines touching the subsystem's
sources since its own review commits, measured on **2026-08-17**.  They age;
regenerate them with ``git log --since=<date> -- <dir>`` rather than trusting
them a month from now.  ``AGENTS.md`` and ``CLAUDE.md`` are excluded from the
counts, since a review's own write-up would otherwise count as change since
the review.

Reviewed, and stable since
~~~~~~~~~~~~~~~~~~~~~~~~~~

What each of these directories' ``AGENTS.md`` says is still what the code
does.

.. list-table::
   :header-rows: 1
   :widths: 26 24 50

   * - subsystem
     - last reviewed
     - since
   * - ``src/event``
     - 2026-07-31
     - nothing
   * - ``src/include``
     - 2026-08-02 (round 2)
     - nothing
   * - ``src/pmix``
     - 2026-07-30
     - 6 commits, ~110 lines
   * - ``src/mca/filem``
     - 2026-08-03 (round 2)
     - 3 commits, ~30 lines
   * - ``src/mca/ess``
     - 2026-08-03 (round 2)
     - 4 commits, ~120 lines
   * - ``src/mca/iof``
     - 2026-08-04 (round 2)
     - 6 commits, ~150 lines
   * - ``src/tools``
     - 2026-07-29
     - 11 commits, ~120 lines

Reviewed, but changed materially since
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These carry a review, and then the ground moved.  The review's findings are
still worth reading — they say what the invariants are — but nothing here has
been re-verified against the code as it now stands, and the top of the list
is not a marginal case.

.. list-table::
   :header-rows: 1
   :widths: 22 20 58

   * - subsystem
     - last reviewed
     - since
   * - ``src/grpcomm``
     - 2026-08-02 (round 3)
     - The subsystem was then taken out of MCA and rebuilt at a new path
       (``src/mca/grpcomm`` became ``src/grpcomm``), collective movements
       were added and most of them removed again, and the xcast was
       reworked twice.  Every line is now at an address the review never
       saw.  **Re-review this one first.**
   * - ``src/prted``
     - 2026-07-30
     - 43 commits, +3,187/-482.  The PMIx server host module
       (``src/prted/pmix``) absorbed most of the launch-message work, the
       lazy proc-data derivation, and the tool-connection data server.
   * - ``src/mca/ras``
     - 2026-07-26
     - 44 commits, +2,093/-494.  Elastic extend/release, the SLURM
       ``--json`` parser and its version gate, node reservation.
   * - ``src/mca/rmaps``
     - 2026-07-28
     - 28 commits, +2,408/-480.  Per-device mapping is nearly all of it,
       including a new enumerator vtable in the base.
   * - ``src/runtime``
     - 2026-07-29
     - 30 commits, +1,792/-635.  Placement packed as maps, the cpuset
       scatter, the mode byte at the head of the job buffer, and the data
       server's move onto a PMIx tool connection.
   * - ``src/mca/schizo``
     - 2026-07-28
     - 30 commits, +1,535/-522.  Device-mapping qualifiers and the
       debugger options.
   * - ``src/rml``
     - 2026-07-29
     - 20 commits, +1,522/-190.  The OOB wire header lost its namespaces,
       per-peer worker bases arrived, and RELM gained demotion handling
       (RELM was reviewed as part of this pass, not separately).
   * - ``src/util``
     - 2026-07-29
     - 22 commits, +889/-96
   * - ``src/mca/odls``
     - 2026-08-04 (round 2)
     - 8 commits, +543/-289.  The receiving half of the cpuset scatter.
   * - ``src/hwloc``
     - 2026-08-01 (round 3)
     - 11 commits, +536/-437.  Device enumeration and the UUID plumbing.
   * - ``src/mca/state``
     - 2026-07-29
     - 17 commits, +424/-120
   * - ``src/mca/errmgr``
     - 2026-08-02 (round 2)
     - 15 commits, +301/-217
   * - ``src/mca/plm``
     - 2026-08-04 (round 2)
     - 13 commits, +279/-147

Not yet reviewed
~~~~~~~~~~~~~~~~

Every directory below has an ``AGENTS.md`` — the orientation guides were
written across the tree in one pass — but an orientation guide is a
description, not a review.  Nobody has read these for defects.

**``src/mca/common``** (with ``common/slurm``).  Written 2026-08-06 and
untouched by any review.  It is now the single place every SLURM component
asks "is this really SLURM", so a wrong answer there is wrong in ``ras``,
``plm`` and ``ess`` at once.

**``src/mca/prtereachable``**, **``src/mca/prtebacktrace``**,
**``src/mca/prteinstalldirs``**.  Nothing but a header-installation fix and
the framework-version stamp has touched these since the pass began.  They are
small and quiet, which is the argument both for leaving them and for the fact
that a defect in them would have gone unnoticed.

**``test/unit`` and the harness scripts under ``contrib/``.**  Reviewed
informally as they were written; never subjected to the pass.  This is worth
saying out loud, because the harness is what decides whether everything else
passes.

Performance work: what the measurements settled
-----------------------------------------------

Nothing in this section is outstanding work.  What is here is the evidence
behind the message-size changes that have landed, and the traps that made
each of them expensive to get to — kept so the next person can extend the
work without re-deriving the ground, or re-proposing something already
measured and answered.

**Judge a header change on what a small message costs.**  ``prte_oob_tcp_hdr_t``
rides every RML message, and it used to carry the origin and the destination
as two ``pmix_proc_t`` — 552 bytes, of which 512 were two fixed 256-byte
nspace arrays holding the same short string.  On small messages that was most
of the traffic: a launch-message cpuset slice for an eight-process node is
101 bytes of payload, so the header was 85% of it.  It is now **30 bytes,
fixed**: two ranks and no namespace at all.

The namespace came out in two steps, and the second one is the useful lesson.
Collapsing the two ``pmix_proc_t`` to two ranks plus one length-prefixed
nspace took it to ~50 bytes and was easy, because the two names had never
differed.  Removing the last ~20 was not a matter of trimming further: what
stood in the way was that "both ends are always this daemon's namespace" was
an *invariant nobody enforced*, and the failure mode if it were ever broken is
a message delivered under the wrong sender identity rather than an error.
Two things settled that.  A **data server hosted by another DVM**
(``prte_pmix_server_uri``, historically ``ompi-server``) was the one feature
that ever wanted to address a foreign namespace, and it turned out never to
have worked over the RML — every send entry point has taken a rank in the
sender's own namespace since the 2022 rework, so the server's namespace was
silently discarded — so it now crosses on a PMIx **tool** connection instead
(:doc:`plans/cross_dvm_data_server/cross-dvm-data-server`).  And the invariant
became a **check**: only daemons open an OOB endpoint, the connect handshake
still carries a namespace, and ``tcp_peer_recv_connect_ack`` refuses a peer
whose namespace is not ours.  Verified once per connection is what lets every
message omit the field.

**Judge this kind of change on wire bytes, not on the raw message size.**
A launch-message size is usually quoted raw, which is the right unit for "how
much buffer does the master build and deflate" and the wrong one for "how
much crosses the network": the xcast compresses before it forwards, and a
field holding the same value in every record can compress away to nothing
while looking enormous raw.  Measured on a live 32-node broadcast at 8 ppn
(``grpcomm_base_verbose 1``, tag 18 is the launch message):

=====================  ===========  ============  =====
what                   raw          wire          ratio
=====================  ===========  ============  =====
whole message          3,790 B      1,280 B       0.34
per proc (slope)       13.0 B       --            --
cpuset (core vs none)  2.0 B/proc   0.77 B/proc   0.40
=====================  ===========  ============  =====

**The cpuset does not compress away, and more repetition does not help it.**
Its marginal wire cost held flat — 0.766, 0.742, 0.797 B/proc — across 8, 16
and 32 nodes, i.e. a fourfold increase in how often the same handful of
cpuset strings recur.  The overall ratio improves with scale (0.42 -> 0.34)
only because the fixed job-level part amortizes.  So ~0.4 is the marginal
ratio to extrapolate with, and it is what said the scatter was worth
building before it was built.

**Do not model this instead of measuring it.**  A synthetic model of the
per-proc region — the right fields, the right varint encoding, a faithful
cpuset distribution — predicted the region would deflate ~180:1 and the
cpuset would cost 0.05 B/proc on the wire.  The real answer is 0.77, a factor
of fifteen out, and the conclusion drawn from the model ("the compressor
already solves this, drop the idea") was the opposite of the one the
measurement supports.  The model is not salvageable by refining it; the
per-proc region does not exist in isolation on the wire.

**Two things settled along the way, so they need not be re-derived:**

* *The state is not worth scattering.*  Nothing in the tree ever sets a proc
  state to ``PRTE_PROC_STATE_RESTART`` — the only occurrence is
  ``prte_pmix_convert_pstate()`` translating an incoming PMIx state — and
  nothing sets ``PRTE_JOB_FLAG_RESTARTED`` either.  Both consumers
  (``odls_base_default_fns.c`` and ``filem_raw_module.c``) treat ``INIT`` and
  ``RESTART`` identically, so the wire never needs to tell them apart.  It is
  ~2 B/proc raw and varint-squashed, and it genuinely varies for the *job
  catchup* caller, which packs already-running jobs whose proc states feed
  ``PMIX_QUERY_PROC_TABLE``.  Leave it alone.  The **node rank** is likewise
  staying in the broadcast: a remote node-rank get that falls through to
  PMIx's one-job-per-node assumption returns a *wrong* value rather than
  nothing.
* *An optional trailing field per proc does not work.*  ``prte_proc_pack``
  runs in a loop and more job fields follow the array, so an absent flag is
  ambiguous both against the next proc's first field and against
  ``stdin_target``; ``PMIX_ERR_UNPACK_PAST_END`` never fires because there is
  always more buffer.  Any conditional field needs a discriminator the
  decoder has already read — which is why the shape is now declared by a
  mode byte at the head of the buffer.

**Measuring the launch message at all takes three settings**, and each one
silently gives a wrong answer if omitted: ``--prtemca hwloc_use_topo_file``
with a NUMA-bearing topology (a dev box and the swarm containers have no NUMA
node, so the default ``NUMA:IF-SUPPORTED`` binds nothing and you measure the
unbound shape while believing otherwise); a PMIx built *without* debug (see
below); and, for a wire number, a payload above the compressor's
``pcompress_base_limit`` — below it ``wire`` simply equals ``raw`` and the
census tells you nothing about the ratio.

**Small effects cannot be measured end-to-end on the container swarm.**  At
40 nodes the collect fence's wall clock has a run-to-run coefficient of
variation of 35–50%, so anything under about a third is not resolvable there
however many arms the sweep has — an attempt to settle whether compressing
the xcast payload helps (an effect of 1–7%) produced overlapping ranges whose
*sign* flipped between radices.  Measure the mechanism directly instead
(``--prtemca grpcomm_base_verbose 1`` reports the size, ratio and
microseconds of every broadcast).  See ``contrib/dockerswarm/AGENTS.md``,
§18.

Decided against
---------------

Work that was looked into and deliberately not done.  These entries are here
so that the next person does not rediscover the idea and spend the effort
again: each says what was measured and what the measurement decided.  Moving
one back up the page takes new evidence, not a fresh reading of the same
facts.

**Making the routing-tree repair the one place that raises
``COMM_FAILED``.**  ``prte_rml_repair_routing_tree`` ends by calling every
subsystem's ``fault_handler`` in turn, and a marker there asked whether that
one place should also set the departed ranks to
``PRTE_PROC_STATE_COMM_FAILED`` rather than leaving each caller to do it.  It
should not.  Repairing the tree and telling the errmgr a daemon is gone are
different questions, and the code has both halves without the other:

* A DVM shrink repairs in one batch for the whole campaign and must **not**
  raise it — ``shrink_campaign_complete`` (``src/mca/ras/base/ras_base_allocate.c``)
  tears the targets out of the DVM itself, and the already-departed guard in
  ``errmgr/dvm`` exists precisely to swallow the comm-failure their real
  departure produces afterwards.
* During an ordered teardown ``prte_rml_route_lost`` returns without repairing
  anything, yet the OOB still raises ``COMM_FAILED`` — and that raise is what
  drives "all my routes and children are gone, terminate" in *both* errmgr
  components.  Centralizing the raise would break orderly shutdown.
* ``errmgr/prted`` heals the tree around a peer it has given up connecting to
  from inside its own error handler, where raising the state again would
  re-enter it.

Four of the six call sites therefore repair without reporting, for three
different reasons, and one report happens where no repair does.  What the
paths that *do* report must not do is diverge, because the routing tree's
``failed_dmns`` set is what they all use to tell a first report from a
duplicate — a path that repairs without reporting makes every later report
for that rank look like a duplicate and suppresses it for good.  They are
collected behind ``report_new_departures`` in
``src/rml/rml_fault_handler.c``; the two lineage-inference paths used to be
outside it, and are not any more.

**Aggregating the launch-message cpuset slices per next hop.**  The scatter
itself has landed: the launch message is packed ``PRTE_JOB_PACK_NO_CPUSETS``
and each daemon is sent the bindings of the procs it will fork, point to
point (``prte_odls_base_send_cpuset_slices``).  Measured with ``--rtos
donotlaunch`` at 1000 x 128 on a 176-core, 5-NUMA topology, that takes the
raw launch message from **1,622,647 to 602,647 bytes** — 8 B/proc, 63% of
what was left after the maps and the empty attribute list came out.

What was proposed on top of that was how those slices leave the master.  It
is one routed send per daemon: the *bytes* aggregate, because
``prte_rml_get_route`` sends each toward its target and the tree carries a
subtree's slices over one link, but the *messages* do not — a 1000-node job
posts 999 sends from the master on the launch path.  One message per next
hop, each carrying the slices for that child's whole subtree and split by the
relay, would make that O(radix).

Measured, that buys very little.  The payload is unchanged either way: a
slice crosses exactly the links between the master and its daemon whichever
way it is bundled.  What aggregation removes is one *message header* per
crossing it saves, and at the default radix of 64 the crossings (Σ depth over
the daemons) fall from 1,934 to 999 at a thousand nodes and from 25,773 to
9,999 at ten thousand.  With the header at 552 bytes — what it was when this
was measured — that is 0.52 MB saved at 1000 x 128 and 8.7 MB at 10,000 x
128, against a launch broadcast that moves ~240 MB and ~24 GB respectively.
The scatter that this entry follows saved ~98 MB at the first of those
shapes; this would save another half.

The master-side argument does not survive either.  The pre-broadcast pack
loop is real — 5.7 ms at 1000 x 128, 51 ms at 10,000 x 128, measured against
the swarm's PMIx — but it is ~22 ns per *pack call* and there are two per
process, so the cost is per process, not per buffer.  Aggregating packs the
same rank/cpuset pairs into fewer buffers and the loop takes just as long.

**So this shall not be done.**  It removes message headers, not payload, and
the headers it removes are two parts in a thousand of what the launch already
broadcasts — while adding a relay protocol on the launch path, with its own
failure handling, that has to work while the tree changes under an elastic
grow.  Fusing the slices into the xcast instead is worse, not better:
``grpcomm_xcast.c`` forwards one packed buffer shared by every child, and
says where it says so that a forward which must differ per child puts the
packing back inside the loop.

What the measurement did turn up is that the **header** was the overhead
worth attacking, and not only here: at 8 processes a node a slice message was
101 bytes of payload behind 552 bytes of header, and every RML message in the
system paid the same.  512 of those bytes were two fixed 256-byte nspace
arrays holding the same short string.  That is now fixed — the header carries
two ranks and one length-prefixed nspace, about 50 bytes on the wire — which
takes more off this traffic than aggregating it ever would have.

**Lazy proc-data registration: the withholding half.**  The *deriving*
half is in: ``dmodex_req`` answers a request for one of the placement keys
out of ``jdata->procs[rank]`` rather than going to the hosting daemon
(``prte_pmix_lazy_procdata``, on by default).  The *withholding* half is not,
and not by omission from the design — the commit that landed the derivation
does not touch ``pmix_server_register_fns.c`` at all.  That file's per-proc
loop still emits a ``PMIX_PROC_INFO_ARRAY`` for **every** proc in the job on
**every** daemon, a table that grows with the whole job on a node running a
fixed slice of it, and ``prte_hostname_cutoff`` is the record of that wall
having been met once already.  Only the two location keys are withheld, and
for a different reason (the cpuset scatter, in the section above).
Narrowing the rest is what the commit message claims and the code does not
do.

The rule it must keep to: switch on what PRRTE is the *authority* for (the
placement and binding it decided, a closed set), never on what an
application is expected to ask for.

What the derivation is worth as it stands is small, and worth knowing before
anyone spends effort on the other half.  Measured with
``contrib/dockerswarm/peerinfo.c`` — every rank asks every other rank in its
job where it is — at eight ranks over four nodes, with
``--prtemca prte_pmix_server_verbose 2``, the twenty-four peer lookups split
six answered by the derivation and eighteen by a wire round trip to the
hosting daemon; with ``prte_pmix_lazy_procdata 0`` it is zero and
twenty-four.  Every one of the six is on the master, and every one is for
``PMIX_RANK``.  That is the only key PMIx never has: it consumes the rank
entry as the array's identifier rather than storing it.  Everything else the
eager registration publishes is found locally and never reaches the
derivation, and the two keys it does not publish the derivation declines by
design (a daemon that does not fork the proc holds no cpuset, so it cannot
tell "unbound" from "not sent").  The master is the exception only because it
keeps every cpuset, so its own lookups are complete and ``PMIX_RANK`` is all
that is left to ask for.

The question this entry used to say had to be measured first — whether a
**partial** entry is usable, or whether PMIx treats a published array as the
complete answer for that rank — is answered, and the answer is that a partial
entry works.  ``pmix_server_get.c`` fetches per key; a rank present with
other keys but missing this one falls through to ``direct_modex``.  Those
eighteen wire answers above *are* that path in production: since the cpuset
scatter, a daemon publishes an entry for a remote proc with no
``PMIX_CPUSET`` in it, and the get is answered by the daemon that holds one.
The one constraint is the reverse case — for a proc the daemon **hosts**, a
missing reserved key returns ``PMIX_ERR_NOT_FOUND`` with no up-call at all,
so nothing may ever be withheld from a proc this daemon forks.

What is left to weigh is whether the withholding is worth having, because it
saves less than it appears to.  PRRTE registers ``PMIX_NODE_MAP`` and
``PMIX_PROC_MAP``, and PMIx's ``store_map`` already materializes
``PMIX_HOSTNAME``, ``PMIX_NODEID``, ``PMIX_LOCAL_RANK`` and
``PMIX_NODE_RANK`` per rank for the whole job out of them, on every daemon,
wherever the host has not spoken to that key itself.  Dropping the proc
arrays therefore does not remove the job-sized table; it removes
``PMIX_GLOBAL_RANK``, ``PMIX_APP_RANK``, ``PMIX_APPNUM``,
``PMIX_REINCARNATION``, and the three keys only the hosting daemon publishes
anyway.  It also hands the node rank to a derivation PMIx documents as
assuming this is the only job on the node, which on a shared node replaces a
right answer with a wrong one rather than with silence.

**So the withholding half shall not be done.**  It cannot remove the
job-sized per-rank table, because PMIx rebuilds most of that table from the
maps whether PRRTE publishes the proc arrays or not; what it can remove is a
handful of keys, and it pays for them with a node rank that is wrong instead
of absent wherever two jobs share a node.  The derivation already in the tree
stays — it is what lets the cpuset scatter be answered — but this entry is
closed, not deferred.  Reopen it only on a measurement showing that the table
PMIx actually builds is what hurts, and that is a PMIx question, not a PRRTE
one.
