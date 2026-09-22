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
by :ref:`prte(1) <man1-prte>`. It connects to the DVM controller as a
PMIx tool and directs it to shut the DVM down, taking with it any jobs
still running in it. The connection options below select which DVM
controller ``pterm`` connects to.

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

``pterm`` exits with status 0 if it reached the DVM controller and the
DVM was directed to terminate, and non-zero otherwise.

EXAMPLES
--------

Terminate the DVM whose controller wrote its URI to a file:

.. code:: sh

   shell$ prte --report-uri dvm.uri --daemonize
   shell$ pterm --dvm-uri file:dvm.uri

.. seealso::
   :ref:`prte(1) <man1-prte>`,
   :ref:`prun(1) <man1-prun>`
