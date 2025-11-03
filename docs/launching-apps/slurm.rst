Launching with Slurm
====================

PRRTE supports two modes of launching parallel jobs under
Slurm:

#. Using PRRTE's full-features ``prterun`` launcher.
#. Using Slurm's "direct launch" capability.

Unless there is a strong reason to use ``srun`` for direct launch, the
PRRTE team recommends using ``prterun`` for launching under Slurm jobs.

Using ``prterun``
-----------------

When ``prterun`` is launched in a Slurm job, ``prterun`` will
automatically utilize the Slurm infrastructure for launching and
controlling the individual processes.
Hence, it is unnecessary to specify the ``--hostfile``,
``--host``, or ``-n`` options to ``prterun``.

.. note:: Using ``prterun`` is the recommended method for launching
   applications in Slurm jobs.

   ``prterun``'s Slurm support should always be available, regardless
   of how PRRTE or Slurm was installed.

For example:

.. code-block:: sh

   # Allocate a Slurm job with 4 slots
   shell$ salloc -n 4
   salloc: Granted job allocation 1234

   # Now run an Open MPI job on all the slots allocated by Slurm
   shell$ prterun mpi-hello-world

This will run the 4 processes on the node(s) that were allocated
by Slurm.

Or, if submitting a script:

.. code-block:: sh

   shell$ cat my_script.sh
   #!/bin/sh
   prterun mpi-hello-world
   shell$ sbatch -n 4 my_script.sh
   srun: jobid 1235 submitted
   shell$

Similar to the ``salloc`` case, no command line options specifying
number of processes were necessary, since PRRTE will obtain
that information directly from Slurm at run time.

