.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Print the PMIx contact URI of this process: on stdout if the argument is
``-``, on stderr if it is ``+``, and otherwise into the file it names.

With PMIx v7.0 or later, ``-`` and ``+`` print the URI of the server's
local socket, which a client or tool of v7.0 or later uses. When the
server takes clients and tools of earlier releases (legacy peers), it also
has a TCP URI, which only they use: ``tcp-`` prints it on stdout and
``tcp+`` on stderr. They are refused when the server has no TCP listener.
A file holds both URIs, so a reader of any release can use it.
