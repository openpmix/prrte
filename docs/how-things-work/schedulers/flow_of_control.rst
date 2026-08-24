Flow-of-Control
===============

Describe how allocation requests are flowed to the scheduler,
scheduler attachment upon startup of either side, where response
to allocation requests are returned and how they get back to
the requestor, where session controls are received for allocation
instantiation and how that is done, where session control is
called to indicate all jobs complete in session.

Probably best to start with an overview of how things flow
and then drill down into the respective steps.

Allocation requests have to be serviced by the scheduler. Since
a requestor could attach directly to the scheduler (e.g., in the
case of a tool submitting a session request) or to a daemon within
the RM (who must then relay it to its system controller for forwarding
to the scheduler), the scheduler must inform the RM of the allocation
via the ``PMIx_Session_control`` API - i.e., the RM cannot guarantee
its ability to intercept and process an allocation response to learn
of a session that needs to be instantiated.

PRRTE's role in the ``PMIx_Allocation_request`` flow is generally to
pass the request on to the scheduler, and transport the reply back
to the requestor.

``PMIX_ALLOC_ACTIVATE`` is the exception: PRRTE answers it itself,
without involving any resource manager. It asks for a daemon on nodes
the requestor **already holds** - an allocated node the DVM is not
spanning, because a ``--host``/``--hostfile`` given at startup narrowed
which nodes got one, or because a released reservation handed it back
without one. Nothing is added to the allocation and no slot count
changes, so there is nothing to ask a scheduler for and nothing a
scheduler could refuse; for the same reason the request is served even
where the scheduler owns the allocation, which is where a request for
new resources cannot be. The nodes are named with ``PMIX_HOST`` and/or
``PMIX_HOSTFILE``, in the same syntax the ``--activate`` command line
option takes, and the request is answered when it is granted - the
daemons themselves are reported by the ``PMIX_DVM_IS_READY`` event that
completes any DVM size change.

An allocation request can also arrive **on a spawn**, as
``PMIX_SPAWN_ALLOC``: the value is an array of ``pmix_info_t`` holding the
request's directive (``PMIX_ALLOC_REQ_DIRECTIVE``) and the info a standalone
request would have carried. PRRTE serves that request first, exactly as it
would serve the standalone one, and only launches the job once it is granted
- pointing the job at what it was given, so it maps onto those resources
rather than onto everything else. The two failures are kept apart: an
allocation that is refused fails the spawn with ``PMIX_ERR_JOB_ALLOC_FAILED``
and launches nothing, while a spawn that fails *after* the grant hands the
allocation back before returning its own error. This saves a caller the
"request, wait, read the id, spawn into it" sequence, and closes the window
in that sequence where resources are held for a job that does not exist.

PRRTE only creates a session object as a result of a call from the
scheduler via ``PMIx_Session_control``. What it does with each directive
of that API - and where it deviates from the letter of the attribute
description, as it must for preemption - is documented in
:ref:`Session Control <session-control-label>`.
