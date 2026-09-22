.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

A job that is not simply terminated when one of its processes fails
|mdash| one given the ``RECOVERABLE`` or ``CONTINUOUS`` runtime option
|mdash| can ask to be told about such failures with the
``NOTIFYERRORS`` runtime option. PRRTE then delivers a PMIx event to
each remaining process in the job whenever one of its processes fails.
The event identifies the failed process in the
``PMIX_EVENT_AFFECTED_PROC`` attribute and, where the process reported
one, its exit status in the ``PMIX_EXIT_CODE`` attribute. A process
captures these events using the ``PMIx_Register_event_handler`` API.
The event codes are:

* ``PMIX_ERR_PROC_KILLED_BY_CMD`` |mdash| the process was ordered to die

* ``PMIX_ERR_PROC_ABORTED_BY_SIG`` |mdash| the process was terminated
  by a signal

* ``PMIX_ERR_PROC_TERM_WO_SYNC`` |mdash| the process called
  ``PMIx_Init`` and then terminated without calling ``PMIx_Finalize``

* ``PMIX_ERR_PROC_REQUESTED_ABORT`` |mdash| the process called
  ``abort``

* ``PMIX_ERR_PROC_KILLED_BY_RELEASE`` |mdash| the process was killed
  because the node it was running on was released from the DVM

* ``PMIX_ERR_EXIT_NONZERO_TERM`` |mdash| the process exited with a
  non-zero status. This is only reported when the
  ``ERROR-NONZERO-STATUS`` runtime option is also set, since otherwise
  such an exit is not an error.

Tools that start jobs can also register for job-level events, among
them ``PMIX_EVENT_JOB_START``, ``PMIX_LAUNCH_COMPLETE``,
``PMIX_READY_FOR_DEBUG`` and ``PMIX_EVENT_JOB_END``.

See the "Notifications" documentation for a full description of the
events PRRTE generates.
