Prerequisites
=============

Compilers
---------

Although it should probably be assumed, you'll need a C compiler that
supports C99.

GNU Autotools
-------------

When building PRRTE from its repository sources, the GNU Autotools
must be installed (i.e., `GNU Autoconf
<https://www.gnu.org/software/autoconf/>`_, `GNU Automake
<https://www.gnu.org/software/automake/>`_, and `GNU Libtool
<https://www.gnu.org/software/libtool/>`_).

.. list-table::
   :header-rows: 1
   :widths: 10 10

   * - Tool
     - Minimum version
   * - Autoconf
     - |autoconf_min_version|
   * - Automake
     - |automake_min_version|
   * - Libtool
     - |libtool_min_version|

.. note:: The GNU Autotools are *not* required when building PRRTE
          from distribution tarballs.  PRRTE distribution tarballs
          are bootstrapped such that end-users do not need to have the
          GNU Autotools installed.

You can generally install GNU Autoconf, Automake, and Libtool via your
Linux distribution native package system, or via Homebrew or MacPorts
on MacOS.  This usually "just works."

If you run into problems with the GNU Autotools, or need to download /
build them manually, see the :ref:`how to build and install GNU
Autotools section <developers-installing-autotools-label>` for much
more detail.

Python
------

Python >= |python_min_version| is required when building PRRTE
from a Git clone for generating the "show help" messages and
building the PRRTE documentation and man pages.

Generating the show help messages can be accomplished with core
Python; only building the full PRRTE documentation and man pages
requires additional Python packages (:ref:`see below <developers-requirements-sphinx-label>`).

Perl
----

PRRTE still uses Perl for a few of its build scripts (most notably,
``autogen.pl``).

Generally speaking, any recent-ish release of Perl 5 should be
sufficient to correctly execute PRRTE's Perl scripts.

.. _developers-requirements-sphinx-label:

Sphinx (and therefore Python)
-----------------------------

`Sphinx <https://www.sphinx-doc.org/>`_ is a Python-based tool used to
generate both the HTML version of the documentation (that you are
reading right now) and the nroff man pages.

Official PRRTE distribution tarballs contain pre-built HTML
documentation and man pages.  This means that -- similar to the GNU
Autotools -- end users do not need to have Sphinx installed, but will
still have both the HTML documentation and man pages installed as part
of the normal configure / build / install process.

However, the HTML documentation and man pages are *not* stored in PRRTE's
Git repository; only the ReStructured Text source code of the
documentation is in the Git repository.  Hence, if you are building
PRRTE from a Git clone, you will need Sphinx (and some Python
modules) in order to build the HTML documentation and man pages.

.. important:: Most systems do not have Sphinx and/or the required
               Python modules installed by default.  :ref:`See the
               Installing Sphinx section
               <developers-installing-sphinx-label>` for details on
               how to install Sphinx and the required Python modules.

If ``configure`` is able to find Sphinx and the required Python
modules, it will automatically generate the HTML documentation and man
pages during the normal build procedure (i.e., during ``make all``).
If ``configure`` is *not* able to find Sphinx and/or the required
Python modules, it will simply skip building the documentation.

.. note:: If you have built/installed PRRTE from a Git clone and
          unexpectedly did not have the man pages installed, it is
          likely that you do not have Sphinx and/or the required
          Python modules available.

          :ref:`See the Installing Sphinx section
          <developers-installing-sphinx-label>` for details on how
          to install Sphinx and the required Python modules.

.. important:: ``make dist`` will fail if ``configure`` did not find
               Sphinx and/or the required Python modules.
               Specifically: if ``make dist`` is not able to generate
               the most up-to-date HTML documentation and man pages,
               you cannot build a distribution tarball.  **This is an
               intentional design decision.**
