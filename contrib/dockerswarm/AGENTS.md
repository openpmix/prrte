# AGENTS.md — the dockerswarm multi-node test harness

Orientation for AI agents and human contributors who need to run PRRTE
across multiple nodes without a cluster. **This directory is the
canonical multi-node harness** — if you are about to hand-craft
containers, tar a source tree into a volume, or patch files inside a
running node, stop: `build.sh` below already does the right thing
(bind-mounts your live working tree into a builder and compiles it
out-of-tree into the shared volume the nodes read).

> **The one exception: SLURM.** There is none here — no scheduler, real or
> stood-in. All of it lives in [`contrib/slurmswarm`](../slurmswarm/), the
> same harness with an actual SLURM installation in the ten containers (§12).
> This harness used to carry a fake control plane (`fake-slurm.py`), which
> could show that PRRTE issued the right commands and could never show that
> SLURM accepts them; once the real thing existed, keeping the fake meant two
> suites asserting the same behavior with the weaker one able to disagree.
> Keep the division: anything with a `SLURM_` in it belongs *there*, and
> everything else belongs here, because this harness is faster and needs no
> scheduler.

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
| `docker-compose.yml` | The ten nodes `prte-node1`..`prte-node10`, each mounting the shared `prte-build` volume. Every one of those names derives from `$PRTE_SWARM`, so two clones can each run a swarm — see §4. |
| `elastic.c` | The elastic test client (`elastic` in the install): issues a PMIx allocation request and waits for the phase-two completion event. |
| *(no file here)* | `build.sh` also compiles [`examples/dynamic.c`](../../examples/dynamic.c) from the main tree as `dynamic` — the only client in this harness that calls `PMIx_Spawn`, and so the only way to get a **parent/child job pair**. See §11. |
| `dataserver.c` | A bare PMIx client for the publish/lookup service (`dataserver` in the install): publish/lookup/lookupwait/lookup2/unpublish. Drives `src/runtime/data_server`. See §13. |
| `jobinfo.c` | A bare PMIx client for the **direct-modex** paths (`jobinfo` in the install): `publish`/`fetch`/`fetchkey`. Drives `src/prted/pmix/pmix_server_fence.c` from a daemon that hosts none of the target job's procs. |
| `proctable.c` | A bare PMIx client for the **proc-table and server-URI queries** (`proctable` in the install): `procs`/`localprocs`/`serveruri`. Those are the only callers of `prte_pmix_convert_state()`, and the local-vs-global proc-table split has no meaning on one host. Drives `src/pmix`. |
| `peerinfo.c` | A bare PMIx client in which every rank asks **every other rank of its own job** where it is (`peerinfo` in the install): rank, app rank, local/node rank, node id, hostname, cpuset, locality string. The only client here that reads a *peer's* reserved keys, and so the only one that reaches the daemon's derive-on-demand path — everything else either reads back what it put itself or asks about the job. Drives `derive_proc_data()` in `src/prted/pmix/pmix_server_fence.c`. See §20. |
| `groupcon.c` | A bare PMIx client that drives a **group construct/destruct** (`groupcon` in the install): every rank contributes a local cid, asks for a context id, constructs, reads every peer's contribution back, destructs. Drives grpcomm's `grp_release` on daemons that merely *received* the broadcast. See §15. |
| `groupinv.c` | A bare PMIx client that forms a group by **invitation** and asks for a context id (`groupinv` in the install). The highest rank leads, so mapped by node the leader is never the HNP - which is the point: that method runs no collective, so its leader asks for the id through job control and only the HNP holds the pool, so the request has to be relayed. Drives `PRTE_PMIX_GROUP_CTXID` in `src/prted/pmix`. See §15. |
| `envspawn.c` | A PMIx client that spawns a child job carrying one of every **envar directive** (`envspawn` in the install): SET/ADD/UNSET/PREPEND/APPEND, pinned to a named host, with the child reporting the environment it actually got into a file on its own node. Those directives have no command-line surface — they arrive only on a spawn request — and `odls` applies them on whichever daemon forks the process, so the child has to land somewhere the parent is not. Drives `prte_odls_base_process_envars`. |
| `slowcat.c` | A deliberately **slow** stdin reader (`slowcat` in the install, no PMIx dependency): copies stdin to a file in small reads with a pause between them, so the daemon feeding it keeps hitting *partial* writes. That is the only way to reach the iof short-write path. |
| `../scaling/scaletest.c` | A PMIx client that **times** a full-data fence and a bare barrier over the whole job (`scaletest` in the install). Not a pass/fail case — a measurement. See §18. It lives in [`contrib/scaling/`](../scaling/) because it is not container-specific: the same client is driven across a real allocation by `contrib/scaling/cluster-sweep.sh`, which is where the questions this harness *cannot* answer get measured. |
| `mpinoop.c` | `MPI_Init` and `MPI_Finalize` and nothing else, so the only thing timed is the modex fence and the barrier behind it. Built only when `OMPI_SRC` is set — see §19. |
| `scaletest.sh` | The measurement driver: stands up a swarm of arbitrary size (default 40), sweeps DVM size × procs-per-node × routing radix × payload, and writes a CSV. See §18. |
| `pmixloop.c` | The client-churn probe (`pmixloop` in the install): cycles `PMIx_Init` / collecting fence / bare fence / `PMIx_Finalize` with a per-rank skew, so one rank opens its next cycle while its peers are still finalizing the last. The reproducer from openpmix#4113 and #4112, and the only client here that connects to its server more than once. See §21. |

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
so. `--distclean` forces it. `--no-distclean` skips the *detection* — it
is an assertion that the tree really is clean, and if it is not,
`build.sh` **stops with an error** rather than building the tree it has
just been told to poison. That is deliberate: it used to warn and carry
on, but nobody reads a warning scrolled off the top of a build whose test
results come out looking fine. The failure mode it was hiding is the
quiet one — same platform, different `--with-pmix`, a suite that passes
against a library nobody chose.

Note what this means for who decides. It is tempting to move the call to
the caller entirely ("the person running it knows whether a distclean is
needed"), and that is wrong: the trigger is *"does the source tree hold
an in-tree build right now"*, which is a property of the tree, not of
your intent. Another worktree, another agent, or your own previous
session may have put one there. `build.sh` can look; you can only guess,
and the two ways of being wrong cost wildly different amounts.

**Do not try to narrow this to "only when configure has to run."** It is
the obvious optimization — an incremental out-of-tree build already has
every object of its own, so VPATH is never searched for one — and it has
been tried and measured. It fails on `prte_config.h`:
`src/include/constants.h` does `#include "prte_config.h"`, and *both files
live in `src/include`*, so the compiler's "search the directory of the
including file first" rule for quoted includes picks up the **source**
tree's stale copy before any `-I` or `-iquote` path. Nothing in `CPPFLAGS`
can override that. (`configure.ac` does now put the build tree ahead of the
source tree, which is correct and fixes every file that includes
`prte_config.h` directly — it just cannot reach this case.) The symptom is
an `"OAC_HAVE_APPLE" redefined` error when the two trees were configured
for different platforms; the quiet and far worse version is same-platform
with a different `--with-pmix`.

The only way to stop paying for the distclean is to stop keeping an
in-tree build (below). Removing the destructive step entirely would mean
building from a *snapshot* of the source inside the volume rather than a
bind mount of the live tree — a real change to how this harness works, not
a smarter trigger.

**The cost, and how to avoid paying it repeatedly.** A distclean destroys
*your* in-tree build, so the next `make check`, `make -C test/offline
check-offline`, or `make install` has to reconfigure and rebuild the
whole tree first. That failure is easy to misread, because a distcleaned
tree does not announce itself — `make check` just says *"No rule to make
target `check'"* and `make` at the root exits 0 having done nothing.

### Build both sides out of tree — that is the intended workflow

**Do not keep an in-tree build.** Build the host side out of tree too and
the whole problem above stops existing:

```sh
./build.sh macos                    # -> <repo>/vpath-macos, install in vpath-macos/install
make -C vpath-macos check           # unit tests
make -C vpath-macos/test/offline check-offline
./build.sh                          # -> /opt/prte/vpath-linux in the volume
```

Then the source tree is never configured at all: `srcdir_has_intree` is
never true, the distclean never fires, nothing you built gets destroyed,
and the tree is always ready to hand to the swarm without a reconfigure
first. Both builds coexist from the same pristine sources, which is
exactly what this harness was built to do.

The habit worth forming is: **the source tree is sources.** If you find
yourself running `./configure` or `make` at the repo root, you are
setting up the next distclean and the next twenty-minute rebuild. That is
now stated project-wide — see the top-level
[`AGENTS.md`](../../AGENTS.md), "GOLDEN RULE: build out of tree" — and it
sits alongside, not against, the golden rule about running `make` from
the top of your build tree: that one is about not hand-compiling single
files or building from deep inside a subdirectory, and VPATH only changes
*which* directory the top is.

The trap to know about if you *do* end up with an in-tree build that gets
cleaned: a distcleaned tree does not announce itself. `make check` says
*"No rule to make target `check'"* and a root-level `make` exits 0 having
done nothing.

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
./build.sh                 # autogen (+ distclean if the source tree holds an
                           #   in-tree build -- see §2), build image, build
                           #   PRRTE into the shared volume from your live tree
docker compose up -d       # start prte-node1 .. prte-node10
./run-tests.sh linux       # full suite: prterun, elastic grow/shrink, relay

# ---- native macOS (single host) ----
./build.sh macos           # native VPATH build into <repo>/vpath-macos
./run-tests.sh macos       # build + single-host launch smoke

# ---- and run the host-side suites from that build, not the source tree ----
make -C ../../vpath-macos check
make -C ../../vpath-macos/test/offline check-offline
```

Keeping the host side out of tree as well is the intended workflow, not
an optimization for heavy users — see §2, "Build both sides out of tree".

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

### Two clones on one host: `PRTE_SWARM`

Every global name this harness claims — the compose project, the ten container
names, the build volume, the docker network — is derived from `$PRTE_SWARM`,
so a second clone (or a second agent) can drive its own swarm:

```sh
export PRTE_SWARM=alt      # for the WHOLE shell; see the warning below
./build.sh                 # -> volume alt-build
docker compose up -d       # -> project altswarm, containers alt-node1..10
./run-tests.sh linux       #    on network altswarm_dvm
```

Unset, it is `prte` and every name is exactly what it has always been:
project `prteswarm`, containers `prte-node1..10`, volume `prte-build`.

Both swarms contain a container whose **hostname is `node1`**, and that is
fine: each user-defined bridge network runs its own embedded DNS serving only
the containers attached to it, so `node2` inside a swarm can only ever mean
that swarm's node2. The tests need no changes — `--host node2:2` means the
right machine in either.

> **Export it, don't prefix one command.** `docker compose` interpolates
> `docker-compose.yml` itself, so `PRTE_SWARM` has to be in *that* command's
> environment. A `docker compose up -d` without it quietly brings up the
> **default** swarm instead, against the default volume, and your build sits
> in `alt-build` unused. `run-tests.sh` says which swarm it looked for when it
> finds nothing, and `build.sh` prints the exact next command including the
> variable.

**What is still shared, and what that costs.** The base image
(`prte-swarm:latest`) is read-only to a running swarm and expensive to build,
so both instances use the same one — but `./build.sh image` (or a
`docker build`) replaces it under the other swarm's feet, and that swarm's
containers then fail the "containers are running the current image" preflight
until they are recreated. Nothing else crosses: separate containers, separate
`/tmp`, separate install, separate network.

**The macOS subset needs no such knob** — its install is already per-clone
(`<repo>/vpath-macos`), and it isolates the two things that are not: it starts
every tool by absolute path out of its own install and matches on that path
when it reaps strays (a bare `pkill -x prte` would kill the other clone's DVM,
and a bare `pgrep -x prte` would *report the other clone's DVM as its own* and
pass a case on it), and it runs PRRTE under a private `TMPDIR` it creates and
removes, so no session dir it deletes was ever anyone else's.

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
would be embedded in.

**For `definitely lost` the number to expect is zero**, and that command above
is the one to check it with. Read `still reachable` differently — PMIx, hwloc
and libevent all keep live state at exit, so compare totals before and after
rather than chasing an absolute there.

Two things this found that are easy to reintroduce:

- A record with **direct bytes but zero indirect bytes** means a container was
  dropped after its payload was moved out — someone unloaded a
  `pmix_data_buffer_t` and never released the shell. That was every
  inter-daemon reliable message (`prte_relm_start_msg`).
- The leak you are looking for may only be on an **error** path, so run a
  command line that *fails*, not just one that works. `prterun -n 1 --display
  map --display cpus hostname` is refused for the repeated option, and that
  return leaked the parser's argv until it was fixed.

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
would run even if staging did nothing.

**Data-file preload** (`--preload-files`) is asserted too, and used not to be:
staged data files landed in the per-proc session dir while the job's cwd was
elsewhere, so nothing could open one by a bare relative name
([#2525](https://github.com/openpmix/prrte/issues/2525)). They now go to
`app->cwd`, and the cases cover a plain file read by both remote ranks, a
relative subdirectory being recreated, an executable staying executable, an
archive whose name contains a space, a collision with a *different* file being
refused rather than overwriting the user's copy, a `..` in the delivered name
being refused, and a file reaching a node grown into the DVM afterwards.

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

**Slow-reader stdin (`iof`)**: the same payload again, but read by `slowcat`
instead of `cat` — and this is the case that finds real bugs, because volume
alone does not reach the interesting code. `cat` drains its stdin pipe as
fast as the daemon can fill it, so the daemon's non-blocking `write(2)`
essentially always completes in full and the **short-write** path is never
taken. `slowcat` keeps the pipe saturated, so once the input passes the pipe
capacity (64 KB on Linux) nearly every write is partial. That is the state
in which [#2579](https://github.com/openpmix/prrte/issues/2579) lived for
years behind a passing 256 KB `cat` test: re-basing a queued chunk without
also decrementing its count re-sent the tail of the chunk on every retry, so
the application received its input **duplicated** — measured here as 354 KB
in, 37 MB out, and then a hang, since the backlog could never drain. The
assertion is therefore on the byte count first (`SLOWCAT-BYTES` must equal
what was piped in) and the md5 second. It runs twice, once with the rank on
node2 and once on node1, because the HNP and the daemon carry **separate**
copies of the write handler — the HNP copy was fixed upstream in 2025 and
the daemon copy was missed.

A third pass runs `slowcat` slower still (64 bytes every 4 ms), which is
what pushes the daemon's stdin backlog past `PRTE_IOF_MAX_INPUT_BUFFERS`
(50 chunks) and makes it send the HNP an **XON/XOFF** message. That message
is nothing but the stream tag, on the same RML tag forwarded output uses,
and the HNP used to unpack the tag and then reach straight for a proc — so
an ordinary slow reader put a PMIx unpack error on the user's terminal. The
case asserts delivery is still exact *and* that the HNP's log gained no
unpack error. Do not weaken the reader's pace: at `cat` speed, or even at
the pace the two cases above use, the backlog may never cross 50 and the
case passes without having tested anything.

A fourth pass runs the same reader with `--prtemca iof_base_verbose 1` and
**counts** the two halves of the flow control in the HNP's log. That is
the case that covers `PMIx_server_IOF_flow_control` actually being
reached, and it is a count rather than a grep on purpose: PMIx has no
status meaning "resume", so an implementation that asserts XOFF fifty
times and releases once looks identical to a correct one under a presence
test — and the job it produces is *hung*, not slow. The case fails if the
counts differ, if flow control never engaged at all (which would mean the
reader was too fast and the case proved nothing), or if a byte went
missing across the suspensions. See the framework guide's *Flow control*
section for where each half lives.

**Grow** (`elastic grow node2:2,node3:2`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, and `prted` now running on node2 and node3.

> The grown nodes join the **reservation** created by the request, and for as
> long as the requesting tool is alive they belong to it alone: a plain
> `prun -n 3 --map-by node hostname` still lands only on node1, because its
> default job allocation is node1's base pool, not the reservation. While the
> tool is running, confirm a grow by `prted` presence on the targets and the
> `PMIX_DVM_IS_READY` event, not by plain-`prun` placement.
>
> To **run something on a grown node while the reservation is still held**,
> spawn into it: `elastic grow node4:2 -- <cmd>` takes the `PMIX_ALLOC_ID` the
> request handed back and spawns `<cmd>` with `PMIX_SPAWN_TARGET` naming it.
> A **later** command of the same user can reach it too:
> `prun --alloc-id <id>` spawns into the reservation from a separate tool.
> Ownership is namespace *and* user — the namespace test is what keeps other
> jobs in the DVM out, and the uid is what keeps the allocation usable after
> the one-shot tool that created it has gone. An allocation the DVM does not
> know is still `PMIX_ERR_NOT_FOUND`, and a job whose namespace is not an
> owner is refused with `PMIX_ERR_NO_PERMISSIONS` (`PRTE_ERR_PERM`) — the two
> answers are deliberately distinct, so "you may not" cannot be read as
> "there is no such allocation".
>
> **Once that tool exits, the reservation is gone.** Its inheritance
> disposition fires — by default "unreserve into the general pool" — so the
> nodes it grew become ordinary pool members and a plain `prun --host node4`
> reaches them with no allocation directive at all. This needs a PMIx defining
> `PMIX_CAP_TOOL_FINALIZED`: a tool that finalizes cleanly is reported to the
> host by nothing else (it is not a child, so no waitpid; and the connection
> drop that follows raises no lost-connection event, because the peer is
> already marked finalized). Against an older PMIx the DVM never learns the
> tool went away, the disposition never runs, and the grown nodes stay
> stranded for the life of the DVM — outside the general pool and unreachable
> through the reservation too, since the only namespace allowed to name it no
> longer exists. `run-tests.sh` skips the cases that assert the released-pool
> behavior when the capability is absent (`pmix_cap`).

**Shrink** (`elastic shrink node3`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, plus a **"PRRTE has lost communication with a remote
daemon"** notice naming the shrunk node — **that notice is expected** (shrink
completion is driven by the targeted daemon's death). The HNP must survive it.

**Relay** (radix-2 deep grow, `run-tests.sh` does this): grown across node2–node9
with `--prtemca prte_rml_radix 2`, the daemon tree is 3–4 deep, so the
`PMIX_DVM_IS_READY` completion fence must relay through intermediate daemons. If
routing/relay is broken the fence hangs to the 60s timeout instead of
completing.

**Personalities and CLI translation (`schizo`, `test_schizo`)**: schizo is
a front-end framework, so most of it is covered by `test/unit/schizo`
with no DVM at all. Three things are only observable across daemons, and
those are what this phase asserts:

- the **envar directives** (`-x`, `--set/prepend/append/unset-env`) are
  applied by `odls`' `process_envars()` on the `prted` that forks the
  process, not by the tool — so the probe runs on node2/node3 and prints
  its own environment. They must take effect **in the order the user gave
  them** (`--prepend-env FOO[:] x --set-env FOO=1` leaves `FOO=1`; the
  reverse leaves `FOO=x:1`), and each exactly once — they used to be
  applied twice, which duplicated every entry prepended onto `PATH`.
- **`--output file=…`** is written per-daemon, and its `:`-delimited
  qualifiers (`raw`, `copy`/`nocopy`) decide whether the output is *also*
  copied back over the wire. Both qualifier orders must behave the same;
  they did not when the qualifier run was split on the wrong delimiter.
  The `pattern` qualifier is checked here too: its `%` conversions may
  appear in the **directory** part of the name (`%h/rank-%R`), so each
  daemon creates the directory its own expansion names — which is exactly
  what one host cannot show.
- a job's **personality is resolved again on every daemon** (`odls` calls
  `detect_proxy` with the job's personality list), so a personality
  nobody claims must be refused with a diagnostic — and, on a persistent
  DVM, refused without taking the HNP down.

**Topology handling (`src/hwloc`, `test_hwloc`)**: `src/hwloc` is a library
of pure functions over a topology, so `test/unit/hwloc` covers nearly all of
it against synthetic topologies. What it cannot reach is the case PRRTE
actually runs in — the topology being queried or rendered arrived from
*another* machine as XML and sits in the HNP alongside nine others. This
phase asserts:

- a **remote node's binding is rendered from its own topology**. The map is
  built by the HNP from what each daemon shipped it, so `--display map` (not
  `--display bind`, whose per-rank line comes from the daemon's own stderr
  and is not forwarded here) is what exercises the path.
- a **package-wide binding does not overrun the element buffer**. These
  containers are 8 cores, one package, **no SMT, and no NUMA node in sysfs**
  — which is exactly the shape that trips it: `--display map:parseable`
  writes one `<core>N</core>` element per bound core into a buffer its caller
  sizes at 20 bytes per PU, while each element needs ~34. The corruption
  lands in the HNP, which is holding every node's topology, so the symptom is
  a crash somewhere unrelated. Do not "simplify" this case onto one node.
- **`--map-by numa` against topologies the HNP never sensed**. The NUMA count
  comes from a cutoff cached on each topology's root object; a topology that
  arrived from a daemon only has that cache if someone built it, and the
  answer when it is missing used to be a silent zero.
- a **DVM cpu-set constrains every node, not just the first**. The expansion
  runs against each node's topology in turn and rewrites the process-wide
  list as it goes, so one node cannot show whether the second got the same
  answer.
- a **malformed cpu-set is refused without taking the HNP down**. An
  unresolvable entry came back as a NULL cpuset that nothing checked, so the
  diagnostic was followed by a segfault inside hwloc.
- **`--display topo` over several topologies at once** — the only place that
  traversal runs against more than one.

Two harness notes. The cpu-set case is written as a **range** (`0-1`) on
purpose: PMIx's command-line parser used to reject any MCA value whose second
character was a dash as `not-enough-arguments`, so that spelling was
unusable, and the case is now the end-to-end regression test for the fix as
much as for the per-node expansion. If it is ever run against a PMIx that
predates the fix it **skips** with a message saying so rather than failing —
the harness bakes PMIx into the image, so an old image is a real possibility.
Build with `PMIX_SRC=<checkout>` to test against a local PMIx.

And the cpu-set case checks **both** binding levels — to a core and to a
package — because a cpu-set used to be honored only at the core level, where
the object sits inside the set anyway; a rank bound to a package came back
owning every core of it. Note that an assertion on a binding has to match the
**whole** bracketed site list: a rank bound to `core:L0-7` starts with a `0`
and slips past a pattern that only looks at the first number, which is
exactly how that defect stayed hidden behind a passing test.

**The PRRTE/PMIx translation shim (`src/pmix`, `test_pmix`)**: everything in
`src/pmix/pmix.c` is a pure integer mapping and is covered exhaustively,
without a DVM, by `test/unit/pmix`. What this phase adds is the one thing a
table test cannot show — that the mapping is actually *reached*, with real
proc states, on a daemon that is not the one you are standing on. The probe
is `proctable`, and it asserts:

- `PMIX_QUERY_PROC_TABLE` over a job spread across four nodes returns every
  proc, the table spans more than one node, and **no proc reports
  `PMIX_PROC_STATE_UNDEF`**. That last one is the whole point:
  `prte_pmix_convert_state()` was written with bare integer cases against a
  state space that does not number like PMIx's, so several real PRRTE states
  fell through to `UNDEF` — a legal answer, so nothing anywhere logged an
  error and the proc simply had no state.
- `PMIX_QUERY_LOCAL_PROC_TABLE` returns *some but not all* of a job's procs.
  On one host "local" and "all" are the same set, so this case cannot exist
  there.
- **every daemon serves every node's `PMIX_SERVER_URI`**. The consumer here
  is a **tool**, not a daemon — daemons reach each other over the RML and
  never form PMIx connections to one another; see `examples/tool.c --uri
  <nodename>`. Each daemon ships its own server URI to the master in its
  `PRTED_CALLBACK` rollup, and the master puts the whole set into the
  `WIREUP` xcast alongside the nidmap, so the answer does not depend on
  which daemon the tool attached to. (Before that the query asked for a key
  nobody published and answered `NOT_FOUND` for every node but the one being
  asked — the producing half of commit `6e481fbb95` was never written.)

  The case asks a **non-master** daemon about three other nodes — the case
  that needs the xcast rather than just the rollup — and requires three
  **distinct** URIs, so an implementation that echoed the local server back
  would not pass. It then checks the master and a non-master agree on a
  third node's URI. An unknown node must come back with a specific status,
  never the generic `PMIX_ERROR` that the wrong-direction conversion used to
  manufacture out of every failure on this path.

- a served URI is **actually reachable**. With `--prtemca
  pmix_remote_connections 1` the servers bind a routable interface, and the
  URI served for a *remote* node must carry that node's own address — not
  loopback, and not the master's. Off (the default) every server binds
  loopback, so the answer is truthful but only usable by a tool on that
  node. This is the assertion that says the feature is worth having, and one
  host cannot make it.

- the URIs **follow the DVM across a grow and a shrink**. `vm_ready()` runs
  on every `VM_READY` and re-sends the whole set, so a grow redistributes
  with no code of its own — asserted rather than assumed. A shrink needs
  nothing: the query resolves hostname → node → `node->daemon` and a shrink
  NULLs that, so a departed node cannot be answered for at all. The case
  shrinks node3 and requires its URI to stop being served while node2's is
  unaffected, so that stays true rather than being an accident of the
  current teardown order.

**The daemon body and the PMIx server host module (`src/prted`,
`test_prted`)**: `src/prted` is where the DVM actually lives, and almost
none of it means anything on one node. This phase asserts the three things
that need a second daemon, plus two cheap startup-hardening cases:

- a **cross-job job-level query** — a client asks for another job's
  `PMIX_JOB_SIZE` from a node that hosts none of that job's procs. The
  probe is `jobinfo`, a bare PMIx client that `build.sh` compiles the same
  way it compiles `elastic`. Note the honest scope comment in the test: the
  local PMIx server may satisfy this from its own cache without upcalling
  into `dmodex_req`, so treat it as coverage of the query working across
  nodes rather than as a guaranteed reproducer for the request-tracker
  mix-up it was written for.
- **job-scoped signal delivery**. `PRTE_DAEMON_SIGNAL_LOCAL_PROCS` carries
  the target job's nspace, and the daemon used to ignore it and signal
  every local child — so in a persistent DVM running two jobs, signalling
  one killed both. It takes two jobs whose procs land on the *same* daemon,
  which means a persistent DVM and more than one node. This case does catch
  the regression: against the unfixed daemon both jobs die.
- a **malformed `--singleton`** and a **`--prefix /`**. Both are startup
  paths that used to fault (the first inside PMIx, the second by
  overrunning a two-byte heap allocation), and both are one line to check
  once a swarm is up.

Two harness notes if you extend this phase. A live `prun` holds its own
`pmix.*` rendezvous file, so any case that leaves one running in the
background must point every later tool at the DVM explicitly — the helpers
`prted_dvm_start`/`PRUN`/`PRUN_BG` do that with `--report-uri` and
`--dvm-uri`. And `cleanup_swarm` reaps daemons and tools but not the
application processes they left behind, so a case that counts procs on a
node should clear strays first.

### The install persists too, so a failed build is silently testable

Same shape as the sticky-configure-args trap below, one level up. The
install lives in the shared volume and outlives any one `build.sh` run, so
a build that dies part-way — `configure` rejecting the baked PMIx because
it predates a capability PRRTE now requires is the everyday case — leaves
the **previous** install standing. `run-tests.sh`'s "tools on PATH" check
passes, the whole suite runs, and every failure it reports is really
"you are testing something else".

`build.sh` now deletes `/opt/prte/.build-stamp` before it starts and
rewrites it only after `make install` succeeds; `run-tests.sh`'s preflight
prints the stamp and refuses to run without one. If you see
`no build stamp in the volume`, re-run `build.sh` and **read its exit
status** — a redirected `./build.sh > log 2>&1; echo rc=$?` reports the
`echo`'s status if you are not careful, which is how this was missed.

If your PMIx is the thing `configure` rejected, build PMIx from source in
the same container: `PMIX_SRC=/path/to/openpmix ./build.sh`.

### The containers persist too, so a rebuilt image does not reach them

The third instance of the same shape, and the nastiest, because the stale
thing is not in the volume at all. The ten nodes are long-lived containers.
Rebuilding the base image — which is how the **baked PMIx** gets updated —
changes `prte-swarm:latest` but leaves the running containers on the image
they were created from. `build.sh` then compiles PRRTE inside a *new*
container (so, against the new PMIx) and installs it into the volume the
*old* containers read, and the daemons load the old PMIx. What you see is
an undefined symbol from `libprrte` in whichever case first reaches a new
PMIx entry point — `undefined symbol: pmix_iof_check_pattern` was the real
one — and nothing in the message mentions containers.

`run-tests.sh`'s preflight now compares each container's image ID against
`prte-swarm:latest` and refuses to run when they differ. The fix is

```sh
docker compose up -d --force-recreate
```

**and it matters where you run that from.** The compose project name
defaults to the *directory* name, `dockerswarm` — which is also the name of
openpmix's identical harness directory. Run from the wrong place (or with a
project name that does not match the containers you have) and compose
adopts the other project: it stopped the `pmix-node*` swarm, renamed our
own containers out from under themselves, failed on the name collision, and
left the ten `prte-node*` containers running the previous image. That is
exactly how the state above was reached. `docker-compose.yml` now pins
`name: prteswarm`, so a plain `docker compose` from this directory always
means this swarm. Verify with:

```sh
docker inspect prte-node1 --format '{{.Image}}'
docker images --no-trunc --format '{{.ID}}' prte-swarm:latest
```

### Rebuilding the image really does need `--no-cache`

`./build.sh image` alone is often not enough. The Dockerfile builds the
baked PMIx from

```dockerfile
RUN git clone --recursive --depth=1 -b "$PMIX_REF" "$PMIX_REPO" /src/pmix
```

and docker caches that layer by its *text*, not by what the remote now
contains — so a rebuild "succeeds" in seconds and bakes exactly the same
PMIx you were trying to get away from. To actually move the baked PMIx
forward:

```sh
docker build --no-cache --build-arg PMIX_REF=master -t prte-swarm:latest .
docker run --rm -v prte-build:/opt/prte prte-swarm:latest \
    rm -rf /opt/prte/vpath-linux /opt/prte/vpath-linux-pmix \
           /opt/prte/pmix /opt/prte/prte /opt/prte/.build-stamp
./build.sh                        # rebuild against the new PMIx
docker compose up -d --force-recreate
```

The volume wipe is not optional: `build.sh` reconfigures when the configure
*arguments* change, and they do not change when only the image's PMIx does
(see the sticky-arguments trap above).

The `PMIX_SRC=/path/to/openpmix` route avoids all of this, but the checkout
it names must be **autogen'd and not itself configured in-tree**: `build.sh`
runs `/pmix-src/configure` from a VPATH directory over a read-only bind
mount, so a tree with its own `config.status` is refused ("source directory
already configured") and a fresh `git clone` has no `configure` at all.

A `git worktree` off an existing clone is the clean way to get one (and it
keeps you out of a tree another session may be building in). Two things it
needs beyond `autogen.pl`:

```sh
git -c submodule.config/oac.url=<clone>/config/oac submodule update --init config/oac
```

because openpmix's `.gitmodules` points `config/oac` at a local path that
will not exist on your machine — and nothing else, because **`build.sh` now
pre-generates the flex output itself**. That was the other trap: a pristine
checkout has a `*.l` with no generated `*.c`, automake produces it at build
time *in the source directory*, and the builder mounts the source
read-only, so the build died with

```
config/ylwrap: line 204: .../keyval_lex.c: Read-only file system
```

A tree that has been built in place once already carries the file, which is
why this only ever bit a fresh clone — exactly what `PMIX_SRC` is usually
pointed at. `gen_lex` in `build.sh` runs `flex` on the host for any missing
one (taking the `-P` symbol prefix from the sibling `Makefile.am`), for the
PRRTE tree as well as the PMIx one.

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

**Arguments are not the only thing that goes stale.** A build dir configured
before the build system was *regenerated* is stale too, and it fails in a way
that points nowhere near the cause. Edit `configure.ac` or a `config/*.m4`,
re-run `./autogen.pl` on the host, and `configure`/`Makefile.in` are now newer
than the volume's `config.status`. An incremental `make` inside the container
then walks into maintainer-mode regeneration — and the container does not have
the exact `aclocal`/`automake` the host used:

```
/prrte-src/config/missing: line 85: aclocal-1.18: command not found
make: *** [Makefile:730: /prrte-src/aclocal.m4] Error 127
```

`build.sh` dies there, so the **previous** install stays in the volume and the
stamp is gone — which at least makes `run-tests.sh` refuse to run rather than
test it. `reconfigure_needed` now also reconfigures when `configure` is newer
than `config.status`, which is the condition that matters.

**And a source tree can go stale by *losing* a directory.** When a framework
stops being a framework (`src/mca/grpcomm` became `src/grpcomm`) or a component
is deleted, the persistent build dir keeps that subdirectory — and the parent
`Makefile`, also written by the old `configure` run, still names it in
`SUBDIRS`. So `make` recurses into a directory whose `Makefile.am` no longer
exists:

```
make[2]: *** No rule to make target '/prrte-src/src/mca/grpcomm/Makefile.am',
         needed by '/prrte-src/src/mca/grpcomm/Makefile.in'.  Stop.
```

Neither the configure arguments nor the `configure` timestamp changed, so
neither test above sees it, and pulling `master` into a tree the volume was
built from before the restructure is enough to hit it. `build.sh` now compares
every build-tree directory that holds a `Makefile` against the source tree,
reconfigures when one of them is gone, and removes the orphan as part of the
same step (`orphan_dirs`/`drop_orphans`) — dropping it matters, or the next run
finds an orphan again and reconfigures forever.

### Swapping `PMIX_SRC` between two trees rebuilds nothing

The nastiest member of this family, because every guard above is working as
designed and none of them applies. Both trees are bind-mounted at the **same**
container path, `/pmix-src`, so switching `PMIX_SRC` from one host checkout to
another changes the configure arguments not at all — `reconfigure_needed` has
nothing to see. What is left is an ordinary incremental `make`, which decides
by timestamp. So if the tree you switch **to** has older mtimes than the
objects built from the tree you switched **from**, make declares every object
up to date and the volume keeps serving the *other* tree's library.

This is exactly the shape an A/B against a `git worktree` produces. A fresh
worktree is stamped today; a long-lived clone is stamped whenever its files
were last written. Going clone → worktree rebuilds (the worktree is newer) and
looks fine. Going worktree → clone recompiles **zero** files, prints
`Linux build complete`, rewrites `.build-stamp` — the build really did succeed,
it just did nothing — and the next suite silently measures the library you were
trying to get away from. That was first hit while A/B-ing openpmix
[#4113](https://github.com/openpmix/openpmix/issues/4113): the "back to master"
build reported success and the reproducer went on failing, which reads as a
fix that does not work.

So when a run is meant to turn on a specific source change, **verify the object
carrying it was compiled**, rather than trusting the build to have noticed:

```sh
grep -cE "server/pmix_server_(fence|group)\.lo" build.log   # must be non-zero
```

To force it, wipe the PMIx build dir and install — a `touch` over the source
tree also works, but it mutates a clone other worktrees and sessions may share:

```sh
docker run --rm -v prte-build:/opt/prte prte-swarm:latest \
    rm -rf /opt/prte/vpath-linux-pmix /opt/prte/pmix /opt/prte/.build-stamp
PMIX_SRC=/path/to/openpmix ./build.sh
```

The same reasoning applies to `OMPI_SRC`, and to `PRTE_SRC` if you ever point
the builder at a second PRRTE tree.

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

### The suite runs the OOB with worker threads

`prte_oob_base.num_progress_threads` defaults to **0** in the library: every
peer's socket send/recv events are serviced on the main progress thread, so
the OOB is effectively single-threaded. With workers configured, each peer is
assigned to a worker base and its handlers run on that thread, posting
completions back to `prte_event_base` — and that is the arrangement in which a
missing lock, a peer touched from two threads, or a completion that assumes it
runs on the main thread has any consequence at all.

At the library default this suite could never reach that path: not one case
would exercise it, and a race introduced there would ship. So `run-tests.sh`
opts in, exporting `PRTE_MCA_oob_progress_threads` into every tool it runs —
and thereby into every daemon, since `prte_plm_base_setup_virtual_machine`
harvests `PRTE_MCA_*` from the launcher's environment onto the daemon command
line.

**The default here is 2**, the smallest genuinely multi-threaded setting: one
worker plus the main thread is already two threads touching the OOB, and two
workers means two peers can be serviced at once. The preflight prints the
number it used, so a suite log always says which mode produced it.

Override with `PRTE_SWARM_OOB_THREADS`:

```sh
PRTE_SWARM_OOB_THREADS=0 ./run-tests.sh linux   # library default, single-threaded
PRTE_SWARM_OOB_THREADS=8 ./run-tests.sh linux   # push the threading harder
```

**If a case fails, re-run it at 0 before reading the failure as a bug in what
you just changed.** A failure that reproduces only with workers is a genuine
threading defect worth chasing; one that reproduces at 0 as well has nothing
to do with the OOB threading. Keeping that distinction cheap is the reason the
knob exists rather than the number being hard-coded.

### Components as DSOs (`PRTE_SWARM_MCA_DSO`)

`--enable-mca-dso` builds every MCA component as a run-time loadable DSO in
`lib/prte` instead of linking it into `libprrte`. It is a supported build —
and the one the stubbed test-build launchers require — but the default is
static, so a component that only works when it is linked in (a symbol that
needed exporting, a constructor that ran at link time, an inter-component
dependency the static link resolved for free) breaks nobody's build and
surfaces at a user's site. CI now builds and smoke-launches it on one Linux
node (`ubuntuMcaDso` in `.github/workflows/builds.yaml`); this is where it
gets run across daemons.

```sh
PRTE_SWARM_MCA_DSO=1 ./build.sh          # into the volume, components as DSOs
PRTE_SWARM_MCA_DSO=1 ./build.sh macos    # native build, same
./run-tests.sh linux                     # the ordinary suite, no flag needed
```

The whole suite is the test: nothing here is DSO-specific, and that is the
point — every case that exercises a component at all now exercises loading
it. The preflight prints which of the two it ran, asked of the install
rather than of the variable, because the volume outlives the shell that set
it: `build.sh` records the mode in `/opt/prte/.build-mode` beside the build
stamp, and the suite reads it there.

**Do not try to infer the mode by counting DSOs in the install.** The
default build installs a couple of its own — the launchers that need
third-party headers are in the default `--enable-mca-dso` list, so an
ordinary swarm build ships `prte_mca_ras_slurm.so` and nothing else. A
count says "DSO build" for that. The CI job has the same problem and
solves it by naming a component that is loadable *only* under the flag
(`rmaps/round_robin`; `rmaps` is never in the default list).

**It found something the first time it ran**, which is the argument for
keeping it: `prun` segfaulted in teardown on every single invocation —
after the job had run correctly, so every affected case reported right
output and `rc=139`. PRRTE has no component repository of its own, so
`PMIx_tool_finalize` reaches `pmix_mca_base_close()` and dlcloses PRRTE's
component DSOs along with PMIx's; `prun` then closed the `ess` framework
*afterwards* and walked its component list into unmapped memory. In the
default build those structs live in `libprrte` and are mapped for the life
of the process, so nothing was ever wrong there. The rule that came out of
it: **a PRRTE framework must be closed while PMIx is still up.**

Three things to know:

- **The knob is spelled `PRTE_SWARM_MCA_DSO`, not `PRTE_MCA_DSO`.** PRRTE
  harvests `PRTE_MCA_*` out of the launcher's environment onto the `prted`
  command line, so the latter name would reach every daemon as
  `--prtemca DSO 1`.
- **It is a configure argument**, so `reconfigure_needed` sees it change and
  reconfigures — the same mechanism that catches a changed `--with-pmix`. No
  volume wipe needed to switch modes.
- **Turning it back off removes the installed DSOs.** `make install` only
  adds, and the install outlives the build dir, so the previous mode's
  `lib/prte` would otherwise sit there for the static build to `dlopen` —
  every component present twice, one copy stale. `build.sh` deletes that
  directory whenever it reconfigures.

Against the baked PMIx this build may not configure at all — PRRTE's floor
moves ahead of the image — in which case use `PMIX_SRC=<checkout>`, the same
remedy as for any other configure rejection here.

## 7. Cleanup hygiene

`run-tests.sh` cleans every node before its first case and between phases.
That first sweep matters more than it looks: the nodes are long-lived
containers while the install they read is replaced under them, so a daemon
or a `pmix.*` rendezvous file left from a previous run may be holding a
library that no longer exists. Rebuilding with a different `PMIX_SRC` and
running straight away produced twenty-one failures with nothing in them
pointing at the cause — the first launch simply came back killed.

If you drive things by hand, clear stale state on **every** node between
DVM runs — the same sweep `cleanup_swarm` does:

```sh
for n in $(seq 1 10); do
  docker exec prte-node$n sh -c '
    for t in prted prte prterun prun pterm; do pkill -9 -x $t; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* /tmp/pmix.*
    find /tmp -maxdepth 2 -name "pmix.*" -prune -exec rm -rf {} +
    true'
done
```

**Every tool has its own session-dir prefix, and one missed prefix is enough.**
`prte.<pid>` is the HNP, `prtrn.<pid>` is `prterun`, `prted.<pid>` is a
bootstrapped daemon standing on its own, `prun.<pid>` is `prun` itself, and
`ompi.<pid>` is anything run under the ompi personality. Each of them holds a
`pmix.*` server rendezvous file, a system-level server drops `pmix.sys.<host>`
straight into `/tmp`, and any one left behind makes the next tool report
*"multiple possible servers … connection handles have been read from files
named pmix.\*"* and fail to find the DVM. That is why the sweep ends with a
`find`: it catches the rendezvous file of a session dir whose prefix this list
has not heard of. Leaving `pmix.*` out of `cleanup_swarm` is
[#2526](https://github.com/openpmix/prrte/issues/2526) — a hand-driven
`prterun` before a suite run made the elastic block fail for reasons that had
nothing to do with the code.

The tools are killed, not just the daemons: a live `prun` or `pterm` is
holding a rendezvous file of its own, and reaping it is the point of a
teardown.

A detached `prted --daemonize` **survives an HNP kill**; orphans on other nodes
make the next DVM trip over stale rendezvous files ("multiple possible
servers"). `pterm` is the clean shutdown; still run the loop afterward to be
safe.

If you are running a second swarm (`PRTE_SWARM`, §4), the loop above is
per-swarm — use that swarm's container names. Nothing in it can reach the
other swarm's `/tmp`, because the containers are different containers.

## 8. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PRRTE source edit | `./build.sh` (incremental into the volume) |
| pick up an openpmix edit | `PMIX_SRC=/path/to/openpmix ./build.sh` |
| run the suite against components built as DSOs | `PRTE_SWARM_MCA_DSO=1 ./build.sh` then the ordinary `./run-tests.sh linux` — see §6, "Components as DSOs" |
| force a clean PRRTE rebuild | `docker volume rm prte-build && ./build.sh` |
| rebuild the base image (new baked PMIx) | `docker build --no-cache --build-arg PMIX_REF=master -t prte-swarm:latest .` — `./build.sh image` reuses docker's cached `git clone`; then wipe the volume's VPATH dirs and recreate the containers (see "The containers persist too") |
| tear down the swarm | `docker compose down` (the `prte-build` volume persists) |
| run a second, independent swarm | `export PRTE_SWARM=alt` and repeat the quick start — see §4. Every command in this table then names that swarm's volume (`alt-build`) and containers (`alt-node*`), and **a `docker compose down` without the variable takes down the *default* swarm** |

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

**A daemon grown in after a shrink starts behind the DVM in two ways, and
only one of them is about routing.** The suite covers both, in adjacent
cases, because neither can see the other's defect:

- Its *departed-vpid set* is empty, so it can compute a first routing tree
  whose parent is a retired rank. Only visible at a radix small enough for
  the tree to have depth — hence the explicit `rml_base_radix 2` in that
  case, without which every daemon's parent is rank 0 and rank 0 is always
  alive.
- Its *collective recovery epoch* is zero, because the shrink's
  `DAEMON_DIED` broadcast went out while it was unreachable. Every fence or
  group contribution it makes is dropped as stale, at any radix. Invisible
  to every case that launches `hostname`, which never fences — so that case
  runs a `fencer` job that spans the grown-in node.

Still bound elastic commands with `timeout` in any new test: a grow that does
not complete otherwise wedges the whole suite rather than failing one case.

## 10. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `prte-node1` | node1 | head node (HNP) — start `prte` here, run all tools here |
| `prte-node2`..`prte-node10` | node2..node10 | DVM nodes (grow/shrink targets) |

Network: bridge `prteswarm_dvm`. All nodes mount the shared `prte-build`
volume read-only at `/opt/prte`, where `build.sh` installs PRRTE
(`/opt/prte/prte`) and writes `/opt/prte/env.sh`. To add or remove nodes, copy
or delete a service block in `docker-compose.yml` (and adjust the `seq 1 10`
loops to match).

The container, volume, and network names above are the `PRTE_SWARM=prte`
default; under another name they all shift together (§4). The **hostnames**
never do — `node1`..`node10` is what the tests name, in every swarm.

### Giving the nodes hardware they do not have

A daemon told to read its topology from a file reports **that** topology
as its own, so hardware the containers lack can be faked — and faked
*differently per node*, which is what makes it useful rather than merely
convenient. Copy a topology XML into each container at the **same path**
with different content, then export both of these on the head node before
starting `prte`:

| variable | what it decides |
|----------|-----------------|
| `PRTE_MCA_hwloc_use_topo_file` | what the daemon reports, hence what the mapper places against |
| `PMIX_MCA_pmix_hwloc_topo_file` | the daemon's PMIx server topology — what resolves a device to a vendor identity at fork time |

Both are needed and they are not the same layer: setting only the first
gives a correct map and no environment. One export on the head node
reaches the whole DVM, because `plm_base_launch_support.c` forwards every
`PRTE_MCA_*` and `PMIX_MCA_*` envar to each daemon as a `--prtemca` /
`--pmixmca` argument — while `/tmp` is container-local, so the content
stays per node.

`test_rmaps` uses this for GPUs, which no container has: each node gets
`test/topologies/turin-4gpu-nvml.xml` with the UUIDs rewritten to carry
its node number. The result is four nodes whose hardware differs only in
serial numbers, which is precisely the case the HNP collapses onto one
recorded topology (`TOPOLOGY ALREADY RECORDED ... SOME DIFFS FOUND`) —
and therefore the case where handing a process another node's device
identity would go unnoticed.

## 11. Spawning a child job (`dynamic`)

Some behavior only exists between a **primary** job and a job it spawned —
`report-child-jobs-separately`, which decides whether a child's exit status
reaches the launcher's, is the reason this was added. No amount of
single-job testing can show it.

`build.sh` compiles the tree's own `PMIx_Spawn` example,
[`examples/dynamic.c`](../../examples/dynamic.c), into the install as
`dynamic`. Two things about it drive how a test is written:

- **Rank 0 spawns `client` from its own cwd** (`$PWD/client`, an absolute
  path built with `getcwd()`). So you choose what the child *is*, and what
  it exits with, by putting an executable at that path and running the
  parent from that directory.
- **`/tmp` is per-container.** The spawn names an absolute path, and the
  child is mapped by the normal policy across the DVM, so the file has to
  exist on *every node the child could land on* — not just the head node.
  A missing file shows up as `PMIX_ERR_JOB_FAILED_TO_LAUNCH`, and too few
  slots as `PMIX_ERR_JOB_FAILED_TO_MAP` (the child asks for 2 procs on top
  of the parent, so allocate at least 3 slots).

```sh
for n in 1 2 3; do
    docker exec prte-node$n bash -lc 'mkdir -p /tmp/dyn &&
        printf "#!/bin/sh\nexit 7\n" > /tmp/dyn/client && chmod +x /tmp/dyn/client'
done
$RUN '. /opt/prte/env.sh; cd /tmp/dyn &&
      prterun --host node1:2,node2:2,node3:2 -np 1 dynamic'   # -> exits 7
```

Always assert that `Spawn success` appears before asserting on the exit
code: a spawn that failed to map or launch also produces a non-zero status,
and would otherwise read as the child's.

## 12. SLURM is not here — it is in `contrib/slurmswarm`

`ras/slurm` is the only ras component with a full elastic **modify** surface,
and everything past initial discovery is a shell-out: `salloc` to grow,
`scontrol show job <id> --json` to learn what SLURM granted, `scontrol update
job <id> ReqNodeList=…` to shrink one in place, `scancel` to give one back.
None of that runs on a developer machine, so this harness used to carry a
stand-in control plane, `fake-slurm.py`, and a phase that drove it.

**Both are gone.** [`contrib/slurmswarm`](../slurmswarm/) is the same harness
with a real `slurmctld`, ten real `slurmd`s and SLURM built from source, and
it now carries every one of those cases. A fake scheduler can show that PRRTE
issued the right command; only a real one can show that SLURM accepts it, and
keeping both meant two suites asserting the same behavior with the weaker one
free to disagree.

Two things survived the move rather than being dropped, because a live
`slurmctld` cannot produce them on demand — see
[`slurm-shim.py`](../slurmswarm/slurm-shim.py), which *wraps* the real
commands rather than replacing them:

- **what PRRTE asked for.** The scheduler records the job it created, not the
  argv that created it, and several behaviors are only visible there: an
  attribute that must be omitted when its source is unset, a `propagate_*`
  MCA parameter that must drop exactly its own argument, the absence of
  `--wrap`.
- **what PRRTE does when the scheduler misbehaves.** Unparsable JSON and a
  failing `scancel` are armed one at a time; everything else passes through
  to the real command.

What is left here is the **compile** coverage: `build.sh` still passes
`--with-jansson`, so `ras_slurm_jansson.c` (the ~1000-line parser) is
compiled by the harness that gets built on every change, rather than only by
the slower sibling. Nothing in this suite runs it.


## 13. The data server (`dataserver`, `test_runtime`)

Most of [`src/runtime`](../../src/runtime/AGENTS.md) needs no DVM and is
covered by `test/unit/runtime`. Two things are not, and they are what the
`test_runtime` phase asserts.

**The publish/lookup data server is a single store on the HNP** that every
client reaches over the RML through its own daemon. That shape is what makes
it a multi-node subject:

- **`PMIX_RANGE_LOCAL` compares the publisher's proxy against the
  requestor's proxy** — the daemons that relayed the two requests. On one
  node those are the same object no matter what the code does, so the check
  cannot be wrong there. (It was: neither object's constructor initialized
  `proxy`, and `PMIX_NEW` does not zero its allocation.)
- **A `PMIX_WAIT` lookup parks in the store** until a later publish
  satisfies it. The publish arrives from one daemon and the reply goes back
  out through another, so the parked request has to carry the requestor's
  proxy and room number across the gap. That path also has to *dispose of*
  the answer buffer it was handed and will never send.
- **A partial lookup** (two keys, one published) has to return the half it
  found alongside `PMIX_ERR_PARTIAL_SUCCESS`. It used to return the status
  and drop both the values and the buffer holding them.

The probe is `dataserver`, compiled by `build.sh` the same way as `elastic`
and `jobinfo`:

```sh
dataserver publish <key> <value> [session|namespace|local|proc-local|global] [secs]
dataserver lookup <key> [secs]           # no wait
dataserver lookupwait <key> [secs]       # PMIX_WAIT -- parks in the store
dataserver lookup2 <key1> <key2> [secs]  # the partial-success shape
dataserver unpublish <key> [secs]        # publish, confirm, unpublish, confirm gone
```

`publish` and `lookupwait` stay alive for their `secs` argument and print a
marker line (`PUBLISHED`, `WAITING`) as soon as they reach that state, so a
test runs them with `PRUN_BG` and greps the capture file rather than
sleeping blind.

**The second thing** is that the object destructors — and the ownership
rules in `src/runtime/AGENTS.md` they encode — only run for real at
teardown. `prte_session_t` had been registered against the wrong parent
class, and the symptom (an assert inside `pmix_list_item_destruct` reading
the middle of the session's own data) fires only when a session is actually
*released*. So the phase ends by running jobs and then taking the DVM down,
asserting `pterm` succeeds, that nothing on the way out looks like an assert
or a fault, and that no daemon survived.

Note that the harness image builds a debug PMIx. That matters here: the
class-hierarchy check that catches this is `#if PMIX_ENABLE_DEBUG` only, so
against a release PMIx the same bug is silent memory corruption instead.

## 14. A remote app gets an EMPTY environment, and that hides a stale PMIx

Two related traps, same root cause, and the second one is genuinely nasty.

**The root cause.** An application launched onto a node other than node1
does *not* inherit the head node's login environment. Concretely, for a
proc on node4:

```
$ prun --host node4:1,node1:1 -n 2 --map-by node sh -c 'echo $(hostname) LDLP=$LD_LIBRARY_PATH'
node4 LDLP=
node1 LDLP=/opt/prte/prte/lib:/opt/prte/pmix/lib:...
```

`-x LD_LIBRARY_PATH` does not change that. node1 works only because its
apps inherit from the HNP, which *was* started from a shell that sourced
`env.sh`.

**Trap 1 — PATH.** The node entrypoint symlinks the install's `bin` into
`/usr/local/bin` when the **container** starts, so anything added by a later
`build.sh` is not on a remote app's PATH. A bare name then fails with
`PMIX_ERR_JOB_FAILED_TO_LAUNCH` and *no diagnostic*. Use an absolute path
for helpers in new cases (`DS=/opt/prte/prte/bin/dataserver`); the older
`jobinfo`/`elastic` cases work only because the entrypoint happened to link
them.

**Trap 2 — the wrong libpmix, silently.** `/usr/local/lib/libpmix.so.2` is
the PMIx baked into the *image*. With no `LD_LIBRARY_PATH`, that is what a
remote app loads — same soname, same version string, different code — so
every PMIx-linking helper on nodes 2-10 was running the image's PMIx rather
than the one `PMIX_SRC=... ./build.sh` had just built. Nothing announces
this. A change spanning both code bases passes on node1 and quietly tests
the wrong library everywhere else; that is exactly how the partial-lookup
case (§13) looked broken after it had been fixed.

`build.sh` now links every helper with `-Wl,-rpath,$PMIX_PREFIX/lib` so the
binary finds the right PMIx regardless of environment. **Any new helper must
carry that too.** To check one:

```sh
docker exec prte-node4 sh -c 'ldd /opt/prte/prte/bin/<helper> | grep pmix'
# must say /opt/prte/pmix/lib, NOT /usr/local/lib
```

The daemons themselves are fine — `prted` is launched with the right
environment and loads the volume's PMIx on every node. It is only the
application processes they fork that lose it.

## 15. Group collectives (`groupcon`, `test_grpcomm`)

`PMIx_Group_construct` is a two-phase collective — an up-tree rollup to
the HNP, then an xcast of the assembled result back down — and the half
worth testing across nodes is the **down-tree** one. `grp_release()` runs
on *every* daemon: it takes the broadcast, hands the group's context id,
group info and endpoint data to its **own** local PMIx server via
`PMIx_server_register_resources()`, and only then releases the local
participants of the collective. On one host there is exactly one daemon
and it is also the HNP, so a daemon that merely *received* the release is
never exercised at all.

That registration used to be waited on, parking the daemon's whole event
loop on every construct; it is now a continuation
(`grp_release_regcbfunc` → `grp_release_resume` →
`grp_release_complete`). **That continuation is what this phase guards.**
Lose the caddy anywhere between issuing the registration and resuming and
the local participants are never released — the construct does not fail,
it *hangs*, on every daemon at once. Deleting the thread-shift and
re-running turns 7 of the phase's 10 assertions red, which is how the
teeth were confirmed rather than assumed.

### ...and the invited group, which has no collective at all

`PMIx_Group_invite` forms a group purely through event notification, so
nothing about it reaches grpcomm. That leaves its leader with no way to ask
the host for a `PMIX_GROUP_ASSIGN_CONTEXT_ID`, and until August 2026 the
directive was simply dropped. It now goes out as a `PMIx_Job_control`
request, which arrives at whichever daemon hosts the leader - and only the
HNP holds the id pool, so any other daemon has to relay it to the HNP and
hand the answer back.

`groupinv` is the probe, and **it makes the highest rank the leader on
purpose**: mapped by node that rank is on the last node of the DVM and never
on the HNP. Run it with the leader on node1 and the relay is skipped and the
case passes having tested nothing, which is why this cannot live in a
single-host suite.

```sh
groupinv <groupID>
```

Every rank also posts one `PMIx_Put` value at `PMIX_REMOTE` scope and never
commits or fences it, then reads every peer's back once the group has formed
- with `PMIX_OPTIONAL` gets, so they cannot leave the process to find it, and
with the context id as a *qualifier*, because that is how a contribution to a
group holding one is stored. Nothing but the group exchange can have carried
those values.

The probe is `groupcon`, compiled by `build.sh` the same way as
`elastic`/`dataserver`/`proctable`:

```sh
groupcon [--ft] [--delay <s>] <groupID> [secs]
```

Every rank contributes `PMIX_GROUP_LOCAL_CID` = 1234 + rank as
`PMIX_GROUP_INFO`, requests `PMIX_GROUP_ASSIGN_CONTEXT_ID`, constructs,
reads **every** rank's local cid back, and destructs. Output is one
grep-friendly `GRP <rank> <what> …` line per step.

**One thing this phase deliberately does not claim.** The read-back runs
with no fence after the construct returns, which looks like an assertion
that the daemon registered the group with its PMIx server before
releasing its clients. It is not, and saying so would be wrong: the
construct hands the same group info back to the client in its *results*,
so the client library answers those reads out of its own cache —
skipping `PMIx_server_register_resources` entirely still passes them
(measured). What the reads are good for is checking that the membership
and group info each daemon returned are complete and identical, which is
a genuinely per-daemon property.

The other two assertions in the phase are shape checks worth keeping: the
group must actually span more than one node (or the case is a
single-host test wearing a hat), and three back-to-back constructs
followed by a plain job must all succeed — a caddy leak or a tracker that
is never deleted shows up as drift across runs rather than as one bad
one.

### Losing a daemon mid-construct (`test_grpcomm_ft`)

The second half of the phase kills a real `prted` while a construct is in
flight. It is gated on `pmix_cap PMIX_CAP_GROUP_FT` and skips without it,
since the whole feature compiles out.

**`--delay <s>` is what makes these cases real.** Without a stagger the
construct is over microseconds after `prun` starts and the kill lands on
a DVM with nothing in flight — every assertion still passes, and nothing
has been tested. The ranks sleep before calling construct; the kill goes
in during that window. If you change the timing, check the capture file
still shows `DELAYING` lines before the daemon dies.

**`--rtos recoverable` is not optional, and this is the thing that will
waste your afternoon.** Killing the daemon that hosts a rank does not
merely remove a group member — by default the errmgr treats the loss of
any proc as fatal to its whole job and terminates the survivors too, so
they never reach the construct at all. The capture then contains nothing
but `DELAYING` lines and *"We cannot recover from this failure, and
therefore will terminate the job"*, and every assertion fails with an
empty string, which reads like the feature is broken when it is the test
that is. The survivors have to be told to survive: `recoverable` (or
`continuous`) is what flips `errmgr/dvm` from abort-the-job to
notify-and-continue. That is also the honest usage — an application
asking for a fault-tolerant group collective is by definition one that
expects to outlive a lost member.

Note the *"will terminate the job"* text is emitted unconditionally by
the daemon-loss path, so it still appears in a recoverable run that goes
on to complete perfectly. Do not read it as a failure, and do not assert
on its absence.

Four cases:

1. **With `--ft`, the construct completes on the survivors.** Three
   survivors must each report `CONSTRUCT PMIX_SUCCESS`, agree on
   `MEMBERS 3`, and receive a `MEMBER-FAILED` event naming the process
   that went away. The event is the part worth having: the construct
   returning success only says the group formed, not that the membership
   is smaller than what was asked for.
2. **Without `--ft`, the same loss aborts** with
   `PMIX_GROUP_CONSTRUCT_ABORT`, and the DVM survives. This is the
   regression guard on the pre-existing behavior, and it is the case that
   fails if the flag stops being plumbed. Note `groupcon` jumps to its
   exit on a failed construct, so there is **no** `DESTRUCT` line here —
   assert on the construct status.
3. **Losing a relay-only daemon does not stall the construct.** Run at
   `--prtemca rml_base_radix 2` so the tree is deep enough to have an
   interior daemon at all; at the default radix every daemon is a child of
   the controller and the case cannot exist. Membership must be
   **unchanged** — nothing was lost, only a message path.
4. **Further constructs still work afterwards**, which is where a leaked
   tracker, memo entry or caddy shows up.

A note if you extend this. Do not assert on the *absence* of a failure
marker — a run whose job never started has no marker either. Every case
here counts positive evidence (`CONSTRUCT`, `MEMBERS`, `MEMBER-FAILED`
lines) and guards the setup separately by checking the target daemon is
actually up before killing it.

## 16. The event base and the constants (`test_event`, `test_include`)

Two phases for two directories that are almost entirely covered without a
DVM — [`src/event`](../../src/event/AGENTS.md) by `test/unit/event` and
[`src/include`](../../src/include/AGENTS.md) by `test/unit/include`. What
lands here is only what a single process cannot show.

**`test_event`.** Three things, all of which need an event on one machine to
have a consequence on another:

- **A job timeout fires, and takes the job with it.** `--timeout` arms a
  `prte_event_evtimer` on the HNP; when it fires, the HNP has to reach
  processes it does not host. The case requires both a non-zero exit *and*
  no surviving application process on any of the three nodes.
- **A job that beats its timeout is not killed by it.** The other half, and
  the one a broken `prte_event_evtimer_del()` breaks: a pending timer that
  is not deleted fires afterwards against a job that has already finished.
  A generous timeout over a fast job must produce a clean result with no
  timeout reported.
- **A signal caught on one node is re-raised on another.** `prun` catches
  the signal on its own event base (a `PRTE_EV_SIGNAL` event on a
  `prte_event_list_item_t`), reads the number back with
  `PRTE_EVENT_SIGNAL()`, and relays it over the RML; the daemon holding the
  process re-raises it locally. `test_prted` already covers the *job
  scoping* of that path with both jobs on one node — what only exists
  across nodes is the relay, so this case puts the launcher on node1 and the
  process on node4.
- **The DVM tears its event base down cleanly on every node.**
  `prte_finalize()` closes the event base — it frees `prte_sync_event_base`
  and clears both globals. Nothing called that function until recently: it
  existed, had no callers, and left both globals pointing at freed memory.
  A base freed while something still holds a registered event, or freed
  before the last PMIx server upcall has drained, shows up here as a daemon
  that crashes or hangs instead of exiting. Only a real shutdown runs that
  code. The case gives eight daemons real work first, then requires `pterm`
  to return 0 with no crash text, every daemon gone, and no session
  directory left behind.

Note the unit test deliberately does **not** assert that raising a signal
runs its callback. Whether a signal raised before the loop is entered is
reported at all is a property of the event library backend — libevent's
kqueue backend on macOS does not report it, and a three-line program using
`event_assign`/`event_add`/`raise` hangs in `event_base_loop()` the same
way. That assertion belongs here, where the signal arrives at a running
process.

**`test_include`.** PRRTE's error codes were renumbered onto
`PMIX_EXTERNAL_ERR_BASE` — they used to sit on top of PMIx's own statuses,
46 of them exactly, and the second half of the list was *positive*. A
renumbering does not break the build. It breaks a real failure path into
"Unknown error", or into somebody else's error. Those paths run on the
daemon that hosts the process, so the phase provokes genuine failures on a
**remote** node — a missing executable, a missing working directory, an
impossible slot request — and requires each to come back named, never as
"Unknown error" and never as a bare number.

The byte-order helpers (`prte_hton64`/`prte_ntoh64`) need no case of their
own: every message between daemons runs its header epoch through them, so
every other phase in this suite exercises them already. What no container
swarm can cover is the case they exist for — a **heterogeneous** DVM, where
the two ends disagree about endianness.

## 17. Daemon bring-up (`test_ess`)

[`src/mca/ess`](../../src/mca/ess/AGENTS.md) *is* the bring-up, so nearly
all of it needs a live DVM by construction, and the two pieces that are
pure parsing are covered without one by `test/unit/ess`. Three things are
left for a swarm:

- **Every daemon derives a distinct identity.** A daemon's rank is
  `ess_base_vpid` plus a per-node index, summed in
  `prte_ess_base_set_identity()`. Getting that sum wrong makes two
  daemons claim the same rank, and the failure is silent — the DVM simply
  loses a node with no error anywhere. So the case asserts a job mapped
  one-per-node reaches as many **distinct** hosts as there are nodes.
- **A daemon that cannot establish an identity fails cleanly.** A vpid
  that is not a number used to be read as 0 by `strtoul`, and 0 is the
  controller's rank, so the daemon would adopt the HNP's identity. The
  case starts a `prted` by hand with a bad vpid on node2 and requires
  both the diagnostic and that no daemon is left running.
- **A bad `--forward-signals` request is refused**, by number as well as
  by name — those are separate parse branches in
  `prte_ess_base_setup_signals()`, and a check added to one is easy to
  leave off the other. This one is single-node by nature; it is here as
  the end-to-end proof that the refusal reaches the user through a real
  tool invocation.

**What is deliberately not here: forwarded signals reaching a process.**
That path is tool-side — `prte`/`prun` catch the signal and relay
`PRTE_DAEMON_SIGNAL_LOCAL_PROCS` over the RML — and it is already covered
twice: `test_event` asserts the cross-node relay (launcher on node1,
process on node4) and `test_prted` asserts its job scoping. A daemon
installs no handlers of its own for those signals; the block in
`prte_ess_base_prted_setup()` that appeared to do so read a list that is
always empty in a `prted`, and has been removed. Do not write a case that
signals a `prted` directly and expects delivery — it will simply kill the
daemon.

## 18. Measuring collective scaling (`scaletest`, `scaletest.sh`)

Everything above this section answers *is it correct*. This one answers
*what does it cost*, and it is the only part of the harness that is not a
pass/fail suite: `scaletest.sh` writes a CSV meant to be opened in a
spreadsheet and plotted.

The subject is the DVM's collectives. A collective's cost **is** the shape
of the daemon tree, so there is nothing here a single host can measure — at
one daemon the routing radix has no effect whatsoever and the allgather
never touches a wire. That is why this lives in the swarm.

### The two numbers

`scaletest` is a bare PMIx client. Each iteration it puts `--nkeys`
uniquely-named values whose sizes cycle through `--sizes`, calls
`PMIx_Commit`, and then runs two fences over the whole job:

- **COLLECT** — `PMIx_Fence` with `PMIX_COLLECT_DATA` **true**. The
  allgather: every rank's committed data goes up the routing tree to the
  HNP and back down to every daemon. This is the one that should grow with
  the payload.
- **BARRIER** — `PMIx_Fence` with `PMIX_COLLECT_DATA` **false**. The same
  collective carrying nothing, so it is the latency floor the tree imposes,
  and `collect − barrier` is what the data itself cost. The summary CSV
  reports that difference as `data_cost_us`.

`PUT` (the put loop plus the commit) is timed too, and is deliberately
*not* part of either fence: a commit only hands data to the local server,
so it measures staging, not movement.

Every phase is preceded by an **unmeasured** barrier. Without it a
straggler's late arrival is charged to the next collective and what you
measure is the skew of the previous one.

### Why the timestamps are absolute, and what that buys

Every "node" here is a container on one host sharing one kernel clock, so
`CLOCK_REALTIME` is directly comparable across ranks. Each rank prints the
absolute start and end of every phase and the driver computes

```
wall = max(end) − min(start)          over every rank
```

which is the real answer to *"how long until all procs cleared the fence"*.
A per-rank elapsed time cannot answer it — a rank that entered late has a
*short* elapsed time precisely because everyone else was already blocked
waiting for it. The driver reports the max of the per-rank elapsed times in
a `*_max_local_us` column alongside, because that is the only thing a real
cluster (with genuinely separate clocks) could measure, and it is useful to
see how far the two diverge.

**Nothing is printed until the very end.** Output from a rank travels back
over the IOF wire through the same daemons the next collective needs, so a
client that printed as it went would be measuring I/O forwarding. Results
are buffered and flushed after a final barrier.

### Running it

The driver keeps its own swarm — `PRTE_SWARM=scale`, containers
`scale-node1..N`, volume `scale-build` — so a sweep and the functional
ten-node swarm cannot disturb each other. `docker compose` cannot loop, so
the compose file is generated:

```sh
./scaletest.sh build                  # PMIX_SRC=<checkout> to build PMIx too
./scaletest.sh up 40                  # generate docker-compose.scale.yml, start 40
./scaletest.sh run --nodes 1,2,4,8,16,32,40 --ppn 1,2 --radix 2,4,64
./scaletest.sh down
```

Four independent knobs, each a comma-separated sweep list:

| flag | varies |
|------|--------|
| `--nodes` | how many containers are in the DVM |
| `--ppn` | application procs per node (`--map-by ppr:N:node`) |
| `--radix` | the routing radix — `--prtemca rml_base_radix` |
| `--nkeys`, `--sizes` | how much data each rank contributes |

A `--sizes` element may itself be a list of sizes the keys cycle through,
joined with `+` (`--sizes 128,1024,'64+4096+65536'` is three configurations,
the last one mixing three sizes). Plus, not comma, because the sweep list is
already comma-separated *and* because a CSV column cannot hold a comma.

**The radix is a property of the DVM**, fixed when the daemons start, so the
sweep restarts the DVM for every `(nodes, radix)` pair and runs the cheap
knobs inside that. Restarting is also what keeps one configuration's
collected data out of the next one's daemons.

### Two things the driver refuses to do

- **Measure a DVM that came up short.** `dvm_start` waits for the URI and
  then runs one proc per node and counts *distinct* hostnames. A 40-node
  request that only brought up 31 daemons would otherwise be silently
  recorded as a 40-node measurement.
- **Map a job into slots the previous job has not released.** Every DVM is
  given `max(--ppn) + 1` slots a node, and the readiness probe is followed
  by a settle. Both exist for one failure: `prun` returning is not the same
  as every proc having been reaped on every daemon, and a job whose ppn
  exactly equals the slot count is then refused outright with *"All nodes
  which are allocated for this job are already filled"*
  (`PMIX_ERR_JOB_FAILED_TO_MAP`). The spare slot is never filled — mapping
  is `ppr:N:node` — so it cannot change what is measured. Do not "tidy" it
  away: `--ppn 1` over 40 nodes failed **every** run without it, while a
  sweep that happened to include a larger ppn passed, which makes the
  failure look like a property of the node count rather than of the slots.
- **Run a configuration that will not fit.** After a collect fence *every*
  daemon holds *every* rank's data, and the iterations use distinct keys —
  so each daemon ends up carrying `bytes_per_rank × nprocs × iterations`.
  At 160 ranks, 32 keys of 64 KB and 5 iterations that is nearly a gigabyte
  **per daemon**, which on a laptop's Docker VM is not a measurement, it is
  an OOM. The driver computes that figure, puts it in the CSV as
  `collected_bytes`, and skips anything above `--max-bytes` (256 MB by
  default) with a message saying so.

  **And here the per-daemon figure is not the one that binds.** Every "node"
  of this swarm is a container on *one* host, so what has to fit is
  `collected_bytes × nodes` — 128 MB a daemon across 40 nodes is 5 GB. That
  is a property of the harness, not of PRRTE; on a real cluster only the
  per-daemon number would matter. `--max-total-bytes` (2 GB by default)
  guards it, and a skip on that ground says so explicitly.

### Reading the output

Two files. `scale-results-raw.csv` is one row per configuration **per
iteration** — pivot it however you like. `scale-results.csv` is one row per
configuration with the median, min and max over the iterations, and is what
you plot. The median is used rather than the mean because a single
scheduling hiccup on an oversubscribed host skews a mean badly and the run
count is small.

**Interpreting numbers from a laptop.** These containers share one host
kernel and a handful of cores. Once `nodes × ppn` passes the core count,
every daemon and every rank is time-slicing, and the absolute microseconds
say more about the CPU scheduler than about PRRTE. What survives that is the
*shape*: how the cost grows with node count, how a radix-2 tree compares
with a flat one at the same size, and how much of the total is payload
(`data_cost_us`) versus tree latency (`barrier_med_us`). Treat absolute
values as meaningful only against another run on the same host with the same
load.

**A debug build measures itself, not PRRTE.** `build.sh` configures PRRTE with
`--enable-debug`, which is right for the functional suites and wrong for any
number you intend to quote. It configures PMIx with `--prefix` alone, and that
distinction matters more than it looks: `PMIX_ENABLE_DEBUG` is what selects
`PMIX_BFROP_BUFFER_FULLY_DESC`, under which every value packed into a buffer
carries a type descriptor it would not carry in production. A debug PMIx
therefore inflates every launch message, nidmap and fence bucket *and* flatters
their compression ratio, because those descriptors are repetitive. So a
measurement taken here is representative only while PMIx is built without debug
— check with `grep PMIX_ENABLE_DEBUG <build>/src/include/pmix_config.h` before
quoting a size, and remember `PMIX_SRC` builds PMIx from your checkout with
whatever arguments `build.sh` passes.

**And know the noise floor before you go looking for a small effect.** At 40
nodes the collect fence's wall clock has a run-to-run coefficient of variation
of 35–50% over five iterations. Anything under about a third is simply not
measurable here, no matter how many arms the sweep has: an attempt to settle
whether compressing the xcast payload helps (an effect of 1–7%) produced
ON/OFF pairs whose ranges overlapped completely and whose *sign* flipped
between radix 4 and radix 64. If what you are after is small, measure the
mechanism directly — `--prtemca grpcomm_base_verbose 1` reports the size,
ratio and microseconds of every broadcast — rather than hoping it will surface
in an end-to-end fence.

### The payload is a ramp, and that matters for anything about compression

`scaletest`'s default fill is `payload[n] = (rank + n) & 0xff` — a repeating
256-byte ramp. It is a fine stand-in for *moving* bytes, and a terrible one for
*compressing* them: deflate squashes it by roughly 250:1, so any compression
ratio measured on it is an upper bound that no real data approaches.
`--entropy` (`./scaletest.sh run --entropy`) fills from an xorshift stream
seeded per rank instead. That is the opposite extreme — deflate cannot shrink
it at all — and a real modex, which is network endpoints, keys and addresses in
a thin frame of repetitive text, sits much closer to that floor than to the
ramp. Run both and the truth is bracketed.

### Checking a fence still *delivers*, not just that it is fast

`scaletest --verify` reads one key back from two peers each iteration: a
**NEAR** one (rank+1) and a **FAR** one (half the job away). Both, not one,
and the reason is specific: the interesting way for a fence to be wrong is to
deliver *some* of the modex rather than none. Under `--map-by ppr:1:node` the
NEAR peer is the rank the local daemon is most likely to be holding anyway, so
a verify that checked only it would pass with the on-demand direct-modex path
completely broken. The FAR peer is the one that can only come back through
that path.

It stays **off by default**: a get perturbs the collective beside it, and a
run that measures and verifies at once measures neither cleanly. It is also
refused alongside `--neighbors` — see below for why.

### What only the peers you need would cost (`--neighbors`)

The COLLECT column is the price of handing every rank the whole job's data.
`scaletest --neighbors` measures the other end of that trade: after the
barrier — a fence that collected **nothing** — each rank fetches just its two
ring neighbours, serially, and the driver reports it as a fourth phase and a
`neighbors_med_us` column.

Those two gets are answered by **direct modex**: the local daemon does not
hold the peer's data, so it fetches it from the daemon that does. Both halves
of the comparison therefore already exist in PRRTE, which is the point — the
argument Slurm's `PMIX_Ring` makes (pay O(1) for the peers you actually need,
not O(N) for everybody) becomes something measurable here rather than
something to reason about. Nothing in the tree implements a ring; this is the
number that would have to justify building one.

**A `--neighbors` run collects nothing, and its COLLECT column is therefore a
second barrier, not a collect.** That is deliberate and it is what makes the
phase mean anything: with a collect fence left in the same iteration, every
daemon already holds every rank's data, the two gets are local cache hits, and
the phase reports microseconds of nothing. That was the original bug — a
4-node run issued **zero** `DMODX REQ FOR` requests and reported ~6 µs.
So read the NEIGHBORS column from this run and the COLLECT column from a
separate plain run at the same size.

`--verify` and `--neighbors` are **refused together**: verify fetches rank+1,
which is one of the two peers NEIGHBORS times, so running both warms the very
cache the next phase is trying to miss against.

**Check the phase is live before believing a number**, because an inert one
looks fast rather than broken:

```sh
prterun ... --prtemca pmix_server_verbose 2 --leave-session-attached \
    scaletest --neighbors 2>&1 | grep -c "DMODX REQ FOR"
```

It must be `2 * nprocs` (4 ranks fetching two peers each is 8). Left and right
are fetched in that order and never overlapped, so the figure is the
pessimistic serial reading of the pattern.

**What it measures, on 8 nodes at one proc per node** — NEIGHBORS against the
COLLECT of a separate plain run:

| Bytes per rank | COLLECT | NEIGHBORS | ratio |
|---|---|---|---|
| 64 (1 key) | 877 µs | 1000 µs | **1.14** |
| 8 KB (8×1 KB) | 2433 µs | 1455 µs | 0.60 |
| 64 KB (8×8 KB) | 5947 µs | 3660 µs | 0.62 |

At 64 bytes a rank the on-demand path **loses** — two client round trips cost
more than the extra half-kilobyte on the tree. The crossover is real, so any
claim that fetching only what you need is cheaper has to say at what per-rank
size.

## 19. Running a real MPI job (`OMPI_SRC`, `mpinoop`, `ring_c`)

Every other client in this harness puts data because a test told it to. An
**MPI** job publishes what its transports actually need and then reads back
exactly the peers it talks to, which is the only workload here that can judge
whether a fence delivers *enough* rather than merely quickly. That is what
this is for, and it is the check any future change to what the fence
distributes has to survive.

```sh
OMPI_SRC=/path/to/ompi ./build.sh          # or: OMPI_SRC=... ./scaletest.sh build
```

The checkout must be **autogen'd and not configured in-tree** — the same rule
as `PMIX_SRC`, and for the same reason: `configure` runs VPATH over a
read-only bind mount. Open MPI is built `--with-prrte=external` against the
install this script just made, so `mpirun` **is** this PRRTE — an MPI job runs
against whatever you just built rather than against a packaged runtime.
Fortran, OpenSHMEM and sphinx
are disabled: the first two dominate the build time and nothing here uses
them, and the docs build needs python modules the image does not carry — it is
the one part of an Open MPI build that fails for a reason having nothing to do
with MPI.

It is **off by default** because it roughly triples the build. Only the
collective work needs it.

Two probes land in `/opt/prte/ompi/bin`:

- **`mpinoop`** — `MPI_Init`, `MPI_Finalize`, and optionally one phase of
  actual communication. It times `MPI_Init` with `clock_gettime`, **not**
  `MPI_Wtime`, which may not be called before `MPI_Init` — the first version
  of this file did, and reported a negative init time.

  **With no flag it exercises no remote peer at all**, and knowing why is the
  whole point of the file. Open MPI resolves a remote peer the *first time it
  talks to it*: `mpi_add_procs_cutoff` defaults to 0 so the pre-add-everybody
  branch never runs, and `ob1` demands the whole world only when a BTL sets
  `MCA_BTL_FLAGS_SINGLE_ADD_PROCS` (TCP does that only with more than one
  interface plus threads). So a program that calls `MPI_Init` and exits never
  touches a peer, and it cannot tell two ways of distributing the modex apart.

  `--ring` (exchange with the two neighbours) and `--all` (explicit
  point-to-point with **every** other rank) are what actually separate them,
  and they bracket the range: the cheap pattern a real application produces
  and the adversarial one for on-demand retrieval.
  `--all` is deliberately not `MPI_Alltoall`: a tuned alltoall on one int runs
  Bruck and contacts about log2(N) partners, so it understates the case badly.
  Each phase is timed twice — first touch, then a repeat with every peer
  already resolved — so resolution cost is isolated rather than inferred.

  **To count modex traffic you must turn Open MPI's collecting fence off
  first**, or the probe cannot see anything:

  ```sh
  OMPI_MCA_pmix_base_collect_data=0 mpirun ... mpinoop --all
  ```

  With the default (`=1`), `MPI_Init`'s fence collects, every daemon already
  holds every rank's data, and a get about any peer is a local hit. The
  `DMODX REQ FOR` count is then flat at **7** across all three modes at 8
  ranks over 8 nodes — and those seven are not first touch at all, they are
  one request per non-master daemon for **rank 0**'s `pml.base.2.0` from PML
  selection. A run that reports the same number under `--ring` and `--all` is
  telling you the fence collected, not that first touch is free.

  With `=0`, the on-demand path is live and the counts are exact:

  | mode | DMODX | touch | repeat |
  |---|---|---|---|
  | baseline | 0 | 0.000 ms | 0.000 ms |
  | `--ring` | 32 | 9.583 ms | 0.027 ms |
  | `--all` | 56 | 6.343 ms | 0.019 ms |

  The arithmetic is *distinct peers each rank touches × nprocs*, and it checks
  out exactly. For `--all` that is 7 peers × 8 = 56. For `--ring` it is **4**,
  not 2: rank 1 fetches `{0,2}` for the ring **union** `{0,3,5}` for the
  barrier's recursive-doubling partners — 4 distinct peers × 8 = 32. That is
  the barrier contribution the header comment warns about, visible in the
  numbers. The key being fetched is `btl.tcp.6.1`, the BTL endpoint blob.
- **`ring_c`** — Open MPI's own example, compiled from the mounted checkout. A
  real neighbour exchange, and the honest version of what `--ring` simulates.

The Open MPI knobs worth knowing when driving these:

| MCA parameter | effect |
|---------------|--------|
| `pmix_base_async_modex` | skip the modex fence in `MPI_Init` entirely; peers are resolved on first use |
| `pmix_base_collect_data` | whether the `MPI_Init` fence sets `PMIX_COLLECT_DATA`. **Set this to 0 for any measurement of on-demand retrieval** — at the default the fence hands every daemon everything and there is no on-demand path left to measure |
| `mpi_add_procs_cutoff` | above this job size, do not add every peer at init |

Those three are the application-side half of the same argument — what the
*client* is willing to defer — and any daemon-side change to what the fence
distributes should be checked against at least `ring_c` before it is believed.
A design that only looks good against `scaletest` has been measured against a
workload that reads exactly what a test told it to put.

**Counting direct-modex requests needs `--leave-session-attached`.** The
`DMODX REQ FOR` traces come out on each `prted`'s stderr, and without that
flag only the master daemon's reach you — so the count is a fraction of the
DVM's and looks reassuringly small. Measured on the same 8-node `--all` run:
**7 without the flag, 56 with it.** Note the trap doubles up — 7 is also
exactly what a *collecting* fence reports, so an under-counted run is
indistinguishable from a correctly-counted one that forgot
`OMPI_MCA_pmix_base_collect_data=0`. Rule out both before trusting a figure.

```sh
prterun ... --prtemca pmix_server_verbose 2 --leave-session-attached \
    /opt/prte/ompi/bin/mpinoop --all 2>&1 | grep -c "DMODX REQ FOR"
```

## 20. Asking a peer where it is (`peerinfo`, `test_pmix`)

A daemon does not have to publish, to its local PMIx server, everything it
knows about every proc of a job. `prte_pmix_lazy_procdata` publishes only
the procs that daemon hosts and derives the rest out of its own job object
when PMIx brings it a request — see
[`src/prted/pmix/AGENTS.md`](../../src/prted/pmix/AGENTS.md), *What a daemon
publishes, and what it derives*.

**Nothing else in this harness asks one rank about another rank.** Every
other client either puts its own data and reads it back, or asks about the
job rather than about a peer, and both are answered out of the local
server's cache without the daemon ever being consulted. So the derivation
could be switched on, the whole suite run green, and the path never once
have executed — which is exactly what happened the first time it was
written, and why the change sat unproven.

Read that as a hole in the coverage and not as a finding about workloads.
PRRTE is not one MPI's runtime; several programming libraries build on it,
and what any of them asks a peer for is not something this tree can know.
The harness can show the derived answer is right and that it is what
answered — it cannot say how often anything wants it.

`peerinfo` is the missing shape: an MPI initialization, in which every rank
looks up where its peers are. Each rank prints one `SELF` line about itself
and one `PEER` line about each of the others, in the same field order:

```
SELF <rank> <fields...>
PEER <myrank> <peerrank> <fields...>
```

so the assertion is a string compare — what rank *r* was told about rank
*p* must be what *p* says about itself. Three things about that:

- **It is the A/B, inside one job.** A `SELF` line is what a proc's own
  daemon published about it, which a daemon does for its own procs either
  way; the `PEER` lines about it are what other daemons derived. Comparing
  two separate *runs* would not work — a second job has a different
  `PMIX_GLOBAL_RANK` offset, so the tables legitimately differ.
- **Run it `--map-by node`, across at least two nodes.** On one node every
  proc is local, nothing is derived, and the case is vacuous. The test
  asserts the span rather than assuming it.
- **`PMIX_RANK` is deliberately not in the compared set.** A process does
  not hold its own rank in its datastore, so a rank asking about itself
  gets `NOT_FOUND` where a rank asking about a peer gets the value. That is
  PMIx, not PRRTE. It is checked separately against the one thing that
  makes it worth checking: the answer has to be the rank that was asked
  about — which is what catches a derivation that walked to the wrong
  entry and produced a whole line of plausible values.

A consistent table still cannot tell the two implementations apart, so a
second case watches the daemon say it did the work:

```sh
prterun --host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node \
    --leave-session-attached --prtemca prte_pmix_server_verbose 2 \
    /opt/prte/prte/bin/peerinfo 2>&1 | grep -c "ANSWERED LOCALLY"
```

It must be **one per remote proc per daemon** — 24 for that command (four
daemons, six remote procs each), *not* one per lookup: PMIx caches the
modex reply, so the second rank on a node asking about the same peer never
reaches the daemon at all. A count equal to the number of lookups would
mean the caching is not working; a count of zero means the path did not
run and everything above it proved nothing. `--leave-session-attached` is
required, and for the same reason it is in §19: a daemonized `prted` has no
stderr to read, so without it only the master's traces come back.

## 21. Client churn against the PMIx server (`pmixloop`, `test_pmix_cycling`, `test_pmix_server_teardown`)

Every other client here connects to its PMIx server once. `pmixloop` cycles
**`PMIx_Init` → collecting fence → bare fence → `PMIx_Finalize`**, hundreds of
times, with a per-rank sleep before the first fence so the ranks are
deliberately skewed — one rank opens cycle N+1 while its peers are still
finalizing cycle N. That is what an MPI Sessions application does, one cycle
per `MPI_Session_init`, and it is the shape that found two distinct bugs:

- **openpmix#4113** — a rank that dropped its socket through `PMIx_Finalize`
  was recorded on the fence tracker's `departed` list even though it had not
  left the accounting, so the next cycle's fence completed without it. The
  ranks drift apart by whole cycles: fences return `PMIX_ERR_PARTIAL_SUCCESS`
  / `LOST_CONNECTION` / `INVALID_ARG`, and eventually a run hangs.
- **openpmix#4112** — a strictly local fence's completion, which only
  thread-shifts, leaves the tracker on `pmix_server_globals.collectives` still
  looking complete and unclaimed, so a second driver completes it again. Two
  handlers release the same tracker and unlink through freed links; the
  corrupted list outlives the event and the abort lands far away, typically as
  an invalid free in `PMIx_server_finalize`.

The two phases assert opposite ends of that: `test_pmix_cycling` reads the
**clients'** tallies, `test_pmix_server_teardown` reads the **server's** exit
status and stderr (under `prterun` the HNP hosts these ranks itself, so it is
the process that aborts).

### Three constraints, each of which silently empties a case

**The machine must be loaded.** This is not a way to make the test faster or
more likely — it is the experiment. Measured against a library with both
fixes removed:

| | 100 iters | 400 iters | 800 iters |
|---|---|---|---|
| idle | passes | passes | passes |
| 3× oversubscribed | **aborts** | aborts | aborts |

Iterations cannot substitute for load. `load_on()` raises `3 × nproc` burners
on node1 — every container shares the one host kernel, so that loads the whole
swarm — and `SWARM_CLEAN` reaps them as a backstop. Delete them to speed the
suite up and both phases keep passing while testing nothing.

**At least two ranks must share a daemon** (`test_pmix_cycling`). The #4113
defect is in one server's own tracker accounting: it takes *two* ranks
finalizing cycle N to satisfy, between them, the expected count of a fence a
faster peer already opened for N+1. At one rank per node there is no such
pair, and the unfixed library passes 5 runs out of 5. Do not rewrite these as
`--map-by node` one-per-host — that is why the second case is 8 ranks over
*two* nodes rather than eight.

**The job must NOT be spread** (`test_pmix_server_teardown`). The #4112 window
exists only for a collective the host never sees — a strictly local fence, the
ordinary case under `fence_localonly_opt`. Give that job a second node and the
fence goes up to PRRTE, `host_called` is set, and the case tests nothing. Note
this is the opposite of the constraint above, which is why the two live in
separate phases rather than sharing a job.

### Confirming they still bite

Both phases were verified by removing the fixes and re-running, which is the
only way to know a passing case is a passing case. Build a PMIx worktree with
the two `if (peer->finalized) return;` guards deleted from
`pmix_server_trk_peer_lost` / `pmix_server_grp_peer_lost` **and** the three
`tracker->completion_fired = true;` assignments deleted from
`pmix_server_op_replies.c`, then `PMIX_SRC=<that tree> ./build.sh`. All six
assertions go red. Both fixes have to come out together: #4124's guard removes
the lost-connection sweep for finalized peers, and that sweep is #4127's second
driver, so with #4124 in place this workload cannot reach #4112 at all.

Mind the `PMIX_SRC` staleness trap when doing that A/B — see §2.
