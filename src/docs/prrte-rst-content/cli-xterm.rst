.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Display the output of the specified application processes in their own
xterm window. Ranks are given as a comma-delimited list of ranks and
inclusive ranges (for example, ``1,3-6,9``), or as ``all``. A trailing
``!`` (for example, ``1,3!``) keeps each window open after its process
exits. The xterm is started by the daemon on the node where the process
runs, so it needs a usable ``DISPLAY`` there: forward it with
``-x DISPLAY``, or start the DVM with an ``ssh -X`` launch agent.
