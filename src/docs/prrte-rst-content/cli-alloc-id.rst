.. -*- rst -*-

   Copyright (c) 2026 Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Direct that the application be executed using the resources of the
allocation identified by the given ID, where that ID is the one the
host environment (e.g., the scheduler) assigned to the allocation when
it was created. The DVM reports this value back to the requester when
an allocation request completes, and it is the value returned by a
query of the allocation.

The named allocation must be one the requester owns |mdash| a job can
only be spawned onto resources its requester has been granted. Naming
an allocation the DVM does not know about, or one the requester does
not own, is an unrecoverable error: the job is not launched.

If no allocation is named, the job is mapped onto the allocation of the
session that requested it (the DVM's default session, in the case of a
tool such as ``prun``).

.. note:: The same allocation can be named in three different ways
          |mdash| by the host-assigned ID given here, by the reference
          ID the user attached to the allocation request
          (``--alloc-refid``), or by the numerical ID of the session
          that holds it (``--session-id``). These are alternative
          spellings of one directive and therefore cannot be combined
          on a single command line.
