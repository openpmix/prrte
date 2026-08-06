# AGENTS.md — `ras/slurm` (SLURM allocator)

Component guide for `src/mca/ras/slurm/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
session/pool model, and the node→session deviation summarized below.

---

## Role and priority

`ras/slurm` reads a SLURM allocation from the environment. It makes
itself available (query priority **50**) only when `SLURM_JOBID` is set;
otherwise it disqualifies. It is the richest ras component: besides
initial discovery it implements the full **elastic modify** surface
(extend / release / cancel) by driving `scontrol`/`salloc` and parsing
SLURM's JSON.

Files:

| File | Contents |
|------|----------|
| `ras_slurm_component.c` | Registration; `query` gates on `SLURM_JOBID`; many `propagate_*` MCA params. |
| `ras_slurm_module.c` | `init`, `allocate`, `modify`, `finalize`; the `SLURM_NODELIST` regex parser; session/tagging helpers; jobid/hostname validation. |
| `ras_slurm_modify_extend.c` | `PMIX_ALLOC_EXTEND`: build & launch a `salloc --no-shell` expander job, wait for it, absorb new nodes. |
| `ras_slurm_modify_release.c` | `PMIX_ALLOC_RELEASE`: `scontrol update job` to shrink; remove nodes by count. |
| `ras_slurm_modify_cancel.c` | `PMIX_ALLOC_REQ_CANCEL`: track and cancel pending extend requests. |
| `ras_slurm_modify_common.c` | Shared helpers: `kill_job`, control-char checks, command-output draining. |
| `ras_slurm_jansson.c` | JSON path: `scontrol show job <id> --json`, extract job fields, add/detach modified resources. |
| `ras_slurm_jansson_stub.c` | No-Jansson stubs so the component builds without the JSON parser. |
| `ras_slurm.h` | Component struct, constants, field enums, the session-stack item type. |

---

## How `allocate()` works

`prte_ras_slurm_allocate`:

1. Requires `SLURM_JOBID`; else `PRTE_ERR_TAKE_NEXT_OPTION`.
2. If a `prte_session_t` already exists for that jobid
   (`prte_get_session_object_from_id`), returns **`PRTE_EXISTS`** —
   `SLURM_NODELIST` is per-job, so distinct job steps re-discover the
   same set and must not re-insert it.
3. Reads `SLURM_NODELIST` (checked against `max_envar_length` by
   `check_taint`) and the per-node task counts:
   - default: `SLURM_TASKS_PER_NODE`, scaled by `SLURM_CPUS_PER_TASK`;
   - with `use_entire_allocation`: `SLURM_JOB_CPUS_PER_NODE` (a debug
     edge case where the tool got 1 task/node but should use it all).
4. `prte_ras_slurm_discover` expands the compressed nodelist regex
   (e.g. `foo[2-10,12],bar` with `2(x10),5`) into `prte_node_t`s with
   per-node `slots`, via `parse_ranges`/`parse_range` (zero-padding
   aware).
5. `prte_ras_slurm_tag_node_allocation` stamps each node with
   `PRTE_NODE_ALLOC_ID` (the numeric jobid).
6. `prte_ras_slurm_assign_new_session` creates a tracking session keyed
   by jobid and pushes a LIFO `prte_slurm_session_stack` item.

Node names and jobids are validated (`validate_hostname`,
`validate_jobid`, `convert_jobid`) against length and character
allowlists before use in shell commands — this component shells out, so
taint control matters.

---

## The node→session deviation (read before touching sessions)

`prte_ras_slurm_assign_new_session` retains a reference to each **real**
pool-bound node (never a `prte_node_copy` duplicate — those are
daemon-less and unusable by the mapper) inside the per-jobid session, but
**deliberately leaves `node->session == NULL`**. These nodes are the
DVM's startup/default session and must remain in the general pool; the
session object is a tracking handle for later identify/release, *not* a
reservation. Setting `node->session` here would withhold the entire base
allocation from mapping. See repo memory *Node-reservation SLURM
deviation* and the framework guide.

---

## `modify()` — elastic extend / release / cancel

`modify` dispatches on `req->allocdir`:

- **`PMIX_ALLOC_EXTEND`** → `serve_extend_req`: propagates the original
  job's SLURM attributes (account, partition, qos, cwd, mem-per-cpu,
  mem-per-node, time, threads-per-core — each gated by a `propagate_*`
  MCA param, all default true), builds `salloc` args, launches an
  **expander job**, waits (event-driven wait tracker) for it, then adds
  the modified resources.
- **`PMIX_ALLOC_RELEASE`** → `serve_release_req`: shrinks the SLURM job
  with `scontrol update job`, removing nodes by count while protecting
  the launching node (`SLURMD_NODENAME`).
- **`PMIX_ALLOC_REQ_CANCEL`** → `serve_cancel_req`: cancels a pending
  extend by request id.

### The salloc child outlives the call that forked it

`--no-shell` is what makes the expander job shrinkable. Allocating with
`sbatch` meant a batch script, and **a batch script is the job** — the job
lives exactly as long as the script runs, which is why it had to be `sleep
infinity`. That script runs on the job's first node, so releasing that node
would kill it and take the whole allocation down with it: the one node that
could never be handed back on its own. `--no-shell` runs nothing at all, so no
node anchors the job and any of them can go.

The price is that `salloc` is the process holding the handshake with
slurmctld, and it does not exit until the allocation is granted. Three
consequences, all in `prte_ras_slurm_exec_salloc`:

- **It is not waited on.** The job ID is read off the child's output — salloc
  names the job as soon as slurmctld has it, pending or not — and the child is
  then handed to `prte_wait_cb` and left running. Waiting inline would block
  the progress thread for the whole queue wait, and PRRTE's own SIGCHLD
  handler reaps every child anyway.
- **Killing it while the job is pending revokes the allocation.** That is the
  intended way out of a failed submit, and the reason nothing else may kill it.
- **Its pipe stays open until it exits.** Closing the read end early would hand
  salloc an `EPIPE` part way through the grant, so the descriptor belongs to
  the reap callback. Nothing reads it in between, which is only safe because
  salloc's remaining output is a few hundred bytes — do not add a verbosity
  flag to the command line.

`salloc_wait_cb` is deliberately diagnostic only. The `scontrol show job
--json` poll drives the extend from submission onwards, so the reap can fire
either side of the request completing and shares no state with it.

`init` allocates `prte_slurm_session_stack` and the pending-request
tracker; `finalize` tears them down. A successful atomic modify returns
`PMIX_OPERATION_SUCCEEDED` so the base completes the request.

The JSON helpers (`ras_slurm_jansson.c`) are compiled only when the
**extensions** are built — jansson available *and* a new enough SLURM, see
the build gate below; otherwise `ras_slurm_jansson_stub.c` provides
`prte_ras_slurm_have_jansson()==false` and no-op stubs. All three `modify`
entry points ask `prte_ras_slurm_have_extensions()` before doing anything,
so a build without them refuses the request and leaves the DVM running.

---

## Things to watch when editing

- **`PRTE_EXISTS` vs `PRTE_SUCCESS`.** Returning `PRTE_SUCCESS` on a
  re-discovered jobid would double-insert the whole allocation. Keep the
  session-exists check.
- **Never set `node->session`** in `assign_new_session` (see above).
- **Taint/validate before shelling out.** Every jobid/hostname that
  reaches an `scontrol`/`salloc`/`squeue` command line must pass the
  validators; `check_taint` also bounds `SLURM_NODELIST` length.
- **Guard JSON features behind `prte_ras_slurm_have_jansson()`** so the
  no-Jansson build path stays correct.
- **Nodes an extend brings in must be `PRTE_NODE_STATE_ADDED`, not `UP`.**
  A DVM grow launches daemons on the nodes in that state and only those
  (`prte_plm_base_setup_virtual_machine`), so a node handed over as plain
  `UP` — the state an *initial* discovery uses — lands in the pool with no
  daemon and the extend adds resources the DVM cannot use. Both producers
  here have to agree: `add_modified_resources` for newly granted nodes and
  the reused-node branch of `extract_reused_nodes` for ones the pool
  already knows.
- **This component is a plugin** (`ras-slurm` is in the default
  `--enable-mca-dso` list), so nothing outside it — the ras unit test
  included — can name its symbols. `test/unit/ras` reaches it the way the
  DVM does: find the component in the framework's list, `query` it, call
  the module it returns.

### Where each half is tested

Without jansson the component is **detect-and-report only**: `query` and
`allocate` work, and `serve_extend_req`/`serve_release_req`/
`serve_cancel_req` each return `PRTE_ERR_NOT_AVAILABLE` immediately. The
coverage follows that seam.

| Half | Covered by |
|------|------------|
| `query` + `allocate` (nodelist expansion, taint refusal, `PRTE_EXISTS` on re-discovery) | `test/unit/ras/test_ras.c` — no scheduler needed, since both read only the environment |
| `modify` (extend/release/cancel, the JSON parser, `validate_hostname`, `drain_cmd_output`) | [`contrib/dockerswarm`](../../../../contrib/dockerswarm/) — it shells out and is inherently multi-node, and it is one of the two automated builds that configure `--with-jansson`, so `ras_slurm_jansson.c` is compiled nowhere else |
| the same surface against a scheduler that can refuse it | [`contrib/slurmswarm`](../../../../contrib/slurmswarm/) — ten containers running a real SLURM, so `salloc` really allocates, `scontrol update ... ReqNodeList=` really has to be a resize SLURM accepts on a RUNNING job, and the JSON is SLURM's own |

### The extensions are a separate build gate: SLURM 24.05 or newer

`prte_ras_slurm_get_jobinfo_json` reads
`job_resources.nodes.{count,list,allocation}`, which is the shape SLURM
adopted in data parser **v0.0.41**. Through 23.11 the same query answers with
`job_resources.nodes` as a plain *string* alongside a flat `allocated_nodes`
array, and every extend fails with *"Failed to parse input JSON"* — a
diagnostic that says nothing about versions and sends people looking at
PRRTE. That floor was found by `contrib/slurmswarm`, and is why its image
builds SLURM from source rather than taking the distribution package.

[`configure.m4`](configure.m4) now decides this at build time.
`PRTE_RAS_SLURM_CHECK_VERSION` asks the client tools (`scontrol`, then
`srun`/`sbatch`/`sinfo`) what version they are, and
`PRTE_RAS_SLURM_CHECK_EXTENSIONS` turns that into one flag:

| Artifact | Use |
|----------|-----|
| `PRTE_HAVE_SLURM_EXTENSIONS` (0/1) | the C gate — test with `#if`, never `#ifdef` |
| `PRTE_SLURM_VERSION_STRING`, `PRTE_SLURM_MIN_EXT_VERSION` | what the run-time diagnostic names |
| `PRTE_WANT_SLURM_EXTENSIONS` (automake) | the **build** gate: `Makefile.am` compiles `ras_slurm_jansson.c` or `ras_slurm_jansson_stub.c`, and configure drops the jansson flags entirely when off |
| `--enable`/`--disable-slurm-extensions` | the override, in both directions. A feature switch, not a package location: the component links no SLURM library, so there is nothing to point a `--with-` at |

Three things about that decision are deliberate:

- **Undetectable means enabled.** Every SLURM client establishes a
  configuration source before it will print `--version`, so on a build node
  with the binaries but no `slurm.conf` the command exits 1 saying
  *"Could not establish a configuration source"* and reports no version at
  all. Configuring somewhere that is not a SLURM client is entirely
  ordinary, and defaulting off there would quietly remove a working feature
  from builds that have always had it. Erring the other way costs exactly
  the run-time failure this check exists to explain, and no more. Use
  `--disable-slurm-extensions` to gate it off locally when you know the
  target's SLURM is older than that.
- **`--enable-testbuild-launchers` forces it on.** That flag exists to
  *compile* what the machine cannot run, so the version of any SLURM sitting
  next to it is beside the point.
- **Only the parser is switched.** The `ras_slurm_modify_*.c` sources build
  command lines and are not tied to any SLURM release, so they keep
  compiling and there is no stub surface to drift. Each of their three entry
  points calls `prte_ras_slurm_have_extensions()`
  ([`ras_slurm_modify_common.c`](ras_slurm_modify_common.c)) first, which is
  also where the two *distinct* reasons for refusal are reported. They used
  to share one message naming jansson, which is a plain falsehood on a
  machine that has jansson and a SLURM too old — and that machine is every
  stock Ubuntu 24.04.

The harness fakes the scheduler with
[`fake-slurm.py`](../../../../contrib/dockerswarm/fake-slurm.py), installed
into the swarm as `salloc`/`scontrol`/`scancel` and handing out real
container hostnames — so an extend really launches daemons and a release
really removes them. Read
[its AGENTS.md §11](../../../../contrib/dockerswarm/AGENTS.md) before adding
a case. Two things that trip people up:

- **The request shapes are not a plain grow.** `modify()` accepts only
  `PMIX_ALLOC_EXTEND`+`NUM_NODES`, `PMIX_ALLOC_RELEASE` with one of
  `NODE_LIST`/`NUM_NODES`/`ALLOC_ID`, and `PMIX_ALLOC_REQ_CANCEL`. A
  node-naming `PMIX_ALLOC_NEW` never reaches this component — the base
  serves it.
- **An extend emits no phase-two completion event.** Its nodes go into the
  general pool (`node->session` stays NULL, by design — see above), and the
  directed event is addressed to the requestor recorded on a *reservation*.
  Phase one carries the result. A release does go through a shrink campaign,
  so it does emit `PMIX_DVM_IS_READY`.

Some slot counts are asserted from `ras_base_verbose` output rather than
from the node pool: the verbose line is what *this component* computed,
so it isolates a parser error from anything the launch path does with the
number afterwards.

### Reference ownership across the session/tracker web

This component holds nodes and sessions in three places at once; the
rules are easy to get wrong and the failures are silent leaks.

- **`session->nodes` holds a retained reference per node**
  (`assign_new_session` does `PMIX_RETAIN` before
  `pmix_pointer_array_add`). Anything that removes a node from that array
  must release it.
- **`prte_ras_slurm_detach_nodes` hands the dropped nodes back through
  its `removed_nodes` out-parameter and does NOT release them.** That
  parameter exists precisely so the caller can. `PMIX_DESTRUCT` on a
  `pmix_pointer_array_t` frees the array, never its items — destructing
  it without releasing first leaks one reference per detached node on
  every partial shrink.
- **`prte_slurm_session_stack` retains a reference on each session it
  holds** (released automatically by `prte_session_stack_item_t`'s
  destructor), independent of the session's own creation reference —
  removal sites just do `pmix_list_remove_item` + `PMIX_RELEASE(item)`.
- The creation reference is separate: `rollback_session` and
  `shrink_complete`'s full-release branch also release the session
  itself, ending the allocation's lifecycle. `prte_ras_slurm_drain_session_stack`
  (`finalize()`) releases only what the stack retained and leaves the
  creation reference alone — releasing it too could double-free if
  whatever else holds it releases its own copy later.
- No more reactive `release_allocation` hook: the stack's own reference
  means `session_des` can't run until the item is already removed, so
  `scancel` is decided explicitly at each removal site instead.
- `prte_ras_slurm_rollback_session` relies on `session_des` clearing
  `prte_sessions[index]`, so the released session leaves no dangling
  global entry. It deliberately does not `scancel` — the `complete:`
  block in the extend path does that via `cancel_pending_req`.
- `pmix_pointer_array_add` returns an **index**, not a status. The
  `0 > rc` tests are correct as failure checks, but feeding that value to
  `prte_pmix_convert_status()` yields a meaningless error code.
</content>
