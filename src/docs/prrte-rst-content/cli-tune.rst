.. -*- rst -*-

   Copyright (c) 2022-2026  Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Comma-delimited list of one or more files containing MCA parameters for
tuning DVM and/or application operations. The option may be given more
than once. A file named by a relative path is looked for first in the
current directory and then among the parameter sets installed with
PRRTE.

Syntax in the file is:

.. code::

   param = value

with one parameter per line. Empty lines and lines beginning with the
``#`` character are ignored, as is any whitespace around the ``=``
character. Quotes around the value are removed.

Each parameter is a *generic* MCA parameter, so it is treated exactly
like ``--mca param value``: it is applied to PRRTE if it belongs to a
PRRTE framework, and otherwise to PMIx. A parameter that belongs to
neither is an error, as is a parameter given twice with different
values. A parameter given explicitly on the command line
(``--prtemca``, ``--pmixmca``) overrides the same parameter in a tune
file.
