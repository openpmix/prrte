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
# PRRTE across ten nodes -- launch, IOF, collectives, elastic, the lot -- with
# a fake SLURM control plane standing in for the scheduler.  That fake
# implements exactly the four command forms PRRTE issues, which makes it a
# good regression test for PRRTE's side of the conversation and no test at all
# of the assumption underneath it: that those four forms are what SLURM
# actually accepts, and that what SLURM says back is what the parser expects.
# Every case here exists because it can only be answered by a real scheduler.
# Everything that is not about SLURM belongs in the sibling harness and is
# deliberately absent here.
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
dvm_start() {
    SA 'rm -f /tmp/prte.out' >/dev/null 2>&1
    SA_BG /tmp/prte.out "cd /root && prte $*"
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
    dvm_stop
    cleanup_cluster

    banner "ras/slurm: SLURM's compressed node list expands to exactly those nodes"
    # The one thing the fake scheduler structurally cannot test.  It hands out
    # comma-separated names; SLURM hands out "node[2,4,6]", and ras/slurm has
    # its own parser for that notation.  A non-contiguous list is used on
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
# The fake scheduler cannot reach this component at all: it supplies
# sbatch/scontrol/scancel, and plm/slurm shells out to *srun*, which is a
# launcher, not a control-plane query.  Every DVM in the sibling harness is
# therefore launched over ssh no matter what the ras is told.  These cases are
# the only place PRRTE's SLURM launcher is exercised.
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
# scheduler that can say no: sbatch really queues a job, "scontrol show job
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

test_elastic() {
    local out jid ajid nodes idx n

    if [ "$HAVE_JSON" != 1 ]; then
        banner "ras/slurm: elastic modify surface"
        skp "no usable JSON path -- the whole extend/release surface is unreachable"
        return
    fi

    banner "ras/slurm: PMIX_ALLOC_EXTEND submits a real job and absorbs it"
    cleanup_cluster
    ALLOC new --tag dvm --nodes 3 --tasks-per-node 2 >/dev/null 2>&1
    jid=$(ALLOC jobid --tag dvm | tr -d ' \r')
    if ! dvm_start --prtemca prte_elastic_mode 1 --prtemca ras_base_verbose 5; then
        bad "no DVM came up for the elastic phase: $(SA 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        cleanup_cluster; return
    fi
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$ajid" ]; then
        bad "extend was refused: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        printf '    HNP: %s\n' "$(SA 'grep -iE "json|error|slurm" /tmp/prte.out | tail -3' | tr '\n' ' ')"
        dvm_stop; cleanup_cluster; return
    fi
    ok "extend accepted and reported PMIX_ALLOC_ID=$ajid"
    SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
        && ok "SLURM really has a job $ajid (sbatch reached the scheduler)" \
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

    banner "ras/slurm: the parent job's attributes reach the sbatch SLURM ran"
    # The fake scheduler could only show that the arguments were *written*.
    # Here they have to be arguments sbatch accepts -- one it does not is a
    # submission failure, not a mis-formatted string -- and the scheduler's
    # own record of the new job is what is asserted.
    out=$(SQ "scontrol show job $ajid -o" | tr -d '\r')
    echo "$out" | grep -q 'NumNodes=2' \
        && ok "the expander job was submitted for exactly 2 nodes" \
        || bad "wrong node count on job $ajid: $(echo "$out" | tr ' ' '\n' | grep -m1 NumNodes)"
    echo "$out" | grep -q 'Partition=debug' \
        && ok "the parent job's partition was propagated" \
        || bad "partition not propagated: $(echo "$out" | tr ' ' '\n' | grep -m1 Partition)"
    # --exclusive is in the fixed part of the sbatch line, and SLURM records
    # it as whole-node ownership.
    # --exclusive is in the fixed part of the sbatch line, and what it buys is
    # WHOLE nodes: without it SLURM would hand back one CPU per node under
    # cons_tres, and PRRTE would then be sharing machines it believes it owns.
    # Assert the cpu count rather than OverSubscribe=, which is NO by default
    # in this partition and would pass with the flag removed.
    n=$(echo "$out" | tr ' ' '\n' | sed -n 's/^NumCPUs=//p')
    [ -n "$n" ] && [ "$n" -ge 16 ] \
        && ok "the expander job owns whole nodes ($n cpus for 2 nodes -- --exclusive)" \
        || bad "the granted job did not get whole nodes (NumCPUs=$n, want >= 16)"

    banner "ras/slurm: releasing one node resizes the SLURM job in place"
    # Removing SOME of a job's nodes keeps the job and resizes it with
    # "scontrol update job <id> ReqNodeList=<survivors>".  Whether SLURM
    # accepts that on a RUNNING job is exactly the assumption a fake
    # scheduler cannot check, and it is the single most valuable assertion
    # in this file.
    out=$(SA "timeout 180 elastic shrink ${nodes##*,}" 2>&1)
    sleep 6
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "partial release completed (PMIX_DVM_IS_READY)" \
        || bad "partial release never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $(idx_of "${nodes##*,}"))" = 0 ] \
        && ok "daemon gone from the released node (${nodes##*,})" \
        || bad "daemon still running on the released node"
    n=$(SQ "squeue -h -j $ajid -o '%D'" | tr -d ' \r')
    [ "$n" = 1 ] \
        && ok "SLURM resized job $ajid down to its surviving node" \
        || bad "SLURM did not accept the in-place resize (job $ajid still has $n nodes): $(SA 'grep -iE "scontrol|resize|update" /tmp/prte.out | tail -2' | tr '\n' ' ')"
    [ "$(job_nodes "$ajid")" = "${nodes%%,*}" ] \
        && ok "the job kept exactly the node PRRTE named as survivor" \
        || bad "job $ajid holds $(job_nodes "$ajid"), expected ${nodes%%,*}"
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

    banner "ras/slurm: release by node COUNT cancels the newest allocation"
    out=$(SA 'timeout 180 elastic extend 2' 2>&1)
    ajid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -n "$ajid" ]; then
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
    else
        bad "could not re-extend: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    fi

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
    if [ -n "$ajid" ]; then
        ok "the oversized extend queued SLURM job $ajid as PENDING"
        SA 'timeout 90 elastic cancel slow-req' >/dev/null 2>&1
        sleep 8
        SQ "squeue -h -j $ajid -o '%i'" | grep -q "$ajid" \
            && bad "the cancelled request left its pending job queued" \
            || ok "the cancelled request scancelled its pending SLURM job"
        SA 'grep -q "REJECTED\|FAILURE" /tmp/extend.out' \
            && ok "the waiting extend reported failure to its requester" \
            || bad "the cancelled extend never answered: $(SA 'tr "\n" " " < /tmp/extend.out' | tail -c 200)"
    else
        bad "the oversized extend never queued a job: $(SA 'tr "\n" " " < /tmp/extend.out' | tail -c 200)"
    fi
    SA 'pgrep -x prte >/dev/null' && ok "HNP survived the cancellation" \
                                  || bad "HNP died during the cancellation"

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
    dvm_stop
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
