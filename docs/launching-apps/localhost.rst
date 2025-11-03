Launching only on the local node
================================

It is common to develop applications on a single workstation or
laptop, and then move to a larger parallel / HPC environment once the
application is ready.

PRRTE supports running multi-process jobs on a single machine.
In such cases, you can simply avoid listing a hostfile or remote
hosts, and simply list a number of processes to launch.  For
example:

.. code-block:: sh

   shell$ prterun -n 6 mpi-hello-world
   Hello world, I am 0 of 6 (running on my-laptop))
   Hello world, I am 1 of 6 (running on my-laptop)
   ...
   Hello world, I am 5 of 6 (running on my-laptop)

If you do not specify the ``-n`` option, ``prterun`` will default to
launching as many processes as there are processor cores (not
hyperthreads) on the machine.
