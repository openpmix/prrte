Launching with PBS / Torque
===========================

PRRTE supports PBS, PBS Pro, Torque, and other related resource
managers.

Verify PBS/Torque support
-------------------------

PBS support is built when PRRTE is configured ``--with-pbs`` or
``--with-tm``. Starting the daemons as tasks of the PBS job also needs
PBS's TM library, which configure looks for under the directory given
to ``--with-pbs``, with ``pbs-config``, and in the default search paths;
``--with-tm=DIR`` names its location explicitly, and implies
``--with-pbs`` (``--with-tm`` together with ``--without-pbs`` is an
error).

If ``--with-pbs`` is given and the TM library cannot be found, configure
stops with an error rather than build a PRRTE that would quietly launch
its daemons with ``ssh`` inside a PBS job. Either say where TM is with
``--with-tm=DIR``, or configure ``--without-tm`` to build PBS support
without it - PRRTE then reads the allocation from PBS but launches the
daemons with ``ssh``, and PBS does not account for them (see below).

The ``prte_info`` command can be used to determine whether or not an
installed PRRTE includes PBS support:

.. code-block::

   shell$ prte_info | grep -E "plm: tm|ras: pbs"

If it does, you should see lines similar to those below (the version
information varies with the version of PRRTE installed):

.. code-block::

       MCA plm: tm (MCA v2.1.0, API v2.0.0, Component v4.0.0)
       MCA ras: pbs (MCA v2.1.0, API v2.0.0, Component v4.0.0)

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

How the daemons are started
---------------------------

PRRTE starts one daemon on each node of the job other than its own, and
starts each one through PBS's TM interface, so that PBS runs the daemon
as a task of the job: PBS charges the job for the daemon and for every
process it starts, and takes them all down when the job ends or is
deleted. The daemons remain those tasks for the life of the job.

PRRTE itself does not load the TM library, since some builds of it carry
private copies of libraries PMIx is also linked against. The TM calls
are made by a small helper program, ``prte_tm_spawn``, installed with
PRRTE in ``$libexecdir/prte`` and linked against the TM library alone.
It is not meant to be run by hand.

Each daemon must receive a secret key from PRRTE before any other daemon
will talk to it. PBS hands each daemon the environment PRRTE gives it,
and the key travels there, as it does under Slurm: it never appears on
a command line, and the daemon removes it from its environment as soon
as it has read it.

The daemons are started with PRRTE's own ``PATH`` and
``LD_LIBRARY_PATH``, and PBS looks for ``prted`` along that ``PATH`` on
each node. Where PRRTE is installed somewhere else on the other nodes,
use ``--prefix`` to say where.

If a daemon cannot be started - ``prted`` is not found, or PBS refuses -
PRRTE reports which node and why, and the launch fails.

If the helper program is missing, PRRTE says so and starts the daemons
with ``ssh`` instead, as it would outside PBS. Daemons started that way
are not PBS tasks, so PBS does not charge the job for the remote nodes'
CPU time, and ``ssh`` must work between the job's nodes without a
password. To launch with ``ssh`` inside a PBS job anyway, use
``--prtemca plm ssh``. To see which launcher was used, run with
``--prtemca plm_base_verbose 1``.

.. warning:: Do not modify ``$PBS_NODEFILE``!

   We've had reports from some sites that system administrators modify
   the ``$PBS_NODEFILE`` in each job according to local policies.
   This will currently cause PRRTE to behave in an unpredictable
   fashion.  As long as no new hosts are added to the hostfile, it
   *usually* means that PRRTE will incorrectly map processes to
   hosts, but in some cases it can cause PRRTE to fail to launch
   processes altogether.
