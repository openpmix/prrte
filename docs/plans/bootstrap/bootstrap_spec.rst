.. _bootstrap-spec-label:

DVM Bootstrap: Specification
============================

Purpose
-------

This document specifies the **bootstrap** method for instantiating a
persistent PRRTE DVM — the alternative to launching the DVM interactively
with the ``prte`` command.  In the bootstrap method there is *no launcher*:
instead of one process (the HNP) reaching out over ssh or a resource
manager to start a daemon on each node, an identical ``prted`` is started
independently on every node of the cluster — typically by the node's own
boot sequence (a systemd unit, an init script, or the scheduler's node
prolog) — and each daemon assembles itself into the DVM by reading a
shared configuration file.

The result is a system-level DVM: a persistent, full-service PMIx
environment that comes up with the cluster and is ready to host user
sessions on demand.  With an appropriately integrated scheduler this
provides a fully workload-managed environment.

This specification defines *what* the bootstrap method guarantees — the
configuration-file contract, how each daemon derives its own identity and
role with no launcher to tell it, and how the daemons wire themselves into
a single DVM — not the internal data structures or code paths, which the
companion implementation plan describes.  Where this specification and the
plan disagree about observable behavior, **this specification is
authoritative** and the plan must be corrected.

Operating model
---------------

The defining property of the bootstrap method is **uniform, launcher-less
start with self-election**:

* The *same* ``prted --bootstrap`` invocation is started on every node.
  No node is told at launch time what its rank is, whether it is the
  controller, or how to reach any peer.  The command line is identical
  everywhere, which is what makes the method suitable for a node boot
  sequence that cannot be specialized per host.

* Every daemon reads the *same* configuration file (``prte.conf``),
  pre-positioned identically on every node, and from it derives everything
  a launched daemon would normally have been handed by its launcher: its
  namespace, its rank, whether it is the DVM controller, the controller's
  contact information, and the full membership of the DVM.

* Exactly one daemon discovers, from the configuration, that it *is* the
  controller — the daemon running on ``DVMControllerHost`` — and promotes
  itself to be the HNP.  Every other daemon establishes itself as an
  ordinary ``prted`` and connects toward the controller.

This is a deliberate inversion of the ``prte``-command model, in which the
HNP is the root of a launch tree it actively constructs.  Under bootstrap
the daemons already exist; the configuration file is the only shared
knowledge, and the DVM forms because every daemon computes the *same* view
of the DVM from that file and therefore agrees on the tree without anyone
distributing it.

Configuration file contract
---------------------------

The configuration file is installed as ``<sysconfdir>/prte.conf`` and is
read by every bootstrapping process from that fixed location.  It is a
line-oriented ``Key=Value`` text file.  A line that is empty or whose
first character is ``#`` is ignored.  A non-comment line with no ``=``, an
empty key, or an empty value is a fatal configuration error that aborts
the bootstrap of the daemon that read it (via the ``help-prte-runtime.txt``
bootstrap diagnostics).

The keys that govern DVM formation are:

.. list-table::
   :header-rows: 1
   :widths: 24 12 64

   * - Key
     - Required
     - Meaning
   * - ``DVMNodes``
     - yes
     - The membership of the DVM: the complete, ordered set of nodes on
       which daemons run.  Expressed as a comma-delimited list of
       hostnames or hostname ranges (``linux0,linux[2:2-10]``), a PMIx
       native regular expression, or ``file:<path>`` naming a file of one
       hostname per line.  This list is the sole source of truth for DVM
       membership and — critically — for rank assignment (see `Identity
       derivation`_).
   * - ``DVMControllerHost``
     - yes
     - The hostname of the node that is to run the DVM controller.  The
       daemon that finds it is running on this host elects itself the HNP.
   * - ``DVMPort``
     - no (default 7817)
     - The single well-known TCP port on which every DVM process listens for
       its peers — the controller for its daemons, and each ``prted`` for its
       peer daemons.  A shared port lets any daemon construct any peer's
       contact address from the peer's host alone.
   * - ``ClusterName``
     - no (default ``cluster``)
     - Names the cluster; the DVM namespace is formed as
       ``<ClusterName>-prte-dvm``.  Distinct names keep DVMs on different
       clusters distinguishable when they share a database.
   * - ``KeepFQDNHostnames``
     - no (default ``false``)
     - Whether host names are matched and stored fully qualified.  When
       ``false`` (the default) a non-IP name is stripped to its short form
       before comparison, so ``DVMControllerHost`` and the entries in
       ``DVMNodes`` are matched against the local node's short name; when
       ``true`` the fully qualified names are compared as-is.  This is
       provided as a configuration key — rather than requiring the operator
       to set the ``prte_keep_fqdn_hostnames`` MCA parameter in a separate
       file — so that all bootstrap behavior lives in the single
       ``prte.conf``.  It simply seeds that same MCA variable.
   * - ``DVMNetworks``
     - no (default: all)
     - A comma-delimited list of networks or interfaces the runtime is to use
       for inter-node (daemon-to-daemon) communication — interface names or
       CIDR subnets.  Restricts the transport to the named networks; when
       omitted the runtime's default interface selection applies.  Provided
       as a configuration key so the transport can be pinned from the same
       ``prte.conf``; it seeds the existing ``prte_if_include`` MCA variable.
   * - ``DVMNetmask``
     - no (default: none)
     - The interface netmask associated with the inter-node network.  It is
       written into the controller contact information that each daemon
       synthesizes from the configuration, so the daemons agree on
       reachability without discovering it dynamically.  When omitted the
       runtime treats the synthesized contact as universally reachable.  The
       value follows the selected address family (``DVMIPVersion``).
   * - ``DVMIPVersion``
     - no (default ``4``)
     - The IP address family the DVM uses for inter-node communication:
       ``4`` (IPv4, the default) or ``6`` (IPv6-only).  It governs which
       transport the daemons listen and connect on, and the form of the
       synthesized controller contact URI.  When ``6`` is selected the
       IPv4 family is disabled; the remaining address-bearing keys
       (``DVMControllerHost``, ``DVMNodes``, ``DVMNetworks``, ``DVMNetmask``)
       are then interpreted as IPv6 values.  Selecting ``6`` requires a PRRTE
       built with IPv6 enabled, else the DVM fails to start with a clear
       diagnostic.

Precedence with MCA parameters
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Several keys — ``DVMNetworks``, ``KeepFQDNHostnames``, and ``DVMPort`` —
duplicate values an administrator could also set as MCA parameters.  They are
present in ``prte.conf`` deliberately, so that all DVM behavior can be managed
in **one place**.  Where a key and an MCA parameter set the same underlying
value, **the configuration file trumps the MCA parameter file**.  (A value
given explicitly on a command line still wins over both, so an operator can
still override the configuration for a one-off.)

Operational and logging keys (``DVMTempDir``, ``SessionTmpDir``,
``ControllerLogPath``, ``PRTEDLogPath``, ``ControllerLogJobState``,
``ControllerLogProcState``, ``PRTEDLogJobState``, ``PRTEDLogProcState``)
tune session-directory placement and state logging; they are documented in
:doc:`../../configuration` and do not affect DVM formation.  An unknown key
is silently ignored so that a newer configuration file remains usable by an
older daemon.

Identity derivation
-------------------

With no launcher to assign it, each daemon must compute its own PMIx
identity — a ``(namespace, rank)`` pair — from the configuration alone, and
every daemon must arrive at a view that is mutually consistent, or the
routing tree will not close.

Namespace
~~~~~~~~~

Every daemon in the DVM shares the single namespace
``<ClusterName>-prte-dvm``.  Because ``ClusterName`` is identical in every
copy of the configuration file, every daemon computes the same namespace.

Rank
~~~~

**The controller is always rank 0**, whether or not its node appears in
``DVMNodes``.  The remaining daemons are ranked by their **position in the
listed order of ``DVMNodes``**, skipping the controller's entry if it is
present, and numbered ``1, 2, 3, …`` in that order.  Concretely, every
daemon:

#. Parses ``DVMNodes`` into the same ordered list of node names.
#. Walks that list in order, assigning ranks ``1, 2, 3, …`` to each entry,
   but **omitting** any entry that matches ``DVMControllerHost`` — that node
   is rank 0, not a numbered position.
#. Locates its own node — matching its resolved host name and aliases under
   the ``KeepFQDNHostnames`` rule — and takes the rank thus computed.

Because the list, its order, and the controller identity are identical in
every copy of the configuration file, the assignment is globally consistent
with no coordination: daemon *i* and daemon *j* independently agree on each
other's rank.

Two membership cases follow directly, and both are supported:

* **Controller not listed in ``DVMNodes``** — the usual case, where the
  controller node runs no application processes.  The controller is rank 0
  and the ``N`` entries of ``DVMNodes`` take ranks ``1 … N``; the DVM has
  ``N + 1`` daemons.
* **Controller listed in ``DVMNodes``** — chosen when application processes
  *are* permitted on the controller node.  The controller is still rank 0;
  its entry is skipped during the ``1 … `` numbering, the other ``N − 1``
  entries take ranks ``1 … N − 1``, and the DVM has ``N`` daemons.  In this
  case the controller node is counted as a compute node of the DVM.

A daemon whose own node is neither ``DVMControllerHost`` nor found in
``DVMNodes`` is not a member of this DVM and must abort its bootstrap with a
clear diagnostic rather than guess a rank.

Controller election
~~~~~~~~~~~~~~~~~~~~

The same ``prted --bootstrap`` command is started on every node — there is
no separate controller command for a system administrator to place on one
special host.  Each ``prted`` compares its own node identity to
``DVMControllerHost``; the single daemon that matches **promotes itself**
into the controller.  It takes rank 0, initializes as the HNP
(``PRTE_PROC_MASTER``) rather than as an ordinary daemon, and listens on
``DVMPort``.  Every other ``prted`` initializes as ``PRTE_PROC_DAEMON``,
listens on the same ``DVMPort``, and treats the controller as its HNP.

This self-promotion is what lets the node boot sequence run one identical
command everywhere.  It requires the bootstrap path in ``prted.c`` to branch
on the election result and initialize as the HNP when it is the elected
controller, rather than unconditionally calling
``prte_init(PRTE_PROC_DAEMON)`` as the draft does today.

Wireup
------

Because every daemon derives the full membership and ordering of the DVM
from ``DVMNodes``, no daemon needs the nidmap distributed to it — each
computes the same map locally.  From that shared map a daemon determines
its parent in the DVM's routing tree and the contact information it needs
to reach it:

* The **controller's contact URI** is synthesized from the known facts:
  the DVM namespace, rank 0, ``DVMControllerHost``, ``DVMPort``, and the
  address family selected by ``DVMIPVersion`` (which fixes both the resolved
  address and the URI's transport form — IPv4 or IPv6).  A non-controller
  daemon builds this URI and uses it to phone home, exactly as a launched
  daemon uses the ``PRTE_PROC_MY_HNP`` URI its launcher provided.
* Each daemon's **own listening endpoint** is ``DVMPort``, the single
  well-known port shared across the DVM so that any daemon can construct any
  peer's contact URI from the peer's rank and node name without a discovery
  exchange.

Because the daemons boot asynchronously and independently, a non-controller
daemon may become ready before the controller is listening.  A daemon that
cannot yet reach the controller must therefore **retry** rather than fail
its bootstrap; it keeps attempting the connection until the controller
answers.  The retry cadence is governed by the existing RML/OOB
connection-retry MCA parameters and is left to the site's default MCA
parameter file — the bootstrap configuration introduces no new key for it.

The DVM is fully formed once every daemon has reported in to the controller
and the controller has confirmed the expected daemon count — ``N + 1`` when
the controller node is not in ``DVMNodes``, or ``N`` when it is (see
`Rank`_).

Two consumers, one parser
-------------------------

Bootstrap configuration is consumed in two places, and both must interpret
the file identically:

#. **Daemon-side (``ess``).**  Before ``prte_init``, each bootstrapping
   ``prted`` reads the configuration to establish its own identity, role,
   port, and the controller URI.  This is the path in
   ``src/mca/ess/base/ess_base_bootstrap.c``, invoked from ``prted.c`` when
   ``--bootstrap`` is present.
#. **Controller-side (``ras``).**  The elected controller must populate its
   node pool with the DVM's nodes.  The ``ras/bootstrap`` component
   (``src/mca/ras/bootstrap/``) becomes selectable when
   ``prte_bootstrap_setup`` is set and, in its ``allocate`` method, parses
   the same ``DVMNodes`` list into ``prte_node_t`` entries.

The ``DVMNodes`` regular-expression parser and the ``Key=Value`` file
reader are therefore required to be a **single shared implementation**
reachable from both the ``ess`` and ``ras`` paths, so the two consumers can
never diverge on how a given configuration file is interpreted.  (Today the
parser and reader are duplicated verbatim between the two files; unifying
them is a precondition for this contract — see `Current draft state`_.)

Current draft state
-------------------

A partial implementation exists and establishes the parsing groundwork but
does **not** yet form a DVM.  This section records the gap between the draft
and this specification so the implementation plan can close it.

What the draft does
~~~~~~~~~~~~~~~~~~~

* ``ess_base_bootstrap.c`` opens ``<sysconfdir>/prte.conf``, parses every
  documented key, validates that ``DVMNodes``, ``DVMControllerHost``, and a
  controller port are present, and expands ``DVMNodes`` (including
  ``file:`` and range syntax) into a node-name array.  It then merely
  **prints** the resulting node names and returns.
* ``ras/bootstrap`` performs the same parse and expansion and appends the
  node names to the caller's node list as ``prte_node_t`` objects — the
  controller-side node pool is thus partially wired.
* The ``--bootstrap`` CLI option is defined for both ``prte`` and
  ``prted``; ``prted.c`` calls ``prte_ess_base_bootstrap()`` and sets
  ``prte_bootstrap_setup``, and ``prte.c`` sets ``prte_bootstrap_setup``.

What the draft does *not* yet do
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* **No identity is derived.**  The daemon never computes its namespace from
  ``ClusterName``, never locates its own node in ``DVMNodes`` to assign its
  rank, and never populates ``PRTE_PROC_MY_NAME`` / ``prte_process_info``.
* **No controller election.**  Nothing compares the local node to
  ``DVMControllerHost``; the ``prted.c`` path always proceeds to
  ``prte_init(PRTE_PROC_DAEMON)``, so no daemon ever promotes itself to the
  HNP.
* **No contact information is built.**  The controller URI is never
  synthesized from ``DVMControllerHost``/``DVMPort``, and the
  ports parsed from the file are not applied to the RML listeners.
* **The operational and logging keys are parsed and immediately freed** —
  ``DVMTempDir``, ``SessionTmpDir``, the log paths and log-state toggles
  have no effect.
* **The parser is duplicated** between ``ess_base_bootstrap.c`` and
  ``ras_boot.c`` rather than shared, so the two consumers can drift.

Conformance summary
-------------------

A conforming implementation guarantees that:

#. An identical ``prted --bootstrap`` started on every node of the cluster,
   with the same ``prte.conf`` pre-positioned on each, forms a single
   persistent DVM with no interactive launcher and no per-node
   specialization of the command line.
#. Every daemon derives the same DVM namespace (``<ClusterName>-prte-dvm``)
   and a globally consistent rank assignment from the canonical ordering of
   ``DVMNodes``, with the controller at rank 0.
#. Exactly one daemon — the one on ``DVMControllerHost`` — elects itself the
   HNP; every other daemon establishes itself as a ``prted`` and connects
   toward the controller using contact information synthesized entirely from
   the configuration file.
#. A daemon whose node is absent from ``DVMNodes``, or a configuration file
   that is missing a required key or is malformed, aborts that daemon's
   bootstrap with a clear diagnostic rather than joining the DVM with a
   guessed identity.
#. The daemon-side (``ess``) and controller-side (``ras``) consumers
   interpret the configuration file — the ``Key=Value`` reader and the
   ``DVMNodes`` parser — through a single shared implementation, so the two
   can never diverge.
#. The DVM is reported as fully formed only once every node named in
   ``DVMNodes`` has reported in to the controller.

Resolved design decisions
-------------------------

The following decisions are settled and are reflected in the contract
above; they are collected here for reference.

* **Rank ordering and the controller's index.**  The controller is always
  rank 0.  The remaining daemons are ranked by their position in the listed
  order of ``DVMNodes``, skipping the controller's entry if present, and
  numbered from 1 (see `Rank`_).  There is no requirement that
  ``DVMControllerHost`` be the first entry in ``DVMNodes``, and it need not
  appear in ``DVMNodes`` at all.
* **Whether the controller is counted in ``DVMNodes``.**  The controller
  node is counted as a member (a compute node) of the DVM **if and only if**
  its host appears in ``DVMNodes``.  When it does, the expected daemon count
  is the cardinality of ``DVMNodes``; when it does not, the count is that
  cardinality plus one.  The common case runs no application processes on
  the controller node and so omits it from ``DVMNodes``.
* **Host matching.**  Matching of the local node against
  ``DVMControllerHost`` and the ``DVMNodes`` entries uses PRRTE's existing
  host-matching behavior (resolved name plus aliases, IP addresses compared
  as-is).  Whether names are compared fully qualified or short-form is
  controlled by the ``KeepFQDNHostnames`` configuration key, which seeds the
  existing ``prte_keep_fqdn_hostnames`` MCA variable so the choice can live
  in ``prte.conf`` rather than a separate MCA parameter file.
* **Startup race and retry.**  A non-controller daemon that finds the
  controller not yet listening retries the connection until it succeeds
  rather than failing its bootstrap.  The retry cadence uses the existing
  RML/OOB connection-retry MCA parameters and is left to the site's default
  MCA parameter file; no new bootstrap key is introduced for it.
* **Controller entry point.**  The controller is a **self-promoted
  ``prted``**.  A system administrator boots the identical
  ``prted --bootstrap`` on every node, and the daemon on
  ``DVMControllerHost`` promotes itself to the HNP.  There is no separate
  controller command to place on a special node.  To keep exactly **one
  bootstrap story**, ``--bootstrap`` is removed from the ``prte`` command;
  bootstrap is reachable only through ``prted --bootstrap``.
