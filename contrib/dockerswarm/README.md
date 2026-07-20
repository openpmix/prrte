# PRRTE DVM test "swarm"

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

> **One-time cost:** a VPATH build refuses to run while the source tree still
> has an in-tree build, so `build.sh` runs `make distclean` + `./autogen.pl` at
> the repo root the first time. After that your top-level source dir stays
> clean and all builds live in `vpath-linux`/`vpath-macos`. If you were building
> PRRTE in-tree before, your builds now live in `vpath-macos/` instead.

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
./build.sh                 # distclean+autogen (once), build image, build PRRTE
                           #   into the shared volume from your live tree
docker compose up -d       # start prte-node1 .. prte-node10
./run-tests.sh linux       # full suite: prterun, elastic grow/shrink, relay

# ---- native macOS (single host) ----
./build.sh macos           # native VPATH build into <repo>/vpath-macos
./run-tests.sh macos       # build + single-host launch smoke
```

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

**Shrink** (`elastic shrink node3`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, plus a **"PRRTE has lost communication with a remote
daemon"** notice naming the shrunk node — **that notice is expected** (shrink
completion is driven by the targeted daemon's death). The HNP must survive it.

**Relay** (radix-2 deep grow, `run-tests.sh` does this): grown across node2–node9
with `--prtemca prte_rml_radix 2`, the daemon tree is 3–4 deep, so the
`PMIX_DVM_IS_READY` completion fence must relay through intermediate daemons. If
routing/relay is broken the fence hangs to the 60s timeout instead of
completing.

## 7. Cleanup hygiene

`run-tests.sh` cleans up between phases; if you drive things by hand, clear
stale state on **every** node between DVM runs:

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

## 9. Known issue

Re-growing a node **immediately after** shrinking it can fail its TCP
connect-back (`prted` exits 255) — the just-killed daemon/session may not have
fully torn down. Tracked as
[openpmix/prrte#2491](https://github.com/openpmix/prrte/issues/2491). Start a
fresh DVM between grow/shrink cycles; a single grow→shrink cycle on a fresh DVM
is the reliable smoke test.

## 10. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `prte-node1` | node1 | head node (HNP) — start `prte` here, run all tools here |
| `prte-node2`..`prte-node10` | node2..node10 | DVM nodes (grow/shrink targets) |

Network: bridge `dvm`. All nodes mount the shared `prte-build` volume read-only
at `/opt/prte`, where `build.sh` installs PRRTE (`/opt/prte/prte`) and writes
`/opt/prte/env.sh`. To add or remove nodes, copy or delete a service block in
`docker-compose.yml` (and adjust the `seq 1 10` loops to match).
