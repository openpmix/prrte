.. _man1-ompi-prterun:

ompi-prterun
============

ompi-prterun |mdash| launch an application with a default DVM

SYNOPSIS
--------

.. code:: sh

   shell$ ompi-prterun ...options...

DESCRIPTION
-----------

``ompi-prterun`` submits a job to the PMIx Reference Runtime Environment
(PRRTE).  A default set of distributed virtual
machine (DVM) options are used; use :ref:`ompi-prun(1) <man1-ompi-prun>` if you
wish to utilize specific DVM options.

Much of this same help documentation for this command is also provided
through ``ompi-prun --help [topic]``.

.. admonition:: PRRTE Docs TODO
   :class: error

   Need to write this man page.

COMMAND LINE OPTIONS
--------------------

.. TODO - add in all supported CLI


DEPRECATED COMMAND LINE OPTIONS
-------------------------------

.. TODO - check for deprecated CLI and add those here


EXIT STATUS
-----------

Description of the various exit statuses of this command.

.. seealso::
   :ref:`ompi-prte(1) <man1-ompi-prte>`
