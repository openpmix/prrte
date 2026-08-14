# Collective scaling measurement on real hardware

This directory holds one measurement client and two ways to drive it.

| file | what it is |
|---|---|
| `scaletest.c` | A PMIx client that times a full-data `PMIx_Fence` (the modex) and a bare barrier over the whole job, printing absolute timestamps per rank. |
| `cluster-sweep.sh` | Drives that client across a **real allocation** — this is the script to hand to someone with a cluster. |
| `../dockerswarm/scaletest.sh` | Drives it across a container swarm on one host. Good for shaking out the mechanics; see the limits below. |

## If someone just handed you this: what to run

You need an allocation, and PRRTE (`prte`, `prun`, `pterm`, `prte_info`) on
your `PATH`. Nothing else — no containers, no root, no scheduler-specific
launcher. The script finds the nodes itself from SLURM, PBS, or a hostfile.

```sh
salloc -N 128 -t 6:00:00          # or sbatch / qsub / --hostfile
cd contrib/scaling

./cluster-sweep.sh --dry-run      # 1. see the plan and a time estimate
./cluster-sweep.sh --quick        # 2. ~15 min, proves the setup works
./cluster-sweep.sh                # 3. the real thing
```

**Please run `--quick` first.** It exercises every code path in about fifteen
minutes, so a typo in a path or a missing `timeout` costs you that rather than
four hours. When it finishes, the estimate printed by `--dry-run` can be
replaced with the real per-job cost you just observed.

The full sweep is roughly **2 hours on 16 nodes and 4 hours on 128**. Trim it
with `--phases`, `--reps`, `--nodes` or `--radix` if that does not fit.

At the end it writes `scaling-results-<host>-<date>.tar.gz`. **Send that back
whole** — it contains the raw samples, a summary, a manifest describing the
machine, and the run log. The manifest matters: we have twice drawn a wrong
conclusion from numbers whose machine we could not describe afterwards.

## What the sweep is trying to settle

PRRTE moves a fence on the RML routing tree — contributions roll **up** to the
HNP, the result is broadcast back **down**. In the broadcast direction a
daemon with `r` children transmits `r` copies of the whole modex, so the cost
carries a factor of the routing radix, which defaults to **64**. Whether a
lower radix is worth the extra tree depth is the central question.

It cannot be answered on a single host, which is why this script exists. A
container swarm has no per-node uplinks for the `r` factor to contend on
(total bytes crossing the wire are the same at any radix; only the *critical
path* changes, and shortening a critical path needs independent links), and it
has far too few cores, so its wall clock reports the CPU scheduler rather than
the collective. Both effects were measured and both are structural. This
sweep needs real nodes and real links.

Each phase answers one thing:

| phase | question |
|---|---|
| `env` | what machine was this, and what build |
| `radix` | **the main one** — how does the modex cost vary with routing radix, at each DVM size |
| `payload` | where does the byte term overtake the fixed cost, at low and high radix |
| `barrier` | the same radix question with *no payload at all*, which isolates latency and per-message cost from bandwidth |
| `gds` | PMIx's `gds/shmem3` (default) against `gds/hash` — on one host this was 77% of the fence's cost at 32 daemons, and we do not know whether that holds on a cluster |
| `threads` | the OOB per-peer worker bases off against on; their stated win is link occupancy, which needs spare cores that a container host does not have |

## Reading the output

`samples.csv` is one row per measured job; `summary.txt` is the median over
repetitions. The column that matters most is **`data_cost_us`** — the
collecting fence minus the bare barrier, measured in the same job — because
that is the payload's own cost with the barrier's latency subtracted out.

Timings are the **largest per-rank duration**, not the wall clock across
ranks, so that skew left over from the previous phase is not charged to this
one.

## Three traps this script is built around

Each of these produced a confidently wrong answer before it was understood.

**A repeated fence is not a repeated sample.** The client publishes *new* keys
every iteration, so the daemon's store grows and iteration *k* is a fence over
*k* rounds of data. On a container swarm the minimum was iteration 1 in all 36
configurations measured, and iteration 10 cost 5–20× iteration 1. So this
script runs **one** measured fence per job and repeats the **job** — a fresh
job is a fresh namespace and therefore a fresh modex, which is what an
application actually pays. Never raise `--iters` to get more samples; raise
`--reps`.

**A short DVM reports a believable number for the wrong scale.** Every DVM is
verified by counting distinct hostnames before anything is measured, and a
short one is skipped rather than measured.

**The build changes the absolute numbers.** `--enable-debug` roughly doubles
them. The manifest records the configure line; compare ratios within one run,
not absolute microseconds across builds.

## Options

```
--nodes A,B,C     DVM sizes            (default: powers of 2 up to the allocation)
--radix A,B,C     routing radices      (default: 2,3,4,8,16,64)
--ppn N,M         procs per node       (default: 1)
--nkeys N         keys per rank        (default: 8)
--sizes A,B,C     bytes per key        (default: 8,128,1024,8192)
--reps N          independent jobs per configuration (default: 5)
--phases a,b      env,radix,payload,barrier,gds,threads
--hostfile PATH   use this instead of detecting the allocation
--out DIR         results directory
--job-timeout S   seconds before a job is abandoned (default: 300)
--quick           short sweep -- run this first
--dry-run         print the plan and estimate, run nothing
```

`--ppn` is worth raising on a real machine. A node's contribution to the modex
is `ppn × bytes-per-rank`, so one process per node understates the payload by
the core count — but it also isolates the *daemon* tree, which is what the
radix question is about. Running both (`--ppn 1,16`) gives each.

## The design record

`docs/plans/scalable_collectives/` is the reasoning these measurements feed:
the cost model, what has already been measured and refuted, and what the
options are. Read it before changing what this sweep collects.
