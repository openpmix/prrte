.. -*- rst -*-

   Copyright (c) 2026      Nanook Consulting.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

Several options take a value that is a small language of its own:
``--map-by``, ``--rank-by``, ``--bind-to``, ``--output``, ``--display``
and ``--rtos``. The words in that value come from a fixed vocabulary,
listed with each option below, and are put together the same way for all
of them.

**Separators.** A value is built from *directives*, each of which may be
followed by *qualifiers*, and any word may carry a value of its own:

* ``:`` separates a directive from its qualifiers, and one qualifier from
  the next: ``--map-by package:span:pe=2``.

* ``,`` separates one directive from the next, in the options that accept
  several: ``--output tag,timestamp`` or ``--display map,bind``. A
  directive's qualifiers follow it with ``:`` as usual, so
  ``--output tag,file=out:nocopy`` is the ``tag`` directive, then the
  ``file`` directive qualified by ``nocopy``.

* ``=`` separates a word from its value: ``pe=2``, ``device=gpu``,
  ``file=out``.

A qualifier written after a ``,`` instead of a ``:`` is therefore not a
qualifier of the directive before it. ``--map-by device=gpu,ndev=2``
names a device called ``gpu,ndev=2``, and since no device is named with a
comma, it is refused with the spelling that was almost certainly meant:

.. code::

   $ prun --map-by device=gpu,ndev=2 ./a.out
   The device named in a mapping request contains a comma:
     Given:  gpu,ndev=2
   ...
     device=gpu:ndev=2

``--rtos`` takes no qualifiers, and its values are not split at ``:`` at
all: a time is written with colons (``--rtos timeout=1:30:00``, one hour
thirty minutes).

**Abbreviations.** Words are case-insensitive, and any of them may be
shortened to a prefix that names only that word:

* ``--map-by pack`` is ``--map-by package``; ``--bind-to hwt`` is
  ``--bind-to hwthread``; ``--map-by core:ov`` is
  ``--map-by core:oversubscribe``.

* A prefix that fits more than one word is refused, and the words it fits
  are listed, rather than one of them being picked:

  .. code::

     $ prun --bind-to n ./a.out
     The --bind-to option was given a word that abbreviates more than one of the
     words it accepts:
       Given:    n
       Matches:  none,numa

  So ``--bind-to n`` must be written ``no`` or ``nu``; ``--map-by :s`` must
  be ``:sp`` (``span``) or ``:sh`` (``shared``); ``--map-by :i`` must be
  ``:inh`` (``inherit``) or ``:int`` (``interleave``); ``--rank-by s`` must
  be ``sl`` or ``sp``; and ``--output ta`` must be ``tag``, ``tag-d`` or
  ``tag-f``.

* A word given in full is always that word, even when it is also the start
  of a longer one: ``pe=2`` is the ``pe`` qualifier, not ``pe-list``, and
  ``--output tag`` is ``tag``, not ``tag-detailed``.

* A word that merely begins with one in the vocabulary is refused, not
  read as the word it begins with. ``--map-by nodes``, ``--bind-to cores``
  and ``--map-by package:spanish`` are all errors.

**Values.** Each word takes no value, may take one, or requires one:

* A word that takes no value refuses one. ``--map-by core:span=false`` is
  an error, not a request for ``span`` - to not ask for something, leave
  it out.

* A word that requires a value refuses to go without: ``--map-by core:pe``
  and ``--rtos timeout`` are errors, as is ``pe=`` with nothing after the
  ``=``.

* The directives of ``--output``, ``--display`` and ``--rtos`` that are
  simply on or off may be given a truth value, and are on when given bare:
  ``--output tag`` and ``--output tag=true`` ask for tagging, and
  ``--output tag=false`` does not.

**Examples.**

.. code::

   # two GPUs per process; bind each to a core beside them
   $ prun -n 2 --map-by device=gpu:ndev=2 --bind-to core ./a.out

   # map by package across the nodes, two cpus per process
   $ prun -n 8 --map-by package:span:pe=2 ./a.out

   # the same, abbreviated
   $ prun -n 8 --map-by pack:sp:pe=2 ./a.out

   # tag the output, and also write it to files without copying it to
   # the terminal
   $ prun -n 4 --output tag,dir=/tmp/out:nocopy ./a.out

   # show the map in a form a script can parse, and the bindings
   $ prun -n 4 --display map:parseable,bind ./a.out

   # stop the job if it runs longer than an hour and a half
   $ prun -n 4 --rtos timeout=1:30:00 ./a.out
