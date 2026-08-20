.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Listing Hosts on the Command Line
=================================

Many PRRTE commands accept the ``--host`` CLI parameter.
``--host`` accepts a comma-delimited list of tokens of the form:

.. code::

   host[:slots]

The ``host`` token can be either:

* A name that resolves to an IP address, or
* An IP address

.. note:: The names and/or IP addresses of hosts are *only* used for
          identifying the target host on which to launch.  They are
          *not* used for determining which network interfaces are used
          by applications (e.g., MPI or other network-based
          applications).

          For network-based applications, consult their documentation
          for how to specify which network interfaces are used.

The optional integer ``:slots`` parameter tells PRRTE the maximum
number of slots to use on that host (see this section on definition
of the term ``slot`` for a description of what a "slot" is).

For example:

.. code::

   prterun --host node1:10,node2,node3:5 ...

``--host`` *selects* from the hosts already available to the DVM
|mdash| those a resource manager allocated, or those the DVM was
started with. It does not add any. Naming a host that is not among
them is an error, reported as:

.. code::

   At least one of the requested hosts is not included in the current
   allocation.

      Missing requested host: node9

To bring a new host into a DVM that is already running, use
``--add-host`` or ``--add-hostfile`` instead.  If the host is one the
allocation already contains but that carries no daemon |mdash| because
the DVM was started across only some of the allocation, or because a
reservation holding it was released |mdash| use ``--activate``, which
only starts a daemon and so is permitted even under a resource
manager's allocation.

The ``:slots`` count applies to *placement*, and not merely to the size
of the job: it is the number of processes that may be placed on that
host, whatever mapping policy is in effect. So

.. code::

   prterun --host node1:1,node2:1,node3:1 -n 3 ./app

puts one process on each of the three hosts under every ``--map-by``
directive, and not three on ``node1``.
