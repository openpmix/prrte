Overview
========

While PRRTE does not itself contain a scheduler, it does support scheduler-related
operations as a means of providing users and researchers with an environment within
which they can investigate/explore dynamic operations. It will , of course, interact
with any PMIx-based scheduler to support allocation requests and directives for
executing jobs within an allocation.

In addition, however, PRRTE provides a pseudo-scheduler for research purposes. The
``psched`` daemon is not a full scheduler - i.e., it does not provide even remotely
optimal resource assignments, nor is it in any way production qualified. For example,
the ``psched`` algorithms are not particularly fast and are focused more on providing
functionality than supporting large volumes of requests.

Within that context, ``psched`` will:

* assemble a resource pool based on the usual PRRTE resource discovery methods. This
  includes reading the allocation made by a host environment, or provided by hostfile
  and/or dash-host command line options.

* upon receipt of allocation requests (either directly from tools or relayed by PRRTE),
  check to see if the specified resources are available.
