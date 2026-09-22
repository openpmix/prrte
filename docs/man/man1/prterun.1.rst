.. _man1-prterun:

prterun
=======

.. include_body

prterun |mdash| start a PRRTE DVM, run one job in it, and shut it down

SYNOPSIS
--------

Single Process Multiple Data (SPMD) model:

.. code:: sh

   prterun [options] <program> [<args>]

Multiple Instruction Multiple Data (MIMD) model:

.. code:: sh

   prterun [global options] [local options1] <program1> [<args1>] : \
           [local options2] <program2> [<args2>] : ... : \
           [local optionsN] <programN> [<argsN>]

DESCRIPTION
-----------

``prterun`` combines :ref:`prte(1) <man1-prte>` and
:ref:`prun(1) <man1-prun>` in one command: it instantiates a PMIx
Reference Runtime Environment (PRRTE) distributed virtual machine (DVM)
across the allocation, launches the given job in it, waits for the job
to complete, and then tears the DVM down again. Use ``prte`` and
``prun`` instead to start a persistent DVM and run many jobs in it, or
give ``prterun`` the ``--dvm`` option to have it submit the job to an
existing DVM rather than start one of its own.

Each application context on the command line is separated by a ``:``.
The options that precede a context's program apply to that context.
Options are recognized only *before* the program name: the first token
that is not an option is taken as the executable, and everything after
it is passed to the program as its arguments. See "Per-app-context
mapping" under ``--mapby`` for how placement options given on
different contexts combine.

All of the text below is also available from the command itself:
``prterun --help`` lists the options, and ``prterun --help <option>``
(for example, ``prterun --help mapby``) prints the full description of
one. ``prterun --help placement`` describes the mapping, ranking and
binding procedure in detail.

OPTIONS
-------

A value may follow its option either as the next argument or after an
``=`` (``--np 4`` or ``--np=4``).

.. rubric:: General options

``-h`` | ``--help [<option>]``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Print the list of options, or the full help for the named option.

``-v`` | ``--verbose``
^^^^^^^^^^^^^^^^^^^^^^

Enable typical debug options.

``-V`` | ``--version``
^^^^^^^^^^^^^^^^^^^^^^

Print version and exit.

.. rubric:: MCA parameters

``--prtemca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-prtemca.rst

``--pmixmca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmixmca.rst

``--tune <files>``
^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-tune.rst

.. rubric:: DVM options

``--dvm <arg>``
^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-dvm.rst

``--default-hostfile <filename>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Specify a default hostfile.

Also see ``--hostfile``.

``--uniform-nodes``
^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-homo-nodes.rst

``--prefix <dir>``
^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-prefix.rst

``--noprefix``
^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-noprefix.rst

``--pmix-prefix <dir>``
^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmix-prefix.rst

``--launch-agent <executable>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Name of daemon executable used to start processes on remote nodes
(default: ``prted``).

This is the executable ``prterun`` shall start on each remote node when
establishing the DVM.

``--max-vm-size <num>``
^^^^^^^^^^^^^^^^^^^^^^^

Maximum number of daemons to start.

``--keepalive <fd>``
^^^^^^^^^^^^^^^^^^^^

Pipe for ``prterun`` to monitor |mdash| job will terminate upon
closure.

``--system-server``
^^^^^^^^^^^^^^^^^^^

Start ``prterun`` and its daemons as the system server on their nodes.

``--set-sid``
^^^^^^^^^^^^^

Direct the DVM (controller and daemons) to separate from the current
session.

``--report-pid <arg>``
^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-report-pid.rst

``--report-uri <arg>``
^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-report-uri.rst

``--allow-run-as-root``
^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-allow-run-as-root.rst

``--forward-signals <signals>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-forward-signals.rst

``--debug-daemons``
^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-debug-daemons.rst

``--debug-daemons-file``
^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-debug-daemons-file.rst

``--leave-session-attached``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-leave-session-attached.rst

.. include:: /man/man1/job-options.rstxt

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

.. include:: /man/man1/deprecated-job-options.rstxt

``--hetero-nodes``
   Has no effect beyond a warning: heterogeneous nodes are detected
   automatically. Give ``--uniform-nodes`` if the nodes are known to be
   identical.

ENVIRONMENT
-----------

``MPIEXEC_TIMEOUT``
   The job timeout in seconds, used when ``--timeout`` is not given.

``PRTEPROXY_USE_DVM``
   When set, ``prterun`` behaves as if ``--dvm`` had been given without
   an argument, submitting the job to an existing DVM.

``PRTE_ALLOW_RUN_AS_ROOT``, ``PRTE_ALLOW_RUN_AS_ROOT_CONFIRM``
   When *both* are set to ``1``, permit execution as root |mdash| see
   ``--allow-run-as-root``.

``PRTE_MCA_<name>``, ``PMIX_MCA_<name>``
   Set the PRRTE or PMIx MCA parameter ``<name>``, as ``--prtemca`` and
   ``--pmixmca`` do on the command line.

EXIT STATUS
-----------

``prterun`` reports the outcome of the job it launched, exactly as
:ref:`prun(1) <man1-prun>` does:

* **0** if the job completed successfully.

* **the application's exit status**, if a process of the job exited with
  a non-zero status or was killed. Only the low eight bits survive, and
  it is the status recorded for the first process whose failure caused
  the job to be terminated, which is also the process named in the
  error message.

* **a non-zero status derived from the reason the job ended**, when the
  job failed without any process having produced an exit status of its
  own |mdash| a job that could not be mapped or launched, for instance.
  Do not attach meaning to the particular value beyond "not zero".

A failure to start the DVM, or an invalid command line, also exits
non-zero.

EXAMPLES
--------

Run four copies of a program across the hosts named in a hostfile:

.. code:: sh

   shell$ prterun --hostfile myhosts -n 4 ./a.out

Show where the processes of a job would be placed and bound, without
launching it:

.. code:: sh

   shell$ prterun --rtos donotlaunch --display map,bind -n 16 ./a.out

Run the same command against a persistent DVM started earlier with
``prte``:

.. code:: sh

   shell$ prterun --dvm search -n 4 ./a.out

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prun(1) <man1-prun>`,
   :ref:`pterm(1) <man1-pterm>`,
   :ref:`prte_info(1) <man1-prte_info>`
