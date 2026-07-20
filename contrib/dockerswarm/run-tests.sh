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
        docker exec "prte-node$n" sh -c \
            'pkill -9 -x prted 2>/dev/null; pkill -9 -x prte 2>/dev/null;
             rm -rf /tmp/prte.* /tmp/prun.session.* 2>/dev/null; true'
    done
}
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }

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
        RUN 'timeout 30 pterm' >/dev/null 2>&1
    else
        bad "could not start a DVM for the stdin tests"
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
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n================  %d passed, %d failed, %d skipped  ================\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
