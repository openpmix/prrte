.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

.. note:: PRRTE accepts both the new ``--rankby`` and the older
          deprecated ``--rank-by`` cmd line options. For simplicity, the
          following description will refer to the new ``--rankby`` form.

PRRTE automatically ranks processes for each job starting from zero.
Regardless of the algorithm used, rank assignments span applications
in the same job |mdash| i.e., a command line of

.. code::

  -n 3 app1 : -n 2 app2

will result in ``app1`` having three processes ranked 0-2 and ``app2``
having two processes ranked 3-4.

By default, process ranks are assigned in accordance with the mapping
directive: jobs mapped by node are ranked by ``NODE``, jobs mapped with
the ``SPAN`` qualifier are ranked by ``SPAN``, jobs mapped to a
hardware object (hwthread, core, cache, NUMA region, or package) are
ranked by ``FILL``, and all other jobs are ranked by ``SLOT``. However,
users can override the default by specifying any of the following
directives using the ``--rankby`` command line option:

* ``SLOT`` assigns ranks to each process on a node in the order in
  which the mapper assigned them. This is the default for jobs mapped
  by slot, and is provided as an explicit option to allow users to
  override any alternative default. When mapping
  to a specific resource type, procs assigned to a given instance
  of that resource on a node will be ranked on a per-resource basis
  on that node before moving to the next node.

* ``NODE`` assigns ranks round-robin on a per-node basis

* ``FILL`` assigns ranks to procs mapped to a particular resource type
  on each node, filling all ranks on that resource before moving to
  the next resource on that node. For example, procs mapped by
  ``L1cache`` would have all procs on the first ``L1cache`` ranked
  sequentially before moving to the second ``L1cache`` on the
  node. Once all procs on the node have been ranked, ranking would
  continue on the next node.

* ``SPAN`` assigns ranks round-robin to procs mapped to a particular
  resource type, treating the collection of resource instances
  spanning the entire allocation as a single "super node" before
  looping around for the next pass. Thus, ranking would begin with the
  first proc on the first ``L1cache`` on the first node, then the next
  rank would be assigned to the first proc on the second ``L1cache``
  on that node, proceeding across until the first proc had been ranked
  on all ``L1cache`` used by the job before circling around to rank
  the second proc on each object.

The ``rankby`` command line option has no qualifiers.

.. note:: Directives are case-insensitive.  ``SPAN`` is the same as
          ``span``.

.. rubric:: Per-app-context ranking (MPMD jobs)

In a multi-program multiple-data (MPMD) job, each application context
may carry its own ``--rankby`` directive, placed ahead of that app's
executable. The rule is the one described for ``--mapby``: a directive
written on the first app segment and nowhere else describes the whole
job; otherwise each app that carries a directive is ranked by its own,
and apps that carry none take the default ranking policy.

Example:

.. code::

   prun --rankby fill -n 4 app1 : --rankby node -n 2 app2

Rank assignments always span all application contexts in the job and
remain globally contiguous: the first app in the command line receives
ranks starting from 0, and each subsequent app starts from the next
unassigned rank, regardless of the per-app ranking directive. The
per-app directive controls only the order in which processes within
that app are assigned their ranks relative to one another.

For example, a command line of

.. code::

   --rankby fill -n 3 app1 : --rankby node -n 2 app2

will result in ``app1`` having three processes ranked 0-2 (assigned
fill-style) and ``app2`` having two processes ranked 3-4 (assigned
node-style).

A more detailed description of the mapping, ranking, and binding
procedure can be obtained via the ``--help placement`` option.
