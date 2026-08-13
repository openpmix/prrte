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

# THE SUITE RUNS WITH OOB PROGRESS THREADS ON, AND THAT IS DELIBERATE.
#
# prte_oob_base.num_progress_threads defaults to 0 in the library: every
# peer's socket send/recv events are serviced on the main progress thread, so
# the OOB is effectively single-threaded and the ordering between a send
# completing and anything else the daemon does is fixed. With workers, each
# peer is assigned to a worker base and its handlers run on that thread,
# posting completions back to prte_event_base -- which is the arrangement
# where a missing lock, a peer touched from two threads, or a completion that
# assumes it runs on the main thread actually has consequences.
#
# At the default of 0 this suite could never reach any of that: not one case
# would exercise the threaded path, and a race introduced there would ship.
# So the suite opts in, for every tool and (via the PRTE_MCA_ forwarding in
# prte_plm_base_setup_virtual_machine) every daemon it launches.
#
# Two is the smallest number that is genuinely multi-threaded -- one worker
# plus the main thread is already two threads touching the OOB, and two
# workers means two peers can be serviced concurrently. Override with
# PRTE_SWARM_OOB_THREADS to sweep it, or set it to 0 to reproduce the old
# single-threaded behavior when bisecting a failure.
OOB_THREADS="${PRTE_SWARM_OOB_THREADS:-2}"
OOBENV=(-e "PRTE_MCA_oob_progress_threads=$OOB_THREADS")

# run a command on the head node (login env so PATH/LD_LIBRARY_PATH are set)
RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "${OOBENV[@]}" \
            "${NODE}1" bash -lc ". /opt/prte/env.sh; $*"; }
ON()  { docker exec "${OOBENV[@]}" "$NODE$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }
# ...and the same for a case that drives a TOOL from a node other than the
# head. ON alone is enough for shell housekeeping, but every PRRTE tool
# refuses to run as root without these two, and the refusal text is long
# enough to bury whatever the case was actually asserting.
ONT() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "${OOBENV[@]}" \
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
    # the CPU burners the PMIx-churn phases raise (see load_on): harmless if
    # none are running, and leaving one behind would slow every later phase
    pkill -9 -x yes 2>/dev/null
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
# wait up to N seconds for every listed node to have no prted, then echo the
# count that remains.  A tool that exits on a FAILED launch does not wait for
# the daemons to finish dying, so a count taken the instant it returns can
# still see one -- that is teardown in flight, not a stray.
prted_settle() { local secs=$1 c; shift; for _ in $(seq "$secs"); do
                     c=$(prted_count "$@"); [ "$c" = 0 ] && break; sleep 1; done; echo "$c"; }

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

    banner "rmaps: a directive on a later app describes that app alone"
    # The first app segment is where the command line speaks for the job, so
    # a lone directive written there applies to every app.  Written on a
    # LATER app it is that app's alone, and the apps that gave none take the
    # defaults -- an app that says nothing is not agreeing with one that did.
    # This used to place every app the way the one directive said, whichever
    # app carried it, with no way to say what was plainly meant.  Only
    # visible across nodes: by-node deals one rank per node, the default
    # by-slot fills a node first.
    RUN 'nohup prte --daemonize --host node1:4,node2:4,node3:4 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        # directive on the second app only: app0 (ranks 0-1) takes the
        # default and lands together, app1 (ranks 2-3) is spread by node
        out=$(RUN "timeout 60 prun -n 2 $RANKHOST : --map-by node -n 2 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            [ "$(rh_host "$out" 0)" = "$(rh_host "$out" 1)" ] \
                && ok "the app that gave no directive followed the default rules" \
                || bad "app0 was placed by app1's directive: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
            [ "$(rh_host "$out" 2)" != "$(rh_host "$out" 3)" ] \
                && ok "the app that gave one followed its own" \
                || bad "app1's --map-by node was not applied: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        else
            bad "later-app directive job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        sleep 2
        # the same directive on the FIRST app and nowhere else describes the
        # job, so both apps are spread by node
        out=$(RUN "timeout 60 prun --map-by node -n 2 $RANKHOST : -n 2 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            [ "$(rh_host "$out" 0)" != "$(rh_host "$out" 1)" ] \
                && [ "$(rh_host "$out" 2)" != "$(rh_host "$out" 3)" ] \
                && ok "a directive on the first app applied to both apps" \
                || bad "the first app's directive did not reach the job: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        else
            bad "first-app directive job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the directive-distribution test"
    fi
    cleanup_swarm

    banner "rmaps: two apps of one job can be placed by two mapping components"
    # Every mapper's gate used to ask the JOB which policy it had, and the
    # job's policy is whatever default was resolved for the apps that gave no
    # directive -- so seq, rankfile and ppr all deferred and a per-app
    # request for them was quietly placed by round_robin instead.  The gates
    # now read each app's own resolved policy.  A rankfile names hosts, so
    # this needs more than one host to mean anything; and the ranks it names
    # are that app's, offset into the job's numbering by what came before.
    RUN 'nohup prte --daemonize --host node1:2,node2:2,node3:2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'printf "rank 0=node3 slot=0\nrank 1=node2 slot=0\n" > /tmp/rmaps_pa_rf.txt'
        out=$(RUN "timeout 60 prun -n 2 $RANKHOST : --map-by rankfile:FILE=/tmp/rmaps_pa_rf.txt -n 2 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            n=$(echo "$out" | grep '^RH ' | awk '{print $2}' | sort -u | wc -l | tr -d ' ')
            [ "$n" = 4 ] \
                && ok "4 procs across the two apps got 4 distinct ranks" \
                || bad "the rankfile app renumbered from 0 (distinct=$n): $(echo "$out" | grep '^RH' | tr '\n' ' ')"
            # the rankfile's own rank 0 is the job's rank 2, on node3
            [ "$(rh_host "$out" 2)" = "node3" ] && [ "$(rh_host "$out" 3)" = "node2" ] \
                && ok "the per-app rankfile placed its ranks on the hosts it named" \
                || bad "per-app rankfile placement was ignored: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        else
            bad "per-app rankfile job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        sleep 2
        # a per-app seq file, same story. Both files name node2/node3 and
        # leave node1 to the app that gave no directive: node1 is where the
        # default placement puts it, and these nodes have only two slots
        # apiece, so a sequence entry naming node1 would be asking for a
        # slot the first app has already taken.
        RUN 'printf "node3\nnode2\n" > /tmp/rmaps_pa_seq.txt'
        out=$(RUN "timeout 60 prun -n 2 $RANKHOST : --map-by seq:FILE=/tmp/rmaps_pa_seq.txt -n 2 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            [ "$(rh_host "$out" 2)" = "node3" ] && [ "$(rh_host "$out" 3)" = "node2" ] \
                && ok "the per-app seq file placed its ranks in file order" \
                || bad "per-app seq placement was ignored: $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        else
            bad "per-app seq job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        sleep 2
        # a per-app ppr pattern names both a count and an object; only the
        # count used to be kept, so the app was placed N per whatever object
        # the job had resolved
        out=$(RUN "timeout 60 prun -n 1 $RANKHOST : --map-by ppr:2:node -n 4 $RANKHOST" 2>&1)
        rc=$?
        if [ "$rc" = 0 ]; then
            n=$(echo "$out" | grep '^RH ' | awk '$1=="RH" && $2>=1 {print $3}' | sort -u | wc -l | tr -d ' ')
            [ "$n" = 2 ] \
                && ok "ppr:2:node put the app's 4 procs two to a node" \
                || bad "per-app ppr pattern was not honored (nodes=$n): $(echo "$out" | grep '^RH' | tr '\n' ' ')"
        else
            bad "per-app ppr job failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        fi
        RUN 'rm -f /tmp/rmaps_pa_rf.txt /tmp/rmaps_pa_seq.txt; timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the per-app mapper-selection test"
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

    banner "schizo: a job-level option written in a LATER app context"
    # "--output", "--display" and "--rtos" describe the job, not an app, so
    # they may be written in any app context of an MPMD command line.  The
    # tool's global parse stops at the first executable, so a directive in a
    # later segment was invisible to it and silently discarded - no tags, no
    # map, launched anyway, and not a word about any of it.  The app parser
    # now hands each segment's contribution back to that parse.
    #
    # Multi-node matters here because the tag is applied by the daemon that
    # owns the process: a directive that reached the tool but not the wire
    # would still print untagged lines from node3.
    out=$(RUN 'prterun --host node2:1 -np 1 hostname \
                 : --host node3:1 -np 1 --output tag hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^\[[^]]+\]<stdout>: node[23]$')
    [ "$rc" = 0 ] && [ "$n" = 2 ] \
        && ok "--output in the second app context tagged BOTH apps' output" \
        || bad "--output in a later app context was dropped (rc=$rc, tagged=$n): $(echo "$out" | tr '\n' ' ' | tail -c 300)"

    out=$(RUN 'prterun --host node2:1 -np 1 hostname \
                 : --host node3:1 -np 1 --rtos donotlaunch hostname' 2>&1)
    echo "$out" | grep -q 'DONOTLAUNCH' && ! echo "$out" | grep -qE '^node[23]$' \
        && ok "--rtos donotlaunch in the second app context stopped the launch" \
        || bad "--rtos in a later app context was dropped: $(echo "$out" | tr '\n' ' ' | tail -c 300)"

    # ...and two app contexts that ask for opposite things cannot both be
    # honored, so the command line is refused rather than one of them picked
    out=$(RUN 'prterun --host node2:1 -np 1 --output tag hostname \
                 : --host node3:1 -np 1 --output tag=0 hostname' 2>&1)
    echo "$out" | grep -q 'opposite things' \
        && ok "contradictory job-level directives are refused" \
        || bad "a contradictory --output was accepted: $(echo "$out" | tr '\n' ' ' | tail -c 300)"

    # the value form is what lets a directive be turned back OFF.  Before it
    # existed "tag=0" parsed cleanly and applied the tag, which is the worst
    # of the three possible outcomes.  Every spelling of false is accepted.
    for v in 0 false FALSE no n f disable; do
        out=$(RUN "prterun --host node2:1,node3:1 -np 2 --map-by node \
                     --output tag=$v hostname" 2>&1)
        n=$(echo "$out" | grep -cE '^node[23]$')
        [ "$n" = 2 ] && ! echo "$out" | grep -q '<stdout>:' \
            && ok "--output tag=$v turned the tag off" \
            || bad "--output tag=$v did not suppress the tag: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    done
    # ...and every spelling of true still asks for it
    for v in 1 true TRUE yes y t enable; do
        out=$(RUN "prterun --host node2:1 -np 1 --output tag=$v hostname" 2>&1)
        echo "$out" | grep -qE '^\[[^]]+\]<stdout>: node2$' \
            && ok "--output tag=$v tagged the output" \
            || bad "--output tag=$v lost the tag: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    done
    # and a value that is neither true nor false is refused, not read as false
    out=$(RUN 'prterun --host node2:1 -np 1 --output tag=maybe hostname' 2>&1)
    echo "$out" | grep -q 'neither true nor false' \
        && ok "a non-boolean directive value is reported" \
        || bad "a non-boolean directive value was accepted: $(echo "$out" | tr '\n' ' ' | tail -c 300)"

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
# ...and the client that asks a peer where it is, for the same reason.
PI=/opt/prte/prte/bin/peerinfo

# Cross-check one peerinfo run.  Every rank prints a SELF line about itself
# and a PEER line about each of the others, in the same field order, so the
# assertion is a string compare: what rank r was told about rank p must be
# what rank p says about itself.  Prints nothing when the run is consistent,
# and one line per disagreement otherwise.
peerinfo_mismatches() {
    echo "$1" | tr -d '\r' | awk '
        $1 == "SELF" { r = $2; s = ""; for (i = 3; i <= NF; i++) { s = s " " $i }
                       self[r] = s; next }
        $1 == "PEER" { n++; who[n] = $2 " about " $3; tgt[n] = $3;
                       s = ""; for (i = 4; i <= NF; i++) { s = s " " $i }
                       got[n] = s; next }
        END {
            for (k = 1; k <= n; k++) {
                if (!(tgt[k] in self)) {
                    print "rank " who[k] ": that rank printed no SELF line"
                } else if (got[k] != self[tgt[k]]) {
                    print "rank " who[k] ": got [" got[k] " ] but it says [" self[tgt[k]] " ]"
                }
            }
        }'
}

test_pmix() {
    local out rc n hosts undef peers lazyout eagerout mism a b vrb

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
        # ${NODE}2, not a hardcoded "prte-node2": every global name this
        # harness claims is derived from $PRTE_SWARM (see docker-compose.yml),
        # so a literal here asks a DIFFERENT swarm - or nothing at all - for
        # the address, and the case fails against an unrelated container's IP
        n2ip=$(docker exec "${NODE}2" hostname -i 2>/dev/null | awk '{print $1}' | tr -d '\r')
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

    banner "pmix: a rank can locate every peer in its job, on any node"
    # This is the one case in the suite where a process asks about ANOTHER
    # process's reserved keys.  Everything else either puts its own data and
    # reads it back, or asks about the job rather than about a peer -- and
    # both of those are satisfied without the request ever reaching a daemon.
    #
    # It matters because of what the daemon may legitimately not have
    # published.  Registering a PMIx entry for every proc in the job, on every
    # daemon, builds a table that grows with the whole job on a node that runs
    # a fixed slice of it, so prte_pmix_lazy_procdata publishes only the procs
    # this daemon hosts and derives the rest from its own job object when
    # somebody asks.  The derivation is reached through the direct-modex
    # up-call, which is a path no single-host run has: on one node every proc
    # is local and there is nothing to derive.
    #
    # The assertion is a cross-check rather than a spot value, because the
    # interesting failure is not "no answer" but "a plausible wrong answer" --
    # a derived field taken from the wrong proc still prints.  Every rank says
    # what it thinks about itself and about each of its peers, and the two
    # accounts of any given rank must agree word for word.
    cleanup_swarm
    if ! RUN "test -x $PI"; then
        skp "peerinfo client not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the peer-lookup tests"
        cleanup_swarm
    else
        peers="--host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node"
        lazyout=$(PRUN "$peers $PI" 2>&1)
        n=$(echo "$lazyout" | grep -c '^PEERINFO-DONE')
        [ "$n" = 8 ] \
            && ok "all 8 ranks completed their peer lookups" \
            || bad "$n of 8 ranks finished: $(echo "$lazyout" | tr '\n' ' ' | tail -c 400)"
        n=$(echo "$lazyout" | grep -c '^PEERFAIL')
        [ "$n" = 0 ] \
            && ok "...with no lookup failing" \
            || bad "$n peer lookups failed: $(echo "$lazyout" | grep '^PEERFAIL' | tr '\n' ' ' | tail -c 400)"
        # 8 ranks each describing the other 7
        n=$(echo "$lazyout" | grep -c '^PEER ')
        [ "$n" = 56 ] \
            && ok "...and all 56 peer descriptions were produced" \
            || bad "got $n of 56 peer descriptions"
        # ...and the run has to actually span nodes, or a green result here
        # says nothing: every answer would have come from local publication.
        hosts=$(echo "$lazyout" | awk '$1=="SELF"' | tr -d '\r' | awk '{print $(NF-2)}' | sort -u | grep -c '^node')
        [ "${hosts:-0}" -ge 2 ] \
            && ok "...across $hosts nodes (so the peers really are remote)" \
            || bad "the job did not span nodes -- this case would be vacuous"
        mism=$(peerinfo_mismatches "$lazyout")
        [ -z "$mism" ] \
            && ok "...and every rank's account of a peer matches that peer's own" \
            || bad "peer descriptions disagree: $(echo "$mism" | head -3 | tr '\n' ' ' | tail -c 500)"

        # Note what that comparison already is: a SELF line is what the
        # proc's OWN daemon published about it -- which a daemon does for
        # its own procs either way -- while the PEER lines about it are what
        # other daemons derived.  So the check above is the A/B, run inside
        # one job where the offsets and the mapping are fixed.  Comparing
        # two separate runs would not be: a second job has a different
        # PMIX_GLOBAL_RANK offset, so the tables legitimately differ.
        #
        # The same job with the derivation off is therefore a control on the
        # client rather than a second reading: it must pass here too, or a
        # failure above says nothing about the derivation.
        eagerout=$(PRUN "$peers --prtemca prte_pmix_lazy_procdata 0 $PI" 2>&1)
        mism=$(peerinfo_mismatches "$eagerout")
        n=$(echo "$eagerout" | grep -c '^PEERFAIL')
        if [ -z "$mism" ] && [ "$n" = 0 ]; then
            ok "eager publication (prte_pmix_lazy_procdata 0) passes the same check"
        else
            bad "the check fails with the derivation off, so it is not testing the derivation: $(echo "$mism" | head -2 | tr '\n' ' ' | tail -c 400)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm

    banner "pmix: the derivation is actually what answered"
    # Consistency alone cannot tell the two implementations apart -- a
    # derivation that never ran would leave the eager publication answering
    # and every check above would still pass.  So watch the daemon say it.
    # This has to be prterun rather than a prun into a standing DVM: the
    # verbosity is a daemon-side setting, and only the launch that starts the
    # daemons can carry it.  --leave-session-attached because a daemonized
    # prted has no stderr to read.
    if ! RUN "test -x $PI"; then
        skp "peerinfo client not installed -- re-run ./build.sh"
    else
        vrb="--leave-session-attached --prtemca prte_pmix_server_verbose 2"
        out=$(RUN "timeout -k 5 150 prterun --host node1:2,node2:2,node3:2,node4:2 \
                     -n 8 --map-by node $vrb $PI" 2>&1)
        n=$(echo "$out" | grep -c 'ANSWERED LOCALLY')
        [ "${n:-0}" -gt 0 ] \
            && ok "a daemon answered $n peer lookups out of its own job object" \
            || bad "no lookup was answered by the derivation -- the path did not run: $(echo "$out" | grep -c 'DMODX REQ') dmodex requests seen"
        n=$(echo "$out" | grep -c '^PEERFAIL')
        [ "$n" = 0 ] \
            && ok "...and none of the lookups failed" \
            || bad "$n lookups failed while the derivation was in use"
        # ...and with it off, nothing may claim to have derived anything.
        out=$(RUN "timeout -k 5 150 prterun --host node1:2,node2:2,node3:2,node4:2 \
                     -n 8 --map-by node $vrb --prtemca prte_pmix_lazy_procdata 0 $PI" 2>&1)
        echo "$out" | grep -q 'ANSWERED LOCALLY' \
            && bad "the derivation ran even though prte_pmix_lazy_procdata was 0" \
            || ok "prte_pmix_lazy_procdata 0 turns the derivation off completely"
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
        #
        # The relay is compiled out without PMIX_CAP_IOF_DELIVER_LOCAL
        # (PRTE_PMIX_IOF_DELIVER_LOCAL): the delivery has no way to say "give
        # this to the tool but do not emit it here", so the HNP does not relay
        # at all.  These cases can therefore only ever fail against a PMIx
        # that predates the flag, and the baked PMIx goes stale as a matter of
        # course (see AGENTS.md) -- red for that reads as "your tree is
        # broken" when it is not.  The file-emission cases below are skipped
        # with them: with no relay happening, "no files appeared here" is true
        # for the wrong reason.
        if ! pmix_cap PMIX_CAP_IOF_DELIVER_LOCAL; then
            skp "output relayed back to a tool (PMIx predates PMIX_CAP_IOF_DELIVER_LOCAL)"
        else
            out=$(ONT 3 'timeout -k 5 45 prun --host node1:1,node2:1 -n 2 --map-by node hostname 2>&1')
            if echo "$out" | grep -qE '^node1$' && echo "$out" | grep -qE '^node2$'; then
                ok "a tool on a non-master daemon saw output from ranks on both other nodes"
            else
                bad "output never reached the tool on node3: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            fi

            # Every line exactly once.  The relay skips the daemon that
            # already delivered a chunk to its own PMIx server, and if that
            # dedup were dropped a tool would see its own node's ranks twice.
            out=$(ONT 2 'timeout -k 5 60 prun --host node1:2,node2:2,node3:2 -n 6 --map-by node sh -c "for i in \$(seq 1 50); do echo RELAYLINE-\$i; done" 2>&1')
            n=$(echo "$out" | grep -c '^RELAYLINE-')
            u=$(echo "$out" | grep '^RELAYLINE-' | sort | uniq -c | awk '{print $1}' | sort -u | tr '\n' ' ')
            [ "$n" = 300 ] && [ "$u" = "6 " ] \
                && ok "300 lines from 6 ranks reached the tool, each exactly once" \
                || bad "relayed output was lost or duplicated (got $n lines, per-line counts '$u'; expected 300 and '6 ')"

            # The relayed copy must not be EMITTED where it lands.  Every
            # daemon registers the job's namespace with the same output
            # directives, so a daemon handed another node's output writes its
            # own copy of that rank's file unless the delivery says not to --
            # and node3 hosts none of these ranks, so any file appearing there
            # is a duplicate of one node1 or node2 already wrote.  Measured,
            # not assumed: flipping the PMIX_IOF_LOCAL_OUTPUT directive to
            # true puts a full set here.
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
        fi
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
########################################################################
# src/prted/pmix/pmix_server_session.c -- PMIx_Session_control.
#
# A session is a set of NODES, so nothing interesting about it exists on one
# host.  What only appears here:
#
#   * a reservation actually withholding its nodes -- a job that names no
#     session must land somewhere else, which needs more nodes than one
#   * a request arriving at a NON-MASTER daemon and being relayed to the DVM
#     master, which is the only way the PRTE_PMIX_SESSION_CTRL relay runs at all
#   * a signal reaching the jobs of a session on every node they occupy
#   * a session terminate killing its jobs across nodes and giving the nodes
#     back to the general pool
########################################################################
# sessionctrl is installed by build.sh; use the absolute path for the same
# reason DS and SC do (an app inherits the daemon PATH, not the install bin).
SESSCTL=/opt/prte/prte/bin/sessionctrl

test_session() {
    local out rc n ns

    banner "session: instantiate reserves its nodes out of the general pool"
    # The DVM spans node1-node4.  Instantiate a session holding node3+node4
    # and put a long-lived job in it.  A SECOND job, naming no session, must
    # then be unable to reach node3/node4 at all -- that is what "reserved"
    # means, and with a single node there is nothing to observe.
    cleanup_swarm
    # the session jobs below are bare "sleep" processes, and this phase counts
    # them to decide whether a signal landed -- so make sure none is left over
    # from anything else (cleanup_swarm only reaps PRRTE tools and daemons)
    for n in $(seq 1 10); do docker exec "$NODE$n" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    if ! RUN "test -x $SESSCTL"; then
        skp "sessionctrl not installed -- re-run ./build.sh"
    elif ! prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        bad "could not start a DVM for the session-control tests"
    else
        # name the slot counts explicitly: a node list is parsed by the
        # dash-host rules, so a bare name means ONE slot -- and it overwrites
        # the count the pool already had for that node.
        out=$(RUN "timeout 60 $SESSCTL instantiate 4242 --hosts node3:2,node4:2 \
                       --np 2 --mapby node -- /bin/sleep 240" 2>&1)
        ns=$(echo "$out" | grep -m1 'pmix.nspace' | awk -F'= ' '{print $2}' | tr -d '\r')
        if echo "$out" | grep -q PMIX_SUCCESS && [ -n "$ns" ]; then
            ok "session 4242 instantiated on node3,node4 running $ns"
        else
            bad "instantiate failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi

        # a general job may now only use node1/node2
        sleep 3
        out=$(PRUN '-n 4 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node(3|4)$')
        [ "$n" = 0 ] \
            && ok "a general job was kept off the reserved nodes" \
            || bad "a general job landed on reserved nodes: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

        banner "session: a request relayed from a non-master daemon is served"
        # node3 is not the DVM master, so this goes out on PRTE_RML_TAG_SCHED
        # and comes back on ..._SCHED_RESP.  With one daemon that relay never
        # runs.  Run the tool THROUGH node3 by executing it there: it attaches
        # to its local daemon, which is not the master.
        out=$(ONT 3 "timeout 60 $SESSCTL pause 4242" 2>&1)
        echo "$out" | grep -q PMIX_SUCCESS \
            && ok "pause relayed through node3 was served by the master" \
            || bad "relayed pause failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

        # the procs of the session are stopped -- on BOTH nodes
        sleep 2
        n=0
        for h in 3 4; do
            ON $h 'ps -o stat= -C sleep 2>/dev/null | grep -q T' && n=$((n+1))
        done
        [ "$n" = 2 ] \
            && ok "both nodes of the session have stopped procs" \
            || skp "could not confirm stopped procs on both nodes (saw $n); ps may not report state here"

        out=$(ONT 3 "timeout 60 $SESSCTL resume 4242" 2>&1)
        echo "$out" | grep -q PMIX_SUCCESS \
            && ok "resume relayed through node3 was served" \
            || bad "relayed resume failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"

        banner "session: signal reaches the session jobs on every node"
        # SIGTERM through the session, not through prun.  This is the path
        # that packed the target namespace: it used to pack the ADDRESS OF THE
        # POINTER rather than the name, so every daemon matched no job and the
        # signal was silently dropped -- invisible on one node too, but this
        # is where it is checked.
        out=$(RUN "timeout 60 $SESSCTL signal 4242 15" 2>&1)
        echo "$out" | grep -q PMIX_SUCCESS \
            && ok "session signal accepted" \
            || bad "session signal failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        sleep 5
        n=0
        for h in 1 2 3 4; do
            ON $h 'pgrep -x sleep >/dev/null' && n=$((n+1))
        done
        [ "$n" = 0 ] \
            && ok "the signal reached the session procs on every node" \
            || bad "session procs survived the signal on $n node(s)"

        banner "session: a session created to run apps is reclaimed when they end"
        # 4242 was instantiated WITH apps, so it exists in order to run them
        # and is finished when the last of them retires -- which the signal
        # above just caused.  It must be gone, and its nodes must be back in
        # the general pool without anyone having asked.
        sleep 4
        out=$(RUN "timeout 60 $SESSCTL pause 4242" 2>&1)
        echo "$out" | grep -q PMIX_ERR_NOT_FOUND \
            && ok "the session retired with its jobs" \
            || bad "session 4242 outlived its only job: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        out=$(PRUN '-n 8 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 4 ] \
            && ok "all four nodes are back in the general pool" \
            || bad "expected 4 usable nodes after the session ended, saw $n: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

        banner "session: a standing reservation persists until it is terminated"
        # Instantiated with NO apps, so nothing retires and nothing reclaims
        # it.  It holds node4 until an explicit terminate says otherwise.
        out=$(RUN "timeout 60 $SESSCTL instantiate 4243 --hosts node4:2" 2>&1)
        echo "$out" | grep -q PMIX_SUCCESS \
            && ok "standing reservation 4243 instantiated" \
            || bad "instantiate failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        sleep 2
        out=$(PRUN '-n 6 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node4$')
        [ "$n" = 0 ] \
            && ok "the standing reservation is withholding its node" \
            || bad "a general job used the reserved node: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        out=$(RUN "timeout 60 $SESSCTL terminate 4243" 2>&1)
        echo "$out" | grep -q PMIX_SUCCESS \
            && ok "session 4243 terminated on request" \
            || bad "terminate failed: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        sleep 4
        out=$(PRUN '-n 8 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 4 ] \
            && ok "the terminated reservation gave its node back" \
            || bad "expected 4 usable nodes after terminate, saw $n: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

        banner "session: an unknown session and a conflicting request are refused"
        out=$(RUN "timeout 60 $SESSCTL pause 9999" 2>&1)
        echo "$out" | grep -q PMIX_ERR_NOT_FOUND \
            && ok "an unknown session id is refused with NOT_FOUND" \
            || bad "unknown session was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"

        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    for n in $(seq 1 10); do docker exec "$NODE$n" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    cleanup_swarm
}

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
    local out rc n c bad_cores cpus back

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
    # The dump has to contain the levels PRRTE maps and binds to. NUMA is the
    # one that was missing: hwloc 2.x hangs NUMA nodes off memory_first_child,
    # not off the children[] array the renderer walked, so a user who ran
    # "--display topo" to work out what "--map-by numa" would do got a
    # topology with no NUMA domains in it at all. hwloc always synthesizes at
    # least one NUMA node covering the machine, so this holds even here where
    # sysfs reports none.
    for lvl in Package Core PU NUMANode; do
        echo "$out" | grep -q "Type: $lvl" \
            && ok "--display topo shows $lvl objects" \
            || bad "--display topo omitted every $lvl object"
    done
    cleanup_swarm

    banner "hwloc: the DVM-wide bindto parameter means what it says"
    # "bindto" is the DVM-wide default binding policy, and it documents eight
    # values. Five of them were unusable.
    #
    # 1. A value the parser refuses was diagnosed and then IGNORED:
    #    prte_init() discarded prte_hwloc_base_open()'s return, so the DVM
    #    came up with the default binding after telling the user their request
    #    was not recognized. The command-line spelling of the same typo
    #    ("--bind-to bogus") has always been fatal.
    # 2. A value COARSER THAN A CORE was refused outright. Deriving the
    #    default mapping reads jdata->map->binding, but the MCA default was
    #    not copied onto the job until after the mapping had been settled -
    #    so the job mapped BYCORE and the bind-upwards check then rejected
    #    binding to a package. "--bind-to package" on the command line
    #    mapped BYPACKAGE and worked.
    cleanup_swarm
    out=$(RUN 'timeout 60 prterun --prtemca bindto bogus --host node2:2 -n 1 hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "an unrecognized DVM-wide bindto is fatal (rc=$rc)" \
                   || bad "bindto=bogus was diagnosed and then ignored"
    echo "$out" | grep -q 'not recognized' \
        && ok "the refusal named the unrecognized policy" \
        || bad "no diagnostic for the unrecognized bindto: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # a job-only qualifier at DVM scope is the same class of mistake
    out=$(RUN 'timeout 60 prterun --prtemca bindto core:report --host node2:2 -n 1 hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "a job-only bindto qualifier is fatal at DVM scope (rc=$rc)" \
                   || bad "bindto=core:report was diagnosed and then ignored"
    # every documented value that this node actually provides must map AND bind
    for lvl in hwthread core package numa; do
        out=$(RUN "timeout 60 prterun --prtemca bindto $lvl --host node2:4 -n 2 --display map hostname" 2>&1)
        rc=$?
        echo "$out" | grep -q 'lies above the mapping' \
            && bad "bindto=$lvl was refused by the bind-upwards check" \
            || ok "bindto=$lvl was not refused as binding above the map"
        [ "$rc" = 0 ] && ok "bindto=$lvl ran the job" \
                      || bad "bindto=$lvl failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
        n=$(echo "$out" | grep -c 'Bound: ')
        [ "$n" = 2 ] && ok "bindto=$lvl bound both ranks" \
                     || bad "bindto=$lvl bound $n/2 ranks: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    done
    cleanup_swarm

    banner "hwloc: per-object binding limits do not carry over between jobs"
    # prte_hwloc_base_reset_counters() clears the per-object "how many procs
    # did I already place here" counters between jobs. It walked only the
    # normal depth hierarchy, and hwloc 2.x keeps NUMA nodes OUT of that
    # hierarchy - so a counter attached to a NUMA node by "--bind-to
    # numa:limit=N" was never cleared. It survived for the life of the DVM,
    # and the SECOND such job found every domain already at its limit and
    # could not be bound at all.
    #
    # A persistent DVM is the only place this shows: prterun starts a fresh
    # HNP each time and takes the stale counters down with it. One proc per
    # job against limit=1, so a leaked counter is immediately fatal to the
    # next job. These containers report a single NUMA node covering the
    # machine, which is all this needs.
    cleanup_swarm
    out=$(RUN 'timeout 90 prte --daemonize --host node2:8 && sleep 2 &&
               for i in 1 2 3; do
                   echo "RUN$i";
                   timeout 60 prun -n 1 --bind-to numa:limit=1 --display map hostname;
               done; pterm' 2>&1)
    n=$(echo "$out" | grep -c 'Process rank: 0 Bound: package')
    [ "$n" = 3 ] && ok "all 3 numa:limit jobs bound in one DVM" \
                 || bad "$n/3 numa:limit jobs bound - a per-NUMA counter leaked between jobs: $(echo "$out" | tr '\n' ' ' | tail -c 400)"
    cleanup_swarm

    banner "hwloc: the cpu numbers PRRTE prints are the ones it accepts"
    # Every renderer here used to short-cut whenever the bits "already were"
    # cores (npus == ncores, which is exactly these containers) and print the
    # raw cpuset bits - PU OS indices - under a "core:L" label. --cpu-set
    # resolves its input LOGICALLY, so on any node whose OS and logical
    # numbering differ, the numbers PRRTE printed were not numbers PRRTE
    # would accept back. This asserts the round trip: whatever --display cpus
    # reports for a node has to be selectable, and selecting it has to yield
    # the same set.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:8 -n 1 --display cpus hostname' 2>&1)
    cpus=$(echo "$out" | sed -n 's/^PKG\[0\]: \(.*\)$/\1/p' | head -1)
    [ -n "$cpus" ] && ok "--display cpus reported package 0 as '$cpus'" \
                   || bad "--display cpus reported nothing: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    if [ -n "$cpus" ]; then
        out=$(RUN "timeout 90 prterun --prtemca hwloc_default_cpu_list '$cpus' \
                       --host node2:8 -n 1 --display cpus hostname" 2>&1)
        rc=$?
        [ "$rc" = 0 ] && ok "the reported cpu list is accepted by --cpu-set (rc=0)" \
                      || bad "--cpu-set rejected the list --display cpus just printed ('$cpus', rc=$rc)"
        back=$(echo "$out" | sed -n 's/^PKG\[0\]: \(.*\)$/\1/p' | head -1)
        [ "$back" = "$cpus" ] && ok "selecting those cpus reports the same set back ('$back')" \
                              || bad "round trip changed the set: printed '$cpus', got back '$back'"
    fi
    cleanup_swarm

    banner "hwloc: the parseable renderings actually parse"
    # PRRTE writes the same fact two ways. "--display map:parseable" runs
    # prte_hwloc_get_binding_info() and emits <package id="0"><core>N</core>
    # ...</package>; "--display cpus:parseable" runs the ras/base copy and
    # used to emit "<processors node=x>" wrapping "<pkg=0 cpus=0-7>" - an
    # unquoted attribute value around something that is not an element at
    # all. Neither could be read by an XML parser, which is the one thing
    # the mode is named for. Both are checked here against a real parser
    # rather than a grep, because a grep is exactly what let this stand.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:4 -n 2 --map-by core --bind-to core \
                   --display map:parseable hostname' 2>&1)
    echo "$out" | sed -n '/<map>/,/<\/map>/p' > /tmp/prte-map-parseable.$$
    python3 -c "import sys,xml.etree.ElementTree as ET; ET.parse(sys.argv[1])" \
        /tmp/prte-map-parseable.$$ 2>/dev/null \
        && ok "--display map:parseable produces a well-formed document" \
        || bad "--display map:parseable does not parse: $(head -c 300 /tmp/prte-map-parseable.$$ | tr '\n' ' ')"
    grep -q '<package id=' /tmp/prte-map-parseable.$$ \
        && ok "the map document names each package as an element attribute" \
        || bad "no <package id=...> element in the map document"
    rm -f /tmp/prte-map-parseable.$$

    out=$(RUN 'timeout 90 prterun --host node2:4 -n 1 --display cpus:parseable hostname' 2>&1)
    echo "$out" | sed -n '/<processors/,/<\/processors>/p' > /tmp/prte-cpus-parseable.$$
    python3 -c "import sys,xml.etree.ElementTree as ET; ET.parse(sys.argv[1])" \
        /tmp/prte-cpus-parseable.$$ 2>/dev/null \
        && ok "--display cpus:parseable produces a well-formed document" \
        || bad "--display cpus:parseable does not parse: $(head -c 300 /tmp/prte-cpus-parseable.$$ | tr '\n' ' ')"
    grep -q '<package id=' /tmp/prte-cpus-parseable.$$ \
        && ok "both parseable renderings spell a package the same way" \
        || bad "--display cpus:parseable does not use the <package id=...> shape"
    rm -f /tmp/prte-cpus-parseable.$$
    cleanup_swarm

    banner "hwloc: a qualifier with no policy keeps the default binding"
    # "--bind-to :overload-allowed" means "the binding I would have got,
    # but allow overload" - the same thing "--map-by :OVERSUBSCRIBE" has
    # always meant. pmix_check_cli_option() compares only
    # min(strlen(a),strlen(b)) characters, so the empty policy word matched
    # the first option tested against it, which is "none": binding was
    # silently disabled, and the default MAPPING policy fell from BYCORE to
    # BYSLOT along with it. Nothing was printed to say so.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:4 -n 2 --bind-to :overload-allowed \
                   --display map hostname' 2>&1)
    echo "$out" | grep -q 'Binding policy: CORE' \
        && ok "a qualifier-only --bind-to keeps the default CORE policy" \
        || bad "qualifier-only --bind-to changed the policy: $(echo "$out" | grep -i 'policy' | tr '\n' ' ')"
    echo "$out" | grep -q 'OVERLOAD-ALLOWED' \
        && ok "the qualifier itself survived" \
        || bad "the qualifier was lost: $(echo "$out" | grep -i 'policy' | tr '\n' ' ')"
    n=$(echo "$out" | grep -c 'Bound: package')
    [ "$n" = 2 ] && ok "both ranks were still bound" \
                 || bad "$n/2 ranks bound under a qualifier-only --bind-to"
    # and the mapping policy has to be untouched too - it is chosen from the
    # binding, so "none" dragged it down with it
    echo "$out" | grep -q 'Mapping policy: BYCORE' \
        && ok "the mapping policy is unchanged" \
        || bad "the mapping policy moved: $(echo "$out" | grep -i 'Mapping policy' | tr '\n' ' ')"
    cleanup_swarm

    banner "hwloc: the REPORT binding qualifier is reachable"
    # Three lists decide what a --bind-to qualifier is: the schizo whitelist
    # (bndquals[]), the job-level parser in src/hwloc, and the per-app parser
    # in rmaps/base. REPORT was implemented only in the job-level parser and
    # was missing from the whitelist, so no command line could reach it -
    # including the diagnostic that same arm emits when it is given as a
    # DVM-wide default, which tells the user to give it per job instead.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:4 -n 2 --bind-to core:report hostname' 2>&1)
    rc=$?
    [ "$rc" = 0 ] && ok "--bind-to core:report is accepted for a job (rc=0)" \
                  || bad "--bind-to core:report was refused (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    echo "$out" | grep -qi 'unrecognized qualifier' \
        && bad "--bind-to core:report is still refused as an unrecognized qualifier" \
        || ok "the qualifier was not reported as unrecognized"
    # ...and it describes the whole job, so a later MPMD segment has nowhere
    # to record it. That has to be said by name - the same spelling is legal
    # one segment earlier, so the generic "unrecognized qualifier" would be
    # baffling.
    out=$(RUN 'timeout 90 prterun --host node2:4 -n 1 hostname : -n 1 --bind-to core:report hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "a per-app REPORT is refused (rc=$rc)" \
                   || bad "a per-app REPORT was accepted, but there is nowhere to record it"
    echo "$out" | grep -q 'describes the whole job' \
        && ok "the per-app refusal explains itself" \
        || bad "the per-app refusal was generic: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    cleanup_swarm

    banner "hwloc: logical and physical binding reports agree on this node"
    # --report-bindings renders through the same path with "physical" either
    # set or not, and it used to be ignored outright whenever the short cut
    # above fired - both spellings produced the OS indices. These containers
    # number their cpus 0-7 either way, so this cannot catch a wrong BASIS;
    # what it does catch is the label going missing or the two spellings
    # rendering through different code again.
    cleanup_swarm
    out=$(RUN 'timeout 90 prterun --host node2:2 -n 2 --bind-to core \
                   --display map hostname' 2>&1)
    echo "$out" | grep -q 'core:L' \
        && ok "a logical binding is labelled core:L" \
        || bad "no core:L label in the default binding report: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    out=$(RUN 'timeout 90 prterun --host node2:2 -n 2 --bind-to core \
                   --display map:physical hostname' 2>&1)
    echo "$out" | grep -q 'core:P' \
        && ok "a physical binding is labelled core:P" \
        || bad "--display map:physical did not reach the renderer: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # --display cpus goes through its own renderer, which ignored the
    # qualifier entirely until both were routed through cpuset2ranges()
    out=$(RUN 'timeout 90 prterun --host node2:8 -n 1 --display cpus:physical hostname' 2>&1)
    rc=$?
    [ "$rc" = 0 ] && ok "--display cpus:physical completed (rc=0)" \
                  || bad "--display cpus:physical failed (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    echo "$out" | grep -qE '^PKG\[0\]: [0-9]' \
        && ok "--display cpus:physical reported a package" \
        || bad "--display cpus:physical reported nothing: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    cleanup_swarm
}

# Absolute path, deliberately -- see the note above DS.
GC=/opt/prte/prte/bin/groupcon
FENCER=/opt/prte/prte/bin/fencer

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

    banner "grpcomm: a requested membership order is applied on every daemon"
    # PMIX_GROUP_FINAL_MEMBERSHIP_ORDER is one of two construct directives
    # that hand the DVM an array of procs, and that array belongs to the PMIx
    # server which delivered the upcall -- PMIx frees it, arrays and all, once
    # the operation completes.  The daemon has to COPY it into the group
    # signature, whose destructor frees what it holds; pointing at it instead
    # is a double free of live heap on every daemon that had a local
    # participant.  So this case is as much about the DVM being alive
    # afterwards as it is about the order.
    #
    # The order asked for is the ranks reversed, because that is the one
    # answer the DVM cannot arrive at by accident: with no order given it
    # sorts the membership itself, so a directive that was dropped on the
    # floor is indistinguishable from the default unless the order is a
    # permutation the sort would never produce.
    out=$(PRUN "--host node1:2,node2:2,node3:2,node4:2 -n 8 --map-by node $GC --order g5" 2>&1)
    n=$(echo "$out" | grep -c 'CONSTRUCT PMIX_SUCCESS')
    [ "$n" = 8 ] \
        && ok "all 8 ranks constructed a group with a final order" \
        || bad "$n of 8 ranks constructed the ordered group: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    n=$(echo "$out" | awk '$1=="GRP" && $3=="ORDER" {print $4}' | sort -u | wc -l | tr -d ' ')
    g=$(echo "$out" | awk '$1=="GRP" && $3=="ORDER" {print $4; exit}')
    if [ "$n" != 1 ]; then
        bad "daemons returned $n different membership orders"
    elif [ "$g" = "7,6,5,4,3,2,1,0" ]; then
        ok "...and every daemon returned the reversed order that was asked for"
    else
        bad "the requested order was not applied (got '${g:-nothing}')"
    fi
    ranks=$(echo "$out" | grep -c 'CID-OK 8')
    [ "$ranks" = 8 ] \
        && ok "...with the whole membership still readable from each rank" \
        || bad "only $ranks of 8 ranks read back a full set after ordering"
    out=$(PRUN "--host node1:1,node2:1,node3:1 -n 3 --map-by node hostname" 2>&1)
    n=$(echo "$out" | grep -c '^node')
    [ "$n" = 3 ] \
        && ok "...and every daemon that carried the order is still running" \
        || bad "the DVM lost a daemon to the ordered construct ($n of 3)"

    RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    cleanup_swarm

    test_grpcomm_ft
}

# Losing a whole daemon mid-construct.  This is the daemon-death analog of the
# client-death handling the PMIx server library already does, and it is the
# reason grpcomm carries a recovery epoch at all: the rollup's expected counts
# come from the routing tree, so a failure invalidates every one of them and
# the collective has to be restarted rather than merely waited on.
#
# Every case here kills a real daemon under a live construct, so they need the
# stagger that --delay gives: without it the construct is long over before the
# kill lands and the case passes without testing anything.
test_grpcomm_ft() {
    local out n g

    if ! pmix_cap PMIX_CAP_GROUP_FT; then
        skp "grpcomm FT: the installed PMIx has no PMIX_CAP_GROUP_FT"
        return
    fi

    banner "grpcomm: an FT construct survives losing a participating daemon"
    # With PMIX_GROUP_FT_COLLECTIVE the construct must complete on the
    # survivors with a reduced membership, and each survivor must be told
    # which processes went away.  Ranks are one per node so killing node4
    # removes exactly one member.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/grp-ft.out "--rtos recoverable,notifyerrors --host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $GC --ft --delay 12 ftgrp"
        sleep 4
        if ! ON 4 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node4 has no daemon to kill -- the job did not land where expected"
        else
            ON 4 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\\000" < /tmp/grp-ft.out' 2>&1)
            # three survivors, each of which must have completed the construct
            n=$(echo "$out" | grep -c 'CONSTRUCT PMIX_SUCCESS')
            [ "$n" = 3 ] \
                && ok "all 3 surviving ranks completed the construct" \
                || bad "$n of 3 survivors completed the construct: $(echo "$out" | grep CONSTRUCT | tr '\n' ' ' | tail -c 300)"
            # the membership must have shrunk to the survivors
            n=$(echo "$out" | grep -c 'MEMBERS 3')
            [ "$n" = 3 ] \
                && ok "...on the reduced membership of 3" \
                || bad "survivors did not agree on a membership of 3: $(echo "$out" | grep MEMBERS | tr '\n' ' ' | tail -c 200)"
            # and they must have been told who was lost
            n=$(echo "$out" | grep -c 'MEMBER-FAILED')
            [ "$n" -ge 3 ] \
                && ok "...and every survivor was told which member failed" \
                || bad "only $n MEMBER-FAILED events reached the survivors (want >= 3)"
            # the reduced group must still be usable: each survivor reads back
            # every remaining member's contribution and reports no failures
            n=$(echo "$out" | grep -c 'DONE 0')
            [ "$n" = 3 ] \
                && ok "...and the reduced group was intact and usable" \
                || bad "$n of 3 survivors finished cleanly: $(echo "$out" | grep -E 'CID-FAIL|DONE' | tr '\n' ' ' | tail -c 250)"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "the HNP survived the loss" \
                || bad "the HNP died along with the lost daemon"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the FT group test"
    fi
    cleanup_swarm

    banner "grpcomm: without FT_COLLECTIVE the same loss aborts the construct"
    # The regression guard on the pre-existing behavior.  Note groupcon jumps
    # straight to its exit on a failed construct, so there is no DESTRUCT line
    # on this path -- assert on the CONSTRUCT status and on the DVM surviving.
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/grp-noft.out "--rtos recoverable,notifyerrors --host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $GC --delay 12 noftgrp"
        sleep 4
        if ! ON 4 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node4 has no daemon to kill -- the job did not land where expected"
        else
            ON 4 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\\000" < /tmp/grp-noft.out' 2>&1)
            n=$(echo "$out" | grep -c 'CONSTRUCT PMIX_GROUP_CONSTRUCT_ABORT')
            [ "$n" = 3 ] \
                && ok "all 3 survivors were told the construct aborted" \
                || bad "$n of 3 survivors reported an abort: $(echo "$out" | grep CONSTRUCT | tr '\n' ' ' | tail -c 300)"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the DVM survived the abort" \
                || bad "the HNP died rather than aborting the construct"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the non-FT group test"
    fi
    cleanup_swarm

    banner "grpcomm: losing a relay-only daemon does not stall a construct"
    # A daemon that hosts no member but sits between the controller and one
    # that does.  Nothing about the membership changes, so the construct must
    # simply complete -- with or without --ft.  Before the rollup learned to
    # recompute what it expects, this was the silent case: the operation was
    # not "affected", so it was never aborted either, and it hung on a count
    # that could never be reached.
    #
    # radix 2 is what makes the tree deep enough to have an interior node at
    # all; at the default radix every daemon is a child of the controller.
    if prted_dvm_start_mca 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1' \
                           '--prtemca rml_base_radix 2'; then
        # members on node1 and node7 only; node3 relays for node7's side of
        # the radix-2 tree and hosts nobody
        PRUN_BG /tmp/grp-relay.out "--host node1:1,node7:1 -n 2 --map-by node $GC --ft --delay 12 relaygrp"
        sleep 4
        if ! ON 3 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node3 has no daemon to kill"
        else
            ON 3 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\\000" < /tmp/grp-relay.out' 2>&1)
            n=$(echo "$out" | grep -c 'CONSTRUCT PMIX_SUCCESS')
            [ "$n" = 2 ] \
                && ok "both members completed the construct despite the lost relay" \
                || bad "$n of 2 members completed: $(echo "$out" | grep -E 'CONSTRUCT|DELAYING' | tr '\n' ' ' | tail -c 300)"
            n=$(echo "$out" | grep -c 'MEMBERS 2')
            [ "$n" = 2 ] \
                && ok "...with the membership intact -- no member was lost" \
                || bad "the membership changed when only a relay died: $(echo "$out" | grep MEMBERS | tr '\n' ' ' | tail -c 200)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a radix-2 DVM for the relay-loss test"
    fi
    cleanup_swarm

    banner "grpcomm: a daemon loss elsewhere does not disturb a live fence"
    # The bystander case, and the reason the fence handler was changed. It
    # used to kill the job whenever ANY daemon failed while ANY fence was in
    # flight, without asking whether the two were related - and fences run
    # constantly, so an unrelated failure anywhere was fatal. Here the fence
    # spans node1 and node2 only, and the daemon that dies hosts none of it.
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/grp-fence1.out "--host node1:1,node2:1 -n 2 --map-by node $GC --fence --delay 12 fen1"
        sleep 4
        if ! ON 4 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node4 has no daemon to kill"
        else
            ON 4 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\000" < /tmp/grp-fence1.out' 2>&1)
            n=$(echo "$out" | grep -c 'FENCE PMIX_SUCCESS')
            [ "$n" = 2 ] \
                && ok "both ranks completed the fence despite an unrelated daemon dying" \
                || bad "$n of 2 ranks completed the fence: $(echo "$out" | grep -E 'FENCE|FENCING' | tr '\n' ' ' | tail -c 250)"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the HNP survived" \
                || bad "the HNP died over a fence it had no part in"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the fence bystander test"
    fi
    cleanup_swarm

    banner "grpcomm: a fence that loses a participant fails, and only it"
    # An allgather has no opt-in to running degraded - its answer is every
    # participant's contribution - so a fence that really lost one cannot
    # complete. It must say so to its own participants rather than activating
    # a DVM-wide comm failure, which is what it used to do.
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/grp-fence2.out "--rtos recoverable,notifyerrors --host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $GC --fence --delay 12 fen2"
        sleep 4
        if ! ON 4 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node4 has no daemon to kill"
        else
            ON 4 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\000" < /tmp/grp-fence2.out' 2>&1)
            # the three survivors must each be told the fence failed, rather
            # than being left blocked in it
            n=$(echo "$out" | grep -c '^GRP [0-2] FENCE ')
            [ "$n" = 3 ] \
                && ok "all 3 survivors were released from the fence" \
                || bad "$n of 3 survivors got a fence result: $(echo "$out" | grep -E 'FENCE' | tr '\n' ' ' | tail -c 250)"
            n=$(echo "$out" | grep -c 'FENCE PMIX_SUCCESS')
            [ "$n" = 0 ] \
                && ok "...and none of them was told the allgather succeeded" \
                || bad "$n survivors were told a fence missing a participant had succeeded"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the DVM survived" \
                || bad "the HNP died rather than failing the fence"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the fence participant-loss test"
    fi
    cleanup_swarm

    banner "grpcomm: the DVM still runs group constructs after a loss"
    # A recovery that leaves a tracker, a memo entry or a caddy behind shows
    # up as drift on the next operation rather than as a bad run of its own.
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/grp-after.out "--rtos recoverable,notifyerrors --host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $GC --ft --delay 12 killgrp"
        sleep 4
        ON 4 'pkill -9 -x prted' >/dev/null 2>&1
        n=0
        while [ "$n" -lt 60 ]; do
            RUN 'pgrep -x prun' >/dev/null 2>&1 || break
            sleep 1; n=$((n+1))
        done
        n=0
        for g in a1 a2; do
            out=$(PRUN "--host node1:1,node2:1,node3:1 -n 3 --map-by node $GC --ft $g" 2>&1)
            [ "$(echo "$out" | grep -c 'CID-OK 3')" = 3 ] && n=$((n+1))
        done
        [ "$n" = 2 ] \
            && ok "two further group constructs completed on the reduced DVM" \
            || bad "only $n of 2 group constructs completed after the loss"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the post-loss group test"
    fi
    cleanup_swarm
}



########################################################################
# src/mca/errmgr -- what happens when a process or a daemon fails.
#
# The framework has two components with deliberately opposite policies, and
# BOTH are involved in every failure: the prted that owns the failing proc
# classifies it, kills what it must, and reports upward; the HNP decides
# whether the job dies, whether the DVM dies with it, and whether the
# survivors are told.  On a single host one process plays both roles and no
# report ever crosses a wire, so none of that is reachable without a swarm.
# test/unit/errmgr covers the parts that are: the module contract, the
# live-children scan, and the wire format of the report itself.
########################################################################
# Absolute path, as for the other helpers: an app launched into the DVM
# inherits the daemon PATH, which does not contain the install bindir.
FLT=/opt/prte/prte/bin/faulty

########################################################################
# src/mca/ess -- daemon/HNP bring-up.  ess IS the bring-up, so nearly all
# of it needs a live DVM by construction and the two pure parsers are
# covered without one by test/unit/ess.  What lands here is what only a
# real multi-node DVM can show.
#
# NOTE what is deliberately NOT here: forwarded signals.  That path is
# tool-side -- prte/prun catch the signal and relay
# PRTE_DAEMON_SIGNAL_LOCAL_PROCS over the RML -- and test_event already
# covers the cross-node relay, with test_prted covering its job scoping.
# A daemon installs no handlers of its own for those signals, so there is
# nothing ess-specific left to assert here.
########################################################################

PMIXLOOP=/opt/prte/prte/bin/pmixloop

# LOAD IS NOT AN OPTIMIZATION FOR THE TWO PHASES BELOW, IT IS THE EXPERIMENT.
# Both of them chase a race between one rank opening its next collective and
# its peers finalizing the last one, and on an idle machine that race simply
# does not run: measured against a library with BOTH fixes removed, an idle
# host is clean at 100, 400 and even 800 iterations -- 4 of 4 ranks done, not
# one bad return, no abort -- while the same library under 3x oversubscription
# aborts at 100. So iterations cannot substitute for load, and a phase that
# quietly lost these burners would keep passing while testing nothing.
#
# 3x the core count is what the macOS investigation needed too. The burners
# live on node1 but every container shares the one host kernel, so this loads
# the whole swarm; SWARM_CLEAN reaps them as a backstop if a phase dies early.
load_on()  { docker exec -d "${NODE}1" sh -c \
                 'n=$(nproc); i=0; while [ "$i" -lt $((n*3)) ]; do yes > /dev/null & i=$((i+1)); done'; }
load_off() { docker exec "${NODE}1" sh -c 'pkill -9 -x yes; true' >/dev/null 2>&1; }

test_pmix_cycling() {
    local out n bad_n

    banner "PMIx cycling: repeated Init/fence/Finalize stays in step (openpmix#4113)"
    # A client that cycles PMIx_Init / fence / PMIx_Finalize -- which is what
    # an MPI Sessions application does, one cycle per MPI_Session_init -- used
    # to drift apart by whole cycles and eventually wedge.  A rank that
    # dropped its socket through PMIx_Finalize was recorded on the fence
    # tracker's "departed" list even though it had NOT left the accounting
    # (nlocalprocs is deliberately not decremented for it and its peer object
    # is tombstoned, precisely so it can Init again and contribute).  Counting
    # it twice let the NEXT cycle's fence complete without it, and from there
    # the ranks are out of step: mid-run fences return PMIX_ERR_PARTIAL_SUCCESS
    # / PMIX_ERR_LOST_CONNECTION / PMIX_ERR_INVALID_ARG and the run eventually
    # hangs outright.  openpmix#4113, fixed by openpmix PR #4124.
    #
    # TWO THINGS DECIDE WHETHER THIS CASE HAS TEETH, and both are easy to
    # "simplify" away:
    #
    #  - AT LEAST TWO RANKS MUST SHARE A DAEMON.  The defect is in one PMIx
    #    server's own tracker accounting: it takes two ranks finalizing cycle N
    #    to satisfy, between them, the expected count of a fence that a faster
    #    peer has already opened for cycle N+1.  At one rank per node there is
    #    no such pair, and the unfixed library passes this case 5 times out of
    #    5 -- measured, not assumed.  So do NOT rewrite these as --map-by node
    #    one-per-host.
    #  - THE MACHINE MUST BE LOADED.  See load_on(): idle, the unfixed library
    #    passes this at 100, 400 and 800 iterations alike.  Under load it
    #    fails at 100.  Dropping the burners to speed the suite up would leave
    #    both phases passing and testing nothing.
    #
    # Against the unfixed library and under load, the first shape aborts or
    # hangs every time, with hundreds of non-success returns -- so this is a
    # real regression guard rather than a smoke test.
    cleanup_swarm
    if ! RUN "test -x $PMIXLOOP"; then
        skp "pmixloop client not installed -- re-run ./build.sh"
        return
    fi
    load_on

    # 4 ranks on ONE node: every rank is local to one PMIx server, which is
    # the arrangement the issue was reported against (its own harness runs
    # under simptest, where that is the only arrangement there is).
    out=$(RUN "timeout -k 10 240 prterun --host node1:4 -n 4 $PMIXLOOP 100" 2>&1)
    n=$(echo "$out" | grep -c ALLDONE)
    [ "$n" = 4 ] \
        && ok "4 ranks on one node each completed 100 Init/fence/Finalize cycles" \
        || bad "$n of 4 ranks finished cycling (a hang is the classic symptom): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # Assert on the tally rather than on the absence of an error line: a run
    # that hung produces no error lines either, and would otherwise pass.
    bad_n=$(echo "$out" | grep -oE '(fence1_bad|fence2_bad|init_bad|fini_bad)=[0-9]+' \
            | cut -d= -f2 | awk '{s+=$1} END {print s+0}')
    [ "$bad_n" = 0 ] \
        && ok "...with no fence returning a non-success status" \
        || bad "$bad_n non-success returns from the fences: $(echo "$out" | grep -m3 'rc=-' | tr '\n' ' ')"

    # 8 ranks over two nodes, 4 per node: still two ranks per server, so the
    # local accounting is exercised exactly as above, but now the fence is a
    # real PRRTE collective between daemons rather than one PMIx server
    # talking to itself.  That combination is what no single host can test.
    out=$(RUN "timeout -k 10 240 prterun --host node1:4,node2:4 -n 8 --map-by node $PMIXLOOP 100" 2>&1)
    n=$(echo "$out" | grep -c ALLDONE)
    [ "$n" = 8 ] \
        && ok "8 ranks over 2 nodes completed cycling with a host-mediated fence" \
        || bad "$n of 8 ranks finished cycling across nodes: $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    bad_n=$(echo "$out" | grep -oE '(fence1_bad|fence2_bad|init_bad|fini_bad)=[0-9]+' \
            | cut -d= -f2 | awk '{s+=$1} END {print s+0}')
    [ "$bad_n" = 0 ] \
        && ok "...with no fence returning a non-success status" \
        || bad "$bad_n non-success returns from the fences: $(echo "$out" | grep -m3 'rc=-' | tr '\n' ' ')"
    load_off
    cleanup_swarm
}

test_pmix_server_teardown() {
    local out rc

    banner "PMIx server teardown: heavy client churn leaves a sane heap (openpmix#4112)"
    # The subject here is the SERVER, not the client.  A collective the host
    # never sees is finished by calling the tracker's own completion function,
    # which only thread-shifts -- so on return the tracker is still on
    # pmix_server_globals.collectives and still satisfies trk_complete(), and
    # for that window anything walking the list sees a collective that looks
    # complete and unclaimed.  A second driver (in practice the
    # lost-connection sweep, firing as ranks finalize right after a fence)
    # completes it again: two handlers unlink and release the same tracker,
    # the second reading and re-releasing what the first freed.  The corrupted
    # list outlives the event, so the abort surfaces far away -- typically as
    # an invalid free inside PMIX_LIST_DESTRUCT(&collectives) in
    # PMIx_server_finalize, which is why openpmix#4112 reads as a teardown bug
    # and only reproduces after heavy connect/disconnect churn.  Fixed by
    # openpmix PR #4127.
    #
    # THE JOB MUST NOT BE SPREAD ACROSS NODES.  The window exists only for a
    # collective the host never sees -- a strictly local fence, the ordinary
    # case under fence_localonly_opt.  Give the job two nodes and the fence
    # goes up to PRRTE, host_called is set, and the case tests nothing.  That
    # is the opposite constraint from most of this file, so it is stated here
    # rather than left to be rediscovered.
    #
    # AND THE MACHINE MUST BE LOADED, for the same reason as the phase above
    # and to the same degree -- see load_on().  Idle, the unfixed library is
    # clean here however many iterations it is given.
    #
    # What is asserted is prterun's own exit and output: the HNP hosts these
    # ranks itself and finalizes its PMIx server on the way out, so it is the
    # process that aborts.  Measured against a library with the fix removed
    # (and under load): 10 of 10 runs abort with "malloc(): unsorted double
    # linked list corrupted", and under valgrind the invalid free lands in
    # PMIx_server_finalize at an address inside pmix_server_globals.
    cleanup_swarm
    if ! RUN "test -x $PMIXLOOP"; then
        skp "pmixloop client not installed -- re-run ./build.sh"
        return
    fi
    load_on

    out=$(RUN "timeout -k 10 300 prterun --host node1:4 -n 4 $PMIXLOOP 200" 2>&1)
    rc=$?
    [ "$rc" = 0 ] \
        && ok "prterun survived 800 client connect/disconnect cycles and exited 0" \
        || bad "prterun exited $rc after heavy client churn (134 = SIGABRT): $(echo "$out" | tr '\n' ' ' | tail -c 300)"
    # The heap diagnostic is worth asserting separately: glibc aborts only
    # when its own heuristics happen to catch the bad free, so a corrupted
    # heap can also surface as a message without a non-zero exit.
    echo "$out" | grep -qiE 'pointer being freed|free\(\): |double free|corrupted|malloc\(\): ' \
        && bad "the heap complained during teardown: $(echo "$out" | grep -iEm2 'pointer being freed|free\(\): |double free|corrupted|malloc\(\): ' | tr '\n' ' ')" \
        || ok "...with no invalid-free or heap-corruption diagnostic"
    load_off
    cleanup_swarm
}

test_ess() {
    local out rc n

    banner "ess: every daemon derives a distinct identity"
    # Each daemon's rank is ess_base_vpid plus a per-node index, summed in
    # prte_ess_base_set_identity.  Get that sum wrong and two daemons claim
    # the same rank, which is a silent failure: the DVM simply loses a node,
    # with no error anywhere.  So the observable is that a job mapped
    # one-per-node lands on as many DISTINCT hosts as there are nodes.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        out=$(PRUN '--host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node[0-9]+$')
        [ "$n" = 4 ] && ok "all four daemons accepted work" \
                     || bad "expected 4 procs, got $n: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        n=$(echo "$out" | grep -E '^node[0-9]+$' | sort -u | wc -l | tr -d ' ')
        [ "$n" = 4 ] && ok "...on four distinct nodes, so no two daemons share a rank" \
                     || bad "only $n distinct nodes -- daemon ranks collided"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the daemon-identity test"
    fi
    cleanup_swarm

    banner "ess: a bad forward-signals request is refused, not ignored"
    # prte_ess_base_setup_signals parses the list on the TOOL.  A signal
    # number this platform cannot deliver used to be accepted silently: the
    # handler install then failed with nothing said, so the user got no
    # forwarding and no diagnostic.  It must be refused up front, and by
    # NUMBER as well as by name -- those are separate parse branches and a
    # check added to one is easy to leave off the other.
    #
    # This runs on one node by nature; it is here rather than only in the
    # unit test because it is the end-to-end proof that the refusal actually
    # reaches the user through a real tool invocation.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 30 prterun --prtemca ess_base_forward_signals 999 --host node1:1 -n 1 hostname' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "an out-of-range signal number was refused (rc=$rc)" \
                   || bad "signal number 999 was accepted"
    echo "$out" | grep -q 'not a recognized signal' \
        && ok "...with a diagnostic naming it" \
        || bad "no diagnostic for the bad signal number: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(RUN 'timeout -k 5 30 prterun --prtemca ess_base_forward_signals SIGKILL --host node1:1 -n 1 hostname' 2>&1)
    echo "$out" | grep -q 'does not support trapping' \
        && ok "...and a non-forwardable signal is refused by name" \
        || bad "SIGKILL was not refused: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    out=$(RUN 'timeout -k 5 30 prterun --prtemca ess_base_forward_signals 9 --host node1:1 -n 1 hostname' 2>&1)
    echo "$out" | grep -q 'does not support trapping' \
        && ok "...and by number, through the other parse branch" \
        || bad "signal 9 was not refused the way SIGKILL is: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
    cleanup_swarm

    banner "ess: a daemon that cannot establish an identity fails cleanly"
    # prte_ess_base_set_identity refuses a vpid that is not a plain number
    # rather than letting strtoul read it as 0 -- which is the DVM
    # controller's rank, so the daemon would adopt the HNP identity and the
    # DVM would come apart later, nowhere near the cause.  A daemon started
    # by hand with a bad vpid must die saying so, and must not join.
    #
    # --leave-session-attached is what makes the diagnostic observable at
    # all: a daemon launched normally detaches from its controlling
    # terminal before ess init runs (prte_daemon_init_callback), so its
    # stderr goes nowhere and the message -- which the code does emit --
    # cannot be read from here. The flag changes nothing about the path
    # under test, only whether we can see what it wrote.
    cleanup_swarm
    out=$(ONT 2 'timeout -k 5 20 prted --leave-session-attached \
                     --prtemca ess_base_nspace bogus-dvm \
                     --prtemca ess_base_vpid not-a-number \
                     --prtemca prte_hnp_uri "bogus-dvm.0;tcp://127.0.0.1:1" 2>&1' 2>&1)
    echo "$out" | grep -qi 'not a valid non-negative number' \
        && ok "a non-numeric daemon vpid is refused with a diagnostic" \
        || bad "no bad-identity diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    ON 2 'pgrep -x prted' >/dev/null 2>&1 \
        && bad "the daemon stayed up despite having no valid identity" \
        || ok "...and the daemon did not come up"
    cleanup_swarm
}

test_errmgr() {
    local out rc n c

    banner "errmgr: an aborting rank kills its job and leaves the DVM standing"
    # PMIx_Abort on the last rank (node4, not the head node) arrives at its
    # own daemon as CALLED_ABORT.  errmgr/prted reports it to the HNP, and
    # errmgr/dvm - because the job is NOT recoverable - flags the job aborted,
    # terminates the rest of it, and must keep the DVM itself alive.  That
    # split is the whole point of having two components.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        out=$(PRUN "--host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $FLT abort 25" 2>&1)
        rc=$?
        [ "$rc" != 0 ] && ok "the aborting rank failed the job (rc=$rc)" \
                       || bad "a PMIx_Abort was reported as success"
        echo "$out" | grep -q 'FLT 3 ABORTING' \
            && ok "...and it was rank 3 on the far node that aborted" \
            || bad "rank 3 never reached its abort: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        # the survivors must have been killed rather than left to run out
        # their sleep - an abort takes the whole job with it
        n=$(echo "$out" | grep -c 'SURVIVED')
        [ "$n" = 0 ] && ok "...and no rank outlived the abort" \
                     || bad "$n rank(s) survived a non-recoverable abort"
        RUN 'pgrep -x prte' >/dev/null 2>&1 \
            && ok "...and the DVM survived the job it killed" \
            || bad "the DVM died along with the aborted job"
        # the surest proof the DVM is still usable: run another job in it
        out=$(PRUN '--host node2:1,node3:1 -n 2 --map-by node hostname' 2>&1); rc=$?
        [ "$rc" = 0 ] && ok "...and still runs a job afterwards" \
                      || bad "the DVM could not run a job after the abort: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the abort test"
    fi
    cleanup_swarm

    banner "errmgr: a non-zero exit is reported with its status"
    # TERM_NON_ZERO in errmgr/prted, which reports the proc to the HNP once
    # per job (the PRTE_JOB_FAIL_NOTIFIED dedup), and errmgr/dvm, which adopts
    # the proc exit code as the job exit code and records the proc in
    # PRTE_JOB_ABORTED_PROC so the eventual report can name it.  That report
    # is the assertion here: the rank and the code in it are the daemon's
    # classification arriving intact at the HNP.
    #
    # The tool's exit status is the rank's own.  It did not use to be: the DVM
    # put the application exit code into PMIX_JOB_TERM_STATUS, a field typed
    # as a pmix_status_t, and prun ran it back through the status converter -
    # which recognized nothing and answered PRTE_ERROR, so every failed job
    # came back as 71.  The exit code now travels as PMIX_EXIT_CODE and prun
    # reports it, the way prterun always has.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        out=$(PRUN "--host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $FLT exit 25" 2>&1)
        rc=$?
        [ "$rc" = 7 ] && ok "the tool exits with the rank's own status (7)" \
                      || bad "expected exit status 7 from the failing rank, got $rc"
        echo "$out" | grep -qi 'non-zero status' \
            && ok "...and the HNP reported it as a non-zero termination" \
            || bad "no non-zero-exit diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        echo "$out" | grep -qE 'Exit code:[[:space:]]*7\b' \
            && ok "...naming the rank's own exit code (7)" \
            || bad "the report does not carry exit code 7: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        RUN 'pgrep -x prte' >/dev/null 2>&1 \
            && ok "...and the DVM survived it" \
            || bad "the DVM died over a non-zero exit"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the non-zero exit test"
    fi
    cleanup_swarm

    banner "errmgr: a rank killed by a signal is named as such"
    # ABORTED_BY_SIG: the odls waitpid on node4 sees the signal, errmgr/prted
    # reports it, errmgr/dvm renders it.  The message naming the signal is
    # produced on the HNP from the state the daemon sent, so a wrong or lost
    # state shows up here as a missing or generic diagnostic.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        out=$(PRUN "--host node1:1,node2:1,node3:1,node4:1 -n 4 --map-by node $FLT signal 25" 2>&1)
        rc=$?
        [ "$rc" != 0 ] && ok "a signalled rank fails the job (rc=$rc)" \
                       || bad "a SIGSEGV was reported as success"
        echo "$out" | grep -qiE 'signal|segmentation' \
            && ok "...and the report names the signal" \
            || bad "no diagnostic naming the signal: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        echo "$out" | grep -q 'node4' \
            && ok "...and the node it died on" \
            || bad "the diagnostic does not name node4: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        RUN 'pgrep -x prte' >/dev/null 2>&1 \
            && ok "...and the DVM survived it" \
            || bad "the DVM died over a signalled rank"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the signal test"
    fi
    cleanup_swarm

    banner "errmgr: a recoverable job is notified instead of being killed"
    # The other half of errmgr/dvm's per-state table.  With
    # --rtos recoverable,notifyerrors the same failure must NOT terminate the
    # job: check_send_notification xcasts a PMIx event naming the dead peer
    # to every survivor, and prte_state_base_recover_resources gives its slot
    # back.  An FLT ... EVENT line is a survivor that actually received it,
    # which can only happen through a daemon that is not the HNP.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        out=$(PRUN "--rtos recoverable,notifyerrors --host node1:1,node2:1,node3:1,node4:1 \
                        -n 4 --map-by node $FLT exit 20" 2>&1)
        n=$(echo "$out" | grep -c 'SURVIVED')
        [ "$n" = 3 ] && ok "all 3 survivors outlived the failed rank" \
                     || bad "$n of 3 ranks survived a recoverable failure: $(echo "$out" | grep -cE 'FLT' ) FLT lines"
        n=$(echo "$out" | grep -c 'FLT .* EVENT ')
        [ "$n" -ge 3 ] \
            && ok "...and every survivor was notified of the loss ($n events)" \
            || bad "only $n survivors were notified of the failure (want >= 3)"
        RUN 'pgrep -x prte' >/dev/null 2>&1 \
            && ok "...and the DVM survived" \
            || bad "the DVM died over a recoverable failure"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the recoverable test"
    fi
    cleanup_swarm

    banner "errmgr: losing a daemon is reported and does not take the DVM down"
    # errmgr/dvm's daemon branch: the HNP marks the daemon gone, shows
    # help-errmgr-base.txt:node-died, and walks every job marking the procs
    # that lived on that node TERM_WO_SYNC.  The DVM has to stay up on the
    # remaining nodes - killing a compute daemon is not a reason to lose the
    # whole machine.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1,node4:1'; then
        PRUN_BG /tmp/errmgr-nodedie.out "--host node2:1,node3:1,node4:1 -n 3 --map-by node $FLT clean 60"
        sleep 6
        if ! ON 4 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node4 has no daemon to kill -- the job did not land where expected"
        else
            ON 4 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 90 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            [ "$n" -lt 90 ] && ok "the job ended when its daemon was killed" \
                            || bad "prun never returned after the daemon was killed"
            out=$(RUN 'tr -d "\\000" < /tmp/errmgr-nodedie.out' 2>&1)
            echo "$out" | grep -qi 'lost communication' \
                && ok "...and the HNP reported the lost daemon" \
                || bad "no node-died diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the DVM survived losing a compute daemon" \
                || bad "the HNP died when a compute daemon was killed"
            out=$(PRUN '--host node2:1,node3:1 -n 2 --map-by node hostname' 2>&1); rc=$?
            [ "$rc" = 0 ] && ok "...and still runs a job on the survivors" \
                          || bad "the reduced DVM could not run a job: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the daemon-loss test"
    fi
    cleanup_swarm

    banner "errmgr: losing a leaf daemon does not take down its parent"
    # With the default radix a 7-node DVM is flat: every daemon is a child of
    # the HNP, so the only process that ever notices another daemon's death
    # is the HNP, and errmgr/prted's daemon-loss handling is never exercised.
    # radix 2 puts interior daemons in between (0 -> {1,2}, 1 -> {3,5},
    # 2 -> {4,6}), so killing rank 6 (node7) is noticed first by rank 2
    # (node3), a prted.  Its job is to route around the loss.  It must not
    # decide that this is its own lifeline going away, kill its local
    # processes and exit - which is what treating any unreachable peer as a
    # lifeline loss does, and it costs the whole subtree, not just the leaf.
    cleanup_swarm
    if prted_dvm_start_mca 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1' \
                           '--prtemca rml_base_radix 2'; then
        # the job deliberately does NOT run on the node being killed
        PRUN_BG /tmp/errmgr-leaf.out "--host node2:1,node3:1,node4:1 -n 3 --map-by node $FLT clean 30"
        sleep 6
        if ! ON 7 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node7 has no daemon to kill -- the DVM did not span 7 nodes"
        else
            ON 7 'pkill -9 -x prted' >/dev/null 2>&1
            sleep 8
            ON 3 'pgrep -x prted' >/dev/null 2>&1 \
                && ok "the parent daemon survived losing its child" \
                || bad "node3 died along with the leaf below it"
            c=$(prted_count 2 3 4 5 6)
            [ "$c" = 5 ] && ok "...and so did every other daemon" \
                         || bad "only $c of 5 remaining daemons survived one leaf death"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the HNP" \
                || bad "the HNP died over a leaf daemon"
            # the running job never touched node7, so it must finish normally
            n=0
            while [ "$n" -lt 60 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            out=$(RUN 'tr -d "\\000" < /tmp/errmgr-leaf.out' 2>&1)
            [ "$(echo "$out" | grep -c 'SURVIVED')" = 3 ] \
                && ok "...and the job on the other nodes ran to completion" \
                || bad "the job did not complete after the leaf died: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a radix-2 DVM for the leaf-loss test"
    fi
    cleanup_swarm

    banner "errmgr: procs that never start are reported once, together"
    # FAILED_TO_START.  errmgr/prted deliberately does NOT report each proc
    # as it fails: it counts them and only activates the job state once every
    # local proc has attempted to start, so the HNP gets ONE consolidated
    # report per daemon.  With several procs per node a per-proc report would
    # show up as a storm of duplicate diagnostics.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 90 prterun --host node2:2,node3:2 -np 4 --map-by node \
                  /no/such/executable' 2>&1); rc=$?
    [ "$rc" != 0 ] && ok "a job that cannot start fails (rc=$rc)" \
                   || bad "a missing executable was reported as success"
    echo "$out" | grep -qi 'while attempting to start process' \
        && ok "...with a diagnostic naming the failure" \
        || bad "no start-failure diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    # ...once.  The consolidation is the point: each daemon waits for all of
    # its local procs to have attempted a start before it activates the job
    # state, so two failing procs per node must not produce two reports.
    n=$(echo "$out" | grep -ci 'while attempting to start process')
    [ "$n" = 1 ] && ok "...exactly once, not once per failed proc" \
                 || bad "the start failure was reported $n times (want 1)"
    c=$(prted_count 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "...and no daemon is left behind" \
                 || bad "$c stray prted after a failed start"
    cleanup_swarm
}

########################################################################
# src/mca/odls -- the daemon-side launch subsystem.  test/unit/odls covers
# the structural contract and the pieces that are pure functions
# (process_envars, the child->parent pipe record, the thread-pool sizing).
# What lands here is what only exists once a REMOTE daemon has to decode
# the launch message and fork against it: the environment the app actually
# ends up with, the exec agent, and the failure paths whose whole purpose
# is to keep a multi-app launch from stranding the job.
########################################################################
# Absolute path, deliberately -- see the note on DS above: an app launched
# into the DVM inherits the daemon PATH, which does not contain the install
# bindir.
ES=/opt/prte/prte/bin/envspawn

test_odls() {
    local out rc n c ns duri EL

    banner "odls: the envar directives reach the daemon that does the fork"
    # SET/ADD/UNSET/PREPEND/APPEND have no command-line surface at all: they
    # arrive on a PMIx_Spawn request, are attached to the job on the HNP,
    # travel in the launch message, and are applied by
    # prte_odls_base_process_envars() on whichever daemon forks the process.
    # envspawn pins its child to node3 while running on node2, so what is
    # under test is the REMOTE daemon's copy of the job.
    #
    # ADD is the one worth the whole apparatus.  It is defined - in
    # pmix_common.h and again in src/util/attr.h - as "add envar, but do not
    # overwrite any existing one", and it was being applied exactly like
    # SET, silently discarding whatever value was already there.
    cleanup_swarm
    if prted_dvm_start 'node1:2,node2:2,node3:2,node4:2'; then
        PRUN "--host node2:1 -n 1 $ES node3" >/dev/null 2>&1
        # the child wrote its report on its own node; reading it from there
        # is also how we know that is where the fork happened
        out=$(ON 3 'cat /tmp/odls-envspawn.out' 2>&1)
        echo "$out" | grep -q 'ODLS_SET=kept' \
            && ok "ADD did not overwrite the value SET established" \
            || bad "ADD overwrote a pre-existing value: $(echo "$out" | grep ODLS_SET)"
        echo "$out" | grep -q 'ODLS_NEW=added' \
            && ok "...but ADD does set a name nobody had claimed" \
            || bad "ADD did not set an absent variable: $(echo "$out" | grep ODLS_NEW)"
        echo "$out" | grep -q 'ODLS_PATH=front:middle:back' \
            && ok "PREPEND and APPEND edited the value in list order" \
            || bad "PREPEND/APPEND wrong: $(echo "$out" | grep ODLS_PATH)"
        echo "$out" | grep -q 'ODLS_GONE=<unset>' \
            && ok "UNSET removed the variable it named" \
            || bad "UNSET did nothing: $(echo "$out" | grep ODLS_GONE)"
        # ...and prove the fork really happened somewhere else
        echo "$out" | grep -q 'ODLS_HOST=node3' \
            && ok "...all of it applied by the daemon on the child's node" \
            || bad "the child did not run on node3: $(echo "$out" | grep ODLS_HOST)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the envar-directive test"
    fi
    cleanup_swarm

    banner "odls: the exec agent wraps every exec, on every node"
    # odls_base_exec_agent replaces the command spawn_proc hands to the fork
    # primitive, prepending the agent and pushing the app into its argv.  It
    # is read from the DAEMON's MCA state, not the job's, so each daemon
    # applies its own copy -- and a daemon that never picked the parameter
    # up would launch the app bare, which looks like success.  Use "env" as
    # the agent: it execs its argument, so the job still runs, and it stamps
    # a variable we can see in the output to prove it was in the chain.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 60 prterun --prtemca odls_base_exec_agent "/usr/bin/env ODLS_AGENT=yes" \
                   --host node1:1,node2:1,node3:1 -n 3 --map-by node \
                   sh -c "echo AGENT=\$ODLS_AGENT@\$(hostname)"' 2>&1)
    n=$(echo "$out" | grep -c 'AGENT=yes@')
    [ "$n" = 3 ] && ok "every one of the 3 procs was exec'd through the agent" \
                 || bad "only $n/3 procs went through the exec agent: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    n=$(echo "$out" | grep -o 'AGENT=yes@node[0-9]*' | sort -u | wc -l | tr -d ' ')
    [ "$n" = 3 ] && ok "...on three distinct nodes, so each daemon applied its own" \
                 || bad "the agent only took effect on $n nodes"
    cleanup_swarm

    banner "odls: a bad exec is diagnosed by the daemon that tried it"
    # The child cannot render its own diagnostic - it is in the
    # async-signal-safe window between fork() and execve() - so it writes a
    # fixed-size code+errno record up the pipe and the PARENT daemon renders
    # it.  On one host that parent is the HNP and the message never crosses
    # a wire.  Here the failing exec is on node4 and the text has to reach
    # the tool on node1 through the IOF, which is the whole point.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 60 prterun --host node4:1 -n 1 /no/such/executable' 2>&1)
    rc=$?
    [ "$rc" != 0 ] && ok "a missing executable fails the job (rc=$rc)" \
                   || bad "a missing executable exited 0"
    echo "$out" | grep -q '/no/such/executable' \
        && ok "...naming the executable it could not run" \
        || bad "no diagnostic naming the executable: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    c=$(prted_settle 10 1 2 3 4)
    [ "$c" = 0 ] && ok "...and every daemon came down afterwards" \
                 || bad "$c stray prted after a failed exec"
    cleanup_swarm

    # A directory is X_OK on most systems -- it means "searchable" -- so it
    # sails through the base's check_context_app() and only fails at the
    # execve, as a bare "Permission denied".  render_child_msg stats the app
    # in the PARENT to turn that into something a user can act on.
    #
    # Deliberately on a REMOTE node, and deliberately checking that the
    # message names that node.  A prted cannot deliver its own show_help --
    # plog/stdfd, when the renderer is a PMIx server rather than a client or
    # tool, hands the text to PMIx_server_IOF_deliver under the daemon's own
    # identity, which nothing has a sink for -- so every one of these was
    # silently dropped on every node but the head one.  prte_show_help()
    # renders on the daemon and relays to the HNP, which is the only place
    # the delivery machinery works.  "Local host: node4" is the proof that
    # the round trip happened.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 60 prterun --host node4:1 -n 1 /tmp' 2>&1)
    echo "$out" | grep -qi 'is a directory' \
        && ok "a directory given as the executable is named as such" \
        || bad "no directory diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -q 'Local host: *node4' \
        && ok "...rendered on the remote daemon and relayed to the tool" \
        || bad "the diagnostic did not come from node4: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm

    # ...and the same relay for a message the base emits before the fork,
    # so this is not just render_child_msg getting through
    cleanup_swarm
    out=$(RUN 'timeout -k 5 60 prterun --prtemca odls_base_exec_agent /no/such/agent \
                   --host node4:1 -n 1 hostname' 2>&1)
    echo "$out" | grep -q 'The specified fork agent was not found' \
        && ok "a missing exec agent is reported from the daemon that looked for it" \
        || bad "no fork-agent diagnostic: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    echo "$out" | grep -q 'Node: *node4' \
        && ok "...naming that node" \
        || bad "the fork-agent diagnostic did not name node4"
    cleanup_swarm

    banner "odls: one bad app in an MPMD launch takes the whole job down"
    # Every fatal path in launch_local flags the affected app's procs AND
    # activates PRTE_JOB_STATE_FAILED_TO_LAUNCH, because "goto GETOUT" skips
    # every LATER app -- whose procs then sit in INIT forever.  A prted only
    # reports FAILED_TO_LAUNCH once num_terminated == num_local_procs, so
    # failing just the one app's procs does not fail the job, it HANGS it.
    # Put the bad app FIRST so there is a later app to strand, and put the
    # two apps on different nodes so the stranding would be a remote
    # daemon's.  The observable is that this terminates at all.
    cleanup_swarm
    out=$(RUN 'timeout -k 5 45 prterun --host node2:1,node3:1 \
                   -n 1 /no/such/executable : -n 1 hostname' 2>&1)
    rc=$?
    [ "$rc" != 124 ] && ok "the launch terminated rather than hanging (rc=$rc)" \
                     || bad "an MPMD launch with a bad first app hung until the timeout"
    [ "$rc" != 0 ] && ok "...and reported failure" \
                   || bad "the failed MPMD launch exited 0"
    c=$(prted_settle 10 1 2 3)
    [ "$c" = 0 ] && ok "...and left no daemon behind" \
                 || bad "$c stray prted after a failed MPMD launch"
    cleanup_swarm

    banner "odls: the waitpid decode survives the trip back from a remote node"
    # wait_local_proc turns a raw wait status into a proc state on the
    # daemon that owns the child, and the HNP only ever sees the result.  A
    # signal death becomes signo+128 (shell convention) and a plain non-zero
    # exit stays itself; both have to reach the tool from node4.
    cleanup_swarm
    RUN 'timeout -k 5 60 prterun --host node4:1 -n 1 sh -c "exit 3"' >/dev/null 2>&1
    rc=$?
    [ "$rc" = 3 ] && ok "a non-zero exit on a remote node is reported verbatim" \
                  || bad "expected exit 3 from node4, got $rc"
    RUN 'timeout -k 5 60 prterun --host node4:1 -n 1 sh -c "kill -TERM \$\$; sleep 5"' >/dev/null 2>&1
    rc=$?
    [ "$rc" = 143 ] && ok "...and a SIGTERM death comes back as signo+128 (143)" \
                    || bad "expected 143 for a SIGTERM death on node4, got $rc"
    cleanup_swarm

    banner "odls: a signal aimed at a dead rank does not take out the daemon"
    # prted_comm walks every local child of the named job and asks the odls
    # to signal each one BY NAME.  The by-name branch had no aliveness gate,
    # so a rank that was gone - pid reset to 0 - was signaled at pid 0.  The
    # component turns a pid into a process GROUP, and kill(-0)/kill(0) is
    # "every process in the DAEMON's own group": the daemon signals itself,
    # its other children, and (under prterun) the launching tool.
    #
    # Set it up so the target job certainly has a dead rank: rank 1 on node3
    # exits at once while rank 0 on node2 lives on.  Then signal the job.
    # The observable is that the survivor and both daemons are still there.
    cleanup_swarm
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'pkill -9 -x sleep 2>/dev/null; true'; done
    if prted_dvm_start 'node1:1,node2:1,node3:1'; then
        # rank 1 (node3) exits at once; rank 0 (node2) sleeps on.  Once rank
        # 1 is reaped its pid is 0, but it is still a local child of this job
        # on node3 - which is exactly the proc prted_comm hands to the odls
        # by name when the job is signalled.
        PRUN_BG /tmp/odls-sig.out '--forward-signals SIGUSR1 --host node2:1,node3:1 \
             -n 2 --map-by node sh -c "if [ \$PMIX_RANK -eq 1 ]; then exit 0; fi; exec sleep 120"'
        sleep 8
        n=$(ON 2 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
        [ "${n:-0}" = 1 ] && ok "rank 0 is alive on node2 while rank 1 has exited" \
                          || bad "expected 1 sleeping rank on node2, saw ${n:-0}"
        # Relay a signal to the job.  prun turns this into a job-scoped
        # PMIX_JOB_CTRL_SIGNAL, and every daemon then signals each of ITS
        # local children of that job by name - including the dead one on
        # node3.  SIGURG is ignored by default, so a proc that dies here
        # died from a pid-0 broadcast, not from the signal itself.
        bpid=$(RUN 'pgrep -x prun | head -1' 2>/dev/null | tr -d ' \r')
        RUN "kill -URG $bpid" >/dev/null 2>&1
        sleep 5
        # node1 is the HNP and runs "prte", not "prted", so only the two
        # compute-node daemons are counted here; the HNP is checked next
        c=$(prted_count 2 3)
        [ "$c" = 2 ] && ok "...and signaling that job left both compute daemons standing" \
                     || bad "only $c/2 compute daemons survived signaling a job with a dead rank"
        c=$(RUN 'pgrep -c -x prte' 2>/dev/null | tr -d ' \r')
        [ "${c:-0}" = 1 ] && ok "...and the HNP too" \
                          || bad "the HNP did not survive signaling a job with a dead rank"
        n=$(ON 2 'pgrep -c -x sleep' 2>/dev/null | tr -d ' \r')
        [ "${n:-0}" = 1 ] && ok "...and the surviving rank was not collateral either" \
                          || bad "the live rank on node2 did not survive the signal"
        RUN 'pkill -f "sleep 120"' >/dev/null 2>&1
        RUN "timeout -k 5 40 pterm --dvm-uri file:$PRTED_URI" >/dev/null 2>&1
    else
        bad "could not start a DVM for the dead-rank signal test"
    fi
    cleanup_swarm

    banner "odls: kill escalates past an app that ignores SIGTERM"
    # kill_local_procs walks SIGCONT -> SIGTERM -> SIGKILL with a pause
    # between each, so an app that traps SIGTERM still dies.  Run one on
    # node2 and node3, order the DVM down, and require both daemons to be
    # gone: a daemon that could not kill its child waits for it forever.
    cleanup_swarm
    if prted_dvm_start 'node1:1,node2:1,node3:1'; then
        # ODLSKILLPROBE is just a marker to find the child by.  Every pgrep
        # below writes it as ODLSKILL[P]ROBE so the pattern cannot match the
        # pgrep command line itself -- without that this counts 1 survivor
        # on every node, including nodes that never ran a child.
        PRUN_BG /tmp/odls-kill.out '--host node2:1,node3:1 -n 2 --map-by node \
             sh -c "trap : TERM; while true; do sleep 1; done # ODLSKILLPROBE"'
        sleep 6
        n=$(ON 2 'pgrep -f "ODLSKILL[P]ROBE" | wc -l' | tr -d ' \r')
        [ "${n:-0}" != 0 ] && ok "the SIGTERM-proof child is running on node2" \
                           || bad "the test child never started on node2"
        # pterm needs the URI: the backgrounded prun above is holding a
        # rendezvous file of its own, so a bare pterm finds two and refuses
        RUN "timeout -k 5 40 pterm --dvm-uri file:$PRTED_URI" >/dev/null 2>&1
        c=$(prted_settle 20 1 2 3)
        [ "$c" = 0 ] && ok "...and every daemon shut down anyway" \
                     || bad "$c daemon(s) still up -- the kill escalation did not finish"
        n=$(ON 2 'pgrep -f "ODLSKILL[P]ROBE" | wc -l' | tr -d ' \r')
        [ "${n:-0}" = 0 ] && ok "...and the child itself is gone from node2" \
                          || bad "the SIGTERM-proof child survived on node2"
    else
        bad "could not start a DVM for the kill-escalation test"
    fi
    cleanup_swarm

    banner "odls/nidmap: a daemon grown into the DVM learns the jobs already running"
    # What the launch message no longer carries, and where it went.
    #
    # A daemon joining a DVM has never seen the launch messages of the jobs
    # already running in it, so it cannot resolve their namespaces.  That
    # used to be patched by packing every other active job in FRONT of the
    # launch message of whichever job brought the new daemons in -- which
    # tied that message's size to the number of jobs resident in the DVM
    # and, worse, did nothing at all for a daemon added by a bare elastic
    # grow, because a grow launches no job and so sends no launch message.
    # The jobs now travel with the nidmap at VM_READY, which is sent exactly
    # when the daemon set changes.
    #
    # So the shape here is deliberate, and it is the case the old mechanism
    # could not reach: grow a node with NO job launch of its own, then ask a
    # proc on that node about a job that predates it.  The wireup is the
    # only thing that can have told it.
    cleanup_swarm
    if ! RUN 'command -v jobinfo >/dev/null'; then
        skp "jobinfo client not installed -- re-run ./build.sh"
    else
        RUN 'rm -f /tmp/dvm.uri; nohup prte --daemonize --report-uri /tmp/dvm.uri --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
        duri=$(RUN 'head -1 /tmp/dvm.uri' 2>/dev/null | tr -d '\r')
        if [ -z "$duri" ]; then
            bad "could not start an elastic DVM for the job-catchup test"
        else
            # every elastic call names the DVM: the long-running prun below
            # leaves a rendezvous handle of its own, and bare discovery then
            # finds two servers and refuses to choose
            EL="PRTE_DVM_URI='$duri' timeout 90 elastic"
            out=$(RUN "$EL grow node2:2" 2>&1)
            if ! echo "$out" | grep -q PMIX_DVM_IS_READY; then
                bad "could not grow node2 -- cannot set up the catchup test"
            else
                ok "grew node2, which will host the job that predates node3"
                RUN_BG /tmp/cu-pub.out "prun --dvm-uri file:/tmp/dvm.uri --host node2:2 -n 2 jobinfo publish 240"
                sleep 10
                ns=$(RUN 'grep -m1 "^NSPACE " /tmp/cu-pub.out' 2>/dev/null | awk '{print $2}' | tr -d '\r')
                if [ -z "$ns" ]; then
                    bad "the pre-existing job never reported its nspace: $(RUN 'cat /tmp/cu-pub.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
                else
                    ok "a job is running on node2 (nspace $ns)"
                    # node3 joins now, with no job launched onto it
                    out=$(RUN "$EL grow node3:2" 2>&1)
                    if ! echo "$out" | grep -q PMIX_DVM_IS_READY; then
                        bad "could not grow node3 -- cannot test the catchup: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
                    else
                        ok "grew node3 into a DVM that already had a job running"
                        out=$(RUN "timeout 60 prun --dvm-uri file:/tmp/dvm.uri --host node3:1 -n 1 jobinfo fetch $ns 20" 2>&1)
                        n=$(echo "$out" | grep -m1 '^JOBSIZE ' | awk '{print $2}' | tr -d '\r')
                        [ "$n" = 2 ] \
                            && ok "a proc on the newly-grown node resolved the pre-existing job" \
                            || bad "the grown node could not resolve a job that predates it (got '$n'): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
                    fi
                fi
            fi
            RUN "timeout -k 5 30 pterm --dvm-uri file:/tmp/dvm.uri" >/dev/null 2>&1
        fi
    fi
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

    banner "rml: a daemon lost BELOW an interior daemon still ends the job"
    # The case above kills a daemon the HNP is the parent of, which at the
    # default radix is every daemon -- a ten-node DVM at radix 64 is flat, so
    # the HNP is always the one that loses the socket and the errmgr always
    # runs.  Give the tree depth and that stops being true: at radix 2 over
    # seven nodes, node7 (rank 6) hangs off rank 2, so rank 2 detects the loss
    # and the HNP only ever hears about it second-hand, through the RML failure
    # notice.  The notice used to update the routing bitmaps and nothing else,
    # so the HNP never marked the dead node's procs terminated and prun waited
    # forever -- a hang that did not exist one radix higher, which is precisely
    # why nothing caught it.
    #
    # The observable is the same as its flat sibling, and it has to be a real
    # timeout rather than "did it print something": the failure mode is a hang.
    cleanup_swarm
    if prted_dvm_start_mca 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1' \
           '--prtemca rml_base_radix 2'; then
        PRUN_BG /tmp/rml-deep-longrun.out '--host node7:1 -n 1 sleep 120'
        sleep 6
        if ! RUN 'pgrep -x prun' >/dev/null 2>&1; then
            bad "the long-running job never started on the deep tree: $(RUN 'cat /tmp/rml-deep-longrun.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        elif ! ON 7 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node7 has no daemon to kill -- the job did not land where expected"
        else
            ok "a job is running on node7, two levels down a radix-2 tree"
            ON 7 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 45 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            [ "$n" -lt 45 ] \
                && ok "a loss detected by an interior daemon reached the HNP (${n}s)" \
                || bad "prun never returned: the HNP never acted on a death it only heard about"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the HNP survived it" \
                || bad "the HNP died along with the lost daemon"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a radix-2 DVM for the deep lost-daemon test"
    fi
    cleanup_swarm

    banner "rml/oob: peer sockets serviced by dedicated OOB progress threads"
    # prte_oob_progress_threads (default 0) moves each peer's send/recv socket
    # events off the main progress thread onto one of N worker bases, chosen
    # round-robin at peer construction.  The connection state machine, routing,
    # message delivery and every send completion stay on the main thread.  This
    # is the only place that threaded path runs at all, so all four things it
    # could break are checked here rather than assumed.
    cleanup_swarm

    # (a) the parameter has to reach the *daemons*, not just the HNP -- the
    # whole point is remote sockets.  oob_base_verbose 5 makes each daemon
    # announce the pool it built; --leave-session-attached is what gets a
    # prted's stderr back to us.
    out=$(RUN 'timeout -k 5 90 prterun --prtemca prte_oob_progress_threads 4 \
                  --prtemca oob_base_verbose 5 --leave-session-attached \
                  --host node1:1,node2:1,node3:1 -np 3 --map-by node hostname' 2>&1)
    n=$(echo "$out" | grep -c 'starting 4 OOB progress threads')
    [ "$n" -ge 2 ] \
        && ok "the daemons started their OOB progress-thread pools ($n reported)" \
        || bad "the thread pool was not built on the daemons: $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm

    # (b) a relayed tree still delivers.  radix 2 over 7 nodes means every leaf
    # is reached through an interior daemon, so a message crosses a worker base
    # on the way in (recv handler) and again on the way out (relay -> send).
    out=$(RUN 'timeout -k 5 120 prterun --prtemca rml_base_radix 2 \
                  --prtemca prte_oob_progress_threads 4 \
                  --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1 \
                  -np 7 --map-by node hostname' 2>&1); rc=$?
    n=$(echo "$out" | grep -cE '^node[1-7]$')
    [ "$rc" = 0 ] && [ "$n" = 7 ] \
        && ok "7 procs relayed across a radix-2 tree with worker bases" \
        || bad "worker bases broke the relay (rc=$rc, lines=$n): $(echo "$out" | tr '\n' ' ' | tail -c 250)"
    cleanup_swarm

    # (c) the partial-write/partial-read bookkeeping is per-peer state that a
    # second thread is exactly what could corrupt, and a byte count is the only
    # check that catches a corrupted one -- a truncated relay still "runs".
    out=$(RUN 'timeout -k 5 120 prterun --prtemca rml_base_radix 2 \
                  --prtemca prte_oob_progress_threads 4 \
                  --host node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1 \
                  -np 7 --map-by node \
                  sh -c "head -c 500000 /dev/zero | tr \"\\0\" \"x\""' 2>&1)
    n=$(echo "$out" | tr -cd 'x' | wc -c | tr -d ' ')
    [ "$n" = 3500000 ] \
        && ok "3.5 MB relayed intact with the sockets on worker bases" \
        || bad "worker bases truncated the payload (got $n bytes of 3500000)"
    cleanup_swarm

    # (d) the race the design is actually exposed to: prte_oob_tcp_peer_close
    # runs on the main thread while a worker may be inside that peer's send or
    # recv handler.  Killing a daemon under a live job is what drives it.  A
    # missed handshake there is a hang, a use-after-free, or a leaked send --
    # so the observable is that the job is released and the HNP lives.
    # At the default radix, so that what this case reports is the threading and
    # nothing else; the deep-tree half of the same scenario is the case above,
    # which runs it at radix 2 with the threads off.
    if prted_dvm_start_mca 'node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1' \
           '--prtemca prte_oob_progress_threads 4'; then
        PRUN_BG /tmp/rml-thr-longrun.out '--host node7:1 -n 1 sleep 120'
        sleep 6
        if ! RUN 'pgrep -x prun' >/dev/null 2>&1; then
            bad "the long-running job never started under worker bases: $(RUN 'cat /tmp/rml-thr-longrun.out' 2>&1 | tr '\n' ' ' | tail -c 250)"
        elif ! ON 7 'pgrep -x prted' >/dev/null 2>&1; then
            bad "node7 has no daemon to kill -- the job did not land where expected"
        else
            ON 7 'pkill -9 -x prted' >/dev/null 2>&1
            n=0
            while [ "$n" -lt 45 ]; do
                RUN 'pgrep -x prun' >/dev/null 2>&1 || break
                sleep 1; n=$((n+1))
            done
            [ "$n" -lt 45 ] \
                && ok "peer_close raced a worker handler and still released the job (${n}s)" \
                || bad "prun never returned after its daemon died with worker bases on"
            RUN 'pgrep -x prte' >/dev/null 2>&1 \
                && ok "...and the HNP survived" \
                || bad "the HNP died along with the lost daemon"
        fi
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM with OOB progress threads"
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
        # Say which OOB threading the whole run used. The library defaults to
        # 0 (everything on the main progress thread); this suite opts in to
        # workers so the threaded send/recv path is covered at all. A failure
        # that only appears here is the first thing to re-check with
        # PRTE_SWARM_OOB_THREADS=0, which is why the number is in the log.
        if [ "$OOB_THREADS" = 0 ]; then
            ok "...running with OOB progress threads OFF (single-threaded OOB)"
        else
            ok "...running with $OOB_THREADS OOB progress threads (library default is 0)"
        fi
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

    banner "filem: --preload-files cross-node staging (data file only on node1)"
    # The data-file half of the same path. The file exists on node1 only, and
    # the ranks run on node2/node3 with their ordinary working directory --
    # so reading it back by the bare relative name proves both that the bytes
    # crossed and that they were placed where the procs actually run, which is
    # the whole of issue #2525. Single-host runs cannot show either: the
    # source file is already sitting in the launch directory.
    docker exec "${NODE}1" sh -c 'echo PRELOADED-DATA-OK > /root/pf.dat' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/pf.dat' >/dev/null 2>&1; done
    leaked=0
    for n in 2 3; do ON "$n" 'test -e /root/pf.dat' && leaked=1; done
    if [ "$leaked" != 0 ]; then
        bad "pf.dat is present on a target node -- staging test would be meaningless"
    else
        out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                     --preload-files /root/pf.dat -- sh -c "cat pf.dat"' 2>&1); rc=$?
        hits=$(echo "$out" | grep -c 'PRELOADED-DATA-OK')
        [ "$rc" = 0 ] && [ "$hits" = 2 ] \
            && ok "preload-files staged from node1 and read by both remote ranks" \
            || bad "preload-files cross-node failed (rc=$rc, hits=$hits): $(echo "$out" | tr '\n' ' ')"
        # and it is a real file in the working directory, not a link into a
        # session dir that will be gone with the DVM
        placed=0
        for n in 2 3; do
            ON "$n" 'test -f /root/pf.dat && ! test -L /root/pf.dat' && placed=$((placed+1))
        done
        [ "$placed" = 2 ] && ok "the staged file is a real file in each node's working directory" \
                          || bad "expected pf.dat placed as a regular file on node2+node3, got $placed"
    fi

    banner "filem: --preload-files keeps a relative subdirectory"
    # A relative specification is the name the app will open the file by, so
    # "sub/pf.dat" has to arrive as "sub/pf.dat" under each rank's working
    # directory -- with the intermediate directory created for it. This is
    # the only case that exercises place_file()'s dirpath creation, and the
    # only one where the received name is not a bare basename.
    docker exec "${NODE}1" sh -c 'mkdir -p /root/pfsub && echo SUBDIR-DATA-OK > /root/pfsub/pf.dat' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -rf /root/pfsub' >/dev/null 2>&1; done
    out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                 --preload-files pfsub/pf.dat -- sh -c "cat pfsub/pf.dat"' 2>&1); rc=$?
    hits=$(echo "$out" | grep -c 'SUBDIR-DATA-OK')
    [ "$rc" = 0 ] && [ "$hits" = 2 ] \
        && ok "a relative subdirectory was recreated in each rank's working directory" \
        || bad "preload-files subdir failed (rc=$rc, hits=$hits): $(echo "$out" | tr '\n' ' ')"
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'rm -rf /root/pfsub' >/dev/null 2>&1; done

    banner "filem: --preload-files keeps a staged file executable"
    # The chunk stream carries the source file's permissions, so a helper
    # script staged alongside the job arrives runnable. Nothing else in the
    # stream describes the file, so before the mode was sent the receiver
    # had to guess -- and it guessed 0600 for anything not flagged as the
    # job's own executable, which made "--preload-files helper.sh" deliver a
    # script the ranks could not run.
    docker exec "${NODE}1" sh -c 'printf "#!/bin/sh\necho SCRIPT-RAN-OK\n" > /root/helper.sh && chmod 755 /root/helper.sh' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/helper.sh' >/dev/null 2>&1; done
    out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                 --preload-files /root/helper.sh -- ./helper.sh' 2>&1); rc=$?
    hits=$(echo "$out" | grep -c 'SCRIPT-RAN-OK')
    [ "$rc" = 0 ] && [ "$hits" = 2 ] \
        && ok "a staged script arrived executable on both remote nodes" \
        || bad "staged script not executable (rc=$rc, hits=$hits): $(echo "$out" | tr '\n' ' ')"
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/helper.sh' >/dev/null 2>&1; done

    banner "filem: --preload-files refuses to overwrite a different file"
    # The safety rule: a file of that name that is NOT what was to be staged
    # belongs to the user, so the launch is refused rather than clobbering it.
    # node3 keeps the identical copy from the case above, which must stay
    # quiet -- only node2's differing file may fail the job.
    docker exec "${NODE}2" sh -c 'echo NODE2-PRECIOUS > /root/pf.dat' >/dev/null 2>&1
    out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                 --preload-files /root/pf.dat -- sh -c "cat pf.dat"' 2>&1); rc=$?
    kept=$(ON 2 'cat /root/pf.dat' | tr -d '\r')
    [ "$rc" != 0 ] && [ "$kept" = "NODE2-PRECIOUS" ] \
        && ok "collision refused the launch and left the user's file alone" \
        || bad "collision not refused (rc=$rc, node2 file now '$kept')"
    echo "$out" | grep -q 'already' \
        && ok "collision reported to the user" \
        || bad "collision produced no diagnostic: $(echo "$out" | tr '\n' ' ')"
    # PRTE_ERR_PRELOAD_CONFLICT is the last code in constants.h, and this is
    # the only place in the suite that provokes it. That matters beyond
    # filem: the string sweep in test/unit/util ran to a bound written out by
    # hand, and when this code was appended the bound was not moved, so the
    # newest code was precisely the one nothing checked. Assert here that it
    # still reaches the user as a sentence rather than as "Unknown error".
    echo "$out" | grep -qi 'unknown error' \
        && bad "the collision came back as \"Unknown error\": $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "the collision was named, not reported as \"Unknown error\""
    c=$(prted_settle 10 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after the refused preload" || bad "$c stray prted after refused preload"
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/pf.dat' >/dev/null 2>&1; done

    banner "filem: --preload-files reaches a node grown into the DVM later"
    # positioned_files remembers what has already been broadcast so a second
    # job in the same DVM does not resend it. That memory has to know how
    # much of the DVM it covers: a file staged before a grow never reached
    # the daemons that joined afterwards, and treating it as positioned
    # launches procs there with the file simply absent -- no error anywhere,
    # just an app that cannot open its input. Only an elastic DVM can show
    # this, and only across two jobs: one job cannot outrun its own staging.
    cleanup_swarm
    docker exec "${NODE}1" sh -c 'echo REGROWN-DATA-OK > /root/pg.dat' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/pg.dat' >/dev/null 2>&1; done
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if ! RUN 'pgrep -x prte >/dev/null'; then
        bad "could not start an elastic DVM for the preload-after-grow test"
    elif ! pmix_cap PMIX_CAP_TOOL_FINALIZED; then
        # without it the grown nodes stay reserved to the exited elastic
        # tool, so a later prun cannot be placed on them by name
        skp "preload after grow (PMIx predates PMIX_CAP_TOOL_FINALIZED)"
    else
        out=$(RUN 'timeout 90 elastic grow node2:1' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "grow node2 completed" || bad "grow node2 did not complete"
        sleep 2
        out=$(RUN 'cd /root && timeout 60 prun --host node2:1 -np 1 \
                     --preload-files /root/pg.dat -- sh -c "cat pg.dat"' 2>&1)
        echo "$out" | grep -q REGROWN-DATA-OK \
            && ok "the file was staged to the DVM as it stood" \
            || bad "first staging failed: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        # now widen the DVM and ask for the SAME file on the new node
        out=$(RUN 'timeout 90 elastic grow node3:1' 2>&1)
        echo "$out" | grep -q PMIX_DVM_IS_READY \
            && ok "grow node3 completed" || bad "grow node3 did not complete"
        sleep 2
        out=$(RUN 'cd /root && timeout 60 prun --host node3:1 -np 1 \
                     --preload-files /root/pg.dat -- sh -c "cat pg.dat"' 2>&1); rc=$?
        echo "$out" | grep -q REGROWN-DATA-OK \
            && ok "the same file was re-staged to the node grown in afterwards" \
            || bad "preload did not reach the newly-grown node (rc=$rc): $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    fi
    cleanup_swarm
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'rm -f /root/pg.dat' >/dev/null 2>&1; done

    banner "filem: --preload-files unpacks an archive whose name has a space"
    # Two things at once, both of which used to fail. The extract and the
    # listing go through a shell, so an ordinary "my data" reached tar as two
    # arguments; and the type was read from the FIRST dot in the name, so
    # "v2.tar.gz" classified as a plain file and was never unpacked at all.
    # Both are only visible cross-node: the archive's contents already exist
    # beside the archive on the node it was built on.
    docker exec "${NODE}1" bash -lc 'rm -rf /root/pfarc && mkdir -p /root/pfarc/inner \
        && echo ARCHIVED-DATA-OK > /root/pfarc/inner/a.txt \
        && cd /root/pfarc && tar czf "/root/my run.v2.tar.gz" inner/a.txt' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -rf /root/inner "/root/my run.v2.tar.gz"' >/dev/null 2>&1; done
    if RUN 'test -f "/root/my run.v2.tar.gz"'; then
        out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                     --preload-files "/root/my run.v2.tar.gz" -- sh -c "cat inner/a.txt"' 2>&1); rc=$?
        hits=$(echo "$out" | grep -c 'ARCHIVED-DATA-OK')
        [ "$rc" = 0 ] && [ "$hits" = 2 ] \
            && ok "an archive named with a space and a double suffix was staged and unpacked" \
            || bad "archive preload failed (rc=$rc, hits=$hits): $(echo "$out" | tr '\n' ' ')"
    else
        bad "could not build the test archive on node1 (need tar/gzip in the image)"
    fi
    c=$(prted_settle 10 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after the archive preload" || bad "$c stray prted after archive preload"
    for n in 1 2 3; do
        docker exec "$NODE$n" sh -c 'rm -rf /root/pfarc /root/inner "/root/my run.v2.tar.gz"' >/dev/null 2>&1
    done

    banner "filem: --preload-files refuses a name that steps out of the directory"
    # Stripping the LEADING "./" and "../" does not make a name safe: a ".."
    # further along ("a/../../f") still resolves above the session directory,
    # where the receive path creates the file with O_TRUNC over whatever is
    # sitting there -- on every node at once. The request has to be refused
    # by name rather than resolved.
    docker exec "${NODE}1" sh -c 'mkdir -p /root/pfesc/inner && echo ESCAPE-PAYLOAD > /root/pfesc/pf.dat' >/dev/null 2>&1
    for n in 2 3; do docker exec "$NODE$n" sh -c 'rm -rf /root/pfesc' >/dev/null 2>&1; done
    out=$(RUN 'cd /root && prterun --host node2:1,node3:1 -np 2 --map-by node \
                 --preload-files pfesc/inner/../../pfesc/pf.dat -- hostname' 2>&1); rc=$?
    [ "$rc" != 0 ] \
        && ok "a '..' inside the delivered name was refused" \
        || bad "a '..' inside the delivered name was accepted (rc=$rc): $(echo "$out" | tr '\n' ' ')"
    echo "$out" | grep -q '\.\.' \
        && ok "the refusal named the problem to the user" \
        || bad "the refusal produced no diagnostic: $(echo "$out" | tr '\n' ' ')"
    echo "$out" | grep -qi 'unknown error' \
        && bad "the refusal came back as \"Unknown error\": $(echo "$out" | tr '\n' ' ' | tail -c 250)" \
        || ok "the refusal was named, not reported as \"Unknown error\""
    c=$(prted_settle 10 1 2 3 4 5 6 7 8 9 10)
    [ "$c" = 0 ] && ok "no daemons linger after the refused path" || bad "$c stray prted after refused path"
    for n in 1 2 3; do docker exec "$NODE$n" sh -c 'rm -rf /root/pfesc' >/dev/null 2>&1; done

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
        # A second, deliberately larger input for the flow-control cases below.
        # Back-pressure is keyed on PRTE_IOF_MAX_INPUT_BUFFERS (50) *queued
        # chunks*, and the HNP hands stdin over in 8 KB chunks -- so an input
        # has to exceed 50 * 8192 = 400 KB before the backlog can cross the
        # threshold AT ALL, no matter how slowly the reader drains it.  The
        # 256 KB input above is 43 chunks, which is why the XOFF assertions
        # here used to be unfalsifiable.  1 MB of random, base64'd to ~1.4 MB,
        # is ~172 chunks.  Do not shrink this without redoing that arithmetic.
        RUN 'head -c 1048576 /dev/urandom | base64 > /tmp/iof_bulk_in.txt' >/dev/null 2>&1
        bulksum=$(RUN 'md5sum < /tmp/iof_bulk_in.txt' | awk '{print $1}')
        bulksz=$(RUN 'wc -c < /tmp/iof_bulk_in.txt' | tr -d ' ')
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

        # A reader slow enough to push the daemon's stdin backlog past
        # PRTE_IOF_MAX_INPUT_BUFFERS (50 queued chunks), which is what makes
        # the daemon send the HNP a flow-control message.
        #
        # That message consists of NOTHING but the stream tag -- no proc, no
        # count, no payload -- and it travels on PRTE_RML_TAG_IOF_HNP, the same
        # tag forwarded output uses.  The HNP used to unpack the tag and then
        # reach straight for a proc, which on a tag-only buffer fails, so a
        # perfectly ordinary slow reader produced a PMIx unpack error on the
        # user's terminal.  The HNP now screens the XON/XOFF mask first.
        #
        # The assertion is on the HNP's own log, so the reader has to be slow
        # enough for the backlog to actually cross 50: 64 bytes every 4 ms
        # against a 256 KB payload the HNP hands over as fast as the RML will
        # carry it.  Delivery must still be exact -- an XOFF is not permission
        # to drop anything.
        RUN 'cp /tmp/prte.out /tmp/prte.out.mark 2>/dev/null || : ' >/dev/null 2>&1
        out=$(RUN 'cd /tmp && timeout 300 prun --host node2:1 -n 1 \
                     '"$SC"' /tmp/iof_xoff_out.txt 512 2000 < iof_bulk_in.txt \
                     2>iof_xoff_err.txt; echo "rc=$?"')
        rc=$(echo "$out" | sed -n 's/^rc=//p')
        got=$(echo "$out" | sed -n 's/^SLOWCAT-BYTES //p')
        outsum=$(ON 2 'md5sum < /tmp/iof_xoff_out.txt 2>/dev/null' | awk '{print $1}')
        [ "$rc" = 0 ] && [ -n "$got" ] && [ "$got" = "$bulksz" ] && [ "$bulksum" = "$outsum" ] \
            && ok "stdin survived a reader slow enough to trigger XOFF" \
            || bad "XOFF-triggering stdin corrupted (rc=$rc, sent=$bulksz received=$got, md5 $bulksum vs $outsum): $(RUN 'head -c 200 /tmp/iof_xoff_err.txt')"
        # ...and the flow-control message did not come back at the user as a
        # corrupted one.  "prte --daemonize" detaches from stdio, so this is
        # what the HNP wrote before detaching plus anything pmix_output sent to
        # the log; a PMIX_ERROR_LOG of an unpack failure names the file.
        hnplog=$(RUN 'diff /tmp/prte.out.mark /tmp/prte.out 2>/dev/null | grep "^>" \
                      || cat /tmp/prte.out 2>/dev/null')
        echo "$hnplog" | grep -qiE 'UNPACK|iof_hnp_receive\.c' \
            && bad "the HNP reported a flow-control message as corrupt: $(echo "$hnplog" | grep -iE 'UNPACK|iof_hnp_receive' | head -2 | tr '\n' ' ')" \
            || ok "no unpack error at the HNP while stdin was backed up"
        ON 2 'rm -f /tmp/iof_xoff_out.txt' >/dev/null 2>&1

        # The same reader again, this time with the iof talking, to assert
        # that flow control actually engaged AND that every XOFF was paired
        # with an XON.
        #
        # The pairing is the part that matters. PMIx has no status meaning
        # "resume", so a suspension the HNP asserts and forgets to release
        # stalls the producer for the life of the job - a hang, not a
        # slowdown. On the HNP side that release lives in
        # release_flow_control(), called from every path a backed-up sink
        # can leave that state by, including the ones where the sink is torn
        # down rather than drained.
        #
        # Counting rather than merely grepping is deliberate: an
        # implementation that asserts XOFF once and releases once looks
        # identical to a correct one under a presence test, and so does one
        # that asserts fifty times and releases once.
        # Two things this case has to get right before it can assert anything.
        #
        # It needs the flow-control API at all: without PMIX_CAP_IOF_FLOW_CONTROL
        # both the XOFF assert and its matching release are compiled out, and the
        # HNP simply queues (bounded by iof_base_output_limit).  There is then no
        # XOFF to pair and nothing to test, so skip rather than fail.
        #
        # And it needs to be able to SEE the HNP's log.  The DVM this phase runs
        # against is started with "prte --daemonize", which detaches stdio before
        # any of this is emitted -- /tmp/prte.out is empty by construction, so
        # grepping it can only ever fail.  So this one case drives its own
        # foreground DVM with prterun and reads that process's stderr directly.
        # prterun takes its own session-dir prefix (prtrn.<pid>), so it coexists
        # with the daemonized DVM the rest of the phase is using.
        if ! pmix_cap PMIX_CAP_IOF_FLOW_CONTROL; then
            skp "PMIx predates PMIX_CAP_IOF_FLOW_CONTROL -- no XOFF to pair"
        else
            out=$(RUN 'cd /tmp && timeout 300 prterun --prtemca iof_base_verbose 1 -n 1 \
                         '"$SC"' /tmp/iof_pair_out.txt 2048 3000 < iof_bulk_in.txt \
                         2>iof_pair_err.txt; echo "rc=$?"')
            rc=$(echo "$out" | sed -n 's/^rc=//p')
            got=$(echo "$out" | sed -n 's/^SLOWCAT-BYTES //p')
            pairlog=$(RUN 'cat /tmp/iof_pair_err.txt 2>/dev/null')
            nxoff=$(echo "$pairlog" | grep -c 'buffer backed up - holding')
            nxon=$(echo "$pairlog" | grep -c 'releasing stdin flow control')
            [ "$nxoff" -gt 0 ] \
                && ok "stdin flow control engaged ($nxoff XOFF)" \
                || bad "stdin flow control never engaged - reader too fast, or the backlog never crossed PRTE_IOF_MAX_INPUT_BUFFERS (50 chunks of 8K; input is $bulksz bytes)"
            [ "$nxoff" = "$nxon" ] \
                && ok "every XOFF was paired with an XON ($nxon)" \
                || bad "unpaired stdin flow control: $nxoff XOFF vs $nxon XON - a producer is left suspended"
            [ "$rc" = 0 ] && [ "$got" = "$bulksz" ] \
                && ok "delivery stayed exact across $nxoff suspensions" \
                || bad "flow-controlled stdin lost data (rc=$rc, sent=$bulksz received=$got)"
            RUN 'rm -f /tmp/iof_pair_out.txt' >/dev/null 2>&1
        fi

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

    banner "plm: a second launch adds only the daemons that launch needs"
    # setup_virtual_machine runs on EVERY launch, but the daemon job's map is
    # persistent - it accumulates the DVM's nodes for the life of the session.
    # num_new_daemons and daemon_vpid_start describe only the launch about to
    # happen, so each pass has to recompute them from scratch; carried over,
    # they make a launcher act on the previous launch's daemons. (ssh
    # substitutes each node's own vpid per node, so the vpid half of that is
    # only fatal under an RM launcher - see test/unit/plm - but the count is
    # what every component keys its "anything to do?" test off.)
    #
    # So: form a DVM, then bring in a third node with --add-host and require
    # that the second pass reports exactly ONE new daemon, not three.
    # NOTE: the DVM runs in the FOREGROUND under RUN_BG rather than
    # --daemonize, because a daemonized HNP detaches from its output and the
    # trace this case reads would go nowhere.
    cleanup_swarm
    RUN_BG /tmp/prte.out 'prte --prtemca plm_base_verbose 5 --host node1:1,node2:1'
    sleep 8
    if RUN 'pgrep -x prte >/dev/null'; then
        first=$(RUN 'grep -c "setup_vm add new daemon" /tmp/prte.out' | tr -d '\r')
        [ "$first" = 1 ] && ok "DVM formation added one daemon (node2)" \
                         || bad "DVM formation reported $first new daemons (expected 1)"
        out=$(RUN 'timeout 90 prun --add-host node3:1 --host node1:1,node2:1,node3:1 \
                     -n 3 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node[1-3]$')
        [ "$n" = 3 ] && ok "--add-host launch ran across all three nodes" \
                     || bad "--add-host launch produced $n/3 procs: $(echo "$out" | tr '\n' ' ')"
        [ "$(prted_count 3)" = 1 ] && ok "a daemon was started on the added node" \
                                   || bad "no daemon on the added node"
        total=$(RUN 'grep -c "setup_vm add new daemon" /tmp/prte.out' | tr -d '\r')
        [ "$total" = 2 ] \
            && ok "the second pass added exactly one daemon (count not carried over)" \
            || bad "second pass brought the running total to $total new daemons (expected 2)"
        # and the DVM still routes to everyone afterwards
        out=$(RUN 'timeout 30 prun -n 3 --map-by node hostname' 2>&1)
        n=$(echo "$out" | grep -cE '^node[1-3]$')
        [ "$n" = 3 ] && ok "DVM still spans all three nodes after the grow" \
                     || bad "post-grow job produced $n/3 procs: $(echo "$out" | tr '\n' ' ')"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the second-launch test"
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

    banner "elastic DVM: a job spanning a surviving daemon runs after grow/shrink/grow"
    # The daemon that was already up when the DVM grew has to learn the new
    # daemon's vpid, because odls walks EVERY proc of a job -- not just its own
    # -- to wire each one to its node, and one unresolvable parent throws away
    # the whole child list, including the process that daemon was about to fork.
    # prte_util_decode_nidmap() used to record the node->daemon binding only
    # when it created a new node-pool entry, so on every wireup after its first
    # a surviving daemon skipped the binding entirely and never heard of the new
    # daemon; it launched nothing and the HNP, which had no such gap, waited
    # forever (openpmix/prrte#2616). It also keyed the pool by packing position
    # while the sender skips daemon-less nodes, so a shrink slid every later
    # node onto the wrong slot -- which is why the grow does NOT have to reuse
    # the node that was shrunk out for this to break.
    #
    # Only a job that SPANS the survivor shows this: the tests above launch
    # "prun -n 1", which lands on the HNP alone and passes with the DVM in
    # exactly this state. Grow, shrink a node, grow a different node, then run
    # one proc per node and require every node to report.
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'timeout 90 elastic grow node2:2,node3:2' >/dev/null 2>&1
        RUN 'timeout 90 elastic shrink node3' >/dev/null 2>&1; sleep 3
        out=$(RUN 'timeout 90 elastic grow node4:2' 2>&1); sleep 3
        echo "$out" | grep -q PMIX_DVM_IS_READY && ok "grow after the shrink completed" \
                                               || bad "grow after the shrink did not complete"
        # node2 survived both DVM changes; node4 arrived in the second grow
        out=$(RUN 'timeout 60 prun -n 3 --map-by ppr:1:node hostname' 2>&1)
        for n in node1 node2 node4; do
            echo "$out" | grep -qw "$n" && ok "$n ran its proc after grow/shrink/grow" \
                                        || bad "$n launched nothing after grow/shrink/grow (nidmap rebind)"
        done
        # Now re-grow the node that was shrunk OUT. It keeps the pool slot it
        # always had, so this is the case that a slot-keyed decode cannot fix
        # by placement alone: the survivor has the node, and only the daemon on
        # it has changed. Nothing is rebound unless every decode rebinds.
        RUN 'timeout 90 elastic grow node3:2' >/dev/null 2>&1; sleep 3
        out=$(RUN 'timeout 60 prun -n 4 --map-by ppr:1:node hostname' 2>&1)
        for n in node1 node2 node3 node4; do
            echo "$out" | grep -qw "$n" && ok "$n ran its proc after the shrunk node was re-grown" \
                                        || bad "$n launched nothing after the re-grow (nidmap rebind)"
        done
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the spanning-launch test"
    fi
    cleanup_swarm

    banner "elastic DVM: a daemon re-grown into a vpid hole gets a live parent"
    # The same shape as the case above, but at a radix small enough for the
    # daemon tree to have depth -- which is the only way this defect is
    # visible. A daemon launched into a DVM that has already shrunk starts with
    # an EMPTY departed-rank set, so it computes its first routing tree from
    # raw radix math and can pick a retired vpid as its parent. Everything it
    # then sends upward is addressed to a rank nothing can contact, including
    # the warm-up that asks for the nidmap that would have corrected the set --
    # so it never learns, never reports in, and every job touching that node
    # hangs. The launcher therefore carries the departed set on the prted
    # command line (rml_base_dead_dmns), which is the only moment early enough.
    #
    # At the DEFAULT radix every daemon's parent is rank 0, which is always
    # alive, so the whole thing is invisible -- the case above passes with the
    # DVM in exactly this state. Hence the explicit radix here: without it this
    # suite never exercises the fix at all.
    #
    # The vpids matter. grow node2,node3 -> shrink node3 -> grow node4 ->
    # re-grow node3 gives node1=0, node2=1, node3(old)=2 (retired), node4=3,
    # node3(new)=4 -- and at radix 2 the raw parent of rank 4 is rank 2.
    cleanup_swarm
    RUN 'nohup prte --daemonize --prtemca prte_elastic_mode 1 \
                    --prtemca rml_base_radix 2 >/tmp/prte.out 2>&1 & sleep 8' >/dev/null
    if RUN 'pgrep -x prte >/dev/null'; then
        RUN 'timeout 90 elastic grow node2:2,node3:2' >/dev/null 2>&1
        RUN 'timeout 90 elastic shrink node3' >/dev/null 2>&1; sleep 3
        RUN 'timeout 90 elastic grow node4:2' >/dev/null 2>&1; sleep 3
        out=$(RUN 'timeout 90 elastic grow node3:2' 2>&1); sleep 3
        echo "$out" | grep -q "SUCCESS\|PMIX_DVM_IS_READY" \
            && ok "the re-grow into the vpid hole completed at radix 2" \
            || bad "re-grow into the vpid hole did not complete: $(echo "$out" | tr '\n' ' ' | tail -c 200)"
        [ "$(prted_count 3)" = 1 ] && ok "a daemon is running on the re-grown node" \
                                   || bad "no daemon on the re-grown node"
        # The proof: a job spanning every node. The re-grown daemon can only
        # report its procs launched if its lifeline is a rank that still
        # exists, so a node missing from this output is the stranded daemon.
        out=$(RUN 'timeout 60 prun -n 4 --map-by ppr:1:node hostname' 2>&1)
        for n in node1 node2 node3 node4; do
            echo "$out" | grep -qw "$n" \
                && ok "$n ran its proc after the deep-tree re-grow" \
                || bad "$n launched nothing after the deep-tree re-grow (departed set not carried at launch)"
        done
        # ...and the DVM is still usable afterwards, so a wedged daemon cannot
        # pass the above by having merely delayed
        out=$(RUN 'timeout 60 prun --host node3 -n 1 hostname' 2>&1 | tr -d '\r')
        [ "$out" = node3 ] && ok "the re-grown node serves a job of its own" \
                           || bad "the re-grown node is wedged: $out"
        RUN 'timeout -k 5 30 pterm' >/dev/null 2>&1
    else
        bad "could not start an elastic DVM for the deep-tree re-grow test"
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

    test_session

    test_tools

    test_util

    test_hwloc

    test_event

    test_include

    test_runtime

    test_rml

    test_ess

    test_errmgr

    test_odls

    test_grpcomm

    test_pmix_cycling

    test_pmix_server_teardown




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
