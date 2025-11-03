.. _label-quickstart-launching-apps:

Quick start: Launching applications
===================================

Although this section skips many details, it offers examples that will
probably work in many environments.

.. caution:: Note that this section is a "Quick start" |mdash| it does
   not attempt to be comprehensive or describe how to build Open MPI
   in all supported environments.  The examples below may therefore
   not work exactly as shown in your environment.

   Please consult the other sections in this chapter for more details,
   if necessary.

Using ``prterun`` to launch applications
----------------------------------------

PRRTE supports both ``prterun`` and
``prun`` to
launch applications.  For example:

.. code-block:: sh

   shell$ prterun -n 2 mpi-hello-world
   # or
   shell$ prte & prun -n 2 mpi-hello-world
   # or
   shell$ prterun -n 1 mpi-hello-world : -n 1 mpi-hello-world

are all equivalent.  For simplicity, the rest of this documentation
will simply refer to ``prterun``.

Other ``prterun`` options
^^^^^^^^^^^^^^^^^^^^^^^^^

``prterun`` supports the ``--help`` option which provides a usage
message and a summary of the options that it supports.  It should be
considered the definitive list of what options are provided.

Several notable options are:

* ``--hostfile``: Specify a hostfile for launchers (such as the
  ``rsh`` launcher) that need to be told on which hosts to start
  parallel applications.  Note that for compatibility with other
  launchers, *--machinefile* is a synonym for ``--hostfile``.
* ``--host``: Specify a host or list of hosts to run on, including
  support for relative index syntax.
* ``-n``: Indicate the number of processes to start.
* ``--prtemca`` or ``--pmixmca``: Set MCA parameters for either
  PRRTE or the underlying PMIx library.
* ``--wdir DIRECTORY``: Set the working directory of the started
  applications.  If not supplied, the current working directory is
  assumed (or ``$HOME``, if the current working directory does not
  exist on all nodes).
* ``-x ENV_VARIABLE_NAME``: The name of an environment variable to
  export to the parallel application.  The ``-x`` option can be
  specified multiple times to export multiple environment variables to
  the parallel application.

Note that the ``prterun`` command supports a
*large* number of options. Detailed help on any option can be obtained
using the hierarchical help system - e.g., ``prterun --help map-by``.

Launching on a single host
--------------------------

It is common to develop applications on a single laptop or
workstation.  In such simple "single program, multiple data (SPMD)" cases,
use ``prterun`` and
specify how many processes you want to launch via the ``-n``
option:

.. code-block:: sh

   shell$ prterun -n 6 mpi-hello-world
   Hello world, I am 0 of 6 (running on my-laptop))
   Hello world, I am 1 of 6 (running on my-laptop)
   ...
   Hello world, I am 5 of 6 (running on my-laptop)

This starts a six-process parallel application, running six copies
of the executable named ``mpi-hello-world``.

If you do not specify the ``-n`` option, ``prterun`` will
default to launching as many processes as
there are processor cores (not hyperthreads) on the machine.

Launching in a non-scheduled environments (via ``ssh``)
-------------------------------------------------------

In general, PRRTE requires the following to launch and run
applications:

#. You must be able to login to remote nodes non-interactively (e.g.,
   without entering a password or passphrase).
#. PRRTE's daemon executable must be findable (e.g., in your ``PATH``).
#. PRRTE's libraries must be findable (e.g., in your
   ``LD_LIBRARY_PATH``).

``prterun`` accepts a ``--hostfile`` option (and its
synonym, the ``--machinefile`` option) to specify a hostfile containing one
hostname per line:

.. code-block:: sh

   shell$ cat my-hostfile.txt
   node1.example.com
   node2.example.com
   node3.example.com slots=2
   node4.example.com slots=10

The optional ``slots`` attribute tells PRRTE the *maximum* number
of processes that can be allocated to that node.  If ``slots`` is not
provided, PRRTE |mdash| by default |mdash| uses the number of
processor cores (not hyperthreads) on that node.

Assuming that each of the 4 nodes in `my-hostfile.txt` have 16 cores:

.. code-block:: sh

   shell$ prterun --hostfile my-hostfile.txt mpi-hello-world
   Hello world, I am 0 of 44 (running on node1.example.com)
   Hello world, I am 1 of 44 (running on node1.example.com)
   ...
   Hello world, I am 15 of 44 (running on node1.example.com)
   Hello world, I am 16 of 44 (running on node2.example.com)
   Hello world, I am 17 of 44 (running on node2.example.com)
   ...
   Hello world, I am 31 of 44 (running on node2.example.com)
   Hello world, I am 32 of 44 (running on node3.example.com)
   Hello world, I am 33 of 44 (running on node3.example.com)
   Hello world, I am 34 of 44 (running on node4.example.com)
   ...
   Hello world, I am 43 of 44 (running on node4.example.com)

You can see the breakdown of how many processes PRRTE launched on
each node:

* node1: 16, because no ``slots`` was specified
* node2: 16, because no ``slots`` was specified
* node3: 2, because ``slots=2`` was specified
* node2: 10, because ``slots=10`` was specified

Note, however, that not all environments require a hostfile.  For
example, PRRTE will automatically detect when it is running in
batch / scheduled environments (such as Slurm, PBS/Torque, SGE,
LoadLeveler), and will use host information provided by those systems.

Also note that if using a launcher that requires a hostfile and no
hostfile is specified, all processes are launched on the local host.

Launching in scheduled environments
-----------------------------------

In scheduled environments (e.g., in a Slurm job, or PBS/Pro, or LSF,
or any other schedule), the user tells the scheduler how many MPI
processes to launch, and the scheduler decides which hosts to use.
The scheduler then passes both pieces of information (the number of
processes and the hosts to use) to PRRTE.

There are two ways to launch in a scheduled environment.  Nominally,
they both achieve the same thing: they launch processes.  The
main user-observable difference between the two methods is that
``prterun`` has *many* more features than scheduler
direct launchers.

Using PRRTE's ``prterun``
^^^^^^^^^^^^^^^^^^^^^^^^^

When using the full-featured ``prterun`` in a
scheduled environment, there is no need to specify a hostfile or
number of processes to launch.  ``prterun``
will receive this information directly from the scheduler.  Hence, if
you want to launch a job that completely "fills" your scheduled
allocation (i.e., one process for each slot in the scheduled
allocation), you can simply:

.. code-block:: sh

   # Write a script that runs your application
   shell$ cat my-slurm-script.sh
   #!/bin/sh
   # There is no need to specify -n or --hostfile because that
   # information will automatically be provided by Slurm.
   prterun mpi-hello-world

You then submit the ``my-slurm-script.sh`` script to Slurm for
execution:

.. code-block:: sh

   # Use -n to indicate how many processes you want to run.
   # Slurm will pick the specific hosts which will be used.
   shell$ sbatch -n 40 my-slurm-script.sh
   Submitted batch job 1234
   shell$

After Slurm job 1234 completes, you can look at the output file to see
what happened:

.. code-block:: sh

   shell$ cat slurm-1234.out
   Hello world, I am 0 of 40 (running on node37.example.com)
   Hello world, I am 1 of 40 (running on node37.example.com)
   Hello world, I am 2 of 40 (running on node37.example.com)
   ...
   Hello world, I am 39 of 40 (running on node19.example.com)

Note that the Slurm scheduler picked the hosts on which the processes
ran.

The above example shows that simply invoking ``mpirun
mpi-hello-world`` |mdash| with no other CLI options |mdash| obtains
the number of processes to run and hosts to use from the scheduler.

``prterun`` has many more features not described in
this Quick Start section.  For example, while uncommon in scheduled
environments, you can use ``-n`` and/or ``--hostfile`` to launch in
subsets of the overall scheduler allocation.  See the ``prterun``
help system for more details.
