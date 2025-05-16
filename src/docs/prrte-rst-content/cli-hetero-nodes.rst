.. -*- rst -*-

   Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

The ``hetero-nodes`` command line directive is used to indicate
that the allocated nodes should be treated as having different
topologies. This can be useful, for example, if a scheduler is
allocating at the CPU instead of node level, it might allocate
different CPUs on the various nodes. In the eyes of the runtime,
this equates to a hetero node situation since the bitmap within
the topology of each node will differ.

.. note:: The runtime does not currently support mixes of chips
          with different endianness
