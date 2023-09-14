Session directory
=================

PRRTE establishes a "session directory" on the filesystem to serve as
a top-level location for temporary files used by both the local PRRTE
daemon and its child processes.

This is done to enable quick and easy cleanup in the event that PRRTE
is unable to fully cleanup after itself.

Directory location
------------------

PRRTE decides where to located the root of the session directory by
examining the following (in precedence order):

#. If the value of the ``prte_top_session_dir`` MCA parameter is not
   empty, use that (it defaults to empty).

   .. note:: MCA parameters can be set via environment variables, on
             the command line, or in a parameter file.

#. If the environment variable ``TMPDIR`` is not empty, use that.
#. If the environment variable ``TEMP`` is not empty, use that.
#. If the environment variable ``TMP`` is not empty, use that.
#. Use ``/tmp``

Directory name
--------------

By default, the session directory name is set to

.. code::

   prte.<nodename>.<uid>

The session directory name can further be altered to include the PID
of the daemon process, if desired:

.. code::

   prte.<nodename>.<pid>.<uid>

by setting the ``prte_add_pid_to_session_dirname`` MCA parameter to a
"true" value (e.g., 1).

Tools
-----

In the case of tools, the rendezvous files containing connection
information for a target server are located in the session directory
tree. Thus, it may be necessary to point the tool at the location
where those files can be found if that location is other than the
expected default.
