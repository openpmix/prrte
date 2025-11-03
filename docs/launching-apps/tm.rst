Launching with PBS / Torque
===========================

PRRTE supports PBS, PBS Pro, Torque, and other related resource
managers.

Verify PBS/Torque support
-------------------------

The ``prte_info`` command can be used to determine whether or not an
installed Open MPI includes Torque/PBS Pro support:

.. code-block::

   shell$ prte_info | grep ras

If the PRRTE installation includes support for PBS/Torque, you
should see a line similar to that below. Note the MCA version
information varies depending on which version of Open MPI is
installed.

.. code-block::

       MCA ras: tm (MCA v2.1.0, API v2.0.0, Component v3.0.0)

Launching
---------

When properly configured, PRRTE obtains both the list of hosts and
how many processes to start on each host from Torque / PBS Pro
directly.  Hence, it is unnecessary to specify the ``--hostfile``,
``--host``, or ``-n`` options to ``prterun``.  PRRTE will use
PBS/Torque-native mechanisms to launch and kill processes (``ssh`` is
not required).

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
