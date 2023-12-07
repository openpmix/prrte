.. Copyright (c) 2021-2023 Nanook Consulting  All rights reserved.
   Copyright (c) 2022      IBM Corporation.  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.
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

Usage: %s [OPTION]...

The following list of command line options are available. Note that
more detailed help for any option can be obtained by adding that
option to the help request as "--help <option>".

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - General Options

   * - Option
     - Description

   * - ``-h`` | ``--help``
     - This help message

   * - ``-h`` | ``--help <arg0>``
     - Help for the specified option

   * - ``-v`` | ``--verbose``
     - Enable typical debug options

   * - ``-V`` | ``--version``
     - Print version and exit

   * - ``--pmixmca <key> <value>``
     - Pass context-specific PMIx MCA parameters; they are considered
       global if only one context is specified (``key`` is the
       parameter name; ``value`` is the parameter value)

   * - ``--prtemca <key> <value>``
     - Pass context-specific PRTE MCA parameters; they are considered
       global if ``--gmca`` is not used and only one context is
       specified (``key`` is the parameter name; ``value`` is the
       parameter value)

   * - ``--tune <files>``
     - File(s) containing MCA params for tuning scheduler operations

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Resource Options

   * - Option
     - Description

   * - ``--default-hostfile <filename>``
     - Provide a default hostfile

   * - ``-H`` | ``--host <hosts>``
     - List of hosts to invoke processes on

   * - ``--hostfile <file>``
     - Provide a hostfile

   * - ``--machinefile <file>``
     - Provide a hostfile (synonym for ``--hostfile``)


.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Specific Options

   * - Option
     - Description

   * - ``--allow-run-as-root``
     - Allow execution as root (**STRONGLY DISCOURAGED**)

   * - ``--daemonize``
     - Daemonize the scheduler into the background

   * - ``--no-ready-msg``
     - Do not print a "DVM ready" message

   * - ``--set-sid``
     - Direct the scheduler to separate from the current session

   * - ``--tmpdir <dir>``
     - Set the filesystem root for the session directory tree

   * - ``--report-pid <arg>``
     - Print out PID on stdout (``-``), stderr (``+``), or a file
       (anything else)

   * - ``--report-uri <arg>``
     - Print out URI on stdout (``-``), stderr (``+``), or a file
       (anything else)

   * - ``--keepalive <arg0>``
     - Pipe to monitor - scheduler will terminate upon closure


Report bugs to %s

[version]

%s (%s) %s

Report bugs to %s

[pmixmca]

.. include:: /prrte-rst-content/cli-pmixmca.rst

[prtemca]

.. include:: /prrte-rst-content/cli-prtemca.rst

[tune]

.. include:: /prrte-rst-content/cli-tune.rst

[default-hostfile]

Specify a default hostfile.

Also see ``--help hostfile``.

[host]

.. include:: /prrte-rst-content/cli-dash-host.rst

[hostfile]

.. include:: /prrte-rst-content/cli-dvm-hostfile.rst

See the "Host specification" HTML documentation for details about the
format and content of hostfiles.

[machinefile]

Provide a hostfile.  This option is a synonym for ``--hostfile``; see
that option for more information.

[allow-run-as-root]

.. include:: /prrte-rst-content/cli-allow-run-as-root.rst

[daemonize]

Daemonize the scheduler into the background

[no-ready-msg]

Do not print a scheduler ready message

[set-sid]

Direct the scheduler to separate from the current
session

[tmpdir]

Define the root location for the PRRTE session directory tree

See the "Session directory" HTML documentation for additional details
about the PRRTE session directory.

[report-pid]

Printout scheduler's PID on stdout [-], stderr [+], or a file
[anything else]

[report-uri]

Printout scheduler's URI on stdout [-], stderr [+], or a file
[anything else]

[keepalive]

Pipe for scheduler to monitor - scheduler will terminate upon closure

