.. Copyright (c) 2026      Nanook Consulting  All rights reserved.
   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

==========================================
Offline mapping test harness
==========================================

``run_offline_maps.py`` drives ``prterun --rtos donotlaunch --display
map`` over a matrix of ``--map-by`` / ``--rank-by`` / ``--bind-to``
directives, crossed with every hwloc topology XML in
``test/topologies/``, parses each printed map, and verifies it against
invariants derived from the topology.  Nothing is launched; no DVM is
started.  See ``SPEC.rst`` for the design and ``IMPL-PLAN.rst`` for the
build-out.

Quick start
===========

From a build tree, ``make check`` runs it automatically (it locates the
freshly built ``prte`` and the committed topologies).  To run it by hand::

    # point it at a built prterun (or have one on PATH)
    export PRTE_PRTERUN=/path/to/build/bin/prterun
    export top_srcdir=/path/to/prrte/srcdir
    ./run_offline_maps.py

Exit status: ``0`` all pass, ``1`` a failure, ``77`` prerequisites missing
(no ``prterun``/topologies -- the Automake "skip" convention).

Useful options
==============

============================  ====================================================
``--full``                    run the full layout x N cross product (slower)
``--filter '<glob>'``         run only cases whose id matches the glob
``--list``                    print case ids without running
``--verbose``                 echo every PASS and the rejection reason
``--topo <file>``             test one explicit topology (repeatable)
``--topo-dir <dir>``          discover ``*.xml`` in a different directory
``--golden``                  also compare a curated subset to golden snapshots
``--update-golden``           regenerate the golden snapshots (review the diff!)
============================  ====================================================

Examples::

    ./run_offline_maps.py --list --filter 'matrix.*.m-package.*'
    ./run_offline_maps.py --full
    ./run_offline_maps.py --golden

Adding a topology
=================

Drop a new ``*.xml`` (e.g. produced by ``lstopo new.xml``) into
``test/topologies/``.  The harness discovers it automatically and runs
the entire pattern set against it -- no edit to the harness or the build
is required.  If you keep golden snapshots, regenerate them with
``--update-golden`` and review the diff before committing.

What is checked
===============

Per successful map: policy-string match; total procs == ``-n``; ranks are
a permutation of ``0..N-1``; per-node counts match the placement shape for
the map policy (round-robin by node, block-fill by slot, node-local for
object maps); ranking shape for ``slot``/``node``; and each binding spans
exactly one object of the requested level, computed from the topology.
``fill``/``span`` ranking and multi-app/ppr placement shapes are pinned by
golden snapshots rather than invariants.  Cases that PRRTE must reject
(``rank-by fill/span`` without an object map; binding above the mapped
object; bad tokens) are verified to be rejected.
