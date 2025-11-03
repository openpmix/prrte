Launching with SSH
==================

When launching jobs in a non-scheduled environment, ``ssh``
is typically used to launch commands on remote nodes.  As listed in
the :doc:`quick start section </launching-apps/quickstart>`,
successfully launching MPI applications with ``ssh`` requires the
following:

#. You must be able to non-interactively login |mdash| without
   entering a password or passphrase |mdash| to all remote nodes from
   all remotes nodes.
#. PRRTE's daemon executablesmust be findable (e.g., in your ``PATH``).
#. PRRTE's libraries must be findable (e.g., in your
   ``LD_LIBRARY_PATH``).

Specifying the hosts for a job
------------------------------

There are three mechanisms for specifying the hosts that an job will run on:

#. The ``--hostfile`` option to ``prterun``.

   Use this option to specify a list of hosts on which to run.  Note
   that for compatibility with other launchers,
   ``--machinefile`` is a synonym for ``--hostfile``.

#. The ``--host`` option to ``prterun``.

   This option can be used to specify a list of hosts on which to run
   on the command line.

#. Running in a scheduled environment.

   If you are running in a scheduled environment (e.g., in a Slurm,
   Torque, or LSF job), PRRTE will automatically get the lists of
   hosts from the scheduler.  See the next subsections for details about
   launching jobs in supported scheduled environements.

.. important:: The specification of hosts using any of the above
               methods has nothing to do with the network interfaces
               that are used for application traffic.  The list of hosts is
               *only* used for specifying which hosts on which to
               launch processes.

Non-interactive ``ssh`` logins
------------------------------

SSH keys must be setup such that the following can be executed without
being prompted for password or passphrase:

.. code-block:: sh

   shell$ ssh othernode echo hello
   hello
   shell$

Consult instructions and tutorials from around the internet to learn
how to setup SSH keys.  Try Google search terms like "passwordless
SSH" or "SSH key authentication".

For simplicity, it may be desirable to configure your SSH keys
without passphrases.  This adds some risk, however (e.g., if your SSH
keys are compromised).  But it simplifies your SSH setup because you
will not need to use ``ssh-agent``.  Evaluate the risk level you are
comfortable with.

.. important:: PRRTE uses a tree-based pattern to launch processes
   on remote nodes.  This means that PRRTE must be able to
   non-interactively login |mdash| without being prompted for password
   or passphrase |mdash| *to any node* in the host list *from any
   node* in the host list.

   It may *not* be sufficient to only setup an SSH key from the node
   where you are invoking ``prterun`` to all other
   nodes.

If you have a shared ``$HOME`` filesystem between your nodes, you can
setup a single SSH key that is used to login to all nodes.

Finding the PRRTE daemon executable and libraries
-------------------------------------------------

Once PRRTE is able to use ``ssh`` to invoke executables on a remote
node, it must be able to find its daemon executable and shared
libraries on that remote node.

If PRRTE is installed in a system-level folder (e.g., in
``/usr/bin``), PRRTE will likely be able to find its daemon
and libraries on the remote node with no additional assistance.

If, however, PRRTE is installed into a path that is not searched by
default, you will need to provide assistance so that PRRTE can find
its daemon and libraries.

.. important:: For simplicity, it is *strongly* recommended that you
   install PRRTE in the same location on all nodes in your job.

You can do this in one of two ways.

Use "prefix" behavior
^^^^^^^^^^^^^^^^^^^^^

.. note:: "Prefix" behavior is only available with ``prterun``; it is not
   available via resource manager direct
   launch mechanisms.  However, this section is about using ``ssh`` to
   launch jobs, which means that there is no resource manager, and
   therefore there is no direct launch mechanism available.

When "prefix" behavior is enabled, PRRTE will automatically set the
``$PATH`` and ``$LD_LIBRARY_PATH`` on remote nodes before executing
remote commands.

.. important:: PRRTE assumes that the installation ``prefix``,
   ``bindir``, and ``libdir`` are the same on the remote node as they
   are on the local node.  If they are not, *then you should not use
   the "prefix" behavior.*

You can enable "prefix" behavior in one of three ways:

#. Use an absolute path name to invoke ``prterun``.

   .. code-block:: sh

      shell$ $HOME/my-prrte/bin/prterun --hostfile my-hostfile.txt mpi-hello-world

   Simply using the absolute path name to ``prterun`` tells PRRTE to enable "prefix" mode.


#. Use the ``--prefix`` option to ``prterun``.

  .. code-block:: sh

     shell$ $HOME/my-prrte/bin/prterun --hostfile my-hostfile.txt \
         --prefix $HOME/my-prrte \
         mpi-hello-world

   The ``--prefix`` option takes a single argument: the prefix path to
   use for the bindir and libdir on the remote node.

#. Configure PRRTE with ``--enable-prterun-prefix-by-default``.

   If PRRTE is built this way, ``prterun`` will
   always enable "prefix" behavior.

Set the ``PATH`` and ``LD_LIBRARY_PATH`` in your shell startup files
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Consider the case where PRRTE was configured with:

.. code-block:: sh

   shell$ ./configure --prefix=$HOME/my-prrte ...

In this cause, PRRTE will be installed into ``$HOME/my-prrte``.
This path is almost certainly not in any system-default search paths,
so it must be added to the ``$PATH`` and ``$LD_LIBRARY_PATH``
environment variables.

Specifically: the goal is that the following non-interactive commands
must be able to execute without error:

.. code-block:: sh

   # First, ensure that this command returns the correct prte_info
   # instance (i.e., $HOME/my-prrte/bin/prte_info).
   shell$ ssh remotenode which prte_info
   /home/myusername/my-prrte/bin/prte_info

   # Next, ensure that you can run that prte_info command without
   # error
   shell$ ssh remotenode prte_info

   # ... lots of output ...

Ensure that you do not see any errors about libraries that cannot be
found.

All shells have some kind of script file that is executed at login
time perform environmental setup tasks.  This startup file is the one
that needs to be edited to:

#. Add PRRTE's daemon executable path (which is likely ``$prefix/bin``, or
   ``$HOME/my-prrte/bin`` in this example) to the ``$PATH``
   environment variable.
#. Add PRRTE's library path (which is likely ``$prefix/lib``, or
   ``$HOME/my-prrte/lib`` in this example) to the
   ``$LD_LIBRARY_PATH`` environment variable.

You probably want to add PRRTE's libraries to the *front* of
``$PATH`` and ``$LD_LIBRARY_PATH`` to ensure that this PRRTE
installation's files are found *first*.

Consult the manual page for your shell for specific details (some
shells are picky about the permissions of the startup file, for
example).  The list below contains some common shells and the startup
files that they read/execute upon login:

.. list-table::
   :header-rows: 1

   * - Shell
     - Non-interactive login
     - Interactive login

   * - ``bash`` or ``zsh``
     - ``$HOME/.bashrc`` if it exists.
     - #. ``$HOME/.bash_profile`` if it exists, or
       #. ``$HOME/.bash_login`` if it exists, or
       #. ``$HOME/.profile`` if it exists (in that order).

       Note that some Linux distributions automatically come
       with ``$HOME/.bash_profile`` scripts for users that
       automatically execute ``$HOME/.bashrc`` as well. Consult the
       bash man page for more information.

   * - ``sh``
     - This shell does not execute any file automatically, so PRRTE
       will execute the ``$HOME/.profile`` script before invoking PRRTE
       executables on remote nodes
     - ``$HOME/.profile``

   * - ``csh``
     - ``$HOME/.cshrc``
     - ``$HOME/.cshrc`` followed by ``$HOME/.login``

   * - ``tcsh``
     - #. ``$HOME/.tcshrc`` if it exists, or
       #. ``$HOME/.cshrc`` if it does not
     - #. ``$HOME/.tcshrc`` if it exists, or
       #. ``$HOME/.cshrc`` if it does not

       Afterwards, execute ``$HOME/.login``
