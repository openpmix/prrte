.. -*- rst -*-

   Copyright (c) 2026 Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Direct that the application be executed using the resources of the
allocation identified by the given reference ID, where that ID is the
one *the user* attached to the allocation request at the time the
allocation was made. This is the convenient counterpart to
``--alloc-id``: it lets a script name an allocation using a label it
chose itself, without having to capture the identifier the host
environment later assigned to it.

The named allocation must be one the requester owns |mdash| a job can
only be spawned onto resources its requester has been granted. Naming
an allocation the DVM does not know about, or one the requester does
not own, is an unrecoverable error: the job is not launched.

If no allocation is named, the job is mapped onto the allocation of the
session that requested it (the DVM's default session, in the case of a
tool such as ``prun``).

.. note:: The same allocation can be named in three different ways
          |mdash| by the reference ID given here, by the ID the host
          environment assigned to the allocation (``--alloc-id``), or
          by the numerical ID of the session that holds it
          (``--session-id``). These are alternative spellings of one
          directive and therefore cannot be combined on a single
          command line.
