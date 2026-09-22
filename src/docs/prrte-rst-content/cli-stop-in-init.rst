.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Include the ``PMIX_DEBUG_STOP_IN_INIT`` attribute in the application's
job info directing that the processes stop in ``PMIx_Init`` pending
release. The directive applies to all processes in the job. This is the
same as ``--rtos stop-in-init``.
