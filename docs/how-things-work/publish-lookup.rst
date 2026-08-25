Publishing and looking up data
==============================

PRRTE backs the PMIx publish/lookup service — what an MPI application
reaches through ``MPI_Publish_name``, ``MPI_Lookup_name`` and
``MPI_Unpublish_name``.  The store is a **single datastore on one process**,
normally the DVM master, that every participant reaches over the RML.  (It
can instead live in a *different* DVM; see :doc:`the cross-DVM data server
plan </plans/cross_dvm_data_server/cross-dvm-data-server>`.)

This page describes the rules the datastore applies, since several of them
decide whether a call succeeds.


Who can see what
----------------

Two independent constraints govern every lookup, and both must pass:

* **The publisher's range** (``PMIX_RANGE``, default
  ``PMIX_RANGE_SESSION``) says who may see the item.  A publisher can
  restrict its data to its own namespace, to processes behind the same
  daemon, to itself, or to a list of user and group ids it names with
  ``PMIX_ACCESS_PERMISSIONS``.

* **The requester's range**, also ``PMIX_RANGE`` and also defaulting to
  ``PMIX_RANGE_SESSION``, says whose data it is willing to search.

A publisher that names no accessors keeps its data to itself: the requester
must present the publisher's own effective uid and gid.  A lookup that is
refused data it would otherwise have found gets
``PMIX_ERR_NO_PERMISSIONS`` rather than ``PMIX_ERR_NOT_FOUND``, so a
permission problem does not look like a missing key.


Duplicate keys
--------------

A key may be published more than once **provided the publications are on
different ranges**.  Publishing a key that is already published on the same
range fails with ``PMIX_ERR_DUPLICATE_KEY``, and nothing is stored — the
publication that was already there is untouched.

A "range" here is a *set of processes*, not merely the ``PMIX_RANGE`` word:
``PMIX_RANGE_NAMESPACE`` used by two processes of different namespaces names
two disjoint sets, and those do not collide.  Neither do two publications
that are invisible to each other because of their access permissions.  What
does collide is the common case — two processes publishing the same key on
``PMIX_RANGE_SESSION``, which is exactly the name collision
``MPI_Publish_name`` is required to report.

Only the process that published an item may remove it.  An unpublish from
anyone else reports success and removes nothing, because there was nothing
of *theirs* to remove.

This is felt most when two generations of a job **overlap**.  A replacement
server starting while its predecessor is still running cannot claim the
predecessor's name: its publish is refused, ``prte.pub.replace`` does not
reach another process's key, and only the original publisher can let the
name go.  The remedies are the ones MPI offers — have the outgoing server
unpublish, publish on a range the newcomer's readers will search, or use a
distinct key.  What is no longer possible is for the newcomer to believe it
succeeded.

Generations that do *not* overlap need none of that: an item published with
the default persistence goes when its job ends, so the next generation
publishes into a store that no longer holds the name.  See `How long data
lasts`_.

.. note:: Earlier releases accepted a duplicate publish, reported
   ``PMIX_SUCCESS``, and stored the value where no lookup would reach it.
   Applications that republished a key without unpublishing it first will
   now see ``PMIX_ERR_DUPLICATE_KEY`` instead of a write that silently did
   nothing.


Replacing your own publication
------------------------------

Because a duplicate is refused, a process that wants to *update* a value it
published itself would have to call ``PMIx_Unpublish`` first.  PRRTE accepts
a directive that does both in one step:

.. code-block:: c

   pmix_info_t info[3];
   PMIX_INFO_LOAD(&info[0], "my_key", value, PMIX_STRING);
   PMIX_INFO_LOAD(&info[1], PMIX_RANGE, &range, PMIX_DATA_RANGE);
   PMIX_INFO_LOAD(&info[2], "prte.pub.replace", NULL, PMIX_BOOL);
   rc = PMIx_Publish(info, 3);

``prte.pub.replace`` reaches **only the caller's own publications**.  A
collision with another process's key is refused with
``PMIX_ERR_DUPLICATE_KEY`` whether or not the directive was given, so this
is a republish and not a way to take a live name away from somebody else.
Only the keys being republished are dropped: a publication holding other
keys keeps them.

This attribute is PRRTE's own — the PMIx Standard defines none for the
purpose.  ``PMIx_Query_info`` reports it among the attributes supported for
``PMIx_Publish``.


How long data lasts
-------------------

``PMIX_PERSISTENCE`` says how long the datastore is to keep an item.  The
default is ``PMIX_PERSIST_APP``.

.. list-table::
   :header-rows: 1

   * - Persistence
     - Retained until
   * - ``PMIX_PERSIST_FIRST_READ``
     - the first lookup that returns it
   * - ``PMIX_PERSIST_PROC``
     - the publishing process terminates
   * - ``PMIX_PERSIST_APP``
     - the publishing application terminates
   * - ``PMIX_PERSIST_SESSION``
     - the DVM terminates
   * - ``PMIX_PERSIST_INDEF``
     - it is explicitly unpublished

PRRTE reclaims ``PMIX_PERSIST_PROC`` and ``PMIX_PERSIST_APP`` data when the
publishing **job** ends.  ``PMIX_PERSIST_PROC`` therefore lives slightly
longer than the Standard requires — until the job ends rather than until the
individual publisher exits — because a message to the datastore for every
terminating process is not a cost the termination path can carry at scale.

This matters most on a persistent DVM, where the store outlives any one job.
A value published with the default persistence is gone once its job ends, so
a later job can publish the same key; one published with
``PMIX_PERSIST_SESSION`` holds the key for the life of the DVM, and a later
job that wants it must have its original publisher take it back first.

``PMIx_Unpublish(NULL, ...)`` removes **everything** the calling process
published, regardless of persistence.
