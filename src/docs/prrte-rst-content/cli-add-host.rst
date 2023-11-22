.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

PRRTE allows a user to expand an existing DVM prior to launching an
application.  Users can specify a a comma-delimited list of node
names, each entry optionally containing a ``:N`` extension indicating
the number of slots to assign to that entry:

.. code::

   --host node01:5,node02

In the absence of the slot extension, one slot will be assigned to the
node. Duplicate entries are aggregated and the number of slots
assigned to that node are summed together.

.. note:: A "slot" is the PRRTE term for an allocatable unit where we
          can launch a process. Thus, the number of slots equates to
          the maximum number of processes PRRTE may start on that node
          without oversubscribing it.

The list can include nodes that are already part of the DVM |mdash| in
this case, the number of slots available on those nodes will be set to
the new specification, or adjusted as directed:

.. code::

   --host node01:5,node02

would direct that node01 be set to 5 slots and node02 will have 1
slot, while

.. code::

   --host node01:+5,node02

would add 5 slots to the current value for node01, and

.. code::

   --host node01:-5,node02

would subtract 5 slots from the current value.

Slot adjustments for existing nodes will have no impact on currently executing
jobs, but will be applied to any new spawn requests. Nodes contained in the
add-host specification are available for immediate use by the accompanying
application.

Users desiring to constrain the accompanying application to the newly added
nodes should also include the ``--host`` command line directive, giving
the same hosts in its argument:

.. code::

   --add-host node01:+5,node02 --host node01:5,node02

Note that the ``--host`` argument indicates the number of slots to assign
node01 for this spawn request, and not the number of slots being added to
the node01 allocation.
