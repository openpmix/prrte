Host specification
==================

PRRTE identifies hosts on which to launch processes by:

#. A Resource Manager (RM) telling PRRTE which hosts to use
#. The user providing a hostfile (also sometimes called a "machine
   file")
#. The user providing a list of hosts on the command line

The sections below describe each of these in more detail.

Host names and domains
----------------------

However a host is named, PRRTE knows it by its name without its
domain: ``n01.cluster.org`` and ``n01`` name the same host, whether
they come from a resource manager, a hostfile, the command line, or
the host's own ``hostname``. So a hostfile may say ``n01.cluster.org``
to select a node a resource manager calls ``n01``, and the reverse.
The name as written is still the one PRRTE uses to reach the host
(for example, the one ``ssh`` is given). IP addresses are left as
they are.

Set the ``prte_keep_fqdn_hostnames`` MCA parameter to compare names
whole instead. Then ``n01.cluster.org`` and ``n01`` are different
hosts, which is what you want only if two of your hosts share a short
name and differ by domain.

.. toctree::
   :maxdepth: 1

   rm
   hostfiles
   cli
   relative-indexing
