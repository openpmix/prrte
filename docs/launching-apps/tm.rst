Launching with PBS / Torque
===========================

PRRTE supports PBS, PBS Pro, Torque, and other related resource
managers.

Verify PBS/Torque support
-------------------------

The ``prte_info`` command can be used to determine whether or not an
installed PRRTE includes Torque/PBS Pro support:

.. code-block::

   shell$ prte_info | grep ras

If the PRRTE installation includes support for PBS/Torque, you
should see a line similar to that below. Note the MCA version
information varies depending on which version of PRRTE is
installed.

.. code-block::

       MCA ras: pbs (MCA v2.1.0, API v2.1.0, Component v5.0.0)

Launching
---------

When properly configured, PRRTE obtains both the list of hosts and
how many processes to start on each host from Torque / PBS Pro
directly.  Hence, it is unnecessary to specify the ``--hostfile``,
``--host``, or ``-n`` options to ``prterun``.  PRRTE will use
PBS/Torque-native mechanisms to launch and kill processes (``ssh`` is
not required where PBS provides ``pbs_tmrsh``).  See :ref:`below
<launching-pbs-tmrsh-label>` for what that requires.

For example:

.. code-block:: sh

   # Allocate a PBS job with 4 nodes
   shell$ qsub -I -lnodes=4

   # Now run a job on all the nodes allocated by PBS/Torque
   shell$ prterun mpi-hello-world

This will run the application processes on the nodes that were allocated by
PBS/Torque.  Or, if submitting a script:

.. code-block:: sh

   shell$ cat my_script.sh
   #!/bin/sh
   prterun mpi-hello-world
   shell$ qsub -l nodes=4 my_script.sh

.. warning:: Do not modify ``$PBS_NODEFILE``!

   We've had reports from some sites that system administrators modify
   the ``$PBS_NODEFILE`` in each job according to local policies.
   This will currently cause PRRTE to behave in an unpredictable
   fashion.  As long as no new hosts are added to the hostfile, it
   *usually* means that PRRTE will incorrectly map processes to
   hosts, but in some cases it can cause PRRTE to fail to launch
   processes altogether.

.. _launching-pbs-tmrsh-label:

How the daemons are started
---------------------------

Inside a PBS job (``PBS_ENVIRONMENT`` and ``PBS_JOBID`` are set), the
``ssh`` launcher starts one PRRTE daemon on each of the job's other
nodes with ``pbs_tmrsh``, which PBS Pro and OpenPBS provide and which is found
when PRRTE is configured ``--with-pbs``.  Where there is no ``pbs_tmrsh``
the daemons are started with ``ssh``, as outside a job.

``pbs_tmrsh`` is not a remote shell: PBS executes the command it is
given directly, with no standard input and with the job's environment
instead of the caller's.  PRRTE therefore names the shell itself
(``/bin/sh -c``), and hands each daemon the DVM's key through a file:

* The file is created in your home directory, readable by you alone,
  under a name that cannot be guessed, and is removed when the last
  ``pbs_tmrsh`` has returned (when ``prterun`` ends, if the daemons
  were kept attached with ``--debug-daemons``, ``--debug-daemons-file``
  or ``--leave-session-attached``).
  A ``prterun`` that is killed outright can leave a
  ``.prte-dvm-key.*`` file behind; it is of no use once that
  ``prterun`` is gone and can be deleted.
* While it exists, the key is stored on that file system and travels
  over it.  Every other way PRRTE hands the key to a daemon avoids
  both; ``pbs_tmrsh`` leaves no other way.  A site that cannot accept
  this should start the daemons with ``ssh`` (below).
* **That directory must be visible, under the same name, on every node
  of the job.**  If your home directory is not, name one that is with
  the ``plm_ssh_tmrsh_keydir`` MCA parameter:

  .. code-block:: sh

     shell$ prterun --prtemca plm_ssh_tmrsh_keydir /shared/scratch/$USER ...

* The directory must be your own, with nobody else able to write to
  it, or have the sticky bit set and belong to you or to root.  On each
  of the other nodes the file is read only if it is yours and not a
  link.  If the nodes do not share the directory, the daemons stop with
  "did not receive its DVM key".
* The shell PBS starts reads no startup files and has the job's
  environment.  ``prterun`` passes its own installation prefix along
  when it is run by its full path, given ``--prefix``, or built with
  ``--enable-prte-prefix-by-default``; otherwise ``prted`` and its
  libraries must be found through the job's ``PATH`` and
  ``LD_LIBRARY_PATH``.
* Daemons are all started from the node ``prterun`` runs on; the tree
  launch the ``ssh`` launcher otherwise uses is turned off.
* Unless kept attached (above), each daemon detaches from the PBS task
  that started it.  Whether PBS
  still accounts for it and for the processes it starts depends on how
  the site tracks jobs.

PBS may take the ``pbs_tmrsh`` calls of a job one at a time.  Where it
does, a launch takes time in proportion to the number of nodes: on one
PBS Professional 2022.1 system each call took about two seconds, and
starting daemons on seven other nodes took fourteen.  Starting them
with ``ssh`` does not have that cost, if the nodes allow it.

If no directory is shared between the nodes, or to start the daemons
with ``ssh`` for any other reason, turn ``pbs_tmrsh`` off:

.. code-block:: sh

   shell$ prterun --prtemca plm_ssh_disable_tmrsh 1 ...
