.. _man1-prun:

prun
====

.. include_body

prun |mdash| submit a job to a running PRRTE DVM

SYNOPSIS
--------

Single Process Multiple Data (SPMD) model:

.. code:: sh

   prun [options] <program> [<args>]

Multiple Instruction Multiple Data (MIMD) model:

.. code:: sh

   prun [global options] [local options1] <program1> [<args1>] : \
        [local options2] <program2> [<args2>] : ... : \
        [local optionsN] <programN> [<argsN>]

DESCRIPTION
-----------

``prun`` submits a job to an existing instance of the PMIx Reference
Runtime Environment (PRRTE) distributed virtual machine (DVM) |mdash|
one started by :ref:`prte(1) <man1-prte>` |mdash| and waits for it to
complete. Use :ref:`prterun(1) <man1-prterun>` instead to start a DVM,
run a single job in it, and shut it down again.

``prun`` connects to the DVM controller as a PMIx tool; the connection
options below select which controller it connects to.

Each application context on the command line is separated by a ``:``.
The options that precede a context's program apply to that context.
Options are recognized only *before* the program name: the first token
that is not an option is taken as the executable, and everything after
it is passed to the program as its arguments. See "Per-app-context
mapping" under ``--mapby`` for how placement options given on
different contexts combine.

All of the text below is also available from the command itself:
``prun --help`` lists the options, and ``prun --help <option>`` (for
example, ``prun --help mapby``) prints the full description of one.
``prun --help placement`` describes the mapping, ranking and binding
procedure in detail.

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

.. rubric:: Connection options

``--dvm-uri <uri>``
^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-dvm-uri.rst

``--namespace <name>``
^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-namespace.rst

``--pid <pid>``
^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pid.rst

``--system-server-first``
^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-system-server-first.rst

``--system-server-only``
^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-system-server-only.rst

``--wait-to-connect <seconds>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-wait-to-connect.rst

``--num-connect-retries <num>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-num-connect-retries.rst

``--do-not-connect``
^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-do-not-connect.rst

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

.. rubric:: MCA parameters

MCA parameters given to ``prun`` |mdash| by ``--prtemca``,
``--pmixmca`` or ``--tune`` |mdash| configure the ``prun`` process
itself. The DVM's daemons keep the parameters the DVM was started with;
give those to :ref:`prte(1) <man1-prte>`.

``--prtemca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-prtemca.rst

``--pmixmca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmixmca.rst

``--tune <files>``
^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-tune.rst

.. include:: /man/man1/job-options.rstxt

.. rubric:: Fault tolerance options

``--enable-recovery``
^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-enable-recovery.rst

``--max-restarts <num>``
^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-max-restarts.rst

``--continuous``
^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-continuous.rst

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

.. include:: /man/man1/deprecated-job-options.rstxt

``--disable-recovery``
   Replaced by ``--rtos recoverable=false``.

``-p`` | ``--parsable`` | ``--parseable``
   Replaced by the ``parseable`` qualifier to ``--display``, e.g.
   ``--display map:parseable``.

ENVIRONMENT
-----------

``MPIEXEC_TIMEOUT``
   The job timeout in seconds, used when ``--timeout`` is not given.

``PRTE_ALLOW_RUN_AS_ROOT``, ``PRTE_ALLOW_RUN_AS_ROOT_CONFIRM``
   When *both* are set to ``1``, permit execution as root |mdash| see
   ``--allow-run-as-root``.

``PRTE_MCA_<name>``, ``PMIX_MCA_<name>``
   Set the PRRTE or PMIx MCA parameter ``<name>``, as ``--prtemca`` and
   ``--pmixmca`` do on the command line.

EXIT STATUS
-----------

``prun`` reports the outcome of the job it launched:

* **0** if the job completed successfully.

* **the application's exit status**, if a process of the job exited with
  a non-zero status or was killed. As with any launcher, only the low
  eight bits survive, and a job of many processes has only one status to
  report: it is the one recorded for the first process whose failure
  caused the job to be terminated, which is also the process named in the
  error message.

* **a non-zero status derived from the reason the job ended**, when the
  job failed without any process having produced an exit status of its
  own |mdash| a job that could not be mapped or launched, for instance.
  Do not attach meaning to the particular value beyond "not zero".

Note that ``prun`` reports the *job's* status, not its own: a failure to
reach the DVM, or an invalid command line, also exits non-zero.

EXAMPLES
--------

Start a DVM, run a job of four processes in it, and shut it down:

.. code:: sh

   shell$ prte --daemonize
   shell$ prun -n 4 hostname
   shell$ pterm

Run two application contexts in one job, placing the first by core and
the second by node:

.. code:: sh

   shell$ prun --mapby core -n 4 ./app1 : --mapby node -n 2 ./app2

Show where the processes of a job would be placed, without launching
it:

.. code:: sh

   shell$ prun --rtos donotlaunch --display map -n 16 ./a.out

Connect to the DVM whose controller wrote its URI to a file:

.. code:: sh

   shell$ prte --report-uri dvm.uri --daemonize
   shell$ prun --dvm-uri file:dvm.uri -n 4 ./a.out

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prterun(1) <man1-prterun>`,
   :ref:`pterm(1) <man1-pterm>`,
   :ref:`prte_info(1) <man1-prte_info>`
