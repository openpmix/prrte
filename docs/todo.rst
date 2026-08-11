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
the work.

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

**``SLURM_TASKS_PER_NODE`` in its single-node spelling.**  The suite asserts
the ``2(xN)`` form that a multi-node allocation produces; the ``2(x1)`` form
goes through the same parser and is not separately covered.

Performance work not landed
---------------------------

**Lazy proc-data registration.**
``prte_pmix_server_register_nspace`` publishes a PMIx proc-data entry for
*every* proc in the job on *every* daemon — a table that grows with the whole
job on a node running a fixed slice of it.  An experimental branch registers
only the procs a daemon hosts and derives the rest on demand in
``dmodex_req``.  It is **written but unproven**, and it is stacked on the
xcast-shared-payload work rather than on a clean master, so it needs a rebase
before it can even be measured.  The rule it must keep to: switch on what
PRRTE is the *authority* for (the placement and binding it decided, a closed
set), never on what an application is expected to ask for.

**Small effects cannot be measured end-to-end on the container swarm.**  At
40 nodes the collect fence's wall clock has a run-to-run coefficient of
variation of 35–50%, so anything under about a third is not resolvable there
however many arms the sweep has — an attempt to settle whether compressing
the xcast payload helps (an effect of 1–7%) produced overlapping ranges whose
*sign* flipped between radices.  Measure the mechanism directly instead
(``--prtemca grpcomm_base_verbose 1`` reports the size, ratio and
microseconds of every broadcast).  See ``contrib/dockerswarm/AGENTS.md``,
§18.

