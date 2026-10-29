.. -*- rst -*-

   Copyright (c) 2026      SiPearl.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Bind the memory of each process to the NUMA node(s) best suited to it
by the chosen measure, among the nodes local to the CPUs the process is
bound to. This is separate from ``--bindto``, which decides where a
process runs: ``--membind`` decides where its memory is allocated. It
needs the process to be bound to CPUs (see below).

Supported directives:

* ``NONE`` binds no memory. This is the default.

* ``CAPACITY``: the local node with the most memory

* ``LOCALITY``: every node local to the process' CPUs

* ``BANDWIDTH``: the local node with the highest bandwidth from the
  process' CPUs. This may be a node with no CPUs of its own |mdash| on
  a processor with on-package high-bandwidth memory (HBM), the HBM
  nodes hold no cores, so a process bound to a core sits on a DDR node
  and ``BANDWIDTH`` binds its memory to the HBM node next to it.

* ``LATENCY``: the local node with the lowest latency from the
  process' CPUs

Nodes tied for the best value are all used.

Directives are case-insensitive and can be shortened to the minimum
number of characters to uniquely identify them. Thus, ``bandwidth``,
``BANDWIDTH`` and ``b`` are the same request. A shortening that fits
more than one of them is refused rather than guessed at |mdash| ``L``
could be ``LOCALITY`` or ``LATENCY``.

Memory is only bound for processes that are bound to CPUs, whether by
``--bindto``, by the binding ``--mapby`` derives, or by PRRTE's
default. ``--bindto none`` leaves processes unbound, and so does an
oversubscribed node, on which PRRTE drops its default binding: PRRTE
then warns once per app and runs the job with that memory unbound. A
``--membind`` takes precedence over the
``hwloc_default_mem_alloc_policy`` MCA parameter, and a failure to bind
memory is handled according to
``hwloc_default_mem_bind_failure_action``. The directives are
currently implemented with hwloc's memory attributes, which requires
hwloc 2.3 or later, and ``BANDWIDTH`` and ``LATENCY`` are only as good
as the values hwloc has for the platform.

The ``hwloc_default_membind`` MCA parameter takes the same
values, and supplies the memory binding for a job or app that gives no
``--membind``.

.. rubric:: Per-app-context memory binding (MPMD jobs)

The rule is the one described for ``--bindto``: a ``--membind`` written
on the first app segment and nowhere else describes the whole job;
otherwise each app that carries one is bound by its own, and apps that
carry none take the default |mdash| they do not inherit another app's
directive.

Example:

.. code::

   prun --bindto core --membind bandwidth -n 4 app1 : --bindto core --membind capacity -n 2 app2

Here the memory of ``app1`` processes is bound to the node with the
highest bandwidth from their core, and that of ``app2`` processes to
the node with the most memory, all within the same job.
