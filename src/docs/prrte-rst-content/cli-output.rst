.. -*- rst -*-

   Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
   Copyright (c) 2023 Jeffrey M. Squyres.  All rights reserved.

   $COPYRIGHT$

   Additional copyrights may follow

   $HEADER$

.. The following line is included so that Sphinx won't complain
   about this file not being directly included in some toctree

The ``output`` command line directive must be accompanied by a
comma-delimited list of case-insensitive options that control how
output is generated. The full directive need not be provided |mdash| only
enough characters are required to uniquely identify the directive. For
example, ``MERGE`` is sufficient to represent the
``MERGE-STDERR-TO-STDOUT`` directive |mdash| while ``TAG`` can not be
used to represent ``TAG-DETAILED`` (though ``TAG-D`` would suffice).

Supported values include:

* ``TAG`` marks each output line with the ``[job,rank]<stream>:`` of
  the process that generated it

* ``TAG-DETAILED`` marks each output line with a detailed annotation
  containing ``[namespace,rank][hostname:pid]<stream>:`` of the
  process that generated it

* ``TAG-FULLNAME`` marks each output line with the
  ``[namespace,rank]<stream>:`` of the process that generated it

* ``TAG-FULLNAME`` marks each output line with the
  ``[namespace,rank]<stream>:`` of the process that generated it

* ``TIMESTAMP`` prefixes each output line with a ``[datetime]<stream>:``
  stamp. Note that the timestamp will be the time when the line is
  output by the DVM and not the time when the source output it

* ``XML`` provides all output in a pseudo-XML format
  ``MERGE-STDERR-TO-STDOUT`` merges stderr into stdout

* ``DIR=DIRNAME`` redirects output from application processes into
  ``DIRNAME/job/rank/std[out,err,diag]``. The provided name will be
  converted to an absolute path

* ``FILE=FILENAME`` redirects output from application processes into
  ``filename.rank.`` The provided name will be converted to an absolute
  path

Supported qualifiers include ``COPY`` (also send a copy of the output to
the stdout/err streams), ``NOCOPY`` (do not copy the output to the
stdout/err streams |mdash| the default), ``RAW`` (do not buffer the output
into complete lines, but instead output it as it is received), and
``PATTERN``.

``PATTERN`` is used with the ``FILE`` directive. It says that the name you
gave is a pattern you compose yourself, rather than a stem to be annotated
with the namespace and rank. These conversions are expanded in it:

.. list-table::
   :header-rows: 1

   * - Conversion
     - Expands to
   * - ``%n``
     - the job's namespace
   * - ``%r``
     - the process's rank
   * - ``%R``
     - the rank, zero-padded to the width of the job's largest rank
   * - ``%h``
     - the hostname of the node the process ran on
   * - ``%%``
     - a literal ``%`` character

The stream suffix (``.out`` or ``.err``) is still appended, so stdout and
stderr can never land on the same file. A pattern containing no ``%`` at
all is therefore simply a fixed name shared by every process in the job.

For example, ``--output file=run/%h/rank-%R:pattern`` writes each
process's stdout to ``run/<hostname>/rank-<rank>.out``.

Any conversion other than those listed above is an error, reported when
the command line is parsed.

Every directive and qualifier above that asks a yes-or-no question
|mdash| everything except ``DIR`` and ``FILE``, which name a place to
write |mdash| may also be given an explicit truth value:

.. code::

   --output tag        the directive is requested
   --output tag=1      the same, said explicitly
   --output tag=0      the directive is NOT requested

True may also be written ``T``, ``Y``, ``TRUE``, ``YES`` or ``ENABLE``,
and false ``F``, ``N``, ``FALSE``, ``NO`` or ``DISABLE`` |mdash|
case-insensitively, and as whole words (``TR`` is not an abbreviation of
``TRUE``).

A value that is neither true nor false is refused rather than guessed at:
the truth test underneath reads anything it does not recognize as
``false``, so ``tag=maybe`` would otherwise quietly turn tagging off.

``--output`` describes the job as a whole: there is no way to give one app
context of an MPMD command line different output handling from another. It
may therefore be written in *any* app context and applies to all of them.
Two app contexts that ask for opposite things are refused, since there is
no way to honor both.
