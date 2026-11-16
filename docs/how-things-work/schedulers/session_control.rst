.. _session-control-label:

Session Control
===============

A *session* in PRRTE is an allocation: a named set of nodes, together with
the jobs running on them. The ``PMIx_Session_control`` API is how a
scheduler creates, operates on, and reclaims one. This section describes
what PRRTE does with each directive of that API, and — where PRRTE cannot
do exactly what the directive describes — what it does instead and why.

Allocation requests and session control
---------------------------------------

Two PMIx APIs touch sessions, and they do different jobs:

* ``PMIx_Allocation_request`` is how a user *asks* for resources to be
  assigned to it: a new allocation, or more nodes or more time for one it
  already holds (``PMIX_ALLOC_EXTEND``). The scheduler decides, and answers
  the requester.

* ``PMIx_Session_control`` issues directives to a session. Its
  ``PMIX_SESSION_INSTANTIATE`` and ``PMIX_SESSION_EXTEND`` directives are not
  requests for resources: they are how the scheduler tells the DVM what it
  has decided — start a session over these nodes for this user, or give this
  session more nodes or more time.

The answer to an allocation request may carry much the same information, but
it goes to the requester, not to the DVM, and the DVM cannot count on seeing
it: the requester may have been a tool rather than one of the DVM's own
processes, so the DVM may not know the request was made at all. And a
scheduler sometimes changes a session for reasons no request asked for. So
the scheduler tells the DVM directly, with session control.

Both produce the same PRRTE object, a session; they differ in who may ask.

Who answers a request
---------------------

A session control request can reach PRRTE from the scheduler, from a tool,
or from an application process, and it can arrive at any daemon in the DVM.
Where it is decided depends on who asked:

* A request arriving at a **non-master daemon** is relayed to the DVM master.
  The master applies the same rules it would to a request that arrived
  directly, so a caller gets the same answer whichever daemon it happens to
  be attached to.

* A request that **originated with the scheduler** is a directive *to* this
  DVM, and the DVM master executes it.

* A request from **anyone else**, when a scheduler is attached, is passed up
  to the scheduler. The scheduler owns the decision and calls back down to
  the DVM to carry it out. The requestor's identity is added to the
  forwarded request as ``PMIX_REQUESTOR``.

* A request from anyone else when **no scheduler is attached** is executed by
  the DVM master, which is the only authority present. This is what makes
  session control usable on a standalone ``prte`` DVM.

In the last case the requestor must be entitled to act on the session. A
session may be created or extended - the directives a scheduler would
otherwise give - by root, by the user the DVM runs as, or by a user or
group named when the DVM was started (``prte --rtos
users=<list>,groups=<list>``, each a ``:``-separated list of names or
numbers), and that holds for the user a session is for as well. A user
asks for more for its own session with ``PMIx_Allocation_request``. To
pause, resume, signal or terminate an existing session, it is enough to be
the user it was granted to - or a member of its group, when the scheduler
named that group with ``PMIX_ALLOC_GID``; a group taken from whoever asked
is recorded but shared with no one. Ownership is by user, not by namespace, so a
second command run by the same person reaches an allocation their first
command created, even though the tool namespace of the first command is
long gone.

Operations
----------

The operational directives are alternatives, not a program: naming more than
one in a single request is refused with ``PMIX_ERR_BAD_PARAM``. A session id
of ``UINT32_MAX`` applies the operation to every session the requestor
controls (never to the DVM's own unreserved node pool).

Instantiation
^^^^^^^^^^^^^

``PMIX_SESSION_INSTANTIATE`` creates the session named by the request's
session id. The resources it is to hold are named by ``PMIX_SESSION_RESOURCES``
(an array that may carry ``PMIX_ALLOC_NODE_LIST``, ``PMIX_ALLOC_ID``,
``PMIX_ALLOC_REQ_ID``, ``PMIX_ALLOC_TIME`` and ``PMIX_ALLOC_INHERITANCE``), or
by those same keys given directly on the request. ``PMIX_ALLOC_ID`` is
required: it is the id the scheduler assigned to the allocation, and the one
the session is named by in allocation requests from then on. An
instantiation without one is refused with ``PMIX_ERR_BAD_PARAM``, and one
whose id already names a session with ``PMIX_ERR_EXISTS``. Those nodes join the DVM's
node pool and are then withheld from general use: a job may map onto them only
if it is running in this session.

``PMIX_SESSION_APP`` supplies the applications to start once the session
exists, and ``PMIX_SESSION_JOB`` the job-level directives that go with them
(job info without apps is an error, as is either one outside an
instantiation). The job is launched into the new session. If the DVM had to
be extended across newly-named nodes, the job waits until those daemons are
up before it is mapped.

The answer carries ``PMIX_SESSION_ID``, the session's ``PMIX_ALLOC_ID``, and — when apps were given — the ``PMIX_NSPACE`` of the launched job.
For a request that named apps, the answer is deferred until the launch
completes, so a successful return means the job is running.

Two constraints are worth knowing:

* **A session must be given resources if it is given apps.** A session
  withholds the nodes named for it, so one created with no resource
  description holds nothing and no job in it can be mapped. PRRTE refuses
  such a request up front rather than letting it appear to succeed and then
  fail in the mapper with "no nodes are available".

* **An instantiation that names no apps is a standing reservation.** It
  persists until it is explicitly terminated (or its time limit expires). One
  that *does* name apps exists in order to run them, and is reclaimed when the
  last of them retires — at which point its nodes return to the general pool
  and, if a scheduler instantiated it, ``PMIX_SESSION_COMPLETE`` is sent.

* **A node belongs to one session at a time.** A request naming a node that
  another session holds is refused; the node becomes available again when
  that session ends or releases it.

* **Only an elastic DVM grows.** A node that is not yet part of the DVM can be
  given to a session only if the DVM was started with
  ``--prtemca prte_elastic_mode 1``. Otherwise the request is refused, and
  nodes the DVM already runs on can still be reserved.

Pause and resume
^^^^^^^^^^^^^^^^

``PMIX_SESSION_PAUSE`` stops every process of every job in the session with
``SIGSTOP``; ``PMIX_SESSION_RESUME`` continues them with ``SIGCONT``. PRRTE
tracks which jobs it has stopped, so a resume does not signal a job that was
never paused and a pause does not re-signal one that already is.

Note what this does **not** do: the processes keep their memory and their
slots. PRRTE has no checkpoint facility with which to vacate a paused job and
reinstate it later.

Preempt and restore
^^^^^^^^^^^^^^^^^^^

``PMIX_SESSION_PREEMPT`` and ``PMIX_SESSION_RESTORE`` are the same mechanism
applied to selected jobs: the jobs named by ``PMIX_NSPACE`` qualifiers, or all
of them if none is given.

**PRRTE's preemption is suspension-based, and this is a deliberate
deviation.** The attribute describes preemption as halting the jobs *and
recovering their resources*. Recovering them would mean checkpointing the
jobs out of memory and restoring them later; PRRTE has no such facility, and
terminating them instead would make the paired ``PMIX_SESSION_RESTORE``
impossible to honor at all. So a preempted job stops and holds its
resources, and a restored job resumes exactly where it was. A scheduler that
genuinely needs the nodes back should terminate the session.

Signal
^^^^^^

``PMIX_SESSION_SIGNAL`` delivers the given signal to every process of every
job in the session, or to the jobs named by ``PMIX_NSPACE`` qualifiers.

Terminate
^^^^^^^^^

``PMIX_SESSION_TERMINATE`` kills every job in the session. A job that was
paused is continued first, since a stopped process cannot act on a
termination order. The session itself is reclaimed once the last job has
retired, so that its nodes are not pulled out from under processes still
being killed.

Extend
^^^^^^

``PMIX_SESSION_EXTEND`` adds nodes to a session (``PMIX_ALLOC_NODE_LIST``,
which also extends the DVM across them), revises its time limit
(``PMIX_ALLOC_TIME`` or ``PMIX_TIMEOUT``), or revises what becomes of its
nodes at the end (``PMIX_ALLOC_INHERITANCE``). An extend that extends nothing
is an error. Like instantiation, it is the scheduler's directive (see
`Allocation requests and session control`_), so with no scheduler attached
it is accepted only from those who may create a session. A host given as
``user@host``, here or on instantiation, names an account to reach it as,
which is refused from anyone: the DVM reaches every node as its own user.

Separation
^^^^^^^^^^

``PMIX_SESSION_SEP`` detaches a session from the namespace that owns it, so
that the two terminate independently. It may accompany any other operation, or
stand alone to change the disposition of an existing session.

**A session created through this API is detached by default**, which is the
opposite of an allocation reservation and is deliberate. A reservation is
nodes a tool took for its own use, and it dies with that tool. A session is
named, addressable by anyone authorized, and destroyed by an explicit
terminate, by its own time limit, or — when it was created to run apps — when
those apps finish. Tying it to the requester instead would make the API
unusable for the thing it exists to do: the requester is a tool, its namespace
lasts only as long as the request, and the session would be reclaimed out from
under its own jobs the moment the caller disconnected. Giving
``PMIX_SESSION_SEP`` a **false** value on the instantiation opts back into
requester lifetime.

Completion
^^^^^^^^^^

``PMIX_SESSION_COMPLETE`` is what the RTE says to its scheduler, not the
other way round: PRRTE is the only party that knows when the jobs have
finished and the nodes are free. A request carrying it is refused with
``PMIX_ERR_NOT_SUPPORTED``.

PRRTE **sends** it when a session the scheduler instantiated is reclaimed.
The notification carries the session id, the allocation id, and a
``PMIX_JOB_INFO_ARRAY`` for each job that ran in the session, giving that
job's ``PMIX_NSPACE`` and ``PMIX_JOB_TERM_STATUS``. Those statuses are
accumulated as the jobs retire, because the job objects themselves do not
survive to the end of the session. The PRRTE-side teardown is completed
before the notification is sent, so the claim it makes — that the resources
have been recovered — is already true when the scheduler receives it.

Session lifetime
----------------

A scheduler may impose a time limit on a session with ``PMIX_ALLOC_TIME`` (in
``months:days:hours:minutes:seconds``, scanned from the right, so a bare
``"2"`` is two seconds) or ``PMIX_TIMEOUT`` (in seconds) on the instantiation.
When the limit expires PRRTE terminates the session exactly as an explicit
``PMIX_SESSION_TERMINATE`` would. ``PMIX_SESSION_EXTEND`` re-arms the limit.
No limit is in force unless one was asked for.

With no scheduler attached, an allocation request (``PMIx_Allocation_request``)
may ask for a time limit with ``PMIX_ALLOC_TIME`` as well, on
``PMIX_ALLOC_NEW`` or ``PMIX_ALLOC_EXTEND``, and the DVM applies it the same
way. A limit set by session control, or by a request from root, the DVM's own
user or a user named by ``--rtos users=``/``groups=``, cannot then be changed
by an allocation request from the session's owner.

When a session is reclaimed, ``PMIX_ALLOC_INHERITANCE`` decides what becomes
of its nodes: ``PMIX_ALLOC_INHERIT_NONE`` and ``PMIX_ALLOC_INHERIT_CHILD``
hand them back to the scheduler, and the default returns them to the DVM's
general pool. Handing the nodes away is the destructive choice, so it is the
one a scheduler has to ask for rather than the one it gets by saying nothing.
The DVM master's own node is never shrunk out of its own DVM, even when it is
a member of the reservation being handed back.

Handing nodes back shrinks the DVM, so it needs a scheduler to hand them to
and a DVM that can change size (``prte_elastic_mode``). Without either, the
nodes go back to the DVM's general pool instead, daemons and all, whatever
the disposition says. The same holds for a release of a reservation, by
allocation id or by naming its nodes: with no scheduler attached the nodes
return to the general pool. Taking a node out of a DVM that has no
scheduler is a release of a node of the general pool, which only those who
may change the DVM may ask for.

What PRRTE does not support
---------------------------

* **Provisioning.** ``PMIX_SESSION_PROVISION_NODES`` and
  ``PMIX_SESSION_PROVISION_IMAGE`` are refused with
  ``PMIX_ERR_NOT_SUPPORTED``. PRRTE runs on the machines it is given, in the
  state it finds them.

* **Resource selection.** A resource description that asks PRRTE to *choose*
  machines — a node count, a cpu count, a memory size, a queue — is refused.
  PRRTE is the thing being scheduled; naming the nodes is the scheduler's
  job. Node lists (plain, regex, or regex2) are what PRRTE acts on.

* **Who a session belongs to.** A session belongs to a user and that
  user's group, and they are who may later act on it. A scheduler
  instantiating a session on a user's behalf must name them with
  ``PMIX_ALLOC_UID`` and ``PMIX_ALLOC_GID``, given together; an
  instantiation from the scheduler without them is refused with
  ``PMIX_ERR_BAD_PARAM``. Only the attached scheduler may name them; from
  anyone else the request is refused with ``PMIX_ERR_NO_PERMISSIONS``. A
  session created by anyone else, when no scheduler is attached, belongs to
  the user and group PMIx identified for the request. ``PMIX_USERID`` and
  ``PMIX_GRPID`` cannot say whom a session is for: PMIx sets them to the
  identity of the process that made the request.

  The applications a session is created to run (``PMIX_SESSION_APP``) are
  its owner's, and run as that user. A DVM not run as root cannot start
  processes as another user, and refuses to start a session's applications
  whose owner it cannot run them as.

An unrecognized directive is not an error. A request may be addressed at more
than one kind of RTE, and refusing on a key PRRTE simply has no opinion about
would break a caller it otherwise serves.

Trying it out
-------------

``examples/sessionctrl.c`` is a tool that drives each of these operations
against a running DVM. Because a DVM with no scheduler answers for itself,
it works against an ordinary ``prte --daemonize``::

    prte --daemonize
    ./sessionctrl instantiate 42 --hosts $(hostname -s) -- /bin/sleep 300
    ./sessionctrl pause 42
    ./sessionctrl resume 42
    ./sessionctrl signal 42 15
    ./sessionctrl terminate 42
    pterm
