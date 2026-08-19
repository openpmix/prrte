# AGENTS.md — the slurmswarm harness: PRRTE under a real SLURM

Orientation for AI agents and human contributors who need to run PRRTE under
an actual SLURM installation without a cluster.

Ten `ubuntu:24.04` containers on one bridge network, running a real
`slurmctld` and ten real `slurmd`s, with PRRTE built from your **live**
working tree into a volume they all mount. `salloc` grants real allocations —
including the headless `--no-shell` ones PRRTE's elastic extend creates —
`srun` launches the daemons, and `scancel` really takes them away.

It is the sibling of [`contrib/dockerswarm`](../dockerswarm/) and is
deliberately the same harness in every respect but one: **the ten containers
are running SLURM.** Same `build.sh`/`docker-compose.yml`/`run-tests.sh`
shape, same shared-volume build of your live tree, same `node1..node10`
hostnames, same conventions. If you know that harness you know this one; the
differences are in §3, §9 and §10 and nowhere else.

> **Read this first if you are here to add a test.** Almost nothing belongs
> here. This suite covers what *only a real scheduler* can answer; everything
> else about multi-node PRRTE — launch, IOF, collectives, elastic grow/shrink
> by hostname, mapping, the lot — belongs in `contrib/dockerswarm`, which is
> faster and does not need a scheduler at all. See §12.

---

## 1. What's here

| File | Purpose |
|------|---------|
| `build.sh` | Builds PRRTE (and optionally PMIx) from your **live** tree via VPATH into the shared volume the cluster mounts. Start here. |
| `run-tests.sh` | Runs the suite and reports PASS/FAIL. Five phases: the cluster itself, `ras/slurm` allocation semantics, `plm/slurm` launch, the elastic modify surface, and a launch smoke test. |
| `Dockerfile` | Base image: toolchain, a baked PMIx, **SLURM built from source**, munge, SSH wiring. It does **not** contain PRRTE. |
| `slurm.conf` | The cluster configuration, one copy baked into the image. Every container-specific choice is commented in the file; see §9. |
| `cgroup.conf` | `CgroupPlugin=disabled`. Two lines, and the reason the containers can stay unprivileged; see §9. |
| `slurm-alloc.py` | Creates and holds a real allocation across many `docker exec` calls, and replays the environment SLURM put in it. See §11. |
| `slurm-shim.py` | A recording, optionally misbehaving, **wrapper** around the real `salloc`/`scontrol`/`scancel`. Answers the two questions slurmctld cannot: what PRRTE *asked* for, and what PRRTE does when the scheduler misbehaves. See §14. |
| `docker-compose.yml` | The ten nodes `prteslurm-node1..prteslurm-node10`. Every name derives from `$PRTE_SLURM_SWARM`, so two clones can each run a cluster. |
| *(no file here)* | The `elastic` and `fencer` test clients are compiled from [`../dockerswarm/`](../dockerswarm/) rather than copied — they are the same programs, and two copies would drift. |

## 2. How it works

```
              your live PRRTE tree  (bind-mounted read-only)
                        │
                        ▼
                 builder container
             VPATH -> /opt/prte/vpath-linux
             install -> /opt/prte  (shared volume)
                        │
                        ▼
        ┌───────────────────────────────────────┐
        │  node1        node2 ... node10        │
        │  slurmctld    slurmd    slurmd        │
        │  slurmd       prted     prted         │
        │  prte (HNP)                           │
        │  ── all mount /opt/prte:ro ──         │
        └───────────────────────────────────────┘
                        ▲
                run-tests.sh drives node1
```

- **Never stale, no commit.** Change a file, rerun `build.sh`, and the cluster
  runs your change (the build is incremental).
- **One source tree, two harnesses.** This and `contrib/dockerswarm` build the
  same tree into two different volumes with two different sets of configure
  arguments. That is supported. What they must not share is an *in-tree*
  build — see "When a distclean is actually needed" in the sibling's
  [`AGENTS.md`](../dockerswarm/AGENTS.md), which applies verbatim here.

## 3. Prerequisites

- Docker (with `docker compose`) and `git`.
- A working autotools toolchain on the host for `autogen.pl`.
- **Network access during the first image build**: it clones PMIx and
  downloads a SLURM release tarball from `download.schedmd.com`.
- Time. The first image build compiles both PMIx and SLURM; budget ten to
  twenty minutes. After that it is cached and `build.sh` only rebuilds PRRTE.

Unlike the sibling harness there is no macOS mode: the subject is a scheduler
that does not run on a Mac. For a native host build use
`../dockerswarm/build.sh macos` — it builds the same source tree.

## 4. Quick start

```sh
# from this directory (contrib/slurmswarm/)
./build.sh                 # image (PMIx + SLURM), then PRRTE from your tree
docker compose up -d       # start prteslurm-node1 .. prteslurm-node10
./run-tests.sh             # the suite
```

Rebuild after editing PRRTE: just rerun `./build.sh` (incremental). No image
rebuild, no `docker compose` restart — the nodes read the shared volume. To
also test an openpmix change, add `PMIX_SRC=/path/to/openpmix`.

### Two clones on one host: `PRTE_SLURM_SWARM`

Every global name — the compose project, the ten container names, the build
volume, the docker network — derives from `$PRTE_SLURM_SWARM` (default
`prteslurm`):

```sh
export PRTE_SLURM_SWARM=alt      # for the WHOLE shell
./build.sh && docker compose up -d && ./run-tests.sh
```

It is deliberately **not** `$PRTE_SWARM`, which is the sibling harness's
variable. The two are meant to be drivable from one shell, and a shared
variable would point both at the same ten container names.

Both clusters contain a container whose **hostname is `node1`**, and that is
fine: each user-defined bridge network runs its own embedded DNS, so `node2`
inside a cluster can only ever mean that cluster's node2 — and, here, only
that cluster's `slurmctld` is reachable at `node1:6817`.

## 5. Driving it by hand

```sh
NODE=prteslurm-node1
SA='docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 '$NODE' bash -lc'

# look at the cluster
docker exec $NODE sinfo
docker exec $NODE squeue

# take an allocation and hold it (see §11 for why this is not just `salloc`)
docker exec $NODE slurm-alloc new --nodes 3 --tasks-per-node 2
docker exec $NODE slurm-alloc env          # what SLURM actually set

# start a DVM inside it -- prte_elastic_mode is REQUIRED for extend/release
$SA '. /opt/prte/env.sh; eval "$(slurm-alloc env)";
     nohup prte --prtemca prte_elastic_mode 1 --prtemca ras_base_verbose 5 \
           >/tmp/prte.out 2>&1 & sleep 8'

$SA '. /opt/prte/env.sh; eval "$(slurm-alloc env)"; prun -n 3 --map-by node hostname'
$SA '. /opt/prte/env.sh; eval "$(slurm-alloc env)"; elastic extend 2'
$SA '. /opt/prte/env.sh; eval "$(slurm-alloc env)"; elastic release 2'
$SA '. /opt/prte/env.sh; eval "$(slurm-alloc env)"; pterm'

docker exec $NODE slurm-alloc free --all   # hand the nodes back
```

`eval "$(slurm-alloc env)"` is what puts the allocation in the shell. Without
it, `ras/slurm` and `plm/slurm` do not even activate — `SLURM_JOBID` is the
gate for both — and you get an ordinary ssh-launched DVM that looks fine and
is testing nothing. That is the mistake this harness makes easy to make; the
suite has a case for the other side of it (`plm/slurm: no allocation means no
SLURM launcher`).

> **The other flag that bites you.** As in the sibling harness, PRRTE gates
> all of the grow/shrink completion machinery behind `prte_elastic_mode`
> (default off). Without `--prtemca prte_elastic_mode 1`, a release returns
> phase-1 SUCCESS and never completes; the client times out after 60s.

## 6. What "success" looks like

The five phases, and what each is really asserting.

**`test_cluster`** — not a PRRTE test. A ten-node `srun`, an allocation held
and handed back, `SLURM_JOBID` still being set (see §13), and the JSON schema
check of §10. It runs first because every later phase blames PRRTE for what
it finds, and a broken cluster would make all of them lie.

**`test_ras_alloc`** — what an allocation *means* to the DVM. The DVM forms
across the whole allocation with no `--host`; the slot count is SLURM's, not
the core count; `--host` selects within the allocation and refuses outside
it. Two cases here cannot exist anywhere else:

- **SLURM's compressed node list.** SLURM hands out `node[2,4,6]`, and
  `ras/slurm` has its own parser for that notation — a fake scheduler handing
  out comma-separated names never reaches it. The allocation is deliberately non-contiguous: a
  parser that reads `node[2,4,6]` as the range 2..6 gets five nodes, three of
  which it was never given, and the case asserts that neither node3 nor node5
  was invented.
- **An allocation that excludes the head node.** `prte` runs on node1 while
  SLURM allocated node2 and node3 only, which also means `srun` is issued
  from a host that is not part of the job.

**`test_plm`** — the launcher. This component is unreachable from the sibling
harness at all: `plm/slurm` shells out to **`srun`**, and there is no SLURM of
any kind over there, so every DVM it builds goes out over ssh no matter what
the ras was told. The cases assert that `plm/slurm` won
selection, that the command really is `srun`, that it carried
`--jobid=<the allocation>` (a launcher that omitted it would work on an idle
cluster and queue a second job on a busy one), that PRRTE read the srun exit
as a **hand-off** rather than a failure once `prted` daemonized, that SLURM
is left with no dangling job step, that `pterm` does not cancel the user's
allocation, and — the other side of the gate — that with no allocation in the
environment the ssh launcher runs instead and creates no SLURM job.

**`test_elastic`** — the phase this harness exists for. Every command goes to
a scheduler that can say no: `salloc` really allocates a job, `scontrol show job
--json` really emits SLURM's own schema, `scontrol update` really has to be a
resize SLURM accepts **on a running job**, and `scancel` really removes an
allocation. Note the request shapes differ from a plain elastic grow:
`elastic grow <nodes>` is `PMIX_ALLOC_NEW` naming hosts, which the *base*
serves without consulting the ras. `ras/slurm`'s `modify()` accepts only
`PMIX_ALLOC_EXTEND`+`NUM_NODES`,
`PMIX_ALLOC_RELEASE`+(`NODE_LIST`|`NUM_NODES`|`ALLOC_ID`) and
`PMIX_ALLOC_REQ_CANCEL` — which nodes an extend lands on is the scheduler's
choice. Hence `elastic extend|release|release-id|cancel`.

Four cases are worth calling out:

- **The in-place resize.** A partial release keeps the SLURM job and shrinks
  it with `scontrol update job <id> ReqNodeList=<survivors>`. Whether SLURM
  accepts that on a RUNNING job is precisely the assumption a fake scheduler
  could never check, and the case asserts the scheduler's own node count came
  down — not merely that PRRTE issued a command.
- **A pending extend, with no fault injection.** A fake scheduler has to be
  *told* to make a job pend. Here the case asks for more nodes than the
  cluster has free and SLURM queues it PENDING by itself, which is the state
  `PMIX_ALLOC_REQ_CANCEL` exists for.
- **An extend that lands on a node an earlier release gave back.** Reported
  as a hang, and the half of it `ras/slurm` owns: a grow completes only
  through `DAEMONS_REPORTED` → `VM_READY`, and neither of the counters that
  gate it is decremented when a daemon departs, so the fence the extend
  raises comes down only if the daemon on the reused node reports in. Which
  node an extend lands on is the *scheduler's* choice, so the case has to
  leave SLURM no alternative: the first extend asks for the **whole free
  pool**, and after two of those nodes are released they are the only nodes
  left to grant. Do not shrink that first request to a fixed size — with a
  spare node anywhere in the cluster the scheduler may grant one that was
  never in the DVM, and the case would assert nothing while looking like it
  passed. The completion assertion runs whatever node was granted (an extend
  that does not complete is a bug wherever it landed); only the claim that a
  *reuse* happened is downgraded to a skip when the scheduler had a spare.
  (The sibling harness covers the same shape chosen by *hostname*; [#2491]
  closed the routing half of it.) The case ends with a **fence** spanning the
  head node and the granted-back one: a daemon launched after a release starts
  at collective recovery epoch zero and its contributions are dropped as stale,
  which every placement assertion above it passes straight through.
- **Which brings in the one piece of SLURM bookkeeping this suite has to
  work around.** A node released by an in-place resize can sit in state
  `IDLE` with its cores still accounted to the job that was shrunk off it —
  `scontrol show node` reports `State=IDLE` and `CPUAlloc=8` together, and
  slurmctld frees them on its own schedule — so an allocation naming that
  node pends on `Reason=Resources` until the queue drains. `sinfo -t idle`
  cannot see the difference; `grantable_nodes()` asks for the free-CPU count
  as well, and every case that needs to know what the scheduler can hand out
  must go through it. A case that takes `IDLE` at face value waits out its
  timeout and then reports the scheduler's accounting as a PRRTE failure.

The tainted-hostname case is carried over from the sibling harness because
the stakes are different here: the command that would be built is one a live
`slurmctld` would act on.

Three more groups came over when the fake scheduler was retired:

- **A release issued while a grow is in flight** ([#2617]). Grow and shrink
  share one launch fence and one broadcast, and the shrink half had no notion
  of an in-flight grow: it was xcast to daemons that were still joining and
  could not be reached, so the campaign neither completed nor aborted and
  every later job parked at the `LAUNCH_APPS` hold. The interleaving is
  opportunistic on purpose — do not "stabilize" it with a sleep between the
  two requests, because the sleep is the bug.
- **What PRRTE put on the `salloc` command line**, and the `propagate_*`
  parameters that gate it. §14.
- **Malformed JSON and a failing `scancel`.** §14.

**The phase is a sequence of independent groups, and that is load-bearing.**
Each group is its own shell function so that one which cannot proceed can
`return` without taking the others with it. The shape this replaced did
exactly that: an extend that came back refused returned out of the *whole*
phase, so five later groups — none of which depended on it — silently did not
run, and the only visible symptom was a suite total about fifty checks lower
than the run before. A group that gives up now says what it gave up on, and
the caller runs the next one regardless.

[#2617]: https://github.com/openpmix/prrte/issues/2617
[#2491]: https://github.com/openpmix/prrte/issues/2491

**`test_launch`** — a deliberately short smoke test. Ranks spread over the
allocated nodes, output forwarded back, a failing job reported as failing,
and a job larger than the allocation refused rather than quietly
oversubscribing nodes the scheduler believes are someone else's.

## 7. Cleanup hygiene

`run-tests.sh` cleans between phases, and its sweep has **two** halves where
the sibling harness has one:

```sh
# 1. the PRRTE half -- identical to contrib/dockerswarm
for n in $(seq 1 10); do
  docker exec prteslurm-node$n sh -c '
    for t in prted prte prterun prun pterm; do pkill -9 -x $t; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* /tmp/pmix.*
    find /tmp -maxdepth 2 -name "pmix.*" -prune -exec rm -rf {} +
    true'
done

# 2. the scheduler half -- give the nodes back
docker exec prteslurm-node1 slurm-alloc free --all
docker exec prteslurm-node1 sinfo          # wait for ten idle nodes
```

The second half is not optional and is easy to forget. A PRRTE **extend**
holds its nodes through a SLURM job of its own, and a run that left those
standing starves every later case of the pool it needs — reported, of course,
as "the grow did not happen", several cases further on. `slurm-alloc free
--all` cancels *every* job in the queue, deliberately: this is a dedicated
single-purpose cluster and there is nothing in it that is not the harness.

Two things about cgroups being off (§9): SLURM's process tracking is
best-effort here, so a stray application process is SLURM's to lose — the
per-node `pkill` sweep is what actually guarantees a clean slate — and
`cleanup_cluster` **waits for the cluster to go idle** rather than assuming a
cancelled job frees its nodes at once. A case that asks for nodes before
slurmctld has reaped the last one does not fail, it *pends*, which reads as a
hang.

## 8. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PRRTE source edit | `./build.sh` (incremental into the volume) |
| pick up an openpmix edit | `PMIX_SRC=/path/to/openpmix ./build.sh` |
| force a clean PRRTE rebuild | `docker volume rm prteslurm-build && ./build.sh` |
| test another SLURM release | `docker build --build-arg SLURM_VERSION=25.05.3 -t prte-slurm-swarm:latest .` then wipe the volume's VPATH dirs and `docker compose up -d --force-recreate` |
| tear down the cluster | `docker compose down` (the `prteslurm-build` volume persists) |
| run a second, independent cluster | `export PRTE_SLURM_SWARM=alt` and repeat the quick start |

**The containers persist, so a rebuilt image does not reach them.** Same trap
as the sibling harness, and here it has a second face: the image is where
*SLURM* lives, so a container older than its image is running a different
scheduler than the one you just configured. `run-tests.sh`'s preflight
compares each container's image ID against the tag and refuses to run when
they differ. The fix is `docker compose up -d --force-recreate`.

**And the image is where the baked PMIx lives.** Rebuilding the image for a
new SLURM also rebuilds PMIx from master, so PRRTE in the volume was linked
against the *previous* one. The configure arguments have not changed, so
`build.sh` will not reconfigure and an incremental `make` will not relink —
wipe the build directory:

```sh
docker run --rm -v prteslurm-build:/opt/prte prte-slurm-swarm:latest \
    rm -rf /opt/prte/vpath-linux /opt/prte/prte /opt/prte/.build-stamp
./build.sh
```

## 9. The SLURM configuration, and what a container costs

Everything below lives in [`slurm.conf`](slurm.conf) and
[`cgroup.conf`](cgroup.conf), commented in place. The four settings that are
not ordinary cluster configuration:

- **`SlurmdParameters=config_overrides`.** All ten "nodes" are containers on
  one host, so every `slurmd` reports the same CPU count — whatever the docker
  VM was given, which is not the 8 that `NodeName=` claims and differs per
  developer machine. Without this, `slurmctld` compares each node's report
  against the configuration, finds it short, and marks the node INVALID:
  `sinfo` shows *drain* with reason "Low socket\*core\*thread count" on every
  node and nothing can ever be allocated. The numbers here are a fiction we
  have chosen, and the jobs are `hostname` and `sleep`.
- **`CgroupPlugin=disabled`.** `slurmd` initializes a cgroup context at
  startup *whatever* `ProctrackType` and `TaskPlugin` are set to, in order to
  put `slurmstepd` in a scope of its own. A container has neither a systemd to
  ask over D-Bus nor a writable `/sys/fs/cgroup`, so without this it dies at
  startup and `sinfo` shows ten `unk*` nodes forever — which reads like a
  networking or munge problem and is neither. This option arrived in SLURM
  24.05, and is the second reason the image pins a modern release (§10).
  Against an older one the alternatives are `cap_add: SYS_ADMIN` plus a
  remount of `/sys/fs/cgroup` in the entrypoint, or `privileged: true`; both
  were tried and both work, and neither is worth asking of a developer's
  machine when the scheduler has a switch for it.
- **`ProctrackType=proctrack/linuxproc`, `TaskPlugin=task/none`.** The cgroup
  plugins want a cgroup hierarchy to own. `linuxproc` tracks a step by walking
  `/proc` parentage, which needs nothing a container lacks. See §7 for the
  consequence.
- **`InactiveLimit=0`.** Load-bearing. The harness holds its allocations with
  `salloc --no-shell`-style background jobs and runs steps into them minutes
  later; a non-zero `InactiveLimit` kills an allocation with no active step,
  which would delete the DVM's allocation out from under it mid-suite.

`SelectType=select/cons_tres` with `CR_Core` is chosen so an allocation can
hand out *part* of a node — PRRTE reads `SLURM_TASKS_PER_NODE` and the suite
asserts the slot count it derives, and a linear select would give whole nodes
only and the slot-count cases would be measuring nothing.

**Authentication.** munge, with a key generated at image build time so all ten
containers share one. A test cluster whose credential key sits in a local
image is not a security posture — it is why this must only ever run on a
private docker network.

## 10. Why the SLURM version is pinned: the JSON schema

The image builds SLURM from source, pinned by `--build-arg SLURM_VERSION`
(default 24.11.6), rather than installing the distribution package. That is
not about JSON *support* — Ubuntu 24.04's `slurm-wlm` does ship
`serializer_json.so` and `scontrol show job --json` works. It is about the
JSON **schema**, and it is the first thing this harness found.

Ubuntu 24.04 carries SLURM 23.11, whose newest data parser is v0.0.40. In
that schema a job's resources are:

```json
"job_resources": { "nodes": "node[1-3]", "allocated_nodes": [ ... ] }
```

`ras_slurm_jansson.c` reads the shape SLURM adopted in data parser **v0.0.41**,
which ships in SLURM **24.05** and is the default output from 24.05 onward:

```json
"job_resources": { "nodes": { "count": 3, "list": "node[1-3]",
                              "allocation": [ ... ] } }
```

Against 23.11, *every* extend fails immediately with

```
PRTE ERROR: Failed to parse input JSON in file .../ras_slurm_jansson.c at line 579
```

and the elastic surface has no coverage at all. So: **PRRTE's `ras/slurm`
requires SLURM 24.05 or newer**, and nothing in the tree said so before this
harness existed.

It says so now, at configure time.
[`src/mca/ras/slurm/configure.m4`](../../src/mca/ras/slurm/configure.m4) asks
the SLURM client tools their version and folds the answer, together with
jansson availability, into `PRTE_HAVE_SLURM_EXTENSIONS` — which gates *which
sources compile* (`ras_slurm_jansson.c` versus its stub) and what the run-time
refusal says. `--enable`/`--disable-slurm-extensions` overrides it in either direction.
A machine with no SLURM to interrogate — a build node, or this harness's
sibling — defaults to **enabled**, so nothing that worked before stops
working; see that component's `AGENTS.md` for why that is the right default.

`run-tests.sh` therefore checks two different things at preflight, and they
are genuinely independent: `PRTE_HAVE_SLURM_EXTENSIONS` in the build
directory's `prte_config.h` says whether *this build* can parse, and the live
schema probe says whether *this cluster* emits what the parser reads (the
build may have been configured somewhere else entirely). Either one wrong
skips the elastic phase rather than failing it — a red suite there would read
as "your tree is broken" when it is not.

The fields the parser needs, all present in v0.0.41 through v0.0.43
(SLURM 24.05, 24.11, 25.05):

| PRRTE reads | SLURM emits |
|-------------|-------------|
| `job_resources.nodes.count` / `.list` | `nodes/count`, `nodes/list` |
| `job_resources.nodes.allocation[]` | `nodes/allocation` |
| `…allocation[].cpus.{count,used}` | `cpus/count`, `cpus/used` |
| `…allocation[].sockets[].cores[].status` | core `status` (ALLOCATED/UNALLOCATED) |
| `threads_per_core.{set,infinite,number}` | the standard SLURM numeric triple |

## 11. Holding an allocation across `docker exec` (`slurm-alloc`)

A person testing PRRTE under SLURM types `salloc -N3 bash` and works in that
shell; everything they run inherits the allocation's environment. A suite
driving containers from outside has no such shell — every case is its own
`docker exec`, and an allocation created in one is gone by the next.

[`slurm-alloc.py`](slurm-alloc.py) closes that gap in two halves:

```sh
slurm-alloc new --nodes 3 --tasks-per-node 2   # real salloc, held in the background
slurm-alloc env                                # export lines to eval
slurm-alloc nodes | jobid | pool | jobs
slurm-alloc free [--all]
```

`new` runs a real `salloc` whose command is a shell that **dumps its own
environment to a file** and then sleeps, holding the allocation; `env` prints
that captured environment back.

**The environment is captured, never reconstructed**, and that distinction is
the point. A reconstruction would encode this author's belief about which
variables SLURM sets, and the interesting failures are exactly where that
belief is wrong. What `env` prints is what SLURM itself put in the
environment of a process inside the allocation — including the compressed
`SLURM_NODELIST` and the `2(x3)` form of `SLURM_TASKS_PER_NODE` that the
`ras/slurm` parsers exist to handle.

## 12. What this harness deliberately does not cover

Everything that is not about the scheduler. Do not add it here.

`contrib/dockerswarm` covers PRRTE across ten nodes — the launch paths, IOF
and its flow control, `grpcomm` collectives and group construct, the data
server, direct modex, `errmgr`, `odls` envars, `rml` routing, `hwloc`,
sessions, and the elastic grow/shrink surface driven by *hostname*. It is
faster, it needs no scheduler, and it is where a regression in any of that
will be found.

**Everything `SLURM_` is now here, including what used to need a fake.** That
harness once carried a stand-in control plane and a `ras/slurm` phase driven
by it; both are gone. The cases that genuinely needed something a live
`slurmctld` will not do — malformed JSON, a failing `scancel`, and the argv
assertions — moved here and are served by `slurm-shim.py` (§14), which wraps
the real commands rather than replacing the scheduler.

The division to hold to:

| Question | Harness |
|----------|---------|
| Does PRRTE issue the right command? | **here** (the shim records the argv — §14) |
| Does SLURM accept that command? | **here** |
| Does PRRTE parse what SLURM said? | **here** (against SLURM's real output) |
| Does PRRTE do the right thing with the answer? | either — prefer `dockerswarm` |
| Anything with no `SLURM_` in it | `dockerswarm` |

## 13. Things a real scheduler taught this harness

Kept as a list because each one cost an afternoon.

- **`SLURM_JOBID` is what PRRTE gates on**, and it has been deprecated in
  favour of `SLURM_JOB_ID` for years. SLURM 24.11 still sets both. If a
  future release stops setting the old name, `ras/slurm` and `plm/slurm` both
  stop activating and *every case in this suite quietly falls back to the ssh
  launcher and passes*. `test_cluster` asserts the variable exists for that
  reason, and it is the one assertion here that is about SLURM rather than
  about PRRTE.
- **The JSON schema moved in 24.05** — §10, the reason for the version pin.
- **`slurmd` needs a cgroup context even when nothing is configured to use
  one**, which is what makes SLURM-in-a-container a version question at all
  (§9).
- **An `srun` that exits is not a failed launch.** `prted` daemonizes, so the
  step ends immediately and SLURM reaps it; PRRTE has to read that as a
  hand-off. Nothing in the fake harness can produce that sequence, because
  nothing in it runs `srun`.
- **A cancelled job does not free its nodes at once.** Anything that cancels
  and then immediately allocates must wait for the cluster to go idle, or it
  pends — and a pending allocation looks exactly like a hang.

## 14. Recording the argv, and making the scheduler misbehave
([`slurm-shim.py`](slurm-shim.py))

A real scheduler answers most questions better than a fake one. It does not
answer these two:

- **What PRRTE asked for.** slurmctld records the job it created, not the
  command line that created it. Several behaviors are visible *only* in the
  argv: an attribute that must be **omitted** when its source is unset rather
  than sent with an empty value, a `propagate_*` MCA parameter that must drop
  exactly its own argument and disturb nothing else, and the absence of
  `--wrap` (an sbatch expander job's script *is* the job, so the node it runs
  on could never be released — `salloc --no-shell` is what makes an arbitrary
  shrink possible). No job record can show that an argument is absent.
- **What PRRTE does when the scheduler misbehaves.** A live `slurmctld` will
  not emit unparsable JSON or fail a `scancel` on request, and those paths
  exist for precisely that.

`slurm-shim.py` is installed by `build.sh` as
`/opt/prte/slurmshim/bin/{salloc,scontrol,scancel}` — dispatching on
`argv[0]`, and deliberately **not** into the install `bin/` that the node
entrypoint puts on every PATH. A case opts in by starting its DVM with that
directory first (`DVM_SHIM=1` in `run-tests.sh`), so nothing else in the
suite can be perturbed by it. It is the **HNP** that needs it: the HNP is
what shells out, the tools never do.

```sh
slurm-shim reset                 # clear argv records and faults
slurm-shim argv                  # argv of the most recent salloc
slurm-shim audit                 # every wrapped command, in order
slurm-shim set bad_json 1        # scontrol --json prints garbage, exits 0
slurm-shim set scancel_fail 1    # scancel fails, verbosely
```

Three properties of it are deliberate:

- **It wraps; it does not replace.** Anything not explicitly faulted is
  passed through to the real command, so the scheduler under test stays real.
- **It `exec`s rather than forks.** PRRTE forks `salloc` itself, reads the
  job id off its output, and reaps it by pid much later. A wrapper sitting in
  the middle would have to reproduce all of that faithfully; becoming the
  command means there is nothing to reproduce. The cost is that the shim
  never sees the job id, so an argv record cannot be keyed by job — `argv`
  reports the most recent one, and every case that asserts on an argv issues
  exactly one extend.
- **`bad_json` exits 0.** A non-zero status would be caught by the caller's
  status check and never reach the parser, which is the code under test.

Fault flags are armed one at a time and cleared by the case that armed them;
`DVM_SHIM` is put back to 0 after each group, or every later phase would be
recording — and possibly still faulting.
