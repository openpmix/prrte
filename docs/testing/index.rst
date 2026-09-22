Testing PRRTE
=============

PRRTE is a runtime, and almost everything that can go wrong with a runtime
goes wrong *between* machines: a daemon that never joins the tree, a file
that never crossed, a collective that completes without everybody in it.
None of that is visible from a single host, and none of it is visible from
a build that merely compiled.

This section is about how to convince yourself that a PRRTE you have
built, or a change you have made to it, actually works — including on
**your own cluster**, which is the only place your own hardware, network,
file system and resource manager are in the room.

**Four layers, and what each one can tell you:**

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - Layer
     - How to run it
     - What it can and cannot show

   * - **Unit tests**
     - ``make check`` from your build tree
     - Parsers, policy resolution, data-type round trips, the object
       model. Fast, no daemons. Cannot show anything about launch.

   * - **Offline mapper harness**
     - ``make -C test/offline check-offline``
     - Every combination of ``--map-by``, ``--rank-by`` and ``--bind-to``
       against synthetic topologies, checked against invariants derived
       from the topology. Over a thousand cases, and it launches nothing.
       Cannot show that a map is *carried out*.

   * - **Container harness**
     - :doc:`containers`
     - The full multi-node suite — launch, I/O forwarding, file staging,
       collectives, elastic grow/shrink, routing relay — across ten
       containers on one machine. Needs no cluster and no scheduler.
       Cannot show anything about a real network, a real file system, or
       hardware the containers do not have.

   * - **Your cluster**
     - :doc:`cluster`
     - **The same suite**, on real nodes over ``ssh``. This is the layer
       that answers "does PRRTE work *here*" — on this interconnect, with
       this file system, under this resource manager, on these CPUs.

The first three are for the PRRTE developer and run on a laptop. The
fourth is for anyone deploying PRRTE, and it is the one this section
exists to make easy: the multi-node suite is not a container thing that
happens to run in containers, it is a *runtime* test suite that had, until
now, only one place to run.

.. toctree::
   :maxdepth: 2

   cluster
   containers
