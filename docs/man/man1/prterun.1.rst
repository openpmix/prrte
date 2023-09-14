.. _man1-prterun:

prterun
========

prterun |mdash| launch an application with a default DVM

SYNOPSIS
--------

.. code:: sh

   shell$ prterun ...options...

DESCRIPTION
-----------

``prterun`` submits a job to the PMIx Reference Runtime Environment
(PRRTE).  A default set of distributed virtual
machine (DVM) options are used; use :ref:`prun(1) <man1-prun>` if you
wish to utilize specific DVM options.

Much of this same help documentation for this command is also provided
through ``prun --help [topic]``.

.. admonition:: PRRTE Docs TODO
   :class: error

   Need to write this man page.

COMMAND LINE OPTIONS
--------------------

.. JMS I suspect we don't want to include prterun-all-cli.rst here --
   I think only a subset of those commands are available for prun.

.. include:: /prrte-rst-content/prterun-all-cli.rst

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

.. JMS I suspect we don't want to include prterun-all-deprecated.rst
   here -- I think only a subset of those commands are available for
   prun.

.. include:: /prrte-rst-content/prterun-all-deprecated.rst

EXIT STATUS
-----------

Description of the various exit statuses of this command.

.. seealso::
   :ref:`prte(1) <man1-prte>`
