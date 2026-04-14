.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

The ``uniform-nodes`` command line directive is used to indicate
that the allocated nodes should be treated as having only one topology,
so optimize the launch for that scenario. This includes ensuring that
all CPU allocations are the same on each node, that each node contains
the same number of devices and topological layers, etc.

.. note:: The runtime does not currently support mixes of chips
          with different endianness
