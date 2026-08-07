# AGENTS.md — the dockerswarm multi-node test harness

Orientation for AI agents and human contributors who need to run PRRTE
across multiple nodes without a cluster. **This directory is the
canonical multi-node harness** — if you are about to hand-craft
containers, tar a source tree into a volume, or patch files inside a
running node, stop: `build.sh` below already does the right thing
(bind-mounts your live working tree into a builder and compiles it
out-of-tree into the shared volume the nodes read).

> **The one exception: a real scheduler.** The SLURM in this harness is
> [`fake-slurm.py`](fake-slurm.py) (§12) — a stand-in control plane, which
> can show that PRRTE issues the right commands and can never show that
> SLURM accepts them. It also supplies no `srun`, so `plm/slurm` is not
> exercised here at all. For that,
> [`contrib/slurmswarm`](../slurmswarm/) is the same harness with an actual
> SLURM installation in the ten containers. Keep the division: anything
> without a `SLURM_` in it belongs *here*, because this harness is faster
> and needs no scheduler.

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
| `groupcon.c` | A bare PMIx client that drives a **group construct/destruct** (`groupcon` in the install): every rank contributes a local cid, asks for a context id, constructs, reads every peer's contribution back, destructs. Drives grpcomm's `grp_release` on daemons that merely *received* the broadcast. See §15. |
| `envspawn.c` | A PMIx client that spawns a child job carrying one of every **envar directive** (`envspawn` in the install): SET/ADD/UNSET/PREPEND/APPEND, pinned to a named host, with the child reporting the environment it actually got into a file on its own node. Those directives have no command-line surface — they arrive only on a spawn request — and `odls` applies them on whichever daemon forks the process, so the child has to land somewhere the parent is not. Drives `prte_odls_base_process_envars`. |
| `slowcat.c` | A deliberately **slow** stdin reader (`slowcat` in the install, no PMIx dependency): copies stdin to a file in small reads with a pause between them, so the daemon feeding it keeps hitting *partial* writes. That is the only way to reach the iof short-write path. |
| `fake-slurm.py` | A stand-in SLURM control plane (`salloc`/`scontrol`/`scancel`) so `ras/slurm`'s elastic modify surface can be exercised. See §12. |
| `scaletest.c` | A PMIx client that **times** a full-data fence and a bare barrier over the whole job (`scaletest` in the install). Not a pass/fail case — a measurement. See §18. |
| `scaletest.sh` | The measurement driver: stands up a swarm of arbitrary size (default 40), sweeps DVM size × procs-per-node × routing radix × payload, and writes a CSV. See §18. |

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

## 12. Faking a SLURM environment (`ras/slurm`)

`ras/slurm` is the only ras component with a full elastic **modify** surface,
and everything past initial discovery is a shell-out: `salloc` to grow,
`scontrol show job <id> --json` to learn what SLURM granted, `scontrol update
job <id> ReqNodeList=…` to shrink one in place, `scancel` to give one back.
None of that runs on a developer machine, so it had no automated coverage at
all — including the ~1000-line JSON parser, which this harness is also the
**only** automated build that even compiles (jansson defaults to off; see
`--with-jansson` in `build.sh`).

[`fake-slurm.py`](fake-slurm.py) supplies the missing scheduler. `build.sh`
installs it into the shared volume as
`/opt/prte/fakeslurm/bin/{salloc,scontrol,scancel}` (it dispatches on
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
      elastic extend 2             # PMIX_ALLOC_EXTEND: salloc, absorb
      fake-slurm audit             # every command PRRTE issued
      fake-slurm args 2001         # the salloc argv it built
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
| `pending_secs` | a new job sits in `PENDING` this long — lets a request be cancelled while in flight, and is what keeps the stub's `salloc` alive long enough to exercise PRRTE's deferred reap (below) |
| `scancel_fail` | `scancel` exits non-zero with far more output than the 256-byte capture buffer holds |
| `bad_json` | `scontrol show job --json` returns unparsable output with exit status 0 |

The suite uses all three: a cancelled pending extend, a failing `scancel`
(whose message must come back truncated and `...`-terminated, and must not
take the HNP down), and malformed JSON. It also asserts a release naming a
hostname with a shell metacharacter is refused before it can reach a command
line.

**The stub's `salloc` blocks, and that is deliberate.** The real one is the
process holding a pending allocation — it announces the job on stderr as soon
as slurmctld has it and only then waits for resources, exiting once they are
granted. PRRTE reads the job ID out of that first line and leaves the child
running, reaping it asynchronously later, so a stub that returned immediately
the way `sbatch` did would never exercise that. `pending_secs` is therefore
the knob that opens the window: while it is non-zero a live `salloc` sits
under the HNP for the whole pending period, and a `scancel` during it makes
the stub report the allocation revoked and exit non-zero, which is the reap
callback's failure branch. That callback also completes the extend — PRRTE does
not poll a queued job — so a stub `salloc` that exits early makes an extend land
before its nodes exist.

**Some slot counts are asserted from `ras_base_verbose` output, not from
the pool.** The verbose line is what the component itself computed, so it
separates a parser error from whatever the launch path later does with the
number. The pool is asserted too, separately — a node whose count came
from the scheduler must still carry that count at mapping time
(`PRTE_NODE_FLAG_SLOTS_GIVEN`).

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
