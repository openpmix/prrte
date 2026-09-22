.. _man1-prte_info:

prte_info
=========

.. include_body

prte_info |mdash| Display information about the PRRTE installation

SYNOPSIS
--------

.. code:: sh

   prte_info [options]

DESCRIPTION
-----------

``prte_info`` provides detailed information about the PRRTE
installation. It can be useful for at least three common scenarios:

#. Checking local configuration and seeing how PRRTE was installed.

#. Submitting bug reports / help requests to the PRRTE community
   (see :doc:`Getting help </getting-help>`).

#. Seeing a list of installed PRRTE plugins and querying what MCA
   parameters they support.

Given no options, ``prte_info`` shows the PRRTE version, its
installation prefix, the architecture it was built for, a summary of
its configuration, and the version of every installed component.

``prte_info`` is also installed as ``prte-info``. ``prte_info --help
<option>`` prints the full description of an option.

OPTIONS
-------

.. rubric:: General options

``-h`` | ``--help [<option>]``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Print the list of options, or the full help for the named option.

``-v`` | ``--verbose``
^^^^^^^^^^^^^^^^^^^^^^

Enable debug output.

``-V`` | ``--version``
^^^^^^^^^^^^^^^^^^^^^^

Print the version of the ``prte_info`` tool itself and exit. See
``--show-version`` for the versions of PRRTE and its components.

.. rubric:: MCA parameters

``--prtemca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-prtemca.rst

``--pmixmca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmixmca.rst

.. rubric:: What to show

``-a`` | ``--all``
^^^^^^^^^^^^^^^^^^

Show all configuration options and MCA parameters.

``--arch``
^^^^^^^^^^

Show architecture on which PRRTE was compiled.

``-c`` | ``--config``
^^^^^^^^^^^^^^^^^^^^^

Show configuration options used to configure PRRTE.

``--hostname``
^^^^^^^^^^^^^^

Show the hostname upon which PRRTE was configured and built.

``--param <framework>[:<component>[,<component>...]]``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Show the MCA parameters of a framework. Given only a framework name
(e.g., ``--param rmaps``), every parameter of the framework and of all
its components is shown; a comma-delimited list of component names
after a colon (e.g., ``--param rmaps:ppr,round_robin``) restricts the
output to those components. The keyword ``all``, given either as
``--param all`` or as the component list, selects everything.

The framework and its components are one argument: ``--param rmaps
ppr`` is refused, because ``ppr`` is taken as a stray argument. The
option may be repeated to show several frameworks.

``--params <framework>[:<component>[,<component>...]]``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Synonym for ``--param``.

``--internal``
^^^^^^^^^^^^^^

Show internal MCA parameters (i.e., parameters not meant to be
modified by users).

``-t`` | ``--type <type>``
^^^^^^^^^^^^^^^^^^^^^^^^^^

Show MCA parameters of the type specified in the parameter. Accepts
the following parameters: ``unsigned_int``, ``unsigned_long``,
``unsigned_long_long``, ``size_t``, ``string``, ``version_string``,
``bool``, and ``double``.

``--path <type>``
^^^^^^^^^^^^^^^^^

Show the paths with which PRRTE was configured. Accepts ``all`` or any
of: ``prefix``, ``exec_prefix``, ``bindir``, ``sbindir``, ``libdir``,
``incdir``, ``mandir``, ``pkglibdir``, ``libexecdir``,
``datarootdir``, ``datadir``, ``sysconfdir``, ``sharedstatedir``,
``localstatedir``, ``infodir``, ``pkgdatadir``, ``pkgincludedir``. The
option may be repeated to show several paths.

``--show-version [<what> [<part>]]``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Show the version of PRRTE or of some of its components. ``<what>`` is
one of:

* ``prte`` or ``all``: PRRTE's own version plus that of every component

* ``<framework>``: the version of every component in that framework

* ``<framework>:<component>``: the version of that one component

The optional ``<part>`` selects which part of the version to show:
``full`` (the default), ``major``, ``minor``, ``release``, ``greek``,
or ``repo``. Given with no argument, the option shows PRRTE's version
and every component's.

Note that this is distinct from ``-V``/``--version``, which only prints
the version of the ``prte_info`` tool itself.

``--include-pmix``
^^^^^^^^^^^^^^^^^^

Include PMIx params and build info in output.

``--show-failed``
^^^^^^^^^^^^^^^^^

Show the components that failed to load, along with the reason why
they failed.

``--selected-only``
^^^^^^^^^^^^^^^^^^^

Show only variables from selected (active) components.

.. rubric:: Output format

``--pretty-print``
^^^^^^^^^^^^^^^^^^

When used in conjunction with other parameters, the output is
displayed in "pretty-print" format (default).

``--parsable`` | ``--parseable``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When used in conjunction with other parameters, the output is
displayed in a machine-parsable format.

``--color <hue>``
^^^^^^^^^^^^^^^^^

Control color coding of the output: ``auto`` (the default |mdash|
color only when stdout is a terminal), ``never``, or ``always``.

EXIT STATUS
-----------

Returns 0 if successful, non-zero if an error is encountered.

EXAMPLES
--------

Show the default output of options and listing of installed
components in a human-readable / prettyprint format:

.. code-block::

   prte_info

Show the default output of options and listing of installed components
in a machine-parsable format:

.. code-block::

   prte_info --parsable

Show the MCA parameters of the ``ppr`` and ``round_robin`` RMAPS
components in a human-readable / prettyprint format:

.. code-block::

   prte_info --param rmaps:ppr,round_robin

Show the ``bindir`` that PRRTE was configured with:

.. code-block::

   prte_info --path bindir

Show the versions of PRRTE and all of its components:

.. code-block::

   prte_info --show-version

Show *all* information about the PRRTE installation, including all
components that can be found, all the MCA parameters that they support,
versions of PRRTE and the components, etc.:

.. code-block::

   prte_info --all

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prte(5) <man5-prte>`
