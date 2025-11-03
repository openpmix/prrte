.. _label-running-applications:

Launching applications
======================

PRRTE can launch processes in a wide variety of environments,
but they can generally be broken down into two categories:

#. Scheduled environments: these are systems where a resource manager
   and/or scheduler are used to control access to the compute nodes.
   Popular resource managers include Slurm, PBS/Pro/Torque, and LSF.
#. Non-scheduled environments: these are systems where resource
   managers are not used.  Launches are typically local (e.g., on a
   single laptop or workstation) or via ``ssh`` (e.g., across a small
   number of nodes).

PRRTE provides two commands for starting applications:

#. ``prun`` - submits the specified application to an existing persistent DVM
   for execution. The DVM continues execution once the application has
   completed. The prun command will remain active until the application
   completes. All application and error output will flow through prun.
#. ``prterun`` - starts a DVM instance and submits the specified application
   to it for execution. The DVM is terminated once the application completes.
   All application and error output will flow through prterun.

The rest of this section usually refers only to ``prterun``, even though the
same discussions also apply to ``prun`` because the command line syntax
is identical.


.. toctree::
   :maxdepth: 1

   quickstart
   prerequisites
   scheduling

   localhost
   ssh
   slurm
   lsf
   tm
   gridengine

   unusual
   troubleshooting
