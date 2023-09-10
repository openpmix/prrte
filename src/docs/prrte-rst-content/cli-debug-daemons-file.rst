.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

Debug daemon output is enabled and all output from the daemons is
redirected into files with names of the form:

.. code::

   output-prted-<daemon-nspace>-<nodename>.log

These names avoid conflict on shared file systems. The files are
located in the top-level session directory assigned to the DVM.
