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

[version]

%s (%s) %s

%s

[usage]

%s (%s) %s

Usage: %s [OPTION]...

Submit job to the PMIx Reference RTE

The following list of command line options are available. Note that
more detailed help for any option can be obtained by adding that
option to the help request as ``--help <option>``.

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

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Debug Options

   * - Option
     - Description

   * - ``--display <arg0>``
     - Comma-delimited list of options for displaying information
       about the allocation and job. Allowed values: ``allocation``,
       ``bind``, ``map``, ``map-devel``, ``topo``.

   * - ``--timeout <seconds>``
     - Timeout the job after the specified number of seconds

   * - ``--spawn-timeout <seconds>``
     - Timeout the job if spawn takes more than the specified number
       of seconds

   * - ``--get-stack-traces``
     - Get stack traces of all application procs on timeout

   * - ``--report-state-on-timeout``
     - Report all job and process states upon timeout

   * - ``--stop-on-exec``
     - If supported, stop each specified process at start of execution

   * - ``--stop-in-init``
     - Direct the specified processes to stop in ``PMIx_Init``

   * - ``--stop-in-app``
     - Direct the specified processes to stop at an
       application-controlled location

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Output Options

   * - Option
     - Description

   * - ``--output <args>``
     - Comma-delimited list of options that control how output is
       generated. Allowed values: ``tag``, ``tag-detailed``,
       ``tag-fullname``, ``timestamp``, ``xml``,
       ``merge-stderr-to-stdout``, ``dir=DIRNAME``,
       ``file=filename``. The ``dir`` option redirects output from
       application processes into
       ``DIRNAME/job/rank/std[out,err,diag]``. The file option
       redirects output from application processes into
       ``filename.rank``. In both cases, the provided name will be
       converted to an absolute path.  Supported qualifiers include
       ``NOCOPY`` (do not copy the output to the stdout/stderr
       streams).

   * - ``--report-child-jobs-separately``
     - Return the exit status of the primary job only

   * - ``--xterm <ranks>``
     - Create a new xterm window and display output from the specified
       ranks there

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Input Options

   * - Option
     - Description

   * - ``--stdin <ranks>``
     - Specify procs to receive stdin [``rank``, ``all``, ``none`, or
       comma-delimited list of integers] (default: ``0``, indicating
       rank 0)

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Placement Options

   * - Option
     - Description

   * - ``--map-by <type>``
     - Mapping Policy for job

   * - ``--rank-by <type>``
     - Ranking Policy for job

   * - ``--bind-to <type>``
     - Binding policy for job

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Launch Options

   * - Option
     - Description

   * - ``--runtime-options <arg0>``
     - Comma-delimited list of runtime directives for the job (e.g.,
       do not abort if a process exits on non-zero status)

   * - ``-c`` | ``--np <num_procs>``
     - Number of processes to run

   * - ``-n`` | ``--n <num_procs>``
     - Number of processes to run

   * - ``-N <num_procs>``
     - Number of processes to run per node

   * - ``--app <filename>``
     - Provide an appfile; ignore all other command line options

   * - ``-H`` | ``--host <hosts>``
     - List of hosts to invoke processes on

   * - ``--hostfile <filename>``
     - Provide a hostfile

   * - ``--machinefile <filename>``
     - Provide a hostfile (synonym for ``--hostfile``)

   * - ``--path <path>``
     - PATH to be used to look for executables to start processes

   * - ``--pmixmca <key> <value>``
     - Pass context-specific PMIx MCA parameters; they are considered
       global if only one context is specified (``key`` is the
       parameter name; ``value`` is the parameter value)

   * - ``--gpmixmca <key> <value>``
     - Pass global PMIx MCA parameters that are applicable to all
       contexts (``key`` is the parameter name; ``value`` is the
       parameter value)

   * - ``--tune <filename>``
     - File(s) containing MCA params for tuning operations

   * - ``--preload-files <filenames>``
     - Preload the comma separated list of files to the remote
       machines current working directory before starting the remote
       process.

   * - ``--pset <name>``
     - User-specified name assigned to the processes in their given
       application

   * - ``--rankfile <filename>``
     - Name of file to specify explicit task mapping

   * - ``-s`` | ``--preload-binary``
     - Preload the binary on the remote machine before starting the
       remote process.

   * - ``--set-cwd-to-session-dir``
     - Set the working directory of the started processes to their
       session directory

   * - ``--show-progress``
     - Output a brief periodic report on launch progress

   * - ``--wd <dir>``
     - Synonym for ``--wdir``

   * - ``--wdir <dir>``
     - Set the working directory of the started processes

   * - ``-x <name>``
     - Export an environment variable, optionally specifying a value
       (e.g., ``-x foo`` exports the environment variable foo and takes
       its value from the current environment; ``-x foo=bar`` exports
       the environment variable name foo and sets its value to ``bar``
       in the started processes; ``-x foo*`` exports all current
       environmental variables starting with ``foo``)

.. list-table::
   :header-rows: 2
   :widths: 20 45

   * -
     - Specific Options

   * - Option
     - Description

   * - ``--do-not-connect``
     - Do not connect to a server

   * - ``--dvm-uri <uri>``
     - Specify the URI of the DVM master, or the name of the file
       (specified as ``file:filename``) that contains that info

   * - ``--namespace <name>``
     - Namespace of the daemon to which we should connect

   * - ``--pid <pid>``
     - PID of the daemon to which we should connect (integer PID or
       ``file:<filename>`` for file containing the PID

   * - ``--system-server-first``
     - First look for a system server and connect to it if found

   * - ``--system-server-only``
     - Connect only to a system-level server

   * - ``--tmpdir <dir>``
     - Set the root for the session directory tree

   * - ``--wait-to-connect <seconds>```
     - Delay specified number of seconds before trying to connect

   * - ``--num-connect-retries <num>```
     - Max number of times to try to connect

   * - ``--personality <name>``
     - Specify the personality to be used

   * - ``--allow-run-as-root``
     - Allow execution as root (**STRONGLY DISCOURAGED**)

   * - ``--forward-signals <signals>``
     - Comma-delimited list of additional signals (names or integers)
       to forward to application processes [``none`` => forward
       nothing]. Signals provided by default include ``SIGTSTP``,
       ``SIGUSR1``, ``SIGUSR2``, ``SIGABRT``, ``SIGALRM``, and
       ``SIGCONT``

Report bugs to %s

[dvm-uri]

Specify the URI of the DVM master, or the name of the file (specified as
``file:filename``) that contains that info

[num-connect-retries]

Max number of times to try to connect to the specified server (int)

[pid]

PID of the daemon to which we should connect (integer PID or
``file:<filename>`` for file containing the PID

[namespace]

Namespace of the daemon we are to connect to (char*)

[system-server-first]

First look for a system server and connect to it if found

[system-server-only]

Connect only to a system-level server - abort if one is not found

[tmpdir]

Define the root location for the PRRTE session directory tree

See the "Session directory" HTML documentation for additional details
about the PRRTE session directory.

[wait-to-connect]

Delay specified number of seconds before trying to connect

[hostfile]

.. include:: /prrte-rst-content/cli-launcher-hostfile.rst

See the "Host specification" HTML documentation for details about the
format and content of hostfiles.

[machinefile]

Provide a hostfile.  This option is a synonym for ``--hostfile``; see
that option for more information.

[host]

.. include:: /prrte-rst-content/cli-dash-host.rst

[parseable]

Output information (e.g., help messages) in machine-parseable
friendly format

[parsable]

Output information (e.g., help messages) in machine-parseable
friendly format

[np]

Specify number of application processes to be started

[no-ready-msg]

Do not print a DVM ready message

[daemonize]

Daemonize the DVM daemons into the background

[system-server]

Start the DVM as the system server

[set-sid]

Direct the DVM daemons to separate from the current session

[report-pid]

Printout PID on stdout [-], stderr [+], or a file [anything else]

[report-uri]

Printout URI on stdout [-], stderr [+], or a file [anything else]

[test-suicide]

Suicide instead of clean abort after delay

[singleton]

ID of the singleton process that started us

[keepalive]

Pipe to monitor - DVM will terminate upon closure

[map-by]

.. include:: /prrte-rst-content/cli-map-by.rst

[rank-by]

.. include:: /prrte-rst-content/cli-rank-by.rst

[bind-to]

.. include:: /prrte-rst-content/cli-bind-to.rst

[runtime-options]

.. include:: /prrte-rst-content/cli-runtime-options.rst

[display]

.. include:: /prrte-rst-content/cli-display.rst

[output]

.. include:: /prrte-rst-content/cli-output.rst

[rankfile]

Name of file to specify explicit task mapping

[do-not-launch]

Perform all necessary operations to prepare to launch the
application, but do not actually launch it (usually used
to test mapping patterns)

[display-devel-map]

Display a detailed process map (mostly intended for developers)
just before launch

[display-topo]

Display the topology as part of the process map (mostly intended
for developers) just before launch

[report-bindings]

Display process bindings to stderr

[display-devel-allocation]

Display a detailed list (mostly intended for developers) of the
allocation being used by this job

[display-map]

Display the process map just before launch

[display-allocation]

Display the allocation being used by this job

[enable-recovery]

Enable recovery from process failure [Default = disabled]

[max-restarts]

Max number of times to restart a failed process

[disable-recovery]

Disable recovery (resets all recovery options to off)

[continuous]

Job is to run until explicitly terminated

[personality]

Specify the personality to be used

[prte-server]

Specify the URI of the publish/lookup server, or the name of the file
(specified as ``file:<filename>`` that contains that info

[dvm-master-uri]

URI for the DVM master

[parent-uri]

URI for the parent if tree launch is enabled

[tree-spawn]

Tree-based spawn in progress

[daemonize]

Daemonize the DVM daemons into the background

[set-sid]

Direct the DVM daemons to separate from the current session

[prtemca]

.. include:: /prrte-rst-content/cli-prtemca.rst

[pmixmca]

.. include:: /prrte-rst-content/cli-pmixmca.rst

[debug-daemons-file]

.. include:: /prrte-rst-content/cli-debug-daemons-file.rst

See the "Session directory" HTML documentation for additional details
about the PRRTE session directory.

[leave-session-attached]

.. include:: /prrte-rst-content/cli-leave-session-attached.rst
