# PRRTE elastic-DVM test "swarm"

A small, self-contained Docker harness for exercising PRRTE's **elastic DVM**
(grow/shrink) behavior — without a real PMIx scheduler — across several
container "nodes". It is the quickest way to bring up a multi-node DVM, drive a
grow and a shrink, and watch the two-phase completion events.

It is **not** a Docker Swarm in the orchestration sense — just ten plain
`ubuntu:24.04` containers on one bridge network, each acting as a DVM node.
"Swarm" is only the nickname. (It was four nodes originally; it was grown to
ten so a shrink can target several daemons on one branch of the radix routing
tree at once — the scenario the collective-shrink-completion work,
[openpmix/prrte#2492](https://github.com/openpmix/prrte/issues/2492), needs.)

> Orientation for an AI agent or new contributor: this directory contains
> everything needed. Read this file top to bottom, then run the Quick start.
> The one thing that will silently waste your time is forgetting
> `--prtemca prte_elastic_mode 1` when starting the DVM — see §3.

---

## 1. What's here

| File | Purpose |
|------|---------|
| `build.sh` | Builds the `prte-elastic:latest` image from **this repo's committed tree** plus a cloned PMIx. Start here. |
| `Dockerfile` | Image recipe: builds PMIx + PRRTE + the `elastic` client into `/usr/local`, sets up passwordless SSH. Driven by `build.sh`. |
| `docker-compose.yml` | Defines the ten nodes `prte-node1`..`prte-node10` on bridge network `dvm`. |
| `elastic.c` | The test client (`/usr/local/bin/elastic` in the image): issues a PMIx allocation request and waits for the phase-two completion event. |

## 2. Prerequisites

- Docker (with `docker compose`) and `git`.
- **Network access during the build** — the Dockerfile clones PMIx and installs
  apt packages.
- Builds and runs as `aarch64` or `x86_64`; the image is arch-neutral.

PMIx is cloned from **master** by default, because the DVM size-change event
codes the client registers for (`PMIX_DVM_IS_READY` / `PMIX_ERR_DVM_MOD`) may
not be in a tagged PMIx release yet. If you build against an older PMIx that
lacks them, PRRTE compiles those completion events out (and `elastic.c` will not
compile) — so stick with master unless you know your PMIx has the codes.

## 3. Quick start

```sh
# from this directory (contrib/dockerswarm/)
./build.sh                 # build prte-elastic:latest (PMIx master + this repo's HEAD)
docker compose up -d       # start prte-node1 .. prte-node10

# convenience: run everything on the head node as root, elastic mode ON
RUN='docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 prte-node1 sh -c'

# 1. start the DVM on node1 -- prte_elastic_mode is REQUIRED (see below)
$RUN 'cd /root && nohup prte --daemonize --prtemca prte_elastic_mode 1 \
        >/tmp/prte.out 2>&1 & sleep 6'

# 2. baseline sanity (only node1 in the DVM so far)
$RUN 'prun --np 1 hostname'

# 3. grow onto node2 + node3, then shrink node3 back out
$RUN 'elastic grow node2:2,node3:2'
$RUN 'elastic shrink node3'

# 4. tear the test DVM down (leaves the containers running)
$RUN 'pterm'
```

Both `grow` and `shrink` should print
`PHASE 2 (completion): received event PMIX_DVM_IS_READY` followed by `SUCCESS`.

> **The flag that bites you.** PRRTE gates *all* of the grow/shrink launch-fence
> and completion-event machinery behind the `prte_elastic_mode` MCA parameter
> (default off). If you start the DVM **without** `--prtemca prte_elastic_mode 1`,
> a grow returns phase-1 SUCCESS and even launches the daemons, but it **never
> completes** — no `PMIX_DVM_IS_READY`, the client times out after 60s, and the
> nodes never wire into the DVM. Always start with the flag.

> **The image is frozen at build-time HEAD.** `build.sh` bakes in *exactly* the
> commit that was `HEAD` when it ran (it uses `git archive`). If your working
> tree has moved on — or you pulled elastic-DVM fixes that postdate the last
> build — the running image is stale and you are testing old code. The classic
> symptom is a grow failing phase-1 with
> `PHASE 1 (acceptance): allocation request returned PMIX_ERR_UNREACH`, with
> **no** `ras`/`plm` verbose output for the request: an image built before the
> no-scheduler grow/shrink support went in still tries to reach a scheduler that
> isn't there. Confirm with `docker inspect --format '{{.Created}}'
> prte-elastic:latest` versus `git log -1 --format=%ci`, and rebuild (§7) if the
> image predates the code you mean to test.

## 4. The `elastic` client

```
elastic grow   <node[:slots],...>   # PMIX_ALLOC_NEW, naming nodes to ADD
elastic shrink <node,...>           # PMIX_ALLOC_RELEASE, naming nodes to REMOVE
```

(`extend` / `release` are accepted aliases.) It registers for both completion
codes, prints the phase-1 allocation response, then waits up to 60s for the
phase-2 event.

A grow uses **`PMIX_ALLOC_NEW`** carrying `PMIX_ALLOC_NODE_LIST` (a new
reservation naming the nodes to add) — **not** `PMIX_ALLOC_EXTEND`, which only
enlarges an existing reservation. With no scheduler attached, PRRTE's `ras/hosts`
component satisfies these requests locally using the real node names, and
`plm/ssh` SSHes to the named nodes to launch their daemons (passwordless SSH is
baked into the image).

## 5. What "success" looks like

**Grow** (`elastic grow node2:2,node3:2`): phase-1 `PMIX_SUCCESS` with a
`pmix.alloc.id`, then phase-2 `PMIX_DVM_IS_READY`, and `prted` now running on
node2 and node3.

> The grown nodes join the **reservation** created by the request, so a plain
> `prun -n 3 --map-by node hostname` still lands only on node1 — its default job
> allocation is node1's base pool, not the reservation. That is the elastic
> pooling model, not a failure. Confirm a grow by `prted` presence on the target
> nodes and the `PMIX_DVM_IS_READY` event, not by plain-prun placement:
>
> ```sh
> for n in 2 3; do docker exec prte-node$n pgrep -ax prted; done
> ```

**Shrink** (`elastic shrink node3`): phase-1 `PMIX_SUCCESS`, then phase-2
`PMIX_DVM_IS_READY`, plus a **"PRRTE has lost communication with a remote
daemon"** notice naming the shrunk node. **That notice is expected** — shrink
completion is driven by the targeted daemon's actual *death* (the comm-failure
path), not by an acknowledgement. The HNP must survive it. Afterward: HNP alive,
node3's `prted` gone, node2's `prted` still alive, `prun -n 1 hostname` still
works.

> **Capturing HNP verbose output.** `prte --daemonize` detaches from stdio, so
> the `>/tmp/prte.out` redirect in the Quick start captures **nothing** — that
> file stays empty no matter how much `--prtemca ..._base_verbose` you add. To
> trace the HNP itself (e.g. `state_base_verbose`, `plm_base_verbose`,
> `ras_base_verbose`), run `prte` in the **foreground** and let Docker background
> it instead of `--daemonize`:
>
> ```sh
> docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
>   prte-node1 sh -c 'cd /root && prte --prtemca prte_elastic_mode 1 \
>     --prtemca plm_base_verbose 5 --prtemca ras_base_verbose 5 >/tmp/prte.out 2>&1'
> ```
>
> Then `docker exec prte-node1 cat /tmp/prte.out` after driving a grow/shrink.
> (`docker exec -d` returns immediately; the process keeps running.)

## 6. Cleanup hygiene (prevents "multiple possible servers")

Between DVM runs, clean stale state on **every** node:

```sh
for n in $(seq 1 10); do
  docker exec prte-node$n sh -c 'pkill -9 -x prted; pkill -9 -x prte;
    rm -rf /tmp/prte.* /tmp/prun.session.*; true'
done
```

A detached `prted --daemonize` **survives an HNP kill** — if you only kill
node1's `prte`, orphan `prted`s linger on the other nodes and the next DVM trips
over stale rendezvous files reporting *"multiple possible servers"*. The `pkill`
loop across all ten nodes is mandatory, not optional. `pterm` is the clean way
to bring a healthy DVM down; still run the loop afterward to be safe.

## 7. Rebuilding after a code change

`build.sh` always rebuilds from your **committed** tree (it uses `git archive`).
After committing a change:

```sh
./build.sh && docker compose up -d --force-recreate
```

For a fast edit/test loop on **uncommitted** PRRTE changes without a full image
rebuild, each running container already has the build tree at `/src/prrte`. Copy
the changed files in and run an incremental build **on all ten nodes** (every
node runs a `prted` that loads the libraries, so an ABI/header change must be
consistent across the DVM):

```sh
ROOT=$(git rev-parse --show-toplevel)
FILES="src/mca/ras/ras.h src/mca/ras/base/base.h \
       src/mca/ras/base/ras_base_allocate.c src/mca/errmgr/dvm/errmgr_dvm.c"
for n in $(seq 1 10); do
  for f in $FILES; do docker cp "$ROOT/$f" "prte-node$n:/src/prrte/$f"; done
  docker exec prte-node$n sh -c \
    'cd /src/prrte && make -j"$(nproc)" && make install && ldconfig'
done
```

PMIx is untouched by PRRTE-only edits, so you never rebuild PMIx for a PRRTE
change.

## 8. Known issue

Re-growing a node **immediately after** shrinking it can fail its TCP
connect-back (`prted` exits 255) — the just-killed daemon/session may not have
fully torn down — and the HNP does not always survive that cleanly. Tracked as
[openpmix/prrte#2491](https://github.com/openpmix/prrte/issues/2491). Start a
fresh DVM between grow/shrink cycles to avoid it. A single grow→shrink cycle on
a fresh DVM is the reliable smoke test.

## 9. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `prte-node1` | node1 | head node (HNP) — start `prte` here, run all tools here |
| `prte-node2` | node2 | DVM node (grow target) |
| `prte-node3` | node3 | DVM node (grow/shrink target) |
| `prte-node4`..`prte-node10` | node4..node10 | DVM nodes (grow/shrink targets, spares) |

Network: bridge `dvm`. To add or remove nodes, copy or delete a service block in
`docker-compose.yml` (and adjust the `seq 1 10` loops above to match).

A single `elastic grow node2:2,node3:2,node4:2,node5:2,node6:2,node7:2,node8:2,node9:2`
on a fresh DVM builds a nine-daemon radix tree with real depth — the shape you
want for exercising a single-branch, multi-daemon shrink (#2492).
