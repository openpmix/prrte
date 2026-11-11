.. _man1-pterm:

pterm
=====

.. include_body

pterm |mdash| terminate a PRRTE DVM

SYNOPSIS
--------

.. code:: sh

   pterm [options]

DESCRIPTION
-----------

``pterm`` terminates an instance of the PMIx Reference Runtime
Environment (PRRTE) distributed virtual machine (DVM) that was started
by :ref:`prte(1) <man1-prte>`. It connects, as a PMIx tool, to the DVM
daemon on the node where it runs |mdash| the DVM controller on the node
where ``prte`` was started, or the :ref:`prted(1) <man1-prted>` daemon on
any other node of the DVM |mdash| and directs the DVM to shut down,
taking with it any jobs still running in it.

The connection is local to the node, as it is for
:ref:`prun(1) <man1-prun>`: ``pterm`` must run on a node where one of the
DVM's daemons is running, with the ``TMPDIR`` that daemon was started
with, or be given the daemon's rendezvous file with
``--dvm-uri file:<path>``. See :ref:`label-prun-local-daemon`. The
connection options below select which of this node's daemons ``pterm``
connects to.

``pterm`` accepts no arguments other than the options listed here.
``pterm --help <option>`` prints the full description of an option.

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

.. rubric:: Specific options

``--pmixmca <key> <value>``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. include:: /prrte-rst-content/cli-pmixmca.rst

``--allow-run-as-root``
^^^^^^^^^^^^^^^^^^^^^^^

Accepted for symmetry with the other PRRTE tools |mdash| ``pterm``
launches nothing, so no root-execution guard applies.

EXIT STATUS
-----------

``pterm`` exits with status 0 if it reached a daemon of the DVM and the
DVM was directed to terminate, and non-zero otherwise.

EXAMPLES
--------

On the node where ``prte`` runs, terminate the DVM whose controller
wrote its URI to a file:

.. code:: sh

   shell$ prte --report-uri dvm.uri --daemonize
   shell$ pterm --dvm-uri file:dvm.uri

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prun(1) <man1-prun>`
