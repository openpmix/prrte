The only purpose of this file is for Sphinx to have a top-level source
RST file that includes all the others.

This file -- and the ``index.txt`` file that it generates -- is not
used by PRTE code.  **PRTE code only uses the individually
``help-*.txt`` files that Sphinx generates.**  The additional output
files that Sphinx generates in the ``_build/text`` tree are just
harmelss by-products; we ignore them.

.. toctree::

   Pinfo <help-schizo-pinfo>

   Prte <help-schizo-prte>

   Prted <help-schizo-prted>

   Prterun <help-schizo-prterun>

   Prun <help-schizo-prun>

   Prun <help-schizo-pterm>

Have to also include these additional files so that Sphinx doesn't
complain that there are RST files in the doc tree that aren't used.

.. toctree::
   :hidden:

   prrte-rst-content/prterun-all-cli
   prrte-rst-content/prterun-all-deprecated
