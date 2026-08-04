# AGENTS.md — `plm/base` (the launch machinery itself)

Guide to `src/mca/plm/base/`. Read the
[framework guide](../AGENTS.md) first — it covers the module contract,
component selection, and which component runs where. This file is about
the code *underneath* the components: the state-machine handlers, the
daemon callback, `setup_virtual_machine`, the command processor, and the
elastic launch fence. A component is mostly glue; nearly every launch bug
lives here.

```
base/
  base.h                    # what other frameworks may call (state handlers, spawn_response,
                            #   the UPDATE_PROC_STATE writers)
  plm_private.h             # framework-internal API + prte_plm_globals_t
  plm_base_frame.c          # open/close/register, the MCA alias, the DEFAULT local-only module
  plm_base_select.c         # pick-ONE selection
  plm_base_receive.c        # the HNP's inbound command processor + the state-update writers
  plm_base_launch_support.c # 3000+ lines: everything else
  plm_base_prted_cmds.c     # xcast terminate/kill/signal
  plm_base_jobid.c          # HNP nspace + per-job nspace assignment
  help-plm-base.txt         # user-facing text (mostly consumed by src/prted, not by plm)
```

---

## `setup_virtual_machine` — the one function to understand

Every component calls it first thing in `launch_daemons`. It answers:
**which nodes need a new daemon, and what vpid does each get?**

It is a switchyard of mutually exclusive branches, each selecting a
different candidate node set, all converging on the `process:` loop that
creates the daemon procs:

| Branch | Selected when | Candidates |
|--------|---------------|-----------|
| fixed DVM | `PRTE_JOB_FIXED_DVM` | none — returns at once |
| **grow** | `PRTE_JOB_EXTEND_DVM` | pool nodes marked `PRTE_NODE_STATE_ADDED`, and *only* those |
| dynamic spawn | the job has an originator | first pass: the whole pool (singleton); later: `ADDED` nodes |
| no-VM / multi-sim | `PRTE_JOB_NO_VM` or `PRTE_JOB_MULTI_DAEMON_SIM` | pool nodes carrying procs |
| initial VM | otherwise | the whole pool, filtered through the app specs |

### The per-launch reset (read this before adding a branch)

The daemon job's **map is persistent** — it accumulates the DVM's nodes
for the life of the session. Two of its fields are not cumulative:

- **`map->num_new_daemons`** — what every component keys its "is there
  anything to launch?" test off;
- **`map->daemon_vpid_start`** — the base vpid `slurm`, `lsf` and `pals`
  substitute into the prted command line, from which each daemon the RM
  starts computes its own name.

Both describe *this* launch, so `setup_virtual_machine` clears them once,
at the top, before any branch runs. That is deliberate placement: when
each branch cleared them for itself, two of the four forgot, and a second
launch that added a node (`--add-host`, an elastic grow) handed an RM
launcher the *first* launch's start vpid — telling the new daemons to
claim ranks that live daemons already own. `ssh` never showed it, because
it substitutes each node's own vpid per node. `test/unit/plm`'s
`test_setup_vm` pins the invariant: each pass must report its own
daemons.

### vpid assignment

Normally the next unused vpid (`daemons->num_procs`). In a **bootstrapped
DVM** it is the node's canonical rank (`node->index`, recorded by
`ras/bootstrap` from `prte.conf`) — not a preference: the daemon computed
that same rank for itself before it ever contacted the HNP, and
`prted_report_launch` looks a reporting daemon up *by the rank it claims*.

`daemons->num_procs` is maintained as the vpid **span**, grown to cover
the vpid just used rather than blindly incremented, so a canonical rank
cannot leave it short of the highest daemon in the job (which would
truncate the nidmap span).

### Elastic bookkeeping

In elastic mode the function also records a **grow campaign** and raises
`prte_dvm_launch_fence`. Two details are load-bearing:

- the campaign's target list is **collected as the vpids are assigned**,
  not reconstructed afterwards as a run starting at `daemon_vpid_start`.
  That reconstruction is right only while vpids come out consecutively,
  which is not the rule in a bootstrapped DVM (there a daemon takes its
  node's canonical rank), and a campaign naming ranks it did not launch
  never drains its fence;
- the requester is found by scanning **all** the targets' session
  backpointers rather than trusting the first — see the framework guide
  for the shrink-then-grow case that motivated it.

---

## The daemon callback — `prte_plm_base_daemon_callback`

The recv on `PRTE_RML_TAG_PRTED_CALLBACK`; the framework guide walks the
five steps. Two ownership rules that are easy to get wrong:

- **`node->topology` is a counted reference** into `prte_node_topologies`.
  Assigning one means retain-new/release-old.
- **`node->topodiff` is owned by the node** and freed by
  `prte_node_destruct`. `hwloc_topology_diff_build()` allocates a list
  *whenever it has anything to say* — including the `TOO_COMPLEX` entry it
  returns **1** with when the topologies genuinely differ, which is the
  common outcome while walking the recorded topologies looking for a
  match. Every list this code does not adopt has to be destroyed, or a
  heterogeneous DVM leaks one diff per node per recorded topology; and the
  node's previous diff has to be destroyed before a new one is stored,
  because a daemon can report in more than once (the bootstrap unheal
  path).

The whole loop is written so a daemon may report **twice**: the URI is
freed before being replaced, the topology reference is released before
being re-taken, and a returning daemon that had been marked
`COMM_FAILED` restores the daemon count.

Everything the loop records lands on `daemon->node`, so it checks that
the proc *has* a node before it starts. `setup_vm` links the two when it
creates a daemon, so a report naming a rank with no node is a report from
something we did not launch — a diagnostic, not a segfault in the HNP.

---

## `plm_base_receive.c` — the command processor

`prte_plm_base_recv` handles `PRTE_RML_TAG_PLM` inside an event (so it is
on the progress thread). Beyond the command dispatch described in the
framework guide, two invariants govern `PRTE_PLM_LAUNCH_JOB_CMD`:

**Ownership of the unpacked job.** `own_jdata` tracks whether the job
object belongs to nobody but us. It is true from the unpack until the job
is handed to a session; the `ANSWER_LAUNCH` error path releases it only
while it is still ours. Anything that can reject the request must
therefore run **before** the job is added to `session->jobs` — that array
*borrows* its entries, so nothing ever removes or releases one, and a job
rejected after being added stays there as a phantom for the life of the
session.

**A spawn request arrives with no namespace.** The requester packs a job
object the HNP has not named yet; `prte_plm_base_setup_job` assigns the
nspace (via `prte_plm_base_create_jobid`) when the job reaches `INIT`.
Anything keyed on the job's identity therefore cannot be done here. That
caught the reservation-ownership grant: the spawned job is supposed to
become an owner of the reservation it targets (so it can spawn onto those
nodes in turn), and recording it at request time wrote an **empty**
namespace into the owner set. An empty namespace is not merely useless —
`PMIx_Check_nspace` treats an empty side as a *wildcard*, so an empty
owner entry matches every namespace and retires the reservation's
ownership gate entirely. The grant now happens in `setup_job`, once the
job has a name, and `prte_session_add_owner` refuses an empty namespace
outright.

The requester is still vetted here, before anything is granted:
`prte_session_is_owned_by(session, requestor)`.

### `TOOL_ATTACHED` — ask the daemon, don't index the pool

A tool that connects through some daemon is reported to the master, which
builds a job object for it. To record *where* the tool is, ask the
reporting daemon: `daemons->procs[sender->rank]->node`. Indexing
`prte_node_pool` by the daemon's rank — which this used to do — assumes
vpids and pool indices run in step, and they need not: the pool holds
every node the allocation named, including ones no daemon was ever
launched on (excluded by a `-host`/hostfile spec, marked `DO_NOT_USE`,
shrunk away), while vpids go only to nodes that get a daemon. And not
knowing where a tool sits is no reason to bring the DVM down, which is
what the old `NOT_FOUND` did by falling into the master's `CLEANUP`
(→ `FORCED_EXIT`).

### Two handlers that used to live here

`prte_plm_base_vm_ready()` and `prte_plm_base_setup_job_complete()` are
**gone**. The DVM state machine registers `state/dvm`'s `vm_ready()` and
`init_complete()` on those states; the copies here were never registered
by anything, so they drifted while looking authoritative — the live
`vm_ready()` also builds the WIREUP xcast and drains the elastic grow
campaigns, neither of which the copy knew about. If you are looking for
the VM_READY behavior, it is in `state/dvm`.

---

## Telling the requester what happened

`prte_plm_base_spawn_response(status, jdata)` is the single answer to a
spawn request, and it is called with **failures** too (`errmgr/dvm` and
`state/dvm` both route a failed job through it, so a quick-failing job
cannot leave its requester unanswered). Three things travel, and they are
not interchangeable:

| What | To whom | When |
|------|---------|------|
| the diagnostic (`report_launch_failure`) | prterun: the job's stderr. A separate tool: a `PMIX_ERR_JOB_FAILED_TO_LAUNCH` event custom-ranged to it | failure only, once (`PRTE_JOB_FLAG_ERR_REPORTED`) |
| `PMIX_LAUNCH_COMPLETE` | the launch proxy of a tool-requested (`PRTE_JOB_DVM_JOB`) spawn | **success only** — the event says the job is running, and a tool is entitled to read it that way |
| the spawn response (status + nspace + room) | the originator, locally or over `PRTE_RML_TAG_LAUNCH_RESP` | always; this is what releases `PMIx_Spawn` |

`PRTE_JOB_SPAWN_NOTIFIED` makes the whole thing single-shot on both the
local and the relayed path.

---

## The `UPDATE_PROC_STATE` writers live here on purpose

`prte_plm_base_pack_state_for_proc()` / `prte_plm_base_pack_state_update()`
sit beside their only reader because the wire carries no version. There
used to be three copies of the writer. `test/unit/plm` round-trips both
shapes through an unpacker that repeats the receiver's sequence — that
test *is* the format contract.

Note the other three PLM message bodies (`REGISTERED`,
`LOCAL_LAUNCH_COMP`, `READY_FOR_DEBUG`, all written by `state/prted`)
have no shared writer and no terminator: their readers loop until the
buffer runs out. If you add a field to one, change both ends in the same
commit — see the top-level AGENTS.md on mixed-version DVMs.

---

## `prte_plm_globals_t` — keep it honest

Three fields were removed from it in the 2026 review because nothing read
them: `daemonlaunchstart`, `tree_spawn_cmd`, and `node_regex_threshold`.
The last one was worse than dead: it was registered as the MCA parameter
`plm_node_regex_threshold` and documented as controlling whether the node
regex went on the prted command line, while the regex has travelled in the
launch message for years. A knob that does nothing is worse than no knob —
a user who hits a too-long command line sets it, sees nothing change, and
cannot tell a wrong setting from a wrong diagnosis. (`plm_ssh_delay` was
retired for the same reason.) The real remedy for an over-long command
line is `plm_ssh_pass_environ_mca_params 0`, which `help-plm-ssh.txt`
names.

What remains: `base_nspace` (+ `next_jobid`), `daemon_nodes_assigned_at_launch`,
and `pass_environ_mca_params`. If you add a field, make sure something
reads it.

---

## Testing what is in here

| Layer | What it reaches |
|-------|-----------------|
| [`test/unit/plm`](../../../../test/unit/plm/) | `plm_types.h` code uniqueness, the module vtable, `wrap_args`, `setup_prted_cmd`, the full `prted_append_basic_args` command line, `set_hnp_name`/`create_jobid`, the `UPDATE_PROC_STATE` round trip, and `setup_virtual_machine`'s per-launch accounting (`test_setup_vm`, which builds the global job/node pools by hand as `test/unit/ras` does) |
| [`test/offline`](../../../../test/offline/) | `setup_virtual_machine` on the `DO_NOT_LAUNCH` path, across the mapper matrix |
| [`contrib/dockerswarm`](../../../../contrib/dockerswarm/) | everything that needs daemons to actually come up: tree-spawn, throttling, the `=`-bearing MCA value, alias reconciliation, and a second launch adding a daemon to a running DVM |

A pure/structural check belongs in the unit test. Anything that needs a
daemon belongs in the swarm. Note that a change to `plm_private.h` changes
the layout of `prte_plm_globals` — rebuild from the **top** of the build
tree before running the unit test, or you will be testing a stale object
against a new library.
