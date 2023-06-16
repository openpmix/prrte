.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

PRRTE allows a user to expand an existing DVM prior to launching an
application.  Users can specify a hostfile that contains a list of
nodes to be added to the DVM using normal hostfile syntax.

The list can include nodes that are already part of the DVM |mdash| in
this case, the number of slots available on those nodes will be set to
the new specification, or adjusted as directed:

.. code::

   node01  slots=5

would direct that node01 be set to 5 slots, while

.. code::

   node01 slots+=5

would add 5 slots to the current value for node01, and

.. code::

   node01  slots-=5

would subtract 5 slots from the current value.

Slot adjustments for existing nodes will have no impact on currently executing
jobs, but will be applied to any new spawn requests. Nodes contained in the
add-hostfile specification are available for immediate use by the accompanying
application.

Users desiring to constrain the accompanying application to the newly added
nodes should also include the ``--hostfile`` command line directive, giving
the same hostfile as its argument:

.. code::

   --add-hostfile <filename> --hostfile <filename>
