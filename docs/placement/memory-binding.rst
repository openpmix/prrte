.. _placement-memory-binding-label:

Binding memory by attribute
===========================

On a machine with more than one kind of memory, e.g
DDR next to HBM, the NUMA node on which a process is cpu-bound to
is not always the node with the *best* memory for it. Example is
Sapphire Rapids (SPR) with DDR+CPU nodes and HBM-only nodes. On SPR,
the local DDR+CPU node is also the default and *closest* node for
memory allocation. HBM nodes hold no cpu cores, so are silently ignored by
automatic page allocation and migration. Exploiting HBM is usually done through
custom allocator libraries (e.g. ``memkind``), by explicit C API and system
calls (``hwloc``, ``mbind()``, ``set_mempolicy()``) or with command-line
binding tools like ``numactl``.

PRRTE can memory-bind each process to the NUMA node(s) with the best value
of a chosen (hwloc) memory attribute, among the nodes local to the cores the
process is bound to. This is separate from, and complementary to, cpu-binding
a process to cores (``--bind-to``). The cpu-binding defines where the process
runs, while the memory-binding decides where its memory is allocated.

.. note:: Memory attributes need **hwloc 2.3 or later**. PRRTE itself builds
   with hwloc 2.1 or later, but with an older hwloc this feature is
   unavailable: asking for an attribute is reported as a memory-binding
   failure (see `Failure handling`_).


Usage
-----

Per job or per app, with the ``--membind`` option of ``mpirun``,
``prterun`` and ``prun``:

.. code-block:: sh

   mpirun --membind attr:bandwidth -n 4 ./a.out
   mpirun --map-by numa --membind attr:bandwidth -n 4 ./a.out
   mpirun --bind-to core --membind attr:bandwidth -n 4 ./a.out

All three bind memory: the first relies on PRRTE's default cpu binding,
the second on the binding ``--map-by`` derives, the third on an explicit
one. What matters is that each process ends up bound to cpus: see
`The process must be bound to cpus`_.

Currently supported attributes are: ``bandwidth``, ``latency``,
``locality`` and ``capacity``.

As the default for every job a DVM runs, use the
``hwloc_default_mem_bind_attribute`` MCA parameter, which takes the same
values. It is a PRRTE parameter, so it is given with ``--prtemca`` or in
the environment as ``PRTE_MCA_hwloc_default_mem_bind_attribute`` |mdash|
``PRTE_MCA_``, not ``OMPI_MCA_``:

.. code-block:: sh

   prte --prtemca hwloc_default_mem_bind_attribute attr:bandwidth ...

A process is bound by its app's ``--membind`` if it has one, else by its
job's, else by the MCA parameter.

The ``--membind`` option
^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-membind.rst

Where the memory goes
^^^^^^^^^^^^^^^^^^^^^

The candidates are the NUMA nodes local to the process' cpuset: those whose
locality contains it (the DDR node holding a core, and an HBM node sharing
that locality) and those inside it (each node of a package the process is
bound to). The choice is never made across the whole machine, so a process
is not sent to a node on another socket because it is larger.

Candidates the PRRTE daemon may not itself use (for example because of a
cgroup) are dropped *before* the choice is made, since the process inherits
that restriction: if the best node is forbidden, the best permitted one is
chosen. Nodes tied for the best value are all kept. If no candidate is left,
or none has a value for the attribute, the process' memory is not bound,
which is handled as a failure (see `Failure handling`_).

.. note:: ``BANDWIDTH`` and ``LATENCY`` are only as good as the values hwloc
   has. On platforms that do not expose the HMAT information, or inside a
   container that hides it, hwloc has nothing to compare and the attribute
   cannot be resolved - which is handled as a failure below.


Interaction with other parameters
---------------------------------

The process must be bound to cpus
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The memory binding is computed from the cpuset a process is bound to, so
it is applied to every process the mapper binds to cpus |mdash| whether
the binding policy was given with ``--bind-to``, derived from ``--map-by``
(``--map-by numa`` binds to the NUMA domain, for example), or PRRTE's
default |mdash| and to no other. A process is left unbound by
``--bind-to none``, and when PRRTE drops its *default* binding because a
node is oversubscribed.

When a memory binding is requested for processes that end up unbound,
PRRTE prints one warning per app when it maps the job, naming how many of
its processes are affected, and runs the job with their memory left
unbound:

.. code-block:: sh

   $ mpirun --bind-to none --membind attr:bandwidth -n 4 ./a.out
   A memory binding was requested for processes that are not bound to
   cpus:
   ...

A process whose default cpu binding is refused by the operating system is
tolerated as unbound (the default is a preference, not a requirement);
its memory is then not bound either, without a warning of its own.

**mem_alloc_policy.** ``hwloc_default_mem_alloc_policy`` is PRRTE's older,
coarser control: it either does nothing (``none``) or restricts a process
to its local NUMA domain (``local_only``). A memory binding requested by
``--membind`` or ``hwloc_default_mem_bind_attribute`` takes precedence, for
every process it applies to.

**Topology files.** A topology read from a file with ``hwloc_use_topo_file``
describes a machine other than the one the process runs on, so no memory
binding is applied.


Failure handling
----------------

Failing to bind is governed by ``hwloc_default_mem_bind_failure_action``,
the same parameter that governs ``local_only``:

* ``silent`` - proceed without comment.
* ``warn`` (default) - warn the first time, then let the job continue,
  possibly with degraded performance.
* ``error`` - abort the job.

This covers the attribute being unavailable (an hwloc older than 2.3, or no
data for it), no usable local node, as well as the operating system
refusing the request. Many container runtimes deny the system call that
sets a memory policy; inside one, use ``silent`` or ``warn`` unless the job
must not run without it.


Seeing what was chosen
----------------------

Raise the hwloc verbosity to see the node set PRRTE selected for each
process:

.. code-block:: sh

   prterun --prtemca hwloc_base_verbose 40 \
           --bind-to core --membind attr:bandwidth -n 2 ./a.out

and inspect the running process on its node: the mappings in
``/proc/<pid>/numa_maps`` carry a ``bind:`` entry naming the node(s), and
``numastat -p <pid>`` shows where its pages actually landed.
