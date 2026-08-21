#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Run the PRRTE-under-real-SLURM test suite against a build produced by
# build.sh.
#
#   ./build.sh && docker compose up -d && ./run-tests.sh
#
# Prints PASS/FAIL per test and a summary; exits non-zero if anything failed.
#
# What this suite is for, and what it is NOT for.  contrib/dockerswarm covers
# PRRTE across ten nodes -- launch, IOF, collectives, elastic by hostname, the
# lot -- and has no SLURM in it at all.  Everything SLURM is here, because the
# assumptions that matter can only be answered by a scheduler that can say no:
# that the commands PRRTE issues are ones SLURM accepts, and that what SLURM
# says back is what the parser expects.  Everything that is not about SLURM
# belongs in the sibling harness and is deliberately absent here.
#
# Two things a live slurmctld will not do on request -- record the argv PRRTE
# built, and misbehave -- are supplied by slurm-shim.py, which wraps the real
# commands rather than replacing them.  See AGENTS.md section 14.
#
# Two clones on one host can each run a cluster: set PRTE_SLURM_SWARM (below).

set -uo pipefail

pass=0 fail=0 skip=0
# Which cluster to drive.  Must match the PRTE_SLURM_SWARM that build.sh and
# `docker compose up -d` ran under -- see docker-compose.yml.
PRTE_SLURM_SWARM="${PRTE_SLURM_SWARM:-prteslurm}"
# Reject what docker would reject, before it turns into a confusing compose
# error.  The filter runs through LC_ALL=C tr rather than a shell [a-z] range
# because in a UTF-8 locale that range follows collation order and matches
# 'B' quite happily.
case "$PRTE_SLURM_SWARM" in [_-]*) PRTE_SLURM_SWARM="" ;; esac
if [ -z "$PRTE_SLURM_SWARM" ] || \
   [ "$PRTE_SLURM_SWARM" != "$(printf '%s' "$PRTE_SLURM_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PRTE_SLURM_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi
NODE="$PRTE_SLURM_SWARM-node"           # container names: ${NODE}1 .. ${NODE}10
SWARM_ENV=""
[ "$PRTE_SLURM_SWARM" = prteslurm ] || SWARM_ENV="PRTE_SLURM_SWARM=$PRTE_SLURM_SWARM "
IMAGE="${IMAGE:-prte-slurm-swarm:latest}"

ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

########################################################################
# talking to the cluster
########################################################################

# Run a command on the head node with the PRRTE install on PATH, but with NO
# SLURM allocation in the environment.  This is how you reach the scheduler
# itself (sinfo/squeue/scontrol/slurm-alloc) and how you get a DVM that is
# NOT under SLURM -- both of which the suite needs.
RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "${NODE}1" bash -lc ". /opt/prte/env.sh; $*"; }
# ...and the same on any node.
ON()  { docker exec "$NODE$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }

# Run a command INSIDE a SLURM allocation -- the environment SLURM itself put
# in a process running under `salloc`, captured by slurm-alloc and replayed
# here.  This is the shell every PRRTE tool in this suite runs in, because it
# is the shell a person under SLURM would be typing into.
#
# $ALLOC_TAG selects which allocation; the suite mostly uses the default.
ALLOC_TAG=dvm
SA() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
           "${NODE}1" bash -lc ". /opt/prte/env.sh;
                                eval \"\$(slurm-alloc env --tag $ALLOC_TAG)\"; $*"; }
# Detached, capturing output to a file -- for the DVM itself and for any tool
# that has to stay alive while a case pokes at it.
SA_BG() {
    local outf=$1; shift
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh;
                             eval \"\$(slurm-alloc env --tag $ALLOC_TAG)\"; $* > $outf 2>&1"
}

# slurm-alloc, outside any allocation
ALLOC() { docker exec "${NODE}1" slurm-alloc "$@" 2>&1; }
# the scheduler's own view
SQ()    { docker exec "${NODE}1" bash -lc "$*" 2>&1; }

# --- the recording wrapper around salloc/scontrol/scancel -------------------
# See slurm-shim.py.  It answers the two questions a live slurmctld cannot:
# what PRRTE *asked* for (a job record cannot show an argument's absence), and
# what PRRTE does when the scheduler misbehaves (which slurmctld will not do
# on request).  Everything it does not fault passes through to the real
# command, so the scheduler under test stays real.
SHIM_BIN=/opt/prte/slurmshim/bin
SHIM() { docker exec "${NODE}1" "$SHIM_BIN/slurm-shim" "$@" 2>/dev/null; }
# the argv of the most recent salloc, one argument per line
shim_argv() { SHIM argv | tr -d '\r'; }

########################################################################
# cleanup
########################################################################

# What must not survive into the next case, on one node.  Identical to the
# sibling harness's sweep -- every tool has its OWN session-dir prefix
# (prte.<pid>, prtrn.<pid>, prted.<pid>, prun.<pid>, ompi.<pid>) and each
# holds a pmix.* rendezvous file, so one missed prefix makes the next tool
# report "multiple possible servers" and fail to find the DVM.
SWARM_CLEAN='
    for t in prted prte prterun prun pterm; do pkill -9 -x $t 2>/dev/null; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* \
           /tmp/pmix.* 2>/dev/null
    find /tmp -maxdepth 2 -name "pmix.*" -prune -exec rm -rf {} + 2>/dev/null
    true'

# The extra half that only this harness needs: give the scheduler its nodes
# back.  A PRRTE extend holds its nodes through a SLURM job of its own, and a
# suite that left those standing would starve every later case of the pool it
# needs -- reported, of course, as "the grow did not happen", several cases
# further on.
cleanup_cluster() {
    for n in $(seq 1 10); do
        docker exec "$NODE$n" sh -c "$SWARM_CLEAN" >/dev/null 2>&1
    done
    ALLOC free --all >/dev/null 2>&1
    # A cancelled job does not free its nodes instantly -- slurmctld has to
    # reap the step and put each node back to idle, and an allocation
    # requested before that happens simply waits.  Wait for the cluster to
    # settle instead, so a case that is about to ask for nodes fails for its
    # own reasons.
    for _ in $(seq 30); do
        [ "$(cluster_idle_count)" = 10 ] && break
        sleep 1
    done
}

cluster_idle_count() {
    docker exec "${NODE}1" sh -c "sinfo -h -N -o '%t' 2>/dev/null | grep -c '^idle'" \
        2>/dev/null | tr -d ' \r'
}

# how many nodes of the given list are running a prted
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }
# "node2,node4" -> "2 4"
idx_of() { echo "$1" | tr ',' '\n' | sed 's/^node//' | tr '\n' ' '; }

########################################################################
# starting a DVM inside an allocation
########################################################################

# Start a DVM on node1 in the current allocation.  $* is appended to the prte
# command line.
#
# The DVM runs in the FOREGROUND under `docker exec -d` rather than with
# --daemonize because several cases assert on what the HNP printed -- a
# scheduler error PRRTE captured and reported -- and a daemonized HNP has
# detached from stdio.
# Set to 1 to give the HNP the recording shim (above) ahead of the real SLURM
# commands on its PATH.  It is the HNP that shells out -- the tools never do --
# so this is the only process that needs it.  Always put it back afterwards:
# leaving it on would have every later phase recording, and one of the faults
# possibly still armed.
DVM_SHIM=0

dvm_start() {
    local pre=""
    [ "$DVM_SHIM" = 1 ] && pre="export PATH=$SHIM_BIN:\$PATH; "
    SA 'rm -f /tmp/prte.out' >/dev/null 2>&1
    SA_BG /tmp/prte.out "${pre}cd /root && prte $*"
    for _ in $(seq 25); do
        SA 'pgrep -x prte >/dev/null' && break
        sleep 1
    done
    # `prte` existing is not the same as the DVM being up: the daemons are
    # launched by srun and have to check in.  Wait for a trivial job to run
    # rather than sleeping a fixed amount, which is both slower and less
    # reliable on a loaded laptop.  Bounded on both axes -- a DVM that never
    # forms should cost one case a couple of minutes, not the suite an hour.
    #
    # Each failed probe has to take its own litter with it.  A `prun` killed
    # by that timeout mid-init leaves its prun.<pid> session dir, and the
    # pmix.* rendezvous file inside it, standing -- and the NEXT tool to run
    # then finds two candidate servers and refuses with "PMIx has found
    # multiple possible servers", which surfaces several cases later as
    # "prun failed to initialize, likely due to no DVM being available".
    # That is a harness artifact with a PRRTE-shaped error message, so it is
    # swept here rather than left for a case to trip over.
    for _ in $(seq 20); do
        SA 'timeout 15 prun -n 1 hostname' >/dev/null 2>&1 && return 0
        docker exec "${NODE}1" sh -c \
            'rm -rf /tmp/prun.* 2>/dev/null; true' >/dev/null 2>&1
        sleep 1
    done
    return 1
}

dvm_stop() { SA 'timeout -k 5 30 pterm' >/dev/null 2>&1; }

########################################################################
# preflight
########################################################################

preflight() {
    banner "preflight"
    local n out

    if ! docker ps --format '{{.Names}}' | grep -q "^${NODE}1$"; then
        echo "  no ${NODE}1 container -- run: ${SWARM_ENV}docker compose up -d" >&2
        exit 2
    fi
    n=$(docker ps --format '{{.Names}}' | grep -c "^${NODE}[0-9]*$")
    [ "$n" = 10 ] && ok "all ten node containers are running" \
                  || { bad "only $n/10 node containers are running"; exit 2; }

    # The containers are long-lived while the image is rebuilt under them --
    # and the image is where SLURM and the baked PMIx live, so a container
    # older than its image is running a different cluster than the one you
    # think you configured.
    local cimg iimg
    cimg=$(docker inspect "${NODE}1" --format '{{.Image}}' 2>/dev/null)
    iimg=$(docker images --no-trunc --format '{{.ID}}' "$IMAGE" 2>/dev/null | head -1)
    if [ -n "$iimg" ] && [ "$cimg" != "$iimg" ]; then
        bad "containers predate $IMAGE -- run: ${SWARM_ENV}docker compose up -d --force-recreate"
        exit 2
    fi
    ok "containers are running the current image"

    # The install lives in a volume that outlives any one build, so a build
    # that died part-way leaves the PREVIOUS install standing and every
    # failure reported below would really be "you are testing something else".
    out=$(ON 1 'cat /opt/prte/.build-stamp 2>/dev/null' | tr -d '\r')
    [ -n "$out" ] && ok "build stamp in the volume: $out" \
                  || { bad "no build stamp in the volume -- rerun ./build.sh and check its exit status"; exit 2; }
    RUN 'command -v prte >/dev/null && command -v prun >/dev/null' \
        && ok "prte/prun are on PATH" \
        || { bad "PRRTE tools are not on PATH in the container"; exit 2; }

    # --- the cluster itself ---
    out=$(SQ 'sinfo --version' | tr -d '\r')
    [ -n "$out" ] && ok "SLURM answers: $out" \
                  || { bad "slurmctld is not answering on node1"; exit 2; }
    # Sweep BEFORE asserting the cluster is idle.  Anything hand-driven leaves
    # an allocation behind -- that is what `slurm-alloc new` is for -- and a
    # suite that reported it as a failure would be red for a condition it is
    # about to fix anyway, on its own first line.  What survives the sweep is
    # a real problem: a drained or down node, which no phase here can repair
    # and every phase here would blame on PRRTE.
    cleanup_cluster
    n=$(cluster_idle_count)
    [ "$n" = 10 ] && ok "all ten nodes are idle in SLURM" \
                  || bad "only $n/10 SLURM nodes are idle after a sweep: $(SQ 'sinfo -h -N -o "%N=%t"' | tr '\n' ' ')"

    # PRRTE parses `scontrol show job --json`, which is a build-time option of
    # SLURM's.  If this SLURM cannot serialize, the elastic phases are not
    # testing the parser -- they are testing an error path -- so say so here
    # rather than reporting eight confusing failures later.
    if SQ 'scontrol show config --json >/dev/null 2>&1'; then
        ok "this SLURM can emit JSON (scontrol --json)"
        HAVE_JSON=1
    else
        skp "this SLURM has no JSON serializer -- the elastic phases will skip"
        HAVE_JSON=0
    fi

    # Were the extensions actually built?  Two independent things can switch
    # them off -- no jansson, or a Slurm older than the JSON schema the parser
    # reads -- and configure folds both into one flag.  Read the flag rather
    # than the arguments that produced it: the arguments are a request, and
    # this is the answer.
    out=$(ON 1 'sed -n "s/^#define PRTE_HAVE_SLURM_EXTENSIONS \([01]\).*/\1/p" \
                    /opt/prte/vpath-linux/src/include/prte_config.h 2>/dev/null' | tr -d ' \r')
    case "$out" in
        1) ok "PRRTE was built with the Slurm elastic extensions" ;;
        0) skp "PRRTE was built WITHOUT the Slurm elastic extensions -- $(ON 1 'grep -m1 PRTE_SLURM_VERSION_STRING /opt/prte/vpath-linux/src/include/prte_config.h' | tr -d '\r')"
           HAVE_JSON=0 ;;
        *) skp "cannot tell whether the Slurm extensions were built (no prte_config.h in the build dir)"
           HAVE_JSON=0 ;;
    esac
}

########################################################################
# 1. the cluster itself
########################################################################
#
# Not a PRRTE test.  It runs first because every other phase here blames
# PRRTE for whatever it finds, and a cluster that cannot launch a step of its
# own would make all of them lie.  Keep it to things SLURM does without
# PRRTE's involvement.
test_cluster() {
    local out n jid

    banner "cluster: SLURM launches a step across every node"
    cleanup_cluster
    out=$(SQ 'srun -N10 --ntasks-per-node=1 hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^node[0-9]+$')
    [ "$n" = 10 ] && ok "a 10-node srun step ran on all ten nodes" \
                  || bad "srun reached $n/10 nodes: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "cluster: an allocation can be held and handed back"
    # This is the mechanism the whole suite runs on -- an allocation created
    # in one `docker exec` and used by the next -- so it is asserted rather
    # than assumed.  See slurm-alloc.py.
    jid=$(ALLOC new --tag probe --nodes 3 --tasks-per-node 2 | tail -1)
    if echo "$jid" | grep -qE '^[0-9]+$'; then
        ok "salloc granted job $jid and the allocation is being held"
    else
        bad "could not create an allocation: $jid"
        return
    fi
    out=$(ALLOC env --tag probe)
    # SLURM_JOBID is the variable both ras/slurm and plm/slurm gate on -- it
    # has been deprecated in favour of SLURM_JOB_ID for years, and if a
    # future release stops setting it, PRRTE stops recognizing SLURM at all
    # and every case here quietly falls back to the ssh launcher and passes.
    echo "$out" | grep -q "^export SLURM_JOBID=" \
        && ok "SLURM still sets SLURM_JOBID (what PRRTE gates on)" \
        || bad "SLURM_JOBID is not set -- PRRTE will not detect this scheduler at all"
    echo "$out" | grep -q "^export SLURM_TASKS_PER_NODE=" \
        && ok "SLURM sets SLURM_TASKS_PER_NODE (the slot count PRRTE reads)" \
        || bad "SLURM_TASKS_PER_NODE is not set: $(echo "$out" | tr '\n' ' ')"
    ALLOC free --tag probe >/dev/null 2>&1
    for _ in $(seq 20); do [ "$(cluster_idle_count)" = 10 ] && break; sleep 1; done
    [ "$(cluster_idle_count)" = 10 ] \
        && ok "cancelling the allocation returned every node to the pool" \
        || bad "nodes did not return to idle: $(SQ 'sinfo -h -N -o "%N=%t"' | tr '\n' ' ')"

    banner "cluster: the JSON schema PRRTE parses is the one this SLURM emits"
    # The reason this harness pins a SLURM version.  ras_slurm_jansson.c
    # reads job_resources.nodes.{count,list,allocation}, which is data_parser
    # v0.0.41 and later (SLURM 24.05+).  Against 23.11 the same query returns
    # job_resources.nodes as a plain STRING and every extend fails with
    # "Failed to parse input JSON" -- a failure that says nothing about
    # PRRTE, so it is worth naming here rather than eight cases later.
    if [ "$HAVE_JSON" != 1 ]; then
        skp "no JSON serializer -- schema check not possible"
        return
    fi
    jid=$(ALLOC new --tag probe --nodes 2 --tasks-per-node 2 | tail -1)
    # Piped into the CONTAINER's python3, not the host's: the host running
    # this script is whatever the developer has, and the harness should not
    # acquire a dependency on it just to read a JSON document.
    out=$(SQ "scontrol show job $jid --json" 2>/dev/null)
    if out=$(printf '%s' "$out" | docker exec -i "${NODE}1" python3 -c '
import json,sys
try:
    jr = json.load(sys.stdin)["jobs"][0]["job_resources"]
except Exception as e:
    print("unreadable: %s" % e); sys.exit(1)
n = jr.get("nodes")
if not isinstance(n, dict):
    print("job_resources.nodes is %s, not an object (SLURM older than 24.05)"
          % type(n).__name__); sys.exit(1)
for k in ("count", "list", "allocation"):
    if k not in n:
        print("job_resources.nodes has no %r" % k); sys.exit(1)
a = n["allocation"][0]
for k in ("name", "cpus", "sockets"):
    if k not in a:
        print("an allocation entry has no %r" % k); sys.exit(1)
if "status" not in a["sockets"][0]["cores"][0]:
    print("a core has no status (PRRTE counts ALLOCATED cores)"); sys.exit(1)
sys.exit(0)' 2>&1); then
        ok "scontrol --json emits the job_resources shape ras/slurm parses"
    else
        bad "JSON schema mismatch: $out"
        HAVE_JSON=0
    fi
    ALLOC free --all >/dev/null 2>&1
}

########################################################################
# 2. ras/slurm: what an allocation means to the DVM
########################################################################
#
# The unit test covers the half of ras/slurm that only reads the environment,
# with the environment hand-written.  What it cannot do is write that
# environment the way SLURM writes it.  Every case below turns on a value
# this harness did not invent: the node list in SLURM's own compressed
# notation, the task count in SLURM's own "N(xM)" notation, and the fact that
# the head node need not be in the allocation at all.
test_ras_alloc() {
    local out n

    banner "ras/slurm: the allocation forms the whole DVM, with no --host"
    cleanup_cluster
    ALLOC new --tag dvm --nodes 3 --tasks-per-node 2 >/dev/null 2>&1
    if ! dvm_start --prtemca ras_base_verbose 5; then
        bad "no DVM came up in a 3-node allocation: $(SA 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        cleanup_cluster; return
    fi
    # node1 runs the HNP, so the daemons to count are on the other two
    [ "$(prted_count 2 3)" = 2 ] \
        && ok "daemons launched on every allocated node, with no --host given" \
        || bad "the allocation did not form the DVM ($(prted_count 2 3)/2 daemons)"
    out=$(SA 'timeout 60 prun -n 3 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 3 ] && ok "a job maps across the whole allocation" \
                 || bad "job did not spread over the allocation ($n/3 distinct): $(echo "$out" | tr '\n' ' ')"

    banner "ras/slurm: the slot count SLURM gave is the slot count PRRTE uses"
    # SLURM_TASKS_PER_NODE arrives as "2(x3)".  A node whose count came from
    # the scheduler must not be re-sized from its core count -- these
    # containers claim 8 cores, so the failure would turn 2 slots into 8 and
    # hide every oversubscription.
    out=$(SA 'timeout 30 prun --display allocation -n 1 hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^[[:space:]]*node[0-9]+:[[:space:]]+slots=2[[:space:]]')
    [ "$n" = 3 ] && ok "all three nodes kept slots=2 from SLURM_TASKS_PER_NODE" \
                 || bad "slot counts were recomputed ($n/3 kept): $(echo "$out" | grep -E 'node[0-9]+:' | tr '\n' ' ')"

    banner "ras/slurm: --host selects within the allocation, and only within it"
    out=$(SA 'timeout 30 prun --host node2 -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node2 ] \
        && ok "--host picks an allocated node" \
        || bad "--host node2 did not run there: $out"
    out=$(SA 'timeout 30 prun --host node9 -n 1 hostname 2>&1 | tr -d "\0"')
    echo "$out" | grep -q 'Missing requested host: node9' \
        && ok "--host outside the allocation is refused, naming the host" \
        || bad "--host node9 was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    # A bare integer is never a hostname: it names the launch id, so "3" has
    # to resolve to the third node of the allocation.  The allocation here is
    # node1-3, so that is node3.
    out=$(SA 'timeout 30 prun --host 3 -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node3 ] \
        && ok "--host 3 resolved to node3 by launch id" \
        || bad "launch-id shorthand did not resolve: $out"
    dvm_stop
    cleanup_cluster

    banner "ras: the allocation has exactly one owner, and it is ras/slurm"
    # The only place this is observable.  In an unmanaged environment just one
    # component answers the query, so single- and multi-select look identical;
    # here ras/slurm (50) and ras/hosts (1) both answer and only one may be
    # selected.  It used to keep both, which is what let a request ras/slurm
    # declined fall through to ras/hosts, and ras/hosts claims NEW/EXTEND/
    # RELEASE on the directive alone - so a PMIX_ALLOC_NEW naming hostnames
    # was answered PMIX_SUCCESS with an allocation id minted while SLURM was
    # never asked, and the daemon launch on the un-allocated node then took
    # the whole DVM down.
    cleanup_cluster
    ALLOC new --tag dvm --nodes 3 --tasks-per-node 2 >/dev/null 2>&1
    out=$(SA 'timeout 60 prterun --prtemca ras_base_verbose 5 -n 1 hostname 2>&1')
    echo "$out" | grep -q 'Component: slurm Priority: 50' \
        && ok "ras/slurm is a candidate" \
        || bad "ras/slurm did not answer the query"
    echo "$out" | grep -q 'Component: hosts Priority: 1' \
        && ok "ras/hosts is a candidate too, so the choice is a real one" \
        || bad "ras/hosts did not answer the query"
    echo "$out" | grep -q 'active allocator is .slurm.*scheduler-owned' \
        && ok "slurm wins, and the allocation is marked scheduler-owned" \
        || bad "wrong allocator: $(echo "$out" | grep -i 'active allocator' | tr '\n' ' ')"
    [ "$(echo "$out" | grep -c 'active allocator is')" = 1 ] \
        && ok "exactly one allocator was selected" \
        || bad "more than one allocator selected"

    banner "ras: add-host cannot grow an allocation the scheduler owns"
    # The same authority, reached from the command line instead of a PMIx
    # directive.  This used to insert the node and then kill the DVM trying to
    # launch a daemon on it.
    out=$(SA 'timeout 60 prterun --add-host node9:2 -n 2 hostname 2>&1 | tr -d "\0"')
    echo "$out" | grep -q "is owned by a resource manager" \
        && ok "add-host is refused, and says why" \
        || bad "add-host was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    echo "$out" | grep -qE '^node[0-9]+$' \
        && bad "the job ran anyway after add-host was refused" \
        || ok "the job did not launch on a node SLURM never granted"
    # ...and the refusal is a refusal, not a casualty: launching still works
    out=$(SA 'timeout 60 prterun -n 3 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 3 ] && ok "the allocation still launches after the refusal" \
                 || bad "launching broke after the add-host refusal ($n/3)"

    banner "rmaps: --host cannot claim more of a node than SLURM allocated"
    # The same authority again, this time over how much of a node a job may
    # take rather than which nodes exist.  Outside a scheduler's allocation a
    # ":N" larger than the node re-describes it - the node counts are the
    # user's own - so this arm is only reachable here.  Without the check the
    # job was sized against the larger number and then mapped against the
    # smaller one, and the user got a bare "failed to map" that said nothing
    # about slots.
    out=$(SA 'timeout 60 prterun --host node1:99 -n 99 hostname 2>&1 | tr -d "\0"')
    echo "$out" | grep -q "asked for more slots on a node" \
        && ok "the request is refused, and says which node and how many" \
        || bad "--host over-claim was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    # the same spec within what SLURM gave is still served
    out=$(SA 'timeout 60 prterun --host node1:2,node2:2 -n 4 hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^node[0-9]+$')
    [ "$n" = 4 ] && ok "a --host within the allocation still runs" \
                 || bad "a legal --host stopped working ($n/4)"
    cleanup_cluster

    banner "ras: --activate CAN start a daemon on a node SLURM already granted"
    # The other side of the same authority, and the reason the refusal above
    # is not the whole story.  A DVM started with --host forms across only
    # part of its allocation, so the rest sits in the node pool, up, with no
    # daemon and no way to reach it.  --activate is that way: it names only
    # nodes SLURM has already granted, adds nothing and asks the scheduler
    # for nothing, so the authority add-host lacks it never needs.
    #
    # This can only be shown here.  Everywhere else PRRTE owns the allocation
    # and add-host would have served the same request, so nothing separates
    # "allowed because it adds nothing" from "allowed because we own it".
    cleanup_cluster
    # four nodes, two in the DVM: node3 for the named form and node4 for the
    # hostfile form, so neither is already in the DVM when its case runs
    ALLOC new --tag dvm --nodes 4 --tasks-per-node 2 >/dev/null 2>&1
    if dvm_start --host node1:2,node2:2; then
        [ "$(prted_count 3 4)" = 0 ] \
            && ok "node3 and node4 are allocated but not in the DVM" \
            || bad "they already have daemons - the test premise is gone"
        out=$(SA 'timeout 90 prun --activate node3 --host node3:2 -n 2 --map-by node hostname 2>&1 | tr -d "\0"')
        c=$(echo "$out" | grep -c '^node3$')
        [ "$c" = 2 ] \
            && ok "--activate brought node3 into the DVM under SLURM (2 procs)" \
            || bad "--activate produced $c/2 procs on node3: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        [ "$(prted_count 3)" = 1 ] \
            && ok "a daemon is running on the activated node" \
            || bad "no daemon on the activated node"
        echo "$out" | grep -q "is owned by a resource manager" \
            && bad "--activate was refused as if it were add-host" \
            || ok "scheduler ownership did not refuse it"

        # the same request read from a hostfile, which is the form a user
        # under SLURM actually has to hand: the file that described the
        # allocation. Its slots= must not be applied - PRRTE has no more
        # authority over the slot count than over the node list.
        SA 'printf "node4 slots=99\n" > /tmp/activate.txt' >/dev/null 2>&1
        out=$(SA 'timeout 90 prun --activate file=/tmp/activate.txt --host node4:2 -n 2 --map-by node hostname 2>&1 | tr -d "\0"')
        c=$(echo "$out" | grep -c '^node4$')
        [ "$c" = 2 ] \
            && ok "file= brought node4 in under SLURM too (2 procs)" \
            || bad "file= produced $c/2 procs on node4: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        [ "$(prted_count 4)" = 1 ] \
            && ok "a daemon is running on the node named by the file" \
            || bad "no daemon on the node the file named"
        alloc=$(SA 'timeout 30 prun --display allocation -n 1 hostname 2>&1 | tr -d "\0"')
        echo "$alloc" | grep -qE '^[[:space:]]*node4:[[:space:]]+slots=2' \
            && ok "the file's slots=99 was not applied under SLURM" \
            || bad "activate changed a scheduler-granted slot count: $(echo "$alloc" | grep node4 | tr '\n' ' ')"

        # ...and the limit still holds: what SLURM did not grant, --activate
        # cannot reach either.  It is a different refusal from add-host's -
        # not "you may not enlarge this allocation" but "that is not in it".
        out=$(SA 'timeout 60 prun --activate node9 -n 1 hostname 2>&1 | tr -d "\0"')
        echo "$out" | grep -q 'not part of this DVM' \
            && ok "--activate refuses a node SLURM never granted" \
            || bad "--activate accepted an unallocated node: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        [ "$(prted_count 9)" = 0 ] \
            && ok "no daemon was launched outside the allocation" \
            || bad "a daemon was launched on node9"
        dvm_stop
    else
        bad "could not start a partial DVM for the --activate test"
    fi
    cleanup_cluster

    banner "ras/slurm: SLURM's compressed node list expands to exactly those nodes"
    # SLURM hands out "node[2,4,6]" and ras/slurm has its own parser for that
    # notation -- a scheduler handing out comma-separated names never reaches
    # it, which is why this case can only live here.  A non-contiguous list is used on
    # purpose: a parser that reads "node[2,4,6]" as the range 2..6 gets five
    # nodes, three of which it was never given.
    ALLOC new --tag dvm --nodelist node2,node4,node6 --tasks-per-node 2 >/dev/null 2>&1
    out=$(ALLOC env --tag dvm | sed -n "s/^export SLURM_NODELIST='\(.*\)'$/\1/p")
    case "$out" in
        *\[*) ok "SLURM handed PRRTE the compressed form: $out" ;;
        *)    skp "SLURM did not compress this node list ($out) -- parser not exercised" ;;
    esac
    if dvm_start --prtemca ras_base_verbose 5; then
        n=$(SA 'grep -c "discover: adding node" /tmp/prte.out' 2>/dev/null | tr -d ' \r')
        [ "$n" = 3 ] && ok "the compressed list expanded to exactly 3 nodes" \
                     || bad "expansion produced $n nodes: $(SA 'grep "adding node" /tmp/prte.out' | tr '\n' ' ')"
        for want in node2 node4 node6; do
            SA "grep -q 'adding node $want ' /tmp/prte.out" \
                && ok "$want is in the DVM allocation" \
                || bad "$want missing from the expanded allocation"
        done
        SA 'grep -q "adding node node3 \|adding node node5 " /tmp/prte.out' \
            && bad "a node BETWEEN the named ones was invented by the parser" \
            || ok "no node between the named ones was invented"
        dvm_stop
    else
        bad "no DVM came up on a non-contiguous allocation"
    fi
    cleanup_cluster

    banner "ras/slurm: an allocation that excludes the head node"
    # prte runs on node1, but SLURM allocated node2 and node3 only.  The head
    # node is in the pool -- it always is -- yet is not usable, so a job must
    # map only onto the allocation and naming node1 must be an error rather
    # than a quiet fall-back onto an unallocated machine.  Note this also
    # exercises srun being issued from a host that is not part of the job.
    ALLOC new --tag dvm --nodelist node2,node3 --tasks-per-node 2 >/dev/null 2>&1
    if dvm_start; then
        [ "$(prted_count 2 3)" = 2 ] \
            && ok "daemons launched on the allocation, not on the head node" \
            || bad "head-node-excluded allocation did not form ($(prted_count 2 3)/2)"
        out=$(SA 'timeout 60 prun -n 2 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[23]$' | sort -u | wc -l | tr -d ' ')
        { [ "$n" = 2 ] && ! echo "$out" | grep -qE '^node1$'; } \
            && ok "the job ran only on allocated nodes" \
            || bad "job leaked onto the unallocated head node: $(echo "$out" | tr '\n' ' ')"
        out=$(SA 'timeout 30 prun --host node1 -n 1 hostname 2>&1 | tr -d "\0"')
        echo "$out" | grep -q 'Missing requested host: node1' \
            && ok "--host naming the unallocated head node is refused" \
            || bad "the unallocated head node was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        # "+n#" counts the ALLOCATION from zero, so with the head node left
        # out of it, +n0 is the first node the job was actually given.  A
        # hostfile has to agree with --host about that: the head node still
        # occupies pool slot 0, and the hostfile filter was the one relative
        # -index implementation that did not skip it, so every index it
        # resolved was one node adrift of the same index on the command line.
        # This is why the case wants an allocation that excludes the head
        # node -- with node1 in it, both readings give the same answer.
        out=$(SA 'timeout 30 prun --host +n0 -n 1 hostname 2>&1 | tr -d "\0"' \
                  | grep -E '^node[0-9]+$' | head -1)
        SA 'printf "+n0\n" > /tmp/relhosts.txt'
        n=$(SA 'timeout 30 prun --hostfile /tmp/relhosts.txt -n 1 hostname 2>&1 | tr -d "\0"' \
                  | grep -E '^node[0-9]+$' | head -1)
        { [ "$out" = node2 ] && [ "$n" = node2 ]; } \
            && ok "+n0 is the allocation's first node for both --host and a hostfile" \
            || bad "relative index disagrees across entry points (--host=$out hostfile=$n, want node2 for both)"
        dvm_stop
    else
        bad "no DVM came up on an allocation excluding the head node"
    fi
    cleanup_cluster
}

########################################################################
# 3. plm/slurm: the daemons go out over srun
########################################################################
#
# The sibling harness cannot reach this component at all: it has no SLURM, so
# every DVM over there goes out over ssh no matter what the ras is told.
# These cases are the only place PRRTE's SLURM launcher is exercised.
test_plm() {
    local out n

    banner "plm/slurm: daemons are launched by srun, inside the SLURM job"
    cleanup_cluster
    ALLOC new --tag dvm --nodes 4 --tasks-per-node 2 >/dev/null 2>&1
    local jid; jid=$(ALLOC jobid --tag dvm | tr -d ' \r')
    if ! dvm_start --prtemca plm_base_verbose 5; then
        bad "no DVM came up in a 4-node allocation: $(SA 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        cleanup_cluster; return
    fi
    SA 'grep -q "plm:slurm: LAUNCH DAEMONS CALLED" /tmp/prte.out' \
        && ok "plm/slurm won selection and ran the launch" \
        || bad "plm/slurm did not launch the daemons -- selection fell back to another component"
    out=$(SA 'grep -A2 "final top-level argv" /tmp/prte.out' 2>/dev/null | tr '\n' ' ')
    echo "$out" | grep -q 'srun ' \
        && ok "the launch command is srun" \
        || bad "no srun in the launch command: $(echo "$out" | tail -c 200)"
    # --jobid is what makes the daemon step join THIS allocation rather than
    # queue a new job of its own.  A launcher that omitted it would appear to
    # work on an idle cluster and deadlock on a busy one.
    echo "$out" | grep -q -- "--jobid=$jid" \
        && ok "srun was told to run inside the allocation (--jobid=$jid)" \
        || bad "srun did not carry --jobid=$jid: $(echo "$out" | tail -c 200)"
    [ "$(prted_count 2 3 4)" = 3 ] \
        && ok "a daemon is running on each non-head allocated node" \
        || bad "only $(prted_count 2 3 4)/3 daemons came up"

    banner "plm/slurm: srun hands off and exits, and the daemons outlive it"
    # prted daemonizes by default, so the srun that launched it completes as
    # soon as the fork is done -- and PRRTE has to recognize that exit as a
    # hand-off rather than as a launch failure.  On a real scheduler the step
    # really does end, and SLURM really does reap it; nothing in the sibling
    # harness can show either.
    SA 'grep -q "primary srun exited after daemon hand-off" /tmp/prte.out' \
        && ok "PRRTE recognized the srun exit as a hand-off, not a failure" \
        || bad "no hand-off note from plm/slurm: $(SA 'grep -c srun /tmp/prte.out')"
    SA 'pgrep -x srun >/dev/null' \
        && bad "an srun is still running after the hand-off" \
        || ok "no srun survives the hand-off"
    n=$(SQ "squeue -h -s -j $jid -o '%i'" | grep -c . | tr -d ' ')
    [ "$n" = 0 ] && ok "SLURM has no leftover job step for the daemons" \
                 || bad "$n job step(s) still registered against job $jid"
    out=$(SA 'timeout 60 prun -n 4 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 4 ] && ok "a job runs across the srun-launched DVM" \
                 || bad "job reached $n/4 nodes: $(echo "$out" | tr '\n' ' ')"

    banner "plm/slurm: pterm takes the DVM down and leaves SLURM clean"
    dvm_stop
    sleep 3
    [ "$(prted_count 1 2 3 4)" = 0 ] \
        && ok "pterm left no daemon behind" \
        || bad "$(prted_count 1 2 3 4) daemon(s) survived pterm"
    # The allocation is the harness's, not PRRTE's: taking the DVM down must
    # not cancel it.  A launcher that killed the job would take the user's
    # shell with it on a real cluster.
    SQ "squeue -h -j $jid -o '%T'" | grep -q RUNNING \
        && ok "the SLURM allocation survived the DVM shutdown" \
        || bad "pterm took the SLURM allocation with it"
    cleanup_cluster

    banner "plm/slurm: no allocation means no SLURM launcher"
    # The gate is SLURM_JOBID, and this is the other side of it: on the very
    # same cluster, with no allocation in the environment, PRRTE must fall
    # back to ssh rather than trying to srun into a job it does not have.
    RUN 'rm -f /tmp/prte-nossh.out'
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc '. /opt/prte/env.sh; cd /root &&
            prte --host node1:2,node2:2,node3:2 --prtemca plm_base_verbose 5 \
                 > /tmp/prte-nossh.out 2>&1'
    for _ in $(seq 25); do
        RUN 'timeout 20 prun -n 1 hostname' >/dev/null 2>&1 && break
        sleep 1
    done
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'grep -q "plm:ssh" /tmp/prte-nossh.out' \
            && ok "without SLURM_JOBID the ssh launcher runs instead" \
            || bad "no plm/ssh activity outside an allocation: $(RUN 'grep -m2 "plm:" /tmp/prte-nossh.out' | tr '\n' ' ')"
        out=$(RUN 'timeout 60 prun -n 3 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 3 ] && ok "the ssh-launched DVM works on the same cluster" \
                     || bad "ssh-launched DVM reached $n/3 nodes: $(echo "$out" | tr '\n' ' ')"
        # ...and it must not have quietly consumed scheduler resources
        n=$(SQ "squeue -h -o '%i'" | grep -c . | tr -d ' ')
        [ "$n" = 0 ] && ok "the out-of-band DVM created no SLURM job" \
                     || bad "$n SLURM job(s) appeared for a DVM launched outside SLURM"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "no DVM came up outside an allocation: $(RUN 'tail -3 /tmp/prte-nossh.out' | tr '\n' ' ')"
    fi
    cleanup_cluster
}

########################################################################
# 4. ras/slurm: the elastic modify surface, against the real scheduler
########################################################################
#
# This is the phase the harness exists for.  Every command here goes to a
# scheduler that can say no: salloc really allocates a job, "scontrol show job
# --json" really emits SLURM's own schema (which is why the image pins 24.05
# or newer -- see test_cluster), "scontrol update" really has to be a resize
# SLURM accepts on a RUNNING job, and scancel really removes an allocation.
#
# The request shapes are not the same as a plain elastic grow.  `elastic grow
# <nodes>` is PMIX_ALLOC_NEW naming hosts, which the base serves without ever
# consulting the ras.  ras/slurm's modify() accepts only
# PMIX_ALLOC_EXTEND+NUM_NODES, PMIX_ALLOC_RELEASE+(NODE_LIST|NUM_NODES|
# ALLOC_ID) and PMIX_ALLOC_REQ_CANCEL -- which nodes an extend lands on is
# the scheduler's choice.  Hence `elastic extend|release|release-id|cancel`.
job_nodes() {   # $1 = slurm job id -> comma-separated hostnames
    local nl; nl=$(SQ "squeue -h -j $1 -o '%N'" | tr -d ' \r')
    [ -n "$nl" ] || return 1
    SQ "scontrol show hostnames $nl" | tr -d '\r' | paste -sd, -
}

# Give the scheduler back everything except the allocation the DVM is living
# in.  A group that leaves an extend standing starves the next group of the
# pool it needs, and that is reported as "the grow did not happen" several
# cases later.
# The nodes SLURM could actually grant right now, and how many.
#
# Deliberately not "the idle ones".  A node this cluster has released through
# an in-place resize can sit in state IDLE with its cores still accounted to
# the job that was shrunk off it -- `scontrol show node` reports State=IDLE
# and CPUAlloc=8 together -- and an allocation that names such a node pends on
# Reason=Resources until every job in the queue is gone.  Asking sinfo for the
# free-CPU count as well is what tells the two apart; a case that takes IDLE
# at face value waits out its timeout and then reports the scheduler's
# accounting as a PRRTE failure.
grantable_nodes() {
    SQ "sinfo -h -N -o '%N %t %C'" | tr -d '\r' \
        | awk '$2 == "idle" && substr($3, 1, 2) == "0/" { print $1 }' | sort -u | paste -sd, -
}
grantable_count() {
    local g; g=$(grantable_nodes)
    [ -z "$g" ] && echo 0 || echo "$g" | tr ',' '\n' | grep -c .
}

drop_extra_jobs() {   # $1 = the job id to keep
    local j
    for j in $(SQ "squeue -h -o '%i'" | tr -d ' \r'); do
        [ "$j" = "$1" ] && continue
        SQ "scancel $j" >/dev/null 2>&1
    done
}

# THE PHASE IS A SEQUENCE OF INDEPENDENT GROUPS, AND THAT IS DELIBERATE.
#
# Each group below is its own function so that a group which cannot proceed
# can `return` without taking the others with it.  The shape this replaced did
# exactly that: an extend that came back refused returned out of the whole
# phase, and five later groups -- the counted release, the release during a
# grow, the cancellable pending extend, the malformed JSON, the tainted
# hostname -- silently did not run.  Nothing said so; the suite total simply
# dropped by about fifty checks.  So: a group that gives up says what it gave
# up on, and the caller runs the next one regardless.
test_elastic() {
    local jid

    if [ "$HAVE_JSON" != 1 ]; then
        banner "ras/slurm: elastic modify surface"
        skp "no usable JSON path -- the whole extend/release surface is unreachable"
        return
    fi

    cleanup_cluster
    # --time is what makes the expander-job trim observable: the partition's
    # MaxTime is INFINITE, and a parent with no end has nothing to align to.
    ALLOC new --tag dvm --nodes 3 --tasks-per-node 2 --time 60 >/dev/null 2>&1
    jid=$(ALLOC jobid --tag dvm | tr -d ' \r')
    if ! dvm_start --prtemca prte_elastic_mode 1 --prtemca ras_base_verbose 5; then
        banner "ras/slurm: elastic modify surface"
        bad "no DVM came up for the elastic phase: $(SA 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        skp "the five groups that need a live DVM did not run"
        cleanup_cluster
        return
    fi

    elastic_grant_group "$jid"
    elastic_count_release_group "$jid"
    elastic_reextend_reuse_group "$jid"
    elastic_release_during_grow_group "$jid"
    elastic_pending_cancel_group "$jid"
    elastic_tainted_hostname_group
    dvm_stop
    cleanup_cluster

    # The last two groups need the recording shim in front of the real SLURM
    # commands, and one of them needs different MCA parameters, so each brings
    # up a DVM of its own.
    elastic_argv_group
    elastic_fault_group
}

# The first extend, and everything that can only be asserted about a grant
# that actually happened.
elastic_grant_group() {
    local jid=$1 out ajid nodes idx n p_end a_end

    banner "ras/slurm: PMIX_ALLOC_EXTEND submits a real job and absorbs it"
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$ajid" ]; then
        bad "extend was refused: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        printf '    HNP: %s\n' "$(SA 'grep -iE "json|error|slurm" /tmp/prte.out | tail -3' | tr '\n' ' ')"
        skp "the attribute, in-place resize and release-id cases need a granted allocation"
        drop_extra_jobs "$jid"
        return
    fi
    ok "extend accepted and reported PMIX_ALLOC_ID=$ajid"
    # An extend is answered in two phases now -- phase one carries the id when
    # Slurm grants, PMIX_DVM_IS_READY follows when the daemons are up -- so the
    # completion event is part of the contract, not an optional extra.
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "the extend completed with PMIX_DVM_IS_READY (phase two)" \
        || bad "no phase-two completion for the extend: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
        && ok "SLURM really has a job $ajid (salloc reached the scheduler)" \
        || bad "no SLURM job $ajid exists -- the id PRRTE reported is not a job"
    nodes=$(job_nodes "$ajid"); idx=$(idx_of "$nodes")
    sleep 8
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 2 ] \
        && ok "daemons launched on the nodes SLURM granted ($nodes)" \
        || bad "no daemons on the granted nodes ($nodes) -- the grow never reached them"
    # An extend puts its nodes in the GENERAL pool -- ras/slurm deliberately
    # leaves node->session NULL -- so a plain prun must reach them with no
    # allocation directive at all.
    out=$(SA 'timeout 60 prun -n 5 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 5 ] && ok "a plain prun maps onto the extended allocation (5 nodes)" \
                 || bad "prun did not reach the extended nodes ($n/5 distinct): $(echo "$out" | tr '\n' ' ')"
    # The slot count comes from counting cores whose status is ALLOCATED,
    # capped by cpus.count.  Here SLURM itself decides that number, so this
    # asserts the parser against real core statuses rather than against a
    # fixture written to match it.
    SA "grep -q 'add_modified_resources: discovered node ${nodes%%,*} with .* slots' /tmp/prte.out" \
        && ok "slots for the granted nodes were derived from the job JSON" \
        || bad "no slot derivation logged for ${nodes%%,*}: $(SA 'grep -m3 discovered.node /tmp/prte.out' | tr '\n' ' ')"

    banner "ras/slurm: the parent job's attributes reach the salloc SLURM ran"
    # What the scheduler RECORDED.  The argv that produced it is asserted
    # separately, under the recording shim -- a job record cannot show that an
    # argument was correctly omitted.
    out=$(SQ "scontrol show job $ajid -o" | tr -d '\r')
    echo "$out" | grep -q 'NumNodes=2' \
        && ok "the expander job was submitted for exactly 2 nodes" \
        || bad "wrong node count on job $ajid: $(echo "$out" | tr ' ' '\n' | grep -m1 NumNodes)"
    echo "$out" | grep -q 'Partition=debug' \
        && ok "the parent job's partition was propagated" \
        || bad "partition not propagated: $(echo "$out" | tr ' ' '\n' | grep -m1 Partition)"
    # The reason the expander job is allocated with "salloc --no-shell" rather
    # than submitted with sbatch, and something only a real scheduler records.
    # --exclusive is in the fixed part of the salloc line, and what it buys is
    # WHOLE nodes: without it SLURM would hand back one CPU per node under
    # cons_tres, and PRRTE would then be sharing machines it believes it owns.
    # Assert the cpu count rather than OverSubscribe=, which is NO by default
    # in this partition and would pass with the flag removed.
    n=$(echo "$out" | tr ' ' '\n' | sed -n 's/^NumCPUs=//p')
    [ -n "$n" ] && [ "$n" -ge 16 ] \
        && ok "the expander job owns whole nodes ($n cpus for 2 nodes -- --exclusive)" \
        || bad "the granted job did not get whole nodes (NumCPUs=$n, want >= 16)"
    # The salloc carried the parent's time LIMIT, not what is left of it, so
    # counted from a later start the expander outlives the DVM it was grown
    # for.  ras/slurm resets the limit once SLURM starts the job; whether
    # SLURM accepts that on a RUNNING job only a real scheduler can settle.
    p_end=$(SQ "squeue -h -j $jid -o '%e'" | tr -d ' \r')
    a_end=$(SQ "squeue -h -j $ajid -o '%e'" | tr -d ' \r')
    # ISO-8601 timestamps sort as strings, so no date arithmetic is needed
    if [ -z "$p_end" ] || [ -z "$a_end" ]; then
        bad "no end time for the parent ($p_end) or the expander ($a_end)"
    elif [ "$a_end" \> "$p_end" ]; then
        bad "job $ajid outlives the parent allocation ($a_end > $p_end)"
    else
        ok "the expander job was trimmed to the parent's end ($a_end <= $p_end)"
    fi
    SA "grep -q 'asking for .* what is left of parent job $jid' /tmp/prte.out" \
        && ok "the salloc asked for the parent's remainder, not its whole limit" \
        || bad "no remainder logged: $(SA 'grep -m3 "asking for" /tmp/prte.out' | tr '\n' ' ')"

    banner "ras/slurm: releasing one node resizes the SLURM job in place"
    # Removing SOME of a job's nodes keeps the job and resizes it with
    # "scontrol update job <id> ReqNodeList=<survivors>".  Whether SLURM
    # accepts that on a RUNNING job is exactly the assumption a fake
    # scheduler cannot check, and it is the single most valuable assertion
    # in this file.
    #
    # Release the job's FIRST node deliberately.  That is the node an sbatch
    # expander job ran its script on, and since the script IS the job, it was
    # the one node that could never be released -- so this direction is what
    # allocating with "salloc --no-shell" actually buys, and only a real
    # scheduler can say whether the resize is accepted.
    out=$(SA "timeout 180 elastic shrink ${nodes%%,*}" 2>&1)
    sleep 6
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "partial release completed (PMIX_DVM_IS_READY)" \
        || bad "partial release never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "${nodes%%,*}"))" = 0 ] \
        && ok "daemon gone from the released node (${nodes%%,*})" \
        || bad "daemon still running on the released node"
    n=$(SQ "squeue -h -j $ajid -o '%D'" | tr -d ' \r')
    [ "$n" = 1 ] \
        && ok "SLURM resized job $ajid down to its surviving node" \
        || bad "SLURM did not accept the in-place resize (job $ajid still has $n nodes): $(SA 'grep -iE "scontrol|resize|update" /tmp/prte.out | tail -2' | tr '\n' ' ')"
    [ "$(job_nodes "$ajid")" = "${nodes##*,}" ] \
        && ok "the job kept exactly the node PRRTE named as survivor" \
        || bad "job $ajid holds $(job_nodes "$ajid"), expected ${nodes##*,}"
    # SLURM writes slurm_job_<id>_resize.{sh,csh} into the caller's cwd on a
    # successful shrink for the user to source; PRRTE deletes them.  Their
    # absence only means anything because the resize above really happened.
    [ -z "$(SA 'find / -maxdepth 2 -name "slurm_job_*_resize.*" 2>/dev/null')" ] \
        && ok "the resize helper scripts SLURM left behind were cleaned up" \
        || bad "slurm_job_*_resize.* left behind after a shrink"

    banner "ras/slurm: releasing a whole allocation scancels its SLURM job"
    out=$(SA "timeout 180 elastic release-id $ajid" 2>&1)
    sleep 6
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "release by allocation id completed" \
        || bad "release-id never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 0 ] && ok "the granted nodes left the DVM" \
                                  || bad "a daemon survived the full release"
    SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
        && bad "SLURM job $ajid still exists after the release" \
        || ok "SLURM job $ajid is gone from the queue"
    # the DVM's own allocation must be untouched: it holds the node PRRTE is
    # running on
    SQ "squeue -h -j $jid -o '%T'" | grep -q RUNNING \
        && ok "the DVM's own SLURM allocation was left alone" \
        || bad "the release took out the base allocation"
    out=$(SA 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node1 ] \
        && ok "DVM still responsive after the release" \
        || bad "DVM wedged after the release: $out"
    drop_extra_jobs "$jid"
}

elastic_count_release_group() {
    local jid=$1 out ajid nodes idx

    banner "ras/slurm: release by node COUNT cancels the newest allocation"
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$ajid" ]; then
        bad "could not re-extend: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        skp "the counted release needs an allocation to release"
        drop_extra_jobs "$jid"
        return
    fi
    nodes=$(job_nodes "$ajid"); idx=$(idx_of "$nodes")
    sleep 8
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 2 ] \
        && ok "re-extend brought two more nodes in ($nodes)" \
        || bad "re-extend did not launch daemons on $nodes"
    out=$(SA 'timeout 180 elastic release 2' 2>&1)
    sleep 6
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "release by count completed" \
        || bad "release by count never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 0 ] \
        && ok "the counted release emptied the newest allocation" \
        || bad "a daemon survived the counted release"
    SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
        && bad "count release did not cancel job $ajid" \
        || ok "the whole SLURM job was cancelled rather than resized"
    drop_extra_jobs "$jid"
}

# The shape reported against ras/slurm: extend, release part of what was
# granted, then extend again onto a node that release just gave back.
#
# Only the last step is new.  A grow completes through DAEMONS_REPORTED ->
# VM_READY, which fires when the daemon job's num_procs equals num_reported,
# and neither counter is ever decremented when a daemon departs.  A release
# therefore leaves the two balanced -- the departed daemon had already
# reported -- so the fence the next extend raises comes down only if the
# daemon on the reused node reports in.  A node that comes back carrying
# anything from its previous life (a pool entry the DVM still believes is
# occupied, a retired vpid) never reports, and the extend hangs with nothing
# logged anywhere.  Issue #2491 closed the routing half of this, which the
# sibling harness covers by hostname; here the reuse is the SCHEDULER'S
# choice, and arranging that is what only a real SLURM can do.
#
# Arranging it means leaving the scheduler no alternative: the first extend
# asks for the WHOLE free pool, so that when two of those nodes are released
# they are the only nodes SLURM has left to grant.  Do not shrink that first
# request to a fixed size -- with a spare node anywhere in the cluster the
# scheduler is free to grant one that was never in the DVM, and the case
# asserts nothing while looking like it passed.
elastic_reextend_reuse_group() {
    local jid=$1 out ajid bjid gnodes released reused pool strays forced n

    banner "ras/slurm: an extend that reuses a node an earlier release gave back"
    n=$(grantable_count)
    if [ "$n" -lt 3 ]; then
        skp "SLURM can grant only $n node(s) -- the reuse case needs three"
        return
    fi
    out=$(SA "timeout 240 elastic extend $n" 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$ajid" ]; then
        bad "could not extend onto the free pool ($n nodes): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        skp "the reuse case needs granted nodes to release two of"
        drop_extra_jobs "$jid"
        return
    fi
    gnodes=$(job_nodes "$ajid")
    sleep 8
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "$gnodes"))" = "$n" ] \
        && ok "the whole free pool joined the DVM ($gnodes)" \
        || bad "not every node of $gnodes got a daemon"
    pool=$(grantable_nodes)
    [ -z "$pool" ] \
        && ok "SLURM has nothing left to grant -- a later extend can only come from a release" \
        || skp "SLURM can still grant '$pool' -- a spare node reappeared under the case"

    released="$(echo "$gnodes" | cut -d, -f1),$(echo "$gnodes" | cut -d, -f2)"
    out=$(SA "timeout 180 elastic shrink $released" 2>&1)
    sleep 6
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "the partial release completed ($released)" \
        || bad "the partial release never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "$released"))" = 0 ] \
        && ok "both released nodes gave up their daemons" \
        || bad "a daemon survived on a released node ($released)"
    [ "$(SQ "squeue -h -j $ajid -o '%D'" | tr -d ' \r')" = "$((n - 2))" ] \
        && ok "SLURM resized the granted job down by the two released nodes" \
        || bad "job $ajid still holds $(SQ "squeue -h -j $ajid -o '%D'" | tr -d ' \r') nodes, expected $((n - 2))"
    # Anything the scheduler can still grant has to be put out of its reach,
    # or the extend below is free to land somewhere that proves nothing.
    # Normally there is nothing to do: the extend above took the whole pool.
    # A node can come back into it late, though -- cores an in-place resize
    # left accounted (see grantable_nodes) are freed on slurmctld's own
    # schedule, not PRRTE's -- so park whatever has reappeared.
    pool=$(grantable_nodes)
    strays=$(echo "$pool" | tr ',' '\n' \
             | grep -vxF -e "${released%%,*}" -e "${released##*,}" | paste -sd, -)
    [ -n "$strays" ] && ALLOC new --tag park --nodelist "$strays" --timeout 30 >/dev/null 2>&1
    pool=$(grantable_nodes)
    forced=0
    [ "$(echo "$pool" | tr ',' '\n' | sort | paste -sd, -)" \
      = "$(echo "$released" | tr ',' '\n' | sort | paste -sd, -)" ] && forced=1
    [ "$forced" = 1 ] \
        && ok "the released nodes are the only ones SLURM has left to grant" \
        || skp "SLURM can also grant '$pool' -- this run cannot force the reuse"

    out=$(SA 'timeout 180 elastic extend 1' 2>&1)
    bjid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$bjid" ]; then
        bad "the re-extend was refused: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        skp "the reuse assertions need the re-extend to be granted"
        SA "timeout 180 elastic release-id $ajid" >/dev/null 2>&1
        ALLOC free --tag park >/dev/null 2>&1
        drop_extra_jobs "$jid"
        return
    fi
    # The reported hang reads exactly like this: phase one hands back an
    # allocation id, and the phase-two completion event never arrives.  It is
    # asserted whatever node the scheduler chose -- an extend that does not
    # complete is a bug wherever it landed.
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "the re-extend completed (phase-two event delivered)" \
        || bad "the re-extend never completed -- the reported hang: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    reused=$(job_nodes "$bjid")
    if echo "$released" | tr ',' '\n' | grep -qx "$reused"; then
        ok "SLURM granted back a node the release had freed ($reused)"
    elif [ "$forced" = 1 ]; then
        bad "the re-extend landed on '$reused', which the release did not free"
    else
        skp "the re-extend landed on '$reused' -- the scheduler had a spare, so no reuse happened"
    fi
    sleep 8
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "$reused"))" = 1 ] \
        && ok "a daemon is running on the granted node ($reused)" \
        || bad "no daemon on $reused -- it never joined the DVM"
    # A daemon that checked in is not yet a node the DVM will map onto: a
    # reuse also has to have left one usable pool entry behind, neither the
    # departed one nor two of them.
    out=$(SA "timeout 60 prun --host $reused:1 -n 1 hostname" 2>&1)
    [ "$(echo "$out" | tail -1 | tr -d '\r')" = "$reused" ] \
        && ok "a job maps onto that node" \
        || bad "$reused ran nothing: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(SA 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node1 ] \
        && ok "DVM still responsive afterwards" \
        || bad "DVM wedged after the re-extend: $out"

    # Give it all back through PRRTE rather than the scheduler.  A scancelled
    # node keeps its pool entry in the DVM, and a request PRRTE still believes
    # is outstanding makes the NEXT group's extend fail with PMIX_ERR_EXISTS --
    # which is why every early return above releases before it gives up.
    SA "timeout 180 elastic release-id $bjid" >/dev/null 2>&1
    SA "timeout 180 elastic release-id $ajid" >/dev/null 2>&1
    ALLOC free --tag park >/dev/null 2>&1
    drop_extra_jobs "$jid"
    for _ in $(seq 30); do
        [ "$(cluster_idle_count)" -ge 6 ] && break
        sleep 1
    done
}

elastic_release_during_grow_group() {
    local jid=$1 out settled snode

    banner "ras/slurm: a release issued while a grow is in flight does not wedge the DVM"
    # Issue #2617.  Grow and shrink share one launch fence and one broadcast,
    # and the shrink half had no notion of an in-flight grow: the shrink was
    # xcast to every daemon INCLUDING ones still joining, which cannot be
    # reached, so the campaign could neither complete nor abort.  It then sat
    # on prte_shrink_campaigns forever and every later job parked at the
    # LAUNCH_APPS hold -- a DVM wedged with no error reported to anyone.
    #
    # The interleaving is opportunistic, and deliberately so.  The extend runs
    # in the background and the release is issued without waiting for its
    # daemons to join; if it lands outside the window the case still passes,
    # correctly, having asserted a legal sequence.  Do not "stabilize" this
    # with a sleep between the two requests: the sleep is the bug.  The window
    # here is a wide one, since salloc has to reach slurmctld and wait for the
    # job to start before any daemon can be launched into it.
    out=$(SA 'timeout 180 elastic extend 1' 2>&1)
    settled=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$settled" ]; then
        bad "could not settle a node to release: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        skp "the release-during-grow case needs a settled node to release"
        drop_extra_jobs "$jid"
        return
    fi
    snode=$(job_nodes "$settled")
    sleep 8
    # shellcheck disable=SC2086
    [ -n "$snode" ] && [ "$(prted_count $(idx_of "$snode"))" = 1 ] \
        && ok "a settled node ($snode) is in place to be released" \
        || bad "the settled node never got a daemon (job=$settled node=$snode)"
    # Now grow WITHOUT letting it settle, and release the settled node into
    # that window.  The target is deliberately not one of the joining nodes --
    # a per-target check would pass it, and the campaign would stall anyway on
    # daemons the caller never selected.
    SA 'nohup timeout 240 elastic extend 2 >/tmp/grow-race.out 2>&1 & sleep 1' \
        >/dev/null 2>&1
    out=$(SA "timeout 180 elastic shrink $snode" 2>&1)
    sleep 25
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "the release completed even though a grow was in flight" \
        || bad "release never completed during a grow: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "$snode"))" = 0 ] \
        && ok "the released node's daemon is gone" \
        || bad "the daemon survived a release issued during a grow"
    # the wedge's signature: every later job parks at LAUNCH_APPS and never
    # launches, with no error reported anywhere
    out=$(SA 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node1 ] \
        && ok "a later job still launches (the DVM is not wedged)" \
        || bad "DVM wedged after a release during a grow: $out"
    SA 'grep -q "SUCCESS\|ALLOC_ID" /tmp/grow-race.out' \
        && ok "the concurrent grow was answered too" \
        || bad "the grow was lost: $(SA 'tr "\n" " " < /tmp/grow-race.out' | tail -c 200)"
    drop_extra_jobs "$jid"
    # a cancelled job does not free its nodes at once, and the next group asks
    # for the whole idle pool
    for _ in $(seq 30); do
        [ "$(cluster_idle_count)" -ge 6 ] && break
        sleep 1
    done
}

elastic_pending_cancel_group() {
    local jid=$1 ajid n

    banner "ras/slurm: an extend SLURM cannot satisfy pends, and can be cancelled"
    # No fault injection needed: ask for more nodes than the cluster has free
    # and SLURM queues the job as PENDING, which is the state the cancel path
    # exists for.  PMIX_ALLOC_REQ_CANCEL is served while the extend's poll
    # loop is still waiting, and must scancel the queued job and turn the
    # waiting request into a failure rather than leaving it to time out.
    n=$(cluster_idle_count)
    SA "nohup timeout 240 elastic extend $((n + 2)) --req-id slow-req \
            >/tmp/extend.out 2>&1 & sleep 2" >/dev/null 2>&1
    sleep 8
    ajid=$(SQ "squeue -h -t PENDING -o '%i'" | tr -d ' \r' | head -1)
    if [ -z "$ajid" ]; then
        bad "the oversized extend never queued a job: $(SA 'tr "\n" " " < /tmp/extend.out' | tail -c 200)"
        skp "the cancel cases need a pending job to cancel"
        drop_extra_jobs "$jid"
        return
    fi
    ok "the oversized extend queued SLURM job $ajid as PENDING"
    SA 'timeout 90 elastic cancel slow-req' >/dev/null 2>&1
    sleep 8
    SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
        && bad "the cancelled request left its pending job queued" \
        || ok "the cancelled request scancelled its pending SLURM job"
    SA 'grep -q "REJECTED\|FAILURE" /tmp/extend.out' \
        && ok "the waiting extend reported failure to its requester" \
        || bad "the cancelled extend never answered: $(SA 'tr "\n" " " < /tmp/extend.out' | tail -c 200)"
    SA 'pgrep -x prte >/dev/null' && ok "HNP survived the cancellation" \
                                  || bad "HNP died during the cancellation"
    drop_extra_jobs "$jid"
}

elastic_tainted_hostname_group() {
    local out

    banner "ras/slurm: a node name that is not a hostname never reaches a shell"
    # Every hostname bound for an scontrol/scancel command line goes through
    # prte_ras_slurm_validate_hostname's allowlist first.  This one carries a
    # command separator: it must be refused outright, and nothing may run.
    # The stake is higher against a real scheduler -- the command that would
    # be built here is one a live slurmctld would act on.
    SA 'rm -f /tmp/pwned' >/dev/null 2>&1
    out=$(SA "timeout 90 elastic shrink 'node9;touch /tmp/pwned'" 2>&1)
    echo "$out" | grep -q 'REJECTED' \
        && ok "a release naming a tainted hostname was refused" \
        || bad "tainted hostname not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    SA 'test -e /tmp/pwned' \
        && bad "the injected command RAN -- hostname validation is not holding" \
        || ok "the injected command never ran"
    SA 'pgrep -x prte >/dev/null' && ok "HNP survived the tainted request" \
                                  || bad "HNP died on a tainted hostname"
}

# What PRRTE ASKED the scheduler for.
#
# The scheduler records the job it created, not the command line that created
# it, so three behaviors have no other witness: an argument that must be
# absent (--wrap), an attribute that must be OMITTED rather than sent empty
# when its source is unset, and a propagate_* MCA parameter that must drop
# exactly its own argument and disturb nothing else.  The shim records the
# argv and then becomes the real salloc, so the allocation these assertions
# are about is a real one.
elastic_argv_group() {
    local out ajid args

    banner "ras/slurm: what PRRTE puts on the salloc command line"
    cleanup_cluster
    if ! ON 1 "test -x $SHIM_BIN/slurm-shim"; then
        skp "the recording shim is not in the volume -- rerun ./build.sh"
        return
    fi
    SHIM reset >/dev/null 2>&1
    ALLOC new --tag dvm --nodes 2 --tasks-per-node 2 >/dev/null 2>&1
    DVM_SHIM=1
    if ! dvm_start --prtemca prte_elastic_mode 1; then
        DVM_SHIM=0
        bad "no DVM came up under the recording shim"
        skp "the salloc argv cases need a DVM"
        cleanup_cluster
        return
    fi
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    args=$(shim_argv)
    if [ -z "$args" ]; then
        bad "no salloc was recorded (extend said: $(echo "$out" | tr '\n' ' ' | tail -c 200))"
        skp "every argv assertion needs a recorded salloc"
    else
        for want in --no-shell --exclusive --nodes=2; do
            echo "$args" | grep -qx -- "$want" \
                && ok "salloc carried $want" \
                || bad "salloc missing $want (got: $(echo "$args" | tr '\n' ' '))"
        done
        echo "$args" | grep -qx -- '--partition=debug' \
            && ok "the parent job's partition reached the salloc line" \
            || bad "partition not on the salloc line: $(echo "$args" | tr '\n' ' ')"
        # A batch script IS the job -- it lives exactly as long as it runs --
        # so an sbatch expander job could never release the node its script
        # ran on.  --no-shell runs nothing at all, which is what makes an
        # arbitrary shrink possible; --wrap would undo that.
        echo "$args" | grep -q -- '--wrap' \
            && bad "the expander job is still anchored to a script (--wrap present)" \
            || ok "the expander job runs nothing, so all of its nodes are releasable"
        # An attribute whose source is unset must be dropped, not sent as an
        # empty value.  Stated as "no argument ends in =", which holds however
        # this cluster happens to be configured -- naming a particular field
        # would make the case a fact about slurm.conf.
        echo "$args" | grep -q -- '=$' \
            && bad "an unset attribute was sent with an empty value: $(echo "$args" | grep -- '=$' | tr '\n' ' ')" \
            || ok "an unset attribute is omitted from the salloc line, not sent empty"
    fi
    [ -n "$ajid" ] && SA "timeout 120 elastic release-id $ajid" >/dev/null 2>&1
    dvm_stop
    DVM_SHIM=0
    cleanup_cluster

    banner "ras/slurm: propagate_* MCA params gate what reaches salloc"
    # Each propagated attribute has its own switch; turning one off must drop
    # exactly that argument and leave the rest of the line intact.
    SHIM reset >/dev/null 2>&1
    ALLOC new --tag dvm --nodes 2 --tasks-per-node 2 >/dev/null 2>&1
    DVM_SHIM=1
    if ! dvm_start --prtemca prte_elastic_mode 1 \
                   --prtemca ras_slurm_propagate_partition 0; then
        DVM_SHIM=0
        bad "no DVM came up with the propagate_* overrides"
        cleanup_cluster
        return
    fi
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    args=$(shim_argv)
    if [ -z "$args" ]; then
        bad "no salloc recorded under the propagate_* overrides: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    else
        echo "$args" | grep -q -- '--partition' \
            && bad "propagate_partition=0 still put --partition on the line" \
            || ok "propagate_partition=0 dropped its salloc argument"
        echo "$args" | grep -qx -- '--no-shell' \
            && ok "disabling one attribute left the rest of the line intact" \
            || bad "disabling one attribute disturbed the rest: $(echo "$args" | tr '\n' ' ')"
    fi
    [ -n "$ajid" ] && SA "timeout 120 elastic release-id $ajid" >/dev/null 2>&1
    dvm_stop
    DVM_SHIM=0
    cleanup_cluster
}

# What PRRTE does when the scheduler misbehaves.
#
# A live slurmctld will not emit unparsable JSON or fail a scancel on request,
# and those paths exist precisely for it doing so.  The shim arms exactly one
# fault at a time and passes everything else through, so the rest of the
# conversation is still with the real scheduler.
elastic_fault_group() {
    local out ajid seg

    banner "ras/slurm: unparsable scheduler JSON fails the request, not the DVM"
    cleanup_cluster
    if ! ON 1 "test -x $SHIM_BIN/slurm-shim"; then
        skp "the recording shim is not in the volume -- rerun ./build.sh"
        return
    fi
    SHIM reset >/dev/null 2>&1
    ALLOC new --tag dvm --nodes 2 --tasks-per-node 2 >/dev/null 2>&1
    DVM_SHIM=1
    if ! dvm_start --prtemca prte_elastic_mode 1; then
        DVM_SHIM=0
        bad "no DVM came up under the recording shim"
        skp "the malformed-JSON and scancel-failure cases need a DVM"
        cleanup_cluster
        return
    fi
    # The HNP runs this parser, so a malformed scontrol response must come
    # back as a refused request rather than a dead DVM.
    SHIM set bad_json 1 >/dev/null 2>&1
    out=$(SA 'timeout 120 elastic extend 1' 2>&1)
    SHIM set bad_json 0 >/dev/null 2>&1
    echo "$out" | grep -q 'REJECTED' \
        && ok "an extend on unparsable JSON was refused" \
        || bad "malformed scheduler JSON was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    SA 'pgrep -x prte >/dev/null' && ok "HNP survived the malformed JSON" \
                                  || bad "HNP died parsing malformed JSON"
    drop_extra_jobs "$(ALLOC jobid --tag dvm | tr -d ' \r')"

    banner "ras/slurm: a failing scancel is reported, truncated, and survived"
    # prte_ras_slurm_drain_cmd_output() captures the scheduler's complaint
    # into a fixed buffer and marks it "..." when it does not fit.  The shim's
    # scancel fails with far more output than that buffer holds.
    out=$(SA 'timeout 180 elastic extend 1' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$ajid" ]; then
        bad "could not submit the job for the scancel-failure case: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        skp "the scancel-failure assertions need an allocation to release"
    else
        SHIM set scancel_fail 1 >/dev/null 2>&1
        SA "timeout 180 elastic release-id $ajid" >/dev/null 2>&1
        sleep 6
        SHIM set scancel_fail 0 >/dev/null 2>&1
        seg=$(SA "awk '/failed to kill job/,0' /tmp/prte.out")
        echo "$seg" | grep -q "failed to kill job $ajid" \
            && ok "the scancel failure was reported with the scheduler's text" \
            || bad "a failing scancel went unreported: $(SA 'tail -3 /tmp/prte.out' | tr '\n' ' ')"
        { echo "$seg" | grep -q '\.\.\.' && ! echo "$seg" | grep -q '(line 20)'; } \
            && ok "the oversized scheduler message was truncated with '...'" \
            || bad "scheduler output was not truncated: $(echo "$seg" | tr '\n' ' ' | tail -c 200)"
        SA 'pgrep -x prte >/dev/null' && ok "HNP survived a scancel failure" \
                                      || bad "HNP died when scancel failed"
    fi
    dvm_stop
    DVM_SHIM=0
    cleanup_cluster
}

########################################################################
# 5. running work in the allocation
########################################################################
#
# A short launch smoke test, deliberately short: everything about mapping,
# IOF, collectives and the rest is the sibling harness's subject and is not
# repeated here.  What these cases add is that the ordinary paths still work
# when the DVM was built out of a scheduler allocation rather than a --host
# list -- the node objects come from a different producer, and that is enough
# to break placement and I/O without breaking startup.
test_launch() {
    local out n

    banner "launch: work runs across an allocation-built DVM"
    cleanup_cluster
    ALLOC new --tag dvm --nodes 4 --tasks-per-node 2 >/dev/null 2>&1
    if ! dvm_start; then
        bad "no DVM came up for the launch phase"
        cleanup_cluster; return
    fi
    out=$(SA 'timeout 60 prun -n 8 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^node[0-9]+$')
    [ "$n" = 8 ] && ok "8 ranks across 4 allocated nodes all reported" \
                 || bad "only $n/8 ranks reported: $(echo "$out" | tr '\n' ' ')"
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 4 ] && ok "the ranks were spread over all four nodes" \
                 || bad "ranks landed on $n/4 nodes"

    banner "launch: stdout comes back from every node, and status with it"
    # IOF over the wire is the sibling harness's subject; what is asserted
    # here is only that a job whose nodes came from SLURM still forwards.
    out=$(SA 'timeout 60 prun -n 4 --map-by node bash -c "echo OUT-\$PMIX_RANK-\$(hostname)"' 2>&1)
    n=$(echo "$out" | grep -c '^OUT-')
    [ "$n" = 4 ] && ok "output from all four ranks was forwarded to the tool" \
                 || bad "only $n/4 output lines came back: $(echo "$out" | tr '\n' ' ')"
    SA 'timeout 60 prun -n 2 --map-by node false' >/dev/null 2>&1
    [ $? != 0 ] && ok "a failing job returns non-zero to the submitter" \
                 || bad "a job of /bin/false reported success"

    banner "launch: oversubscribing the allocation is refused"
    # The slot count is SLURM's (2 a node), and a request past it must be
    # refused rather than quietly oversubscribing the scheduler's nodes --
    # on a shared cluster that is somebody else's job losing its cores.
    out=$(SA 'timeout 60 prun -n 40 hostname 2>&1 | tr -d "\0"')
    echo "$out" | grep -qiE 'not enough|no available|already filled|Insufficient' \
        && ok "a job larger than the allocation was refused" \
        || bad "a 40-rank job on 8 slots was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    dvm_stop
    sleep 3
    [ "$(prted_count 1 2 3 4)" = 0 ] \
        && ok "pterm left no daemon behind" \
        || bad "$(prted_count 1 2 3 4) daemon(s) survived pterm"
    cleanup_cluster
}

########################################################################
# main
########################################################################

HAVE_JSON=0
preflight
test_cluster
test_ras_alloc
test_plm
test_elastic
test_launch

# Leave the cluster as we found it: no PRRTE processes, no allocations, every
# node idle.  A suite that left an allocation standing would make the NEXT
# run's first phase fail for want of nodes.
cleanup_cluster

printf '\n=== summary ===\n'
printf '  passed: %d\n  failed: %d\n  skipped: %d\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ] || exit 1
exit 0
