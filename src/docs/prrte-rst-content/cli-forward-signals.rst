.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Comma-delimited list of the signals (names or integers) to be
forwarded to application processes (``none`` = forward nothing).

The list *replaces* the default set rather than adding to it, so it
names every signal that is to be forwarded. The default set, used when
this option is not given, is SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT,
SIGALRM, and SIGCONT.
