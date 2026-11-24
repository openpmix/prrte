# AGENTS.md — the pbsswarm harness: PRRTE under a real PBS

Ten `ubuntu:24.04` containers on one bridge network running a real PBS —
OpenPBS, built from source: `pbs_server`, `pbs_sched`, `pbs_comm` and the
server's PostgreSQL datastore on node1, and a `pbs_mom` on every node — with
PRRTE built from your **live** working tree into a volume they all mount.
`qsub` runs real jobs, `pbs_tmrsh` and the TM API start real tasks, `qstat
-xf` reports what PBS really charged, and `qdel` really kills.

It is the sibling of [`contrib/slurmswarm`](../slurmswarm/) and deliberately
the same harness in every respect but the scheduler: same `build.sh` /
`docker-compose.yml` / `run-tests.sh` shape, same shared-volume build of your
live tree, same `node1..node10` hostnames. If you know that one you know this
one; the differences are in §3, §8 and §9.

> **Almost nothing belongs here.** This suite covers what *only PBS* can
> answer — what a job's allocation is, whether the daemons are tasks of the
> job, what PBS charges, what `qdel` does. Everything else about multi-node
> PRRTE belongs in [`contrib/dockerswarm`](../dockerswarm/). See §7.

---

## 1. What's here

| File | Purpose |
|------|---------|
| `build.sh` | Builds PRRTE (and optionally PMIx) from your live tree via VPATH into the shared volume. Start here. |
| `run-tests.sh` | The suite. Phases: `cluster`, `ras`, `plm`, `accounting`, `qdel` (§5); name some to run only those. |
| `Dockerfile` | Base image: toolchain, **OpenPBS built from source** (pinned commit), a baked PMIx, the job user `prte`, SSH. No PRRTE. |
| `node-entrypoint.sh` | Starts one node: `/etc/hosts`, the job user's ssh key, PBS — every container-specific step is commented there (§8). |
| `pbs-alloc` | Holds a real PBS allocation across many `docker exec` calls and replays the job's environment (§6). |
| `tm-probe.c` | Asks the TM API directly what a TM launcher relies on (§5). Built by `build.sh` into `/opt/prte/pbsswarm/bin`, linked against OpenPBS's `libpbs` and nothing of ours. |
| `docker-compose.yml` | The ten nodes `prtepbs-node1..10`; every global name derives from `$PRTE_PBS_SWARM`. |

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
        ┌────────────────────────────────────────┐
        │  node1         node2 ... node10        │
        │  pbs_server    pbs_mom   pbs_mom       │
        │  pbs_sched                             │
        │  pbs_comm      /home/prte shared       │
        │  postgres      /opt/prte read-only     │
        │  pbs_mom                               │
        └────────────────────────────────────────┘
```

PBS runs no job as root, and PRRTE should meet PBS as the ordinary user it
runs as on a cluster, so **everything runs as `prte`** (uid 1000), whose home
is a volume shared by every node — where PBS returns a job's output files
and where `pbs-alloc` keeps a held allocation's environment.

## 3. Prerequisites

Docker with `docker compose`, a host autotools toolchain for `autogen.pl`,
and network access for the first image build (it clones OpenPBS and PMIx).
The image build takes a few minutes; after that `build.sh` rebuilds only
PRRTE. As with slurmswarm there is no macOS mode.

## 4. Quick start

```sh
# from this directory (contrib/pbsswarm/)
./build.sh                 # image (OpenPBS + PMIx), then PRRTE from your tree
docker compose up -d       # start prtepbs-node1 .. prtepbs-node10
./run-tests.sh             # the suite (or: ./run-tests.sh cluster plm)
```

`PMIX_SRC=/path/to/openpmix ./build.sh` builds that PMIx instead of the
baked one (`PMIX_CONFIG_ARGS` adds configure arguments); a later plain
`./build.sh` removes it from the volume again. Two
clusters on one host: `export PRTE_PBS_SWARM=alt` for the whole shell.

### Driving it by hand

```sh
N=prtepbs-node1
AS() { docker exec -u prte -w /home/prte "$1" bash -lc ". /etc/profile.d/pbs.sh; $2"; }

AS $N 'pbsnodes -a | grep state'             # ten free moms
AS $N 'pbs-alloc new --nodes 3 --ncpus 2'    # hold an allocation
AS $N 'pbs-alloc ms; pbs-alloc nodes'        # its node, its nodes
M=prtepbs-$(AS $N 'pbs-alloc ms')            # run things THERE, in its environment:
AS $M 'eval "$(pbs-alloc env)"; prterun --map-by ppr:1:node hostname'
AS $N 'pbs-alloc free --all'

# or a real batch job, which is what the accounting cases use
AS $N 'echo "prterun --map-by ppr:1:node hostname" | qsub -l select=3:ncpus=1 -l place=scatter'
AS $N 'qstat -xf <id> | grep resources_used'
```

**A held allocation is only meaningful on its own node.** `$PBS_NODEFILE`
and the job's `$TMPDIR` live on the mother superior, and the TM calls
`pbs_tmrsh` makes go to that node's mom. `pbs-alloc ms` says which node; run
there.

## 5. What the phases assert

**`cluster`** — not a PRRTE test, and it runs first because every later phase
blames PRRTE for what it finds. Ten free moms; a ten-node job whose node file
names the nodes by *hostname* (§8 says why that is not automatic); and the
properties of PBS any launcher depends on:

- `pbs_tmrsh` starts a command on a sister node — and gives it **no stdin**
  and **the job's environment, not the caller's**. That pair is why a DVM
  key cannot reach a `pbs_tmrsh`-started daemon, and why PRRTE does not use
  it.
- `tm_spawn()` **delivers the environment it is handed** (`tm-probe`), its
  tasks **run to the end after the spawner has exited**, and PBS **charges
  the job** for them. Those are the three things `plm/tm`, which calls TM
  from a helper process, relies on - checked against the API rather than
  assumed.
- `pbs-alloc` holds an allocation and replays its environment.

**`ras`** — a held allocation of three nodes with two cpus each: `ras/pbs` is
the source, every node arrives with the slots PBS assigned, a job with no
`-n` fills them, and `--host` outside the allocation is refused.

**`plm`** — the launch, by `plm/tm` and its helper `prte_tm_spawn`. The
assertion that matters is launcher-agnostic: **every remote daemon is a task
of the job** — descended from its node's `pbs_mom`, not from `sshd` —
because only then does PBS account for it and clean it up. Also: the job ran
on every node and no daemon is left behind; the DVM key is on no daemon's
command line, is gone from each daemon's environment and was never in the
helper's; a daemon that cannot start (`prte_launch_agent` naming nothing)
fails the launch at once rather than leaving it waiting; and with the helper
missing (`PRTE_LIBEXECDIR` pointed elsewhere) PRRTE says so and launches
with ssh.

**`accounting`** — the reason a PBS launcher exists. A three-node PRRTE job
whose ranks each spin for 10 s must be charged for all three nodes
(`resources_used.cput`); the same job with `--prtemca plm ssh` is the
control, and is charged for less.

**`qdel`** — deleting the job takes every daemon of a DVM running in it down
with it.

## 6. Holding an allocation across `docker exec` (`pbs-alloc`)

Same idea as slurmswarm's `slurm-alloc`. A person runs `qsub -I` and works in
that shell; a suite of separate `docker exec`s cannot. `pbs-alloc new`
submits a job whose script saves its environment and node name in
`~prte/.pbs-alloc/` and sleeps, holding the allocation; `env` prints the
`PBS_*` variables (plus `NCPUS`, `OMP_NUM_THREADS`, `TMPDIR`) back as export
lines. **Captured, never reconstructed** — the cases replay what PBS put in
a job's environment, not anyone's idea of it.

A process started this way is not inside the job's session, so PBS does not
charge the job for *it* — only for tasks the TM API started. That is why the
accounting cases use real batch jobs instead.

## 7. What this harness does not cover

Anything with no `PBS_` in it — use `contrib/dockerswarm`. And, within PBS:

- **Altair PBS Pro's commercial build.** That build's TM library carries
  private copies of libevent that conflict with PMIx's — the reason PRRTE
  calls TM only from its helper, never in-process. OpenPBS's `libpbs` links
  no libevent, so the conflict cannot be reproduced here; that the helper
  avoids it has to be confirmed on a PBS Pro site.
- PBS's cgroup hook, multi-vnode hosts, real scale.

## 8. The PBS configuration, and what a container costs

Each of these cost an afternoon in the first bring-up; all of them are in
[`node-entrypoint.sh`](node-entrypoint.sh) and the compose file.

- **`/etc/hosts` for every node.** PBS knows a node by the name its address
  resolves *back* to, and Docker's DNS answers a reverse lookup with the
  container name and network (`prtepbs-node2.prtepbsswarm_dvm`). Without the
  entries the node file names nodes by those, and `pbs_tmrsh node2` is refused
  with "host is not a node in job". `cluster` checks the node file for this.
- **`init: true`.** PBS's init script stops and restarts daemons by watching
  their pids; under a PID 1 that never reaps, every daemon that exits stays a
  zombie and the script waits on it forever (a `pbs_init.d restart` hangs).
- **`$min_check_poll 1`, `$max_check_poll 2`** in each mom's config. A mom
  samples a job's resource use periodically and a sister reports its share
  on the same cycle; with the defaults a job of a few seconds is charged for
  a fraction of what it used, and the accounting cases would measure the
  sampling instead of the launcher. Even so, keep a few seconds of tail after
  the last task: a job that ends the instant its remote task does can lose
  that node's final report.
- **The containers share the host's CPUs, so the accounting jobs run with
  `--bind-to none`.** Each container sees the same cpus, and PRRTE binds every
  node's first rank to the same core - so three "nodes" spinning in parallel
  share one real cpu and PBS charges a third of what the test expects. That
  read exactly like a launcher whose remote nodes go unaccounted (9 s of 30,
  and 3 s for the ssh control); it was the binding.
- **`$usecp *:/home/ /home/`.** The home is shared, so a job's output is
  copied back, not `scp`'d.
- **`flatuid`, `job_history_enable`.** One uid space; and `qstat -xf` answers
  for a finished job, which is where its `resources_used` are read.
- Harmless at startup: "Failed to protect PBS from Linux OOM killer" and a
  `ulimit` it may not raise — a container's limits.

## 9. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PRRTE source edit | `./build.sh` |
| also an openpmix edit | `PMIX_SRC=/path/to/openpmix ./build.sh` |
| another OpenPBS commit | `OPENPBS_REF=<sha> ./build.sh image`, then `docker compose up -d --force-recreate` |
| clean PRRTE rebuild | `docker volume rm prtepbs-build && ./build.sh` |
| tear down | `docker compose down` (the build and home volumes persist; `docker volume rm prtepbs-home` resets the home) |

The image is where PBS lives, so a container older than its image is running
a different PBS: `run-tests.sh`'s preflight refuses, and `docker compose up
-d --force-recreate` is the fix. Its preflight also refuses a build without
`plm/tm`, its helper, or `ras/pbs`, which would leave every case launching
over ssh.
