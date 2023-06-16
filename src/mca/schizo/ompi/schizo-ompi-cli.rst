.. -*- rst -*-

   Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
   Copyright (c) 2022      Cisco Systems, Inc.  All rights reserved.
   Copyright (c) 2022      IBM Corporation.  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.
   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

Fault Tolerance Options (if enabled)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``--enable-recovery``: Enable recovery from process failure (Default
  = disabled)
* ``--max-restarts``: Max number of times to restart a failed process
* ``--disable-recovery``: Disable recovery (resets all recovery
  options to off)
* ``--continuous``: Job is to run until explicitly terminated
* ``--with-ft``: Specify the type(s) of error handling that the
  application will use.

MPI Options
^^^^^^^^^^^

* ``--initial-errhandler``: Specify the initial error handler that is
  attached to predefined communicators during the first MPI call.
* ``--display-comm``: Display table of communication methods between
  MPI_COMM_WORLD ranks during MPI_Init
* ``--display-comm-finalize``: Display table of communication methods
  between ranks during MPI_Finalize
* ``--soft``: This option does nothing, but is mandated by the MPI
  standard
* ``--arch <filename>``: This option does nothing, but is mandated by
  the MPI standard
* ``--file <filename>``: This option does nothing, but is mandated by
  the MPI standard
