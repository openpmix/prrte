.. _man1-prte_info:

prte_info
=========

.. include_body

prte_info |mdash| Display information about the PRRTE installation

SYNOPSIS
--------

``prte_info [options]``


DESCRIPTION
-----------

``prte_info`` provides detailed information about the PRRTE
installation. It can be useful for at least three common scenarios:

#. Checking local configuration and seeing how PRRTE was installed.

#. Submitting bug reports / help requests to the PRRTE community
   (see :doc:`Getting help </getting-help>`).

#. Seeing a list of installed PRRTE plugins and querying what MCA
   parameters they support.


OPTIONS
-------

``prte_info`` accepts the following options:

* ``-h`` | ``--help <arg0>``: Show help message. If the optional
  argument is not provided, then a generalized help message similar
  to the information provided here is returned. If an argument is
  provided, then a more detailed help message for that specific
  command line option is returned.

* ``-v`` | ``--verbose``: Enable debug output.

* ``-V`` | ``--version``: Print version and exit.

* ``-a`` | ``--all``: Show all configuration options and MCA
  parameters.

* ``--arch``: Show architecture on which PRRTE was compiled.

* ``-c`` | ``--config``: Show configuration options

* ``--hostname``: Show the hostname on which PRRTE was configured
  and built.

* ``--internal``: Show internal MCA parameters (not meant to be
  modified by users).

* ``--param <arg0>:<arg1>,<arg2>``: Show MCA parameters.  The first
  parameter is the framework (or the keyword "all"); the second parameter
  is a comma-delimited list of specific component names (if only <arg0>
  is given, then all components will be reported).

* ``--path <type>``: Show paths that PRRTE was configured
  with. Accepts the following parameters: ``all``, ``prefix``, ``bindir``,
  ``libdir``, ``incdir``, ``pkglibdir``, ``sysconfdir``.

* ``--pretty-print``: When used in conjunction with other parameters, the output is
  displayed in "prettyprint" format (default)

* ``--parsable``: When used in conjunction with other parameters, the output is
  displayed in a machine-parsable format

* ``--parseable``: Synonym for ``--parsable``

* ``--color <hue>``: Control color coding: auto (default), never, always

* ``--show-failed``: Show the components that failed to load along with the reason why they failed

* ``--selected-only``: Show only variables from selected components.


EXIT STATUS
-----------

Returns 0 if successful, non-zero if an error is encountered

EXAMPLES
--------

Examples of using this command.

Show the default output of options and listing of installed
components in a human-readable / prettyprint format:

.. code-block::

   prte_info

Show the default output of options and listing of installed components
in a machine-parsable format:

.. code-block::

   prte_info --parsable

Show the MCA parameters of the "lsf" RMAPS component in a
human-readable / prettyprint format:

.. code-block::

   prte_info --param rmaps lsf

Show the "bindir" that PRRTE was configured with:

.. code-block::

   prte_info --path bindir

Show the version of PRRTE version numbers in a prettyprint format:

.. code-block::

   prte_info --version

Show *all* information about the PRRTE installation, including all
components that can be found, all the MCA parameters that they support,
versions of PRRTE and the components, etc.:

.. code-block::

   prte_info --all

.. seealso::
   :ref:`prte(1) <man1-prte>`
