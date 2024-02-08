Terminology
=================

PRRTE uses terms adopted from PMIx to describe its operations. Terms include:

``Workload Manager (WLM)`` is often called the ``scheduler`` and is responsible for
scheduling and assigning resources.

``Resource Manager (RM)`` is the runtime environment (RTE)

``Session`` refers to a set of resources assigned by the WLM that has been
reserved for one or more users.
A session is identified by a `session ID` that is
unique within the scope of the governing WLM.
Historically, HPC sessions have consisted of a static allocation of resources - i.e., a block of resources assigned to a user in response to a specific request and managed as a unified collection. However, this is changing in response to the growing use of dynamic programming models that require on-the-fly allocation and release of system resources. Accordingly, the term ``session`` in this project refers to a potentially dynamic entity, perhaps comprised of resources accumulated as a result of multiple allocation requests that are managed as a single unit by the WLM.

``Job`` refers to a set of one or more ``applications`` executed as a single invocation by the user within a session with a unique identifier, the ``job ID``, assigned by the RM or launcher. For example, the command line `mpiexec -n 1 app1 : -n 2 app2` generates a single MPMD job containing two applications. A user may execute multiple jobs within a given session, either sequentially or concurrently.

``Namespace`` refers to a character string value assigned by the RM to a job.  All applications executed as part of that job share the same namespace. The namespace assigned to each job must be unique within the scope of the governing RM and often is implemented as a string representation of the numerical ``Job ID``. The namespace and job terms will be used interchangeably throughout the project.

``Application`` represents a set of identical, but not necessarily unique,
execution contexts within a job.

``Process`` is assumed for ease of presentation to be an operating system process, also commonly referred to as a heavyweight process. A process is often comprised of multiple lightweight threads, commonly known as simply `threads`.

``Client`` refers to a process that was registered with the PMIx server prior to being started, and connects to that PMIx server via ``PMIx_Init`` using its assigned namespace and rank with the information required to connect to that server being provided to the process at time of start of execution.

``Tool`` refers to a process that may or may not have been registered with the PMIx server prior to being started and intializes using ``PMIx_tool_init``.

