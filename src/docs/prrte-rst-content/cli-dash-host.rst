.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Host syntax consists of a comma-delimited list of node names, each
entry optionally containing a ``:N`` extension indicating the number
of slots to assign to that entry:

.. code::

   --host node01:5,node02

In the absence of the slot extension, one slot will be assigned to the
node. Duplicate entries are aggregated and the number of slots
assigned to that node are summed together.

.. note:: A "slot" is the PRRTE term for an allocatable unit where we
   can launch a process. Thus, the number of slots equates to the
   maximum number of processes PRRTE may start on that node without
   oversubscribing it.

Given to a job, ``--host`` *selects* from the hosts already available
to the DVM |mdash| those a resource manager allocated, or those the DVM
was started with. It does not add any: naming a host that is not among
them is an error. Use ``--add-host`` or ``--add-hostfile`` to bring a
new host into a running DVM, or ``--activate`` to start a daemon on a
host the allocation already contains.

The ``:N`` count applies to *placement*, and not merely to the size of
the job: it is the number of processes that may be placed on that host,
whatever mapping policy is in effect. Asking for more slots on a host
than it has is an error under a resource manager, which decided how big
the host is. Without one, the larger count is taken as the size of the
host for that job only; the allocation itself is unchanged.

See the "Host specification" documentation for details about the
format and content of hostfiles.
