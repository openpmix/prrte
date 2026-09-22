Running the multi-node suite on your cluster
============================================

PRRTE ships a multi-node test suite — around 650 assertions covering
launch, I/O forwarding, file staging, collectives, the data server,
elastic grow and shrink, routing relay, error reporting and teardown.
It was written for a harness of ten containers on one machine, but almost
nothing in it is about containers: the cases speak in *logical* node names
and reach those nodes through a small transport layer.

Point that transport at real machines and the same suite runs on your
cluster:

.. code-block:: sh

   shell$ cd contrib/dockerswarm
   shell$ ./cluster-setup.sh --prefix /path/to/your/prrte-install
   shell$ ./run-tests.sh cluster

This page is the full account of doing that.

The harness ships in the release tarball as well as in the git repository,
so ``contrib/dockerswarm`` is there whichever way you got PRRTE — you do
not need a clone to test the release you installed.

.. note:: This is the *only* supported way to run the suite outside the
          container harness. Do not copy cases out of ``run-tests.sh``
          into a script of your own — the transport, the cleanup between
          cases, and the gates that decide what your cluster can honestly
          prove all live in that file, and a case lifted out of it will
          either fail for reasons that are not PRRTE's or pass without
          testing anything.

Why bother, if the containers already pass
------------------------------------------

Because the container harness deliberately has no cluster in it. It runs
ten Linux containers on one kernel, one loopback-ish bridge network, one
file system image and whatever CPU the host has. That is exactly right for
catching runtime logic errors quickly, and it is silent about:

* **your interconnect** — a network where a connect can be refused, an
  address can be unroutable, and latency is not a memory copy;
* **your file system** — whether ``$HOME`` is shared, whether ``/tmp`` is
  node-local, how a preload behaves against a parallel file system;
* **your resource manager** — whether PRRTE's view of an allocation
  matches the scheduler's;
* **your hardware** — NUMA, SMT, GPUs, multiple fabric interfaces. The
  containers have none of it, and the mapping and binding code cares about
  all of it;
* **your scale** — ten nodes is ten nodes. Routing-tree behavior, xcast
  relay depth and collective cost all change with node count.

A green container run says the logic is right. A green run on your cluster
says PRRTE works *here*.

What the harness needs
----------------------

**At least two nodes.** This suite is about what happens *between*
daemons; on one host most of it cannot exist. Ten nodes runs everything;
fewer runs a subset and says which cases it declined (see
:ref:`fewer-nodes`).

**A PRRTE installation every node can reach**, at the same path, with
``prted`` findable from a *non-interactive* shell. The second half is the
part that catches people out: ``plm/ssh`` launches a daemon through a
non-login shell, which sources none of your dot files. Either put the
install's ``bin`` on the default ``PATH`` on every node, or — much easier
— configure PRRTE with:

.. code-block:: sh

   shell$ ./configure --prefix=/shared/prrte --enable-prte-prefix-by-default ...

which makes a remote daemon be launched by its full path. This is how the
container harness builds, for exactly this reason.

**Password-less ``ssh`` between the nodes**, from the head node to each of
the others. The harness drives the head node locally, so you do not need
to be able to ``ssh`` back to yourself.

**A C compiler on the machine you run the setup script from**, to build
the suite's helper clients (below).

**The nodes to yourself for the duration.** See :ref:`cluster-safety`.

The helper clients, and why the suite needs them
------------------------------------------------

Most of the suite does not run ``hostname``. It runs small, bare PMIx
programs that put the runtime into states no ordinary application reaches:
a group construct that loses a member half way through, a lookup that
parks in the data server until somebody publishes, a client that cycles
``PMIx_Init``/fence/``PMIx_Finalize`` hundreds of times, a rank that asks
every other rank where it is. Their sources live in ``contrib/dockerswarm``
(plus two from ``examples/``), and ``cluster-setup.sh`` compiles them into
your install's ``bin``:

.. code-block:: sh

   shell$ ./cluster-setup.sh --prefix /shared/prrte

That is all it does to your installation — it adds executables beside
``prun``. It never builds or installs PRRTE itself; build and install
PRRTE the ordinary way first (:doc:`/install`).

It then checks the cluster can host the suite — every node reachable and
carrying the install, the scratch directory node-local, and one real
``prterun`` across two nodes — and prints the exact environment to export
for the run. Run it with ``--check-only`` to re-check without rebuilding,
or ``--no-check`` to build the clients and stop.

.. note:: If your install prefix is read-only (a site installation you do
          not own), pass ``--bindir`` a directory you *do* own that is on
          every node's ``PATH``. The clients must be visible on every node
          the suite maps a process onto, not just the head node.

How the harness reaches your nodes
----------------------------------

Every case in the suite is written against **logical** node names:
``node1`` is the head node, where the tools run, and ``node2`` upwards are
the rest. Your machines are called something else, so the transport
rewrites in both directions — a command on its way out has each ``node<N>``
replaced by the real host, and the output on its way back has the real
names replaced by ``node<N>``.

That is why an assertion written years ago as ``grep -c node2`` still means
"ran on the second node" on a machine called ``nid001234``, and it is why
the 800-odd node references in the suite did not have to be parameterized
one at a time. The mapping is built during the preflight from three names
per node: the one you addressed it by, what ``hostname`` prints there, and
the fully qualified form.

Commands travel over ``ssh``, on its *standard input* rather than in its
argument list, which is the one arrangement with no quoting to get wrong.
If you are standing on the head node it is driven directly instead — a
cluster that lets you ``ssh`` out to the compute nodes does not always let
you ``ssh`` back to yourself. The preflight works out which case you are in
and says so; you can run the suite from a login node that is not in the
node list at all.

.. note:: The rewrite is a plain textual substitution, so a host name that
          is also an ordinary English word — a node literally called
          ``head``, ``all`` or ``test`` — will be replaced wherever it
          appears in a message, not only where it names a machine. Nothing
          breaks, but an assertion's diagnostic can read oddly. Cluster
          host names are rarely like that; if yours are, address the nodes
          by their fully qualified names in ``PRTE_CLUSTER_NODES``.

Configuration
-------------

Everything is an environment variable, and on most clusters you will set
two or three of them.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Variable
     - Meaning

   * - ``PRTE_CLUSTER_NODES``
     - Comma- or space-separated node list, **head node first**. Takes
       precedence over everything below.

   * - ``PRTE_CLUSTER_HOSTFILE``
     - ...or a file, one host per line. ``#`` comments and anything after
       the host name (``slots=`` and friends) are ignored.

   * - *(nothing)*
     - With neither of the above, the node list comes from the allocation
       you are standing in: ``SLURM_JOB_NODELIST`` (through ``scontrol show
       hostnames``), ``PBS_NODEFILE``, ``LSB_DJOB_HOSTFILE`` or
       ``LSB_HOSTS``.

   * - ``PRTE_CLUSTER_PREFIX``
     - The PRRTE installation to test. Defaults to the prefix of whatever
       ``prte`` is on your ``PATH``.

   * - ``PRTE_CLUSTER_PMIX_PREFIX``
     - The PMIx prefix, needed only for the capability-gated cases.
       Normally discovered by asking the loader which ``libpmix`` ``prted``
       actually resolves against — which is the right question when a
       packaged PMIx of the same soname is also installed.

   * - ``PRTE_CLUSTER_WORKDIR``
     - Per-node scratch directory. Default ``/tmp/prte-cluster-$USER``.
       **This must not be shared between nodes** — see
       :ref:`cluster-shared-fs`.

   * - ``PRTE_CLUSTER_RSH``
     - How to reach a node. Default
       ``ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new``.

   * - ``PRTE_CLUSTER_LOCAL_HEAD``
     - Whether the head node is this machine. Normally worked out for you
       by comparing this machine's name with the first node in the list,
       so running the suite from a login node against compute nodes just
       works. Set it to ``1`` or ``0`` to override.

   * - ``PRTE_CLUSTER_ENV_FILE``
     - A file sourced before every tool invocation — for a ``module load``,
       a site profile, or extra MCA settings.

   * - ``PRTE_CLUSTER_KEEP_RM_ENV``
     - ``1`` keeps the resource manager's environment. Off by default; see
       :ref:`cluster-scheduler`.

   * - ``PRTE_SWARM_WORKER_THREADS``
     - Size of PRRTE's worker thread pool for the whole run (default 8).
       Set it to ``0`` to put everything back on the main progress thread
       — the first thing to try when a failure smells like a race.

   * - ``TEST_ONLY``
     - Space-separated list of phase functions to run instead of the whole
       suite, for iterating on one of them. The preflight always runs.

A typical invocation:

.. code-block:: sh

   shell$ export PRTE_CLUSTER_PREFIX=/shared/prrte
   shell$ export PRTE_CLUSTER_NODES=cn01,cn02,cn03,cn04,cn05,cn06,cn07,cn08,cn09,cn10
   shell$ ./run-tests.sh cluster

.. _cluster-scheduler:

Running inside a scheduler allocation
-------------------------------------

This is the normal way to get nodes, and it is supported: grab an
allocation and run the suite in it.

.. code-block:: sh

   shell$ salloc -N 10 -t 2:00:00
   salloc$ cd /path/to/prrte/contrib/dockerswarm
   salloc$ ./run-tests.sh cluster          # the node list comes from the allocation

**By default the harness drops the scheduler's environment** — every
``SLURM_*``, ``PBS_*``, ``LSB_*``, ``LSF_*`` and ``FLUX_*`` variable — from
the shell each tool runs in, and this is deliberate. The suite names hosts
with ``--host`` and grows the DVM onto them; under a live allocation
``ras`` takes the allocation instead, a grow beyond it is correctly
refused, and the elastic phases then fail for a reason that has nothing to
do with the code under test. The nodes came *from* the allocation, so
nothing is being taken that was not granted — what changes is only that
PRRTE launches with ``ssh`` rather than with the scheduler's own launcher.

Set ``PRTE_CLUSTER_KEEP_RM_ENV=1`` to keep it, which is what you want when
the *scheduler integration* is the thing you are testing. PRRTE then
discovers the node pool through ``ras`` and launches its daemons the way
the scheduler expects, and everything that does not resize the DVM — launch,
mapping and binding, I/O forwarding, file staging, collectives, error
reporting, teardown — runs over that path. That is the end-to-end answer to
"does PRRTE work under *our* scheduler". Expect the cases that grow the DVM
past the allocation to be refused; the preflight says so up front rather
than letting you read the failures as regressions.

Two runs, one with the environment dropped and one with it kept, is the
thorough thing to do: the first exercises the runtime, the second exercises
the integration.

.. note:: Testing PRRTE's SLURM integration specifically — the ``salloc``
          grow path, the ``scontrol show job --json`` parser, ``scancel``
          on release — is what ``contrib/slurmswarm`` is for. It runs a
          real ``slurmctld`` in containers, so it can exercise a scheduler
          that can say *no* without an allocation of your own. See
          :doc:`containers`.

.. _fewer-nodes:

Fewer than ten nodes
--------------------

The suite addresses at most ten logical nodes and needs at least two. In
between, each phase is checked against the number of nodes it actually
names — read out of the phase's own source, so the gate stays right as
cases are added — and a phase that needs more nodes than you have is
**skipped with the reason**, not run against hosts that do not exist:

.. code-block:: text

   SKIP test_rml: needs 10 nodes, this run has 4

Most phases need four nodes or fewer, so a four-node allocation runs the
large majority of the suite. A handful of cases inside otherwise-small
phases reach further, and those are gated one at a time in the same way.

.. _cluster-shared-fs:

The scratch directory must not be shared
----------------------------------------

Several cases prove that a file crossed the wire **by its absence** on the
target node: the harness writes a file on node1 only, checks it is not on
node2 or node3, then runs a job there with ``--preload-files`` and requires
the ranks to read it.

On a cluster with a shared home directory that test cannot fail — it
becomes vacuous, which is worse than failing. So the preflight writes a
marker in ``PRTE_CLUSTER_WORKDIR`` on the head node and looks for it on the
second; if it is there, every file-staging case is skipped with the reason:

.. code-block:: text

   SKIP filem: --preload-files cross-node staging (data file only on node1):
        /home/you/scratch is shared, so staging cannot be proved

The default, ``/tmp/prte-cluster-$USER``, is node-local on nearly every
cluster, which is why it is the default. If yours mounts a shared
``/tmp``, point ``PRTE_CLUSTER_WORKDIR`` at something node-local — a local
SSD, ``/dev/shm``, a node-local scratch mount — and those cases come back.

Other things that can be skipped, and why
-----------------------------------------

A skip is never a pass. Each one names the condition that produced it, and
each condition is something you can change.

**Bootstrap DVM cases.** A bootstrapped DVM is configured through
``<sysconfdir>/prte.conf``. The harness has to write that file and every
node has to read the same one, so these cases need an install prefix you
own on a shared file system. A site installation you cannot write skips
them.

**Capability-gated cases.** Some cases assert behavior that requires a
PMIx new enough to define a particular capability flag. They are skipped
against an older PMIx rather than failed — but that gate can only answer
if the harness can find ``pmix_version.h``. If the preflight warns it
cannot, set ``PRTE_CLUSTER_PMIX_PREFIX``, or the gate answers "no" to
everything and a block of real coverage disappears quietly.

**Hardware-dependent cases.** A few mapping and binding cases stage a
synthetic topology onto the nodes so that hardware the machine lacks can
be reasoned about. Those run anywhere. Cases that ask about *your* actual
NUMA, SMT or device layout are the ones your cluster adds over the
containers, and they may legitimately behave differently here — which is
the point of running this at all.

.. _cluster-safety:

What the harness does to your nodes
-----------------------------------

.. warning:: **The suite needs these nodes to itself.** Between cases it
             kills every ``prte``, ``prted``, ``prterun``, ``prun`` and
             ``pterm`` **belonging to you** on every node in the list, and
             removes the session directories under ``/tmp`` that those
             tools leave behind. That is tidying up after itself, and it is
             also fatal to another job of yours sharing a node. It cannot
             touch another user's processes — the kills are scoped to your
             uid — but it will happily take down your own DVM in the next
             terminal.

The run prints this as a note during the preflight, before the first case.
If you are sharing nodes with your own work, get a separate allocation.

The sweep matters: it runs before the first case, not only between them.
A daemon or a ``pmix.*`` rendezvous file left behind by an earlier run
makes the *next* run fail in twenty places, none of which points at the
cause.

Reading the output
------------------

One line per assertion, and a count at the end:

.. code-block:: text

   === filem: --preload-files cross-node staging (data file only on node1) ===
     PASS  preload-files staged from node1 and read by both remote ranks
     PASS  the staged file is a real file in each node's working directory

   ================  612 passed, 0 failed, 47 skipped  ================

The exit status is zero only if nothing failed. Skips do not fail the run,
so **read the skip count**: a large one means your cluster declined to
prove a large part of the suite, and the reasons above say how to get it
back.

Iterating on one phase
----------------------

``TEST_ONLY`` runs only the phases you name. The preflight still runs —
the checks that say the install is reachable and the sweep that says the
nodes are clean are exactly the ones whose absence makes a subset run lie.

.. code-block:: sh

   shell$ TEST_ONLY="test_grpcomm test_rml" ./run-tests.sh cluster

The phase names are the ``test_*`` functions in ``run-tests.sh``.

Measuring rather than testing
-----------------------------

The suite answers *is it correct*. For *what does it cost* — collective
scaling, launch message size, routing radix — use
``contrib/scaling/cluster-sweep.sh``, which drives the same ``scaletest``
client across a real allocation and writes a CSV. Absolute numbers from
the container harness are not meaningful (ten containers share one kernel
and a handful of cores); numbers from your cluster are.

Troubleshooting
---------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Symptom
     - Cause

   * - ``no nodes: set PRTE_CLUSTER_NODES...``
     - No node list and no allocation. Set one of the variables above.

   * - ``node2 (cn02): no prted on PATH under <prefix>``
     - The install is not visible on that node, or not on a
       *non-interactive* shell's ``PATH``. Rebuild with
       ``--enable-prte-prefix-by-default``.

   * - ``cannot run a command there``
     - ``ssh`` to that node failed. Check password-less ``ssh`` from the
       head node, or set ``PRTE_CLUSTER_RSH``.

   * - Every launch case fails with ``Daemon exit status: 127``
     - Same as above: the remote shell could not find ``prted``.

   * - A wall of ``SKIP ... is shared, so staging cannot be proved``
     - ``PRTE_CLUSTER_WORKDIR`` is on a shared file system. See
       :ref:`cluster-shared-fs`.

   * - Elastic grow cases refused
     - You are inside an allocation with ``PRTE_CLUSTER_KEEP_RM_ENV=1``.
       See :ref:`cluster-scheduler`.

   * - Helper clients report a PMIx error that makes no sense
     - The clients loaded a different ``libpmix`` than the daemons.
       ``cluster-setup.sh`` links them with an rpath to the right one; if
       you built them by hand, do the same.

   * - A case fails only sometimes
     - Re-run it with ``PRTE_SWARM_WORKER_THREADS=0``. A failure that
       survives that is not a threading bug; one that disappears is.

   * - A diagnostic has odd words replaced by ``node<N>``
     - A host name that is also an ordinary word. Use fully qualified
       names in ``PRTE_CLUSTER_NODES``.

Where the cases live
--------------------

``contrib/dockerswarm/run-tests.sh`` holds them all, grouped into ``test_*``
phase functions named after the part of the tree they cover. Its companion
``contrib/dockerswarm/AGENTS.md`` is the maintainer's guide: what each
helper client is for, what each phase is asserting and why it cannot be
asserted on one host, and the traps that have cost people afternoons.

If a case fails on your cluster and passes in the containers, that is
worth reporting — it is very likely a real portability defect, and it is
exactly the kind this section exists to surface. File it at
https://github.com/openpmix/prrte/issues with the phase name, the
assertion text, and the output of ``prte_info --all``.
