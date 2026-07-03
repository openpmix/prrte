.. _bootstrap-plan-label:

DVM Bootstrap Implementation Plan
=================================

This document describes the implementation that turns the partial bootstrap
draft into a working launcher-less DVM.  The externally observable contract
it delivers — the configuration-file semantics, identity derivation,
controller self-election, and wireup — is specified in
:ref:`bootstrap-spec-label`, which is authoritative for observable behavior.
Where this plan and that specification disagree, the specification wins and
this plan must be corrected.

Approach
--------

The guiding principle is **reuse the existing daemon-startup plumbing
instead of building a parallel one**.  A normally-launched ``prted`` receives
its identity, its DVM size, and its HNP's contact URI from environment
variables (MCA parameters) that its launcher sets before ``exec``; the
``ess`` and ``rml`` layers then read those parameters during ``prte_init``.
Bootstrap has no launcher, but it runs ``prte_ess_base_bootstrap()`` *before*
``prte_init`` and can set exactly the same environment.  So the bootstrap
code's job is narrow and well-defined:

#. Parse ``prte.conf`` (shared parser, Step 1).
#. Compute this node's identity and role from the parsed values (Step 2).
#. Express that identity through the **same MCA parameters a launcher would
   have set** (Steps 3–7).
#. Choose the process type passed to ``prte_init`` — HNP for the controller,
   ordinary daemon otherwise (Step 8).

Everything downstream — name assignment, routing-tree computation, RML
contact setup, session directories — then happens through the unchanged
existing code paths.  The structural changes outside the bootstrap files are
small and confined to Step 8: ``prted.c`` branches on the election result to
initialize as the HNP when it is the controller, and the redundant
``--bootstrap`` handling is removed from ``prte`` (``prte.c`` and
``schizo_prte.c``).

The following table is the crux of the design: for each thing a launcher
normally provides, it names the MCA parameter / environment variable
bootstrap sets and where that value is consumed.

.. list-table::
   :header-rows: 1
   :widths: 34 30 36

   * - Value
     - Set by bootstrap as
     - Consumed by
   * - Controller namespace + rank
     - ``PMIX_SERVER_NSPACE`` = ``<ClusterName>-prte-dvm``,
       ``PMIX_SERVER_RANK`` = ``0``
     - ``prte_plm_base_set_hnp_name()`` (uses these directly, no ``@0``
       suffix) — controller only
   * - Daemon namespace
     - ``PRTE_MCA_ess_base_nspace`` = ``<ClusterName>-prte-dvm``
     - ``ess/env`` ``env_set_name()`` — non-controller daemons
   * - Daemon rank (vpid)
     - ``PRTE_MCA_ess_base_vpid`` = computed rank
     - ``ess/env`` ``env_set_name()``
   * - DVM daemon count
     - ``PRTE_MCA_ess_base_num_procs`` = daemon count
     - ``ess/env`` (sets ``prte_process_info.num_daemons``, which drives
       ``prte_rml_compute_routing_tree()``)
   * - Controller contact URI
     - ``PRTE_MCA_prte_hnp_uri`` = synthesized URI (Step 4)
     - ``prte_process_info.my_hnp_uri`` → ``rml.c`` non-master branch
   * - Listening port
     - ``PRTE_MCA_prte_static_ipv4_ports`` **or**
       ``PRTE_MCA_prte_static_ipv6_ports`` = ``DVMPort`` (family per
       ``DVMIPVersion``; every process)
     - ``oob/tcp`` listener
   * - Address family
     - ``PRTE_MCA_prte_disable_ipv6_family`` / ``..._ipv4_family`` = ``0``/``1``
       (per ``DVMIPVersion``)
     - ``oob/tcp`` family selection
   * - Inter-node networks
     - ``PRTE_MCA_prte_if_include`` = ``DVMNetworks``
     - ``prte_if_include`` (interface/subnet selection in ``oob/tcp``)
   * - Interface netmask
     - written into the synthesized ``PRTE_MCA_prte_hnp_uri`` mask field
       (Step 4); not a standalone MCA parameter
     - ``set_addr()`` reachability filtering
   * - FQDN matching
     - ``PRTE_MCA_prte_keep_fqdn_hostnames`` = ``0``/``1``
     - ``prte_keep_fqdn_hostnames`` (host matching in ``proc_info.c``)

Because bootstrap runs before ``prte_init`` opens and registers any
framework, setting these in the environment (with ``PMIx_Setenv``) is
sufficient: each framework's ``register`` reads its value from the
environment as usual.

Precedence: config file trumps the MCA param file
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Several of the keys above (``DVMNetworks``, ``KeepFQDNHostnames``, ``DVMPort``)
duplicate values an administrator could otherwise set as MCA parameters.
They are included in ``prte.conf`` deliberately, so a site can manage all DVM
behavior in **one place**.  The required precedence is that **a value set in
``prte.conf`` overrides the same value in an MCA parameter file**.

This falls out naturally from setting the values as *environment* MCA
parameters: PMIx MCA precedence already ranks an environment variable above a
parameter *file*, and a command-line ``--prtemca`` above the environment.  So
bootstrap sets each config-derived value with ``PMIx_Setenv(..., overwrite =
true)``: it beats the operator's default MCA param file (as required) while
still yielding to an explicit command-line override.  The one exception is a
value bootstrap supplies as its *own* fallback default rather than from the
config file — notably the retry parameters of Step 7, which have no config
key — where ``overwrite = false`` is used so any operator MCA setting still
wins.

Step 1 — Factor the config parser into a shared utility
-------------------------------------------------------

The ``Key=Value`` reader and the ``DVMNodes`` regular-expression expander are
today **duplicated verbatim** between ``src/mca/ess/base/ess_base_bootstrap.c``
and ``src/mca/ras/bootstrap/ras_boot.c``.  The spec requires the two consumers
to interpret a configuration file identically, so the parser must become a
single implementation both call.

Create ``src/util/prte_bootstrap.c`` / ``src/util/prte_bootstrap.h`` holding:

.. code-block:: c

   /* All values parsed from prte.conf, owned by the struct. */
   typedef struct {
       char     *cluster;          /* ClusterName (default "cluster") */
       char     *ctrlhost;         /* DVMControllerHost (required) */
       uint32_t  port;             /* DVMPort (default 7817) */
       int       ip_version;       /* DVMIPVersion: 4 (default) or 6 */
       int       radix;            /* DVMRadix (default 64) */
       uint32_t  connect_max_time; /* DVMConnectMaxTime seconds (default 30) */
       char    **nodes;            /* expanded DVMNodes (required) */
       bool      keep_fqdn;        /* KeepFQDNHostnames (default false) */
       char     *dvm_networks;     /* DVMNetworks (default NULL -> all) */
       char     *dvm_netmask;      /* DVMNetmask (default NULL -> empty) */
       uint32_t  retry_max_delay;  /* DVMRetryMaxDelay seconds (default 5) */
       char     *dvmtmpdir;
       char     *sessiontmpdir;
       char     *ctrllogpath;
       char     *prtedlogpath;
       bool      ctrl_log_jobstate, ctrl_log_procstate;
       bool      prted_log_jobstate, prted_log_procstate;
   } prte_bootstrap_config_t;

   /* Read <sysconfdir>/prte.conf, validate required keys, expand DVMNodes.
    * Emits the help-prte-runtime.txt bootstrap diagnostics on error. */
   int prte_bootstrap_parse(prte_bootstrap_config_t *cfg);
   void prte_bootstrap_config_free(prte_bootstrap_config_t *cfg);

Move the file reader, ``regex_extract_nodes``, ``regex_parse_value_ranges``,
``regex_parse_value_range``, and ``read_file`` (all currently duplicated) into
this file as the single implementation.  The keys the draft does not yet
parse — ``KeepFQDNHostnames``, ``DVMNetworks``, ``DVMNetmask``,
``DVMIPVersion``, ``DVMRadix``, ``DVMConnectMaxTime``, ``DVMRetryMaxDelay``,
and the log-state booleans — are added
to the parser here, and the split ``DVMControllerPort``/``PRTEDPort`` keys are
collapsed to the single ``DVMPort``.  ``ess_base_bootstrap.c`` and
``ras_boot.c`` are reduced to callers of ``prte_bootstrap_parse()``.

Register the new object files in ``src/util/Makefile.am``.

Step 2 — Compute identity and elect the controller
---------------------------------------------------

This is the new logic the draft lacks entirely.  It lives in
``prte_ess_base_bootstrap()`` and runs after ``prte_bootstrap_parse()``.

**Namespace.**  The DVM namespace is ``<cfg.cluster>-prte-dvm``, identical on
every node.

**Controller election.**  Compare the local node to ``cfg.ctrlhost`` using
the existing ``prte_check_host_is_local()`` helper (which already handles
aliases and IP addresses); the ``cfg.keep_fqdn`` value is applied first so
the comparison is short-form or fully-qualified per the configuration.  The
single node that matches is the controller.

**Rank assignment** follows the resolved rule from the spec:

.. code-block:: text

   rank_of(node):
       if node == ctrlhost:            return 0
       r = 1
       for entry in DVMNodes (listed order):
           if entry == ctrlhost:       continue     # controller is rank 0
           if entry == node:           return r
           r = r + 1
       return NOT_FOUND                              # not a member -> abort

Every daemon runs this against its own node.  The **daemon count** is
``len(DVMNodes)`` when ``ctrlhost`` appears in ``DVMNodes`` (it is counted as
a compute node) and ``len(DVMNodes) + 1`` when it does not.  A daemon that is
neither the controller nor found in ``DVMNodes`` fails the bootstrap with a
diagnostic (a new ``help-prte-runtime.txt`` entry, e.g.
``bootstrap-node-not-member``) rather than guessing a rank.

The function computes ``(is_controller, my_rank, num_daemons)`` and passes
them to Step 3.

Step 3 — Publish identity through the existing MCA plumbing
-----------------------------------------------------------

With identity computed, bootstrap sets the environment per the table in
`Approach`_, then returns to ``prted.c`` which selects the process type.

The steps here are grouped by concern, not by execution order: the
family-dependent values used below — ``port_param`` (the name of the
static-port MCA parameter) and ``port_str`` (``DVMPort`` as a string) — are
resolved in Step 5, which runs *before* this publishing so the correct
family-specific parameter is set.

**Controller path:**

.. code-block:: c

   PMIx_Setenv("PMIX_SERVER_NSPACE", dvm_nspace, true, &environ);
   PMIx_Setenv("PMIX_SERVER_RANK", "0", true, &environ);
   /* listen on the shared well-known DVM port */
   PMIx_Setenv(port_param, port_str, true, &environ);   /* family-specific; see Step 5 */
   *is_controller = true;

``prte_plm_base_set_hnp_name()`` already honors ``PMIX_SERVER_NSPACE`` /
``PMIX_SERVER_RANK`` and, on that path, uses the namespace **verbatim** (no
``@0`` suffix), so the controller's daemon namespace is exactly
``<ClusterName>-prte-dvm`` and its rank is 0 — the values every compute
daemon assumes when it synthesizes the controller URI.

**Daemon path:**

.. code-block:: c

   PMIx_Setenv("PRTE_MCA_ess_base_nspace", dvm_nspace, true, &environ);
   PMIx_Setenv("PRTE_MCA_ess_base_vpid", rank_str, true, &environ);
   PMIx_Setenv("PRTE_MCA_ess_base_num_procs", ndaemons_str, true, &environ);
   PMIx_Setenv("PRTE_MCA_prte_hnp_uri", ctrl_uri, true, &environ);   /* Step 4 */
   PMIx_Setenv(port_param, port_str, true, &environ);   /* family-specific; see Step 5 */
   *is_controller = false;

The ``ess/env`` component already wins selection for a daemon
(``PRTE_PROC_IS_DAEMON`` → priority 1) and its ``env_set_name()`` reads these
three ``ess_base`` parameters into ``PRTE_PROC_MY_NAME`` and
``prte_process_info.num_daemons``.  No new ``ess`` component is needed.

In both paths ``PRTE_MCA_prte_keep_fqdn_hostnames`` is set from
``cfg.keep_fqdn`` (Step 6).

Step 4 — Synthesize the controller contact URI
----------------------------------------------

A compute daemon must set ``prte_process_info.my_hnp_uri`` to a URI the RML
can parse (``prte_rml_parse_uris`` → ``set_addr``).  The two forms produced by
``prte_oob_base_get_addr()``, one per address family, are:

.. code-block:: text

   <process-name>;tcp://<ipv4>:<port>:<if_mask>          # DVMIPVersion=4
   <process-name>;tcp6://[<ipv6>]:<port>:<if_mask>       # DVMIPVersion=6

where ``<process-name>`` is the controller's name (``<ClusterName>-prte-dvm``,
rank 0) rendered by ``prte_util_convert_process_name_to_string()``, and the
transport tuple is the controller's IP, the shared ``DVMPort``, and an
interface mask.  Bootstrap:

#. Resolves ``cfg.ctrlhost`` to an address of the **selected family**
   (``getaddrinfo`` with ``ai_family`` = ``AF_INET`` or ``AF_INET6`` per
   ``cfg.ip_version``).
#. Builds the process-name string for ``(dvm_nspace, 0)``.
#. Fills the mask field from ``cfg.dvm_netmask`` when the administrator
   supplied ``DVMNetmask``; otherwise leaves it **empty** and relies on the
   parser tolerating an empty mask (below).
#. Assembles the URI in the form matching ``cfg.ip_version`` — ``tcp://`` for
   IPv4, or ``tcp6://[...]`` (bracketed address, per RFC 3986) for IPv6 — and
   sets it as ``PRTE_MCA_prte_hnp_uri``.

The ``DVMNetmask`` key exists precisely to give the mask field a correct,
administrator-controlled value: a compute daemon synthesizing the controller
URI cannot otherwise know the controller's real interface mask.  When it is
provided, the mask field of the transport tuple (of whichever family) carries
``cfg.dvm_netmask`` and the URI needs no special parser handling.

.. note::
   **Empty-mask tolerance is still required as the fallback** for when
   ``DVMNetmask`` is omitted.  ``set_addr()`` in ``oob_base_stubs.c`` parses
   the third, ``if_mask``, field of each transport tuple and uses it for
   reachability filtering.  Make ``set_addr()`` treat a missing/empty mask
   field as "reachable" (rather than requiring the operator to always set
   ``DVMNetmask``), and verify the change does not regress the normal
   launched path, where the mask is always present.  This localized parser
   change is the primary implementation risk and should be prototyped first.

Step 5 — Apply the address family, listening port, and inter-node networks
--------------------------------------------------------------------------

**Address family.**  ``DVMIPVersion`` (``cfg.ip_version``) chooses the family
for all inter-node communication, and bootstrap resolves it into three things:
the name of the static-port MCA parameter, the family enable/disable flags,
and the URI form of Step 4.  Because ``disable_ipv6_family`` defaults to
*true* in the OOB, an IPv6-only DVM must positively enable it:

.. code-block:: c

   if (6 == cfg.ip_version) {
   #if !PRTE_ENABLE_IPV6
       /* built without IPv6 — cannot honor DVMIPVersion=6 */
       pmix_show_help("help-prte-runtime.txt", "bootstrap-ipv6-unavailable",
                      true, prte_process_info.nodename);
       return PRTE_ERR_SILENT;   /* help already shown; match file convention */
   #endif
       port_param = "PRTE_MCA_prte_static_ipv6_ports";
       PMIx_Setenv("PRTE_MCA_prte_disable_ipv6_family", "0", true, &environ);
       PMIx_Setenv("PRTE_MCA_prte_disable_ipv4_family", "1", true, &environ);
   } else {
       port_param = "PRTE_MCA_prte_static_ipv4_ports";
       /* IPv4 is the OOB default; no family flags need forcing */
   }

The ``#if !PRTE_ENABLE_IPV6`` guard keeps the project's ``#if FOO`` discipline
and turns "configured for IPv6 on an IPv4-only build" into an explicit,
diagnosed failure rather than a silent fall-through.

**Listening port.**  The single ``DVMPort`` is applied uniformly through the
existing static-port machinery: bootstrap sets ``port_param`` (the family
value resolved above) to ``DVMPort`` on every process — controller and daemon
alike (Step 3 code).  The ``oob/tcp`` listener already consumes the
``static_ipvN_ports`` values; no listener change is needed.  Because the port
is well-known and shared across the DVM, any daemon can construct any peer's
contact tuple from its rank and node name — which is what makes the
launcher-less URI synthesis in Step 4 possible.

**Inter-node networks.**  ``DVMNetworks`` — the comma-delimited list of
networks/interfaces the runtime should use for inter-node communication — is
applied by setting ``PRTE_MCA_prte_if_include`` from ``cfg.dvm_networks``.
``prte_if_include`` already accepts a comma-delimited list of interface names
or CIDR subnets of either family (``split_and_resolve`` in ``oob/tcp``), so no
code change is needed; the key simply lets the administrator pin the runtime's
transport to specific networks from the same ``prte.conf`` (per the precedence
rule, overriding any
``if_include`` in the MCA param file).  When ``DVMNetworks`` is omitted the
runtime's default interface selection is unchanged.

Step 6 — Wire the ``KeepFQDNHostnames`` key
-------------------------------------------

The parser reads ``KeepFQDNHostnames`` into ``cfg.keep_fqdn`` (Step 1).
Bootstrap seeds the existing MCA variable before ``prte_init`` so the choice
made in ``prte.conf`` takes effect everywhere host names are matched or
stored:

.. code-block:: c

   PMIx_Setenv("PRTE_MCA_prte_keep_fqdn_hostnames",
               cfg.keep_fqdn ? "1" : "0", true, &environ);

Bootstrap must also apply the same short-vs-FQDN normalization to its **own**
node-matching in Step 2 (controller election and rank assignment), so its
matching agrees with the runtime's later behavior.

Step 7 — Startup retry with capped exponential backoff
------------------------------------------------------

Daemons boot independently; a compute daemon may try to reach the controller
long before the controller's listener is up, and there is no upper bound on
how late the controller may arrive (a node in the boot order, a controller
host that reboots).  A bootstrap daemon must therefore **never give up** — it
keeps trying to connect to the controller forever — but it must not busy-spin
against a down controller either.  The behavior is a **capped exponential
backoff**: retry frequently at first, then after progressively longer delays,
until the delay reaches a configured maximum, and then keep retrying at that
maximum rate indefinitely.

The existing OOB reconnect path in ``oob_tcp_connection.c`` already reschedules
a failed connect and already treats ``prte_max_recon_attempts < 0`` as
"infinite", but its delay is **fixed** at ``prte_retry_delay`` seconds — there
is no backoff and no cap.  Two changes give us the desired curve:

#. **Add a maximum-delay MCA parameter.**  Register ``prte_retry_max_delay``
   (seconds) alongside ``prte_retry_delay`` and ``prte_max_recon_attempts`` in
   ``oob_tcp.c``.  It defaults to ``0``, which means "no backoff — use the
   fixed ``retry_delay``", so the launched path is unchanged.

#. **Compute the delay as a function of the attempt count.**  In the reconnect
   block of ``oob_tcp_connection.c`` (the ``!connected`` path that today sets
   ``tv.tv_sec = prte_oob_base.retry_delay``), when ``retry_max_delay`` exceeds
   ``retry_delay`` derive the delay from the existing ``peer->num_retries``
   counter and cap it:

   .. code-block:: c

      /* base case (retry_max_delay == 0): fixed delay, unchanged behavior */
      unsigned secs = prte_oob_base.retry_delay;
      if (prte_oob_base.retry_max_delay > prte_oob_base.retry_delay) {
          /* exponential backoff: retry_delay, 2x, 4x, ... capped */
          uint64_t d = (uint64_t) prte_oob_base.retry_delay << peer->num_retries;
          if (d > (uint64_t) prte_oob_base.retry_max_delay) {
              d = prte_oob_base.retry_max_delay;
          }
          secs = (unsigned) d;
      }
      tv.tv_sec = secs;

   The ``<< num_retries`` shift is guarded by the cap, so the large-shift
   overflow is harmless (any value past the cap is clamped to
   ``retry_max_delay``).  ``num_retries`` is already incremented on each retry
   and reset to ``0`` on a successful connect, so no new state is needed.

**Bootstrap seeds the curve.**  ``prte_retry_delay`` defaults to ``0``, which
disables retry entirely, so bootstrap must positively enable it.  It sets three
parameters before ``prte_init``:

* ``prte_retry_delay`` → a short initial delay (e.g. ``1`` s) — bootstrap's own
  default, set with ``overwrite=false`` so an operator MCA setting wins.
* ``prte_max_recon_attempts`` → ``-1`` (never give up) — likewise
  ``overwrite=false``.
* ``prte_retry_max_delay`` → the value of the ``DVMRetryMaxDelay`` config key
  (default ``5`` s).  This one comes from ``prte.conf``, so it is set with
  ``overwrite=true`` per the precedence rule (config file trumps the MCA param
  file).

With ``retry_delay=1`` and ``retry_max_delay=5`` the delay sequence is
``1, 2, 4, 5, 5, 5, …`` seconds — frequent early attempts that settle onto a
steady 5-second poll and continue until the controller answers.

.. note::
   The backoff is a general OOB improvement, not a bootstrap-only code path:
   it is gated on ``retry_max_delay > retry_delay``, and with the default
   ``retry_max_delay = 0`` the launched path keeps its exact current
   fixed-delay behavior.  Only bootstrap turns it on, via ``DVMRetryMaxDelay``.

Step 7b — Radix wireup and ancestor healing
-------------------------------------------

Left to the retry loop alone, every daemon would phone home directly to the
controller, and the controller would have to service one connection per node.
Bootstrap instead wires each daemon into the **radix routing tree** at boot so
a daemon connects to its parent and the controller serves at most
``DVMRadix`` children.  Two knobs, both set from ``prte.conf`` before
``prte_init``:

* **Radix.**  ``ess_base_bootstrap.c`` publishes ``PRTE_MCA_rml_base_radix``
  from ``cfg.radix`` (``overwrite=true`` — config trumps).  Because the radix
  and the ``DVMNodes`` ordering are identical on every node, each daemon's
  ``prte_rml_compute_routing_tree()`` derives the same tree and therefore the
  same parent (lifeline).  The synthesized phone-home URI targets that parent's
  rank/host/``DVMPort`` rather than always rank 0.
* **Connect-max-time.**  A new ``prte_connect_max_time`` OOB parameter
  (registered in ``oob_tcp.c``, default ``0`` = forever) is published from
  ``cfg.connect_max_time``.  It bounds how long the connection state machine
  will retry a **non-lifeline** peer before giving up.

**Healing reuses the lost-connection climb.**  The DVM already climbs the
ancestor tree when a *live* parent is lost: ``lost_connection`` (in
``oob_tcp_component.c``) calls ``prte_rml_route_lost(rank)``, which for a
non-HNP parent runs ``prte_rml_repair_routing_tree()`` — promoting the daemon
to its grandparent and returning ``PRTE_SUCCESS`` (→ ``COMM_FAILED``), or
returning ``PRTE_ERR_FATAL`` for the HNP (→ ``LIFELINE_LOST``, die).  The
bootstrap startup race — a parent that never comes *up* — is the same problem
one step earlier, so it reuses the same logic:

* When a connection attempt to a non-lifeline peer exceeds
  ``connect_max_time``, ``oob_tcp_connection.c`` gives up and activates
  ``PRTE_PROC_STATE_FAILED_TO_CONNECT`` (it already does this on hard failure;
  the change is the time bound).
* ``failed_to_connect`` (in ``oob_tcp_component.c``) is made to mirror
  ``lost_connection``: route the peer through ``prte_rml_route_lost(rank)`` so
  a missing parent triggers the same grandparent promotion and the climb walks
  up the tree.  Reaching the controller (rank 0) yields the HNP path, which is
  retried forever rather than fatal — the daemon simply keeps trying the
  controller per Step 7.

The net effect: a daemon waits ``DVMConnectMaxTime`` for each successive
ancestor and, in the worst case (only the controller ever boots), climbs to
rank 0 and retries it forever.  RELM re-drives the rollup over the repaired
tree, so no reported-in state is lost across a heal.

Step 8 — ``prted.c`` branch and ``prte`` bootstrap removal
----------------------------------------------------------

``prte_ess_base_bootstrap()`` gains an out-parameter (or a
``prte_bootstrap_is_controller`` global) reporting the election result.
``src/tools/prted/prted.c`` (currently line 343, which unconditionally
proceeds to ``prte_init(PRTE_PROC_DAEMON)``) becomes:

.. code-block:: c

   bool is_controller = false;
   ret = prte_ess_base_bootstrap(&is_controller);
   if (PRTE_SUCCESS != ret) {
       return ret;
   }
   ...
   ret = prte_init(&argc, &argv,
                   is_controller ? PRTE_PROC_MASTER : PRTE_PROC_DAEMON);

A self-promoted ``prted`` running as ``PRTE_PROC_MASTER`` selects the
``ess/hnp`` module and becomes a full DVM controller — functionally the same
as ``prte``.  This realizes the spec's decision that the controller is a
self-promoted ``prted`` booted uniformly across the cluster.

**Remove the duplicate entry point.**  ``prte.c`` also honors ``--bootstrap``
(setting ``prte_bootstrap_setup`` at line 486).  With the self-promotion
model this path is redundant for DVM formation, and to keep exactly **one
bootstrap story** it is removed: the ``PRTE_CLI_BOOTSTRAP`` handling is
deleted from ``prte.c``, and the ``--bootstrap`` option is dropped from the
``prte`` personality in ``schizo_prte.c`` (leaving it on ``prted`` only).
Bootstrap is thereafter reachable exactly one way — ``prted --bootstrap``,
booted uniformly on every node, with the controller self-promoting.  The
``prte_bootstrap_setup`` global remains (it still gates the ``ras/bootstrap``
component on the controller), now set solely on the ``prted`` path.

Step 9 — Controller-side node pool (``ras/bootstrap``)
------------------------------------------------------

On the controller, ``ras/bootstrap``'s ``allocate()`` builds the node pool
from ``DVMNodes``.  It must assign each node the **same vpid** the daemon on
that node computes for itself in Step 2, so the controller's view of
rank ↔ node agrees with reality:

* The controller's own node is rank 0 (already placed by ``ess/hnp`` at
  ``prte_node_pool[0]``).
* Each ``DVMNodes`` entry is placed at ``prte_node_pool[rank]`` using the
  Step 2 rank rule (skipping the controller entry if present).

The draft currently appends nodes without ranks; this step replaces that with
rank-indexed insertion via the shared parser's node list, reusing the
canonical-ordering helper from Step 2 (factor it into ``prte_bootstrap.c`` so
both the ``ess`` and ``ras`` sides call one implementation).

Step 10 — Apply the operational and logging keys
------------------------------------------------

The draft parses ``DVMTempDir``, ``SessionTmpDir``, the log paths, and (newly,
per Step 1) the four log-state booleans, then frees them with no effect.  Wire
each to its existing runtime setting before ``prte_init``:

* ``DVMTempDir`` / ``SessionTmpDir`` → the session-directory base
  (``prte_process_info.tmpdir_base`` / the top session dir), matching how the
  ``--tmpdir`` family is applied today.
* ``ControllerLogPath`` / ``PRTEDLogPath`` and the ``*LogJobState`` /
  ``*LogProcState`` toggles → the controller/daemon state-logging options
  described in :doc:`../../configuration`.

Each key is applied on the side it governs (controller-only keys only when
``is_controller``).  These are independent of DVM formation and can land after
the formation path (Steps 1–9) is working.

Step 11 — Update the example config file and the configurator tool
------------------------------------------------------------------

Two user-facing artifacts enumerate every configuration key and must be kept
in lockstep with the parser (Step 1), or an administrator will generate a
``prte.conf`` the runtime no longer understands.

**Example config file** — ``src/etc/prte.conf`` ships with every key present
but commented out.  Update it to:

* collapse ``DVMControllerPort`` and ``PRTEDPort`` into the single
  ``#DVMPort=7817``;
* add the new keys in the Bootstrap Options block —
  ``#DVMNetworks=``, ``#DVMNetmask=``, ``#DVMIPVersion=4``, ``#DVMRadix=64``,
  ``#KeepFQDNHostnames=false``, ``#DVMRetryMaxDelay=5``, and
  ``#DVMConnectMaxTime=30``.

**Configurator tool** — ``docs/_templates/configurator.html`` is the Sphinx
template (editable source, *not* a generated artifact) rendered as the
``configurator.html`` page referenced from :doc:`../../configuration`.  It is
a self-contained HTML form whose ``displayfile()`` JavaScript assembles a
``prte.conf`` from the field values via ``get_field()`` /
``get_checkbox_value()``.  Update it to match the key set:

* Replace the ``controller_port`` (``DVMControllerPort``) and ``prted_port``
  (``PRTEDPort``) inputs with a single ``DVMPort`` input (default ``7817``),
  and the corresponding two ``get_field()`` lines in ``displayfile()`` with
  one.
* Add inputs and ``displayfile()`` emission for ``DVMNetworks``,
  ``DVMNetmask``, ``DVMIPVersion`` (a ``4``/``6`` selector), ``DVMRadix``
  (default ``64``), ``DVMRetryMaxDelay`` (default ``5``), and
  ``DVMConnectMaxTime`` (default ``30``), plus a ``KeepFQDNHostnames`` on/off
  switch (reusing the existing ``onoffswitch`` pattern and
  ``get_checkbox_value()``).
* Reconcile the standing note that reads *"Hostname values should not be
  specified as fully qualified domain names"* with the new
  ``KeepFQDNHostnames`` option: the note holds only when
  ``KeepFQDNHostnames=false`` (the default), so reword it to say that host
  names must be given short-form **unless** ``KeepFQDNHostnames`` is enabled,
  in which case they must be fully qualified — matching the host-matching rule
  the runtime applies (Step 6).

Neither artifact affects DVM formation, but both are part of the deliverable:
the documentation-update requirement for a user-visible change applies here.
Do **not** edit the rendered copies under ``docs/_build/`` or any installed
``.../html/`` tree — those regenerate from these sources.

Summary of files changed
------------------------

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - File
     - Change
   * - ``src/util/prte_bootstrap.h`` / ``.c`` (new)
     - The shared parser: ``prte_bootstrap_config_t``,
       ``prte_bootstrap_parse()``, the ``DVMNodes`` expander, and the
       canonical rank-ordering helper (Steps 1, 2, 9).
   * - ``src/util/Makefile.am``
     - Build the new object files.
   * - ``src/mca/ess/base/ess_base_bootstrap.c``
     - Replace the inlined parser with a call to ``prte_bootstrap_parse()``;
       add identity computation, controller election, the env-publishing of
       identity/URI/port/FQDN, and the ``is_controller`` out-parameter
       (Steps 2–7).
   * - ``src/mca/ess/base/base.h``
     - Update the ``prte_ess_base_bootstrap()`` prototype (out-parameter).
   * - ``src/mca/ras/bootstrap/ras_boot.c``
     - Replace the inlined parser with ``prte_bootstrap_parse()``; assign
       rank-indexed nodes via the shared ordering helper (Step 9).
   * - ``src/tools/prted/prted.c``
     - Branch on the election result to init as ``PRTE_PROC_MASTER`` vs
       ``PRTE_PROC_DAEMON`` (Step 8).
   * - ``src/prted/prte.c``
     - Remove the ``PRTE_CLI_BOOTSTRAP`` handling — one bootstrap story
       (Step 8).
   * - ``src/mca/schizo/prte/schizo_prte.c``
     - Drop the ``--bootstrap`` option from the ``prte`` personality; keep it
       on ``prted`` (Step 8).
   * - ``src/rml/oob/oob_base_stubs.c``
     - Make ``set_addr()`` tolerate a missing/empty interface-mask field so a
       synthesized controller URI parses (Step 4) — pending the prototype in
       `Open risks`_.
   * - ``src/rml/oob/oob_tcp.c``
     - Register the new ``prte_retry_max_delay`` MCA parameter (Step 7).
   * - ``src/rml/oob/oob_tcp_connection.c``
     - Compute the reconnect delay as a capped exponential backoff when
       ``retry_max_delay > retry_delay`` (Step 7).
   * - ``src/util/proc_info.c`` (or the tmpdir path)
     - Apply ``DVMTempDir`` / ``SessionTmpDir`` (Step 10).
   * - ``src/mca/schizo/prte/help-*.txt`` / ``help-prte-runtime.txt``
     - New diagnostics for a node absent from ``DVMNodes``
       (``bootstrap-node-not-member``) and for ``DVMIPVersion=6`` on a build
       without IPv6 (``bootstrap-ipv6-unavailable``).
   * - ``src/etc/prte.conf``
     - Collapse the two port keys to ``DVMPort``; add ``DVMNetworks``,
       ``DVMNetmask``, ``DVMIPVersion``, ``KeepFQDNHostnames``,
       ``DVMRetryMaxDelay`` (Step 11).
   * - ``docs/_templates/configurator.html``
     - Same key changes in the form + ``displayfile()`` JS; reconcile the
       FQDN note with ``KeepFQDNHostnames`` (Step 11).
   * - ``docs/configuration.rst``
     - Documented the consolidated ``DVMPort`` and the new ``DVMNetworks`` /
       ``DVMNetmask`` / ``DVMIPVersion`` / ``DVMRetryMaxDelay`` keys (already
       updated).

Open risks
----------

* **URI interface-mask (Step 4).**  The synthesized controller URI must
  survive ``set_addr()``.  When ``DVMNetmask`` is set the mask field is
  populated and the URI parses as-is; the residual risk is the
  *empty-mask fallback* used when it is omitted.  Prototype the "tolerate
  empty mask" change first and confirm it does not alter reachability
  filtering on the normal launched path.  This is the highest-risk item;
  everything else reuses proven wiring.
* **Retry backoff (Step 7).**  Confirm the capped-backoff change is inert on
  the launched path when ``retry_max_delay == 0`` (its default), that seeding
  the bootstrap defaults with ``overwrite=false`` honors an operator override,
  and that an infinite (``max_recon_attempts = -1``) retry against a
  never-arriving controller backs off to the cap rather than busy-spinning or
  overflowing the shift.
* **Host matching consistency.**  Bootstrap's Step 2 matching and the
  runtime's later ``prte_check_host_is_local`` / ``prte_keep_fqdn_hostnames``
  matching must agree; a mismatch would let a daemon elect itself correctly
  yet be placed at the wrong node index on the controller.  Exercise with
  both short and FQDN configurations.
* **IPv6-only clusters.**  ``DVMIPVersion=6`` is handled in Steps 4–5
  (family-specific port parameter, ``disable_ipv*_family`` flags, and the
  ``tcp6://[...]`` URI form), gated on a ``PRTE_ENABLE_IPV6`` build.  The
  residual verification is that a synthesized ``tcp6`` URI round-trips through
  ``set_addr()`` (including the empty-mask fallback) exactly as the ``tcp4``
  form does, and that disabling the IPv4 family on an IPv6-only DVM does not
  disturb the loopback/self short-circuit paths.  Dual-stack (both families
  active at once) is out of scope: ``DVMIPVersion`` selects exactly one.

Testing
-------

There is no unit-test harness; validate by forming a DVM.  The existing
``contrib/dockerswarm`` multi-node harness (used for elastic-mode testing) is
the natural vehicle: pre-position an identical ``prte.conf`` in every
container, boot ``prted --bootstrap`` on each, and confirm the controller
elects itself, every compute daemon phones home and appears in the routing
tree, and ``prun -n N hostname`` launches across the bootstrapped DVM.  Test
the boot-order-skew case by delaying the controller container's start so the
compute daemons must retry (Step 7).
