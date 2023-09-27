.. -*- rst -*-

   Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

[bogus section]

This section is not used by PRTE code.  But we have to put a RST
section title in this file somewhere, or Sphinx gets unhappy.  So we
put it in a section that is ignored by PRTE code.

Hello, world
------------

[usage]

%s (%s) %s

Usage: ``%s [OPTION]...``

Provide detailed information on your PRRTE installation.

The following list of command line options are available. Note that
more detailed help for any option can be obtained by adding that
option to the help request as ``--help <option>``.

.. list-table::
   :header-rows: 1
   :widths: 20 45

   * - Option
     - Description

   * - ``-a`` | ``--all``
     - Show all configuration options and MCA parameters

   * - ``--arch``
     - Show architecture on which PRRTE was compiled

   * - ``-c`` | ``--config``
     - Show configuration options

   * - ``-h`` | ``--help``
     - This help message

   * - ``--hostname``
     - Show the hostname on which PRRTE was configured and built

   * - ``--internal``
     - Show internal MCA parameters (not meant to be modified by users)

   * - ``--param <framework>:<component1>,<component2>``
     - Show MCA parameters.  The first parameter is the framework (or
       the keyword ``all``); the second parameter is a comma-delimited
       list of specific component names (if only ``<framework>`` is
       given, then all components will be reported).

   * - ``--path <type>``
     - Show paths with which PRRTE was configured.  Accepts the
       following parameters: ``prefix``, ``bindir``, ``libdir``,
       ``incdir``, ``mandir``, ``pkglibdir``, ``sysconfdir``.

   * - ``--show-version <type>:<part>``
     - Show version of PRRTE or a component.  The first parameter can
       be the keywords ``prte``, ``all``, or a framework name
       (indicating all components in a framework), or a
       ``framework:component`` string (indicating a specific
       component).  The second parameter can be one of: ``full``,
       ``major``, ``minor``, ``release``, ``greek``, ``git``.

   * - ``-V`` | ``--version``
     - Print version and exit

Report bugs to %s

[param]

Syntax: ``--param <arg0> <arg1>``

Show MCA parameters.  The first parameter is the framework (or the
keyword ``all``); the second parameter is the specific component name
(or the keyword ``all``).

[internal]

Syntax: ``--internal``

Show internal MCA parameters (i.e., parameters not meant to be
modified by users)

[path]

Syntax: ``--path <arg0>``

Show the paths with which PRRTE was configured.  Accepts the following
parameters: ``prefix``, ``bindir``, ``libdir``, ``incdir``,
``mandir``, ``pkglibdir``, ``sysconfdir``.

[arch]

Syntax: ``--arch``
Show architecture on which PRRTE was compiled

[config]

Syntax: ``-c`` or ``--config``

Show configuration options used to configure PRRTE

[hostname]

Syntax: ``--hostname``

Show the hostname upon which PRRTE was configured and built

[all]

Syntax: ``-a`` or ``--all``

Show all configuration options and MCA parameters
