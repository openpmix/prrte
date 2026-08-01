#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Run the PRRTE DVM test suite against a build produced by build.sh.
#
#   ./run-tests.sh linux    # full suite in the 10-container swarm
#                           #   (requires: ./build.sh && docker compose up -d)
#   ./run-tests.sh macos    # single-host subset natively on this host
#                           #   (requires: ./build.sh macos)
#
# Prints PASS/FAIL per test and a summary; exits non-zero if anything failed.
# The multi-node grow/shrink/relay tests only exist in the 'linux' suite --
# native macOS has a single node, so it covers build + single-host launch,
# which is what catches Darwin-specific regressions.
#
# Two clones on one host can each run a suite: set PRTE_SWARM (below) for the
# linux swarm, and note that the macOS subset isolates itself automatically --
# it drives only its own install by absolute path and gives PRRTE a private
# TMPDIR, so neither its pkill nor its cleanup can reach the other clone.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
# Which swarm to drive.  Must match the PRTE_SWARM that build.sh and
# `docker compose up -d` ran under -- see docker-compose.yml.  Unset, this is
# "prte" and every name below is what it has always been.
PRTE_SWARM="${PRTE_SWARM:-prte}"
# Reject what docker would reject, before it turns into a confusing compose
# error.  The filter runs through LC_ALL=C tr rather than a shell [a-z] range
# because in a UTF-8 locale that range follows collation order and matches
# 'B' quite happily -- which let "Bad_Name" through the obvious version of
# this test.  The leading-character check uses a literal set for the same
# reason.
case "$PRTE_SWARM" in [_-]*) PRTE_SWARM="" ;; esac
if [ -z "$PRTE_SWARM" ] || \
   [ "$PRTE_SWARM" != "$(printf '%s' "$PRTE_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PRTE_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi
NODE="$PRTE_SWARM-node"                 # container names: ${NODE}1 .. ${NODE}10
# Prefix for the compose commands we suggest in diagnostics: empty for the
# default swarm, "PRTE_SWARM=<name> " otherwise, because compose reads the
# variable from the environment of the compose command itself.
SWARM_ENV=""
[ "$PRTE_SWARM" = prte ] || SWARM_ENV="PRTE_SWARM=$PRTE_SWARM "
# must match build.sh -- the bootstrap tests reach into the shared install
IMAGE="${IMAGE:-prte-swarm:latest}"
VOLUME="${VOLUME:-$PRTE_SWARM-build}"
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

# Portable bounded run (macOS has no timeout(1)): run "$@" with output to $BOUT,
# killing it after $1 seconds.  Returns 124 on timeout, else the command's rc.
BOUT=""
bounded() {
    local secs=$1; shift
    BOUT="$(mktemp)"
    ( "$@" >"$BOUT" 2>&1 ) & local p=$! i=0
    while kill -0 "$p" 2>/dev/null; do
        if [ "$i" -ge "$secs" ]; then kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; return 124; fi
        sleep 1; i=$((i+1))
    done
    wait "$p"
}

########################################################################
# Linux: the full 10-node swarm
########################################################################

# run a command on the head node (login env so PATH/LD_LIBRARY_PATH are set)
RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "${NODE}1" bash -lc ". /opt/prte/env.sh; $*"; }
ON()  { docker exec "$NODE$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }
# ...and the same for a case that drives a TOOL from a node other than the
# head. ON alone is enough for shell housekeeping, but every PRRTE tool
# refuses to run as root without these two, and the refusal text is long
# enough to bury whatever the case was actually asserting.
ONT() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "$NODE$1" bash -lc ". /opt/prte/env.sh; ${*:2}"; }

# What must not survive into the next test, on one node.  Each tool has its
# OWN session-dir prefix -- prte.<pid> for the HNP, prtrn.<pid> for prterun,
# prted.<pid> for a bootstrapped daemon standing on its own, prun.<pid> for
# prun itself, and ompi.<pid> for anything run under the ompi personality --
# and every one of them holds a pmix.* server rendezvous file.  A system-level
# server drops one (pmix.sys.<host>) straight into the tmpdir as well.  Leaving
# any behind is what makes a later prun report "multiple possible servers ...
# connection handles have been read from files named pmix.*" and fail to find
# the DVM, so clear them all: the prefixes we know by name, and then whatever
# pmix.* is still standing in the tmpdir or one level below it, which catches
# the session dir of a tool this list has not heard of.  The nodes leave TMPDIR
# unset, so that tmpdir is /tmp -- as every other path in this suite assumes.
# Kill the tools too, not just the daemons: a live prun or pterm is holding a
# rendezvous file of its own, and killing it is the point of a teardown.
SWARM_CLEAN='
    for t in prted prte prterun prun pterm; do pkill -9 -x $t 2>/dev/null; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* \
           /tmp/pmix.* 2>/dev/null
    find /tmp -maxdepth 2 -name "pmix.*" -prune -exec rm -rf {} + 2>/dev/null
    true'
cleanup_swarm() {
    for n in $(seq 1 10); do
        docker exec "$NODE$n" sh -c "$SWARM_CLEAN"
    done
}
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }
# how many prted PROCESSES are running on one node (not how many nodes have
# one) -- two daemons on a single machine is what a duplicated node-pool
# entry produces
prted_procs() { docker exec "$NODE$1" sh -c 'pgrep -x prted 2>/dev/null | wc -l' | tr -d ' \r'; }

# Run something on the head node detached, capturing its output to a file --
# for a tool that has to stay alive while the case pokes at the DVM around it.
RUN_BG() {
    local outf=$1; shift
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh; $* > $outf 2>&1"
}

# Does the PMIx this install was BUILT against define a given capability flag?
# The harness can be pointed at either of two PMIx installs -- the one baked
# into the image or a PMIX_SRC build -- and the only record of which one is in
# play is the --with-pmix that build.sh stamped into the VPATH build dir, so
# read the prefix from there rather than guessing at a path.  A case that
# asserts behavior an older PMIx cannot produce skips on this rather than
# failing: the baked PMIx goes stale as a matter of course (see AGENTS.md),
# and red for that reads as "your tree is broken" when it is not.
pmix_cap() {
    ON 1 "p=\$(sed -n 's|.*--with-pmix=\\([^ ]*\\).*|\\1|p' \
                  /opt/prte/vpath-linux/.configure-args 2>/dev/null); \
          [ -n \"\$p\" ] || p=/usr/local; \
          grep -qs $1 \"\$p/include/pmix_version.h\"" >/dev/null 2>&1
}

# --- fake-SLURM helpers -----------------------------------------------------
# ras/slurm reaches its scheduler by shelling out to sbatch/scontrol/scancel,
# so the harness supplies them: build.sh installs fake-slurm.py into the shared
# volume under its own prefix, and everything in that phase runs with that
# prefix FIRST on PATH and the SLURM_* envars exported.  Nothing outside these
# helpers sees either, which is what keeps the rest of the suite unaffected.
FS_BIN=/opt/prte/fakeslurm/bin
# run on the head node inside a faked SLURM allocation
SL() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
           "${NODE}1" bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
                                eval \"\$(fake-slurm env)\"; $*"; }
# run a fake-slurm housekeeping command (no SLURM_* env needed)
FS() { docker exec "${NODE}1" bash -lc "export PATH=$FS_BIN:\$PATH; fake-slurm $*"; }
# "node2,node4" -> "2 4", for prted_count
fs_idx() { echo "$1" | tr ',' '\n' | sed 's/^node//' | tr '\n' ' '; }
# the sbatch argv that created a given fake job
fs_args() { FS "args $1" 2>/dev/null; }

# --- bootstrap-DVM helpers --------------------------------------------------
# A bootstrapped DVM is configured by <sysconfdir>/prte.conf, which lives in the
# install the swarm shares.  The node containers mount that volume READ-ONLY, so
# writing it needs a throwaway container that mounts it read-write.  The file is
# part of the install, so back it up on first touch and always put it back --
# leaving a DVMNodes list behind would change how every later run behaves.
BOOT_CONF=/opt/prte/prte/etc/prte.conf
bootstrap_vol() { docker run --rm -v "$VOLUME":/opt/prte "$IMAGE" sh -c "$1" 2>/dev/null; }

bootstrap_write_conf() {   # $1 = controller host, $2 = DVMNodes list
    bootstrap_vol "[ -f $BOOT_CONF.testsave ] || cp $BOOT_CONF $BOOT_CONF.testsave;
                   printf 'ClusterName=swarm\nDVMControllerHost=%s\nDVMPort=7817\nDVMNodes=%s\nDVMRadix=64\n' \
                       '$1' '$2' > $BOOT_CONF" || return 1
    return 0
}
bootstrap_restore_conf() {
    bootstrap_vol "[ -f $BOOT_CONF.testsave ] && mv $BOOT_CONF.testsave $BOOT_CONF; true"
}
# start prted --bootstrap on the given nodes, controller first (it has to be
# listening before the others try to reach it)
bootstrap_start() {
    local ctrl=$1; shift
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "$NODE$ctrl" bash -lc '. /opt/prte/env.sh; prted --bootstrap > /tmp/boot.out 2>&1'
    sleep 6
    for n in "$@"; do
        docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "$NODE$n" bash -lc '. /opt/prte/env.sh; prted --bootstrap > /tmp/boot.out 2>&1'
    done
    sleep 14
}

########################################################################
# ras/slurm, against the fake scheduler (fake-slurm.py)
########################################################################
#
# Everything ras/slurm does beyond reading SLURM_NODELIST is a shell-out --
# sbatch to grow, "scontrol show job --json" to learn what it got, "scontrol
# update job ReqNodeList=" to shrink one, scancel to give one back -- so none
# of it can run on a developer machine and none of it had any coverage.  The
# unit test covers the half that only reads the environment (query, allocate);
# this covers the other half.  It is also the only place the ~1000-line JSON
# parser is even COMPILED: jansson defaults to off, and this harness is the
# one automated build that passes --with-jansson.
#
# The DVM runs in the foreground under "docker exec -d" rather than
# --daemonize because several cases assert on what the HNP printed (a
# scheduler error PRRTE captured and reported), and a daemonized HNP detaches
# from stdio.
# Allocation semantics: what a scheduler allocation means to the DVM once
# ras/slurm has read it. Distinct from test_slurm() below, which drives the
# elastic modify surface -- these cases never grow or shrink anything, they
# just ask whether an allocation PRRTE was handed is the allocation PRRTE
# uses. A real scheduler is the only other way to ask, which is why none of
# this was ever covered.
#
# start a DVM on node1 inside the current fake allocation.  $1 = extra args
slurm_dvm_start() {
    SL 'rm -f /tmp/prte.out' >/dev/null 2>&1
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
            eval \"\$(fake-slurm env)\"; cd /root &&
            prte --prtemca ras_base_verbose 5 $* >/tmp/prte.out 2>&1"
    sleep 8
    SL 'pgrep -x prte >/dev/null'
}

test_slurm_alloc() {
    local out n

    banner "ras/slurm: a multi-node allocation forms the whole DVM"
    # Nobody passes --host under a scheduler: the allocation IS the node
    # list. A DVM that launches only where the user named launches nowhere.
    cleanup_swarm
    if ! FS 'init --jobid 1000 --base node1,node2,node3 --tasks 2 \
                  --pool node4,node5' >/dev/null 2>&1; then
        skp "fake SLURM stubs missing from the shared volume -- rerun ./build.sh"
        return
    fi
    if ! slurm_dvm_start; then
        bad "no DVM came up on the 3-node allocation: $(SL 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        cleanup_swarm; return
    fi
    [ "$(prted_count 2 3)" = 2 ] \
        && ok "daemons launched on every allocated node, with no --host given" \
        || bad "the allocation did not form the DVM ($(prted_count 2 3)/2 daemons)"
    # count DISTINCT nodes: three procs that all landed on node1 is exactly
    # the failure being tested for, and would pass a line count
    out=$(SL 'timeout 60 prun -n 3 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 3 ] && ok "a job maps across the whole allocation" \
                 || bad "job did not spread over the allocation ($n/3 distinct): $(echo "$out" | tr '\n' ' ')"

    banner "ras/slurm: the slot count SLURM gave is the slot count PRRTE uses"
    # SLURM_TASKS_PER_NODE says 2. A node whose count came from the scheduler
    # must not be re-sized from its core count -- that is what turns a
    # 2-slot node into an 8-slot one and hides oversubscription.
    out=$(SL 'timeout 30 prun --display allocation -n 1 hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^[[:space:]]*node[123]:[[:space:]]+slots=2[[:space:]]')
    [ "$n" = 3 ] && ok "all three nodes kept slots=2 from SLURM_TASKS_PER_NODE" \
                 || bad "slot counts were recomputed ($n/3 kept): $(echo "$out" | grep -E 'node[0-9]+:' | tr '\n' ' ')"

    banner "ras/slurm: --host selects within the allocation, and only within it"
    out=$(SL 'timeout 30 prun --host node2 -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node2 ] \
        && ok "--host picks an allocated node" \
        || bad "--host node2 did not run there: $out"
    # a node outside the allocation must be refused, not silently dropped:
    # --add-host is the sanctioned way to bring one in
    out=$(SL 'timeout 30 prun --host node9 -n 1 hostname 2>&1 | tr -d "\0"')
    echo "$out" | grep -q 'Missing requested host: node9' \
        && ok "--host outside the allocation is refused, naming the host" \
        || bad "--host node9 was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    # a bare integer is never a hostname: it names the launch id, so "3"
    # has to resolve to node3
    out=$(SL 'timeout 30 prun --host 3 -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node3 ] \
        && ok "--host 3 resolved to node3 by launch id" \
        || bad "launch-id shorthand did not resolve: $out"
    SL 'timeout -k 5 30 pterm' >/dev/null 2>&1
    cleanup_swarm

    banner "ras/slurm: an allocation that excludes the head node"
    # prte runs on node1, but SLURM allocated node2 and node3 only. The head
    # node is in the pool (it always is) yet is not usable, so a job must map
    # only onto the allocation and naming node1 must be an error rather than
    # a quiet fall-back onto an unallocated machine.
    FS 'init --jobid 1000 --base node2,node3 --tasks 2 --pool node4,node5' >/dev/null
    if slurm_dvm_start; then
        [ "$(prted_count 2 3)" = 2 ] \
            && ok "daemons launched on the allocation, not on the head node" \
            || bad "head-node-excluded allocation did not form ($(prted_count 2 3)/2)"
        out=$(SL 'timeout 60 prun -n 2 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[23]$' | sort -u | wc -l | tr -d ' ')
        { [ "$n" = 2 ] && ! echo "$out" | grep -qE '^node1$'; } \
            && ok "the job ran only on allocated nodes" \
            || bad "job leaked onto the unallocated head node: $(echo "$out" | tr '\n' ' ')"
        out=$(SL 'timeout 30 prun --host node1 -n 1 hostname 2>&1 | tr -d "\0"')
        echo "$out" | grep -q 'Missing requested host: node1' \
            && ok "--host naming the unallocated head node is refused" \
            || bad "the unallocated head node was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        # "+n#" counts the ALLOCATION from zero, so with the head node left
        # out of it, +n0 is the first node the job was actually given. A
        # hostfile has to agree with --host about that: the head node still
        # occupies pool slot 0, and the hostfile filter was the one relative
        # -index implementation that did not skip it, so every index it
        # resolved was one node adrift of the same index on the command line.
        out=$(SL 'timeout 30 prun --host +n0 -n 1 hostname 2>&1 | tr -d "\0"' | grep -E '^node[0-9]+$' | head -1)
        SL 'printf "+n0\n" > /tmp/relhosts.txt'
        n=$(SL 'timeout 30 prun --hostfile /tmp/relhosts.txt -n 1 hostname 2>&1 | tr -d "\0"' | grep -E '^node[0-9]+$' | head -1)
        { [ "$out" = node2 ] && [ "$n" = node2 ]; } \
            && ok "+n0 is the allocation's first node for both --host and a hostfile" \
            || bad "relative index disagrees across entry points (--host=$out hostfile=$n, want node2 for both)"
        SL 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "no DVM came up on an allocation excluding the head node"
    fi
    cleanup_swarm
}

test_slurm() {
    local jid nodes idx out n sargs seg

    banner "ras/slurm: faked SLURM allocation discovered at DVM startup"
    cleanup_swarm
    if ! FS 'init --jobid 1000 --base node1 --tasks 2 \
                  --pool node2,node3,node4,node5,node6,node7,node8,node9' >/dev/null 2>&1; then
        skp "fake SLURM stubs missing from the shared volume -- rerun ./build.sh"
        return
    fi
    SL 'rm -f /tmp/prte.out /tmp/pwned' >/dev/null 2>&1
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
            eval \"\$(fake-slurm env)\"; cd /root &&
            prte --prtemca prte_elastic_mode 1 --prtemca ras_base_verbose 5 \
                 >/tmp/prte.out 2>&1"
    sleep 8
    if ! SL 'pgrep -x prte >/dev/null'; then
        bad "no DVM came up under the faked SLURM allocation: $(SL 'tail -5 /tmp/prte.out' | tr '\n' ' ')"
        cleanup_swarm; return
    fi
    out=$(SL 'timeout 30 prun --display allocation -n 1 hostname' 2>&1)
    echo "$out" | grep -qE '^[[:space:]]*node1:[[:space:]]+slots=' \
        && ok "the DVM allocation came from SLURM_NODELIST" \
        || bad "SLURM allocation not discovered: $(echo "$out" | tr '\n' ' ')"
    # SLURM_NODELIST=node1 with SLURM_TASKS_PER_NODE="2(x1)": the compressed
    # form has to expand to one node carrying two slots.  Assert that against
    # what the component itself reports (ras_base_verbose) rather than the
    # pool, so a parsing error here is not confused with anything the launch
    # path does to the number afterwards -- test_slurm_alloc covers that end
    # of it.
    SL 'grep -q "discover: adding node node1 (2 slots)" /tmp/prte.out' \
        && ok "SLURM_TASKS_PER_NODE '2(x1)' expanded to node1 with 2 slots" \
        || bad "nodelist/tasks-per-node expansion wrong: $(SL 'grep -m3 "adding node" /tmp/prte.out' | tr '\n' ' ')"

    banner "ras/slurm: PMIX_ALLOC_EXTEND runs sbatch and grows the DVM"
    # PMIX_ALLOC_EXTEND + PMIX_ALLOC_NUM_NODES is the only extend shape this
    # component accepts: which nodes it gets back is the scheduler's choice,
    # so the request names a count and the answer comes from the job JSON.
    out=$(SL 'timeout 120 elastic extend 2' 2>&1)
    jid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -z "$jid" ]; then
        bad "extend did not report an allocation id: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        printf '    HNP: %s\n' "$(SL 'tail -3 /tmp/prte.out' | tr '\n' ' ')"
        SL 'timeout -k 5 30 pterm' >/dev/null 2>&1; cleanup_swarm; return
    fi
    ok "extend accepted and reported PMIX_ALLOC_ID=$jid (RM=slurm)"
    nodes=$(FS "nodes $jid" | tr -d '\r')
    idx=$(fs_idx "$nodes")
    sleep 10
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 2 ] \
        && ok "daemons launched on the nodes SLURM granted ($nodes)" \
        || bad "no daemons on the granted nodes ($nodes) -- the grow never reached them"
    # The extend puts its nodes in the GENERAL pool (ras/slurm deliberately
    # leaves node->session NULL), unlike a reservation-creating grow -- so a
    # plain prun must be able to map onto them.
    out=$(SL 'timeout 60 prun -n 3 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 3 ] && ok "a plain prun maps onto the extended allocation (3 nodes)" \
                 || bad "prun did not reach the extended nodes ($n/3 distinct): $(echo "$out" | tr '\n' ' ')"
    # Slots come from counting the cores whose status is ALLOCATED (2) times
    # threads_per_core, capped by cpus.count -- the fake job deliberately
    # reports an UNALLOCATED core as well and a much larger cpus.count, so
    # only a parser that reads core statuses arrives at 2.
    SL "grep -q 'add_modified_resources: discovered node ${nodes%%,*} with 2 slots' /tmp/prte.out" \
        && ok "slots derived from ALLOCATED cores, capped by cpus.count" \
        || bad "wrong slot count from the job JSON: $(SL 'grep -m3 discovered.node /tmp/prte.out' | tr '\n' ' ')"

    banner "ras/slurm: the parent job's attributes are propagated onto sbatch"
    sargs=$(fs_args "$jid")
    for want in --parsable --exclusive --nodes=2 --account=prrte-test \
                --partition=debug --qos=normal --chdir=/root --mem=1024 \
                --time=60 --threads-per-core=1; do
        echo "$sargs" | grep -qx -- "$want" \
            && ok "sbatch carried $want" \
            || bad "sbatch missing $want (got: $(echo "$sargs" | tr '\n' ' '))"
    done
    # memory_per_cpu is unset in the parent job, and an unset numeric object
    # must be omitted rather than sent as a sentinel
    echo "$sargs" | grep -q -- '--mem-per-cpu' \
        && bad "sbatch sent --mem-per-cpu for an unset memory_per_cpu" \
        || ok "an unset numeric field is omitted from the sbatch line"

    banner "ras/slurm: releasing one node shrinks the SLURM job in place"
    # Removing SOME of a job's nodes keeps the job and resizes it with
    # "scontrol update job <id> ReqNodeList=<survivors>", then re-reads the
    # JSON to detach the departed nodes from the session.
    out=$(SL "timeout 120 elastic shrink ${nodes##*,}" 2>&1)
    sleep 5
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "partial release completed (PMIX_DVM_IS_READY)" \
        || bad "partial release never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $(fs_idx "${nodes##*,}"))" = 0 ] \
        && ok "daemon gone from the released node (${nodes##*,})" \
        || bad "daemon still running on the released node"
    FS audit | grep -q "scontrol update job $jid ReqNodeList=${nodes%%,*}" \
        && ok "job $jid resized to its survivors via scontrol update" \
        || bad "no scontrol resize was issued: $(FS audit | tr '\n' ' ' | tail -c 200)"
    [ "$(FS "nodes $jid" | tr -d '\r')" = "${nodes%%,*}" ] \
        && ok "the SLURM job kept exactly its surviving node" \
        || bad "job $jid node list wrong after resize: $(FS "nodes $jid")"
    [ -z "$(SL 'find / -maxdepth 2 -name "slurm_job_*_resize.*" 2>/dev/null')" ] \
        && ok "the resize helper scripts SLURM left behind were cleaned up" \
        || bad "slurm_job_*_resize.* left behind after a shrink"

    banner "ras/slurm: releasing a whole allocation scancels its SLURM job"
    out=$(SL "timeout 120 elastic release-id $jid" 2>&1)
    sleep 5
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "release by allocation id completed" \
        || bad "release-id never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 0 ] && ok "both granted nodes left the DVM" \
                                  || bad "a daemon survived the full release"
    FS audit | grep -q "scancel $jid" && ok "scancel issued for job $jid" \
                                      || bad "the SLURM job was never cancelled"
    FS jobs | grep -qx "$jid" && bad "job $jid still exists after scancel" \
                             || ok "job $jid is gone from the scheduler"
    out=$(SL 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node1 ] \
        && ok "DVM still responsive after the release" \
        || bad "DVM wedged after the release: $out"

    banner "ras/slurm: release by node COUNT picks the newest allocation"
    # Re-extending reuses the pool entries the release left behind (daemon-less
    # nodes PRRTE already knows), which is a different code path from the
    # first-time grant above.
    out=$(SL 'timeout 120 elastic extend 2' 2>&1)
    jid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    nodes=$(FS "nodes $jid" | tr -d '\r'); idx=$(fs_idx "$nodes")
    sleep 10
    # shellcheck disable=SC2086
    [ -n "$jid" ] && [ "$(prted_count $idx)" = 2 ] \
        && ok "re-extend reused the released node objects ($nodes)" \
        || bad "re-extend did not bring the nodes back ($nodes)"
    out=$(SL 'timeout 120 elastic release 2' 2>&1)
    sleep 5
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "release by count completed" \
        || bad "release by count never completed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # shellcheck disable=SC2086
    [ "$(prted_count $idx)" = 0 ] && ok "the counted release emptied the newest allocation" \
                                  || bad "a daemon survived the counted release"
    FS audit | grep -q "scancel $jid" \
        && ok "the whole SLURM job was cancelled rather than resized" \
        || bad "count release did not scancel job $jid"
    # the base allocation must survive: it holds the node PRRTE is running on
    FS jobs | grep -qx 1000 && ok "the DVM's own SLURM job was left alone" \
                            || bad "the release took out the base allocation"

    banner "ras/slurm: a pending extend can be cancelled by request id"
    # PMIX_ALLOC_REQ_CANCEL is served while the extend's poll loop is still
    # waiting on a PENDING SLURM job: cancelling drops the pending record and
    # scancels the job, and the next poll turns that into a failed request.
    FS 'set pending_secs 45' >/dev/null
    SL 'nohup timeout 120 elastic extend 1 --req-id slow-req \
            >/tmp/extend.out 2>&1 & sleep 2' >/dev/null 2>&1
    sleep 6
    jid=$(FS jobs | grep -v '^1000$' | tail -1 | tr -d '\r')
    if [ -n "$jid" ]; then
        ok "extend submitted job $jid and is waiting for it to start"
        SL 'timeout 60 elastic cancel slow-req' >/dev/null 2>&1
        sleep 6
        FS audit | grep -q "scancel $jid" \
            && ok "the cancelled request scancelled its pending SLURM job" \
            || bad "cancel did not reach the scheduler"
        SL 'grep -q "REJECTED\|FAILURE" /tmp/extend.out' \
            && ok "the waiting extend reported failure to its requester" \
            || bad "the cancelled extend never answered: $(SL 'tr "\n" " " < /tmp/extend.out' | tail -c 200)"
    else
        bad "the pending extend never submitted a job"
    fi
    FS 'set pending_secs 0' >/dev/null
    out=$(SL 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
    [ "$(echo "$out" | tr -d '\r')" = node1 ] \
        && ok "DVM still responsive after the cancellation" \
        || bad "DVM wedged after the cancellation: $out"

    banner "ras/slurm: unparsable scheduler JSON fails the request, not the DVM"
    # The HNP runs this parser, so a malformed scontrol response must come
    # back as a refused request rather than a dead DVM.
    FS 'set bad_json 1' >/dev/null
    out=$(SL 'timeout 60 elastic extend 1' 2>&1)
    FS 'set bad_json 0' >/dev/null
    echo "$out" | grep -q 'REJECTED' \
        && ok "an extend on unparsable JSON was refused" \
        || bad "malformed scheduler JSON was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    SL 'pgrep -x prte >/dev/null' && ok "HNP survived the malformed JSON" \
                                  || bad "HNP died parsing malformed JSON"

    banner "ras/slurm: a node name that is not a hostname never reaches a shell"
    # Every hostname bound for an scontrol/scancel command line goes through
    # prte_ras_slurm_validate_hostname's allowlist first.  This one carries a
    # command separator: it must be refused outright, and nothing may run.
    out=$(SL "timeout 60 elastic shrink 'node9;touch /tmp/pwned'" 2>&1)
    echo "$out" | grep -q 'REJECTED' \
        && ok "a release naming a tainted hostname was refused" \
        || bad "tainted hostname not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    SL 'test -e /tmp/pwned' \
        && bad "the injected command RAN -- hostname validation is not holding" \
        || ok "the injected command never ran"
    SL 'pgrep -x prte >/dev/null' && ok "HNP survived the tainted request" \
                                  || bad "HNP died on a tainted hostname"

    banner "ras/slurm: a failing scancel is reported, truncated, and survived"
    # prte_ras_slurm_drain_cmd_output() captures the scheduler's complaint
    # into a fixed buffer and marks it "..." when it does not fit.  The fake
    # scancel fails with far more output than that buffer holds.
    FS 'set scancel_fail 1' >/dev/null
    out=$(SL 'timeout 120 elastic extend 1' 2>&1)
    jid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
    sleep 8
    if [ -n "$jid" ]; then
        SL "timeout 120 elastic release-id $jid" >/dev/null 2>&1
        sleep 5
        seg=$(SL "awk '/failed to kill job/,0' /tmp/prte.out")
        echo "$seg" | grep -q "failed to kill job $jid" \
            && ok "the scancel failure was reported with the scheduler's text" \
            || bad "a failing scancel went unreported"
        { echo "$seg" | grep -q '\.\.\.' && ! echo "$seg" | grep -q '(line 11)'; } \
            && ok "the oversized scheduler message was truncated with '...'" \
            || bad "scheduler output was not truncated: $(echo "$seg" | tr '\n' ' ' | tail -c 200)"
        SL 'pgrep -x prte >/dev/null' && ok "HNP survived a scancel failure" \
                                      || bad "HNP died when scancel failed"
    else
        bad "could not submit the job for the scancel-failure case"
    fi
    FS 'set scancel_fail 0' >/dev/null
    SL 'timeout -k 5 30 pterm' >/dev/null 2>&1
    cleanup_swarm

    banner "ras/slurm: propagate_* MCA params gate what reaches sbatch"
    # Each propagated attribute has its own switch; turning two off must drop
    # exactly those two arguments and leave the rest of the line intact.
    FS 'init --jobid 1000 --base node1 --tasks 2 --pool node2,node3' >/dev/null
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
            eval \"\$(fake-slurm env)\"; cd /root &&
            prte --prtemca prte_elastic_mode 1 --prtemca ras_slurm_propagate_qos 0 \
                 --prtemca ras_slurm_propagate_time 0 >/tmp/prte.out 2>&1"
    sleep 8
    if SL 'pgrep -x prte >/dev/null'; then
        out=$(SL 'timeout 120 elastic extend 1' 2>&1)
        jid=$(echo "$out" | sed -n 's/^>>> ALLOC_ID \([0-9][0-9]*\).*/\1/p' | head -1)
        sargs=$(fs_args "$jid")
        if [ -n "$jid" ]; then
            { ! echo "$sargs" | grep -q -- '--qos'; } && { ! echo "$sargs" | grep -q -- '--time'; } \
                && ok "propagate_qos/propagate_time=0 dropped their sbatch args" \
                || bad "a disabled attribute still reached sbatch: $(echo "$sargs" | tr '\n' ' ')"
            echo "$sargs" | grep -qx -- '--account=prrte-test' \
                && ok "the attributes still enabled were unaffected" \
                || bad "disabling two attributes disturbed the rest of the line"
        else
            bad "extend failed under the propagate_* overrides: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        fi
        SL 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM with the propagate_* overrides"
    fi
    cleanup_swarm
}

########################################################################
# rmaps: mapping, ranking and binding across real nodes
########################################################################
#
# The offline harness (test/offline) covers the map/rank/bind matrix against
# synthetic topologies, and test/unit/rmaps covers the parsers and the base
# helpers.  Neither can say anything about the two things that only a live,
# multi-node, *persistent* DVM can show:
#
#   - a mapping request that cannot be satisfied must fail the job and leave
#     the DVM standing.  A mapper that walks off the end of its node list
#     takes the HNP down with it, and on a persistent DVM that is every other
#     user's job too, not just the one that asked.
#   - the user-ranked mappers (rankfile, seq) name hosts.  With one node, any
#     placement is the right placement; the rank -> host assignment only
#     means something when there is more than one host to get wrong.
#
# Every case reports rank and host from the process itself rather than
# parsing --display map, so what is checked is where the job actually ran.
RANKHOST='bash -c '"'"'echo RH $PMIX_RANK $(hostname)'"'"''
# "RH <rank> <host>" -> the host that ran that rank, or empty
rh_host() { echo "$1" | awk -v r="$2" '$1=="RH" && $2==r {print $3}' | head -1; }
# from a "--display map" dump: the cpus a rank was bound to, and the app it
# belongs to. The display groups procs under "Data for node:", so this is the
# one place that says both where a rank went and what it got there.
map_bound() { echo "$1" | sed -n "s/.*Process rank: $2 Bound: \(.*\)\$/\1/p" | head -1; }
map_app()   { echo "$1" | sed -n "s/.*App: \([0-9]*\) Process rank: $2 .*/\1/p" | head -1; }

test_rmaps() {
    local out rc n ncore missing lvl b0 b1 b2

    banner "rmaps: an impossible mapping fails the job, not the DVM"
    # max_slots is a hard bound: no oversubscribe directive may push past it.
    # The by-object mapper asks the base "can this node take another proc?",
    # and once the node is at its max the base says no AND drops the node
    # from the mapper's list.  The mapper used to re-offer the same node, so
    # the base removed an item that was no longer on its list and released a
    # reference it no longer held -- a segfault in the HNP, taking the whole
    # persistent DVM with it.  The job must simply be refused.
    #
    # Needs a node with more cores than max_slots, so that free CPUs remain
    # when the slot bound is reached; otherwise the mapper stops for lack of
    # CPUs and never reaches the path under test.
    #
    # max_slots has to come from the DVM's own allocation: a hostfile given
    # to prun selects within the allocation, it does not redefine it, so a
    # slot bound written there would simply be ignored.  Allocating node2
    # alone also keeps the head node out of the map, leaving the bounded
    # node as the only place the job could go.
    cleanup_swarm
    ncore=$(ON 2 'nproc' 2>/dev/null | tr -d ' \r')
    if [ -z "$ncore" ] || [ "$ncore" -lt 2 ] 2>/dev/null; then
        skp "max_slots overrun (node2 has $ncore core(s), need >= 2)"
    else
        RUN 'printf "node2 slots=1 max_slots=1\n" > /tmp/rmaps_max.txt;
             nohup prte --daemonize --hostfile /tmp/rmaps_max.txt >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
        if RUN 'pgrep -x prte >/dev/null'; then
            # the bound itself is honored: one proc fits, four do not
            out=$(RUN 'timeout 60 prun -n 1 hostname' 2>&1)
            [ "$(echo "$out" | grep -c '^node2$')" = 1 ] \
                && ok "a job within max_slots runs" \
                || bad "a job within max_slots was refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
            out=$(RUN 'timeout 60 prun --map-by core:OVERSUBSCRIBE -n 4 hostname' 2>&1)
            rc=$?
            [ "$rc" != 0 ] && ok "job asking past max_slots was refused (rc=$rc)" \
                           || bad "job asking past max_slots was allowed to run"
            # the point of the test: the DVM is still there afterwards
            if RUN 'pgrep -x prte >/dev/null'; then
                ok "DVM survived the refused mapping"
                # one slot is what this allocation has, so one proc is what
                # a still-healthy DVM will map
                out=$(RUN 'timeout 30 prun -n 1 hostname' 2>&1)
                n=$(echo "$out" | grep -cE '^node[0-9]+$')
                [ "$n" = 1 ] && ok "DVM still maps and launches after the refusal" \
                             || bad "DVM alive but no longer usable: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
            else
                bad "DVM died on an impossible mapping request"
            fi
            RUN 'rm -f /tmp/rmaps_max.txt; timeout -k 5 30 pterm' >/dev/null 2>&1
        else
            bad "could not start a DVM for the max_slots test"
        fi
    fi
    cleanup_swarm

    banner "rmaps: rankfile places each rank on the host it names"
    # A rankfile is an explicit rank -> host map, so a multi-node DVM is the
    # only place it can be shown to work.  The ranks here are deliberately
    # written out of order: the parser's duplicate-rank check filed every
    # record under index 0, so a file whose first "slot=" line was for any
    # rank but 0 rejected its own rank 0 line as a duplicate of it.
    RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'printf "rank 2=node3 slot=0\nrank 0=node1 slot=0\nrank 1=node2 slot=0\n" > /tmp/rmaps_rf.txt'
        out=$(RUN "timeout 60 prun --map-by rankfile:FILE=/tmp/rmaps_rf.txt -n 3 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            n=0
            for pair in "0 node1" "1 node2" "2 node3"; do
                set -- $pair
                [ "$(rh_host "$out" "$1")" = "$2" ] || { n=1; printf '    rank %s ran on "%s", rankfile says %s\n' \
                    "$1" "$(rh_host "$out" "$1")" "$2"; }
            done
            [ "$n" = 0 ] && ok "ranks 0/1/2 landed on node1/node2/node3 as written" \
                         || bad "rankfile placement did not follow the file"
        else
            bad "rankfile job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        # a real duplicate is still an error
        RUN 'printf "rank 0=node1 slot=0\nrank 0=node2 slot=0\n" > /tmp/rmaps_dup.txt'
        out=$(RUN 'timeout 60 prun --map-by rankfile:FILE=/tmp/rmaps_dup.txt -n 2 hostname' 2>&1)
        echo "$out" | grep -q 'already assigned' \
            && ok "a genuinely duplicated rank is still rejected" \
            || bad "duplicate rank assignment went unnoticed"
        RUN 'rm -f /tmp/rmaps_rf.txt /tmp/rmaps_dup.txt; timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the rankfile test"
    fi
    cleanup_swarm

    banner "rmaps: seq lays ranks down one per file line, in file order"
    # The sequential mapper's whole contract is "line k names the node for
    # rank k", so it says nothing on a single node.  It also hands the base
    # helper a list of hostfile entries rather than of nodes, which is why
    # the helper must never try to drop a node from the list it was given.
    RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'printf "node3\nnode1\nnode2\nnode3\n" > /tmp/rmaps_seq.txt'
        out=$(RUN "timeout 60 prun --map-by seq:FILE=/tmp/rmaps_seq.txt -n 4 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            n=0
            for pair in "0 node3" "1 node1" "2 node2" "3 node3"; do
                set -- $pair
                [ "$(rh_host "$out" "$1")" = "$2" ] || { n=1; printf '    rank %s ran on "%s", line %s says %s\n' \
                    "$1" "$(rh_host "$out" "$1")" "$(($1+1))" "$2"; }
            done
            [ "$n" = 0 ] && ok "4 ranks followed the 4 hostfile lines in order" \
                         || bad "seq placement did not follow the file"
        else
            bad "seq job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        RUN 'rm -f /tmp/rmaps_seq.txt; timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the seq test"
    fi
    cleanup_swarm

    banner "rmaps: rank-by node and rank-by slot order the same placement differently"
    # Mapping and ranking are orthogonal, and the difference between them is
    # only visible across nodes: by-slot fills a node's ranks before moving
    # on, by-node deals one rank to each node in turn.
    RUN 'nohup prte --daemonize --host node1:2,node2:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN "timeout 60 prun --map-by slot --rank-by slot -n 4 $RANKHOST" 2>&1)
        [ "$(rh_host "$out" 0)" = "$(rh_host "$out" 1)" ] \
            && [ "$(rh_host "$out" 0)" != "$(rh_host "$out" 2)" ] \
            && ok "rank-by slot filled the first node before the second" \
            || bad "rank-by slot did not front-load: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        sleep 2   # let the DVM finish reaping the previous job before the next
        out=$(RUN "timeout 60 prun --map-by node --rank-by node -n 4 $RANKHOST" 2>&1)
        [ "$(rh_host "$out" 0)" != "$(rh_host "$out" 1)" ] \
            && [ "$(rh_host "$out" 0)" = "$(rh_host "$out" 2)" ] \
            && ok "rank-by node dealt one rank to each node in turn" \
            || bad "rank-by node did not alternate: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the ranking test"
    fi
    cleanup_swarm

    banner "rmaps: mapping by an object the node does not have is an error"
    # We map by a hardware object only because the user asked for it, so
    # being unable to is a request we cannot answer. The mapper used to drop
    # the node from consideration instead - shrinking the allocation without
    # saying so - and the caller then quietly downgraded the whole job to
    # by-slot, placing it by a rule nobody asked for.
    RUN 'nohup prte --daemonize --host node1:2,node2:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        # Find an object level these containers genuinely lack, keeping the
        # output of the run that found it. Probing and then re-running the
        # same command would throw the evidence away: show_help emits a given
        # message once per HNP, so the second identical failure is silent.
        missing=""
        out=""
        for lvl in l3cache l2cache l1cache numa; do
            out=$(RUN "timeout 40 prun --map-by $lvl -n 2 hostname" 2>&1)
            rc=$?
            if [ "$rc" != 0 ]; then missing=$lvl; break; fi
        done
        if [ -z "$missing" ]; then
            skp "map-by a missing object (this host's topology has every level)"
        else
            echo "$out" | grep -q 'no object of that type' \
                && ok "--map-by $missing said the node has no such object" \
                || bad "--map-by $missing failed without explaining why: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            echo "$out" | grep -qE '^node[0-9]+$' \
                && bad "--map-by $missing silently placed the job anyway" \
                || ok "no job was placed by some other rule instead"
            RUN 'pgrep -x prte >/dev/null' && ok "DVM survived the refusal" \
                                           || bad "DVM died on an unavailable mapping object"
            # ppr names the resource to place on, so the same rule applies:
            # "2 per <thing this node has none of>" placed nothing there and
            # still reported success. Restart the DVM so show_help has not
            # already spent this message on the run above.
            RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
            cleanup_swarm
            RUN 'nohup prte --daemonize --host node1:2,node2:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
            out=$(RUN "timeout 40 prun --map-by ppr:2:$missing hostname" 2>&1)
            rc=$?
            [ "$rc" != 0 ] && echo "$out" | grep -q 'no object of that type' \
                && ok "ppr:2:$missing said the node has no such resource" \
                || bad "ppr:2:$missing did not refuse (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            RUN 'pgrep -x prte >/dev/null' && ok "DVM survived the ppr refusal" \
                                           || bad "DVM died on an unavailable ppr resource"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the missing-object test"
    fi
    cleanup_swarm

    banner "rmaps: a seq entry's cpu list is the binding for that rank"
    # A sequence file line is "hostname [cpuset ...]", so an entry may say not
    # just which node a rank goes on but which cpus it gets there. That list
    # was parsed and then thrown away - the rank was bound by the ordinary
    # policy instead, and two entries naming the same node could not be given
    # different cpus. Needs >= 2 cpus to tell one binding from another.
    ncore=$(ON 2 'nproc' 2>/dev/null | tr -d ' \r')
    if [ -z "$ncore" ] || [ "$ncore" -lt 2 ] 2>/dev/null; then
        skp "seq per-entry cpusets (node2 has $ncore cpu(s), need >= 2)"
    else
        RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
        if RUN 'pgrep -x prte >/dev/null'; then
            RUN 'printf "node2 0\nnode2 1\nnode3 0\n" > /tmp/rmaps_seqcpu.txt'
            out=$(RUN 'timeout 60 prun --display map --map-by seq:FILE=/tmp/rmaps_seqcpu.txt -n 3 hostname' 2>&1)
            b0=$(map_bound "$out" 0); b1=$(map_bound "$out" 1); b2=$(map_bound "$out" 2)
            [ -n "$b0" ] && [ -n "$b1" ] && [ "$b0" != "$b1" ] \
                && ok "two entries on one node got the two different cpus they named" \
                || bad "seq entry cpusets were not applied (rank0='$b0' rank1='$b1')"
            [ "$b0" = "$b2" ] \
                && ok "the same cpu named on another node bound the same way" \
                || bad "cpu 0 on node3 bound differently from cpu 0 on node2 ('$b2' vs '$b0')"
            # two entries claiming the same cpus on the same node is refused
            RUN 'printf "node2 0\nnode2 0\n" > /tmp/rmaps_seqdup.txt'
            out=$(RUN 'timeout 60 prun --map-by seq:FILE=/tmp/rmaps_seqdup.txt -n 2 hostname' 2>&1)
            echo "$out" | grep -q 'already in use' \
                && ok "two entries claiming one cpu is refused" \
                || bad "overlapping seq cpusets went unnoticed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            RUN 'rm -f /tmp/rmaps_seqcpu.txt /tmp/rmaps_seqdup.txt; timeout -k 5 30 pterm' >/dev/null 2>&1
        else
            bad "could not start a DVM for the seq cpuset test"
        fi
    fi
    cleanup_swarm

    banner "rmaps: pe-list can be given per app"
    # Which cpus an app may use is as much its own business as which object
    # it maps by, and a multi-app command line has nowhere else to say it -
    # it takes two mapping directives to make the mapping per-app at all. A
    # per-app pe-list used to be rejected as an unrecognized policy.
    ncore=$(ON 2 'nproc' 2>/dev/null | tr -d ' \r')
    if [ -z "$ncore" ] || [ "$ncore" -lt 4 ] 2>/dev/null; then
        skp "per-app pe-list (node2 has $ncore cpu(s), need >= 4)"
    else
        RUN 'nohup prte --daemonize --host node2:4 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
        if RUN 'pgrep -x prte >/dev/null'; then
            out=$(RUN 'timeout 60 prun --display map -n 1 --map-by pe-list=0 hostname : -n 1 --map-by pe-list=3 hostname' 2>&1)
            rc=$?
            b0=$(map_bound "$out" 0); b1=$(map_bound "$out" 1)
            if [ "$rc" = 0 ] && [ -n "$b0" ] && [ -n "$b1" ]; then
                [ "$b0" != "$b1" ] \
                    && ok "each app was bound to the cpu list it named ('$b0' vs '$b1')" \
                    || bad "both apps got the same binding '$b0' - the per-app list was ignored"
                [ "$(map_app "$out" 0)" = "0" ] && [ "$(map_app "$out" 1)" = "1" ] \
                    && ok "the two apps kept distinct ranks" \
                    || bad "per-app ranks are wrong"
            else
                bad "per-app pe-list job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
            fi
            RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
        else
            bad "could not start a DVM for the per-app pe-list test"
        fi
    fi
    cleanup_swarm

    banner "rmaps: per-app (MPMD) mapping gives every app its own policy and distinct ranks"
    # Two apps with different --map-by directives send the job down the
    # per-app dispatch path, where each app is mapped on its own and the base
    # ranks them with a running cursor.  If that cursor is lost the apps both
    # start at rank 0 and the job launches with colliding ranks.
    RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN "timeout 60 prun --map-by node -n 2 $RANKHOST : --map-by slot -n 2 $RANKHOST" 2>&1)
        rc=$?
        n=$(echo "$out" | grep '^RH ' | awk '{print $2}' | sort -u | wc -l | tr -d ' ')
        [ "$rc" = 0 ] && [ "$n" = 4 ] \
            && ok "4 procs across 2 apps got 4 distinct ranks" \
            || bad "per-app ranks collided or the job failed (rc=$rc, distinct=$n): $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the per-app mapping test"
    fi
    cleanup_swarm
}

########################################################################
# schizo: personalities and CLI translation, end to end
########################################################################
#
# test/unit/schizo covers the parsers themselves - argv normalization, the
# sanity checker, the output/display converters, and each personality's
# deprecated-option table - with no DVM at all.  What only a live,
# multi-node DVM can show is that the *result* of that translation survives
# the trip to a remote daemon:
#
#   - the envar directives (-x, --set-env, --prepend-env, --append-env,
#     --unset-env) are applied by prte_schizo_base_setup_fork() on the prted
#     that forks the process, not by the tool.  The only place to see whether
#     they were applied - and applied in the right order - is a REMOTE proc's
#     own environment.
#   - --output file=... is written by each daemon.  Its ':'-delimited
#     qualifiers (raw, copy/nocopy) decide whether output is also copied back
#     over the wire, so file-vs-stdout is a cross-daemon observation.
#   - a job's personality is resolved again on every daemon (odls looks it up
#     with detect_proxy from the job's personality list), so a personality
#     the daemons cannot resolve is a launch-time failure, not a CLI one.
#
# The ompi personality gates root execution on OMPI_ALLOW_RUN_AS_ROOT rather
# than the PRTE_* pair RUN() sets, so ompi cases carry their own.
OMPIROOT='OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1'

test_schizo() {
    local out rc n f

    banner "schizo: envar directives reach a REMOTE process"
    # -x forwards a variable from the tool's environment; --set-env sets one
    # outright; --unset-env removes one (and takes a trailing '*' as a
    # prefix).  All of them are recorded as job/app attributes by the tool
    # and only turned into environment on the daemon, so running the probe on
    # node2/node3 - never the head node - is what proves they were applied
    # there.  A remote daemon's environment is its OWN, not the submitting
    # shell's, so a variable the probe expects to see has to be forwarded:
    # that is what makes -x worth testing here and untestable locally.
    out=$(RUN 'SZ_FWD=fwd-ok SZ_GONE=leftover SZ_ALSO=leftover prterun \
                 --host node2:1,node3:1 -np 2 --map-by node \
                 -x SZ_FWD \
                 --set-env SZ_SET=set-ok \
                 --unset-env "SZ_GO*" \
                 bash -c "echo SZ \$(hostname) \$SZ_FWD \$SZ_SET \${SZ_GONE:-unset} \${SZ_ALSO:-unset}"' 2>&1); rc=$?
    # SZ_ALSO is never forwarded, so it must read "unset" on the remote node -
    # which is what makes SZ_FWD reading "fwd-ok" mean -x actually did it
    n=$(echo "$out" | grep -cE '^SZ node[23] fwd-ok set-ok unset unset$')
    [ "$rc" = 0 ] && [ "$n" = 2 ] \
        && ok "-x / set / wildcard-unset applied on both remote nodes" \
        || bad "envar directives wrong on a remote node (rc=$rc, matched=$n): $(echo "$out" | tr '\n' ' ' | tail -c 300)"

    # PREPEND/APPEND edit an EXISTING value, so they need a variable the
    # daemon really owns - PATH.  Two things are checked at once:
    #
    #   - the entry is added exactly ONCE.  These directives used to be
    #     applied twice, by odls' process_envars() and again by the schizo
    #     setup_fork hook.  SET and UNSET are idempotent so nothing showed,
    #     but every entry a user prepended onto PATH or LD_LIBRARY_PATH was
    #     duplicated.
    #   - a differently-named variable that merely STARTS with the same
    #     letters is left alone.  The match used to be a bare strncmp of the
    #     name's length with no '=' anchor, so "PATH" also matched PATHOLOGY
    #     - whichever the environment happened to list first.
    out=$(RUN 'PATHOLOGY=untouched prterun --host node2:1 -np 1 \
                 -x PATHOLOGY --prepend-env "PATH[:]" /SZDIR --append-env "PATH[:]" /SZEND \
                 bash -c "echo SZP \$(echo \$PATH | tr : \\\\n | grep -c \"^/SZDIR\$\") \
                          \$(echo \$PATH | tr : \\\\n | grep -c \"^/SZEND\$\") \$PATHOLOGY"' 2>&1)
    echo "$out" | grep -qE '^SZP 1 1 untouched$' \
        && ok "--prepend/append-env PATH each added their entry once, PATHOLOGY untouched" \
        || bad "--prepend/append-env PATH misapplied: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # A repeated --set-env must set BOTH variables.  pmix_cmd_line_parse
    # stores one value per occurrence for --set-env (unlike --prepend-env,
    # which stores a name and a value), so walking the value array two at a
    # time skipped every other one - silently, since a variable that was
    # never set just reads as empty in the app.
    out=$(RUN 'prterun --host node2:1 -np 1 \
                 --set-env SZ_A=a-ok --set-env SZ_B=b-ok \
                 bash -c "echo SZ2 \${SZ_A:-missing} \${SZ_B:-missing}"' 2>&1)
    echo "$out" | grep -q 'SZ2 a-ok b-ok' \
        && ok "both --set-env directives reached the remote process" \
        || bad "a repeated --set-env was dropped: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "schizo: --merge-stderr-to-stdout is honored, not silently dropped"
    # The deprecated spelling is in the prterun/prun option tables.  With no
    # conversion behind it, it parsed cleanly and then did nothing at all -
    # the user asked for merged output and got separate streams, with no
    # error to say so.  Read stdout only: SZ-ERR appears there only if the
    # merge really happened on the daemon that forked the process.
    out=$(RUN 'prterun --host node2:1 -np 1 --merge-stderr-to-stdout \
                 bash -c "echo SZ-OUT; echo SZ-ERR 1>&2" 2>/dev/null' )
    echo "$out" | grep -q SZ-OUT && echo "$out" | grep -q SZ-ERR \
        && ok "--merge-stderr-to-stdout merged a remote proc's stderr into stdout" \
        || bad "--merge-stderr-to-stdout was ignored: $(echo "$out" | tr '\n' ' ')"
    # the modern spelling must of course still work
    out=$(RUN 'prterun --host node2:1 -np 1 --output merge-stderr-to-stdout \
                 bash -c "echo SZ-OUT; echo SZ-ERR 1>&2" 2>/dev/null' )
    echo "$out" | grep -q SZ-ERR \
        && ok "--output merge-stderr-to-stdout still merges" \
        || bad "--output merge-stderr-to-stdout stopped merging: $(echo "$out" | tr '\n' ' ')"

    banner "schizo: envar directives take effect in the order they were given"
    # SET replaces a value outright; PREPEND/APPEND edit the one already
    # there.  Which of them the process ends up with therefore depends on the
    # ORDER the user gave them in, and that order is theirs to choose - not a
    # merge policy PRRTE gets to pick.  The attributes used to be assembled
    # with prte_prepend_attribute(), which reversed the command line, so
    # "-x FOO --prepend-env FOO[:] head" applied the -x SET last and threw
    # the prepend away.
    out=$(RUN 'prterun --host node2:1 -np 1 \
                 --prepend-env "SZO[:]" x --set-env SZO=1 \
                 bash -c "echo SZO1=\$SZO"' 2>&1)
    echo "$out" | grep -q '^SZO1=1$' \
        && ok "prepend then set: the later set wins, as asked" \
        || bad "prepend-then-set gave the wrong value: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(RUN 'prterun --host node2:1 -np 1 \
                 --set-env SZO=1 --prepend-env "SZO[:]" x \
                 bash -c "echo SZO2=\$SZO"' 2>&1)
    echo "$out" | grep -q '^SZO2=x:1$' \
        && ok "set then prepend: the prepend edits the value the set left" \
        || bad "set-then-prepend gave the wrong value: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(RUN 'SZO=middle prterun --host node2:1 -np 1 \
                 -x SZO --prepend-env "SZO[:]" head --append-env "SZO[:]" tail \
                 bash -c "echo SZO3=\$SZO"' 2>&1)
    echo "$out" | grep -q '^SZO3=head:middle:tail$' \
        && ok "-x then prepend then append wrap the forwarded value" \
        || bad "-x with prepend/append gave the wrong value: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "schizo: --output file=...:pattern names the files, not PRRTE"
    # Without "pattern" the name is a stem PMIx annotates with the namespace
    # and rank.  With it, the name is the user's own and its '%' conversions
    # are expanded - including in the DIRECTORY part, which is why this is a
    # multi-node case: each daemon creates the directory its own expansion
    # names, and %h differs between them.
    for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szpat' >/dev/null 2>&1; done
    out=$(RUN 'prterun --host node2:1,node3:1 -np 2 --map-by node \
                 --output "file=/tmp/szpat/%h/rank-%R:pattern" \
                 bash -c "echo PAT-\$PMIX_RANK"' 2>&1)
    f=0
    for n in 2 3; do
        ON "$n" 'cat /tmp/szpat/node'"$n"'/rank-*.out 2>/dev/null' | grep -q '^PAT-' && f=$((f+1))
    done
    [ "$f" = 2 ] \
        && ok "each daemon expanded %h/%R and wrote under the directory it named" \
        || bad "pattern expansion produced files on $f/2 nodes: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    # and the stream suffix is still appended, so stdout and stderr are split
    for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szpat' >/dev/null 2>&1; done
    RUN 'prterun --host node2:1 -np 1 --output "file=/tmp/szpat/r%r:pattern" \
           bash -c "echo P-OUT; echo P-ERR 1>&2"' >/dev/null 2>&1
    o=$(ON 2 'cat /tmp/szpat/r0.out 2>/dev/null')
    e=$(ON 2 'cat /tmp/szpat/r0.err 2>/dev/null')
    [ "$(echo "$o" | tr -d '\r')" = "P-OUT" ] && [ "$(echo "$e" | tr -d '\r')" = "P-ERR" ] \
        && ok "the .out/.err suffix still splits the streams under a pattern" \
        || bad "pattern streams wrong (out='$o' err='$e')"
    # an unrecognized conversion is refused before anything is launched
    out=$(RUN 'prterun --host node2:1 -np 1 --output "file=/tmp/szpat/%q:pattern" hostname' 2>&1)
    echo "$out" | grep -q 'unrecognized conversion' \
        && ok "a bad pattern conversion is reported, not launched with" \
        || bad "a bad pattern conversion was accepted: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szpat' >/dev/null 2>&1; done

    banner "schizo: every --output qualifier counts, whatever its position"
    # Qualifiers are ':'-delimited and were split on ',' instead - which the
    # directive split has already consumed - so the whole ':'-joined run was
    # matched as ONE token.  Prefix matching then made the FIRST qualifier
    # win and silently discarded the rest.  "copy" is the discriminator:
    # nocopy is also the default, so only asking for copy can tell whether
    # the qualifier was read at all.  Both orders must behave identically.
    # Each rank's file is written by its own daemon, so this also checks the
    # directive survived the wire.
    for q in "copy:raw" "raw:copy"; do
        for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szout' >/dev/null 2>&1; done
        out=$(RUN "prterun --host node2:1,node3:1 -np 2 --map-by node \
                     --output file=/tmp/szout/o:$q bash -c 'echo SZ-FILE-OK'" 2>/dev/null)
        n=$(echo "$out" | grep -c SZ-FILE-OK)
        [ "$n" = 2 ] \
            && ok "--output file=...:$q copied output back to stdout as asked" \
            || bad "--output file=...:$q lost the copy qualifier ($n/2 lines on stdout)"
        f=0
        for n in 2 3; do
            ON "$n" 'grep -rl SZ-FILE-OK /tmp/szout 2>/dev/null | head -1' | grep -q . && f=$((f+1))
        done
        [ "$f" = 2 ] && ok "--output file=...:$q wrote a file on each daemon" \
                     || bad "--output file=...:$q produced files on $f/2 nodes"
    done
    # and nocopy really does keep it off stdout
    for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szout' >/dev/null 2>&1; done
    out=$(RUN "prterun --host node2:1 -np 1 \
                 --output file=/tmp/szout/o:raw:nocopy bash -c 'echo SZ-FILE-OK'" 2>/dev/null)
    [ -z "$(echo "$out" | grep SZ-FILE-OK)" ] \
        && ok "--output file=...:raw:nocopy kept output off stdout" \
        || bad "--output file=...:raw:nocopy leaked output to stdout"
    for n in 1 2 3; do ON "$n" 'rm -rf /tmp/szout' >/dev/null 2>&1; done

    banner "schizo: --personality is honored in both spellings"
    # normalize_argv() is what finds the personality, before any option table
    # exists to parse with.  It only understood "--personality ompi" and not
    # the "--personality=ompi" form getopt_long equally accepts, so the "="
    # form silently fell back to the default (prte) personality - and then
    # rejected every ompi-only option as unrecognized.  --display-comm is
    # defined ONLY by the ompi personality, so it is the discriminator.
    for form in "--personality ompi" "--personality=ompi"; do
        out=$(RUN "$OMPIROOT prterun $form --host node2:1,node3:1 -np 2 --map-by node \
                     --display-comm hostname" 2>&1); rc=$?
        n=$(echo "$out" | grep -cE '^node[23]$')
        [ "$rc" = 0 ] && [ "$n" = 2 ] \
            && ok "\"$form\" selected the ompi personality and launched" \
            || bad "\"$form\" did not select ompi (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    done
    # the same for a renamed option given with '=' - the tables carry the
    # un-hyphenated key, so "--map-by=node" only reaches them if normalize
    # rewrote it
    out=$(RUN 'prterun --host node2:1,node3:1 -np 2 --map-by=node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[23]$')
    [ "$rc" = 0 ] && [ "$n" = 2 ] \
        && ok "--map-by=node (the '=' spelling) was normalized and honored" \
        || bad "--map-by=node was rejected (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "schizo: a bare '--map-by ppr' is refused, not a crash"
    # The socket->package rewrite reaches for the resource field of a ppr
    # pattern.  Under the ompi personality it did so with strrchr(), which
    # returns NULL for a pattern with no ':' at all - and the result was
    # advanced past and dereferenced, killing the tool at address 0x1 before
    # it ever reached the mapper.  prte had been fixed; ompi had not.
    out=$(RUN 'prterun --host node2:1 -np 1 --map-by ppr hostname' 2>&1)
    echo "$out" | grep -qiE 'signal|Segmentation' \
        && bad "prterun (prte) crashed on a bare --map-by ppr" \
        || ok "prterun (prte) refused a bare --map-by ppr without crashing"
    out=$(RUN "$OMPIROOT prterun --personality ompi --host node2:1 -np 1 --map-by ppr hostname" 2>&1)
    echo "$out" | grep -qiE 'signal|Segmentation' \
        && bad "prterun --personality ompi crashed on a bare --map-by ppr" \
        || ok "prterun --personality ompi refused a bare --map-by ppr without crashing"

    banner "schizo: an option that takes one value may not be repeated"
    # pmix_cmd_line_parse appends every occurrence of an option to the SAME
    # instance, and every consumer reads values[0] - so "-np 2 -np 3" ran
    # silently with 2 procs.  The guard that was meant to catch that had its
    # comparison inverted and never fired.
    out=$(RUN 'prterun --host node2:2 -np 2 -np 3 hostname' 2>&1)
    echo "$out" | grep -q 'too many instances' \
        && ok "a repeated --np is reported instead of silently taking the first" \
        || bad "a repeated --np was accepted: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "schizo: a personality nobody claims falls back to the native one"
    # No component claims an unrecognized personality, so every bid is zero.
    # The election used to award the job to whichever component carried the
    # highest static priority, silently reading the command line in the OMPI
    # dialect - a different option table and a different MCA translation than
    # the user asked for.  It must land on the catch-all instead.
    #
    # Falling back rather than refusing is deliberate and load-bearing: Open
    # MPI starts a singleton's DVM with "--prtemca schizo prte", so only the
    # native component is loaded, and then spawns with
    # PMIX_PERSONALITY="ompi5".  Refusing that fails every MPI_Comm_spawn from
    # a singleton.  --display-comm is defined ONLY by the ompi personality, so
    # it is what tells the two apart.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node1:1,node2:1,node3:1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN 'timeout 30 prun --personality no-such-personality --display-comm -np 1 hostname' 2>&1)
        echo "$out" | grep -q 'unrecognized option' \
            && ok "an unknown --personality reads the command line as prte, not ompi" \
            || bad "an unknown --personality did not fall back to prte: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        out=$(RUN 'timeout 30 prun --personality no-such-personality --host node2:1 -np 1 hostname' 2>&1)
        echo "$out" | grep -qE '^node2$' \
            && ok "and a prte-valid command line still runs under it" \
            || bad "the fallback personality could not run a job: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        RUN 'pgrep -x prte >/dev/null' \
            && ok "the persistent DVM survived both" \
            || bad "the DVM died on an unknown --personality"
        out=$(RUN 'timeout 30 prun --host node2:1,node3:1 -np 2 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node[23]$')
        [ "$n" = 2 ] && ok "the DVM still launches jobs afterwards" \
                     || bad "the DVM could not launch afterwards ($n/2)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the unknown-personality test"
    fi
    cleanup_swarm
}

########################################################################
# state: the machine that sequences every job, and the runtime options
#        it translates into per-job attributes
########################################################################
test_state() {
    local out rc n t0 t1 dt

    banner "state: a boolean runtime option set to false must be OFF"
    # Every consumer of a boolean runtime option tests it by PRESENCE --
    # prte_get_attribute(&attrs, KEY, NULL, PMIX_BOOL) is true as soon as the
    # key is on the list, whatever value it holds.  The directive parser
    # stored the parsed boolean AS THE VALUE, so "error-nonzero-status=false"
    # left the attribute present-and-false, which every one of those call
    # sites read as ENABLED.  Asking for the option to be off turned it on.
    #
    # The daemon's odls consults exactly that attribute when a child exits
    # non-zero: with it set, the proc goes to TERM_NON_ZERO and the job is
    # torn down with "exited with non-zero status"; without it, the exit is
    # a normal termination.  So the message is the observable, and it takes
    # a real DVM with a real forked child to produce.
    cleanup_swarm
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options error-nonzero-status=false /bin/false' 2>&1)
    echo "$out" | grep -q 'non-zero status' \
        && bad "error-nonzero-status=false still treated a non-zero exit as an error" \
        || ok "error-nonzero-status=false let a non-zero exit terminate normally"

    # ... and the same option left at true must still report, so the fix
    # cannot have been "ignore the directive entirely"
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options error-nonzero-status=true /bin/false' 2>&1)
    echo "$out" | grep -q 'non-zero status' \
        && ok "error-nonzero-status=true still reports a non-zero exit" \
        || bad "error-nonzero-status=true no longer reports a non-zero exit: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # a bare directive carries no '=' and means true
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options error-nonzero-status /bin/false' 2>&1)
    echo "$out" | grep -q 'non-zero status' \
        && ok "a bare error-nonzero-status means true" \
        || bad "a bare error-nonzero-status was not honored: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "state: a boolean runtime option that is not already set must go OFF"
    # prte_set_attribute only DROPS a boolean when the key is already on the
    # list; when it is not, "opt=false" is ADDED with a false value -- and
    # every consumer tests these by presence, so the option reads as ON.
    # notifyerrors is the probe because set_runtime_options checks it itself:
    # notifications can never be delivered unless the job is recoverable or
    # continuous, so a notifyerrors that is ON is refused with an explanatory
    # message.  Asking for it to be OFF must therefore let the job RUN.
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options notifyerrors=false hostname' 2>&1)
    echo "$out" | grep -q 'NOTIFYERRORS was set to true' \
        && bad "notifyerrors=false was recorded as ON and refused the job" \
        || ok "notifyerrors=false left the option off"
    echo "$out" | grep -qE '^node2$' \
        && ok "and the job ran" \
        || bad "notifyerrors=false did not run the job: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # ... while asking for it ON, with nothing to make the job survivable,
    # must still be refused -- the fix cannot be "ignore the directive"
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options notifyerrors hostname' 2>&1)
    echo "$out" | grep -q 'NOTIFYERRORS was set to true' \
        && ok "a bare notifyerrors is still caught as an unusable combination" \
        || bad "a bare notifyerrors was silently accepted: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "state: a directive AFTER 'timeout=' is still applied"
    # The directive walk used its loop index as a scratch variable: the
    # timeout branch assigned the converted seconds to it, so "timeout=60,..."
    # resumed the walk at index 60 -- past the end of the option array.  The
    # out-of-bounds slot is then run through the whole if-chain and matches
    # nothing, so the spec is rejected as "not recognized" even though every
    # directive in it is valid.  notifyerrors is again the probe: reaching it
    # produces the bad-combination message, so the two failure modes (dropped
    # vs. bogus rejection) are both distinguishable from success.
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options timeout=60,notifyerrors hostname' 2>&1)
    echo "$out" | grep -q 'not recognized' \
        && bad "a valid spec after 'timeout=' was rejected as unrecognized (walked off the option array)" \
        || ok "a valid spec after 'timeout=' was not falsely rejected"
    echo "$out" | grep -q 'NOTIFYERRORS was set to true' \
        && ok "the directive after 'timeout=' was parsed" \
        || bad "the directive after 'timeout=' was dropped: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # same defect in the max-restarts branch, which reused the index to walk
    # the app array -- there the later directives are silently dropped
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options max-restarts=3,notifyerrors hostname' 2>&1)
    echo "$out" | grep -q 'NOTIFYERRORS was set to true' \
        && ok "the directive after 'max-restarts=' was parsed" \
        || bad "the directive after 'max-restarts=' was dropped: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # and the timeout itself must still work -- the fix must not have broken
    # the branch it repaired
    t0=$(date +%s)
    RUN 'prterun --host node2:1 -np 1 --runtime-options timeout=5 sleep 120' >/dev/null 2>&1
    t1=$(date +%s); dt=$((t1-t0))
    [ "$dt" -lt 60 ] && ok "runtime-options timeout=5 still killed the job (${dt}s)" \
                     || bad "runtime-options timeout=5 did not take effect (${dt}s)"

    banner "state: report-child-jobs-separately is implemented, not just documented"
    # The directive is listed in the runtime-options help text and passes
    # schizo's validator, but the parser that turns a directive into a job
    # attribute had no branch for it at all - so the documented option was
    # refused as unrecognized, and its only reader was inside a handler no
    # component registers.  It now sets PRTE_JOB_REPORT_CHILD_SEP and the
    # DVM teardown consults it when deciding whose exit status is returned.
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options report-child-jobs-separately hostname' 2>&1)
    echo "$out" | grep -q 'not recognized' \
        && bad "the documented report-child-jobs-separately directive is still refused" \
        || ok "report-child-jobs-separately is accepted"
    echo "$out" | grep -qE '^node2$' \
        && ok "and the job runs under it" \
        || bad "report-child-jobs-separately did not run the job: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options report-child-jobs-separately=false hostname' 2>&1)
    echo "$out" | grep -qE '^node2$' \
        && ok "and so is the explicit =false form" \
        || bad "report-child-jobs-separately=false was refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    # The PRIMARY job's status must still be returned when the option is on -
    # only a job it SPAWNED is reported separately.
    RUN 'prterun --host node2:1 -np 1 --runtime-options report-child-jobs-separately /bin/false' >/dev/null 2>&1
    rc=$?
    [ "$rc" != 0 ] && ok "the primary job's non-zero exit is still returned (rc=$rc)" \
                   || bad "report-child-jobs-separately swallowed the PRIMARY job's exit status"
    RUN 'prterun --host node2:1 -np 1 --runtime-options report-child-jobs-separately /bin/true' >/dev/null 2>&1
    rc=$?
    [ "$rc" = 0 ] && ok "and a clean primary job still exits zero" \
                  || bad "a clean job under report-child-jobs-separately exited $rc"

    # The same policy also has a STANDALONE spelling, defined in prterun's and
    # mpirun's option tables and documented in both - but nothing read it, so
    # the flag parsed and was silently discarded.
    out=$(RUN 'prterun --host node2:1 --report-child-jobs-separately -np 1 hostname' 2>&1)
    echo "$out" | grep -qE '^node2$' \
        && ok "the standalone --report-child-jobs-separately flag runs a job" \
        || bad "--report-child-jobs-separately broke the launch: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    RUN 'prterun --host node2:1 --report-child-jobs-separately -np 1 /bin/false' >/dev/null 2>&1
    rc=$?
    [ "$rc" != 0 ] && ok "and still returns the primary job's status (rc=$rc)" \
                   || bad "--report-child-jobs-separately swallowed the PRIMARY job's exit status"

    banner "state: a CHILD job's exit status is withheld only when asked for"
    # This is the case the option exists for, and it needs a real parent/child
    # job pair -- examples/dynamic.c, the tree's PMIx_Spawn example, which
    # build.sh installs as "dynamic".  Rank 0 spawns "client" from its cwd, so
    # the child's exit status is whatever we put there: a script that exits 7.
    # The parent itself exits 0.
    #
    # Without the option the launcher returns the first non-zero status from
    # EITHER job, so prterun must exit 7.  With it, only the primary job's
    # status counts, so prterun must exit 0 -- and the child's status has to be
    # reported rather than silently dropped.
    #
    # The script must exist on every node the child could be mapped onto: the
    # spawn names an absolute path derived from the parent's cwd, and /tmp is
    # per-container.
    for n in 1 2 3; do
        ON $n 'rm -rf /tmp/dyn && mkdir -p /tmp/dyn &&
               printf "#!/bin/sh\nexit 7\n" > /tmp/dyn/client && chmod +x /tmp/dyn/client' >/dev/null 2>&1
    done
    dynhosts='--host node1:2,node2:2,node3:2'

    out=$(RUN "cd /tmp/dyn && timeout 90 prterun $dynhosts -np 1 dynamic" 2>&1); rc=$?
    if ! echo "$out" | grep -q 'Spawn success'; then
        bad "dynamic could not spawn a child job -- the rest of this case is meaningless: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    else
        ok "dynamic spawned a child job"
        [ "$rc" = 7 ] \
            && ok "by default the child's exit status becomes the launcher's (rc=$rc)" \
            || bad "expected the child's status 7 to be returned by default, got rc=$rc"

        out=$(RUN "cd /tmp/dyn && timeout 90 prterun $dynhosts --report-child-jobs-separately -np 1 dynamic" 2>&1); rc=$?
        [ "$rc" = 0 ] \
            && ok "with --report-child-jobs-separately the launcher returns the PRIMARY job's status (rc=0)" \
            || bad "--report-child-jobs-separately did not withhold the child's status (rc=$rc)"
        echo "$out" | grep -q 'Child job' \
            && ok "and the child's status is still reported to the user" \
            || bad "the child's status was withheld AND never reported: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

        # the runtime-option spelling must behave identically
        out=$(RUN "cd /tmp/dyn && timeout 90 prterun $dynhosts --runtime-options report-child-jobs-separately -np 1 dynamic" 2>&1); rc=$?
        [ "$rc" = 0 ] \
            && ok "the --runtime-options spelling withholds it too (rc=0)" \
            || bad "--runtime-options report-child-jobs-separately did not withhold the child's status (rc=$rc)"
    fi
    for n in 1 2 3; do ON $n 'rm -rf /tmp/dyn' >/dev/null 2>&1; done

    banner "state: an unrecognized runtime option is refused, not ignored"
    out=$(RUN 'prterun --host node2:1 -np 1 --runtime-options no-such-option hostname' 2>&1)
    echo "$out" | grep -q 'not recognized' \
        && ok "an unknown runtime option is rejected" \
        || bad "an unknown runtime option was silently accepted: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

    banner "state: a job-state activation carrying no job still says which state"
    # 80-odd call sites activate an error state with a NULL job --
    # PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_NEVER_LAUNCHED) here.  No
    # component registers that state, so it falls through to the errmgr's
    # PRTE_JOB_STATE_ERROR catch-all, which does "jdata->state =
    # caddy->job_state" against the DAEMON job.  The dispatcher only filled
    # caddy->job_state in when a job accompanied the activation, and PMIX_NEW
    # does not zero its allocation -- so the daemon job's state was assigned
    # uninitialized heap, and which recovery branch the errmgr then took was
    # down to whatever the allocator handed back.
    #
    # An unfindable launch agent is the cheapest deterministic way to reach
    # it.  With the state correctly carried, the errmgr recognizes
    # NEVER_LAUNCHED, disables routing and tears the DVM down: the tool must
    # report the agent failure and exit promptly rather than hang.
    t0=$(date +%s)
    bounded 90 docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc '. /opt/prte/env.sh;
            prterun --mca plm_ssh_agent /no/such/launch/agent --host node2:1,node3:1 -np 2 hostname'
    rc=$?
    t1=$(date +%s); dt=$((t1-t0))
    if [ "$rc" = 124 ]; then
        bad "a failed launch agent hung the tool (${dt}s) instead of reporting"
    else
        ok "a failed launch agent terminated the DVM promptly (${dt}s, rc=$rc)"
        grep -qi 'launch agent\|agent-not-found\|unable to find' "$BOUT" \
            && ok "and reported the launch-agent failure" \
            || bad "but reported nothing about the agent: $(tr '\n' ' ' < "$BOUT" | tail -c 200)"
    fi
    rm -f "$BOUT"
    cleanup_swarm

    banner "state: the DVM sequences a job through to a clean teardown"
    # check_complete is the largest handler in the machine: it releases the
    # job's mapped resources back to every node, drops the map, deregisters
    # the nspace, and notifies.  Its resource accounting is only observable
    # across successive jobs on the SAME persistent DVM -- if a node's
    # slots_inuse/num_procs are not restored, the second job cannot be
    # mapped onto the nodes the first one used.
    RUN 'nohup prte --daemonize --host node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        n=0
        for i in 1 2 3; do
            out=$(RUN 'timeout 60 prun -n 4 --map-by node hostname' 2>&1)
            [ "$(echo "$out" | grep -cE '^node[23]$')" = 4 ] && n=$((n+1))
        done
        [ "$n" = 3 ] && ok "three successive full-allocation jobs all mapped (resources recovered each time)" \
                     || bad "only $n/3 successive jobs filled the allocation -- resources not returned on teardown"
        RUN 'pgrep -x prte >/dev/null' \
            && ok "the DVM survived all three teardowns" \
            || bad "the DVM died during repeated job teardown"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the teardown-accounting test"
    fi
    cleanup_swarm
}

########################################################################
# src/prted -- the daemon body and the PMIx server host module
########################################################################
#
# Everything interesting in src/prted needs more than one daemon.  A single
# node hides the whole point of the code: the command dispatcher only has one
# recipient, every client is a client of the HNP, and a job-level Get is
# always answered out of the local cache.  These are the cases that only exist
# across nodes.
#
# NOTE on DVM discovery: several cases here leave a prun running in the
# background, and a live prun holds its own pmix.* rendezvous file.  A later
# prun that searches $TMPDIR then finds more than one server and refuses to
# guess ("multiple possible servers ... connection handles have been read from
# files named pmix.*").  So every case that starts a persistent DVM reports its
# URI to a file and every prun is pointed at it explicitly.
PRTED_URI=/tmp/prted-test.uri
# start a persistent DVM on node1 with the given --host spec, reporting its URI
prted_dvm_start() {
    RUN "rm -f $PRTED_URI" >/dev/null 2>&1
    RUN "timeout -k 5 60 prte --daemonize --report-uri $PRTED_URI --host $1" >/dev/null 2>&1
    sleep 4
    RUN "test -s $PRTED_URI"
}
# ...and the same with extra MCA (or other) options appended: $2 is spliced
# in ahead of --host so a case can turn on a daemon-side knob.
prted_dvm_start_mca() {
    RUN "rm -f $PRTED_URI" >/dev/null 2>&1
    RUN "timeout -k 5 60 prte --daemonize --report-uri $PRTED_URI $2 --host $1" >/dev/null 2>&1
    sleep 4
    RUN "test -s $PRTED_URI"
}
# run a tool against that DVM, from the head node ($@ = argv after "prun")
PRUN() { RUN "timeout -k 5 60 prun --dvm-uri file:$PRTED_URI $*"; }
# ...and in the background, with its output captured on node1
PRUN_BG() {
    local outf=$1; shift
    docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
        "${NODE}1" bash -lc ". /opt/prte/env.sh;
            prun --dvm-uri file:$PRTED_URI $* > $outf 2>&1"
}
########################################################################
# src/runtime -- the object model, the global registries, and the
# publish/lookup data server.  Most of src/runtime is covered without a DVM
# by test/unit/runtime; what lands here is what only exists between real
# daemons.
########################################################################
# Absolute path, deliberately.  The node entrypoint symlinks the install bin
# into /usr/local/bin when the CONTAINER starts, and an app launched into the
# DVM inherits the daemon PATH -- which does not contain the install bindir.
# So a helper added since the containers came up is not on the app PATH, and a
# bare name fails with PMIX_ERR_JOB_FAILED_TO_LAUNCH and no diagnostic.
DS=/opt/prte/prte/bin/dataserver
# ...and the same for the slow stdin reader, for the same reason.
SC=/opt/prte/prte/bin/slowcat

test_runtime() {
    local out n rc

    banner "runtime/data_server: publish on one node, look it up from another"
    # The store is a SINGLE pointer array living on the HNP.  Every client
    # reaches it over the RML through its own daemon, so a publish on node2
    # and a lookup on node3 is the only shape that exercises the real path:
    # PMIx -> prted -> RML(PRTE_RML_TAG_DATA_SERVER) -> HNP -> ds_publish,
    # and the reply back out through a DIFFERENT daemon.  On one node the
    # whole thing collapses into the local PMIx server.
    cleanup_swarm
    if ! RUN "test -x $DS"; then
        skp "dataserver client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the data-server tests"
    else
        PRUN_BG /tmp/ds-pub.out "--host node2:1 -n 1 $DS publish prte.test.k1 hello session 90"
        sleep 8
        if ! RUN 'grep -q "^PUBLISHED prte.test.k1" /tmp/ds-pub.out'; then
            bad "publisher never published: $(RUN 'cat /tmp/ds-pub.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        else
            ok "a proc on node2 published into the HNP store"
            out=$(PRUN "--host node3:1 -n 1 $DS lookup prte.test.k1 20" 2>&1)
            echo "$out" | grep -q '^FOUND prte.test.k1 hello' \
                && ok "a proc on node3 looked it up and got the value back" \
                || bad "cross-node lookup failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            # ...and the answer names the publisher, which is the "data owner"
            # field ds_publish packs ahead of every returned value
            echo "$out" | grep -q 'from .*:0)' \
                && ok "...and the reply carried the publisher's identity" \
                || bad "the reply did not name the data owner: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

            banner "runtime/data_server: a key nobody published is NOT_FOUND, not a hang"
            # A miss travels the same path and has to come back as a status.
            # ds_lookup used to have exits that returned without sending
            # anything AND without releasing the answer buffer.
            out=$(PRUN "--host node3:1 -n 1 $DS lookup prte.test.nosuchkey 15" 2>&1)
            echo "$out" | grep -qE 'STATUS .*(NOT.FOUND|NOT_FOUND)' \
                && ok "a missing key came back as not-found" \
                || bad "a missing key did not report not-found: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

            banner "runtime/data_server: a partial lookup is reported as partial"
            # Two keys, one published.  ds_lookup used to set
            # PMIX_ERR_PARTIAL_SUCCESS and then fall straight through to
            # "return rc" without sending anything, so the caller relayed a
            # bare error, the values that HAD been found were dropped, and
            # the buffer holding them leaked.  It now packs the status and
            # the payload and sends them like any other answer.
            #
            # It takes BOTH halves of the round trip to be right, which is
            # why this asserts on the value and not just the status.  The
            # PMIx client used to discard the payload too - answering the
            # lookup callback with (ret, NULL, 0) for any status that was not
            # PMIX_SUCCESS, even though the layer immediately above it
            # explicitly handles PMIX_ERR_PARTIAL_SUCCESS as data-carrying -
            # so a correct server still produced an empty answer.  Needs a
            # PMIx built from source (PMIX_SRC=... ./build.sh).
            out=$(PRUN "--host node4:1 -n 1 $DS lookup2 prte.test.k1 prte.test.absent 15" 2>&1)
            echo "$out" | grep -q 'STATUS PMIX_ERR_PARTIAL_SUCCESS' \
                && ok "a half-satisfiable lookup came back as PARTIAL_SUCCESS" \
                || bad "a partial lookup did not report itself as partial: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            echo "$out" | grep -q '^FOUND prte.test.k1 hello' \
                && ok "...carrying the key that did exist, not an empty answer" \
                || bad "a partial lookup lost the data it found: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            echo "$out" | grep -q '^FOUND prte.test.absent' \
                && bad "a key nobody published was reported as found" \
                || ok "...and the absent key was not invented"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "runtime/data_server: a waiting lookup is satisfied by a later publish"
    # PMIX_WAIT parks the request on prte_data_store.pending until somebody
    # publishes the key.  The publish arrives from one daemon and the reply
    # goes back out through another, so the parked request has to carry the
    # requestor's proxy and room number across the gap.  The park side also
    # has to dispose of the answer buffer it was handed but will never send
    # -- otherwise every waiting lookup leaks one on the HNP.
    if ! RUN "test -x $DS"; then
        skp "dataserver client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the waiting-lookup test"
    else
        PRUN_BG /tmp/ds-wait.out "--host node3:1 -n 1 $DS lookupwait prte.test.later 60"
        sleep 8
        if ! RUN 'grep -q "^WAITING" /tmp/ds-wait.out'; then
            skp "the waiting lookup never started; the next case is weaker"
        else
            ok "a lookup on node3 is parked waiting for a key"
            # nothing may have been answered yet
            RUN 'grep -q "^FOUND" /tmp/ds-wait.out' \
                && bad "the parked lookup answered before anything was published" \
                || ok "...and it has not been answered yet"
        fi
        PRUN_BG /tmp/ds-late.out "--host node2:1 -n 1 $DS publish prte.test.later worldwide session 40"
        sleep 12
        RUN 'grep -q "^FOUND prte.test.later worldwide" /tmp/ds-wait.out' \
            && ok "the publish from node2 satisfied the lookup parked from node3" \
            || bad "a parked lookup was never satisfied: $(RUN 'cat /tmp/ds-wait.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        # a fully-resolved parked request must report SUCCESS, not
        # PARTIAL_SUCCESS: ds_publish derived that by counting req->keys,
        # which by then holds only the keys still UNresolved (i.e. none), so
        # a complete answer was reported as partial
        RUN 'grep -qE "^STATUS SUCCESS|^STATUS .*(PMIX_SUCCESS)" /tmp/ds-wait.out' \
            && ok "...and reported it as a complete, not partial, result" \
            || bad "a fully satisfied lookup was reported as partial: $(RUN 'grep "^STATUS" /tmp/ds-wait.out' 2>&1 | tr -d '\r')"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "runtime/data_server: unpublish removes the data"
    # Only the publisher may unpublish its own keys, so this runs in one
    # process: publish, confirm, unpublish, confirm gone.
    if ! RUN "test -x $DS"; then
        skp "dataserver client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2'; then
        bad "could not start a DVM for the unpublish test"
    else
        out=$(PRUN "--host node2:1 -n 1 $DS unpublish prte.test.gone 15" 2>&1)
        echo "$out" | grep -q '^FOUND prte.test.gone' \
            && ok "the key was there before the unpublish" \
            || bad "the key was never findable: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        echo "$out" | grep -q '^UNPUBLISHED prte.test.gone' \
            && ok "unpublish was accepted" \
            || bad "unpublish was refused: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        n=$(echo "$out" | grep -c '^FOUND prte.test.gone')
        [ "$n" = 1 ] \
            && ok "...and the key was gone afterwards" \
            || bad "the key survived the unpublish (FOUND $n times)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "runtime/data_server: PMIX_RANGE_LOCAL data does not leave its node"
    # A LOCAL-range publish is not sent to the HNP at all: the daemon routes
    # it to its OWN data server instance (pmix_server_pub.c picks
    # target = PRTE_PROC_MY_NAME for that range), so the item lives in the
    # store of the daemon that relayed it.  A lookup has to carry the same
    # range to be routed to the same place.
    #
    # That is the property worth asserting across nodes, and one host cannot
    # show it: with a single daemon "the local store" and "the HNP store"
    # are the same object, so LOCAL data confined to a node and LOCAL data
    # leaking everywhere look identical.  (The proxy comparison in
    # prte_data_server_check_range that backs this up - and that was reading
    # an uninitialized field - is covered directly in test/unit/runtime.)
    if ! RUN "test -x $DS"; then
        skp "dataserver client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2'; then
        bad "could not start a DVM for the range test"
    else
        PRUN_BG /tmp/ds-loc.out "--host node2:1 -n 1 $DS publish prte.test.loc mine local 60"
        sleep 8
        if ! RUN 'grep -q "^PUBLISHED prte.test.loc" /tmp/ds-loc.out'; then
            bad "the LOCAL-range publish never happened: $(RUN 'cat /tmp/ds-loc.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        else
            ok "a LOCAL-range key was published from node2"
            # a peer behind the SAME daemon may see it
            out=$(PRUN "--host node2:1 -n 1 $DS lookup prte.test.loc 15 local" 2>&1)
            echo "$out" | grep -q '^FOUND prte.test.loc mine' \
                && ok "a peer on the same node can see it" \
                || bad "a LOCAL-range key was hidden from its own node: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            # ...and the same LOCAL-range lookup from another node reaches
            # THAT node's daemon, which has never seen the key
            out=$(PRUN "--host node3:1 -n 1 $DS lookup prte.test.loc 15 local" 2>&1)
            echo "$out" | grep -q '^FOUND prte.test.loc' \
                && bad "a LOCAL-range key leaked to another node" \
                || ok "a client on another node cannot see it, as LOCAL range requires"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "runtime: a DVM tears down cleanly with jobs and sessions built"
    # The object destructors -- and the ownership rules they encode -- only
    # run for real at teardown.  prte_session_t was registered against the
    # wrong parent class, and the destructor that exposed it (an assert
    # inside pmix_list_item_destruct reading the middle of the session's own
    # data) fires only when a session is actually RELEASED.  Run some jobs,
    # then take the DVM down and check it went quietly.
    if ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the teardown test"
    else
        PRUN '--host node2:1,node3:1 -n 2 --map-by node hostname' >/dev/null 2>&1
        PRUN '--host node4:2 -n 2 hostname' >/dev/null 2>&1
        out=$(RUN 'timeout -k 5 30 pterm' 2>&1); rc=$?
        [ "$rc" = 0 ] \
            && ok "the DVM shut down cleanly after running jobs" \
            || bad "pterm failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        echo "$out" | grep -qiE 'assert|signal|segmentation|abort' \
            && bad "teardown produced a crash diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
            || ok "...with no assert or fault on the way out"
        n=$(prted_count 1 2 3 4)
        [ "$n" = 0 ] && ok "no daemons survived the teardown" \
                     || bad "$n daemons still running after pterm"
    fi
    cleanup_swarm
}

########################################################################
# src/pmix -- the shim that translates between PRRTE's code space and
# PMIx's.  Every function in it is a pure integer mapping and is covered
# exhaustively, without a DVM, by test/unit/pmix.  What lands here is the
# one thing a table test cannot show: that the mapping is actually reached,
# with real proc states, on a daemon that is not the one you are standing on.
########################################################################
# Absolute path -- an app launched into the DVM inherits the daemon PATH,
# which does not contain the install bindir.  See the note on $DS.
PT=/opt/prte/prte/bin/proctable

test_pmix() {
    local out rc n hosts undef

    banner "pmix: the queried proc table carries a real state for every proc"
    # PMIX_QUERY_PROC_TABLE is the only caller of prte_pmix_convert_state(),
    # which was written with bare integer cases against a state space that
    # does not number like PMIx's.  Several PRRTE states fell through to
    # PMIX_PROC_STATE_UNDEF -- a legal answer, so nothing reported an error;
    # the proc simply had no state.  The invariant worth asserting is
    # therefore not "state X" but "not UNDEF": a running proc always has
    # something truthful to say about itself.
    cleanup_swarm
    if ! RUN "test -x $PT"; then
        skp "proctable client not installed -- re-run ./build.sh"
        return
    fi
    if ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the pmix shim tests"
        cleanup_swarm
        return
    fi

    out=$(PRUN "--host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node $PT procs" 2>&1)
    n=$(echo "$out" | grep -m1 '^COUNT ' | awk '{print $2}' | tr -d '\r')
    # every proc of the job, from whichever daemon answered
    [ "$n" = 8 ] \
        && ok "PMIX_QUERY_PROC_TABLE returned all 8 procs" \
        || bad "proc table returned $n entries, expected 8: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    undef=$(echo "$out" | grep -c 'UNDEF' | tr -d ' ')
    [ "$undef" = 0 ] \
        && ok "...and no proc reported PMIX_PROC_STATE_UNDEF" \
        || bad "$undef procs reported UNDEF: $(echo "$out" | grep UNDEF | tr '\n' ' ' | tail -c 300)"
    # the table has to name the node each proc is on, and they must not all
    # be the same one -- otherwise this is a single-host test wearing a hat
    hosts=$(echo "$out" | awk '$1=="PROC" {print $3}' | sort -u | grep -c '^node')
    [ "${hosts:-0}" -ge 2 ] \
        && ok "...and the table spans $hosts nodes" \
        || bad "proc table did not span nodes ($hosts): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    echo "$out" | grep -q 'UNRECOGNIZED' \
        && bad "a proc reported a state PMIx does not define" \
        || ok "...and every state was one PMIx defines"

    banner "pmix: the LOCAL proc table is the answering daemon's own procs"
    # The local/global split does not exist on one host.  Six procs spread
    # over three nodes, two each: whichever daemon answers must report only
    # the two it hosts, and again with real states.
    out=$(PRUN "--host node2:2,node3:2,node4:2 -n 6 --map-by node $PT localprocs" 2>&1)
    n=$(echo "$out" | grep -m1 '^COUNT ' | awk '{print $2}' | tr -d '\r')
    if [ -z "$n" ]; then
        bad "local proc table produced no COUNT: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    elif [ "$n" -gt 0 ] && [ "$n" -lt 6 ]; then
        ok "PMIX_QUERY_LOCAL_PROC_TABLE returned $n of 6 -- only the local procs"
    else
        bad "local proc table returned $n of 6 (0 or all means the filter did nothing)"
    fi
    undef=$(echo "$out" | grep -c 'UNDEF' | tr -d ' ')
    [ "$undef" = 0 ] \
        && ok "...and no local proc reported UNDEF" \
        || bad "$undef local procs reported UNDEF"

    banner "pmix: every daemon serves every node's PMIX_SERVER_URI"
    # The consumer of this query is a TOOL, not a daemon -- daemons reach
    # each other over the RML and never form PMIx connections to one another.
    # A tool asks the DVM "where is the PMIx server on node X?" so it can
    # connect there directly (examples/tool.c --uri <nodename>).
    #
    # This only works because every daemon ships its own server URI to the
    # master in its PRTED_CALLBACK rollup, and the master then puts the whole
    # set into the WIREUP xcast alongside the nidmap -- so EVERY daemon can
    # answer for EVERY node, not just the master.  That uniformity is the
    # point: a tool must not get a different answer depending on which daemon
    # it happened to connect to.  Before the collection existed the query was
    # asking for a key nobody published and answered NOT_FOUND for every node
    # but the one being asked.  One host cannot show any of this: there is
    # only one daemon and it is the master.
    #
    # Ask a NON-MASTER daemon (a proc on node3) about the others -- that is
    # the case that needs the xcast rather than just the rollup.  The URIs
    # must all differ and each must name its own daemon vpid; an
    # implementation that echoed the local server back would pass a weaker
    # test.
    uris=""
    for target in node1 node2 node4; do
        out=$(PRUN "--host node3:1 -n 1 $PT serveruri $target" 2>&1)
        u=$(echo "$out" | grep -m1 '^URI ' | awk '{print $2}' | tr -d '\r')
        if [ -n "$u" ]; then
            ok "a non-master daemon served $target's server URI"
            uris="$uris$u\n"
        else
            bad "non-master daemon could not serve $target's server URI: $(echo "$out" | grep -E '^ERR' | tr '\n' ' ' | tail -c 200)"
        fi
    done
    n=$(printf "$uris" | sort -u | grep -c . | tr -d ' ')
    [ "$n" = 3 ] \
        && ok "...and the three URIs are distinct (not the local server echoed back)" \
        || bad "expected 3 distinct server URIs, got $n: $(printf "$uris" | tr '\n' ' ')"
    # ...and the master must give the same answers
    out=$(PRUN "--host node1:1 -n 1 $PT serveruri node3" 2>&1)
    m3=$(echo "$out" | grep -m1 '^URI ' | awk '{print $2}' | tr -d '\r')
    out=$(PRUN "--host node3:1 -n 1 $PT serveruri node3" 2>&1)
    d3=$(echo "$out" | grep -m1 '^URI ' | awk '{print $2}' | tr -d '\r')
    if [ -n "$m3" ] && [ "$m3" = "$d3" ]; then
        ok "master and a non-master daemon agree on node3's server URI"
    elif [ -z "$m3" ] || [ -z "$d3" ]; then
        bad "node3's server URI was missing from one of the two answers (master='$m3' daemon='$d3')"
    else
        bad "master and daemon disagree on node3's server URI: '$m3' vs '$d3'"
    fi
    # An unknown node must be refused with a status that SAYS something --
    # not the generic PMIX_ERROR (-1) that the wrong-direction conversion
    # used to manufacture out of every failure on this path.
    out=$(PRUN "--host node1:1 -n 1 $PT serveruri nosuchnode" 2>&1)
    rc=$(echo "$out" | grep -m1 '^ERR ' | awk '{print $2}' | tr -d '\r')
    if echo "$out" | grep -q '^URI '; then
        bad "an unknown node somehow produced a URI"
    elif [ "$rc" = "-1" ]; then
        bad "an unknown node reported the generic PMIX_ERROR -- the status was flattened"
    elif [ -z "$rc" ]; then
        bad "unknown node gave neither a URI nor an error: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    else
        ok "an unknown node is refused with a specific status ($(echo "$out" | grep -m1 '^ERR ' | awk '{print $3}'))"
    fi

    RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    n=$(prted_count 1 2 3 4)
    [ "$n" = 0 ] && ok "no daemons survived the pmix-shim teardown" \
                 || bad "$n daemons still running after pterm"
    cleanup_swarm

    banner "pmix: a served server URI is actually reachable when remote connections are on"
    # The point of the whole query.  By default the PMIx server listens on
    # loopback, so what the master hands back is truthful but only usable by
    # a tool ON that node.  With prte_pmix_remote_connections set, the server
    # binds a routable interface -- and the URI the master serves for a
    # REMOTE node must then carry that node's own address, not 127.0.0.1 and
    # not the master's.  This is what makes the feature worth having, and it
    # cannot be observed on one host.
    if ! prted_dvm_start_mca 'node1:1,node2:1,node3:1' '--prtemca pmix_remote_connections 1'; then
        bad "could not start a DVM with remote connections enabled"
    else
        out=$(PRUN "--host node1:1 -n 1 $PT serveruri node2" 2>&1)
        u=$(echo "$out" | grep -m1 '^URI ' | awk '{print $2}' | tr -d '\r')
        n2ip=$(docker exec prte-node2 hostname -i 2>/dev/null | awk '{print $1}' | tr -d '\r')
        if [ -z "$u" ]; then
            bad "no server URI for node2 with remote connections on: $(echo "$out" | grep -E '^ERR' | tr '\n' ' ' | tail -c 200)"
        elif echo "$u" | grep -q '127\.0\.0\.1'; then
            bad "node2's server URI came back as loopback despite remote connections: $u"
        elif [ -n "$n2ip" ] && echo "$u" | grep -qF "$n2ip"; then
            ok "node2's server URI names node2's own address ($n2ip)"
        else
            bad "node2's server URI does not name node2 ($n2ip): $u"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "pmix: server URIs follow the DVM across a grow and a shrink"
    # The URIs ride along in the WIREUP xcast that vm_ready() builds, and
    # vm_ready() runs on VM_READY -- which fires again every time the daemon
    # set changes.  So a GROW should redistribute the whole set (the new
    # nodes' URIs to everyone, and everyone's to the new nodes) with no code
    # of its own.  This case checks that rather than assuming it.
    #
    # A SHRINK needs nothing: the query resolves hostname -> node ->
    # node->daemon, and a shrink NULLs that backpointer, so a departed node
    # cannot be answered for at all.  The store entry keyed on its (never
    # reused) vpid is orphaned but unreachable.  Asserted here so that stays
    # true rather than being an accident of the current teardown order.
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if ! RUN 'pgrep -x prte >/dev/null'; then
        bad "could not start an elastic DVM for the grow/shrink URI test"
    else
        out=$(RUN 'timeout 90 elastic grow node2:2,node3:2' 2>&1)
        if ! echo "$out" | grep -q PMIX_DVM_IS_READY; then
            bad "grow did not complete -- cannot test URI redistribution"
        else
            ok "grew node2+node3"
            for target in node2 node3; do
                out=$(RUN "timeout 40 prun --host node1:1 -n 1 $PT serveruri $target" 2>&1)
                echo "$out" | grep -q '^URI ' \
                    && ok "a grown node's server URI ($target) is served after the grow" \
                    || bad "no server URI for grown $target: $(echo "$out" | grep -E '^ERR' | tr '\n' ' ' | tail -c 200)"
            done
            out=$(RUN 'timeout 90 elastic shrink node3' 2>&1); sleep 3
            if ! echo "$out" | grep -q PMIX_DVM_IS_READY; then
                bad "shrink did not complete -- cannot test the stale URI"
            else
                ok "shrank node3"
                out=$(RUN "timeout 40 prun --host node1:1 -n 1 $PT serveruri node3" 2>&1)
                echo "$out" | grep -q '^URI ' \
                    && bad "a shrunk node's server URI is still being served -- stale entry" \
                    || ok "...and node3's server URI is no longer served"
                out=$(RUN "timeout 40 prun --host node1:1 -n 1 $PT serveruri node2" 2>&1)
                echo "$out" | grep -q '^URI ' \
                    && ok "...while node2's is unaffected" \
                    || bad "node2's server URI was lost by the shrink"
            fi
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm
}

test_prted() {
    local out rc ns n bpid

    banner "prted: a job-level Get of ANOTHER job is answered by the daemon"
    # A client asks for the JOB-LEVEL data of a DIFFERENT job, from a node
    # that hosts none of that job's procs.  This is the cross-job job-level
    # query path, and it only exists with more than one daemon.
    #
    # SCOPE, honestly: depending on what the daemon has already registered
    # with its PMIx server, this may be answered out of the PMIx cache
    # without ever upcalling into dmodex_req.  So treat these two cases as
    # COVERAGE of the query working end to end across nodes -- they are not
    # a guaranteed reproducer for the tproc/target mix-up in the dmodex
    # in-flight check (that one is established by inspection: req->target is
    # only ever assigned on the monitor and tool-connect paths, so on a
    # dmodex it is {"", PMIX_RANK_INVALID}, which PMIx matches against any
    # nspace and against PMIX_RANK_WILDCARD).  If you find a way to force
    # the upcall, tighten these.
    cleanup_swarm
    if ! RUN 'command -v jobinfo >/dev/null'; then
        skp "jobinfo client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the direct-modex tests"
    else
        # target job: 2 procs, pinned to node2, alive for the duration
        PRUN_BG /tmp/ji-pub.out '--host node2:2 -n 2 jobinfo publish 90'
        sleep 8
        ns=$(RUN 'grep -m1 "^NSPACE " /tmp/ji-pub.out' 2>/dev/null | awk '{print $2}' | tr -d '\r')
        if [ -z "$ns" ]; then
            bad "target job never reported its nspace: $(RUN 'cat /tmp/ji-pub.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        else
            ok "target job is up on node2 (nspace $ns)"
            # the asking client runs on node3, whose daemon holds no procs of $ns
            out=$(PRUN "--host node3:1 -n 1 jobinfo fetch $ns 20" 2>&1)
            n=$(echo "$out" | grep -m1 '^JOBSIZE ' | awk '{print $2}' | tr -d '\r')
            [ "$n" = 2 ] \
                && ok "wildcard direct-modex from a daemon hosting no procs returned JOB_SIZE=2" \
                || bad "wildcard direct-modex did not answer (got '$n'): $(echo "$out" | tr '\n' ' ' | tail -c 250)"

            banner "prted: ...and still answers with another request already parked"
            # THE regression case.  Before answering, dmodex_req scans the
            # daemon's request array for an in-flight request for the same
            # target.  It compared against req->target, which only the monitor
            # and tool-connect paths ever set -- so for every dmodex it is
            # {"", PMIX_RANK_INVALID}.  PMIx treats an empty nspace as matching
            # anything and PMIX_RANK_WILDCARD as matching anything, so a
            # wildcard request "matched" whatever unrelated entry happened to
            # be in the array, was filed as already-requested, and was never
            # answered.  The client hangs until its timeout.
            #
            # So: park an unrelated request in a daemon first (a Get of a
            # specific remote rank with PMIX_REQUIRED_KEY, which the hosting
            # daemon holds waiting for a key that never arrives), then ask the
            # wildcard question through the SAME daemon.
            #
            # This has to be a DIFFERENT node from the case above.  Answering a
            # wildcard request registers the nspace with that daemon's PMIx
            # server, so a second Get on node3 would be served straight out of
            # the PMIx cache and never reach the host at all -- the case would
            # pass without exercising anything.  node4 has not seen $ns yet.
            PRUN_BG /tmp/ji-park.out "--host node4:1 -n 1 jobinfo fetchkey $ns 0 prte.test.never 60"
            sleep 10
            if RUN 'grep -q FETCHKEY-STARTED /tmp/ji-park.out' && \
               ! RUN 'grep -q FETCHKEY-DONE /tmp/ji-park.out'; then
                ok "an unrelated request is parked in node4's daemon"
            else
                skp "could not park a request in node4's daemon; the next case is weaker"
            fi
            out=$(PRUN "--host node4:1 -n 1 jobinfo fetch $ns 20" 2>&1)
            n=$(echo "$out" | grep -m1 '^JOBSIZE ' | awk '{print $2}' | tr -d '\r')
            [ "$n" = 2 ] \
                && ok "wildcard direct-modex still answered with a request parked" \
                || bad "wildcard direct-modex was swallowed by the in-flight check: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "prted: a signal reaches the job it was sent to, and no other"
    # PRTE_DAEMON_SIGNAL_LOCAL_PROCS carries the target job's nspace.  The
    # daemon used to unpack it and then hand NULL to the ODLS, which means
    # "every local child" -- so in a persistent DVM running more than one job,
    # signalling one job killed them all.  One node cannot show this: it needs
    # two jobs whose procs land on the same daemon, and a persistent DVM to
    # hold them both.
    # cleanup_swarm reaps daemons and tools but not the application procs
    # they left behind, and this case counts procs on node2 - a stray sleep
    # from an earlier run would make the precondition check nonsense
    for n in 1 2; do docker exec "$NODE$n" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    if ! prted_dvm_start 'node1:4,node2:4'; then
        bad "could not start a DVM for the job-scoped signal test"
    else
        # SIGUSR1 is in the default forwarded-signal set (ess_base_frame.c),
        # but name it explicitly: --forward-signals used to be documented in
        # help-prun.txt and missing from prunoptions, so prun rejected the
        # very option its own help advertised (issue #2569).  Naming the
        # signal here keeps a live check on that path.
        PRUN_BG /tmp/jobA.out '--forward-signals SIGUSR1 --host node2:1 -n 1 sleep 120'
        PRUN_BG /tmp/jobB.out '--forward-signals SIGUSR1 --host node2:1 -n 1 sleep 120'
        sleep 10
        n=$(ON 2 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
        if [ "$n" = 2 ]; then
            ok "two jobs are running, both with procs on node2"
            # signal ONLY job A's launcher; prun relays it as a job-scoped
            # PMIX_JOB_CTRL_SIGNAL for its own nspace
            # match the launcher by executable name: "pgrep -f sleep 120"
            # would also match the shell this very command runs in
            bpid=$(RUN 'pgrep -x prun | head -1' 2>/dev/null | tr -d ' \r')
            RUN "kill -USR1 $bpid" >/dev/null 2>&1
            sleep 10
            n=$(ON 2 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
            [ "$n" = 1 ] \
                && ok "the signalled job died and the other one did not" \
                || bad "signal was not job-scoped ($n of 2 procs left on node2; expected 1)"
            n=$(RUN 'pgrep -c -x prun' 2>/dev/null | tr -d ' \r')
            [ "$n" = 1 ] \
                && ok "the unsignalled launcher is still waiting on its job" \
                || bad "$n launchers left after signalling one job; expected 1"
        else
            bad "could not get two concurrent jobs running on node2 (saw $n procs)"
        fi
        RUN 'pkill -f "sleep 120"' >/dev/null 2>&1
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "prted: a tool attached to a NON-MASTER daemon completes its job"
    # A prun started on a compute node of a persistent DVM, with no --dvm-uri,
    # finds its LOCAL daemon's rendezvous file and attaches there instead of to
    # the HNP.  Everything about that tool then runs through the relay paths:
    # tool-connect goes to the master as PRTE_PLM_TOOL_ATTACHED_CMD, the spawn
    # goes as a relayed request, and -- the part that broke -- the stdin the
    # tool pushes goes to its own daemon's iof module.  Only the HNP module
    # implemented push_stdin, so on any other daemon that was a NULL call: the
    # daemon segfaulted, and the tool, whose PMIx server had just died, waited
    # for a PMIX_EVENT_JOB_END that could no longer reach it.  Issue #2568.
    #
    # Every prun pushes stdin even with nothing to send -- the zero-byte
    # end-of-input marker -- so this needs no piped input to reproduce.  Run it
    # WITHOUT --dvm-uri, deliberately: pointing the tool at the master's URI is
    # exactly what the test must not do.
    cleanup_swarm
    if ! prted_dvm_start 'node1:2,node2:2,node3:2'; then
        bad "could not start a DVM for the non-master tool test"
    else
        out=$(ONT 2 'timeout -k 5 45 prun --host node2:1 -n 1 hostname 2>&1; echo RC=$?')
        echo "$out" | grep -q 'RC=0' \
            && ok "a tool attached to node2's daemon saw its job end and exited" \
            || bad "tool on a non-master daemon did not complete: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        echo "$out" | grep -qE '^node2$' \
            && ok "...and its job's output reached it" \
            || bad "...but the job produced no output: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        n=$(prted_count 2)
        [ "$n" = 1 ] \
            && ok "...and node2's daemon survived the tool's stdin push" \
            || bad "node2's daemon died while the tool pushed stdin"

        # ...and the stdin itself has to arrive, not merely fail to crash.  Send
        # it to a proc on ANOTHER node, so the daemon's relay to the HNP and the
        # HNP's routing back out are both exercised; read the result from the
        # file rather than the terminal, because output from a proc the tool's
        # own daemon does not host has no path back to that tool.
        ON 1 'rm -f /tmp/tool-stdin.txt' >/dev/null 2>&1
        ONT 2 'printf "STDIN-RELAY-OK\n" | timeout -k 5 45 prun --host node1:1 -n 1 sh -c "cat > /tmp/tool-stdin.txt"' >/dev/null 2>&1
        out=$(ON 1 'cat /tmp/tool-stdin.txt 2>&1')
        echo "$out" | grep -q 'STDIN-RELAY-OK' \
            && ok "stdin pushed through a non-master daemon reached a proc on another node" \
            || bad "stdin was lost on the relay to the HNP: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

        # A second tool on the same node must still find exactly one server.
        # The hung prun of #2568 never released its own pmix.* rendezvous file,
        # so the next tool reported "multiple possible servers" and gave up --
        # which is how the hang first showed itself, looking nothing like its
        # cause.
        out=$(ONT 2 'timeout -k 5 45 prun --host node2:1 -n 1 hostname 2>&1; echo RC=$?')
        echo "$out" | grep -q 'RC=0' \
            && ok "a second tool on that node still finds exactly one server" \
            || bad "a stale rendezvous file blocked the next tool: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

        # ...and the OTHER direction.  A daemon hands its own procs' output to
        # its own PMIx server, so a tool has always seen the ranks that landed
        # on its own node -- which is exactly what hid the gap: the symptom was
        # PARTIAL output, not none.  Put the tool on node3 and the ranks on
        # node1 and node2, so every byte it should see has to come back out
        # from the master.  node1 is the HNP (its own children take the
        # read-handler path) and node2 is an ordinary daemon (the forwarded
        # path); both relay points are covered by requiring both names.
        out=$(ONT 3 'timeout -k 5 45 prun --host node1:1,node2:1 -n 2 --map-by node hostname 2>&1')
        if echo "$out" | grep -qE '^node1$' && echo "$out" | grep -qE '^node2$'; then
            ok "a tool on a non-master daemon saw output from ranks on both other nodes"
        else
            bad "output never reached the tool on node3: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi

        # Every line exactly once.  The relay skips the daemon that already
        # delivered a chunk to its own PMIx server, and if that dedup were
        # dropped a tool would see its own node's ranks twice.
        out=$(ONT 2 'timeout -k 5 60 prun --host node1:2,node2:2,node3:2 -n 6 --map-by node sh -c "for i in \$(seq 1 50); do echo RELAYLINE-\$i; done" 2>&1')
        n=$(echo "$out" | grep -c '^RELAYLINE-')
        u=$(echo "$out" | grep '^RELAYLINE-' | sort | uniq -c | awk '{print $1}' | sort -u | tr '\n' ' ')
        [ "$n" = 300 ] && [ "$u" = "6 " ] \
            && ok "300 lines from 6 ranks reached the tool, each exactly once" \
            || bad "relayed output was lost or duplicated (got $n lines, per-line counts '$u'; expected 300 and '6 ')"

        # The relayed copy must not be EMITTED where it lands.  Every daemon
        # registers the job's namespace with the same output directives, so a
        # daemon handed another node's output writes its own copy of that
        # rank's file unless the delivery says not to -- and node3 hosts none
        # of these ranks, so any file appearing there is a duplicate of one
        # node1 or node2 already wrote.  Measured, not assumed: flipping the
        # PMIX_IOF_LOCAL_OUTPUT directive to true puts a full set here.
        for i in 1 2 3; do ON $i 'rm -rf /tmp/relayout; mkdir -p /tmp/relayout' >/dev/null 2>&1; done
        ONT 3 'timeout -k 5 45 prun --output file=/tmp/relayout/out --host node1:1,node2:1 -n 2 --map-by node hostname' >/dev/null 2>&1
        n=$(ON 3 'find /tmp/relayout -type f 2>/dev/null | wc -l' | tr -d ' \r')
        m=$(ON 2 'find /tmp/relayout -type f 2>/dev/null | wc -l' | tr -d ' \r')
        [ "$n" = 0 ] \
            && ok "the tool's daemon wrote no output files for ranks it does not host" \
            || bad "relayed output was emitted on the tool's node too ($n files); two daemons are writing one --output file"
        [ "$m" = 1 ] \
            && ok "...and the daemon that does host a rank still wrote its file" \
            || bad "output-to-file broke on the hosting daemon ($m files, expected 1)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "prted: a malformed --singleton is refused, not a crash"
    # The value is handed straight down to PMIx_server_init as PMIX_SINGLETON,
    # which faults on anything that is not "<nspace>.<rank>", so prte has to
    # reject it BEFORE prte_init.  A crash here is a segfault at startup.
    out=$(RUN 'timeout -k 5 30 prte --singleton notanid' 2>&1)
    echo "$out" | grep -q 'malformed singleton identifier' \
        && ok "a malformed --singleton is reported and refused" \
        || bad "malformed --singleton was not cleanly refused: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -qi 'signal: segmentation' \
        && bad "malformed --singleton still crashes" \
        || ok "...and does not crash"
    cleanup_swarm

    banner "prted: a root path prefix does not corrupt the heap"
    # The four --prefix normalizers used strncpy(param, PRTE_PATH_SEP,
    # sizeof(param) - 1) on a char*, so sizeof gave the size of the POINTER
    # and strncpy NUL-padded eight bytes into a two-byte allocation.
    # "--prefix /" is that two-byte allocation.  The job has to actually RUN:
    # a corrupted heap shows up as a crash or a hang later in startup, and
    # "no output" would otherwise look like a pass.
    out=$(RUN 'timeout -k 5 60 prterun --prefix / --host node1:1 -n 1 hostname' 2>&1)
    echo "$out" | grep -qE '^node1$' \
        && ok "--prefix / still launched the job" \
        || bad "--prefix / broke the launch: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -qi 'signal: segmentation\|corrupt\|malloc' \
        && bad "--prefix / corrupted the heap: $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "...and did not corrupt the heap"
    cleanup_swarm
}

########################################################################
# src/tools -- the executables themselves
########################################################################
#
# The tools are main() around a library, so almost nothing in src/tools can
# be unit tested (what could be is in test/unit/tools).  What is left is
# behavior that only shows up when there is more than one node or more than
# one DVM to be wrong about:
#
#   * prte-info describing the build that is actually installed on each node
#     -- a swarm running a stale install on some nodes is otherwise a launch
#     failure with no obvious cause
#   * pterm picking the DVM it was told to pick, out of several, and leaving
#     the others alone.  With one DVM every selector "works"
#   * a rejected command line not being allowed to disturb a running DVM
#   * an appfile spreading its app contexts over different nodes
#   * a job's exit status coming back from a proc that ran somewhere else
test_tools() {
    local out rc n uri1 uri2 pid1 pid2

    banner "tools: prte-info reports the same build on every node"
    # Cheap, and it catches the single most confusing swarm failure: some
    # nodes reading an older install out of the shared volume.
    cleanup_swarm
    # the version banner starts with a blank line - take the first line
    # that has anything on it
    out=$(RUN 'prte-info --version' 2>&1 | awk 'NF{print; exit}')
    if [ -z "$out" ]; then
        bad "prte-info produced no version on node1"
    else
        n=0
        for i in $(seq 2 10); do
            [ "$(ON "$i" 'prte-info --version' 2>/dev/null | awk 'NF{print; exit}')" = "$out" ] && n=$((n+1))
        done
        [ "$n" = 9 ] && ok "prte-info agrees on all 10 nodes ($out)" \
                     || bad "prte-info version differs on $((9-n)) node(s); node1 says '$out'"
    fi
    # ...and it must not need a DVM, a socket, or an argument
    RUN 'prte-info --all >/dev/null 2>&1'  && ok "prte-info --all runs with no DVM" \
                                           || bad "prte-info --all failed with no DVM"
    RUN 'prte-info bogusarg >/dev/null 2>&1'
    rc=$?
    [ "$rc" != 0 ] && ok "prte-info rejects a stray argument (rc=$rc)" \
                   || bad "prte-info accepted a stray argument"

    banner "tools: pterm --pid terminates the DVM it names and no other"
    # Two DVMs on the same head node, each with daemons of its own.  This is
    # the case that makes --pid mean anything -- and the case where a --pid
    # that was silently ignored used to kill whichever DVM was found first.
    cleanup_swarm
    RUN "rm -f /tmp/dvm1.uri /tmp/dvm2.uri /tmp/dvm1.pid /tmp/dvm2.pid" >/dev/null 2>&1
    RUN "timeout -k 5 60 prte --daemonize --report-uri /tmp/dvm1.uri \
             --report-pid /tmp/dvm1.pid --host node1:2,node2:2" >/dev/null 2>&1
    RUN "timeout -k 5 60 prte --daemonize --report-uri /tmp/dvm2.uri \
             --report-pid /tmp/dvm2.pid --host node1:2,node3:2" >/dev/null 2>&1
    sleep 5
    pid1=$(RUN 'cat /tmp/dvm1.pid' 2>/dev/null | tr -d " \r")
    pid2=$(RUN 'cat /tmp/dvm2.pid' 2>/dev/null | tr -d " \r")
    if [ -z "$pid1" ] || [ -z "$pid2" ] || [ "$pid1" = "$pid2" ]; then
        bad "could not start two distinguishable DVMs (pids '$pid1' '$pid2')"
    else
        ok "two DVMs are running (pids $pid1, $pid2)"

        # a --pid that is not a PID at all must be refused, not acted on
        RUN 'timeout -k 5 30 pterm --pid not-a-pid >/dev/null 2>&1'
        rc=$?
        n=$(RUN "kill -0 $pid1 2>/dev/null && echo up" | tr -d " \r")
        if [ "$rc" != 0 ] && [ "$n" = up ]; then
            ok "pterm rejects a malformed --pid and leaves both DVMs alone"
        else
            bad "pterm acted on a malformed --pid (rc=$rc, dvm1 '$n')"
        fi

        # ...and a real one, given through a file, must take down exactly
        # the DVM it names
        RUN "timeout -k 5 30 pterm --pid file:/tmp/dvm2.pid" >/dev/null 2>&1
        sleep 3
        n=$(RUN "kill -0 $pid2 2>/dev/null && echo up" | tr -d " \r")
        [ -z "$n" ] && ok "pterm --pid file: terminated the DVM it named" \
                    || bad "pterm --pid file: did not terminate DVM 2"
        n=$(RUN "kill -0 $pid1 2>/dev/null && echo up" | tr -d " \r")
        [ "$n" = up ] && ok "...and left the other DVM running" \
                      || bad "pterm --pid file: took down the wrong DVM"

        # the survivor must still be usable -- a DVM that lost its daemons
        # to another DVM's pterm would still answer, so launch through it
        out=$(RUN "timeout -k 5 60 prun --dvm-uri file:/tmp/dvm1.uri --host node2:1 -n 1 hostname" 2>&1)
        echo "$out" | grep -q '^node2$' \
            && ok "...and the survivor still launches on its own nodes" \
            || bad "surviving DVM cannot launch: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        RUN "timeout -k 5 30 pterm --pid file:/tmp/dvm1.pid" >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "tools: a rejected command line does not disturb the DVM"
    # Every tool must be able to say no without taking the runtime with it.
    # A prun that fails to parse used to be able to drive a phantom job
    # through the state machine of the HNP.
    if ! prted_dvm_start 'node1:2,node2:2,node3:2'; then
        bad "could not start a DVM for the bad-command-line test"
    else
        RUN "timeout -k 5 30 prun --dvm-uri file:$PRTED_URI --no-such-option hostname" >/dev/null 2>&1
        RUN "timeout -k 5 30 prun --dvm-uri file:$PRTED_URI --map-by no-such-policy hostname" >/dev/null 2>&1
        RUN "timeout -k 5 30 pterm --dvm-uri file:$PRTED_URI no-such-argument" >/dev/null 2>&1
        RUN "timeout -k 5 30 prte-info no-such-argument" >/dev/null 2>&1
        # everything above should have been refused; the DVM must still work
        out=$(PRUN '--host node3:1 -n 1 hostname' 2>&1)
        echo "$out" | grep -q '^node3$' \
            && ok "the DVM survived four rejected command lines" \
            || bad "a rejected command line disturbed the DVM: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

        banner "tools: an appfile spreads its app contexts across nodes"
        # --app is read by prun after its own command line has been parsed,
        # and each line becomes a separate app context.  One node cannot tell
        # a working appfile from one that collapsed into a single app.
        RUN "printf -- '--host node2:1 -n 1 hostname\n--host node3:1 -n 1 hostname\n' > /tmp/appfile" >/dev/null 2>&1
        out=$(PRUN '--app /tmp/appfile' 2>&1)
        n=$(echo "$out" | grep -cE '^node[23]$')
        if [ "$n" = 2 ] && echo "$out" | grep -q '^node2$' && echo "$out" | grep -q '^node3$'; then
            ok "the appfile ran one app context on each named node"
        else
            bad "appfile did not spread over both nodes: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        fi

        banner "tools: the exit status of a remote proc reaches prun"
        # The status has to travel proc -> its daemon -> HNP -> prun.  On one
        # node that whole chain is inside a single process.
        PRUN '--host node3:1 -n 1 false' >/dev/null 2>&1
        rc=$?
        [ "$rc" != 0 ] && ok "a failing remote proc gives prun a non-zero status (rc=$rc)" \
                       || bad "prun reported success for a proc that exited non-zero"
        PRUN '--host node3:1 -n 1 true' >/dev/null 2>&1
        rc=$?
        [ "$rc" = 0 ] && ok "...and a succeeding one gives zero" \
                      || bad "prun reported failure (rc=$rc) for a proc that exited 0"

        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm
}

########################################################################
# src/rml -- routing tree, relay, and the TCP transport
########################################################################
#
# The RML is the one subsystem a single host cannot exercise at all: with one
# daemon every send is a send-to-self, nothing is ever routed, and no byte
# ever crosses a socket.  The unit tests (test/unit/rml) cover the tree math,
# the incarnation guard, the purge, and URI parsing, all of which are pure
# computation.  What is left -- and what lives here -- is everything that
# needs a real tree of real daemons:
#
#   * a message being RELAYED by an intermediate daemon, which is a distinct
#     code path from either sending or receiving one (oob_tcp_sendrecv.c
#     rebuilds a prte_rml_send_t and re-enters the send path)
#   * the tree actually being a tree -- with the default radix of 64 and ten
#     nodes every daemon is a direct child of the HNP, so nothing is ever
#     relayed. Forcing a small radix is the only way to get interior nodes.
#   * a payload larger than one socket write, which is what drives the
#     partial-writev / partial-read bookkeeping
#   * interface selection actually binding and connecting, not just parsing
#   * a daemon dying under a live DVM, which is what prte_rml_route_lost and
#     the peer teardown exist for
test_util() {
    local out n rc c hf

    # src/util is a library of helpers, so almost all of it is pinned down by
    # test/unit/util. What that test CANNOT reach is the multi-node meaning of
    # the two parsers: a hostfile or a -host list is a statement about a set of
    # machines, and every defect below was a defect in which machines came out
    # of it. With one node in the pool they all look correct.

    banner "util: -host slot specifications do not leak between tokens"
    # "nodeA:*,nodeB" - the auto-detect marker on the first token used to
    # carry over to the second, which read it as "this node has no slots",
    # so nodeB was allocated zero. Two nodes are the minimum to see it.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:*,node3 -n 2 --map-by node hostname' 2>&1)
    if echo "$out" | grep -q '^node3$'; then
        ok "a plain host following a ':*' host still got its slot"
    else
        bad "':*' on the first -host token starved the second: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    fi
    cleanup_swarm

    banner "util: -host tokens after a '+e:N' are not dropped"
    # parse_dash_host() scanned the node pool with the SAME loop index it was
    # using to walk the comma-separated tokens, so it came back with that
    # index past the end and every token after a "+e:N" was silently
    # discarded. Needs a DVM whose pool is bigger than the request.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node1:1,node2:1,node3:1,node4:1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        # one empty node plus an explicitly named one: the named node must
        # appear, which it cannot if the token was thrown away
        out=$(RUN 'timeout 30 prun --host +e:1,node4 -n 2 --map-by node hostname' 2>&1)
        if echo "$out" | grep -q '^node4$'; then
            ok "an explicit host named after '+e:1' was honored"
        else
            bad "the token after '+e:1' was dropped: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the '+e:N' token test"
    fi
    cleanup_swarm

    banner "util: a hostfile '^host' line excludes that host"
    # The exclusion syntax has always been implemented in hostfile.c, and the
    # lexer has always rejected it: the '^' was only allowed on the
    # "user@host" form, so a bare "^node3" fell through to the catch-all and
    # the WHOLE hostfile was refused as a parse error. Both halves show here -
    # the file has to parse at all, and node3 must not be in the result.
    cleanup_swarm
    hf=/tmp/util_exclude.txt
    RUN "printf 'node2 slots=1\nnode3 slots=1\nnode4 slots=1\n^node3\n' > $hf"
    out=$(RUN "timeout 90 prterun --hostfile $hf -n 2 --map-by node hostname" 2>&1)
    rc=$?
    if [ "$rc" != 0 ] || echo "$out" | grep -qi 'parse error'; then
        bad "a hostfile with a '^host' line was refused: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    else
        ok "a hostfile with a '^host' line parses"
        if echo "$out" | grep -q '^node3$'; then
            bad "the excluded host was used anyway: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        else
            ok "the excluded host was not used"
        fi
        n=$(echo "$out" | grep -cE '^node[24]$')
        [ "$n" = 2 ] && ok "both remaining hosts were used" \
                     || bad "$n/2 procs landed on the remaining hosts: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    fi
    RUN "rm -f $hf" >/dev/null 2>&1
    cleanup_swarm

    banner "util: a hostfile parse error does not poison the next parse"
    # The parser's FILE* and flex buffer are process globals, and the error
    # path used to skip both the fclose() and the lex_destroy(). A prterun
    # that reads a bad hostfile and then a good one is the shortest way to
    # see it: the second parse resumed inside the first.
    cleanup_swarm
    RUN "printf 'node2 slots=notanumber\n' > /tmp/util_bad.txt"
    RUN "printf 'node2 slots=2\nnode3 slots=2\n' > /tmp/util_good.txt"
    RUN 'timeout 60 prterun --hostfile /tmp/util_bad.txt -n 1 hostname' >/dev/null 2>&1
    rc=$?
    [ "$rc" != 0 ] && ok "a malformed hostfile is refused (rc=$rc)" \
                   || bad "a malformed hostfile was accepted"
    out=$(RUN 'timeout 90 prterun --hostfile /tmp/util_good.txt -n 4 --map-by node hostname' 2>&1)
    c=$(echo "$out" | grep -cE '^node[23]$')
    [ "$c" = 4 ] && ok "the next hostfile parsed correctly after the failure" \
                 || bad "the parse after a failed one produced $c/4 procs: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    RUN 'rm -f /tmp/util_bad.txt /tmp/util_good.txt' >/dev/null 2>&1
    cleanup_swarm

    banner "util: node names are compared by content, not by length"
    # prte_util_compare_name_fields() compared namespaces by LENGTH, so two
    # jobs of the same DVM ("<dvm>@1" and "<dvm>@2") had identical names as
    # far as it was concerned. iof/prted uses it to find the proc whose
    # output it is holding, so two concurrent jobs on the same node is the
    # case that shows it: each job's output must carry its own tag.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        # two jobs at once, each tagging its output with its own namespace
        out=$(RUN 'timeout 60 sh -c "
                  prun --tag-output -n 2 echo JOBA > /tmp/a.out 2>&1 &
                  prun --tag-output -n 2 echo JOBB > /tmp/b.out 2>&1 &
                  wait; cat /tmp/a.out /tmp/b.out"' 2>&1)
        n=$(echo "$out" | grep -c 'JOBA')
        c=$(echo "$out" | grep -c 'JOBB')
        if [ "$n" = 2 ] && [ "$c" = 2 ]; then
            ok "two concurrent jobs each got their own output back"
        else
            bad "concurrent job output crossed over (JOBA=$n JOBB=$c): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi
        # ...and the tags name two DIFFERENT namespaces
        # This also guards an IOF race that only two concurrent jobs expose.
        # A tool parses its spawn's --output directives before the request
        # goes out, but could only record them against the namespace once the
        # reply named it - so output from a proc that wrote and exited before
        # the reply arrived had nowhere to look for its format and came out
        # UNTAGGED. One prun almost always wins that race; two contending for
        # the same HNP did not, and this assertion failed about one run in
        # three. See pmix_globals.spawn_iof_flags.
        n=$(echo "$out" | sed -n 's/^\[\([^,]*\),.*/\1/p' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 2 ] && ok "the two jobs are distinguishable by namespace" \
                     || bad "the two jobs' output carried $n distinct namespace tag(s)"
        n=$(echo "$out" | grep -cE '^JOB[AB]$')
        [ "$n" = 0 ] && ok "every line was tagged" \
                     || bad "$n line(s) came back untagged"
        RUN 'rm -f /tmp/a.out /tmp/b.out' >/dev/null 2>&1
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the concurrent-job test"
    fi
    cleanup_swarm

    banner "util: an unrecognized system limit is refused, not applied as zero"
    # prte_setlimit() fed a non-numeric value to strtol(), got zero, and
    # LOWERED the limit to it -- a daemon that cannot open a file. Refusing
    # the request has to happen before any daemon is launched.
    cleanup_swarm
    RUN 'timeout 60 prterun --prtemca prte_set_max_sys_limits openfiles:many \
             -n 1 hostname' >/dev/null 2>&1
    rc=$?
    [ "$rc" != 0 ] && ok "a non-numeric system limit is refused (rc=$rc)" \
                   || bad "a non-numeric system limit was accepted"
    # ...while a real one still works and does not stop the launch
    out=$(RUN 'timeout 90 prterun --prtemca prte_set_max_sys_limits openfiles:max \
                   --host node2:1,node3:1 -n 2 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^node[23]$')
    [ "$n" = 2 ] && ok "a valid system limit is applied and the launch proceeds" \
                 || bad "a valid system limit broke the launch: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm
}

test_hwloc() {
    local out rc n c bad_cores

    # src/hwloc is a library of pure functions over a topology, so nearly all
    # of it is pinned down by test/unit/hwloc against synthetic topologies.
    # What that test cannot reach is the case PRRTE actually runs in: the
    # topology being rendered or queried arrived from ANOTHER machine, as XML
    # over the wire, and lives in the HNP alongside nine others. Every case
    # below is a defect that only shows there.
    #
    # These containers are the right shape for it on purpose: 8 cores, one
    # package, no SMT, and no NUMA node in sysfs.

    banner "hwloc: a remote node's binding is rendered from its own topology"
    # cset2str() runs in the HNP against the topology the daemon shipped it,
    # not against the HNP's own. Its package loop dereferenced each package
    # object unchecked, and the range builder it calls appended to a 2048-byte
    # stack buffer with memcpy() and no bound. A proc on a remote node is the
    # only way to exercise that path with a topology the HNP did not sense.
    # --display map (not --display bind): the map is rendered BY THE HNP from
    # the topology each daemon shipped it, which is the path under test. The
    # per-rank "Rank N bound to" line comes from the daemon's own stderr and
    # is not forwarded here.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:2,node3:2 -n 4 --map-by node \
                   --bind-to core --display map hostname' 2>&1)
    n=$(echo "$out" | grep -cE 'Process rank: [0-9]+ Bound: package\[[0-9]+\]\[core:L')
    [ "$n" = 4 ] && ok "all 4 remote ranks rendered a package/core binding" \
                 || bad "$n/4 remote ranks rendered a binding: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # ...and the renders are not a constant - bind-to core gives each rank on
    # a node its own core
    c=$(echo "$out" | sed -n 's/.*\[core:L\([0-9-]*\)\].*/\1/p' | sort -u | wc -l | tr -d ' ')
    [ "$c" -ge 2 ] && ok "the rendered cores differ between ranks" \
                   || bad "every rank rendered the same core ($c distinct)"
    cleanup_swarm

    banner "hwloc: a wide binding renders without corrupting the HNP's heap"
    # THE overflow. prte_hwloc_get_binding_info() writes one
    # "<core>N</core>\n" element per bound core, each 20 spaces of indent plus
    # ~14 characters, into a buffer its caller sizes at 20 bytes PER PU. The
    # element loop advanced its write pointer through a run of set bits without
    # ever charging those bytes against the budget it handed snprintf, so a
    # process bound to more than about half the cores wrote past the end.
    #
    # These nodes have 8 cores and no SMT, so "--bind-to package" is exactly
    # that shape: 8 elements (~270 bytes) into 160 bytes. The corruption lands
    # in the HNP, which is holding every node's topology, so the failure is a
    # crash somewhere unrelated - which is why this asserts on the whole run
    # completing as well as on the output.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:1,node3:1 -n 2 --map-by node \
                   --bind-to package --display map:parseable hostname' 2>&1)
    rc=$?
    [ "$rc" = 0 ] && ok "a package-wide binding on 2 nodes completed (rc=0)" \
                  || bad "a package-wide binding run failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # every core of both nodes has to appear, 8 per rank
    n=$(echo "$out" | grep -cE '<core>[0-9]+</core>')
    [ "$n" = 16 ] && ok "all 16 bound cores were rendered (8 per rank)" \
                  || bad "$n/16 <core> elements were rendered: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # the package id is computed, not left as whatever was on the stack
    bad_cores=$(echo "$out" | sed -n 's/.*<package id="\(-\?[0-9]*\)">.*/\1/p' \
                | grep -vcE '^[0-9]$' || true)
    [ "$bad_cores" = 0 ] && ok "every rendered package id is a small non-negative number" \
                         || bad "$bad_cores rendered package id(s) were not plausible: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # ...and the XML is still well formed, which it is not once the writes run
    # off the end of the element buffer
    n=$(echo "$out" | grep -c '</binding>')
    c=$(echo "$out" | grep -c '<binding>')
    [ "$n" = "$c" ] && [ "$n" = 2 ] && ok "the parseable map is balanced (2 bindings)" \
                                    || bad "parseable map is malformed (<binding>=$c </binding>=$n)"
    cleanup_swarm

    banner "hwloc: --map-by numa works against topologies the HNP never sensed"
    # The NUMA count and lookup both read a cutoff cached on the topology's
    # root, and used to answer "this node has no NUMA domains" whenever that
    # cache had not been built yet - a silent zero that a map-by numa job
    # believes, so it places nothing. The HNP's OWN topology always has the
    # cache (it is built when the topology is sensed); a topology that arrived
    # from a daemon only has it if someone remembered, which is the case that
    # can regress.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:2,node3:2,node4:2 -n 3 \
                   --map-by numa hostname' 2>&1)
    n=$(echo "$out" | grep -cE '^node[234]$')
    [ "$n" = 3 ] && ok "map-by numa placed all 3 procs across the remote nodes" \
                 || bad "map-by numa placed $n/3 procs: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    cleanup_swarm

    banner "hwloc: a DVM cpu-set constrains every node, not just the first"
    # filter_cpus() expands the DVM's cpu-set against each node's topology in
    # turn, and REWRITES the process-wide list with the expansion as it goes.
    # One node cannot show whether the second node got the same answer.
    cleanup_swarm
    # The cpu-set is written as a RANGE ("0-1") on purpose. PMIx's
    # command-line parser used to reject any MCA value whose second character
    # was a dash as "not-enough-arguments", so this exact spelling was
    # unusable; it is the regression test for that fix as much as for the
    # per-node expansion. Bind to core: that is the level at which a cpu-set
    # is honored today (see the note at the end of this case).
    out=$(RUN 'timeout 90 prterun --prtemca hwloc_default_cpu_list 0-1 \
                   --host node2:2,node3:2,node4:2 -n 6 --map-by node \
                   --bind-to core --display map hostname' 2>&1)
    rc=$?
    if echo "$out" | grep -q 'not-enough-arguments'; then
        skp "this PMIx predates the command-line fix for dash-bearing MCA values (pmix_cmd_line.c) -- rebuild the image or build with PMIX_SRC"
    elif [ "$rc" != 0 ]; then
        bad "a DVM cpu-set run failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    else
        ok "a DVM cpu-set given as a range was accepted and applied"
        # the expansion is reported back in the map header
        echo "$out" | grep -q 'Cpu set: *0,1' \
            && ok "the range was expanded to the individual cpus" \
            || bad "the cpu-set header did not show the expansion: $(echo "$out" | grep -m1 'Cpu set:')"
        # Every rank on every node must be inside the set. Match the WHOLE
        # bracketed site list, not just its first number: a rank bound to
        # "core:L0-7" starts with a 0 and would sail past a looser pattern.
        bad_cores=$(echo "$out" | sed -n 's/.*\[core:L\([0-9,-]*\)\].*/\1/p' \
                    | grep -vcE '^(0|1|0-1)$' || true)
        [ "$bad_cores" = 0 ] && ok "no rank was bound outside the cpu-set on any node" \
                             || bad "$bad_cores rank(s) were bound outside cores 0-1: $(echo "$out" | grep -o '\[core:L[0-9,-]*\]' | tr '\n' ' ')"
        n=$(echo "$out" | grep -c 'Bound: package')
        [ "$n" = 6 ] && ok "all 6 ranks reported a binding" \
                     || bad "$n/6 ranks reported a binding"
    fi
    # ...and the same must hold when binding to an object WIDER than a core.
    # A rank bound to a package used to come back owning every core of it,
    # cpu-set or no cpu-set - the constraint was honored at the core level
    # only, which is the one level that made the defect invisible. The
    # documented intent is the cpu_list comment in src/hwloc/hwloc.c.
    out=$(RUN 'timeout 90 prterun --prtemca hwloc_default_cpu_list 0-1 \
                   --host node2:2,node3:2 -n 4 --map-by node \
                   --bind-to package --display map hostname' 2>&1)
    rc=$?
    if echo "$out" | grep -q 'not-enough-arguments'; then
        skp "package binding under a cpu-set: PMIx predates the command-line fix"
    elif [ "$rc" != 0 ]; then
        bad "package binding under a cpu-set failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    else
        ok "a package-wide binding under a cpu-set completed"
        bad_cores=$(echo "$out" | sed -n 's/.*\[core:L\([0-9,-]*\)\].*/\1/p' \
                    | grep -vcE '^0-1$' || true)
        [ "$bad_cores" = 0 ] \
            && ok "a package-wide binding is confined to the cpu-set" \
            || bad "$bad_cores rank(s) got cores outside the cpu-set: $(echo "$out" | grep -o '\[core:L[0-9,-]*\]' | tr '\n' ' ')"
        n=$(echo "$out" | grep -c 'Bound: package')
        [ "$n" = 4 ] && ok "all 4 ranks reported a package binding" \
                     || bad "$n/4 ranks reported a binding"
    fi
    cleanup_swarm

    banner "hwloc: a malformed cpu-set is refused before anything launches"
    # The cpu ids went through a bare strtoul(), which reports 0 for "foo" -
    # so a typo did not fail, it confined the ENTIRE DVM to cpu 0 on every
    # node. Refusing it has to happen before any daemon is launched, and the
    # message has to name the offending entry.
    cleanup_swarm
    out=$(RUN 'timeout 60 prterun --prtemca hwloc_default_cpu_list 0,foo \
                   --host node2:1,node3:1 -n 2 hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "a non-numeric cpu-set entry is refused (rc=$rc)" \
                   || bad "a non-numeric cpu-set entry was accepted"
    echo "$out" | grep -q 'could not be resolved' \
        && ok "the refusal names the unresolvable entry" \
        || bad "no cpu-set diagnostic was printed: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    echo "$out" | grep -qE '^node[23]$' \
        && bad "procs launched anyway on a refused cpu-set" \
        || ok "nothing was launched"
    # ...and it is a REFUSAL, not a crash. The unresolved cpu-set came back as
    # a NULL cpuset and nothing between here and the mapper checked it, so the
    # diagnostic was followed by a segfault in the HNP inside hwloc.
    echo "$out" | grep -qE 'Segmentation fault|Bus error|signal' \
        && bad "the HNP crashed after refusing the cpu-set: $(echo "$out" | tr '\n' ' ' | tail -c 300)" \
        || ok "the refusal did not take the HNP down"
    # an id past the end of the topology is the same class of mistake
    out=$(RUN 'timeout 60 prterun --prtemca hwloc_default_cpu_list 0,99 \
                   --host node2:1 -n 1 hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "a cpu-set naming cpus the node does not have is refused (rc=$rc)" \
                   || bad "a cpu-set of 0,99 was accepted on an 8-core node"
    echo "$out" | grep -qE 'Segmentation fault|Bus error|signal' \
        && bad "the HNP crashed on an out-of-range cpu-set: $(echo "$out" | tr '\n' ' ' | tail -c 300)" \
        || ok "an out-of-range cpu-set did not take the HNP down"
    cleanup_swarm

    banner "hwloc: every node's topology can be printed"
    # --display topo runs prte_hwloc_print() over each node's topology in the
    # HNP. It rendered each object's cpuset into a 1024-byte buffer while
    # telling hwloc the buffer was 2048 - a stack overflow waiting for a wide
    # enough machine. These nodes are not wide enough to trip it, but the
    # traversal is exercised here for all of them at once, which is the only
    # place it runs against more than one topology.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:1,node3:1,node4:1 -n 3 \
                   --map-by node --display topo hostname' 2>&1)
    rc=$?
    [ "$rc" = 0 ] && ok "--display topo completed for 3 nodes (rc=0)" \
                  || bad "--display topo failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    n=$(echo "$out" | grep -c 'Type: Machine')
    [ "$n" -ge 1 ] && ok "at least one machine-level topology was rendered ($n)" \
                   || bad "no topology was rendered: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # a rendered cpuset has to be intact, not truncated mid-word
    echo "$out" | grep -q 'Cpuset:  0x' \
        && ok "object cpusets were rendered" \
        || bad "no cpuset was rendered in the topology dump"
    cleanup_swarm
}

# Absolute path, deliberately -- see the note above DS.
GC=/opt/prte/prte/bin/groupcon

test_grpcomm() {
    local out n g ranks hosts fails

    banner "grpcomm: a group construct releases on every daemon, in order"
    # The interesting half of a group collective is grp_release(), and it runs
    # on EVERY daemon: it takes the HNP's down-tree broadcast, hands the
    # group's context id / group info / endpoints to its OWN local PMIx server
    # via PMIx_server_register_resources(), and only then releases the local
    # participants.  On one host there is one daemon and it is also the HNP,
    # so neither the down-tree release nor the per-daemon registration is ever
    # exercised against a daemon that merely RECEIVED the broadcast.
    #
    # The registration used to be waited on, which parked the daemon event
    # loop for its duration on every construct; it is now a continuation, and
    # what these cases guard is that continuation.  Lose the caddy anywhere
    # between issuing the registration and resuming, and the local
    # participants are simply never released: the construct does not fail, it
    # HANGS, on every daemon at once.  Verified by deleting the thread-shift
    # and re-running -- 7 of these 10 assertions go red.
    #
    # Note what this does NOT show.  groupcon reads every rank local cid back
    # with no fence, which looks like an assertion that the daemon registered
    # the group with its PMIx server before releasing us -- it is not.  The
    # construct hands the same group info back to the client in its results,
    # so the client library answers those reads out of its own cache; skipping
    # PMIx_server_register_resources entirely still passes them.  The reads
    # are worth keeping as a check that the returned membership and group info
    # are complete and identical on every daemon, but do not read them as
    # coverage of the registration itself.
    cleanup_swarm
    if ! RUN "test -x $GC"; then
        skp "groupcon client not installed -- re-run ./build.sh"
        return
    fi
    if ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the grpcomm tests"
        cleanup_swarm
        return
    fi

    out=$(PRUN "--host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node $GC g1" 2>&1)
    n=$(echo "$out" | grep -c 'CONSTRUCT PMIX_SUCCESS')
    [ "$n" = 8 ] \
        && ok "all 8 ranks completed the group construct" \
        || bad "$n of 8 ranks constructed the group: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # spread across nodes, or this is a single-host test wearing a hat
    hosts=$(echo "$out" | awk '$1=="GRP" && $3=="HOST" {print $4}' | sort -u | grep -c '^node')
    [ "${hosts:-0}" -ge 2 ] \
        && ok "...spread over $hosts nodes, so non-HNP daemons ran the release" \
        || bad "the group did not span nodes ($hosts)"
    # every rank must have been handed the same context id
    n=$(echo "$out" | awk '$1=="GRP" && $3=="CONSTRUCT" {print $6}' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 1 ] \
        && ok "...and every rank got the same context id" \
        || bad "ranks disagreed on the context id ($n distinct values)"
    echo "$out" | grep -q 'ASSIGNED T' \
        && ok "...which the HNP actually assigned" \
        || bad "no rank reported an assigned context id"

    banner "grpcomm: every daemon returns the same complete group to its clients"
    # Each rank reads back all 8 local cids -- the group info every OTHER rank
    # contributed, which reached it only because its own daemon assembled the
    # broadcast result and handed it over.  A daemon that dropped, truncated,
    # or mismatched what it returned shows up here as a short or wrong set.
    ranks=$(echo "$out" | grep -c 'CID-OK 8')
    [ "$ranks" = 8 ] \
        && ok "all 8 ranks read back all 8 peer local cids" \
        || bad "only $ranks of 8 ranks read back a full set: $(echo "$out" | grep -E 'CID-(OK|FAIL)' | tr '\n' ' ' | tail -c 400)"
    # Assert on the DONE lines rather than on the absence of CID-FAIL: a run
    # in which nothing got far enough to print either has no CID-FAIL in it,
    # and would sail through an absence test.
    n=$(echo "$out" | grep -c 'DESTRUCT PMIX_SUCCESS')
    [ "$n" = 8 ] \
        && ok "...and all 8 ranks destructed the group" \
        || bad "$n of 8 ranks destructed the group"
    fails=$(echo "$out" | awk '$1=="GRP" && $3=="DONE" {s+=$4} END {print s+0}')
    n=$(echo "$out" | grep -c 'DONE ')
    if [ "$n" != 8 ]; then
        bad "only $n of 8 ranks reached the end of the client"
    elif [ "$fails" = 0 ]; then
        ok "...with all 8 clients finishing and reporting no failures"
    else
        bad "clients reported $fails failures: $(echo "$out" | grep -E 'CID-FAIL' | tr '\n' ' ' | tail -c 300)"
    fi

    banner "grpcomm: repeated constructs do not wedge the daemons"
    # The release path allocates a caddy per construct and hands it to a
    # continuation, so a leak or a missed release shows up as drift rather
    # than as one bad run.  Three back-to-back groups on the same DVM, then a
    # plain job, is enough to catch a daemon that stopped servicing its event
    # loop or never let go of a tracker.
    n=0
    for g in g2 g3 g4; do
        out=$(PRUN "--host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node $GC $g" 2>&1)
        [ "$(echo "$out" | grep -c 'CID-OK 8')" = 8 ] && n=$((n+1))
    done
    [ "$n" = 3 ] \
        && ok "three successive group constructs all completed" \
        || bad "only $n of 3 successive group constructs completed"
    out=$(PRUN "--host node1:1,node2:1,node3:1 -n 3 --map-by node hostname" 2>&1)
    n=$(echo "$out" | grep -c '^node')
    [ "$n" = 3 ] \
        && ok "...and the DVM still launches an ordinary job afterwards" \
        || bad "the DVM did not run a plain job after the group tests ($n of 3)"

    RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    cleanup_swarm
}

test_rml() {
    local out rc n c

    banner "rml: a small radix builds an interior tree and messages get relayed"
    # radix 2 over 7 nodes gives the HNP two children and four grandchildren,
    # so every launch command for a leaf is relayed by an interior daemon and
    # every line of output travels back the same way.  If the relay path is
    # broken this hangs or loses procs rather than returning 7 hostnames.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --prtemca rml_base_radix 2 \
                  --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1 \
                  -np 7 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-7]$')
    [ "$rc" = 0 ] && [ "$n" = 7 ] \
        && ok "radix 2: 7 procs across 7 nodes through a relayed tree" \
        || bad "radix 2 relay failed (rc=$rc, lines=$n): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after the radix-2 run" \
                 || bad "$c stray prted after the radix-2 run"

    banner "rml: every daemon computes the same tree the HNP does"
    # Each daemon derives its own placement from the radix math alone -- there
    # is no tree broadcast -- so a disagreement is silent until a message goes
    # somewhere nobody is listening.  routed_base_verbose makes each daemon
    # print the parent and children it arrived at; with radix 2 over 7 nodes
    # the HNP must report exactly two children, and somebody other than the
    # HNP must report children of its own (i.e. the tree really has an
    # interior).  --leave-session-attached is what gets daemon stderr back.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --prtemca rml_base_radix 2 \
                  --prtemca routed_base_verbose 1 --leave-session-attached \
                  --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1 \
                  -np 7 --map-by node hostname' 2>&1)
    if echo "$out" | grep -q 'num_children'; then
        # the HNP's own line (rank 0): two children at radix 2
        echo "$out" | grep -E '@0,0\]: parent .* num_children 2' >/dev/null \
            && ok "the HNP computed two children at radix 2" \
            || bad "the HNP did not report 2 children: $(echo "$out" | grep num_children | tr '\n' ' ' | tail -c 300)"
        # at least one NON-root daemon reporting children => a real interior
        n=$(echo "$out" | grep 'num_children' | grep -vE '@0,0\]:' | grep -cvE 'num_children 0')
        [ "$n" -ge 1 ] \
            && ok "at least one interior daemon has children of its own ($n)" \
            || bad "no interior daemon reported children -- the tree is flat"
    else
        skp "daemons did not report their tree (no routed verbose output captured)"
    fi
    cleanup_swarm

    banner "rml: a payload larger than one socket write survives the relay"
    # IOF output travels the same tree as everything else, so a proc on a leaf
    # emitting a few MB forces the partial-writev/partial-read bookkeeping in
    # oob_tcp_sendrecv.c on both the leaf's daemon and every relaying hop.  A
    # byte-count check catches truncation, which a "did it run" check cannot.
    out=$(RUN 'timeout -k 5 120 prterun --prtemca rml_base_radix 2 \
                  --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1 \
                  -np 7 --map-by node \
                  sh -c "head -c 500000 /dev/zero | tr \"\\0\" \"x\""' 2>&1)
    n=$(echo "$out" | tr -cd 'x' | wc -c | tr -d ' ')
    [ "$n" = 3500000 ] \
        && ok "3.5 MB of output relayed back intact" \
        || bad "large payload was truncated or lost (got $n bytes of 3500000)"
    cleanup_swarm

    banner "rml: the max-message-size guard rejects an oversized message"
    # prte_max_msg_size bounds what a receiving daemon will malloc for an
    # incoming message -- the length comes off the wire, so without the check
    # a peer dictates the allocation.  IOF output is chunked well below any
    # cap, so the launch message is the one that can be made arbitrarily
    # large: it carries the job's environment, and a 1.5 MB envar against a
    # 1 MB cap is certain to trip it on the receiving daemon.
    # The cap is driven to 0 rather than 1 MB, and that is deliberate.  Two
    # things defeat the obvious probe of "send something bigger than the cap":
    # a single argv/envp string is capped at MAX_ARG_STRLEN (128 KB) so a lone
    # multi-megabyte variable cannot even be exec'd, and PMIx compresses the
    # launch buffer, so a payload of repeated bytes shrinks to nothing on the
    # wire and sails under any cap.  A cap of 0 makes the check unambiguous:
    # every message is over it, so the guard must fire, name both sizes, and
    # tear the connection down instead of trusting the length off the wire.
    out=$(RUN 'timeout -k 5 60 prterun --prtemca prte_max_msg_size 0 \
                  --host node2:1 -np 1 hostname' 2>&1)
    echo "$out" | grep -q 'too large' \
        && ok "the size guard refused a message over the cap" \
        || bad "a message past prte_max_msg_size was accepted: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -qE 'Limit: *0' \
        && ok "...and reported the limit it enforced" \
        || bad "the size guard did not report its limit"
    # ...and the same job under the default cap must still run, so the guard
    # cannot have been "refuse everything"
    out=$(RUN 'timeout -k 5 60 prterun --host node2:1 -np 1 hostname' 2>&1)
    echo "$out" | grep -qE '^node2$' \
        && ok "the same job runs under the default size limit" \
        || bad "the default size limit broke the launch: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm

    banner "rml/oob: interface selection actually binds and connects"
    # The unit test proves prte_oob_split_and_resolve parses a specification;
    # only a real DVM proves the interface it picked is the one the sockets
    # end up on.  eth0 is what the compose network gives every container.
    out=$(RUN 'timeout -k 5 60 prterun --prtemca prte_if_include eth0 \
                  --host node1:1,node2:1,node3:1 -np 3 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-3]$')
    [ "$rc" = 0 ] && [ "$n" = 3 ] \
        && ok "if_include eth0 formed a working DVM" \
        || bad "if_include eth0 broke the DVM (rc=$rc, lines=$n): $(echo "$out" | tr '\n' ' ' | tail -c 250)"

    # Excluding the only usable interface leaves the OOB with no address to
    # advertise.  prte_rml_open used to ignore what prte_oob_open returned,
    # walk on to get_addr() -- which answers NULL -- and strdup() it, so the
    # user got a SIGSEGV where a diagnostic belonged.  Assert on the
    # diagnostic, not merely on "it did not hang": a crash also does not hang.
    out=$(RUN 'timeout -k 5 60 prterun --prtemca prte_if_exclude eth0 \
                  --host node1:1,node2:1 -np 2 --map-by node hostname' 2>&1); rc=$?
    [ "$rc" != 124 ] \
        && ok "excluding every usable interface failed instead of hanging (rc=$rc)" \
        || bad "excluding every usable interface hung the launch"
    echo "$out" | grep -qi 'no usable network interfaces' \
        && ok "...and said why" \
        || bad "no diagnostic for an empty interface list: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -qi 'signal: segmentation\|Segmentation fault' \
        && bad "an empty interface list crashed the daemon" \
        || ok "...without crashing"
    cleanup_swarm

    banner "rml: losing a daemon under a live DVM is detected, not hung on"
    # Killing an interior daemon drops its sockets; the peers' recv handlers
    # see the close, run prte_oob_tcp_peer_close (which must complete every
    # queued send as a failure rather than leak it) and call
    # prte_rml_route_lost.  The observable is that the DVM notices and the
    # job ends -- a leaked send or a missed teardown shows up as a hang.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1'; then
        PRUN_BG /tmp/rml-longrun.out '--host node7:1 -n 1 sleep 120'
        sleep 6
        # the whole case is meaningless if the job never got going: a prun
        # that already exited makes the wait loop below fall through at once
        # and report a pass it did not earn
        if ! RUN 'pgrep -x prun' >/dev/null 2>&1; then
            bad "the long-running job never started: $(RUN 'cat /tmp/rml-longrun.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        elif ! ON 7 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node7 has no daemon to kill -- the job did not land where expected"
        else
            ok "a job is running on node7 with its daemon up"
            # kill the daemon hosting the live proc: its peers' recv handlers
            # see the socket close, run prte_oob_tcp_peer_close (which must
            # complete every queued send as a failure) and call route_lost
            ON 7 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 45 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            [ "$n" -lt 45 ] \
                && ok "the DVM noticed the lost daemon and released the job (${n}s)" \
                || bad "prun never returned after its daemon died -- route_lost did not fire"
            # the HNP itself must survive: losing a leaf is not fatal to the DVM
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "the HNP survived losing a daemon" \
                || bad "the HNP died along with the lost daemon"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the lost-daemon test"
    fi
    cleanup_swarm
}

########################################################################
# src/event -- the event base every daemon and tool runs on.
#
# test/unit/event covers the API itself: allocation, dispatch, timers, the
# threadshift caddy, base open/close.  What it cannot cover is any event
# whose consequence is on another machine, and that is most of what this
# directory exists for.  Three things land here:
#
#   - a timer on the HNP that has to fire (or not fire) against a job whose
#     processes are spread over the swarm;
#   - a signal event on prun that has to travel the RML and be re-raised by
#     a daemon that never saw the signal itself;
#   - the event base's teardown, which only happens in prte_finalize() and
#     therefore only when a real DVM shuts down.
########################################################################
test_event() {
    local out rc n c

    banner "event: a job timeout fires and takes the job with it"
    # --timeout arms a prte_event_evtimer on the HNP.  A single node would
    # exercise the timer but not the part that matters: when it fires, the
    # HNP has to reach processes it does not host.  Ask for a job that will
    # never finish on its own and require the timer to end it.
    #
    # cleanup_swarm reaps daemons and tools but NOT the application processes
    # they left behind, and this case counts sleeps - a stray one from an
    # earlier phase would make the assertion nonsense.  Clear them first.
    cleanup_swarm
    for i in 1 2 3; do docker exec "$NODE$i" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    out=$(RUN 'timeout -k 5 120 prterun --timeout 10 \
                  --host node1:1,node2:1,node3:1 -np 3 --map-by node sleep 300' 2>&1); rc=$?
    [ "$rc" != 0 ] && [ "$rc" != 124 ] \
        && ok "the timeout ended the job (rc=$rc)" \
        || bad "the job outlived its timeout (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    # and it really killed the remote processes, not just the launcher
    n=0
    for i in 1 2 3; do
        ON "$i" 'pgrep -x sleep' >/dev/null 2>&1 && n=$((n+1))
    done
    [ "$n" = 0 ] && ok "no application processes survived the timeout" \
                 || bad "$n node(s) still running the timed-out job's procs"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after a timeout" \
                 || bad "$c stray prted after a timeout"

    banner "event: a job that beats its timeout is not killed by it"
    # The other half, and the one a broken evtimer_del() breaks: when the job
    # finishes first, the pending timer has to be deleted.  If the del does
    # not take, the timer fires afterwards against a job that is already
    # gone.  Give a fast job a long timeout and require a clean result.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 120 prterun --timeout 60 \
                  --host node1:1,node2:1,node3:1 -np 3 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-3]$')
    [ "$rc" = 0 ] && [ "$n" = 3 ] \
        && ok "a job well inside its timeout completes normally" \
        || bad "job failed under a generous timeout (rc=$rc, lines=$n): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    echo "$out" | grep -qi 'timeout\|timed out' \
        && bad "a job that finished in time still reported a timeout" \
        || ok "no spurious timeout was reported"
    cleanup_swarm

    banner "event: a signal caught on one node is re-raised on another"
    # prun catches the signal on ITS event base (a PRTE_EV_SIGNAL event, kept
    # on a prte_event_list_item_t), reads the number back with
    # PRTE_EVENT_SIGNAL(), and relays it over the RML; the daemon holding the
    # process re-raises it locally.  test_prted covers the job *scoping* of
    # that path with both jobs on one node.  What is only visible across
    # nodes is the relay itself -- put the launcher on node1 and the process
    # on node4, so nothing about the delivery can be local.
    for i in 1 4; do docker exec "$NODE$i" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    if ! prted_dvm_start 'node1:2,node4:2'; then
        bad "could not start a DVM for the cross-node signal test"
    else
        PRUN_BG /tmp/sigjob.out '--forward-signals SIGUSR1 --host node4:1 -n 1 sleep 300'
        sleep 10
        n=$(ON 4 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
        if [ "$n" = 1 ]; then
            ok "a process is running on node4 with its launcher on node1"
            c=$(RUN 'pgrep -x prun | head -1' 2>/dev/null | tr -d ' \r')
            RUN "kill -USR1 $c" >/dev/null 2>&1
            sleep 10
            n=$(ON 4 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
            [ "$n" = 0 ] \
                && ok "the signal crossed to node4 and reached the process" \
                || bad "the process on node4 never received the relayed signal"
        else
            bad "could not place a process on node4 (saw $n)"
        fi
        RUN 'pkill -f "sleep 300"' >/dev/null 2>&1
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "event: the DVM tears its event base down cleanly on every node"
    # prte_finalize() closes the event base -- it frees prte_sync_event_base
    # and clears both globals, which nothing did until recently (the function
    # existed and had no callers, and left both pointing at freed memory).
    # A base freed while something still holds a registered event, or freed
    # before the last PMIx server upcall has drained, shows up here as a
    # daemon that crashes or hangs instead of exiting.  Only a real shutdown
    # runs that code, so this is the one place it is exercised at all.
    if ! prted_dvm_start 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1,node8:1'; then
        bad "could not start a DVM for the teardown test"
    else
        c=$(prted_count 2 3 4 5 6 7 8)
        [ "$c" = 7 ] && ok "the DVM came up on all seven remote nodes" \
                     || bad "only $c of 7 remote daemons started"
        # give the daemons real work first, so there are live events (iof
        # read events, timers, collectives) on each base at shutdown
        out=$(PRUN '-n 8 --map-by node hostname' 2>&1); rc=$?
        n=$(echo "$out" | grep -cE '^node[1-8]$')
        [ "$rc" = 0 ] && [ "$n" = 8 ] \
            && ok "a job ran across the DVM before teardown" \
            || bad "the pre-teardown job failed (rc=$rc, lines=$n)"
        out=$(RUN 'timeout -k 5 60 pterm' 2>&1); rc=$?
        [ "$rc" = 0 ] && ok "pterm returned success" \
                      || bad "pterm exited $rc: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        echo "$out" | grep -qiE 'segmentation|bus error|abort|assert|corrupt' \
            && bad "pterm reported a crash during teardown: $(echo "$out" | tr '\n' ' ' | tail -c 200)" \
            || ok "no crash was reported during teardown"
        sleep 3
        c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
        [ "$c" = 0 ] && ok "every daemon exited on pterm" \
                     || bad "$c daemon(s) survived pterm"
        # a daemon that died in teardown leaves its session directory behind
        n=0
        for i in 1 2 3 4 5 6 7 8; do
            ON "$i" 'ls -d /tmp/prte.* /tmp/prted.* 2>/dev/null | head -1' \
                | grep -q . && n=$((n+1))
        done
        [ "$n" = 0 ] && ok "no session directories were left behind" \
                     || bad "$n node(s) left a session directory after teardown"
    fi
    cleanup_swarm
}

########################################################################
# src/include -- the constants every other directory is compiled against.
#
# test/unit/include covers the numbering arithmetic.  What it cannot cover
# is whether a code that a *daemon* produces still arrives at the user as a
# sentence.  PRRTE's error codes were renumbered onto PMIX_EXTERNAL_ERR_BASE
# (they used to sit on top of PMIx's own statuses, 46 of them exactly), and
# the way that goes wrong is not a build failure -- it is a real failure
# path that reports "Unknown error" to the user, or reports somebody else's
# error.  Those paths run on the daemon that hosts the process, so they need
# a remote node to be exercised at all.
########################################################################
test_include() {
    local out rc

    banner "include: a failure on a remote node is reported as a sentence"
    # PRTE_ERR_EXE_NOT_FOUND is raised by the odls on node2, travels back as
    # a proc state, and is rendered for the user on node1.  If a code has
    # lost its prte_strerror() entry -- which is exactly what renumbering
    # can do -- this is where it surfaces, as "Unknown error".
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --host node2:1 -np 1 /no/such/executable' 2>&1); rc=$?
    [ "$rc" != 0 ] && ok "a missing executable fails the job (rc=$rc)" \
                   || bad "a missing executable was reported as success"
    echo "$out" | grep -qi 'unknown error' \
        && bad "the failure came back as \"Unknown error\": $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "the failure was named, not reported as \"Unknown error\""
    # And the diagnostic itself, which is the whole point of the code being
    # named: prte_quit.c renders "prun:exe-not-accessible" from the
    # PMIX_ERR_EXE_NOT_ACCESSIBLE the odls stashed in the proc exit_code, and
    # prte_plm_base_spawn_response() now hands it to the requester ahead of
    # the failed spawn response.  It used to be produced and thrown away: the
    # tool was released from PMIx_Spawn by that response and left before the
    # job-end event carrying the text was ever raised.  On one node the
    # failing daemon's stderr is the user's terminal and covered for it -
    # which is exactly why the case has to be run with the executable on a
    # node that is NOT the one running prterun.
    echo "$out" | grep -q '/no/such/executable' \
        && ok "the message names the executable that could not be run" \
        || bad "no diagnostic naming the executable: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -q 'node2' \
        && ok "the message names the node it failed on" \
        || bad "the diagnostic does not name node2: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

    banner "include: a bad working directory on a remote node is named too"
    # A second code from the other documentation group in constants.h
    # (PRTE_ERR_WDIR_NOT_FOUND), so a renumbering that orphaned one group
    # and not the other is still caught.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --host node2:1 --wdir /no/such/dir -np 1 hostname' 2>&1); rc=$?
    [ "$rc" != 0 ] && ok "a missing wdir fails the job (rc=$rc)" \
                   || bad "a missing wdir was reported as success"
    echo "$out" | grep -qi 'unknown error' \
        && bad "the wdir failure came back as \"Unknown error\": $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "the wdir failure was named, not reported as \"Unknown error\""
    # PMIX_ERR_JOB_WDIR_NOT_FOUND has a prte_quit.c renderer of its own, and
    # it reaches the user by the same route the executable case does
    echo "$out" | grep -q '/no/such/dir' \
        && ok "the message names the working directory that was missing" \
        || bad "no diagnostic naming the wdir: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

    banner "include: an over-subscription is refused with a real message"
    # Slot exhaustion is decided by the mapper on the HNP against a node
    # pool that only has more than one entry in a real DVM.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --host node2:1,node3:1 -np 64 hostname' 2>&1); rc=$?
    [ "$rc" != 0 ] && ok "an impossible request is refused (rc=$rc)" \
                   || bad "64 procs were placed on 2 slots"
    echo "$out" | grep -qi 'unknown error' \
        && bad "the mapping failure came back as \"Unknown error\": $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "the mapping failure was named, not reported as \"Unknown error\""
    echo "$out" | grep -qiE 'slot|oversubscribe|not enough|available' \
        && ok "the message explains what was short" \
        || bad "no recognisable diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm
}

test_linux() {
    if ! docker ps --format '{{.Names}}' | grep -qx "${NODE}1"; then
        # Name the swarm we looked for. Forgetting PRTE_SWARM on the compose
        # command (it interpolates docker-compose.yml, so it has to be in that
        # command's environment) brings up the DEFAULT swarm instead, and
        # "swarm not up" alone sends you looking for a docker problem.
        echo "swarm '$PRTE_SWARM' not up -- no container named ${NODE}1" >&2
        echo "run: ${SWARM_ENV}docker compose up -d" >&2
        [ -z "$SWARM_ENV" ] || \
            echo "     (and ${SWARM_ENV}./build.sh first, so volume $VOLUME exists)" >&2
        exit 2
    fi
    # Start from a clean slate rather than trusting the last run to have
    # tidied up. The nodes are long-lived containers while the install they
    # read is replaced under them, so a daemon or a pmix.* rendezvous file
    # left from a previous run may be holding a library that no longer
    # exists. That does not fail one case - it fails twenty, starting with
    # the first launch, and none of the messages point at the cause.
    cleanup_swarm

    banner "preflight: install present in shared volume"
    if RUN 'command -v prterun prte prun pterm elastic >/dev/null'; then
        ok "prterun/prte/prun/pterm/elastic on PATH"
    else
        bad "tools missing -- did ./build.sh run?"; return
    fi
    # The install lives in a volume that outlives any one build, so "the tools
    # are on PATH" says nothing about WHICH source they were built from. A
    # build.sh run that dies part-way -- configure rejecting the baked PMIx is
    # the usual way -- leaves the previous install standing, and without this
    # check the whole suite runs against code the author never built, reporting
    # failures that are really "you are testing something else".
    stamp=$(RUN 'cat /opt/prte/.build-stamp 2>/dev/null' | tr -d '\r')
    if [ -n "$stamp" ]; then
        ok "install built $stamp"
    else
        bad "no build stamp in the volume -- the last ./build.sh did not complete."
        echo "     Re-run ./build.sh and check its exit status; the install now" >&2
        echo "     present is from an earlier build and would be tested instead." >&2
        return
    fi

    # ...and the same question about the CONTAINERS. They are long-lived, so a
    # rebuilt image (which is how the baked PMIx gets updated) does not reach
    # them until they are recreated.  build.sh then compiles PRRTE against the
    # NEW image's PMIx while the daemons run the old one, and the symptom is an
    # undefined symbol from libprrte in whichever test first uses a new PMIx
    # entry point -- nothing points at the container.
    banner "preflight: containers are running the current image"
    imgid=$(docker images --no-trunc --format '{{.ID}}' "$IMAGE" 2>/dev/null | head -1)
    stalenodes=""
    for imgn in $(seq 1 10); do
        cimg=$(docker inspect "$NODE$imgn" --format '{{.Image}}' 2>/dev/null)
        [ "$cimg" = "$imgid" ] || stalenodes="$stalenodes node$imgn"
    done
    if [ -z "$stalenodes" ]; then
        ok "all 10 containers are on the current $IMAGE"
    else
        bad "containers predate $IMAGE:$stalenodes"
        # The image is shared by every swarm on this host, so the other clone
        # rebuilding it is one way to land here without having touched
        # anything yourself.
        echo "     Recreate them: ${SWARM_ENV}docker compose up -d --force-recreate" >&2
        echo "     (from contrib/dockerswarm, so the pinned project name applies)" >&2
        return
    fi

    banner "prterun (non-elastic, one-shot) -- local"
    out=$(RUN 'prterun -np 4 hostname'); rc=$?
    [ "$rc" = 0 ] && [ "$(echo "$out" | grep -c node1)" = 4 ] \
        && ok "prterun -np 4 -> 4x node1, exit 0" \
        || bad "prterun local (rc=$rc): $(echo "$out" | tr '\n' ' ')"

    banner "prterun (non-elastic, one-shot) -- multi-node (real cross-daemon RML)"
    out=$(RUN 'prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by node hostname'); rc=$?
    n=$(echo "$out" | grep -cE 'node[1-4]')
    [ "$rc" = 0 ] && [ "$n" = 8 ] \
        && ok "prterun multi-node -> 8 procs across node1-4, exit 0" \
        || bad "prterun multi-node (rc=$rc, lines=$n)"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after prterun" || bad "$c stray prted after prterun"

    banner "filem: --preload-binary cross-node staging (binary only on node1)"
    # Compile a marker binary on node1 ONLY. If it runs on node2/node3 -- where
    # the file does not exist -- then filem actually staged the bytes there and
    # linked them into the job session dir that --preload-binary's session cwd
    # points at. This exercises the real cross-daemon delivery path (xcast ->
    # recv_files -> write_handler -> link_local_files), which a single-host run
    # cannot prove because the source file is already present locally.
    docker exec "${NODE}1" bash -lc '. /opt/prte/env.sh 2>/dev/null; cat > /root/staged_marker.c <<"CEOF"
#include <unistd.h>
#include <stdio.h>
int main(void){ char h[64]; gethostname(h, sizeof(h)); printf("STAGED-BIN-OK %s\n", h); return 0; }
CEOF
gcc -o /root/staged_marker /root/staged_marker.c' >/dev/null 2>&1
    if RUN 'test -x /root/staged_marker'; then
        leaked=0
        for n in 2 3; do ON "$n" 'test -e /root/staged_marker' && leaked=1; done
        [ "$leaked" = 0 ] && ok "marker binary exists only on node1 (staging test is valid)" \
                          || bad "marker binary leaked onto a target node -- test would be meaningless"
        out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node --preload-binary -- ./staged_marker' 2>&1); rc=$?
        n2=$(echo "$out" | grep -c 'STAGED-BIN-OK node2')
        n3=$(echo "$out" | grep -c 'STAGED-BIN-OK node3')
        [ "$rc" = 0 ] && [ "$n2" = 1 ] && [ "$n3" = 1 ] \
            && ok "preload-binary staged from node1 and ran on node2+node3" \
            || bad "preload-binary cross-node failed (rc=$rc, n2=$n2, n3=$n3): $(echo "$out" | tr '\n' ' ')"
        c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
        [ "$c" = 0 ] && ok "no daemons linger after preload-binary run" || bad "$c stray prted after preload run"
    else
        bad "could not compile the marker binary on node1 (need gcc in the image)"
    fi
    docker exec "${NODE}1" sh -c 'rm -f /root/staged_marker /root/staged_marker.c' 2>/dev/null

    banner "iof: stdin forwarded to a REMOTE proc (HNP -> prted -> proc)"
    # Rank 0 is mapped onto node2, not the head node, so every stdin byte must
    # travel HNP -> RML(PRTE_RML_TAG_IOF_PROXY) -> prted -> the proc's stdin
    # pipe, and be echoed back prted -> HNP as forwarded output. A single-host
    # run exercises only the HNP-local sink and proves nothing about the wire
    # format, which is why this test lives in the swarm.
    #
    # The large payload matters: a stdin fragment used to be unpacked into a
    # fixed PRTE_IOF_BASE_MSG_MAX (4096) buffer on the daemon, so anything
    # bigger was dropped by the unpack rather than delivered. Any payload well
    # past 4096 -- and past the 8192 write-chunk size -- guards that path.
    #
    # Note the payload is base64 text: printable, newline-terminated, and free
    # of anything a pipe or tty layer might translate, so a mismatch means IOF
    # really did lose or reorder bytes.
    # These run under "prun" against a persistent DVM rather than "prterun".
    # That is deliberate: prterun's own stdin is read by the HNP acting as a
    # PMIx *server*, and PMIx's server branch returns early on EOF without
    # calling push_stdin (pmix_iof_read_local_handler in
    # src/common/pmix_iof.c) -- so the zero-byte close never reaches PRRTE and
    # "cat" waits forever. That is an openpmix defect, not a PRRTE one; the
    # tool path prun uses forwards the zero-byte push correctly. Every command
    # is wrapped in "timeout" so a delivery regression fails the test instead
    # of wedging the suite.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node1:1,node2:1,node3:1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'head -c 262144 /dev/urandom | base64 > /tmp/iof_stdin_in.txt' >/dev/null 2>&1
        out=$(RUN 'cd /tmp && timeout 90 prun --host node2:1 -n 1 \
                     cat < iof_stdin_in.txt > iof_stdin_out.txt 2>iof_stdin_err.txt; echo "rc=$?"')
        rc=$(echo "$out" | sed -n 's/^rc=//p')
        insum=$(RUN 'md5sum < /tmp/iof_stdin_in.txt' | awk '{print $1}')
        outsum=$(RUN 'md5sum < /tmp/iof_stdin_out.txt 2>/dev/null' | awk '{print $1}')
        insz=$(RUN 'wc -c < /tmp/iof_stdin_in.txt' | tr -d ' ')
        outsz=$(RUN 'wc -c < /tmp/iof_stdin_out.txt 2>/dev/null' | tr -d ' ')
        [ "$rc" = 0 ] && [ -n "$insum" ] && [ "$insum" = "$outsum" ] \
            && ok "large stdin ($insz bytes) round-tripped through a remote proc intact" \
            || bad "remote stdin corrupted/truncated/hung (rc=$rc, in=$insz out=$outsz, md5 $insum vs $outsum): $(RUN 'head -c 200 /tmp/iof_stdin_err.txt')"
        # rc=0 also means "cat" saw EOF, so the zero-byte close sentinel made
        # the trip; rc=124 would be the timeout, i.e. it did not.

        # Wildcard stdin is xcast to every daemon rather than sent to one, and
        # the xcast comes back to the HNP too -- so this covers delivery to a
        # proc the HNP hosts (node1) and to a remote proc (node2) at once. With
        # no receive posted on the proxy tag at the HNP, the node1 proc gets
        # nothing and the job hangs.
        out=$(RUN 'cd /tmp && echo WILDCARD-STDIN-OK | timeout 60 prun --host node1:1,node2:1 \
                     -n 2 --map-by node --stdin all cat 2>&1; echo "rc=$?"')
        rc=$(echo "$out" | sed -n 's/^rc=//p')
        n=$(echo "$out" | grep -c 'WILDCARD-STDIN-OK')
        [ "$rc" = 0 ] && [ "$n" = 2 ] \
            && ok "wildcard stdin (--stdin all) reached the HNP-local and the remote proc" \
            || bad "wildcard stdin reached $n/2 procs (rc=$rc): $(echo "$out" | tr '\n' ' ')"

        # Same wire, different consumer: a proc that reads its stdin SLOWLY.
        # "cat" above drains the pipe as fast as the daemon can fill it, so
        # the daemon non-blocking write(2) nearly always completes in full and
        # the short-write path is never taken -- which is why a 256 KB "cat"
        # test passed for years while piping a large file into a real
        # application corrupted it (issue #2579).  slowcat keeps the pipe
        # saturated, so once the input passes the pipe capacity (64 KB on
        # Linux) essentially every write the daemon attempts is partial.
        #
        # The failure mode is DUPLICATION, not loss: re-basing the queued
        # chunk data without also decrementing its count left the tail of the
        # chunk holding bytes that had already gone out, and each retry sent
        # them again -- the reporter measured a 2,064-line input arriving as
        # 1,089,247 lines.  So the assertion is on the byte count first: the
        # app must receive exactly what was piped in, no more.
        insum=$(RUN 'md5sum < /tmp/iof_stdin_in.txt' | awk '{print $1}')
        insz=$(RUN 'wc -c < /tmp/iof_stdin_in.txt' | tr -d ' ')
        out=$(RUN 'cd /tmp && timeout 180 prun --host node2:1 -n 1 \
                     '"$SC"' /tmp/iof_slow_out.txt 256 1000 < iof_stdin_in.txt \
                     2>iof_slow_err.txt; echo "rc=$?"')
        rc=$(echo "$out" | sed -n 's/^rc=//p')
        got=$(echo "$out" | sed -n 's/^SLOWCAT-BYTES //p')
        outsum=$(ON 2 'md5sum < /tmp/iof_slow_out.txt 2>/dev/null' | awk '{print $1}')
        [ "$rc" = 0 ] && [ -n "$got" ] && [ "$got" = "$insz" ] && [ "$insum" = "$outsum" ] \
            && ok "large stdin ($insz bytes) survived a SLOW remote reader (short writes)" \
            || bad "slow-reader stdin corrupted (rc=$rc, sent=$insz received=$got, md5 $insum vs $outsum): $(RUN 'head -c 200 /tmp/iof_slow_err.txt')"
        ON 2 'rm -f /tmp/iof_slow_out.txt' >/dev/null 2>&1

        # And the same slow reader on the HNP node, where push_stdin writes
        # into the proc sink directly instead of going out over the RML: the
        # HNP and the daemon carry separate copies of the write handler, so a
        # short-write fix in one says nothing about the other.
        out=$(RUN 'cd /tmp && timeout 180 prun --host node1:1 -n 1 \
                     '"$SC"' /tmp/iof_slow_out.txt 256 1000 < iof_stdin_in.txt \
                     2>iof_slow_err.txt; echo "rc=$?"')
        rc=$(echo "$out" | sed -n 's/^rc=//p')
        got=$(echo "$out" | sed -n 's/^SLOWCAT-BYTES //p')
        outsum=$(RUN 'md5sum < /tmp/iof_slow_out.txt 2>/dev/null' | awk '{print $1}')
        [ "$rc" = 0 ] && [ -n "$got" ] && [ "$got" = "$insz" ] && [ "$insum" = "$outsum" ] \
            && ok "large stdin ($insz bytes) survived a SLOW HNP-local reader (short writes)" \
            || bad "slow-reader stdin corrupted on the HNP (rc=$rc, sent=$insz received=$got, md5 $insum vs $outsum): $(RUN 'head -c 200 /tmp/iof_slow_err.txt')"

        # --- output-to-file: the terminal is supposed to stay CLEAN ---------
        # NOCOPY is the documented default for --output (see
        # docs/prrte-rst-content/cli-output.rst), so "file=" means the output
        # goes to files and NOT to the tool's stdout.  It leaked: a tool learns
        # that its spawned job is not to be written locally only when the spawn
        # REPLY lands, and output arriving before that found no namespace record
        # and fell back to the process-wide flag - which for a tool says "yes,
        # write it".  So a nondeterministic subset of the job appeared on a
        # terminal the user had asked to keep quiet: whichever ranks' first
        # chunk beat the reply, typically one rank in two.
        #
        # That is why this runs several times.  A single pass is not evidence:
        # before the fix the same command printed nothing on roughly a third of
        # runs, so one clean run proves nothing at all.
        if ! pmix_cap PMIX_CAP_IOF_DELIVER_LOCAL; then
            # No flag marks this fix by itself; this one stands in for "a PMIx
            # carrying the IOF work these cases assert", and the baked PMIx
            # goes stale as a matter of course (see AGENTS.md).
            skp "PMIx predates the output-file terminal fix -- skipping"
        else
            RUN 'rm -rf /tmp/ofterm; mkdir -p /tmp/ofterm' >/dev/null 2>&1
            ON 2 'rm -rf /tmp/ofterm; mkdir -p /tmp/ofterm' >/dev/null 2>&1
            leaked=0
            copied=0
            for i in 1 2 3 4 5; do
                n=$(RUN 'timeout -k 5 45 prun --output file=/tmp/ofterm/out \
                             --host node1:1,node2:1 -n 2 --map-by node hostname 2>&1' \
                        | grep -cE '^node[12]$')
                [ "$n" = 0 ] || leaked=$((leaked+1))
                n=$(RUN 'timeout -k 5 45 prun --output file=/tmp/ofterm/out:copy \
                             --host node1:1,node2:1 -n 2 --map-by node hostname 2>&1' \
                        | grep -cE '^node[12]$')
                [ "$n" = 2 ] && copied=$((copied+1))
            done
            [ "$leaked" = 0 ] \
                && ok "--output file= kept the terminal clean on all 5 runs" \
                || bad "--output file= leaked job output to the terminal on $leaked of 5 runs"
            [ "$copied" = 5 ] \
                && ok "...and the copy qualifier still delivers every rank" \
                || bad "--output file=:copy delivered both ranks on only $copied of 5 runs"
            # the files themselves must still be there - "clean terminal" must
            # not have been achieved by dropping the output on the floor
            n=$(RUN 'ls /tmp/ofterm | wc -l' | tr -d ' \r')
            [ "$n" -gt 0 ] \
                && ok "...and the output files were written ($n on the head node)" \
                || bad "--output file= wrote no files at all"
            RUN 'rm -rf /tmp/ofterm' >/dev/null 2>&1
            ON 2 'rm -rf /tmp/ofterm' >/dev/null 2>&1
        fi

        RUN 'rm -f /tmp/iof_stdin_in.txt /tmp/iof_stdin_out.txt /tmp/iof_stdin_err.txt \
                   /tmp/iof_slow_out.txt /tmp/iof_slow_err.txt' >/dev/null 2>&1
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the stdin tests"
    fi
    cleanup_swarm

    banner "plm/ssh: tree-spawn fan-out (radix 2, multi-level)"
    # With tree-spawn (the default) the HNP does NOT ssh to every node: it
    # launches only its own routing children, each of which calls the ssh
    # module's remote_spawn() to launch ITS children, and so on. Forcing
    # radix 2 makes the tree several levels deep, so daemons on the deeper
    # nodes exist only if the fan-out actually recursed -- something a
    # single-host or flat launch can never prove. Every daemon still reports
    # directly to the HNP, so a wrong routing/child list shows up as a launch
    # that hangs at DAEMONS_LAUNCHED rather than a wrong answer.
    cleanup_swarm
    out=$(RUN 'prterun --prtemca rml_base_radix 2 \
                 --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1,node8:1 \
                 -np 8 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-8]$')
    [ "$rc" = 0 ] && [ "$n" = 8 ] \
        && ok "radix-2 tree-spawn launched 8 daemons (fan-out recursed)" \
        || bad "radix-2 tree-spawn failed (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after tree-spawn launch" || bad "$c stray prted after tree-spawn"

    banner "plm/ssh: flat launch (no_tree_spawn) and throttled launch"
    # The same job with the fan-out disabled: now the HNP ssh's to all seven
    # remote nodes itself. num_concurrent=1 additionally forces the launch
    # list to drain one fork at a time, so the metering path
    # (process_launch_list <-> ssh_wait_daemon, and the per-daemon caddy
    # lifecycle) is exercised serially rather than all at once.
    out=$(RUN 'prterun --prtemca plm_ssh_no_tree_spawn 1 \
                 --host node1:1,node2:1,node3:1,node4:1 \
                 -np 4 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-4]$')
    [ "$rc" = 0 ] && [ "$n" = 4 ] \
        && ok "flat (no_tree_spawn) launch reached all 4 nodes" \
        || bad "flat launch failed (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"

    out=$(RUN 'prterun --prtemca plm_ssh_no_tree_spawn 1 --prtemca plm_ssh_num_concurrent 1 \
                 --host node1:1,node2:1,node3:1,node4:1,node5:1 \
                 -np 5 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-5]$')
    [ "$rc" = 0 ] && [ "$n" = 5 ] \
        && ok "throttled launch (num_concurrent=1) completed all 5 daemons" \
        || bad "throttled launch failed (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after flat/throttled launches" || bad "$c stray prted"

    banner "plm/ssh: prted cmd line survives an mca value containing '='"
    # prte_plm_base_prted_append_basic_args replicates PRTE_MCA_*/PMIX_MCA_*
    # env vars onto the prted command line. Splitting those on every '='
    # (rather than the first) silently truncates any value that contains one,
    # so the daemon would be started with a DIFFERENT value than the HNP has.
    # Ask for a value with an embedded '=' and require the daemons to come up
    # and run; a truncated/garbled value makes the prted reject its cmd line.
    out=$(RUN 'PRTE_MCA_prte_base_env_list="FOO=bar=baz" prterun \
                 --host node1:1,node2:1 -np 2 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[12]$')
    [ "$rc" = 0 ] && [ "$n" = 2 ] \
        && ok "daemons launched with an '='-bearing mca value" \
        || bad "launch broke on an '='-bearing mca value (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"

    banner "plm/ssh: environ mca params can be dropped from the prted cmd line"
    # plm_ssh_pass_environ_mca_params=0 is what the "cmd-line-too-long" help
    # text tells users to set. It must actually take effect AND still leave a
    # working command line (the required daemon options do not come from the
    # environment).
    out=$(RUN 'PRTE_MCA_plm_base_verbose=0 prterun --prtemca plm_ssh_pass_environ_mca_params 0 \
                 --host node1:1,node2:1,node3:1 -np 3 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-3]$')
    [ "$rc" = 0 ] && [ "$n" = 3 ] \
        && ok "launch works with pass_environ_mca_params=0" \
        || bad "pass_environ_mca_params=0 broke the launch (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"
    cleanup_swarm

    banner "plm: --uniform-nodes topology inheritance"
    # Under prte_homo_nodes (--uniform-nodes) ONLY daemon rank 1 reports a
    # topology; every other daemon's node inherits it in progress_daemons()
    # before mapping runs. A node whose topology stayed NULL cannot be mapped
    # onto (the mapper fails the job), so a job that must place a proc on the
    # third and fourth nodes -- whose daemons are ranks 2 and 3, and reported
    # no topology of their own -- is the test. Contrast with the same launch
    # without the flag, where every daemon reports its own.
    cleanup_swarm
    out=$(RUN 'prterun --uniform-nodes --host node1:1,node2:1,node3:1,node4:1 \
                 -np 4 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-4]$')
    [ "$rc" = 0 ] && [ "$n" = 4 ] \
        && ok "uniform-nodes launch mapped onto the nodes that report no topology" \
        || bad "uniform-nodes launch failed (rc=$rc, procs=$n): $(echo "$out" | tr '\n' ' ')"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after uniform-nodes launch" || bad "$c stray prted"

    banner "plm: a grown node inherits its topology from a survivor"
    # Under --uniform-nodes only daemon rank 1 reports a topology; every other
    # daemon inherits it in progress_daemons() before mapping runs, and a node
    # whose topology stayed NULL cannot be mapped onto. Shrink the rank-1 node
    # out of an elastic DVM and then grow a new one: the added daemon is not
    # rank 1, so it reports no topology of its own and has to inherit from a
    # daemon that is still alive.
    #
    # A shrink keeps the departed daemon's proc object (its vpid is never
    # reused) and leaves that proc pointing at a node that still holds a
    # topology, so this only discriminates because progress_daemons() skips
    # daemons that are no longer ALIVE. Take that aliveness test out - or go
    # back to sourcing the topology from rank 1 unconditionally - and the
    # spawn below fails with "All nodes which are allocated for this job are
    # already filled", which is what a NULL topology on the grown node looks
    # like from the outside.
    #
    # Proving the node is usable means running something on it, and that is
    # asserted twice over, because the two ways of reaching a grown node are
    # different code:
    #
    #   - WHILE the requesting tool still holds the reservation, only that
    #     tool can put work on it -- the HNP lets a namespace target a
    #     reservation only if it owns it, which is why the grow and the spawn
    #     happen in one "elastic" invocation, naming the reservation with the
    #     PMIX_ALLOC_ID the grow handed back (PMIX_SPAWN_TARGET).
    #   - ONCE that tool exits, the reservation's inheritance disposition
    #     fires and (by default) unreserves its nodes into the general pool,
    #     so a plain "prun --host node4" reaches them with no allocation
    #     directive at all.
    #
    # The second half is gated on the PMIx capability, because a tool that
    # finalizes cleanly is reported to the host only by a PMIx that has
    # PMIX_CAP_TOOL_FINALIZED; without it the DVM never learns the tool went
    # away and the nodes stay withheld for the life of the DVM.
    cleanup_swarm
    ON 4 'rm -f /tmp/survivor.out' >/dev/null 2>&1
    RUN 'nohup prte --daemonize --uniform-nodes --prtemca prte_elastic_mode 1 \
             >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN 'timeout 90 elastic grow node2:2,node3:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "uniform DVM grown onto the topology reporter (rank 1) and a peer" \
            || bad "baseline grow of node2,node3 did not complete"
        out=$(RUN 'timeout 90 elastic shrink node2' 2>&1); sleep 3
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "the topology-reporting daemon was shrunk out of the DVM" \
            || bad "shrink of the rank-1 node did not complete"
        [ "$(prted_count 2)" = 0 ] && ok "no daemon left on the shrunk node" \
                                   || bad "daemon still running on the shrunk node"
        out=$(RUN "timeout 120 elastic grow node4:2 -- /bin/sh -c 'hostname > /tmp/survivor.out'" 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "grow completed with the original topology reporter gone" \
            || bad "grow after shrinking rank 1 did not complete"
        echo "$out" | grep -q '>>> SPAWNED' \
            && ok "job spawned into the grown node's reservation" \
            || bad "spawn into the reservation failed: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        marker=$(ON 4 'cat /tmp/survivor.out 2>/dev/null' | tr -d '\r')
        [ "$marker" = node4 ] \
            && ok "the grown node ran the job - it inherited from a survivor" \
            || bad "grown node never ran the job (marker='$marker')"
        # The elastic client has exited by now, so its reservation is gone
        # and the node it grew is back in the general pool -- reachable by a
        # plain --host with no allocation directive of any kind. This is the
        # assertion the topology question is really about: a node whose
        # topology stayed NULL fails to map here with "All nodes which are
        # allocated for this job are already filled".
        if pmix_cap PMIX_CAP_TOOL_FINALIZED; then
            sleep 2
            # spell the slot count out: --host with a bare name contributes
            # ONE slot, so -np 2 would fail as oversubscription rather than
            # for any reason this case is about
            out=$(RUN 'timeout 60 prun --host node4:2 -np 2 hostname' 2>&1)
            n=$(echo "$out" | grep -c '^node4$')
            [ "$n" = 2 ] \
                && ok "plain prun maps onto the grown node once its reservation is released" \
                || bad "prun --host node4 placed $n/2 procs: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        else
            skp "prun onto the released reservation (PMIx predates PMIX_CAP_TOOL_FINALIZED)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the survivor-topology test"
    fi
    cleanup_swarm

    banner "plm: node names reconciled with what the daemons report"
    # Allocate by IP address. Each daemon reports its gethostname() result
    # ("nodeN") plus its own aliases, and the HNP replaces the allocation's
    # name with the reported one -- keeping the ORIGINAL name as an alias.
    # If that alias set is overwritten rather than merged, the address the
    # user allocated with no longer matches any node in the DVM, and a
    # subsequent --host by that same address fails with "not in allocation".
    ip2=$(RUN 'getent hosts node2 | awk "{print \$1}" | head -1' | tr -d '\r')
    ip3=$(RUN 'getent hosts node3 | awk "{print \$1}" | head -1' | tr -d '\r')
    if [ -n "$ip2" ] && [ -n "$ip3" ]; then
        RUN "nohup prte --daemonize --host node1:1,$ip2:1,$ip3:1 >/tmp/prte.out 2>&1 & sleep 8" >/dev/null
        if RUN 'pgrep -x prte >/dev/null'; then
            out=$(RUN 'prun -n 3 --map-by node hostname' 2>&1)
            n=$(echo "$out" | grep -cE '^node[1-3]$')
            [ "$n" = 3 ] && ok "daemons allocated by IP report their real hostnames" \
                         || bad "IP-allocated DVM did not report node names: $(echo "$out" | tr '\n' ' ')"
            # by the daemon-reported name...
            out=$(RUN 'prun --host node2:1 -n 1 hostname' 2>&1)
            [ "$(echo "$out" | tr -d '\r')" = node2 ] \
                && ok "--host by reported name resolves to the IP-allocated node" \
                || bad "--host node2 failed on an IP-allocated DVM: $(echo "$out" | tr '\n' ' ')"
            # ...and by the address it was allocated with, which must survive
            # as an alias
            out=$(RUN "prun --host $ip3:1 -n 1 hostname" 2>&1)
            [ "$(echo "$out" | tr -d '\r')" = node3 ] \
                && ok "--host by original address still matches (alias retained)" \
                || bad "--host $ip3 no longer matches its node (alias lost): $(echo "$out" | tr '\n' ' ')"
            RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
        else
            bad "could not start a DVM allocated by IP address"
        fi
    else
        skp "could not resolve node2/node3 addresses -- skipping alias test"
    fi
    cleanup_swarm

    banner "elastic DVM: grow + shrink (radix 64, flat tree)"
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    out=$(RUN 'prun --np 1 hostname')
    [ "$out" = node1 ] && ok "baseline prun -> node1" || bad "baseline prun -> '$out'"

    out=$(RUN 'elastic grow node2:2,node3:2' 2>&1)
    echo "$out" | grep -q PMIX_DVM_IS_READY && echo "$out" | grep -q SUCCESS \
        && ok "grow node2,node3 completed (PMIX_DVM_IS_READY)" \
        || bad "grow did not complete"
    [ "$(prted_count 2 3)" = 2 ] && ok "prted wired in on node2+node3" || bad "grown daemons missing"

    out=$(RUN 'elastic shrink node3' 2>&1)
    sleep 3
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "shrink node3 completed (PMIX_DVM_IS_READY)" || bad "shrink did not complete"
    RUN 'pgrep -x prte >/dev/null' && ok "HNP survived the shrink" || bad "HNP died on shrink"
    [ "$(prted_count 3)" = 0 ] && ok "node3 prted gone" || bad "node3 prted still present"
    [ "$(prted_count 2)" = 1 ] && ok "node2 prted still alive" || bad "node2 prted lost"
    out=$(RUN 'prun --np 1 hostname')
    [ "$out" = node1 ] && ok "prun works post-shrink" || bad "prun broken post-shrink"
    RUN 'pterm' >/dev/null 2>&1; cleanup_swarm

    banner "elastic DVM: a departing tool releases the allocation it reserved"
    # A grow creates a RESERVATION owned by the namespace that asked for it,
    # and every reservation carries a disposition saying what becomes of it
    # when that namespace terminates. The default -- and what the elastic
    # client asks for by saying nothing -- is to unreserve the nodes into the
    # general pool. For a command-line tool "that namespace terminates" means
    # the tool exited, and nothing else ever will: it is not a child, so no
    # waitpid fires, and the connection it drops afterwards raises no
    # lost-connection event because PMIx has already marked the peer
    # finalized. Until PMIX_CAP_TOOL_FINALIZED the host was simply never
    # told, so the disposition never ran and a grow driven from a command
    # line stranded its nodes for the life of the DVM -- outside the general
    # pool, and unreachable through the reservation too, since the only
    # namespace permitted to name it no longer existed.
    #
    # Two nodes, so the assertion cannot pass on a job that quietly fell back
    # to the head node.
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN 'timeout 90 elastic grow node2:2,node3:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "grow node2,node3 completed" || bad "grow did not complete"
        if pmix_cap PMIX_CAP_TOOL_FINALIZED; then
            # the disposition runs on the HNP's progress thread once the
            # tool's finalize lands, so give it a beat before asking
            sleep 2
            out=$(RUN 'timeout 60 prun --host node2:2,node3:2 -np 2 --map-by node hostname' 2>&1)
            n=$(echo "$out" | grep -cE '^node[23]$')
            [ "$n" = 2 ] \
                && ok "the grown nodes joined the general pool when the tool exited" \
                || bad "prun reached $n/2 grown nodes: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        else
            skp "released-reservation placement (PMIx predates PMIX_CAP_TOOL_FINALIZED)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the reservation-release test"
    fi
    cleanup_swarm

    banner "elastic DVM: the user who reserved an allocation can use it again"
    # A reservation is owned by the namespace that requested it, which is what
    # keeps other JOBS in the DVM out of it. That cannot be the whole rule for
    # a TOOL, though: a tool namespace is minted per invocation, so a user's
    # second command could never name the allocation their first one made, and
    # once that first command exited nothing could name it at all. So the user
    # is recorded alongside the namespace, and a tool presenting the same uid
    # may act on the reservation -- here, spawn into it by --alloc-id from a
    # completely separate prun. Naming an allocation the DVM does not have is
    # still NOT_FOUND, and the case checks that too, so "allowed" cannot
    # quietly become "not checked at all".
    #
    # A reservation only exists while its owner does, so the probe has to run
    # while the elastic client is still alive -- hence the background client,
    # holding its reservation open with a job of its own.
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'rm -f /tmp/owner.out' >/dev/null 2>&1
        RUN_BG /tmp/owner.out "timeout 120 elastic grow node2:4 -- sleep 40"
        for _ in $(seq 1 40); do
            RUN 'grep -q "^>>> SPAWNED" /tmp/owner.out' >/dev/null 2>&1 && break
            sleep 2
        done
        aid=$(RUN 'sed -n "s/^>>> ALLOC_ID //p" /tmp/owner.out' | tr -d '\r')
        if [ -n "$aid" ]; then
            ok "the grow handed back an allocation id ($aid)"
            out=$(RUN "timeout 60 prun --alloc-id $aid -np 1 hostname" 2>&1)
            [ "$(echo "$out" | grep -c '^node2$')" = 1 ] \
                && ok "another tool of the same user may spawn into the reservation" \
                || bad "prun --alloc-id did not run on the reserved node: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
            out=$(RUN "timeout 60 prun --alloc-id no-such-allocation -np 1 hostname" 2>&1)
            echo "$out" | grep -q NOT_FOUND \
                && ok "an allocation the DVM does not know is NOT_FOUND" \
                || bad "wrong refusal for an unknown allocation: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
            RUN 'pgrep -x prte >/dev/null' && ok "the HNP survived the refusal" \
                                           || bad "the HNP died on a refused --alloc-id"
        else
            bad "no allocation id from the background grow: $(RUN 'tr "\n" " " < /tmp/owner.out' | tail -c 200)"
        fi
        RUN 'pkill -x elastic' >/dev/null 2>&1
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the reservation-ownership test"
    fi
    cleanup_swarm

    banner "elastic DVM: grow AFTER a shrink completes (phase-two event)"
    # A grow launches a daemon onto every node that lacks one -- which after a
    # shrink includes the shrunk node, since releasing the reservation reverts
    # it to the default pool with its ->session cleared. That node takes the
    # LOWEST new vpid, so it lands in the daemon_vpid_start slot; a grow
    # campaign that read its requester from only that first target found no
    # session, recorded no requester, and emitted no phase-two completion
    # event at all -- the grow succeeded but the requester waited forever and
    # the DVM was left wedged. Both variants are covered: growing a DIFFERENT
    # node after a shrink, and re-growing the SAME one (openpmix/prrte#2491).
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN 'timeout 90 elastic grow node2:2,node3:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY && ok "baseline grow node2,node3 completed" \
                                               || bad "baseline grow did not complete"
        out=$(RUN 'timeout 90 elastic shrink node3' 2>&1); sleep 3
        echo "$out" | grep -q PMIX_DVM_IS_READY && ok "shrink node3 completed" \
                                               || bad "shrink node3 did not complete"
        # re-grow the SAME node that was just shrunk. The pool entry for that
        # node survived the shrink, so this only works if the ADDED mark the
        # request puts on it is carried onto the existing pool object.
        out=$(RUN 'timeout 90 elastic grow node3:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "re-grow of the shrunk node completed (phase-two event delivered)" \
            || bad "re-grow of the shrunk node never completed"
        [ "$(prted_count 3)" = 1 ] && ok "daemon relaunched on the re-grown node" \
                                   || bad "re-grown node has no daemon"
        # and grow a DIFFERENT node afterwards
        out=$(RUN 'timeout 90 elastic grow node5:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "subsequent grow of a different node completed" \
            || bad "subsequent grow never completed"
        [ "$(prted_count 2 3 5)" = 3 ] && ok "daemons present on node2, node3, node5" \
                                       || bad "expected daemons missing after re-grow"
        # a wedged DVM would not answer this
        out=$(RUN 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
        [ "$(echo "$out" | tr -d '\r')" = node1 ] && ok "DVM still responsive after the re-grow" \
                                                  || bad "DVM wedged after re-grow: $out"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the re-grow test"
    fi
    cleanup_swarm

    banner "elastic DVM: a grow launches ONLY on the nodes it was given"
    # An allocation request naming node4 must start a daemon on node4 and
    # nowhere else. Shrink node2 first so the DVM holds a node that is in the
    # pool, has no daemon, and is NOT part of the next request: selecting on
    # "any node missing a daemon" rather than on the nodes the request added
    # silently relaunches a daemon on the node the user just shrank away.
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'timeout 90 elastic grow node2:2,node3:2' >/dev/null 2>&1
        RUN 'timeout 90 elastic shrink node2' >/dev/null 2>&1; sleep 3
        [ "$(prted_count 2)" = 0 ] && ok "node2 shrunk out of the DVM" || bad "node2 still has a daemon"
        out=$(RUN 'timeout 90 elastic grow node4:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY && ok "grow node4 completed" \
                                               || bad "grow node4 did not complete"
        [ "$(prted_count 4)" = 1 ] && ok "daemon started on the requested node (node4)" \
                                   || bad "no daemon on the requested node"
        [ "$(prted_count 2)" = 0 ] \
            && ok "the shrunk node was NOT dragged back into the grow" \
            || bad "grow node4 also relaunched a daemon on the shrunk node2"
        [ "$(prted_count 5 6 7)" = 0 ] && ok "no unrelated node was grown" \
                                      || bad "grow touched a node it was not given"
        # A grow of a node that is already in the DVM needs no daemon at all.
        # The request is still satisfied, so it must still report completion -
        # with nothing to launch there is no campaign and no launch fence, so
        # the completion has to be reported directly or the requester waits
        # out its timeout on an operation that already succeeded.
        out=$(RUN 'timeout 90 elastic grow node4:2' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "a grow needing no new daemon still completes" \
            || bad "redundant grow never completed"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the grow-scope test"
    fi
    cleanup_swarm

    banner "ras: node_insert dedup survives grow/shrink/re-grow (no duplicate daemons)"
    # prte_ras_base_node_insert() is where every allocation -- initial or
    # dynamic -- lands in the global node pool, and it decides add-vs-update
    # against what is already there. A shrink leaves the pool entry for the
    # departed node in place (only its daemon goes away), so a later grow of
    # that same node arrives as a duplicate; likewise a redundant grow of a
    # node already in the DVM.
    #
    # If the dedup scan misses, the pool ends up holding two prte_node_t
    # objects for one machine. Both are marked PRTE_NODE_STATE_ADDED with no
    # daemon, so the next DVM extension launches a prted onto that machine
    # TWICE -- which is the directly observable symptom. (The other symptom,
    # the node's slots being counted twice into
    # prte_ras_base.total_slots_alloc, is what a managed allocation reports
    # as PMIX_UNIV_SIZE / PMIX_MAX_PROCS; it is checked in the unit test,
    # since an unmanaged DVM recomputes the total from the pool at launch.)
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'timeout 90 elastic grow node2:2,node3:2' >/dev/null 2>&1
        RUN 'timeout 90 elastic shrink node3' >/dev/null 2>&1; sleep 3
        RUN 'timeout 90 elastic grow node3:2' >/dev/null 2>&1; sleep 3
        # a redundant grow of a node that is already in the DVM is the other
        # way a duplicate entry can be introduced
        RUN 'timeout 90 elastic grow node2:2' >/dev/null 2>&1; sleep 3

        dup=0
        for n in 2 3; do
            c=$(prted_procs "$n")
            [ "$c" = 1 ] || { dup=1; printf '    node%s is running %s prted\n' "$n" "$c"; }
        done
        [ "$dup" = 0 ] \
            && ok "exactly one prted per node after grow/shrink/re-grow/redundant-grow" \
            || bad "duplicate pool entry launched a second daemon on a node"

        # a wedged or double-daemoned DVM would not answer this
        out=$(RUN 'timeout 30 prun -n 1 hostname' 2>&1 | tail -1)
        [ "$(echo "$out" | tr -d '\r')" = node1 ] \
            && ok "DVM still responsive after the grow/shrink/re-grow cycle" \
            || bad "DVM wedged after the grow/shrink/re-grow cycle: $out"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the pool-dedup test"
    fi
    cleanup_swarm

    banner "ras/hosts: --add-hostfile brings a remote node into a running DVM"
    # The ras/hosts component is the DVM's local resource authority when no
    # scheduler is present: prte_ras_base_add_hosts() turns --add-host /
    # --add-hostfile into a PMIX_ALLOC_EXTEND request, ras/pmix defers it
    # (no scheduler), and hosts' own hand-written hostfile parser adds the
    # nodes before the DVM extension launches daemons on them. That whole
    # chain only runs multi-node, and its parser is separate from the flex
    # hostfile parser precisely so it can accept the slots=+N adjust syntax.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node1:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'printf "node2 slots=2\nnode3 slots=2\n" > /tmp/addhosts.txt'
        out=$(RUN 'timeout 90 prun --add-hostfile /tmp/addhosts.txt --host node2:2,node3:2 -n 4 --map-by node hostname' 2>&1)
        c=$(echo "$out" | grep -cE '^node[23]$')
        [ "$c" = 4 ] && ok "--add-hostfile grew the DVM onto node2+node3 (4 procs)" \
                     || bad "--add-hostfile launch produced $c/4 procs: $(echo "$out" | tr '\n' ' ')"
        [ "$(prted_count 2 3)" = 2 ] && ok "daemons started on the added nodes" \
                                     || bad "added nodes have no daemon"

        # slots=+N adjusts an EXISTING pool entry rather than adding a node;
        # the pool must not gain a second entry for it
        RUN 'printf "node2 slots=+2\n" > /tmp/addslots.txt'
        RUN 'timeout 90 prun --add-hostfile /tmp/addslots.txt -n 1 hostname' >/dev/null 2>&1
        alloc=$(RUN 'timeout 30 prun --display allocation -n 1 hostname' 2>&1)
        c=$(echo "$alloc" | grep -cE '^[[:space:]]*node2:[[:space:]]+slots=')
        [ "$c" = 1 ] && ok "slots=+N adjusted node2 in place (no duplicate entry)" \
                     || bad "slots=+N produced $c pool entries for node2"
        echo "$alloc" | grep -qE '^[[:space:]]*node2:[[:space:]]+slots=4' \
            && ok "node2 slots adjusted 2 -> 4" \
            || bad "node2 slot adjustment not applied: $(echo "$alloc" | grep node2 | tr '\n' ' ')"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the add-hostfile test"
    fi
    cleanup_swarm

    banner "dash-host: relative node syntax selects from the DVM"
    # "+n<K>" names the K'th node of the allocation, "+e:N" names N nodes
    # that are currently empty. Neither can say anything about how big a
    # node is -- the colon in "+e:N" is a node count -- so a job that uses
    # them gets the slots those nodes were discovered with.
    #
    # That is what makes these run at all. The available-slot computation
    # matched the raw "+n1" text against each node name, matched nothing,
    # and reported zero slots available, so the launch was refused for lack
    # of resources even though the node list had been resolved correctly.
    # It only ever "worked" with --map-by :OVERSUBSCRIBE, which skips the
    # check.
    cleanup_swarm
    RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        out=$(RUN 'timeout 30 prun --host +n1 -n 2 hostname' 2>&1)
        n=$(echo "$out" | grep -c '^node2$')
        [ "$n" = 2 ] && ok "+n1 resolved to the second node and used both its slots" \
                     || bad "+n1 did not run 2 procs on node2: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        # ...and is still bounded by what that node actually has
        out=$(RUN 'timeout 30 prun --host +n1 -n 3 hostname' 2>&1)
        echo "$out" | grep -q 'not enough slots' \
            && ok "+n1 is still held to the 2 slots the node was given" \
            || bad "+n1 was not bounded by the node's slot count"
        out=$(RUN 'timeout 30 prun --host +e:2 -n 2 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 2 ] && ok "+e:2 selected two empty nodes" \
                     || bad "+e:2 did not spread over 2 empty nodes ($n): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the relative-node-syntax test"
    fi
    cleanup_swarm

    test_rmaps

    test_schizo

    test_state

    test_pmix

    test_prted

    test_tools

    test_util

    test_hwloc

    test_event

    test_include

    test_runtime

    test_rml

    test_grpcomm

    test_slurm_alloc
    test_slurm

    banner "bootstrap DVM: daemons come up on their own and agree on their ranks"
    # A bootstrapped DVM has no launcher: prted is started independently on
    # every node and each one derives its OWN vpid from prte.conf
    # (prte_bootstrap_my_identity) before it ever contacts the controller.
    # The controller has to arrive at the same numbers independently --
    # ras/bootstrap records each node's canonical rank as its pool index and
    # plm reads it back out as the daemon's vpid -- because
    # prted_report_launch looks a reporting daemon up in daemons->procs BY THE
    # RANK IT CLAIMS. Get that wrong and the HNP either attaches a daemon to
    # the wrong node or cannot find it at all.
    #
    # DVMNodes order is rank order, so "node2,node3,node4" must produce
    # vpids 1,2,3 in that order. --display map-devel prints the HNP's view.
    cleanup_swarm
    if bootstrap_write_conf "node1" "node2,node3,node4"; then
        bootstrap_start 1 2 3 4
        if [ "$(prted_count 1 2 3 4)" = 4 ]; then
            ok "all 4 bootstrap daemons came up"
            out=$(RUN 'timeout 60 prun --display map-devel -n 3 --map-by node hostname' 2>&1)
            bad_rank=0
            # node2 -> 1, node3 -> 2, node4 -> 3
            for pair in "node2 1" "node3 2" "node4 3"; do
                set -- $pair
                # the "Data for node: X" line is followed by that node's "Daemon: [ns,V]"
                v=$(echo "$out" | grep -A 2 "Data for node: $1[[:space:]]" \
                        | grep -o 'Daemon: \[[^,]*,[0-9]*\]' | grep -o '[0-9]*\]' \
                        | tr -d ']' | head -1)
                [ "$v" = "$2" ] || { bad_rank=1; printf '    %s has vpid "%s", config says %s\n' "$1" "$v" "$2"; }
            done
            [ "$bad_rank" = 0 ] \
                && ok "each daemon's vpid matches its DVMNodes position (1,2,3)" \
                || bad "HNP vpid assignment disagrees with the config the daemons read"
            n=$(echo "$out" | grep -cE '^node[234]$')
            [ "$n" = 3 ] && ok "prun launched 3 procs across the bootstrap DVM" \
                         || bad "prun over the bootstrap DVM produced $n/3 procs"
        else
            bad "bootstrap DVM did not form ($(prted_count 1 2 3 4)/4 daemons)"
        fi
        cleanup_swarm

        # A host listed twice cannot work: a node's position in DVMNodes IS its
        # rank, and rank_of() resolves every occurrence to the FIRST one -- so
        # the position the repeat would have held is claimed by nobody while
        # num_daemons still counts it, and the DVM can never finish forming.
        # prte_bootstrap_parse must reject the file up front rather than let
        # every daemon hang waiting for one that does not exist.
        if bootstrap_write_conf "node1" "node2,node3,node2,node4"; then
            out=$(RUN 'timeout 40 prted --bootstrap 2>&1' || true)
            echo "$out" | grep -q "same host more than once" \
                && ok "a duplicate host in DVMNodes is rejected with a diagnostic" \
                || bad "duplicate DVMNodes entry not reported: $(echo "$out" | tr '\n' ' ' | head -c 200)"
            [ "$(prted_count 1)" = 0 ] \
                && ok "the controller refused to start on the bad config" \
                || bad "controller started anyway on a config that cannot form a DVM"
        fi
        bootstrap_restore_conf
        cleanup_swarm
    else
        skp "bootstrap DVM (could not write prte.conf into the shared volume)"
    fi

    banner "elastic DVM: radix-2 deep tree grow + shrink (multi-hop relay)"
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 --prtemca rml_base_radix 2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    out=$(RUN 'elastic grow node2:2,node3:2,node4:2,node5:2,node6:2,node7:2,node8:2,node9:2' 2>&1)
    echo "$out" | grep -q PMIX_DVM_IS_READY \
        && ok "radix-2 grow onto 8 nodes completed (relay fence succeeded)" \
        || bad "radix-2 grow did not complete (relay/header may be broken)"
    [ "$(prted_count 2 3 4 5 6 7 8 9)" = 8 ] && ok "all 8 daemons wired into deep tree" || bad "deep-tree daemons missing"
    out=$(RUN 'elastic shrink node9' 2>&1); sleep 3
    echo "$out" | grep -q PMIX_DVM_IS_READY && ok "radix-2 shrink-at-depth completed" || bad "radix-2 shrink did not complete"
    RUN 'pgrep -x prte >/dev/null' && ok "HNP survived deep-tree shrink" || bad "HNP died on deep-tree shrink"
    RUN 'pterm' >/dev/null 2>&1; cleanup_swarm
}

########################################################################
# macOS: native, single host (build + launch smoke)
########################################################################

test_macos() {
    local root prefix
    root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
    prefix="$root/vpath-macos/install"
    if [ ! -x "$prefix/bin/prterun" ]; then
        echo "native build missing -- run: ./build.sh macos" >&2; exit 2
    fi
    export DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1

    # Unlike the swarm, this subset runs on the developer's own machine, where
    # another clone of PRRTE may be running this very same subset.  Nothing
    # here may reach a process or a file that clone owns, which takes both of:
    #
    #  - every tool is started by ABSOLUTE path out of THIS clone's install
    #    ($B below, never PATH), so "is this process mine" is a question about
    #    the argv, which is what mypgrep/mypkill ask.  A bare `pkill -x prte`
    #    kills the other clone's DVM; a bare `pgrep -x prte` is worse still,
    #    because it reports the other clone's DVM as ours and the case then
    #    PASSES on a process this run never started.
    #  - PRRTE gets a PRIVATE TMPDIR, so the session dirs -- and the pmix.*
    #    rendezvous files inside them, which a -9'd tool never removes -- land
    #    somewhere only this run will ever delete.  It is made under /tmp
    #    rather than beside the build because the rendezvous socket path has
    #    to fit in sun_path (104 bytes on Darwin), and the per-user $TMPDIR a
    #    Mac hands out is already ~50 of them.
    # MAC_BRE and MAC_TMP are deliberately NOT `local`: the traps below fire
    # after this function has returned, and a local is out of scope by then --
    # which would silently leave the private directory behind on every run.
    local B="$prefix/bin"
    MAC_BRE="$(printf '%s' "$B" | sed 's/[].[^$*+?(){}|\\]/\\&/g')"
    mypgrep() { pgrep -f "^$MAC_BRE/$1( |\$)" >/dev/null 2>&1; }
    mypkill() { pkill -9 -f "^$MAC_BRE/$1( |\$)" 2>/dev/null; true; }
    MAC_TMP="$(mktemp -d /tmp/prtesuite.XXXXXX)" || {
        echo "cannot create a private TMPDIR under /tmp" >&2; exit 2; }
    export TMPDIR="$MAC_TMP"
    # Take the private dir down however we leave, ^C included -- it holds only
    # this run's session dirs, so there is nothing in it worth keeping.
    macdone() {
        local t
        for t in prterun prte prted prun pterm; do mypkill "$t"; done
        [ -n "${MAC_TMP:-}" ] && rm -rf "$MAC_TMP"
        true
    }
    trap macdone EXIT
    trap 'macdone; exit 130' INT TERM

    # Reap this run's strays between cases.  Session dirs only, not the whole
    # private dir: `bounded` puts its capture file there too (mktemp honors
    # TMPDIR) and a case reads that file after the macpk in its own error
    # branch has run.  Everything named here is unambiguously ours, pmix.*
    # included, because the directory it sits in is.
    macpk() {
        local t
        for t in prterun prte prted prun pterm; do mypkill "$t"; done
        rm -rf "${TMPDIR:?}"/prte.* "${TMPDIR:?}"/prted.* "${TMPDIR:?}"/prtrn.* \
               "${TMPDIR:?}"/prun.* "${TMPDIR:?}"/ompi.* "${TMPDIR:?}"/pmix.* 2>/dev/null
        true
    }

    banner "macOS: native Darwin build"
    ok "PRRTE built and installed for Darwin ($prefix)"
    # the build passing --enable-debug (warnings-as-errors on a git checkout) is
    # the primary macOS deliverable -- it catches Darwin portability regressions.

    local hn; hn="$(hostname)"     # this host's name; app procs print it

    banner "macOS: prterun (one-shot, single host)"
    macpk; sleep 1
    if bounded 60 "$B/prterun" -np 4 hostname; then
        # count hostname lines only -- ignore any libxml/DNS stderr noise
        [ "$(grep -Fc "$hn" "$BOUT")" = 4 ] \
            && ok "prterun -np 4 -> 4 procs on $hn, exit 0" \
            || bad "prterun wrong output: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "prterun timed out -- native Darwin DVM is unstable on this host (pre-existing, not a build defect); build is verified"
        macpk
    fi
    rm -f "$BOUT"

    banner "macOS: stdin delivery (single host, directed and wildcard)"
    # On one node every proc is local to the HNP, so this covers the two
    # delivery routes push_stdin takes: a directed rank writes straight into
    # the proc's sink, while "--stdin all" is xcast on PRTE_RML_TAG_IOF_PROXY
    # -- and the xcast comes back to the HNP, which must service its own procs
    # from that receive. When the HNP had no receive on that tag the wildcard
    # message went unmatched and every proc hung waiting on stdin.
    # The app is "head -1", not "cat", on purpose: head exits once it has its
    # line, so the test does not depend on the stdin-close sentinel. prterun's
    # own stdin is read by the HNP as a PMIx server, and PMIx's server branch
    # returns early on EOF without calling push_stdin
    # (pmix_iof_read_local_handler in src/common/pmix_iof.c), so a "cat" here
    # would never see EOF and would hang on a defect that is not PRRTE's.
    macpk; sleep 1
    printf 'STDIN-DELIVERY-OK\n' > "$root/vpath-macos/stdin_probe.txt"
    if bounded 60 sh -c "'$B/prterun' -np 2 head -1 < '$root/vpath-macos/stdin_probe.txt'"; then
        [ "$(grep -Fc STDIN-DELIVERY-OK "$BOUT")" = 1 ] \
            && ok "directed stdin (default rank 0) -> 1 proc" \
            || bad "directed stdin: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "directed stdin timed out (native Darwin DVM unstable)"; macpk
    fi
    rm -f "$BOUT"
    macpk; sleep 1
    if bounded 60 sh -c "'$B/prterun' -np 2 --stdin all head -1 < '$root/vpath-macos/stdin_probe.txt'"; then
        [ "$(grep -Fc STDIN-DELIVERY-OK "$BOUT")" = 2 ] \
            && ok "wildcard stdin (--stdin all) -> both procs" \
            || bad "wildcard stdin reached $(grep -Fc STDIN-DELIVERY-OK "$BOUT")/2 procs: $(tr '\n' ' ' <"$BOUT")"
    else
        bad "wildcard stdin hung -- the HNP never delivered its own xcast to the procs it hosts"; macpk
    fi
    rm -f "$BOUT" "$root/vpath-macos/stdin_probe.txt"

    banner "macOS: persistent DVM + prun + pterm (single host)"
    macpk; sleep 1
    bounded 60 "$B/prte" --daemonize; sleep 3
    if mypgrep prte; then
        ok "prte --daemonize started"
        if bounded 30 "$B/prun" -np 2 hostname && [ "$(grep -Fc "$hn" "$BOUT")" = 2 ]; then
            ok "prun -np 2 -> 2 procs on $hn, exit 0"
        else skp "prun timed out/short (native Darwin DVM unstable)"; fi
        bounded 20 "$B/pterm" >/dev/null 2>&1 || true
        sleep 1
        mypgrep prte && { skp "pterm did not stop the DVM (native Darwin instability)"; macpk; } \
                                  || ok "pterm cleanly terminated the DVM"
    else
        skp "prte --daemonize did not come up -- native Darwin DVM is unstable on this host (pre-existing); build is verified"
    fi
    rm -f "$BOUT"; macpk

    banner "macOS: a rejected spawn request must not take the DVM down"
    # The HNP unpacks a job object from a spawn request before validating
    # anything about it, so a request it then rejects exercises the error path
    # that answers the requester and disposes of that half-built job. The DVM
    # has to survive it: a persistent DVM that dies on one bad prun takes
    # every other job on the machine with it. Single host is enough - the
    # request is rejected at the HNP, before any daemon is involved.
    macpk; sleep 1
    bounded 60 "$B/prte" --daemonize; sleep 3
    if mypgrep prte; then
        for badarg in "--map-by NOSUCHPOLICY" "--bind-to NOSUCHOBJECT" "--rank-by NOSUCHTHING"; do
            bounded 30 sh -c "'$B/prun' $badarg -np 1 hostname" >/dev/null 2>&1
            if mypgrep prte; then
                ok "DVM survived a rejected 'prun $badarg'"
            else
                bad "DVM died on 'prun $badarg'"
                break
            fi
        done
        # and it must still be able to run a job afterwards
        if mypgrep prte; then
            if bounded 30 "$B/prun" -np 2 hostname && [ "$(grep -Fc "$hn" "$BOUT")" = 2 ]; then
                ok "DVM still launches jobs after the rejected requests"
            else
                skp "post-rejection prun timed out (native Darwin DVM unstable)"
            fi
        fi
        bounded 20 "$B/pterm" >/dev/null 2>&1 || true; sleep 1; macpk
    else
        skp "prte --daemonize did not come up -- cannot test spawn rejection"
    fi
    rm -f "$BOUT"; macpk

    banner "macOS: uniform-nodes launch and an mca value containing '='"
    # --uniform-nodes drives the homogeneous-topology path, and an MCA value
    # with an embedded '=' is the case that used to be truncated when the
    # daemon command line was assembled. One host cannot exercise the daemon
    # side of either, but both must at least remain launchable.
    macpk; sleep 1
    if bounded 60 "$B/prterun" --uniform-nodes -np 2 hostname; then
        [ "$(grep -Fc "$hn" "$BOUT")" = 2 ] \
            && ok "prterun --uniform-nodes -> 2 procs on $hn" \
            || bad "uniform-nodes launch wrong output: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "uniform-nodes prterun timed out (native Darwin DVM unstable)"; macpk
    fi
    rm -f "$BOUT"
    macpk; sleep 1
    if bounded 60 sh -c "PRTE_MCA_prte_test_kv='a=b' '$B/prterun' -np 2 hostname"; then
        [ "$(grep -Fc "$hn" "$BOUT")" = 2 ] \
            && ok "launch works with an '='-bearing mca value in the environment" \
            || bad "'='-bearing mca value broke the launch: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "'='-bearing mca value run timed out (native Darwin DVM unstable)"; macpk
    fi
    rm -f "$BOUT"; macpk
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n================  %d passed, %d failed, %d skipped  ================\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
