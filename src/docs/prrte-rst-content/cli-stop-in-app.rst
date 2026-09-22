.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Include the ``PMIX_DEBUG_STOP_IN_APP`` attribute in the application's
job info directing that the processes stop at an
application-determined point pending release. The directive applies to
all processes in the job. This is the same as ``--rtos stop-in-app``.

An optional argument names the one breakpoint at which they are to
stop |mdash| e.g., ``--stop-in-app=mpi-init``. PRRTE cannot know where
any given breakpoint lives; all it can do is pass the name to the
application in the ``PMIX_BREAKPOINT`` environment variable and then
wait for the "ready for debug" event the application generates when it
gets there. It is therefore up to the application to recognize the
name and stop in the corresponding place. Given without an argument,
the processes stop at whichever such place they reach first.

Since the argument is read as a boolean when it spells one, a
breakpoint cannot be named ``true``, ``false``, or any other spelling of
a truth value.
