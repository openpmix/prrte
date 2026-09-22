.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Timeout the job if execution is not complete after the specified number
of seconds. The value must be a non-negative integer; zero means no
timeout. If this option is not given, the value of the
``MPIEXEC_TIMEOUT`` environment variable, if set, is used instead. See
also ``--report-state-on-timeout`` and ``--get-stack-traces``.
