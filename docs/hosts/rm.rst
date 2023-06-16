Resource Manager-Provided Hosts
===============================

When launching under a Resource Manager (RM), the RM usually
picks which hosts |mdash| and how many processes can be launched on
each host |mdash| on a per-job basis.

The RM will communicate this information to PRRTE directly; users can
simply omit specifying hosts or numbers of processes.
