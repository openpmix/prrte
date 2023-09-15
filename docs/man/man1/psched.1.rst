.. _man1-psched:

psched
======

psched |mdash| a modest scheduler for PRRTE

SYNOPSIS
--------

.. code:: sh

   shell$ psched ...options...

DESCRIPTION
-----------

``psched`` is a standalone daemon that acts as a dynamic
scheduler for PRRTE.

Extensive help documentation for this command is provided through
``psched --help [topic]``.

COMMAND LINE OPTIONS
--------------------
The following command line options are recognized by ``psched``.

General command line options
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-general.rst

MCA parameters
^^^^^^^^^^^^^^

* ``--pmixmca <key> <value>``: Pass context-specific PMIx MCA
  parameters (``<key>>`` is the parameter name; ``<value>`` is the
  parameter value).
  :ref:`See below for details <label-psched-pmixmca>`.

* ``--prtemca <key> <value>``: Pass context-specific PRRTE MCA
  parameters to the scheduler.
  :ref:`See below for details <label-psched-prtemca>`.

* ``--tune <filename>``: File(s) containing MCA params for tuning
  scheduler operations.
  :ref:`See below for details <label-psched-tune>`.

Output options
^^^^^^^^^^^^^^

* ``--output <list>``: Comma-delimited list of options that control
  how output is generated.  :ref:`See below for details
  <label-psched-output>`.

* ``--stream-buffering <value>``: Control how output is buffered.
  :ref:`See below for details <label-psched-stream-buffering>`.

Resource options
^^^^^^^^^^^^^^^^

* ``--default-hostfile <filename>``: Provide a default hostfile.

* ``-H`` | ``--host <list>``: Comma-delimited list of hosts to be
  included in scheduler queues
  :ref:`See below for details <label-psched-host>`.

* ``--hostfile <filename>``: Provide a hostfile.
  :ref:`See below for details <label-psched-hostfile>`.

* ``--machinefile <filename>``: Synonym for ``--hostfile``.


Specific options
^^^^^^^^^^^^^^^^

* ``--allow-run-as-root``: Allow execution as root **(STRONGLY
  DISCOURAGED)**.  :ref:`See below for details
  <label-psched-allow-run-as-root>`.

* ``--daemonize``: Daemonize the scheduler into the background.

* ``--no-ready-msg``: Do not output a "ready" message when the
  scheduler has completed initializing.

* ``--set-sid``: Direct the scheduler to separate from the current
  session.

* ``--tmpdir <dir>``: Set the root for the session directory tree.

* ``--report-pid <value>``: Print out PID on stdout (``-``), stderr
  (``+``), or a filename (anything else)

* ``--report-uri <value>``: Print out URI on stdout (``-``), stderr
  (``+``), or a filename (anything else)

* ``--keepalive <filename>``: Named pipe filename to monitor |mdash|
  ``psched`` will terminate upon closure


Debug options
^^^^^^^^^^^^^

* ``--debug``: Synonym for ``--leave-session-attached``

* ``--leave-session-attached``: Do not discard stdout/stderr of remote
  PRTE daemons.
  :ref:`See below for details <label-psched-leave-session-attached>`.


Details of individual command line options
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The sections below offer more detail than the abbreviated lists,
above.

.. _label-psched-pmixmca:

The ``--pmixmca`` option
~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-pmixmca.rst

.. _label-psched-prtemca:

The ``--prtemca`` option
~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-prtemca.rst

.. _label-psched-tune:

The ``--tune`` option
~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-tune.rst

.. _label-psched-allow-run-as-root:

The ``--allow-run-as-root`` option
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-allow-run-as-root.rst

.. _label-psched-output:

The ``--output`` option
~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-output.rst

.. _label-psched-stream-buffering:

The ``--stream-buffering`` option
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-stream-buffering.rst

.. _label-psched-host:

The ``--host`` option
~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-dash-host.rst

.. _label-psched-hostfile:

The ``--hostfile`` option
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-dvm-hostfile.rst

.. _label-psched-leave-session-attached:

The ``--leave-session-attached`` option
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-leave-session-attached.rst

.. _label-psched-display:

The ``--display`` option
~~~~~~~~~~~~~~~~~~~~~~~~

.. include:: /prrte-rst-content/cli-display.rst


.. seealso::
   :ref:`prte(1) <man1-prte>`
