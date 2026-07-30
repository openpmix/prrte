.. -*- rst -*-

   Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Set the named environmental variable to the specified value. This will overwrite the
existing value, if it exists. Equivalent to the "-x foo=val" option

These directives are applied in the order they are given on the command
line. ``--set-env`` replaces a value outright while ``--prepend-env`` and
``--append-env`` edit the value already there, so the order is the result:
``--set-env FOO=1 --prepend-env FOO[:] x`` leaves ``FOO=x:1``, while
``--prepend-env FOO[:] x --set-env FOO=1`` leaves ``FOO=1``.
