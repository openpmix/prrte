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

.. include:: /prrte-rst-content/prterun-all-cli.rst

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

The following command line options are deprecated and may disappear in
a future version of PRRTE:

.. include:: /prrte-rst-content/prterun-all-deprecated.rst

.. seealso::
   :ref:`prte(1) <man1-prte>`
