.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Hostfiles (sometimes called "machine files") are a combination of two
things:

#. A listing of hosts on which to launch processes.
#. Optionally, limit the number of processes which can be launched on
   each host.

Hostfile syntax consists of one node name on each line, optionally
including a designated number of "slots":

.. code:: sh

   # This is a comment line, and will be ignored
   node01  slots=10
   node13  slots=5

   node15
   node16
   node17  slots=3
   ...

Blank lines and lines beginning with a ``#`` are ignored.

A hostfile may hold at most 64 MiB, and no line in it may be longer
than 1 MiB; a larger file is refused with a message saying which limit
it met. Reading it may also take no longer than PMIx's
``pmix_runas_timeout`` MCA parameter (10 seconds by default): the DVM
master reads the file in a separate process, as the user the file is
read for, and gives up on a file whose file system does not answer in
that time. The same limits apply to a rankfile and to a sequence file
(``--map-by seq``).

An entry names a node and nothing else. PRRTE reaches every node as the
user the DVM runs as, and does not start a daemon as any other user, so
an entry may not name an account to reach the node as: ``user@node`` and
the ``username=`` keyword are refused as parse errors, naming the
hostfile and the line. So is a rankfile entry naming one.

A node name written with a leading ``^`` is *excluded* rather than
used:

.. code:: sh

   node01  slots=4
   node02  slots=4
   node03  slots=4
   ^node02

Where the hostfile also names nodes, an exclusion takes a node back
out of what it named |mdash| the file above names ``node01`` and
``node03``.  A hostfile given to a job in a running DVM may consist of
nothing but exclusions, and then it selects every node of the
allocation except the ones it excludes.

A "slot" is the PRRTE term for an allocatable unit where we can launch
a process.  See the section on definition of the term ``slot`` for a
longer description of slots.

In the absence of the ``slot`` parameter, PRRTE will assign either the
number of slots to be the number of CPUs detected on the node or the
resource manager-assigned value if operating in the presence of an
RM.

.. important:: If using a resource manager, the user-specified number
               of slots is capped by the RM-assigned value.

A hostfile given to a job that is being submitted to an already-running
DVM *selects within* the DVM's allocation: it names the subset of nodes
that job may use, and a ``slots`` count smaller than the node's own is
the number of slots that job may take there.  It says nothing about how
big the node is, so it applies to that job alone |mdash| the node is
back to its allocated size for the next job, which may be someone
else's.  Changing the allocation is what ``--add-hostfile`` is for.
