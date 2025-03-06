.. -*- rst -*-

   Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Prefix to be used by a PRRTE executable to look for its PMIx installation
on remote nodes. This is the location of the top-level directory for the
installation. If the installation has not been moved, it would be the
value given to "--prefix" when the installation was configured.

Note that PRRTE cannot determine the exact name of the library subdirectory
under this location. For example, some systems will call it "lib" while others
call it "lib64". Accordingly, PRRTE will use the library subdirectory name
of the PMIx installation used to build PRRTE.

