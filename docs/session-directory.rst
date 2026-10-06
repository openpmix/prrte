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

By default, the session directory name is set to

.. code::

   <prefix>.<pid>

where `prefix` names the tool setting up the session directory -
`prte`, `prtrn` (for `prterun`), or `prted`, for example - though
special tools may also create a session directory tree. When the
temporary directory is on a file system shared between nodes, the
node's name is included as well:

.. code::

   <prefix>.<nodename>.<pid>

The session directory name includes the PID
of the daemon process to allow a user to have multiple
instances of a tool concurrently executing on a node.

A directory already at that name is used only if it belongs to the
user and no one else can write to it - one left over from an earlier
run of the user's own, for example. Otherwise PRRTE creates its
session directory beside it instead, under the same name with a
unique suffix added:

.. code::

   <prefix>.<pid>.XXXXXX

.. note::

   Each tool will generate its own session directory tree. This
   is done to avoid cleanup race conditions where one tool might
   cleanup the session directory, and thereby remove the contact
   information for a tool that is continuing to execute.


Tools
-----

In the case of tools, the rendezvous files containing connection
information for a target server are located in the session directory
tree. Thus, it may be necessary to point the tool at the location
where those files can be found if that location is other than the
expected default.
