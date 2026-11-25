.. -*- rst -*-

   Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
   Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

The ``uniform-nodes`` command line directive is used to indicate that
the allocated nodes should be treated as having only one topology, so
optimize the launch for that scenario. This includes ensuring that all
CPU allocations are the same on each node, that each node contains the
same number of devices and topological layers, etc.

.. note:: The runtime does not currently support mixes of chips with
          different endianness.

.. warning:: Only the first daemon to report is asked for its topology,
             so anything that differs between nodes goes unseen. In
             particular, ``--map-by device=gpu`` checks only that node's
             GPUs for an identity the vendor's runtime accepts. An NVIDIA
             GPU's identity can come from the driver at daemon startup,
             and so can be present on one node and absent on another. A
             node that cannot name one of its GPUs is then mapped anyway,
             and its processes see every GPU on the node. Without this
             option such a node is refused.
