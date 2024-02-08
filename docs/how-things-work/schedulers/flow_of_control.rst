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

PRRTE's role in the ``PMIx_Allocation_request`` flow is simply to
pass the request on to the scheduler, and transport the reply back
to the requestor.

PRRTE only creates a session object as a result of a call from the
scheduler via ``PMIx_Session_control``.
