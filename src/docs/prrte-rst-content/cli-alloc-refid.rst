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

The named allocation must be one the requester is entitled to use
|mdash| a job can only be spawned onto resources its requester has been
granted. Entitlement is by namespace and by user: the namespace that
requested the allocation holds it, as does every job spawned into it,
and so does any tool run by the user the allocation was granted to.
That last part is what lets a later command reach an allocation an
earlier one created, since a tool's namespace lasts only as long as the
command that made it.

Naming an allocation the DVM does not know about is an unrecoverable
error and reports ``PMIX_ERR_NOT_FOUND``; naming one the requester may
not use is likewise unrecoverable and reports
``PMIX_ERR_NO_PERMISSIONS``. In either case the job is not launched.

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
