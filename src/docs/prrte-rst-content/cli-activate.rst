.. -*- rst -*-

   Copyright (c) 2026 Nanook Consulting.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

A DVM does not necessarily span every node of its allocation.  A
``--host`` or ``--hostfile`` given when the DVM was started narrows
which allocated nodes get a daemon, and a released reservation hands
its nodes back without one.  Such a node is allocated and up, but no
part of the DVM: nothing can be launched on it.

``--activate`` starts a daemon on those nodes, bringing them into the
DVM before the accompanying application is launched.  It takes the
same argument syntax as ``--host``: a comma-delimited list of node
names,

.. code::

   --activate node01,node02

or, to bring in everything the allocation holds that is not already in
the DVM,

.. code::

   --activate +all

or the relative form that names a node by its position in the
allocation,

.. code::

   --activate +n3

which selects the fourth node (relative node indices count from zero).
Entries may also name a file, in the same format ``--hostfile`` reads,
and the forms may be mixed in one list:

.. code::

   --activate file=/path/to/hostfile
   --activate node01,file=/path/to/hostfile

Only the node names are taken from the file.  A ``slots=`` it carries is
not applied |mdash| a hostfile given to a launcher selects nodes, it does
not resize them |mdash| but everything else the hostfile format offers,
including ``^host`` exclusions, works as it does anywhere else.

.. note:: ``+e`` is **not** accepted here.  For ``--host`` it means
          "nodes with no application process running on them", which
          says nothing about whether a node is in the DVM: most of the
          nodes it picks are already in it, so the request would start
          no daemon and still report success.

Unlike ``--add-host``, this adds nothing to the allocation.  It can
only name nodes the allocation already contains, it changes no slot
counts, and it asks no resource manager for anything |mdash| which is
why it is permitted even where the allocation is owned by a scheduler
and ``--add-host`` is refused.  For the same reason a ``:N`` slot
extension is not accepted here: activate has no authority to set slot
counts, so a count given to it is refused rather than silently
ignored.

Naming a node that is already in the DVM is not an error |mdash| the
request is a statement about the DVM's membership, and for that node it
is already satisfied.  For the same reason ``+all`` is satisfied, not
refused, when every allocated node is already in the DVM.

Users desiring to constrain the accompanying application to the newly
activated nodes should also include the ``--host`` command line
directive, giving the same hosts in its argument:

.. code::

   --activate node01,node02 --host node01,node02
