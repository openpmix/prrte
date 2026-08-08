.. _slurm-pmi2-vs-pmix-fence-label:

Slurm's PMI2 and PMIx Fence Collectives
=======================================

Slurm ships two independent MPI plugins that both implement a
"exchange business cards, then synchronize" operation for the processes
of a job step: ``mpi/pmi2`` (the PMI-1/PMI-2 key-value store) and
``mpi/pmix`` (the PMIx server integration). Both are frequently
described as "the fence", and they are frequently assumed to behave the
same way. They do not.

This page records what each one actually does, based on a reading of the
Slurm sources (``src/plugins/mpi/pmi2`` and ``src/plugins/mpi/pmix``, as
of Slurm 26.11.0). It exists because PRRTE and PMIx are regularly
compared against these implementations, and because one recurring claim
about the PMI2 collective -- that it does not assemble a full copy of the
data at each participant -- is true only under one specific reading of
"participant".

.. note:: File and line references below point into the *Slurm* source
          tree, not PRRTE's. They are accurate as of the version named
          above and are given to make the claims checkable, not as
          durable anchors.


Does PMI2 assemble a full copy at every participant?
----------------------------------------------------

The answer depends on what counts as a participant.

**At the node (stepd) level: yes, it does.** The PMI2 fence is a genuine
allgather. Every ``slurmstepd`` ends up holding a complete, parsed copy
of every key/value pair put by every task in the step.

**At the process (task) level: no, it does not.** No MPI task ever
receives a copy of the fence data. The fence response delivered to the
tasks carries a return code and nothing else; a task retrieves keys one
at a time, on demand, over its Unix socket to the local stepd.

Both halves are visible directly in the code:

* ``client.c:579`` -- ``send_kvs_fence_resp_to_clients()`` builds a single
  response containing only ``cmd=`` and ``rc=`` (plus an error string on
  failure) and writes that identical, payload-free response to each local
  task descriptor. There is no data path from the fence to the client at
  all.

* ``tree.c:198`` -- on the way down, each stepd unpacks *every*
  key/value pair out of the broadcast buffer and ``kvs_put()``\ s it into
  the node-local hash table.

* ``pmi2.c:332`` -- ``_handle_kvs_get()`` then answers a task's
  ``kvs-get`` purely from that local hash. It never performs a remote
  fetch, because it never needs to: everything is already resident on the
  node.

So the often-repeated statement is best restated as: *PMI2 replicates the
entire key-value store to every node, and to no process.*


The PMI2 algorithm
------------------

Fan-in / fan-out over Slurm's message-forwarding tree, rooted at **srun**
-- not at a compute node.

#. **Put (no barrier).** ``_handle_kvs_put()`` (``pmi2.c:261``)
   deliberately does *not* touch the node hash -- the code comments it as
   ``/* no need to add k-v to hash. just get it ready to be up-forward
   */``. It appends the packed ``key,val`` straight into a flat byte
   buffer, ``temp_kvs_buf`` (``kvs.c:143``).

#. **Fan-in.** A stepd counts down ``tasks_to_wait`` (its local tasks)
   and ``children_to_wait`` (its subtree node count). When both reach
   zero it ships ``temp_kvs_buf`` to its parent (``pmi2.c:286``,
   ``tree.c:103``). Intermediate stepds concatenate a child's payload
   onto their own with a raw ``memcpy`` -- ``temp_kvs_merge()``
   (``kvs.c:167``). Nothing is parsed, deduplicated, or re-encoded on the
   way up.

#. **Fan-out.** Once all subtrees have reported, srun calls
   ``temp_kvs_send()`` with ``nodelist = job_info.step_nodelist``
   (``kvs.c:196``) -- that is, a **single broadcast of the entire merged
   blob to every node in the step**, via ``slurm_forward_data()``. Every
   stepd receives byte-identical, complete data.

#. **Store and release.** Each stepd parses the blob into ``kvs_hash``
   (``tree.c:201``), then releases its local tasks with the payload-free
   barrier-out described above.

Three properties follow from this structure:

* Duplicate keys ride the wire. Deduplication happens only at *store*
  time -- ``kvs_put()`` replaces an existing key unless
  ``PMI2_KVS_NO_DUP_KEYS`` is set (``kvs.c:287``).

* There is no way to opt out of collecting the data. The PMI2 fence is
  unconditionally an allgather; a "barrier only" fence does not exist.

* The one scalable alternative offered by the plugin is ``PMIX_Ring``
  (``ring.c``) -- a genuine scan carrying only left/right neighbor
  values. It is a separate API and deliberately does *not* distribute
  full data.


The PMIx algorithms
-------------------

Slurm's ``mpi/pmix`` plugin implements **two** fence collectives and
chooses between them at runtime (``pmixp_client.c:738``; overridable via
the ``SLURM_PMIXP_FENCE`` environment variable)::

    type = TREE     if there is no data to collect
    type = RING     if (collect && ndata > 0)

The in-source rationale is that the tree wins for a zero-data fence (a
pure barrier), while the ring wins once there is a real payload to move.

Tree
^^^^

``pmixp_coll_tree.c`` -- the same overall shape as PMI2, but rooted on a
compute node and driven by an explicit six-state machine
(``SYNC`` → ``COLLECT`` → ``UPFWD``/``UPFWD_WSC``/``UPFWD_WPC``
→ ``DOWNFWD``).

* The local contribution is copied into ``ufwd_buf``
  (``pmixp_coll_tree.c:958``); each child's contribution is appended to
  that same buffer (``:1113``).

* At the root (``prnt_host == NULL``), ``ufwd_buf`` is copied into
  ``dfwd_buf`` (``:529``). That copy is the turnaround point of the
  collective.

* Fan-out depends on the transport. With direct connections enabled,
  each node forwards ``dfwd_buf`` to its own children. Without them,
  only the root sends -- as a single ``PMIXP_EP_HLIST`` broadcast to
  every other node (``:622``), which is structurally the same shortcut
  PMI2 takes.

* Every node hands the complete ``dfwd_buf`` to its local PMIx server
  via ``pmixp_lib_modex_invoke()`` (``:689`` on the root path, ``:780``
  for everyone else).

Ring
^^^^

``pmixp_coll_ring.c`` -- no root and no tree. Each node has exactly one
successor (``_ring_next_id()``, ``:59``).

* A node appends every contribution it receives to its own ``ring_buf``
  and forwards it one hop onward, stopping when a contribution has
  travelled all the way around: ``_pmixp_coll_contrib()`` (``:527``)
  forwards unless ``contrib_id == _ring_next_id()``.

* The collective completes locally when ``_ring_remain_contrib() == 0``
  -- that is, when this node has seen all ``peers_cnt`` contributions
  (``:385``) -- and the full ``ring_buf`` goes to the local PMIx server
  (``_invoke_callback()``, ``:339``).

* Duplicated or replayed contributions are rejected through a
  per-context ``contrib_map`` bitmap (``:690``).

* Three ring contexts (``PMIXP_COLL_RING_CTX_NUM == 3``) allow
  consecutive fences to overlap.

Both algorithms deliver a complete copy to every node. At the process
level PMIx behaves like PMI2 -- the client does not hold the blob either;
the node-local PMIx server stores it and serves ``PMIx_Get`` from it.

The meaningful difference is that PMIx can skip the collection entirely.
If the application does not pass ``PMIX_COLLECT_DATA``
(``pmixp_client_v2.c:133``), ``ndata`` is empty, the fence degenerates
into a barrier, and subsequent ``PMIx_Get`` calls for remote peers are
issued as on-demand point-to-point direct-modex requests
(``pmixp_dmdx.c``). PMI2 has no equivalent.


How they differ
---------------

.. list-table::
   :header-rows: 1
   :widths: 22 26 26 26

   * - Property
     - PMI2
     - PMIx tree
     - PMIx ring
   * - Topology
     - fan-in/out tree, **root = srun**
     - fan-in/out tree, root = node 0
     - ring, no root
   * - Fan-out
     - always one srun broadcast to the whole nodelist
     - per-node forwarding with direct connections, else a root broadcast
     - n/a -- data arrives as it circulates
   * - Latency
     - 2·log(N) plus the srun hop
     - 2·log(N)
     - N−1 hops, but pipelined per contribution
   * - Peak load
     - srun holds and sends the whole blob
     - root holds and sends the whole blob
     - evenly spread; no hotspot
   * - Payload
     - parsed key/value strings, re-hashed on every node
     - opaque PMIx modex blob
     - opaque PMIx modex blob
   * - Ordering
     - tree order, identical on every node
     - tree order, byte-identical everywhere
     - rotated per node (content complete, order differs)
   * - Opt out of data
     - **no** -- always an allgather
     - yes (``PMIX_COLLECT_DATA`` off → barrier plus direct modex)
     - yes
   * - Concurrent fences
     - no -- single ``kvs_seq``, one in flight
     - one in flight
     - up to three contexts in flight
   * - Deduplication
     - at store time only (``kvs_put()``)
     - none (the PMIx server's job)
     - none (the PMIx server's job)
   * - Delivered to clients
     - no -- one ``kvs-get`` per key to the local stepd
     - no -- ``PMIx_Get`` from the local server
     - no

Structurally, then: PMI2 and the PMIx tree are the same algorithm, but
PMI2 places the aggregation point in ``srun`` and hard-codes a "collect
everything, always" policy. The PMIx ring is the genuinely different
one -- it trades logarithmic depth for even load distribution and
eliminates the root bottleneck, which is exactly why the plugin selects
it when there is a real payload to move.
