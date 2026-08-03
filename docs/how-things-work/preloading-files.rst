.. _preloading-files-label:

Preloading Files
================

A job's executable and its input files usually have to be present on every
node that will run it.  Where a shared filesystem does not provide that,
PRRTE can carry them out itself: ``--preload-binary`` stages the
executable, and ``--preload-files`` stages a comma-separated list of data
files.  Both are per-application-context directives, so each app of an
MPMD command line can name its own.

.. code:: sh

   # stage the executable and two input files out to every node
   shell$ prun -n 4 --preload-binary --preload-files inputs/mesh.dat,params.cfg ./solver

The files are read once on the DVM master, broadcast to every daemon, and
written into that node's session directory.  Just before a daemon forks the
application processes, it puts them where those processes will look for
them.


Where the files land
--------------------

**In the working directory the processes will run in** |mdash| the
directory the job would have started in anyway: the directory ``prun`` was
invoked from, or the one named by ``--wdir``.  So the name a process uses
to open a preloaded file is the relative name it was preloaded under:

.. code:: sh

   shell$ prun -n 4 --preload-files /scratch/me/mesh.dat ./solver   # solver opens "mesh.dat"
   shell$ prun -n 4 --preload-files inputs/mesh.dat ./solver        # solver opens "inputs/mesh.dat"

The two forms differ because an absolute path cannot be reproduced under a
working directory: PRRTE never writes a preloaded file to an absolute
location, since that would let a job overwrite anything on a remote node.
A file named by an **absolute** path is therefore placed under its basename.
A file named by a **relative** path keeps that relative path, since that is
the name the application already uses for it, and any directories it needs
are created.

Two directives change this:

* ``--preload-binary`` sets the working directory of its app to the job's
  session directory (it is exactly ``--set-cwd-to-session-dir`` plus the
  staging), so a job that preloads its executable finds its preloaded data
  files there as well.
* The ``filem_raw_flatten_directory_trees`` MCA parameter reduces *every*
  preloaded file to its basename, so nothing lands in a subdirectory.

An archive |mdash| a file whose name ends in ``.tar``, ``.bz`` or ``.gz``
|mdash| is unpacked, and it is the *contents* that are placed, at the paths
the archive names them by, relative to the working directory.


Names that would step outside the working directory are refused
---------------------------------------------------------------

Because a preloaded file is always placed *inside* the working directory,
the name it is delivered under has to stay inside it.  A leading ``./`` or
``../`` is simply removed |mdash| ``--preload-files ../inputs/mesh.dat``
delivers ``inputs/mesh.dat``, which is the useful reading.  A ``..``
anywhere else in the name has no such reading: ``a/../../mesh.dat`` names a
file two levels *above* the directory the job asked for it in, on every
node at once.  Such a request is refused before anything is staged:

.. code:: sh

   shell$ prun -n 4 --preload-files a/../../mesh.dat ./solver
   --------------------------------------------------------------------------
   A file requested for preloading cannot be delivered under the name it was
   given:

      File: a/../../mesh.dat
   ...
   --------------------------------------------------------------------------

Name the file by a path relative to the directory it should appear in, or
by an absolute path |mdash| in which case it is delivered under its
basename.


Existing files are never overwritten
------------------------------------

PRRTE will not overwrite a file it did not put there.  If something of the
same name is already in the working directory and its contents are not what
was about to be staged, the job is aborted with a message naming the file,
the directory and the node:

.. code:: sh

   shell$ prun -n 4 --preload-files /scratch/me/mesh.dat ./solver
   --------------------------------------------------------------------------
   A preloaded file cannot be placed in the working directory of the
   processes that are to use it: something else of that name is already
   there, and its contents are not what was to be staged.

      File:              mesh.dat
      Working directory: /home/me/run
      Node:              node07
   --------------------------------------------------------------------------

Remove or rename the file that is in the way, give the job a different
working directory with ``--wdir``, or preload the file under a name that
does not collide.

Two files preloaded by one job that would land under the same name are
refused the same way, before anything is staged |mdash| that is what
``--preload-files /data/a/mesh.dat,/data/b/mesh.dat`` asks for, and only
one of them could be delivered.  Name them by paths relative to a common
directory (``--preload-files a/mesh.dat,b/mesh.dat``) if both are wanted.

A file that is already there and is *byte-for-byte identical* to what was
to be staged is not an overwrite, and is left alone.  This is what makes
the ordinary cases quiet: every process of an app shares one working
directory, several apps or several jobs in a DVM may share one, the
directory may be a shared filesystem that another node's daemon has already
written, and the file the user asked to preload may simply be sitting in
the directory they launched from.

Note that the staged files remain in the working directory after the job
completes |mdash| they were delivered there, not borrowed.
