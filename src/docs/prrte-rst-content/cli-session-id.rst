.. -*- rst -*-

   Copyright (c) 2026 Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Direct that the application be executed using the resources of the
session identified by the given numerical ID. A *session* is the
container the DVM uses to hold an allocation together with the jobs
running against it, and every allocation the DVM grants is assigned
one. The session ID is reported back to the requester when an
allocation request completes.

The value must be an unsigned 32-bit integer; anything else is rejected
before the job is submitted.

The named session must be one the requester owns |mdash| a job can only
be spawned onto resources its requester has been granted. Naming a
session the DVM does not know about, or one the requester does not own,
is an unrecoverable error: the job is not launched.

If no session is named, the job is mapped onto the session that
requested it (the DVM's default session, in the case of a tool such as
``prun``).

.. note:: The same allocation can be named in three different ways
          |mdash| by the session ID given here, by the ID the host
          environment assigned to the allocation (``--alloc-id``), or
          by the reference ID the user attached to the allocation
          request (``--alloc-refid``). These are alternative spellings
          of one directive and therefore cannot be combined on a single
          command line.
