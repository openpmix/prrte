.. _man1-prted:

prted
=====

.. include_body

prted |mdash| PRRTE daemon

SYNOPSIS
--------

.. code:: sh

   prted [options]

DESCRIPTION
-----------

``prted`` is the per-node daemon of a PMIx Reference Runtime
Environment (PRRTE) distributed virtual machine (DVM). One ``prted``
runs on each node of the DVM other than the one hosting the DVM
controller; it launches and monitors the application processes on its
node, serves as their local PMIx server, and relays their I/O and
status to the controller.

Users do not normally run ``prted`` themselves: the DVM controller
(:ref:`prte(1) <man1-prte>` or :ref:`prterun(1) <man1-prterun>`)
starts it on each remote node, through the launcher appropriate to the
environment, and passes it the options it needs. The options below are
documented chiefly for those debugging a DVM's startup, or setting one
up by hand.

``prted --help <option>`` prints the full description of an option.

OPTIONS
-------

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

.. rubric:: Debug options

``--debug-daemons``
^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-debug-daemons.rst

``--debug-daemons-file``
^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-debug-daemons-file.rst

``--leave-session-attached``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-leave-session-attached.rst

.. rubric:: DVM options

``--prtemca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-prtemca.rst

``--pmixmca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmixmca.rst

``--dvm-master-uri <uri>``
^^^^^^^^^^^^^^^^^^^^^^^^^^

Specify the URI of the DVM master, or the name of the file (specified
as ``file:filename``) that contains that info.

``--parent-uri <uri>``
^^^^^^^^^^^^^^^^^^^^^^

Specify the URI of the prted acting as the parent of this prted in a
tree-based spawn operation.

``--tree-spawn``
^^^^^^^^^^^^^^^^

A tree-based spawn operation is in progress.

``--bootstrap``
^^^^^^^^^^^^^^^

Self-construct the DVM based on a configuration file.

``--uniform-nodes``
^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-homo-nodes.rst

.. rubric:: Specific options

``--daemonize``
^^^^^^^^^^^^^^^

Daemonize this ``prted`` into the background, detaching it from the
terminal that started it and closing its output streams. This is what
the DVM controller normally directs a remotely-launched daemon to do;
see ``--leave-session-attached`` for the way to keep those streams open
instead.

``--set-sid``
^^^^^^^^^^^^^

Direct the daemon to separate from the current session.

``--system-server``
^^^^^^^^^^^^^^^^^^^

Start the daemon as the system server on its node.

``--pubsub-server <uri>``
^^^^^^^^^^^^^^^^^^^^^^^^^

Contact information for external PRRTE publish/lookup data server.

``--allow-run-as-root``
^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-allow-run-as-root.rst

DEPRECATED COMMAND LINE OPTIONS
-------------------------------

``--hetero-nodes``
   Has no effect beyond a warning: heterogeneous nodes are detected
   automatically. Give ``--uniform-nodes`` if the nodes are known to be
   identical.

``--debug``
   Has no effect; it is accepted, with a warning, so that old command
   lines still parse.

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prterun(1) <man1-prterun>`,
   :ref:`prte(5) <man5-prte>`
