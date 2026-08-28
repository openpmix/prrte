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

**A daemon is never told that an application has terminated.**  The data
store honors ``PMIX_PERSIST_APP`` at the application it names — the master
counts each app's terminations and purges when the last one ends
(``prte_state_base_purge_app``) — but only for its own store.  A
``PMIX_RANGE_LOCAL`` publish lives in the store of the *daemon* that relayed
it, and no message tells that daemon an application is over; it learns of a
process exiting (it reaps the child) and of a namespace ending
(``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD``), and nothing in between.  So a local-range
item published with an explicit ``APP`` persistence is held until its
namespace ends, which is later than asked for but never shorter.

Deferred rather than designed around: it needs a new notification, it affects
only local-range publishes that name ``APP`` explicitly — not the default,
which is ``PMIX_PERSIST_NSPACE`` — and no reported problem depends on it.  See
``docs/plans/datastore/`` for the specification and the design the rest of
that work follows.

**``ras/flux`` has no ``modify()``.**  It returns ``PMIX_ERR_NOT_SUPPORTED``
(``src/mca/ras/flux/ras_flux_module.c``), so the elastic extend/release
surface exists for SLURM only.  Everything above the component is
RM-agnostic; what is missing is the Flux-side conversation.

**``ras_pmix_rank`` names nothing PMIx can act on.**  Every other
``ras_pmix_*`` connection parameter is the name of a PMIx attach attribute
and is now handed to ``PMIx_tool_attach_to_server`` (see
``src/mca/ras/pmix/``).  This one has no counterpart: ``PMIX_SERVER_RANK`` is
a ``PMIx_server_init`` attribute — the rank a server takes for *itself* — and
PMIx identifies an attach target by namespace and rendezvous, so a rank
selects nothing.  ``ras_pmix_server_host`` is a milder version of the same
thing: ``PMIX_SERVER_HOSTNAME`` is the attribute defined for what the
parameter means, but no PTL reads it, so it is forwarded to the server rather
than used to choose one.  Either PMIx grows a way to name a scheduler by
these, or the parameters should go; they are registered and reported by
``prte_info`` today, which is a promise neither can keep.

**``ras/pmix`` forwards allocation requests to a scheduler and never gives
anything back.**  The component (``src/mca/ras/pmix/``) exists to relay a
runtime ``PMIx_Allocation_request`` to a host PMIx server acting as the system
scheduler, and it does that — but three things around it are unfinished, and
they became visible only once ``ras`` selection went single-owner.

Its ``allocate()`` returns ``PRTE_ERR_TAKE_NEXT_OPTION``: it discovers no
initial allocation at all.  While every component that answered the query was
kept, the next one down did that job.  Now the component that answers *is* the
allocator, so pointing a DVM at a PMIx scheduler — a ``ras_pmix_uri``, an
``ras_pmix_server_host``, ``ras_pmix_system_scheduler`` — makes this the
allocator and leaves the base falling through to its one-slot local-node
fabrication, with any ``--hostfile`` unread.  Asking the scheduler for the
DVM's own allocation is the missing piece; until it exists, this component is
only usable on a DVM whose initial nodes came from somewhere the user does not
mind being ignored.

It declares ``scheduler_owned = true`` and implements neither
``release_allocation`` nor ``shrink_complete`` — the two module hooks the
framework offers precisely so an RM learns when a reservation is torn down or
a shrink has drained.  ``ras/slurm`` implements both.  So nodes a PMIx
scheduler grants are never handed back to it, and stay charged to the DVM for
its lifetime.

And a request the scheduler *grants* but PRRTE then fails to apply locally is
not compensated: ``passthru`` reports the local failure to the requester and
nothing issues the ``PMIX_ALLOC_RELEASE`` that would return the nodes.  The
self-inflicted version of this is fixed — the answer is now merged into the
request rather than substituted for it, so the routing directives the local
completion needs are still there — but a genuinely malformed request still
reaches the scheduler before it is refused here.

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

**``register_nspace()`` checks the list it is building, not the entries it
puts in it.**  ``prte_pmix_server_register_nspace``
(``src/prted/pmix/pmix_server_register_fns.c``) makes roughly forty
``PMIX_INFO_LIST_ADD`` calls and reads the status of three of them.  The
review checked the two that decide whether the registration is coherent at
all — that the list handle exists, and that the final conversion into the
array handed to ``PMIx_server_register_nspace`` succeeded — and left the
rest, on the argument that ``PMIx_Info_list_add`` fails only on a NULL list
or out of memory, and that forty checks would treble the length of an
already very long function to report a condition under which the daemon is
failing everywhere at once.  The consequence if that argument is wrong is a
job registered with a key silently missing.  What would settle it properly
is not forty checks but an accumulator — one status the adds fold into,
tested once — which is a change to the macro, not to this file.

**A client registration that fails is logged and the launch continues.**
``register_nspace()`` calls ``PMIx_server_register_client`` for each proc it
is about to host and, on an error, logs it and carries on.  The proc's
``PMIx_Init`` will then be refused by our own PMIx server, which the
application sees as an obscure failure some way downstream rather than as
the launch failure it is.  Failing the whole job on it would be the honest
alternative and is not obviously right either — the observed failure modes
are out-of-memory — so it is recorded rather than changed.

**``PRTE_JOB_NSPACE_REGISTERED`` is written and never read.**  Both
registration paths in ``pmix_server_register_fns.c`` set it, and nothing in
the tree consults it.  Either something should — the obvious candidate is
the wildcard direct-modex arm of ``dmodex_req``, which re-registers a
namespace it may already have registered — or the attribute should go, along
with its entry in ``src/util/attr.h``.  Leaving it is the third option and
is what the code does today.

**A session control addressed to every session answers with no session id.**
``apply_to_all`` (``src/prted/pmix/pmix_server_session.c``) applies the
operation to each session the requestor controls and then builds one answer
carrying the request's control id and nothing else.  It used to report
whichever session happened to be served last, which was worse; but the PMIx
description of a ``UINT32_MAX`` session id does not say what the answer
should carry, and an array of per-session results is the other plausible
reading.  Naming none of them is a choice, not a settled question.

**The bootstrap configuration parses one option it deliberately does not
publish.**  ``SessionTmpDir`` is read into the bootstrap configuration and
then left there (``prte_ess_base_bootstrap_params``,
``src/mca/ess/base/ess_base_bootstrap.c``): the facility it would drive — a
session-directory base for application jobs distinct from the DVM's own —
does not exist, so publishing it as an MCA envar would promise behavior the
daemon does not have.  The parse stays because the file format is the
specification; the plumbing waits on the facility.

What is missing is small but not free.  ``_setup_job_session_dir``
(``src/util/session_dir.c``) roots every job directory at
``prte_process_info.top_session_dir`` unconditionally, and no MCA parameter
reroots it — ``prte_tmpdir_base`` and its local/remote siblings move the
*whole* tree.  It is worth having: ``jdata->session_dir`` is what PRRTE hands
PMIx as ``PMIX_NSDIR`` (``pmix_server_register_fns.c``), which is where the
gds/shmem segments and an MPI's shared-memory backing files land, so
separating that from the DVM's own small control directory is a real
operational knob.  Two things have to be decided before writing it:

* **Uniqueness.**  Today it comes from the ``<prefix>.<host>.<pid>`` level
  above the job directories.  A bare ``<SessionTmpDir>/<jobid>`` collides
  between two DVMs on one node, so that level has to be replicated under the
  new root — and ``prte_job_session_dir_finalize`` then has two roots to
  destroy rather than one.
* **What the clients are told.**  ``PMIX_SERVER_TMPDIR`` must stay the
  daemon's own directory (it is where client rendezvous files go), so
  ``PMIX_NSDIR`` would no longer sit beneath it.
  ``docs/how-things-work/session_dirs.rst`` says "usually placed underneath",
  not "must be", so this is legal — but it is a change a client can observe.

The ``Log*`` half of this entry is **closed, not implemented**: the six
``ControllerLog*`` / ``PRTEDLog*`` keys have been removed from the file
format rather than plumbed.  The facility they would have driven now exists
as the ``state_base_log_jobstate`` / ``state_base_log_procstate`` /
``state_base_log_path`` MCA parameters
(``src/mca/state/base/state_base_log.c``), and that is the only way to ask
for it.  A configuration key applies to every daemon of every DVM the
cluster starts, for as long as the line is present and with nobody watching,
while the volume of a per-transition record scales with the processes
launched and lands on the same disk the session directories use — a knob
with that blast radius has to be asked for by the run that wants it.  An
older ``prte.conf`` carrying the keys still parses: they are unknown keys
now, and unknown keys are ignored.

**A fence cannot tell a post-release straggler from the next round.**  A
fence signature is only its participant list, so nothing on the
``PRTE_RML_TAG_FENCE`` wire says which *round* a contribution belongs to.
That is invisible in the normal flow — a daemon converges only once every
contribution it expects has arrived, so nothing can arrive afterwards — but
the controller also ends a fence early, on a ``PMIX_TIMEOUT`` and on a
participant lost to a failed daemon (``abort_fence_op()``,
``src/grpcomm/grpcomm_fence.c``).  A contribution still climbing the tree
then lands on a daemon that has already retired its tracker, and
``fence_recv()`` builds a fresh one for it.  The next fence over the same
participants finds that tracker, inherits its ``nreported`` and its bucket,
and can converge early carrying the previous round's data.

The group collective solves the same problem with ``completed_group_ops``, a
bounded memo of released operations that ``grp_recv`` consults, and that fix
**cannot** be copied here.  A group is keyed by ``groupID`` plus operation
and ``group()`` forgets the memo entry when a local client starts one; a
daemon relaying a fence for its subtree has no local client, so the memo
would never be forgotten there and the *next* fence's legitimate
contribution would be dropped — a hang in place of a wrong answer.  On a
pure relay the two are genuinely indistinguishable without a round
discriminator on the wire, and a fence has no originator to assign one: this
is the same "every participant must reach the same answer independently"
problem that the withdrawn lateral fence ran into
(``src/grpcomm/AGENTS.md``).

What would work is a per-signature release count: each daemon counts the
releases it has seen for a signature, stamps its contributions with that
count, and drops a contribution stamped below its own — the recovery epoch's
mechanism, scoped to a signature and driven by releases rather than by
failures.  That is a wire change plus a memo that holds a counter, and it
needs the dockerswarm harness to validate, so it has not been attempted.

**A connected job's teardown costs one DVM-wide broadcast per process.**
`PMIx_Connect` obliges the host to tell an assemblage about every member
that leaves without disconnecting first, and PRRTE keeps that promise in
``notify_assemblage()`` (``src/prted/pmix/pmix_server_connect.c``): one
``PMIX_ERR_PROC_TERM_WO_SYNC`` per departing proc, each broadcast to every
daemon in the DVM with a ``PMIX_EVENT_CUSTOM_RANGE`` naming the membership,
so that PMIx delivers it to exactly the members that registered for it.

Running to completion is not a disconnect, and a spawned child is connected
to its parent by default, so an ``MPI_Comm_spawn``\ ed job of N ranks ends by
issuing N broadcasts across the whole DVM.  Nothing pays for this unless an
assemblage exists — the registry is empty in the ordinary case and the check
is the first thing every one of those entry points makes — but where one
does exist the cost grows with the product of the job size and the DVM.

Coalescing them is not simply an optimization to write.  The event is per
proc by definition: a member is entitled to know *which* peer went, and the
exit code travels with it.  A batched event would have to carry a list and
every consumer would have to learn to read one, which is a PMIx interface
question rather than a PRRTE one.  What could be done here without touching
the definition is to stop broadcasting: the membership is held on the master
and the daemons hosting those members are derivable from it, so the
notification could be sent to those daemons rather than xcast to all of
them.  That trades a broadcast for a proc-to-daemon walk on the master, and
it needs the dockerswarm harness to show it delivers the same events.

**A job cannot ask for a transport.**  The network allocation request the
odls builds for each job names ``<nspace>.net`` and a security key and
otherwise takes whatever transport the resource manager offers by default
(``prte_odls_base_default_get_add_procs_data``,
``src/mca/odls/base/odls_base_default_fns.c``).  There is no command-line
surface for saying which one, and the note there is the record that one was
always intended.

**Event registration is accepted and discarded.**  PMIx offers the host a
pair of hooks — ``register_events``/``deregister_events`` — through which it
reports which status codes its local clients have asked to hear about, so
that a host can stop distributing the ones nobody wants.  PRRTE takes both
(``_register_events``/``_deregister_events``,
``src/prted/pmix/pmix_server_notify.c``), thread-shifts them, and answers
success without recording anything: every notification a daemon originates is
xcast to the whole DVM and every daemon hands it to its own PMIx server,
which filters it against its clients' registrations there.

That is correct — no event is lost, and none is delivered to a process that
did not ask — and it is why the arms have been empty for as long as they have
existed.  What it costs is a broadcast per event whether or not any process
in the DVM wants the code.  Using the registrations would mean each daemon
keeping the union of its clients' codes, propagating that set on change, and
consulting it before the xcast in ``_notify_event()`` — a DVM-wide replicated
set that has to be right under grow, shrink and daemon loss, which is
considerably more machinery than the broadcast it saves.  It has not been
judged worth it, and the empty arms are the record of the decision rather
than of an oversight.

**A launcher's fork/exec agent directive is read and dropped.**  A tool that
launches ``prte`` through PMIx may hand it launch directives, and the one
``prte()`` (``src/prted/prte.c``) ever looked for was
``PMIX_FORKEXEC_AGENT``: it did the ``PMIx_Get``, released the value, and
carried on, so the directive had no effect and nothing said so.  The ``Get``
has been removed, since keeping it made the code read as though the
directive were honored.

Honoring it is more than restoring the call.  The agent is used by whichever
daemon forks the process, and each daemon reads its own from its own MCA
state (``odls_base_exec_agent``, ``prte_odls_globals.exec_agent``) — so a
value learned by the HNP has to be propagated, either into the daemons'
environment at launch or as an attribute on the daemon job object that the
odls consults ahead of its MCA value.  There is already a per-job
``PRTE_JOB_EXEC_AGENT`` for the application job, which is the shape to
follow; what is absent is the DVM-wide equivalent and the decision about
which of the two should win.

**A tool forwards signals from the signal handler itself, which is not
async-signal-safe.**  Every forwardable signal is forwarded by default, and
both tool bodies take them with ``signal()`` and do the work in the handler:
``signal_forward_callback()`` in ``src/prted/prun_common.c`` calls
``PMIx_Job_control``, and its counterpart in ``src/prted/prte.c`` calls
``PMIx_Job_control_nb``.  Neither is async-signal-safe - both take locks,
allocate, and write to a socket - and neither is the ``fprintf`` beside
them.

Note what the non-blocking form did and did not fix.  Moving ``prte.c`` to
``PMIx_Job_control_nb`` removed a *different* deadlock: the blocking call
was made from the thread that drives ``prte_event_base``, and waited for a
completion only that thread could produce.  This one is still open, and it
is open in both files.  Neither PMIx nor PRRTE blocks signals in its
progress threads, so the kernel may deliver to any thread - including one
already inside PMIx holding the lock the handler is about to want.  The
window is real rather than theoretical: the main thread is inside PMIx for
the whole of ``PMIx_Spawn``, which covers mapping and launching the job.

The fix is not a smaller call.  It is to stop doing the work in the
handler: the handler should ``write()`` a byte to a self-pipe - which is
async-signal-safe and is already the pattern ``prte.c``'s
``abort_signal_callback()`` uses for ctrl-c, via ``term_pipe`` - and the
forwarding should happen on a thread that can safely make the call.  That
is easy in ``prte.c``, which drives an event base and can take a signal
event on it.  It is the harder half in ``prun_common.c``, whose main thread
spends the job parked in ``PRTE_PMIX_WAIT_THREAD``: it has no event base of
its own, so it needs either one or a thread whose job is to drain that
pipe.  Because the answer differs between the two files and changes how
every PRRTE tool takes a signal, it is recorded here rather than done
alongside an unrelated fix.

**Two resource-usage queries are recognized and answer nothing.**
``PMIX_QUERY_PROC_RESOURCE_USAGE`` and ``PMIX_QUERY_NODE_RESOURCE_USAGE``
have arms of their own in ``_query()``
(``src/prted/pmix/pmix_server_queries.c``) and both arms are empty.  PRRTE
collects no resource usage: the odls knows a child's pid and its exit code
and nothing about what it consumed while it ran, and a daemon samples
nothing about its node beyond the topology it discovered at startup.

Having the arms rather than not having them changes what the caller is
told, and for the worse.  The default arm answers
``PMIX_ERR_NOT_SUPPORTED``, which is the truth; falling into an empty arm
that adds no result makes the query come back ``PMIX_ERR_NOT_FOUND``,
which says the DVM looked and found nothing.  Whoever implements these
should either fill them in or delete them, and until then the honest
reading of them is that they are placeholders for a sampling path that was
never built - one that would need a per-proc collector in the odls and a
node-level one in each daemon, plus a decision about how often either runs.

Test coverage
-------------

**Init and finalize.**  Nothing covers the ``prte_init``/``prte_finalize``
sequence itself beyond a smoke test, and nothing configures with
``--disable-per-user-config-files``, so the ``#else`` arm of
``PRTE_WANT_HOME_CONFIG_FILES`` is never compiled in CI
(``src/runtime/AGENTS.md``).

**``--disable-pretty-print-stacktrace``.**  Nothing in CI configures with it,
which is how the arm came to not build at all: with the feature off, two
statics in ``src/util/stacktrace.c`` were unreferenced and ``--enable-debug``
made that an error.  That is fixed, but the option is still compiled only
when somebody thinks to try it, and the ``contrib/platform`` files that set
it are not built either.  It is one more configuration in the same class as
the ``#else`` arm above: cheap to break, and nothing watching.

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

**Session control over every session, relayed from a non-master daemon.**
That combination is the one path through ``pmix_server_session.c`` that runs
``apply_to_session`` more than once per request, and it is what made a
use-after-free possible there: the answer for the second session was built
out of strings pointing into the info array the first answer had already
freed.  ``contrib/dockerswarm``'s ``test_session`` covers a relayed request
and covers operations on one named session, but never the two together, so
the fix is argued rather than demonstrated.  The case is cheap —
``examples/sessionctrl.c`` already takes a session id, so it needs a
``UINT32_MAX`` spelling, two sessions instantiated by the same requestor,
and the request issued from a node that is not the master.

**``prte_pmix_server_register_tool()`` past its early returns.**  The unit
test (``test_tool_registration``) pins the three decisions the function
makes before it needs a live PMIx server: the rank screen, the refusal of a
namespace PRRTE will not file, and the already-registered short circuit.
Everything after that — including the missing-command-line case, where PMIx
sent no ``PMIX_CMD_LINE`` because it could not read its own argv — ends in
``PMIx_server_register_nspace`` and so cannot be reached from
``test/unit/prted``, which initializes no server.  Nor does the harness
produce it: PMIx on Linux always finds ``/proc/self/cmdline``, so the tool
always sends one.  What would cover it is a client that attaches with
``PMIx_tool_init`` and composes its own connection info array.

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
   * - ``mca/odls/base/odls_base_default_fns.c``,
       ``prte_odls_base_default_get_add_procs_data``
     - a job cannot ask for a transport
   * - ``mca/ess/base/ess_base_bootstrap.c``,
       ``prte_ess_base_bootstrap_params``
     - one bootstrap option is parsed and not plumbed
   * - ``mca/ras/flux/ras_flux_module.c``, ``modify``
     - ``ras/flux`` has no ``modify()``
   * - ``prted/pmix/pmix_server_notify.c``, ``_register_events``,
       ``_deregister_events``
     - event registration is accepted and discarded
   * - ``prted/pmix/pmix_server_queries.c``, ``_query``,
       ``PMIX_QUERY_*_RESOURCE_USAGE``
     - two resource-usage queries are recognized and answer nothing

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
   * - ``src/grpcomm``
     - 2026-08-25 (round 4)
     - nothing
   * - ``src/prted/pmix``
     - 2026-08-25
     - nothing
   * - ``src/prted`` (excluding ``pmix/``)
     - 2026-08-25
     - nothing.  All four of ``prte.c``, ``prted_comm.c``,
       ``prte_app_parse.c`` and ``prun_common.c``; with the row above it,
       the directory is now reviewed end to end.
   * - ``src/mca/ras/base``
     - 2026-08-26
     - nothing.  The framework's own four files only — the driver, the
       node insert, the framework hooks and the selector.  The ``ras``
       *components* are not covered by this and stay in the table below.
   * - ``src/mca/ras/pmix``
     - 2026-08-26
     - nothing.  Both files.  Note that nothing automated exercises this
       component's one real path: there is no PMIx scheduler in any
       harness, so the review is a reading plus what ``make check`` can
       reach through the module vtable.
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
   * - ``src/mca/ras`` (components only; see ``base/`` above)
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

**Guarding the stack-trace handler against a platform with no
``siginfo_t``.**  ``show_stackframe`` (``src/util/stacktrace.c``) is
installed as an ``sa_sigaction`` handler and reads ``si_code``, ``si_addr``
and their neighbors unconditionally.  A ``FIXME`` carried over from the
code's Open MPI ancestry asked for a second, plain ``sa_handler`` arm for
"systems which don't have siginfo".  There are none.  ``siginfo_t``, the
``sa_sigaction`` member of ``struct sigaction`` and the ``SA_SIGINFO`` flag
are all mandatory in POSIX.1-2001 — not one of its option groups — and
PRRTE's floor sits far above that: C11, PMIx 7, hwloc 2.1, libevent 2.0, and
unguarded POSIX-2001 calls in every direction.  The comment dates from 2004,
when the Unixes it was written for were still in the field.

What the probes around it check is the part that genuinely is optional, and
that division is the right one: ``si_fd`` is not in POSIX at all (a
Solaris/Linux extension — macOS does not have it, as any build tree's
``prte_config.h`` shows), and ``si_band`` belongs to the obsolescent XSI
SIGPOLL option.  Both are probed, and both are guarded at their single use;
so is every ``si_code`` value the switch decodes, and so is ``SIGPOLL``
itself.  The only thing taken on faith is the structure.

A second handler would therefore be code no compiler anywhere would ever be
handed, living in a signal handler, where a mistake is untraceable.  Nor is
a ``configure`` probe for the structure worth having: a check that can only
ever answer yes is the same dead weight in a different file, and guessing at
what a platform that has never existed would want is not portability.  The
platform PRRTE cannot serve here already has its answer —
``--disable-pretty-print-stacktrace`` compiles the handler out entirely and
is used by several ``contrib/platform`` files.

Checking that claim was worth more than the probe would have been: the arm
did not build.  ``unable_to_print_msg`` and ``set_stacktrace_filename`` sat
outside the switch their only users live behind, so with the feature off
they were unreferenced and ``--enable-debug`` rejected the file.  Both now
live inside the switch, and the disabled build compiles warning-free, passes
``make check``, and launches.  That nobody had noticed is its own entry
under **Test coverage**.

So the ``FIXME`` is gone and nothing replaced it but a comment saying what is
assumed and why.  If a platform without ``siginfo_t`` ever turns up, it will
say so as a compile error naming the type in this one file, and the
disable option is right there.

**Giving RELM an MCA variable to choose between reliability modules.**  The
marker in ``prte_relm_register`` asked for the enum variable that would select
one, and the entry that carried it said the work was only worth doing if a
second module was.  None was: RELM shipped with a ``prte_relm_module_t`` of
four function pointers that only ever held the base module's, a ``base/``
subdirectory holding the only implementation, and a second bundle of nine
callbacks on the state machine that the base module's ``init()`` wired to its
own functions.  Two dispatch surfaces, one implementation, and nothing that
could pick anything.

The one candidate second module was a passthrough that degraded a reliable
send to an ordinary one — an escape hatch and an A/B lever, since the base
protocol walks the path three times per message (data down, ACK up, ACK-ACK
down) and rides paths as busy as every IOF chunk from a daemon to the HNP.
That is an on/off switch, not a choice between strategies, and it would hand a
user a way to silently lose the messages this layer exists not to lose.  A
genuinely different strategy — end-to-end acknowledgement, journaled delivery
— has no requirement behind it and no author.

So the indirection was removed rather than completed, the way ``routed`` and
``oob`` were folded into ``src/rml`` and ``grpcomm`` was de-framework'd before
it: ``base/`` is flattened into ``src/rml/relm/``, the protocol's entry points
are ordinary functions the engine calls directly, and ``relm.h`` is four
symbols wide.  The split that survives is structural — ``state_machine.c`` is
the generic engine, ``state_updates.c`` and ``link_updates.c`` are the
protocol on top of it — and it is a compile-time boundary rather than a
dispatch one.  Reintroducing dispatch is cheap against an interface that
narrow, so it can be done when a second implementation actually exists.

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
