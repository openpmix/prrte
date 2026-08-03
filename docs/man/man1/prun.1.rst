.. _man1-prun:

prun
=====

prun |mdash| launch an application

SYNOPSIS
--------

.. code:: sh

   shell$ prun ...options...

DESCRIPTION
-----------

``prun`` submits a job to the PMIx Reference Runtime Environment
(PRRTE).  The user has control over various distributed virtual
machine (DVM) options.

Much of this same help documentation for this command is also provided
through ``prun --help [topic]``.

.. admonition:: PRRTE Docs TODO
   :class: error

   Need to write this man page.

COMMAND LINE OPTIONS
--------------------

The following command line options are supported:

.. TODO - add in all supported CLI

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

.. TODO - check for deprecated CLI and add those here

EXIT STATUS
-----------

``prun`` reports the outcome of the job it launched:

* **0** if the job completed successfully.

* **the application's exit status**, if a process of the job exited with
  a non-zero status or was killed. As with any launcher, only the low
  eight bits survive, and a job of many processes has only one status to
  report: it is the one recorded for the first process whose failure
  caused the job to be terminated, which is also the process named in the
  error message.

* **a non-zero status derived from the reason the job ended**, when the
  job failed without any process having produced an exit status of its
  own |mdash| a job that could not be mapped or launched, for instance.
  Do not attach meaning to the particular value beyond "not zero".

Note that ``prun`` reports the *job's* status, not its own: a failure to
reach the DVM, or an invalid command line, also exits non-zero.

.. seealso::
   :ref:`prte(1) <man1-prte>`
