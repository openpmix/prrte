.. _label-multi-user-dvm:

Sharing a DVM between users
===========================

By default a DVM serves one user: the user it runs as. Tools of any other
user are refused when they connect. The ``prte_pmix_no_foreign_tools``
MCA parameter (default ``true``) controls this; setting it to ``0`` lets
other users' tools connect::

   prte --prtemca prte_pmix_no_foreign_tools 0 ...

What a tool of another user may then do - signal a job, read its output,
query it - is decided by the job's owner and its access list (see the
``users=`` and ``groups=`` runtime options, ``prterun --help
runtime-options``).

Who a job's processes run as
----------------------------

A job belongs to the user whose tool started it, and its processes run as
that user, with that user's groups:

* **A DVM running as root** starts every job's processes as the user the
  job belongs to, on every node. They see themselves as that user, their
  PMIx server accepts them as that user, and the session directories made
  for the job (``PMIX_NSDIR``, ``PMIX_PROCDIR``) belong to that user and
  no one else. Jobs of root itself run as root.

* **A DVM running as any other user** cannot start processes as another
  user, so it runs only its own user's jobs and root's. A job of any other
  user is refused, with a message saying why, and the tool that asked is
  told ``PMIX_ERR_NO_PERMISSIONS``. It is never run with the DVM's
  privileges instead.

  A tool also refuses, on its own side, to connect to a server another
  ordinary user runs, unless it is told it may (PMIx's
  ``pmix_ptl_base_server_access``).

So a DVM meant to run several users' jobs runs as root.

What the DVM does not do for another user
-----------------------------------------

Some directives ask the DVM itself, rather than the job's processes, to
open a file. The DVM does that as the user it runs as, so they are
accepted only from root and from the DVM's own user:

* a hostfile, or a hostfile of hosts to add to the DVM;
* a rankfile or sequential-mapping file;
* a file for the process table (``--output-proctable``);
* binaries or files to preload.

A job of any other user that asks for one of them is refused, naming the
directive. Output files a job's ``--output`` directive names are not on
this list: they are opened as the job's own user.

Who may grow the DVM
--------------------

Bringing hosts into an elastic DVM - with ``--add-host`` or
``--activate`` on a job, or with an allocation request
(``PMIx_Allocation_request``) that names nodes - changes what the DVM
consists of. It is accepted from root, the DVM's own user, and the users
and groups the DVM was started with (``prte --rtos users=...,groups=...``),
as creating a session is. When the DVM hands such requests to a PMIx
scheduler, the scheduler decides instead.

A host may be given as ``user@host`` to name the user the DVM logs in to
it as. That is accepted only from root and the DVM's own user, and a host
name may contain only letters, digits, ``.``, ``-`` and ``_``.
