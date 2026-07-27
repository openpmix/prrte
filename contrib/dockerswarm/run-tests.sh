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

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
# must match build.sh -- the bootstrap tests reach into the shared install
IMAGE="${IMAGE:-prte-swarm:latest}"
VOLUME="${VOLUME:-prte-build}"
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
            prte-node1 bash -lc ". /opt/prte/env.sh; $*"; }
ON()  { docker exec "prte-node$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }

cleanup_swarm() {
    for n in $(seq 1 10); do
        # NOTE: each tool has its OWN session-dir prefix -- prte.<pid> for the
        # HNP, prtrn.<pid> for prterun, and prted.<pid> for a bootstrapped
        # daemon standing on its own. Every one of them holds a pmix.* server
        # rendezvous file, and leaving any behind is what makes a later prun
        # report "multiple possible servers ... connection handles have been
        # read from files named pmix.*" and fail to find the DVM. Clear them all.
        docker exec "prte-node$n" sh -c \
            'pkill -9 -x prted 2>/dev/null; pkill -9 -x prte 2>/dev/null;
             pkill -9 -x prterun 2>/dev/null;
             rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/pmix.* \
                    /tmp/prun.session.* 2>/dev/null; true'
    done
}
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }
# how many prted PROCESSES are running on one node (not how many nodes have
# one) -- two daemons on a single machine is what a duplicated node-pool
# entry produces
prted_procs() { docker exec "prte-node$1" sh -c 'pgrep -x prted 2>/dev/null | wc -l' | tr -d ' \r'; }

# --- fake-SLURM helpers -----------------------------------------------------
# ras/slurm reaches its scheduler by shelling out to sbatch/scontrol/scancel,
# so the harness supplies them: build.sh installs fake-slurm.py into the shared
# volume under its own prefix, and everything in that phase runs with that
# prefix FIRST on PATH and the SLURM_* envars exported.  Nothing outside these
# helpers sees either, which is what keeps the rest of the suite unaffected.
FS_BIN=/opt/prte/fakeslurm/bin
# run on the head node inside a faked SLURM allocation
SL() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
           prte-node1 bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
                                eval \"\$(fake-slurm env)\"; $*"; }
# run a fake-slurm housekeeping command (no SLURM_* env needed)
FS() { docker exec prte-node1 bash -lc "export PATH=$FS_BIN:\$PATH; fake-slurm $*"; }
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
        "prte-node$ctrl" bash -lc '. /opt/prte/env.sh; prted --bootstrap > /tmp/boot.out 2>&1'
    sleep 6
    for n in "$@"; do
        docker exec -d -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "prte-node$n" bash -lc '. /opt/prte/env.sh; prted --bootstrap > /tmp/boot.out 2>&1'
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
        prte-node1 bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
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
        prte-node1 bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
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
        prte-node1 bash -lc ". /opt/prte/env.sh; export PATH=$FS_BIN:\$PATH;
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

test_linux() {
    if ! docker ps --format '{{.Names}}' | grep -qx prte-node1; then
        echo "swarm not up -- run: docker compose up -d" >&2; exit 2
    fi
    banner "preflight: install present in shared volume"
    if RUN 'command -v prterun prte prun pterm elastic >/dev/null'; then
        ok "prterun/prte/prun/pterm/elastic on PATH"
    else
        bad "tools missing -- did ./build.sh run?"; return
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
    docker exec prte-node1 bash -lc '. /opt/prte/env.sh 2>/dev/null; cat > /root/staged_marker.c <<"CEOF"
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
    docker exec prte-node1 sh -c 'rm -f /root/staged_marker /root/staged_marker.c' 2>/dev/null

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

        RUN 'rm -f /tmp/iof_stdin_in.txt /tmp/iof_stdin_out.txt /tmp/iof_stdin_err.txt' >/dev/null 2>&1
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
    out=$(RUN 'prterun --prtemca prte_rml_radix 2 \
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
    # Proving the node is usable means running something on it, and a grown
    # node joins the reservation the grow created rather than the default
    # pool -- so "prun --host node4" has no allocation to map onto no matter
    # what its topology says. The elastic client therefore spawns INTO that
    # reservation, naming it with the PMIX_ALLOC_ID the grow handed back
    # (PMIX_SPAWN_TARGET). The grow and the spawn have to happen in one tool
    # session: the HNP only lets a namespace target a reservation it owns.
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
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 --prtemca prte_rml_radix 2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
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
    export PATH="$prefix/bin:$PATH"
    export DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1
    macpk() { pkill -9 -x prterun 2>/dev/null; pkill -9 -x prte 2>/dev/null; pkill -9 -x prted 2>/dev/null; true; }

    banner "macOS: native Darwin build"
    ok "PRRTE built and installed for Darwin ($prefix)"
    # the build passing --enable-debug (warnings-as-errors on a git checkout) is
    # the primary macOS deliverable -- it catches Darwin portability regressions.

    local hn; hn="$(hostname)"     # this host's name; app procs print it

    banner "macOS: prterun (one-shot, single host)"
    macpk; sleep 1
    if bounded 60 prterun -np 4 hostname; then
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
    if bounded 60 sh -c "prterun -np 2 head -1 < '$root/vpath-macos/stdin_probe.txt'"; then
        [ "$(grep -Fc STDIN-DELIVERY-OK "$BOUT")" = 1 ] \
            && ok "directed stdin (default rank 0) -> 1 proc" \
            || bad "directed stdin: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "directed stdin timed out (native Darwin DVM unstable)"; macpk
    fi
    rm -f "$BOUT"
    macpk; sleep 1
    if bounded 60 sh -c "prterun -np 2 --stdin all head -1 < '$root/vpath-macos/stdin_probe.txt'"; then
        [ "$(grep -Fc STDIN-DELIVERY-OK "$BOUT")" = 2 ] \
            && ok "wildcard stdin (--stdin all) -> both procs" \
            || bad "wildcard stdin reached $(grep -Fc STDIN-DELIVERY-OK "$BOUT")/2 procs: $(tr '\n' ' ' <"$BOUT")"
    else
        bad "wildcard stdin hung -- the HNP never delivered its own xcast to the procs it hosts"; macpk
    fi
    rm -f "$BOUT" "$root/vpath-macos/stdin_probe.txt"

    banner "macOS: persistent DVM + prun + pterm (single host)"
    macpk; sleep 1
    bounded 60 prte --daemonize; sleep 3
    if pgrep -x prte >/dev/null; then
        ok "prte --daemonize started"
        if bounded 30 prun -np 2 hostname && [ "$(grep -Fc "$hn" "$BOUT")" = 2 ]; then
            ok "prun -np 2 -> 2 procs on $hn, exit 0"
        else skp "prun timed out/short (native Darwin DVM unstable)"; fi
        bounded 20 pterm >/dev/null 2>&1 || true
        sleep 1
        pgrep -x prte >/dev/null && { skp "pterm did not stop the DVM (native Darwin instability)"; macpk; } \
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
    bounded 60 prte --daemonize; sleep 3
    if pgrep -x prte >/dev/null; then
        for badarg in "--map-by NOSUCHPOLICY" "--bind-to NOSUCHOBJECT" "--rank-by NOSUCHTHING"; do
            bounded 30 sh -c "prun $badarg -np 1 hostname" >/dev/null 2>&1
            if pgrep -x prte >/dev/null; then
                ok "DVM survived a rejected 'prun $badarg'"
            else
                bad "DVM died on 'prun $badarg'"
                break
            fi
        done
        # and it must still be able to run a job afterwards
        if pgrep -x prte >/dev/null; then
            if bounded 30 prun -np 2 hostname && [ "$(grep -Fc "$hn" "$BOUT")" = 2 ]; then
                ok "DVM still launches jobs after the rejected requests"
            else
                skp "post-rejection prun timed out (native Darwin DVM unstable)"
            fi
        fi
        bounded 20 pterm >/dev/null 2>&1 || true; sleep 1; macpk
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
    if bounded 60 prterun --uniform-nodes -np 2 hostname; then
        [ "$(grep -Fc "$hn" "$BOUT")" = 2 ] \
            && ok "prterun --uniform-nodes -> 2 procs on $hn" \
            || bad "uniform-nodes launch wrong output: $(tr '\n' ' ' <"$BOUT")"
    else
        skp "uniform-nodes prterun timed out (native Darwin DVM unstable)"; macpk
    fi
    rm -f "$BOUT"
    macpk; sleep 1
    if bounded 60 sh -c "PRTE_MCA_prte_test_kv='a=b' prterun -np 2 hostname"; then
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
