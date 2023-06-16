Overview
========

PRRTE provides a set of three controls for assigning process
locations and ranks:

#. Mapping: Assigns a default location to each process
#. Ranking: Assigns a unique integer rank value to each process
#. Binding: Constrains each process to run on specific processors

This section provides an overview of these three controls.  Unless
otherwise this behavior is shared by ``prun(1)`` (working with a PRRTE
DVM), and ``prterun(1)``. More detail about PRRTE process placement is
available in the following sections (using ``--help
placement-<section>``):

* ``examples``: some examples of the interactions between mapping,
  ranking, and binding options.

* ``fundamentals``: provides deeper insight into PRRTE's mapping,
  ranking, and binding options.

* ``limits``: explains the difference between *overloading* and
  *oversubscribing* resources.

* ``diagnostics``: describes options for obtaining various diagnostic
  reports that aid the user in verifying and tuning the placement for
  a specific job.

* ``rankfiles``: explains the format and use of the rankfile mapper
  for specifying arbitrary process placements.

* ``deprecated``: a list of deprecated options and their new
  equivalents.

* ``all``: outputs all the placement help except for the
  ``deprecated`` section.


Quick Summary
-------------

The two binaries that most influence process layout are ``prte(1)``
and ``prun(1)``.  The ``prte(1)`` process discovers the allocation,
establishes a Distributed Virtual Machine by starting a ``prted(1)``
daemon on each node of the allocation, and defines the efault
mapping/ranking/binding policies for all jobs.  The ``prun(1)`` process
defines the specific mapping/ranking/binding for a specific job. Most
of the command line controls are targeted to ``prun(1)`` since each job
has its own unique requirements.

``prterun(1)`` is just a wrapper around ``prte(1)`` for a single job
PRRTE DVM. It is doing the job of both ``prte(1)`` and ``prun(1)``,
and, as such, accepts the sum all of their command line arguments. Any
example that uses ``prun(1)`` can substitute the use of ``prterun(1)``
except where otherwise noted.

The ``prte(1)`` process attempts to automatically discover the nodes
in the allocation by querying supported resource managers. If a
supported resource manager is not present then ``prte(1)`` relies on a
hostfile provided by the user.  In the absence of such a hostfile it
will run all processes on the localhost.

If running under a supported resource manager, the ``prte(1)`` process
will start the daemon processes (``prted(1)``) on the remote nodes
using the corresponding resource manager process starter. If no such
starter is available then ``ssh`` (or ``rsh``) is used.

Minus user direction, PRRTE will automatically map processes in a
round-robin fashion by CPU, binding each process to its own CPU. The
type of CPU used (core vs hwthread) is determined by (in priority
order):

* user directive on the command line via the HWTCPUS qualifier to
  the ``--map-by`` directive

* setting the ``rmaps_default_mapping_policy`` MCA parameter to
  include the ``HWTCPUS`` qualifier. This parameter sets the default
  value for a PRRTE DVM |mdash| qualifiers are carried across to DVM
  jobs started via ``prun`` unless overridden by the user's command
  line

* defaulting to ``CORE`` in topologies where core CPUs are defined,
  and to ``hwthreads`` otherwise.

By default, the ranks are assigned in accordance with the mapping
directive |mdash| e.g., jobs that are mapped by-node will have the
process ranks assigned round-robin on a per-node basis.

PRRTE automatically binds processes unless directed not to do so by
the user. Minus direction, PRRTE will bind individual processes to
their own CPU within the object to which they were mapped. Should a
node become oversubscribed during the mapping process, and if
oversubscription is allowed, all subsequent processes assigned to that
node will *not* be bound.

.. _placement-definition-of-slot-label:

Definition of 'slot'
--------------------

The term "slot" is used extensively in the rest of this documentation.
A slot is an allocation unit for a process.  The number of slots on a
node indicate how many processes can potentially execute on that node.
By default, PRRTE will allow one process per slot.

If PRRTE is not explicitly told how many slots are available on a node
(e.g., if a hostfile is used and the number of slots is not specified
for a given node), it will determine a maximum number of slots for
that node in one of two ways:

#. Default behavior: By default, PRRTE will attempt to discover the
   number of processor cores on the node, and use that as the number
   of slots available.

#. When ``--use-hwthread-cpus`` is used: If ``--use-hwthread-cpus`` is
   specified on the command line, then PRRTE will attempt to discover
   the number of hardware threads on the node, and use that as the
   number of slots available.

This default behavior also occurs when specifying the ``--host``
option with a single host.  Thus, the command:

.. code:: sh

   shell$ prun --host node1 ./a.out

launches a number of processes equal to the number of cores on node
``node1``, whereas:

.. code:: sh

   shell$ prun --host node1 --use-hwthread-cpus ./a.out

launches a number of processes equal to the number of hardware
threads on ``node1``.

When PRRTE applications are invoked in an environment managed by a
resource manager (e.g., inside of a Slurm job), and PRRTE was built
with appropriate support for that resource manager, then PRRTE will
be informed of the number of slots for each node by the resource
manager.  For example:

.. code:: sh

   shell$ prun ./a.out

launches one process for every slot (on every node) as dictated by
the resource manager job specification.

Also note that the one-process-per-slot restriction can be overridden
in unmanaged environments (e.g., when using hostfiles without a
resource manager) if oversubscription is enabled (by default, it is
disabled).  Most parallel applications and HPC environments do not
oversubscribe; for simplicity, the majority of this documentation
assumes that oversubscription is not enabled.

Slots are not hardware resources
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Slots are frequently incorrectly conflated with hardware resources.
It is important to realize that slots are an entirely different metric
than the number (and type) of hardware resources available.

Here are some examples that may help illustrate the difference:

#. More processor cores than slots: Consider a resource manager job
   environment that tells PRRTE that there is a single node with 20
   processor cores and 2 slots available.  By default, PRRTE will
   only let you run up to 2 processes.

   Meaning: you run out of slots long before you run out of processor
   cores.

#. More slots than processor cores: Consider a hostfile with a single
   node listed with a ``slots=50`` qualification.  The node has 20
   processor cores.  By default, PRRTE will let you run up to 50
   processes.

   Meaning: you can run many more processes than you have processor
   cores.

.. _placement-definition-of-processor-element-label:

Definition of "processor element"
---------------------------------

By default, PRRTE defines that a "processing element" is a processor
core.  However, if ``--use-hwthread-cpus`` is specified on the command
line, then a "processing element" is a hardware thread.
