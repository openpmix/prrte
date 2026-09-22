.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Provide an appfile describing the application contexts to be launched.
Each line of the file that is neither blank nor a comment (a line whose
first non-blank character is ``#``) describes one application context,
written as it would be on the command line: the options that apply to
that context, followed by the executable and its arguments. Lines are
split at spaces; quotes are not interpreted.

Options given on the command line alongside ``--app`` are combined with
the first application context in the file, so job-level options (such
as ``--mapby``) apply to the whole job as usual, while an option that
is also given on the file's first line is refused as a duplicate. The
command line may not also name an application |mdash| an executable, or
a ``:``-separated application context |mdash| as well.
