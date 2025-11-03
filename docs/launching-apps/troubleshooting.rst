Troubleshooting
===============

Launching applications can be a complex process that involves many moving parts.
This section attempts to provide solutions to some of the most common
problems users encounter.

Errors about missing libraries
------------------------------

When building PRRTE with the compilers that have libraries in
non-default search path locations, you may see errors about those
compiler's support libraries when trying to launch applications if
their corresponding environments were not setup properly.

For example, you may see warnings similar to the following:

.. code-block:: sh

   # With the Intel compiler suite
   shell$ prterun -n 1 --host node1.example.com hello
   prted: error while loading shared libraries: libimf.so: cannot open shared object file: No such file or directory
   --------------------------------------------------------------------------
   A daemon (pid 11893) died unexpectedly with status 127 while
   attempting to launch so we are aborting.
   ...more error messages...

   # With the PGI compiler suite
   shell$ prterun -n 1 --host node1.example.com hello
   prted: error while loading shared libraries: libpgcc.so: cannot open shared object file: No such file or directory
   ...more error messages...

   # With the PathScale compiler suite
   shell$ prterun -n 1 --host node1.example.com hello
   prted: error while loading shared libraries: libmv.so: cannot open shared object file: No such file or directory
   ...more error messages...

Specifically, PRRTE first attempts to launch a "helper" daemon
``prted`` on ``node1.example.com``, but it failed because one of
``prted``'s dependent libraries was not able to be found.  The
libraries shown above (``libimf.so``, ``libpgcc.so``, and
``libmv.so``) are specific to their compiler suites (Intel, PGI, and
PathScale, respectively).  As such, it is likely that the user did not
setup the compiler library in their environment properly on this node.

Double check that you have setup the appropriate user environment
on the target node, for both interactive and non-interactive logins.

.. note:: It is a common error to ensure that the user environment
          is setup properly for *interactive* logins, but not for
          *non-interactive* logins.

Here's an example of a user-compiled MPI application working fine
locally, but failing when invoked non-interactively on a remote node:

.. code-block:: sh

   # Compile a trivial MPI application
   head_node$ cd $HOME
   head_node$ mpicc mpi_hello.c -o mpi_hello

   # Run it locally; it works fine
   head_node$ ./mpi_hello
   Hello world, I am 0 of 1.

   # Run it remotely interactively; it works fine
   head_node$ ssh node2.example.com

   Welcome to node2.
   node2$ ./mpi_hello
   Hello world, I am 0 of 1.
   node2$ exit

   # Run it remotely *NON*-interactively; it fails
   head_node$ ssh node2.example.com $HOME/mpi_hello
   mpi_hello: error while loading shared libraries: libimf.so: cannot open shared object file: No such file or directory

In cases like this, check your shell script startup files and verify
that the appropriate compiler environment is setup properly for
non-interactive logins.

Problems when running across multiple hosts
-------------------------------------------

When you are able to run jobs on a single host, but fail to run
them across multiple hosts, try the following:

#. Ensure that your launcher is able to launch across multiple hosts.
   For example, if you are using ``ssh``, try to ``ssh`` to each
   remote host and ensure that you are not prompted for a password.
   For example:

   .. code-block::

      shell$ ssh remotehost hostname
      remotehost

   If you are unable to launch across multiple hosts, check that your
   SSH keys are setup properly.  Or, if you are running in a managed
   environment, such as in a Slurm, Torque, or other job launcher,
   check that you have reserved enough hosts, are running in an
   allocated job, etc.

#. Ensure that your ``PATH`` and ``LD_LIBRARY_PATH`` are set correctly
   on each remote host on which you are trying to run.  For example,
   with ``ssh``:

   .. code-block::

      shell$ ssh remotehost env | grep -i path
      PATH=...path on the remote host...
      LD_LIBRARY_PATH=...LD library path on the remote host...

   If your ``PATH`` or ``LD_LIBRARY_PATH`` are not set properly, see
   :ref:`this section <running-prerequisites-label>` for
   the correct values.  Keep in mind that it is fine to have multiple
   PRRTE installations installed on a machine; the *first* PRRTE
   installation found by ``PATH`` and ``LD_LIBARY_PATH`` is the one
   that matters.

#. Run a simple operating system job across multiple hosts.  This verifies
   that the PRRTE run-time system is functioning properly across
   multiple hosts.  For example, try running the ``hostname`` command:

   .. code-block::

      shell$ prterun --host remotehost hostname
      remotehost
      shell$ prterun --host remotehost,otherhost hostname
      remotehost
      otherhost

   If you are unable to run operating system jobs across multiple hosts, check
   for common problems such as:

   #. Check your non-interactive shell setup on each remote host to
      ensure that it is setting up the ``PATH`` and
      ``LD_LIBRARY_PATH`` properly.
   #.  Check that PRRTE is finding and launching the correct
       version of PRRTE on the remote hosts.
   #. Ensure that you have firewalling disabled between hosts (PRRTE
      opens random TCP and sometimes random UDP ports between
      hosts in a single MPI job).
   #. Try running with the ``plm_base_verbose`` MCA parameter at level
      10, which will enable extra debugging output to see how PRRTE
      launches on remote hosts.  For example:

      .. code-block::

         prterun --prtemca plm_base_verbose 10 --host remotehost hostname``

#. Now run a simple PMIx-based job across multiple hosts that does not
   involve inter-process communications.  The ``hello_c`` program in the
   ``examples`` directory in the PRRTE distribution is a good
   choice.  This verifies that the PMIx subsystem is able to initialize
   and terminate properly.  For example:

   .. code-block::

      shell$ prterun --host remotehost,otherhost hello_c
      Hello, world, I am 0 of 1, (PRRTE VERSION, package: PRRTE jsquyres@example.com Distribution, ident: VERSION, DATE)
      Hello, world, I am 1 of 1, (PRRTE VERSION, package: PRRTE jsquyres@example.com Distribution, ident: VERSION, DATE)

   If you are unable to run simple, non-communication jobs, this
   can indicate that your PRRTE installation is unable to
   initialize properly on remote hosts.  Double check your
   non-interactive login setup on remote hosts.
