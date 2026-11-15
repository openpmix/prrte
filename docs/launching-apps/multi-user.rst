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

Files the DVM opens for a job
-----------------------------

Some directives ask the DVM itself, rather than the job's processes, to
open a file:

* a hostfile, or a hostfile of hosts to add to the DVM;
* a rankfile or sequential-mapping file;
* a file for the process table (``--rtos output-proctable=<file>``).

The DVM opens each of these as the job's own user, with that user's
groups, so a job may name only files its user could open itself. A file
it may not read is reported as one that cannot be opened, and a process
table is created, or replaced, only where the job's user may write. An
allocation request that names a hostfile has it read as the requester in
the same way. Output files a job's ``--output`` directive names are
opened as the job's own user too.

Preloading binaries or files (``--preload-files``, ``--preload-binary``)
is the exception: the DVM reads them, and its daemons write the copies on
every node, as the user the DVM runs as. Preloading is therefore accepted
only from root and from the DVM's own user, and a job of any other user
that asks for it is refused, naming the directive.

Who may grow the DVM
--------------------

Bringing hosts into an elastic DVM - with ``--add-host``,
``--add-hostfile`` or ``--activate`` on a job, or with an allocation
request (``PMIx_Allocation_request``) that names nodes or a hostfile -
changes what the DVM consists of. It is accepted from root, the DVM's own user, and the users
and groups the DVM was started with (``prte --rtos users=...,groups=...``),
as creating a session is. When the DVM hands such requests to a PMIx
scheduler, the scheduler decides instead.

A user asks for resources - a new allocation, or more nodes or time for one
of its own - with ``PMIx_Allocation_request``, and the scheduler decides.
``PMIx_Session_control`` with ``PMIX_SESSION_INSTANTIATE`` or
``PMIX_SESSION_EXTEND`` is not such a request: it is how the scheduler tells
the DVM what it has decided. With no scheduler attached, those two
directives are accepted only from the same users who may grow the DVM, and
that includes the user the session is for. The user a session is for may
still pause, resume, signal and terminate it.

A host may be given as ``user@host`` to name the user the DVM logs in to
it as, on the command line, in a hostfile of hosts to add, or in the node
list of a session being created or extended. That is
accepted only from root and the DVM's own user, and a host name may
contain only letters, digits, ``.``, ``-`` and ``_``.

Whom an allocation is for
-------------------------

An allocation request (``PMIx_Allocation_request``) for new or extended
resources is for the user who makes it, and that user's group, unless it
says otherwise with ``PMIX_ALLOC_UID`` and ``PMIX_ALLOC_GID``. A user may
name only itself; root may name any user. Any user may name a group it
belongs to. Anything else is refused with ``PMIX_ERR_NO_PERMISSIONS``.
The allocation belongs to the user and group it is for, and they are who
may extend or release it.
