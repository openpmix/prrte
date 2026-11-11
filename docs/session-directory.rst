Session directory
=================

PRRTE establishes a "session directory" on the filesystem to serve as
a top-level location for temporary files used by both the local PRRTE
daemon and its child processes.

This is done to enable quick and easy cleanup in the event that PRRTE
is unable to fully cleanup after itself.

More detail on session directories is provided in the How Things Work
:ref:`session directory <session-dir-detail-label>` section.

Directory location
------------------

PRRTE decides where to locate the root of the session directory by
examining the following (in precedence order):

#. If the value of the ``prte_top_session_dir`` MCA parameter is not
   empty, use that (it defaults to empty).

   .. note:: MCA parameters can be set via environment variables, on
             the command line, or in a parameter file.

   .. note:: If necessary, the value of the top session directory on
             the local node where the launcher (e.g., ``prun``, ``prterun``,
             or ``mpirun``) is executing can be set separately from
             the value to be used on compute nodes via the
             ``prte_local_tmpdir_base`` and ``prte_remote_tmpdir_base``
             parameters.

#. If the environment variable ``TMPDIR`` is not empty, use that.
#. If the environment variable ``TEMP`` is not empty, use that.
#. If the environment variable ``TMP`` is not empty, use that.
#. Use ``/tmp``

Directory name
--------------

The session directory is named

.. code::

   <prefix>.XXXXXX

where `prefix` names the tool setting up the session directory -
`prte`, `prtrn` (for `prterun`), `prted` or `prun`, for example - though
special tools may also create a session directory tree. The six
characters after the prefix are chosen when the directory is created,
so that no two directories have the same name: any number of instances
of a tool can run at once on a node, and on a temporary directory shared
between nodes, each node's instances still have directories of their
own. A directory already in the temporary directory is never used.

Each session directory holds an empty file named after the node it
belongs to:

.. code::

   node.<nodename>

so that a directory found later - left behind on a shared file system,
for example - can be traced to its node. The file is removed with the
directory, and stays while PRRTE keeps anything else there, such as
output files.

.. note::

   Each tool will generate its own session directory tree. This
   is done to avoid cleanup race conditions where one tool might
   cleanup the session directory, and thereby remove the contact
   information for a tool that is continuing to execute.


Tools
-----

In the case of tools, the rendezvous files containing connection
information for a target server are not in the session directory tree:
each daemon writes them in its temporary directory, in a subdirectory
named ``pmix-user.<uid>``, and a tool such as ``prun`` looks for them
under its own. A tool started with a different ``TMPDIR`` than the
daemon it is to reach therefore finds nothing, and has to be pointed at
the daemon's rendezvous file (``prun --dvm-uri file:<path>``). See
:ref:`label-prun-local-daemon`.
