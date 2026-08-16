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
the work.  The last section is the other outcome: work that was investigated
and decided against, kept so nobody spends the effort a second time.

Runtime behavior
----------------

**An elastic re-extend that reuses a previously-shrunk node was reported to
hang.**  The sequence is: allocate one node, extend by three, shrink two,
then extend by one *reusing* a node the shrink released.  The first extend
completes; the re-extend does not
(`#2491 <https://github.com/openpmix/prrte/issues/2491>`_ closed the routing
half of this, not this half).  The mechanism identified at the time: a grow
completes only through ``DAEMONS_REPORTED`` → ``VM_READY``, which fires when
``daemons->num_procs == daemons->num_reported``, and both counters are
monotonic — neither is decremented when a daemon departs.  A shrink therefore
leaves them balanced, so the first extend is fine, while a re-extend raises
the fence for a daemon that must report before the grow can complete.  The
dockerswarm suite covers grow → shrink → re-grow by *hostname* and passes;
what has never been re-verified against current master is the same shape
driven through ``ras/slurm``, where the reuse code lives.  A case belongs in
``contrib/slurmswarm/run-tests.sh`` either way — it is cheap, and its
absence is why this is still an open question rather than a closed one.

**``ras/flux`` has no ``modify()``.**  It returns ``PMIX_ERR_NOT_SUPPORTED``
(``src/mca/ras/flux/ras_flux_module.c``), so the elastic extend/release
surface exists for SLURM only.  Everything above the component is
RM-agnostic; what is missing is the Flux-side conversation.

**``PMIX_RANGE_CUSTOM`` denies everyone.**  The data server's ``check_range``
has no accessor-list implementation and falls through to ``PMIX_ERROR``
(``src/runtime/data_server/``).  Denying is the safe direction for an
unimplemented rule, and it is the reason a publisher cannot express "these
specific processes may read this".

**``filem/raw`` does not survive losing a daemon mid-staging.**  Its fault
handler is deliberately minimal (``src/mca/filem/raw/filem_raw_module.c``);
the note in place observes that the real thing should be straightforward,
since the transport it stages over (xcast) is already resilient.  Until then,
a daemon lost while files are in flight fails the staging rather than
re-driving it.

**A promoted daemon's failure notice does not carry its ancestor list.**  In
``src/rml/rml_fault_handler.c``, a daemon whose parent changed reports the
failures in its subtree upward, but not the ancestor chain it now believes
in — so the new parent cannot confirm that the child agrees with it about the
shape of the repaired tree.

**Nobody owns marking failed procs ``COMM_FAILED``.**  The recovery
sequence in ``src/rml/rml_fault_handler.c`` calls each subsystem's
``fault_handler`` in turn, and the marked question there is whether that
one place should also be what sets the affected procs to
``PRTE_PROC_STATE_COMM_FAILED``, instead of leaving each handler to do it.
It is a question about where the responsibility belongs, not a known
misbehavior.

**A RELM message-id clash is logged and ignored.**  Two places in
``src/rml/relm/base/state_updates.c`` detect that a message uid already
belongs to a different message — a second start for the same uid, or a
payload whose size disagrees with the one already held — log
``PRTE_ERR_OP_IN_PROGRESS`` and carry on.  What the right answer is (fail
the job, or something narrower) has never been decided.  The ids are
generated, so a clash means something upstream is already wrong.

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

**RELM has one module and no way to choose another.**
``prte_relm_register`` registers the base implementation unconditionally
(``src/rml/relm/relm.c``); the MCA variable that would select between modules
does not exist.  This is only worth doing if a second module does.

**The post-fork child still calls into hwloc once.**  ``odls`` was converted
to async-signal-safe operations between ``fork`` and ``exec`` except for
``hwloc_set_membind``, which allocates internally; replacing it with a bare
``set_mempolicy``/``mbind`` means reproducing hwloc's NUMA nodeset handling
and was left for later (``src/mca/odls/AGENTS.md``).

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

Performance work not landed
---------------------------

**Aggregate the launch-message cpuset slices per next hop.**  The scatter
itself has landed: the launch message is packed ``PRTE_JOB_PACK_NO_CPUSETS``
and each daemon is sent the bindings of the procs it will fork, point to
point (``prte_odls_base_send_cpuset_slices``).  Measured with ``--rtos
donotlaunch`` at 1000 x 128 on a 176-core, 5-NUMA topology, that takes the
raw launch message from **1,622,647 to 602,647 bytes** — 8 B/proc, 63% of
what was left after the maps and the empty attribute list came out.

What is left to do is how those slices leave the master.  Today it is one
routed send per daemon: the *bytes* aggregate, because
``prte_rml_get_route`` sends each toward its target and the tree carries a
subtree's slices over one link, but the *messages* do not — a 1000-node job
posts 1000 sends from the master on the launch path.  Sending one message
per next hop, each carrying the slices for that child's whole subtree, and
having the relay split it, makes that O(radix) per daemon instead.  It needs
a relay step the point-to-point form does not.

**Judge this kind of change on wire bytes, not on the raw message size.**
The figures above are raw, which is the right unit for "how much buffer does
the master build and deflate" and the wrong one for "how much crosses the
network": the xcast compresses before it forwards, and a field holding the
same value in every record can compress away to nothing while looking
enormous raw.  Measured on a live 32-node broadcast at 8 ppn
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
