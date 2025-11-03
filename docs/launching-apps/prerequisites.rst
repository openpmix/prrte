.. _running-prerequisites-label:

Prerequisites
=============

Successful launch of jobs by PRRTE requires the ability to
find the PRRTE daemon executables and shared libraries on all nodes at run
time.

In general, if the PRRTE executables and libraries can be found via in system-default
search paths (i.e., without the
user needing to set or modify ``PATH`` or ``LD_LIBRARY_PATH``), then
nothing additional needs to be done.  However, if the PRRTE binaries
and libraries are not found, the instructions below may be used to locate them.

In general, PRRTE requires that its executables are in your
``PATH`` on every node on which you will run and if PRRTE was
compiled as dynamic libraries (which is the default), the directory
where its libraries are located must be in your ``LD_LIBRARY_PATH`` on
every node.
For example:

* If PRRTE is installed in ``/usr/bin`` and ``/usr/lib``), that is
  usually sufficient, and the user does not need to do anything extra.
* If PRRTE is installed in a location that is not searched by
  default, users may need to add ``$prefix/bin`` to their ``PATH`` and
  ``$libdir`` (which defaults to ``$prefix/lib``) to their
  ``LD_LIBRARY_PATH``.

  .. caution:: In scheduled environments, ensuring PRRTE's
               executables and libraries can be found on the node that
               executes ``prterun`` may be
               sufficient.

               In non-scheduled environments, users may need to set
               the ``PATH`` and ``LD_LIBRARY_PATH`` environment
               variables in their shell setup files (e.g.,
               ``$HOME/.bashrc``) so that non-interactive
               ``ssh``-based logins will be able to find the PRRTE
               executables and libraries.

               For example, if PRRTE was installed with a prefix of
               ``/opt/prrte``, then the following should be in your
               ``PATH`` and ``LD_LIBRARY_PATH``

               .. list-table::
                  :header-rows: 1

                  * - Environment variable
                    - Value to add

                  * - ``PATH``
                    - ``/opt/prrte/bin``

                  * - ``LD_LIBRARY_PATH``
                    - ``/opt/prrte/lib``

               Depending on your environment, you may need to set these
               values in your shell startup files (e.g., ``.bashrc``,
               ``.cshrc``, etc.).

Additionally, PRRTE requires that jobs can be started on remote
nodes without any input from the keyboard.  For example, if using
``ssh`` as the remote agent, you must have your environment setup to
allow execution on remote nodes without entering a password or
passphrase.

Adding PRRTE to ``PATH`` and ``LD_LIBRARY_PATH``
---------------------------------------------------

PRRTE *must* be able to find its executables in your ``PATH``
on every node (if PRRTE was compiled as dynamic libraries, then its
library path must appear in ``LD_LIBRARY_PATH`` as well).  As such, your
configuration/initialization files need to add PRRTE to your ``PATH``
/ ``LD_LIBRARY_PATH`` properly.

How to do this may be highly dependent upon your local configuration;
you may need to consult with your local system administrator.  Some
system administrators take care of these details for you, some don't.
Some common examples are included below, however.

You must have at least a minimum understanding of how your shell works
to get PRRTE in your ``PATH`` / ``LD_LIBRARY_PATH`` properly.  Note
that PRRTE must be added to your ``PATH`` and ``LD_LIBRARY_PATH``
in the following situations:

#. When you login to an interactive shell

   If your interactive login environment is not configured properly,
   executables like ``prterun`` will not be found, and it is typically
   obvious what is wrong.  The PRRTE executable directory can
   manually be added to the ``PATH``, or the user's startup files can
   be modified such that the PRRTE executables are added to the
   ``PATH`` every login.  This latter approach is preferred.

   All shells have some kind of script file that is executed at login
   time to set things like ``PATH`` and ``LD_LIBRARY_PATH`` and
   perform other environmental setup tasks.  This startup file is the
   one that needs to be edited to add PRRTE to the ``PATH`` and
   ``LD_LIBRARY_PATH``. Consult the manual page for your shell for
   specific details (some shells are picky about the permissions of
   the startup file, for example).  The table below lists some common
   shells and the startup files that they read/execute upon login:

   .. list-table::
      :header-rows: 1
      :widths: 10 90

      * - Shell
        - Interactive login startup files

      * - ``bash``
        - ``.bash_profile`` if it exists, or ``.bash_login`` if it
          exists, or ``.profile`` if it exists

          (in that order).  Note that some Linux distributions
          automatically come with

          ``.bash_profile`` scripts for users that automatically
          execute ``.bashrc`` as well.

          Consult the ``bash(1)`` man page for more information.

      * - ``zsh``
        - ``.zshrc`` followed by ``.zshenv``

      * - ``sh`` (or Bash

          named ``sh``)
        - ``.profile``

      * - ``csh``
        - ``.cshrc`` followed by ``.login``

      * - ``tcsh``
        - ``.tcshrc`` if it exists, ``.cshrc`` if it does not, followed by
          ``.login``

#. When you login to non-interactive shells on remote nodes

   If your non-interactive remote environment is not configured
   properly, executables like ``prterun`` will not function properly,
   and it can be somewhat confusing to figure out.

   The startup files in question here are the ones that are
   automatically executed for a non-interactive login on a remote node
   (e.g., ``ssh othernode ps``).  Note that not all shells support
   this, and that some shells use different files for this than listed
   for interactive logins.  Some shells will supersede non-interactive
   login startup files with files for interactive logins.  That is,
   running non-interactive login startup file *may* automatically
   invoke interactive login startup file.  The following table lists
   some common shells and the startup file that is automatically
   executed, either by PRRTE or by the shell itself:

   .. list-table::
      :header-rows: 1
      :widths: 10 90

      * - Shell
        - Non-interactive login startup files

      * - ``bash``
        - ``.bashrc`` if it exists

      * - ``zsh``
        - ``.zshrc`` followed by ``.zshenv``

      * - ``sh`` (or Bash

          named ``sh``)
        - This shell does not execute any file automatically,

          so PRRTE will execute the ``.profile`` script

          before invoking PRRTE executables on remote nodes

      * - ``csh``
        - ``.cshrc``

      * - ``tcsh``
        - ``.tcshrc`` if it exists, ``.cshrc`` if it does not


Using the ``--prefix`` option with prterun
------------------------------------------

If users are unable to add the relevant directories to ``PATH`` and
``LD_LIBRARY_PATH``, the ``prterun`` ``--prefix``
option *may* be sufficient.

There are some situations where you cannot modify the ``PATH`` or
``LD_LIBRARY_PATH`` |mdash| e.g., some ISV applications prefer to hide
all parallelism from the user, and therefore do not want to make the
user modify their shell startup files.

In such cases, you can use the ``prterun````--prefix`` command line
option, which takes as an argument the
top-level directory where PRRTE was installed.  While relative
directory names are possible, they can become ambiguous depending on
the job launcher used; using absolute directory names is strongly
recommended.

For example, say that PRRTE was installed into
``/opt/prrte-VERSION``.  You would use the ``--prefix`` option
thusly:

.. code-block::

   shell$ prterun --prefix /opt/prrte-VERSION -n 4 a.out

This will prefix the ``PATH`` and ``LD_LIBRARY_PATH`` on both the
local and remote hosts with ``/opt/prrte-VERSION/bin`` and
``/opt/prrte-VERSION/lib``, respectively.  This is *usually*
unnecessary when using resource managers to launch jobs (e.g., Slurm,
Torque, etc.) because they tend to copy the entire local environment
|mdash| to include the ``PATH`` and ``LD_LIBRARY_PATH`` |mdash| to
remote nodes before execution.  As such, if ``PATH`` and
``LD_LIBRARY_PATH`` are set properly on the local node, the resource
manager will automatically propagate those values out to remote nodes.
The ``--prefix`` option is therefore usually most useful in
``ssh``-based environments (or similar), OR when the cluster has been
configured with PRRTE located in a different location on the
remote nodes.

It is possible to make this the default behavior by passing to
``configure`` the flag ``--enable-prterun-prefix-by-default``.  This
will make ``prterun`` behave exactly the same as
``prterun --prefix $prefix ...``, where ``$prefix`` is the value given
to ``--prefix`` in ``configure``.

Finally, note that specifying the absolute pathname to ``prterun`` is
equivalent to using the ``--prefix`` argument.  For
example, the following is equivalent to the above command line that
uses ``--prefix``:

.. code-block::

   shell$ /opt/prrte-VERSION/bin/prterun -n 4 a.out
