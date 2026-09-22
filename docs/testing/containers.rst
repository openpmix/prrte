The container harnesses
=======================

Two harnesses in ``contrib/`` run PRRTE across several "nodes" without a
cluster, using plain containers on one machine. They are the everyday
development loop: fast, needing nothing but Docker, and running your
*live* working tree rather than a commit.

They are documented in full in the trees themselves — this page says what
each one is for and when to reach for it. If you have real nodes, see
:doc:`cluster`: the first harness's suite runs there too, unchanged.

``contrib/dockerswarm`` — ten nodes, no scheduler
-------------------------------------------------

The canonical multi-node harness. ``build.sh`` bind-mounts your working
tree into a builder container, compiles it out-of-tree into a shared
volume, and ten ``ubuntu`` containers mount that volume as their PRRTE
installation. ``run-tests.sh linux`` then drives the suite described in
:doc:`cluster` across them.

.. code-block:: sh

   shell$ cd contrib/dockerswarm
   shell$ ./build.sh
   shell$ docker compose up -d
   shell$ ./run-tests.sh linux

It builds PMIx from source on every run, because PRRTE uses PMIx
*internals* — the pair is one code base in two repositories, and testing
one against a frozen copy of the other tests neither properly.

``./run-tests.sh macos`` is a single-host subset that builds and smoke-tests
natively on a Mac, which is what catches Darwin portability regressions.

Full guide: ``contrib/dockerswarm/AGENTS.md``.

``contrib/slurmswarm`` — the same, with a real SLURM
----------------------------------------------------

Everything about the *scheduler* lives here, and nothing else does. The
same ten containers, but running a real ``slurmctld``, ten real
``slurmd``\ s and SLURM built from source, so ``ras/slurm`` and
``plm/slurm`` can be exercised against a scheduler that can refuse a
request.

That matters because most of PRRTE's elastic surface under SLURM is a
shell-out — ``salloc`` to grow, ``scontrol show job --json`` to learn what
was granted, ``scontrol update`` to shrink in place, ``scancel`` to give a
node back. A stand-in scheduler can show that PRRTE issued the right
command; only a real one can show that SLURM accepts it.

.. code-block:: sh

   shell$ cd contrib/slurmswarm
   shell$ ./build.sh
   shell$ docker compose up -d
   shell$ ./run-tests.sh

Full guide: ``contrib/slurmswarm/AGENTS.md``.

Which one
---------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Question
     - Where

   * - Does this runtime change work across daemons?
     - ``contrib/dockerswarm`` — faster, and needs no scheduler.

   * - Does PRRTE talk to SLURM correctly?
     - ``contrib/slurmswarm``.

   * - Does PRRTE work on *my* machines?
     - :doc:`cluster`.
