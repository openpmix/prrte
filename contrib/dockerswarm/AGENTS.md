# AGENTS.md — the dockerswarm multi-node test harness

Orientation for AI agents and human contributors who need to run PRRTE
across multiple nodes without a cluster. **This directory is the
canonical multi-node harness** — if you are about to hand-craft
containers, tar a source tree into a volume, or patch files inside a
running node, stop: `build.sh` below already does the right thing
(bind-mounts your live working tree into a builder and compiles it
out-of-tree into the shared volume the nodes read).

A small, self-contained harness for exercising PRRTE across several container
"nodes" — a persistent DVM, one-shot `prterun`, and the **elastic DVM**
(grow/shrink) plus multi-hop routing/relay — plus a native single-host build on
the host OS. It is the quickest way to bring up a multi-node DVM, drive a grow
and a shrink, and watch the two-phase completion events.

It is **not** a Docker Swarm in the orchestration sense — just ten plain
`ubuntu:24.04` containers on one bridge network, each acting as a DVM node.
"Swarm" is only the nickname.

> **What changed:** this harness now builds your **live working tree** — no
> commit required and never stale. `build.sh` bind-mounts the source into a
> builder container and compiles it **out-of-tree (VPATH)** into a shared
> volume the nodes mount, and also builds natively on the host for macOS
> coverage. The old "git-archive the committed tree into the image" flow (and
> the copy-files-into-ten-containers workaround) is gone.

> Orientation for an AI agent or new contributor: read this file top to bottom,
> then run the Quick start. The one thing that will silently waste your time is
> forgetting `--prtemca prte_elastic_mode 1` when starting the DVM by hand — see
> §5. (`run-tests.sh` already passes it.)

---

## 1. What's here

| File | Purpose |
|------|---------|
| `build.sh` | Builds PRRTE (and optionally PMIx) from your **live** tree via VPATH: into a shared volume for the Linux swarm, or natively for macOS. Start here. |
| `run-tests.sh` | Runs the test suite and reports PASS/FAIL: the full multi-node suite on Linux, a single-host subset on macOS. |
| `Dockerfile` | Base image: toolchain, a baked PMIx, SSH wiring, and a node entrypoint. It does **not** contain PRRTE. |
| `docker-compose.yml` | The ten nodes `prte-node1`..`prte-node10`, each mounting the shared `prte-build` volume. |
| `elastic.c` | The elastic test client (`elastic` in the install): issues a PMIx allocation request and waits for the phase-two completion event. |
| `fake-slurm.py` | A stand-in SLURM control plane (`sbatch`/`scontrol`/`scancel`) so `ras/slurm`'s elastic modify surface can be exercised. See §11. |

## 2. How it works

```
              your live PRRTE tree  (bind-mounted read-only)
                        │
        ┌───────────────┴───────────────┐
        │ build.sh linux                │ build.sh macos
        ▼                               ▼
  builder container                native on host
  VPATH -> /opt/prte/vpath-linux   VPATH -> <repo>/vpath-macos
  install -> /opt/prte  (volume)   install -> <repo>/vpath-macos/install
        │                               │
        ▼                               ▼
  10 nodes mount /opt/prte:ro      run-tests.sh macos
  run-tests.sh linux              (single-host smoke)
```

- **One source, two builds.** The source tree is compiled *out of tree*, so a
  Linux build (`vpath-linux`, inside the container) and a macOS build
  (`vpath-macos`, on the host) coexist from the same pristine sources.
- **Never stale, no commit.** Change a file, rerun `build.sh`, and the swarm
  runs your change (the build is incremental).
- **Both code bases (optional).** Set `PMIX_SRC=/path/to/openpmix` and `build.sh`
  builds PMIx from source too; otherwise PMIx is the copy baked into the image
  (Linux) or an installed PMIx (macOS).

### When a distclean is actually needed

**The rule: an out-of-tree build cannot share a source tree with an
in-tree build. If the source tree holds one when `build.sh` runs, it has
to go — every time, not just the first.**

There are two distinct reasons, and the second is the one that actually
bites:

1. **A VPATH `configure` refuses to run** while the source tree holds a
   `config.status`/`Makefile`. This is the obvious one, and it only
   applies on the first run — afterwards `build.sh` does
   `[ -f config.status ] || configure` and skips it.
2. **`VPATH = srcdir` makes an incremental out-of-tree `make` reach into
   the source tree.** Automake sets `VPATH` to the source directory, so
   make can satisfy an object target from a stale `src/**/*.lo` +
   `.libs/*.o` left there by an in-tree build. Between a macOS host tree
   and the Linux container those objects are not even the same
   architecture, and the build dies with:

   ```
   libtool:   error: 'prte_bootstrap.lo' is not a valid libtool object
   ```

   Reason 2 has no "first run only" escape — it applies to every
   incremental rebuild.

So `build.sh` distcleans whenever it finds in-tree artifacts, and says
so. `--no-distclean` skips it (only safe if the tree really is clean);
`--distclean` forces it.

**The cost, and how to avoid paying it repeatedly.** A distclean destroys
*your* in-tree build, so the next `make check`, `make -C test/offline
check-offline`, or `make install` has to reconfigure and rebuild the
whole tree first. That failure is easy to misread, because a distcleaned
tree does not announce itself — `make check` just says *"No rule to make
target `check'"* and `make` at the root exits 0 having done nothing.

If you run the host-side suites often, **stop keeping an in-tree build**:
build the host side out of tree too (`./build.sh macos`, which installs
into `vpath-macos/install`) and run `make check` from `vpath-macos`. Then
the source tree stays pristine, both builds coexist exactly as this
harness intends, and there is never anything to clean.

## 3. Prerequisites

- Docker (with `docker compose`) and `git` for the Linux swarm.
- A working autotools toolchain (`autoconf`/`automake`/`libtool`/`perl`) on the
  host for `autogen.pl` and the macOS build.
- **Network access during the first image build** (clones PMIx, installs apt
  packages). PMIx is cloned from **master** by default, because the DVM
  size-change event codes the client registers for (`PMIX_DVM_IS_READY` /
  `PMIX_ERR_DVM_MOD`) may not be in a tagged release yet.

## 4. Quick start

```sh
# from this directory (contrib/dockerswarm/)

# ---- Linux swarm ----
./build.sh                 # autogen (+ distclean only if a VPATH configure
                           #   must run), build image, build PRRTE into the
                           #   shared volume from your live tree
docker compose up -d       # start prte-node1 .. prte-node10
./run-tests.sh linux       # full suite: prterun, elastic grow/shrink, relay

# ---- native macOS (single host) ----
./build.sh macos           # native VPATH build into <repo>/vpath-macos
./run-tests.sh macos       # build + single-host launch smoke
```

**On macOS, say which dependencies to build against.** Unlike the container,
this host is whatever you have installed, and the two knobs are:

```sh
PMIX_HOME=/path/to/pmix \
EXTRA_CONFIGURE_ARGS="--with-libevent=/path/to/libevent --with-hwloc=/path/to/hwloc" \
  ./build.sh macos
```

Leaving `PMIX_HOME`/`PMIX_SRC` unset lets configure autodetect PMIx, and if
the host has more than one installed it may not pick the one you meant.
PRRTE uses PMIx *internals*, so a mismatch is not a link error — it builds,
installs, and then segfaults the instant a tool starts. That failure reaches
`run-tests.sh macos` as a wall of "native Darwin DVM is unstable" skips,
which is why the build now prints the libraries `prted` actually resolved
against; check those first when the macOS suite goes quiet. (Most of the
"Darwin instability" this suite used to report on at least one host was
exactly this — a build against a stale second PMIx.)

Rebuild after editing PRRTE: just rerun `./build.sh` (incremental). No image
rebuild, no `docker compose` restart needed — the nodes read the shared volume.
To also test an openpmix change, add `PMIX_SRC=/path/to/openpmix`.

## 5. Driving the DVM by hand

`run-tests.sh` automates all of this, but to poke at it yourself:

```sh
RUN='docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 prte-node1 bash -lc'

# start the DVM on node1 -- prte_elastic_mode is REQUIRED for grow/shrink
$RUN '. /opt/prte/env.sh; nohup prte --daemonize --prtemca prte_elastic_mode 1 \
        >/tmp/prte.out 2>&1 & sleep 6'

$RUN '. /opt/prte/env.sh; prun --np 1 hostname'        # baseline
$RUN '. /opt/prte/env.sh; elastic grow node2:2,node3:2'
$RUN '. /opt/prte/env.sh; elastic shrink node3'
$RUN '. /opt/prte/env.sh; pterm'
```

`. /opt/prte/env.sh` puts the shared-volume install on `PATH`/`LD_LIBRARY_PATH`
in a non-login `docker exec` shell (login shells get it automatically).

Both `grow` and `shrink` should print
`PHASE 2 (completion): received event PMIX_DVM_IS_READY` followed by `SUCCESS`.

> **The flag that bites you.** PRRTE gates *all* of the grow/shrink launch-fence
> and completion-event machinery behind `prte_elastic_mode` (default off).
> Without `--prtemca prte_elastic_mode 1`, a grow returns phase-1 SUCCESS and
> even launches daemons, but **never completes** — the client times out after
> 60s. Always start with the flag.

> **Capturing HNP verbose output.** `prte --daemonize` detaches from stdio, so
> a `>/tmp/prte.out` redirect captures nothing. To trace the HNP, run it in the
> foreground under `docker exec -d`:
>
> ```sh
> docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
>   prte-node1 bash -lc '. /opt/prte/env.sh; cd /root && prte --prtemca prte_elastic_mode 1 \
>     --prtemca plm_base_verbose 5 --prtemca ras_base_verbose 5 >/tmp/prte.out 2>&1'
> ```

### Leak checking under Linux

The image carries `valgrind`, so the Linux swarm is the place to look for
leaks on the paths a single host cannot reach — anything involving real
daemons, xcast, or the RML wire. Run the HNP under it:

```sh
docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
  prte-node1 bash -lc '. /opt/prte/env.sh; cd /tmp && \
    valgrind --leak-check=full --show-leak-kinds=definite,indirect \
             --num-callers=25 --log-file=/tmp/vg-hnp.txt \
      prterun --host node1:1,node2:1,node3:1,node4:1 -np 4 --map-by node hostname'
docker cp prte-node1:/tmp/vg-hnp.txt .
```

A daemon can be traced the same way by pointing `prte_launch_agent` at a
wrapper script that execs `valgrind prted`. Note this is deliberately **not**
part of `run-tests.sh`: a valgrind run is many times slower than the suite it
would be embedded in. Compare totals before and after a change rather than
chasing an absolute number — PMIx and hwloc contribute their own.

## 6. What "success" looks like

**`prterun`** (`prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by
node hostname`): stands up a transient DVM, runs the job, and exits cleanly with
no daemons left behind — the non-elastic launch path.

**Preload (`filem`)** (`--preload-binary`): the harness compiles a marker binary
on **node1 only**, then runs it under `--preload-binary` on node2+node3. Because
the executable does not exist on the target nodes, a successful run proves
`filem` actually staged the bytes across daemons (xcast → `recv_files` →
`write_handler`) and linked them into the job session dir that
`--preload-binary`'s session-cwd points at. This is the one path a single-host
build can't validate — locally the source file is already present, so the app
would run even if staging did nothing. (Data-file preload, `--preload-files`,
is **not** asserted here: staged data files land in the per-proc session dir but
the default cwd is elsewhere, so they are not reachable by a bare relative path —
a separate, pre-existing gap.)

**Remote stdin (`iof`)**: a large base64 payload is piped into `prterun` on
node1 for a job whose rank 0 is mapped onto **node2**, running `cat`. Because
the reading proc is not on the head node, every byte must cross
HNP → `PRTE_RML_TAG_IOF_PROXY` → `prted` → the proc's stdin pipe and come back
as forwarded output; an md5 match proves nothing was dropped, truncated, or
reordered. This is the other path a single-host build cannot validate —
locally, `push_stdin` writes straight into the proc's sink and the wire format
is never exercised. The payload is deliberately far larger than the 4096-byte
read fragment and the 8192-byte write chunk. A companion case pipes a short
line with `--stdin all` to check the wildcard/xcast delivery.

**Grow** (`elastic grow node2:2,node3:2`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, and `prted` now running on node2 and node3.

> The grown nodes join the **reservation** created by the request, so a plain
> `prun -n 3 --map-by node hostname` still lands only on node1 — its default job
> allocation is node1's base pool, not the reservation. Confirm a grow by
> `prted` presence on the targets and the `PMIX_DVM_IS_READY` event, not by
> plain-`prun` placement.
>
> To actually **run something on a grown node**, spawn into the reservation:
> `elastic grow node4:2 -- <cmd>` takes the `PMIX_ALLOC_ID` the request handed
> back and spawns `<cmd>` with `PMIX_SPAWN_TARGET` naming it. The grow and the
> spawn must happen in one `elastic` invocation — the HNP only lets a namespace
> target a reservation it owns, and the owner is the tool that created it.

**Shrink** (`elastic shrink node3`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, plus a **"PRRTE has lost communication with a remote
daemon"** notice naming the shrunk node — **that notice is expected** (shrink
completion is driven by the targeted daemon's death). The HNP must survive it.

**Relay** (radix-2 deep grow, `run-tests.sh` does this): grown across node2–node9
with `--prtemca prte_rml_radix 2`, the daemon tree is 3–4 deep, so the
`PMIX_DVM_IS_READY` completion fence must relay through intermediate daemons. If
routing/relay is broken the fence hangs to the 60s timeout instead of
completing.

### The build dirs persist, so configure arguments are sticky

The VPATH build dirs live in the shared volume and outlive any one run.
`build.sh` therefore used to configure only when there was no
`config.status`, which quietly reused the arguments of whichever run
created the directory. Setting `PMIX_SRC` after a plain build built your
PMIx *and then linked PRRTE against the baked one anyway*, with nothing in
the output to say so — a build that looks like it tested your change and
did not.

`build.sh` now stamps the configure arguments in `.configure-args` beside
`config.status` and reconfigures when they differ; it says
`(re)configuring PRRTE: …` when it does. If you are chasing something that
depends on a configure-time decision, check that line rather than assuming.
This is the same shape as the `show_help` staleness trap below: a
persistent build dir plus a "only if missing" rule.

### Writing a case that asserts on an error message

**`show_help` emits a given message once per HNP.** A test that probes with
some command and then re-runs it to assert on the output gets nothing the
second time — the first, discarded run already spent the message. Capture
the output of the run that produced the condition and assert on that. (The
`rmaps` "map by an object the node does not have" case is written this way,
and was written the other way first.)

The corollary for a **persistent** DVM: the diagnostic is produced on the
HNP, and `prte --daemonize` has detached from stdio, so `>/tmp/prte.out`
captures nothing. PRRTE relays the message back to the submitting tool, so
assert on `prun`'s own output; if you need the HNP's stdio, start it in the
foreground under `docker exec -d` (see §5).

---

## 7. Cleanup hygiene

`run-tests.sh` cleans every node before its first case and between phases.
That first sweep matters more than it looks: the nodes are long-lived
containers while the install they read is replaced under them, so a daemon
or a `pmix.*` rendezvous file left from a previous run may be holding a
library that no longer exists. Rebuilding with a different `PMIX_SRC` and
running straight away produced twenty-one failures with nothing in them
pointing at the cause — the first launch simply came back killed.

If you drive things by hand, clear stale state on **every** node between
DVM runs:

```sh
for n in $(seq 1 10); do
  docker exec prte-node$n sh -c 'pkill -9 -x prted; pkill -9 -x prte;
    rm -rf /tmp/prte.* /tmp/prun.session.*; true'
done
```

A detached `prted --daemonize` **survives an HNP kill**; orphans on other nodes
make the next DVM trip over stale rendezvous files ("multiple possible
servers"). `pterm` is the clean shutdown; still run the loop afterward to be
safe.

## 8. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PRRTE source edit | `./build.sh` (incremental into the volume) |
| pick up an openpmix edit | `PMIX_SRC=/path/to/openpmix ./build.sh` |
| force a clean PRRTE rebuild | `docker volume rm prte-build && ./build.sh` |
| rebuild the base image (new baked PMIx) | `./build.sh image` (or `PMIX_REF=v6.1.0 ./build.sh image`) |
| tear down the swarm | `docker compose down` (the `prte-build` volume persists) |

## 9. Grow after a shrink

Historically this was the harness's standing known issue: re-growing a node
immediately after shrinking it was reported to fail its TCP connect-back
(`prted` exits 255) — [openpmix/prrte#2491](https://github.com/openpmix/prrte/issues/2491) —
and the advice was to start a fresh DVM between grow/shrink cycles.

What was actually reproducible here was a **hang**, and it applied to growing
*any* node after a shrink, not just re-growing the same one: `grow
node2,node3` → `shrink node2` → `grow node4` left a healthy `prted` on node4,
but no phase-two completion event ever arrived, so the client timed out after
60s and the DVM was wedged (a later `prun` never returned either).

The cause was in the grow campaign, not in the launch: a grow starts a daemon
on **every** node that lacks one, and a shrunk node reverts to the default
pool with its `->session` cleared, so the re-absorbed node takes the lowest
new vpid and lands in the `daemon_vpid_start` slot. The campaign read its
requester from only that first target, found no session, recorded no
requester — and so emitted no completion event even though the grow had
succeeded. `prte_plm_base_setup_virtual_machine` now scans all of a
campaign's targets for the first one carrying a requestor.

Chasing that turned up the related defect that a grow was **not scoped to
the nodes it was given**: the extend path built its candidate list from the
whole node pool, so any node lacking a daemon joined — which after a shrink
meant `grow node4` also relaunched a daemon on the node you had just shrunk
away. A grow now selects only nodes its own request marked
`PRTE_NODE_STATE_ADDED`, and RAS carries that mark onto a pool entry that
already exists (re-growing a shrunk node relies on it).

`run-tests.sh` covers re-growing the same node, growing a different one
afterwards, and that a grow leaves every node it was not given alone.

Still bound elastic commands with `timeout` in any new test: a grow that does
not complete otherwise wedges the whole suite rather than failing one case.

## 10. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `prte-node1` | node1 | head node (HNP) — start `prte` here, run all tools here |
| `prte-node2`..`prte-node10` | node2..node10 | DVM nodes (grow/shrink targets) |

Network: bridge `dvm`. All nodes mount the shared `prte-build` volume read-only
at `/opt/prte`, where `build.sh` installs PRRTE (`/opt/prte/prte`) and writes
`/opt/prte/env.sh`. To add or remove nodes, copy or delete a service block in
`docker-compose.yml` (and adjust the `seq 1 10` loops to match).

## 11. Faking a SLURM environment (`ras/slurm`)

`ras/slurm` is the only ras component with a full elastic **modify** surface,
and everything past initial discovery is a shell-out: `sbatch` to grow,
`scontrol show job <id> --json` to learn what SLURM granted, `scontrol update
job <id> ReqNodeList=…` to shrink one in place, `scancel` to give one back.
None of that runs on a developer machine, so it had no automated coverage at
all — including the ~1000-line JSON parser, which this harness is also the
**only** automated build that even compiles (jansson defaults to off; see
`--with-jansson` in `build.sh`).

[`fake-slurm.py`](fake-slurm.py) supplies the missing scheduler. `build.sh`
installs it into the shared volume as
`/opt/prte/fakeslurm/bin/{sbatch,scontrol,scancel}` (it dispatches on
`argv[0]`), deliberately **not** into the install `bin/` that the node
entrypoint puts on every node's default PATH — a test has to opt in by
prepending that directory, so this cannot perturb anything else in the suite.

It is not a SLURM emulator. It implements exactly the four command forms
PRRTE issues, keeps its state under `/tmp/fake-slurm`, and hands out **real
container hostnames** — so a grow driven through it launches real daemons on
real nodes and a release really removes them.

Driving it by hand:

```sh
RUN='docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 prte-node1 bash -lc'

$RUN 'export PATH=/opt/prte/fakeslurm/bin:$PATH
      fake-slurm init --jobid 1000 --base node1 --tasks 2 --pool node2,node3,node4
      eval "$(fake-slurm env)"     # SLURM_JOBID, SLURM_NODELIST, SLURMD_NODENAME, ...
      . /opt/prte/env.sh
      nohup prte --prtemca prte_elastic_mode 1 --prtemca ras_base_verbose 5 \
            >/tmp/prte.out 2>&1 &
      sleep 8
      elastic extend 2             # PMIX_ALLOC_EXTEND: sbatch, poll, absorb
      fake-slurm audit             # every command PRRTE issued
      fake-slurm args 2001         # the sbatch argv it built
      elastic shrink node3         # partial: scontrol update ReqNodeList=
      elastic release-id 2001'     # whole job: scancel
```

Two things to know:

- **The request shapes differ from a plain grow.** `elastic grow <nodes>` is
  `PMIX_ALLOC_NEW` naming hosts, which the *base* serves. `ras/slurm`'s
  `modify()` accepts only `PMIX_ALLOC_EXTEND`+`NUM_NODES`,
  `PMIX_ALLOC_RELEASE`+(`NODE_LIST`|`NUM_NODES`|`ALLOC_ID`), and
  `PMIX_ALLOC_REQ_CANCEL` — which nodes an extend lands on is the
  scheduler's choice. Hence `elastic extend|release|release-id|cancel`.
- **An extend emits no phase-two event.** Its nodes join the general pool
  (the component deliberately leaves `node->session` NULL), and the directed
  completion event is addressed to the requestor recorded on a *reservation*.
  Phase one carries the real result, which is why `elastic extend` does not
  wait for phase two. A **release** does go through a shrink campaign and
  does emit `PMIX_DVM_IS_READY`.

Fault injection, for the paths that only exist to handle a scheduler
misbehaving (`fake-slurm set <key> <value>`):

| key | effect |
|-----|--------|
| `pending_secs` | a new job sits in `PENDING` this long — lets a request be cancelled mid-poll |
| `scancel_fail` | `scancel` exits non-zero with far more output than the 256-byte capture buffer holds |
| `bad_json` | `scontrol show job --json` returns unparsable output with exit status 0 |

The suite uses all three: a cancelled pending extend, a failing `scancel`
(whose message must come back truncated and `...`-terminated, and must not
take the HNP down), and malformed JSON. It also asserts a release naming a
hostname with a shell metacharacter is refused before it can reach a command
line.

**Some slot counts are asserted from `ras_base_verbose` output, not from
the pool.** The verbose line is what the component itself computed, so it
separates a parser error from whatever the launch path later does with the
number. The pool is asserted too, separately — a node whose count came
from the scheduler must still carry that count at mapping time
(`PRTE_NODE_FLAG_SLOTS_GIVEN`).
