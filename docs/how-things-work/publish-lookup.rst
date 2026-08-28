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

Published data is owned by the **user** that published it, and only its
owner may remove it.  An unpublish from another user reports success and
removes nothing, because there was nothing of *theirs* to remove.

The owner is the effective uid recorded when the item was published (and
the gid, where both are known) — not the publishing process.  A later job
of the same user, in a namespace of its own, may therefore take back a name
its predecessor left behind.  That matters because a process takes no data
with it when it exits: were the test the publishing process, an item
published by a job that has ended — or that crashed before it could
unpublish — would be readable by its own user's next job, unusable by it
(publishing over it is a duplicate) and removable by nobody at all, for the
life of the DVM.

Ownership is not access, and neither implies the other.  Naming a uid in
``PMIX_ACCESS_USERIDS`` lets that user *read* the item and never remove it;
and a publisher whose own accessor list excludes it may still remove what it
cannot read, because it owns it.

Two generations of a job that **overlap** are still constrained, but only by
the duplicate rule.  A replacement server starting while its predecessor is
still running cannot simply publish the predecessor's name — that is a
duplicate — but if the two run as the same user it may unpublish it first,
or publish over it in one step with ``prte.pub.replace``.  Across users
neither is possible, and the remedies are the ones MPI offers: have the
outgoing server unpublish, publish on a range the newcomer's readers will
search, or use a distinct key.  What is not possible in any case is for the
newcomer to believe it succeeded when it did not.

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

``prte.pub.replace`` reaches **only the caller's own publications**, where
"own" is the same ownership rule an unpublish applies: the publishing user.
A collision with another *user's* key is refused with
``PMIX_ERR_DUPLICATE_KEY`` whether or not the directive was given, so this
is a republish and not a way to take a live name away from somebody else.
It has to be scoped that way rather than to the process: once a same-user
process may unpublish a key and then publish its own, the two-step is
available anyway, and refusing the one-step form would only make the same
outcome take two calls.

Only the keys being republished are dropped: a publication holding other
keys keeps them.

This attribute is PRRTE's own — the PMIx Standard defines none for the
purpose.  ``PMIx_Query_info`` reports it among the attributes supported for
``PMIx_Publish``.


How long data lasts
-------------------

``PMIX_PERSISTENCE`` says how long the datastore is to keep an item.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Persistence
     - Retained until
   * - ``PMIX_PERSIST_FIRST_READ``
     - the first lookup that returns it |mdash| or, if no lookup ever asks
       for it, the retention timeout below
   * - ``PMIX_PERSIST_PROC``
     - the publishing process terminates
   * - ``PMIX_PERSIST_APP``
     - the publishing **application** terminates
   * - ``PMIX_PERSIST_NSPACE``
     - the publishing process's **namespace** terminates |mdash| every
       application in the job.  **This is PRRTE's default.**
   * - ``PMIX_PERSIST_SESSION``
     - the session |mdash| the allocation the publisher was running within
       |mdash| is torn down.  For a DVM with no allocation of its own, that
       is the end of the DVM.
   * - ``PMIX_PERSIST_INDEF``
     - it is explicitly unpublished, or the retention timeout below expires

Two of those need saying plainly, because they are easy to read past.

**An application is not a job.**  ``PMIX_PERSIST_APP`` means the publishing
process's app context.  A single ``prun`` may launch several |mdash| an MPMD
job |mdash| and they all share the one namespace assigned to the job without
having to terminate together, so an app that exits early takes its ``APP``
data with it while the job runs on.  MPI hides this by requiring a job's
applications to terminate together, which is an MPI rule and not a general
one.  A publisher that means "until my job is over" wants
``PMIX_PERSIST_NSPACE``.

**The default is ``PMIX_PERSIST_NSPACE``, not the Standard's
``PMIX_PERSIST_APP``.**  Applying the Standard's default literally, once
``APP`` is read correctly, would shorten the retention every unmarked
publish in an MPMD job has been getting.  ``NSPACE`` is that same lifetime,
now stated rather than implied.  A publisher that wants application lifetime
asks for ``APP`` and gets it.

``PMIx_Unpublish(NULL, ...)`` removes **everything the calling process
published**, regardless of persistence.  That form is scoped to the process
even though removal by key is scoped to the user: it is a blunt sweep whose
scope is implicit, and a job tidying up after itself at teardown must not
take out its own user's other running jobs.  Reaching across a namespace
boundary requires naming the key.


Data that names no lifetime, and the store that holds it
--------------------------------------------------------

Two of the persistences above name a criterion a running system may never
reach.  ``PMIX_PERSIST_INDEF`` is retained "until specifically deleted", and
only its owner may delete it |mdash| so on a persistent DVM it is a
permanent allocation made by a process that may be long gone.  An
unread ``PMIX_PERSIST_FIRST_READ`` item is the same shape: job A publishes a
name for job B to pick up, and the user then decides not to run job B.

PRRTE bounds both, with two MCA parameters.

.. list-table::
   :header-rows: 1
   :widths: 34 12 54

   * - Parameter
     - Default
     - Meaning
   * - ``prte_data_server_timeout``
     - ``300``
     - Seconds of **idleness** after which a ``PMIX_PERSIST_INDEF`` or
       unread ``PMIX_PERSIST_FIRST_READ`` item is removed.  ``0`` disables
       the timeout.
   * - ``prte_data_server_max_size``
     - ``16777216``
     - Maximum bytes of published data one datastore will hold **for any
       one uid**.  Reaching it evicts that uid's own least recently used
       items.  ``0`` disables the cap.

The timeout is an **idle** timeout, not a lifetime.  The clock starts when
the item is published and restarts on every lookup that returns one of its
keys, so a rendezvous name in active use is never pulled out from under its
readers |mdash| what expires is a name nobody has asked for in five minutes.
It applies to nothing else: a persistence that names a lifetime has a
criterion the runtime does reach, and cutting it short would break the
retention its publisher was promised.  An item is removed no earlier than
its timeout, and normally within a fraction of it afterwards.

The cap is applied **per publishing user**, and eviction never crosses a uid
boundary: a user who fills the store evicts only their own data, and no
publisher can be made to lose an item by anything another user does.
Without that scoping the cap would be an abuse primitive in its own right,
since publishing junk in bulk would become a way to push somebody else's
name out of the store.  A single publish larger than the whole cap is
refused with ``PMIX_ERR_OUT_OF_RESOURCE`` and evicts nothing.  The first
eviction for a user prints a warning naming the limit and this parameter.

A reader whose item expired or was evicted gets ``PMIX_ERR_NOT_FOUND``,
exactly as if the item had never been published.  There is no separate
status for "it was here and we dropped it": a publisher cannot distinguish
eviction from a reader that never arrived, and is not meant to.

.. note:: Both parameters are read once at startup by every daemon, so
   ``prte --prtemca prte_data_server_timeout 600 ...`` sets them
   DVM-wide.  Neither can be changed while the DVM is running.
