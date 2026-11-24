#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# PRRTE under a real PBS (OpenPBS).  See AGENTS.md.
#
#   ./run-tests.sh [phase ...]     phases: cluster ras plm accounting qdel
#
# Everything here runs as the job user, prte - PBS runs no job as root, and
# that is how PRRTE meets PBS on a cluster.  Almost nothing belongs here
# that is not about the scheduler: see AGENTS.md section 7.

set -uo pipefail

pass=0 fail=0 skip=0

PRTE_PBS_SWARM="${PRTE_PBS_SWARM:-prtepbs}"
case "$PRTE_PBS_SWARM" in [_-]*) PRTE_PBS_SWARM="" ;; esac
if [ -z "$PRTE_PBS_SWARM" ] || \
   [ "$PRTE_PBS_SWARM" != "$(printf '%s' "$PRTE_PBS_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PRTE_PBS_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi
NODE="$PRTE_PBS_SWARM-node"            # container names: ${NODE}1 .. ${NODE}10
IMAGE="${IMAGE:-prte-pbs-swarm:latest}"
NNODES=10
PROBE=/opt/prte/pbsswarm/bin/tm-probe
HELPER=/opt/prte/prte/libexec/prte/prte_tm_spawn

ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# as root on node $1
ON() { docker exec "$NODE$1" bash -lc "${*:2}"; }
# as the job user on node $1, with PBS's and PRRTE's commands on the PATH
AS() { docker exec -u prte -w /home/prte "$NODE$1" bash -lc ". /etc/profile.d/pbs.sh; ${*:2}"; }
# pbs-alloc, run on the server
PA() { AS 1 "pbs-alloc $*"; }

# The held allocation tagged $ALLOC_TAG: its mother superior's node number,
# and a command run there, as the job user, in the job's environment.
ALLOC_TAG=dvm
ms() { PA ms --tag "$ALLOC_TAG" | sed 's/^node//'; }
SA() {
    local m
    m="$(ms)" || return 1
    AS "$m" "eval \"\$(pbs-alloc env --tag $ALLOC_TAG)\"; $*"
}

# Submit a batch job: $1 = name, $2 = select, the script on stdin. Waits for
# it to finish; prints the job id. Output goes to ~prte/jobs/<name>.out.
QJOB() {
    local name=$1 select=$2 jobid
    docker exec -i -u prte "${NODE}1" bash -c "mkdir -p ~/jobs; cat > ~/jobs/$name.sh; chmod +x ~/jobs/$name.sh"
    jobid="$(AS 1 "qsub -N $name -l $select -l place=scatter -j oe -o /home/prte/jobs/$name.out ~/jobs/$name.sh")" || return 1
    wait_job "$jobid" "${3:-180}"
    echo "$jobid"
}
wait_job() {   # $1 = job id, $2 = seconds
    local i
    for i in $(seq "$2"); do
        [ "$(job_attr "$1" job_state)" = F ] && return 0
        sleep 1
    done
    return 1
}
job_attr() { AS 1 "qstat -xf $1 2>/dev/null" | awk -v a="$2" '$1 == a { print $3 }'; }
job_out()  { AS 1 "cat ~/jobs/$1.out 2>/dev/null"; }
cput_secs() {  # hh:mm:ss -> seconds
    local t; t="$(job_attr "$1" resources_used.cput)"
    [ -n "$t" ] || { echo 0; return; }
    echo "$t" | awk -F: '{ print ($1 * 3600) + ($2 * 60) + $3 }'
}

# node numbers ("1 2 3") from pbs-alloc's comma-separated names
nums() { echo "$1" | tr ',' ' ' | sed 's/node//g'; }
# how many of the nodes numbered $@ run a prted. A malformed number is an
# error, not a node without a daemon: counting it as zero once made every
# "no daemon is left" check pass whatever was running.
prted_count() {
    local c=0 n
    for n in "$@"; do
        case "$n" in ''|*[!0-9]*) echo "prted_count: bad node number '$n'" >&2; echo -1; return ;; esac
        ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1))
    done
    echo "$c"
}

# Whether node $1's prted is a task of the PBS job - descended from that
# node's pbs_mom - rather than of sshd or of nothing at all.
prted_under_mom() {
    ON "$1" 'p=$(pgrep -x prted | head -1); [ -n "$p" ] || exit 2
             while [ "$p" -gt 1 ]; do
                 [ "$(ps -o comm= -p $p)" = pbs_mom ] && exit 0
                 p=$(ps -o ppid= -p $p | tr -d " ")
             done; exit 1' >/dev/null 2>&1
}

cleanup_cluster() {
    PA free --all >/dev/null 2>&1
    local n
    for n in $(seq 1 $NNODES); do
        ON "$n" 'for t in prted prte prterun prun pterm prte_tm_spawn; do pkill -9 -x $t; done
                 rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/pmix.*
                 true' >/dev/null 2>&1
    done
    # a deleted job frees its nodes on the server's schedule, not at once
    local i
    for i in $(seq 60); do
        [ "$(free_count)" = "$NNODES" ] && return 0
        sleep 1
    done
    echo "  (warning: only $(free_count) of $NNODES nodes free after cleanup)"
}
free_count() { ON 1 '. /etc/profile.d/pbs.sh; pbsnodes -a' | grep -c 'state = free'; }

preflight() {
    banner "preflight"
    local want n got
    want="$(docker image inspect -f '{{.Id}}' "$IMAGE" 2>/dev/null)"
    for n in $(seq 1 $NNODES); do
        got="$(docker inspect -f '{{.Image}}' "$NODE$n" 2>/dev/null)"
        if [ -z "$got" ]; then
            echo "  ${NODE}$n is not running - docker compose up -d" >&2
            exit 2
        fi
        if [ "$got" != "$want" ]; then
            echo "  ${NODE}$n runs an older image than $IMAGE - docker compose up -d --force-recreate" >&2
            exit 2
        fi
        for _ in $(seq 120); do
            ON "$n" 'test -f /var/run/pbsswarm.ready' && break
            sleep 1
        done
    done
    if ! ON 1 'test -f /opt/prte/.build-stamp'; then
        echo "  no complete build in the volume - ./build.sh" >&2
        exit 2
    fi
    echo "  build: $(ON 1 'cat /opt/prte/.build-stamp')"
    for c in "plm: tm" "ras: pbs"; do
        if AS 1 'prte_info' | grep -q "MCA $c"; then
            echo "  MCA $c: built"
        else
            echo "  MCA $c: NOT built - this suite would be testing ssh" >&2
            exit 2
        fi
    done
    if ! ON 1 "test -x $HELPER"; then
        echo "  $HELPER: not installed - plm/tm would decline and ssh launch" >&2
        exit 2
    fi
    cleanup_cluster
}

# ---------------------------------------------------------------------------
# The cluster, before any PRRTE: every later phase blames PRRTE for what it
# finds, and a broken cluster would make all of them lie.
test_cluster() {
    banner "the cluster"
    local j out nodes n

    check "all $NNODES moms are up and free" '[ "$(free_count)" = "$NNODES" ]'

    j="$(QJOB nodefile "select=$NNODES:ncpus=1" <<'EOF'
#!/bin/bash
sort -u "$PBS_NODEFILE" | tr '\n' ' '; echo
EOF
)"
    out="$(job_out nodefile)"
    nodes="$(for n in $(seq 1 $NNODES); do echo -n "node$n "; done)"
    check "a $NNODES-node job runs, and its node file names the nodes by hostname" \
        '[ "$(echo $out | tr " " "\n" | sort -V | tr "\n" " ")" = "$nodes" ]'
    [ "$(echo $out | tr ' ' '\n' | sort -V | tr '\n' ' ')" = "$nodes" ] || \
        echo "    node file: $out  (container names here mean /etc/hosts was not written - see node-entrypoint.sh)"

    # What pbs_tmrsh gives the remote command: no stdin, and the job's
    # environment rather than the caller's. These are the properties a
    # launcher using it has to live with.
    j="$(QJOB tmrsh "select=2:ncpus=1" <<'EOF'
#!/bin/bash
other=$(sort -u "$PBS_NODEFILE" | grep -vx "$(hostname)" | head -1)
echo "rc0=$(pbs_tmrsh $other /bin/true; echo $?)"
echo hello | CALLER_VAR=set pbs_tmrsh $other /bin/sh -c 'read x; echo "stdin=[$x] caller_var=[$CALLER_VAR]"'
EOF
)"
    out="$(job_out tmrsh)"
    check "pbs_tmrsh starts a command on another node of the job" 'echo "$out" | grep -q "^rc0=0"'
    check "pbs_tmrsh gives the command no stdin, and not the caller's environment" \
        'echo "$out" | grep -q "stdin=\[\] caller_var=\[\]"'

    # What a TM launcher relies on: tm_spawn delivers the environment it is
    # handed, its tasks outlive the process that spawned them, and the job
    # is charged for them.
    j="$(QJOB tmprobe "select=3:ncpus=1" <<EOF
#!/bin/bash
$PROBE 'echo "env=\$PRTE_TM_PROBE"; timeout 8 sh -c "while :; do :; done"; echo "done-on-\$(hostname)"'
echo "probe-rc=\$?"
sleep 14
EOF
)"
    out="$(job_out tmprobe)"
    check "tm_spawn: every spawn accepted" 'echo "$out" | grep -q "^spawned=2" && echo "$out" | grep -q "^probe-rc=0"'
    check "tm_spawn delivers the environment it is given" \
        '[ "$(echo "$out" | grep -c "^env=delivered-by-tm_spawn")" = 2 ]'
    check "a TM task runs to the end after its spawner has exited" \
        '[ "$(echo "$out" | grep -c "^done-on-node")" = 2 ]'
    n="$(cput_secs "$j")"
    check "PBS charges the job for the TM tasks (cput ${n}s, two 8s tasks)" '[ "$n" -ge 11 ]'

    # a held allocation, which the interactive cases below run in
    if PA new --tag probe --nodes 2 --ncpus 1 >/dev/null 2>&1; then
        ALLOC_TAG=probe
        check "a held allocation replays its job's environment on its node" \
            'SA "test -n \"\$PBS_JOBID\" && test -r \"\$PBS_NODEFILE\""'
        PA free --tag probe >/dev/null 2>&1
        ALLOC_TAG=dvm
    else
        bad "pbs-alloc new: the holder job did not start"
    fi
}

# ---------------------------------------------------------------------------
# ras/pbs: what the allocation means to the DVM. A node's slots are its
# mpiprocs - the times $PBS_NODEFILE lists it - not its ncpus.
test_ras() {
    banner "ras/pbs"
    cleanup_cluster
    if ! PA new --tag dvm --nodes 3 --ncpus 2 >/dev/null 2>&1; then
        bad "held allocation for the phase"; return
    fi
    local nodes out n
    nodes="$(PA nodes --tag dvm)"
    echo "  allocation: $(PA jobid --tag dvm) on $nodes (2 cpus each)"

    out="$(SA "timeout 60 prterun --prtemca ras_base_verbose 5 --display allocation -n 1 true" 2>&1)"
    check "ras/pbs is the allocation's source" 'echo "$out" | grep -qi "ras:pbs"'
    for n in ${nodes//,/ }; do
        check "the DVM has $n, with the 2 slots PBS assigned it" \
            'echo "$out" | grep -q "^ *$n: slots=2 "'
    done

    out="$(SA "timeout 60 prterun hostname" 2>/dev/null | sort | uniq -c | awk '{print $1}' | sort -u | tr '\n' ' ')"
    check "a job with no -n fills every slot of the allocation (2 per node)" '[ "$out" = "2 " ]'

    local outside=""
    for n in $(seq 1 $NNODES); do
        echo ",$nodes," | grep -q ",node$n," || { outside="node$n"; break; }
    done
    out="$(SA "timeout 60 prterun --host $outside -n 1 hostname" 2>&1)"
    check "--host naming a node outside the allocation is refused" \
        '! echo "$out" | grep -qx "$outside"'

    PA free --tag dvm >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# The launch: the daemons must be tasks of the job - descended from each
# node's pbs_mom - or PBS neither accounts for them nor cleans them up.
test_plm() {
    banner "launch"
    cleanup_cluster
    if ! PA new --tag dvm --nodes 3 --ncpus 1 >/dev/null 2>&1; then
        bad "held allocation for the phase"; return
    fi
    local nodes out m n others
    nodes="$(PA nodes --tag dvm)"
    m="$(ms)"
    others="$(echo "${nodes//,/ }" | tr ' ' '\n' | grep -vx "node$m" | sed 's/^node//' | tr '\n' ' ')"

    # The helper exists to keep PRRTE's libraries away from the TM
    # library, so it may need only libpbs, libc, and what libpbs itself
    # needs or its libtool archive declares (libtool adds those for a
    # -lpbs). Anything else came from PRRTE's own link flags.
    local extra allowed
    allowed="$(ON 1 'readelf -d /opt/pbs/lib/libpbs.so' | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
               ON 1 'sed -n "s/^dependency_libs=//p" /opt/pbs/lib/libpbs.la 2>/dev/null' |
                   tr -d "'" | tr ' ' '\n' | sed -n 's/^-l\(.*\)/lib\1.so/p')"
    extra="$(ON 1 "readelf -d $HELPER" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' |
             grep -v -e '^libpbs\.so' -e '^libc\.so' -e '^ld-linux' |
             while read -r lib; do
                 echo "$allowed" | grep -qx -e "$lib" -e "${lib%.so*}.so" || echo "$lib"
             done)"
    check "the TM helper needs nothing beyond libpbs and what libpbs needs" '[ -z "$extra" ]'
    [ -z "$extra" ] || echo "    also needs: $(echo $extra)"

    SA "timeout 90 prterun --map-by ppr:1:node sleep 8" >/dev/null 2>&1 &
    local bg=$!
    sleep 5
    local under=0
    for n in $others; do prted_under_mom "$n" && under=$((under+1)); done
    wait "$bg"
    check "every remote daemon is a task of the job (under pbs_mom)" '[ "$under" = "$(echo $others | wc -w | tr -d " ")" ]'

    out="$(SA "timeout 90 prterun --prtemca plm_base_verbose 5 --map-by ppr:1:node hostname" 2>&1)"
    check "the job ran on every node of the allocation" \
        '[ "$(echo "$out" | grep -cx "node[0-9]*")" = 3 ]'
    echo "  launcher: $(echo "$out" | grep -o 'plm:[a-z]*:' | sort -u | tr '\n' ' ')"
    sleep 2
    check "no daemon is left behind" '[ "$(prted_count $(nums "$nodes"))" = 0 ]'

    # The DVM key reaches each daemon in the environment tm_spawn sends,
    # never on a command line - and the daemon wipes it from its own.
    SA "timeout 90 prterun --map-by ppr:1:node sleep 8" >/dev/null 2>&1 &
    bg=$!
    sleep 5
    local leaked=0 seen=0 h
    for n in $others; do
        ON "$n" 'p=$(pgrep -x prted | head -1); [ -n "$p" ] || exit 2
                 tr "\0" "\n" < /proc/$p/cmdline | grep -q "^PRTE_DVM_KEY\|[0-9a-f]\{64\}" && exit 1
                 tr "\0" "\n" < /proc/$p/environ | grep -q "^PRTE_DVM_KEY=[0-9a-f]\{64\}" && exit 1
                 exit 0' >/dev/null 2>&1
        case $? in 0) seen=$((seen+1)) ;; 1) leaked=$((leaked+1)) ;; esac
    done
    h="$(ON "$m" 'p=$(pgrep -x prte_tm_spawn | head -1); [ -n "$p" ] || exit 2
                  tr "\0" "\n" < /proc/$p/environ | grep -c "^PRTE_DVM_KEY="' 2>/dev/null)"
    wait "$bg"
    check "the key is on no daemon's command line, and gone from its environment" \
        '[ "$seen" = "$(echo $others | wc -w | tr -d " ")" ] && [ "$leaked" = 0 ]'
    check "the helper does not carry the key in its own environment" '[ "$h" = 0 ]'

    # A daemon that cannot start is reported when its task ends, not waited
    # for: TM has no notion of "exec failed", only of a task that ended.
    out="$(SA "timeout 60 prterun --prtemca prte_launch_agent /nonexistent/prted --map-by ppr:1:node hostname; echo rc=\$?" 2>&1)"
    check "a daemon that cannot start is reported, and the launch fails" \
        'echo "$out" | grep -q "could not be started as a task of the PBS job" && echo "$out" | grep -q "^rc=[1-9]" && ! echo "$out" | grep -q "^rc=124"'
    sleep 2

    # Without the helper, plm/tm says so and declines, and ssh launches -
    # daemons PBS does not account for. PRTE_LIBEXECDIR moves where PRRTE
    # looks for it.
    out="$(SA "PRTE_LIBEXECDIR=/nonexistent timeout 90 prterun --prtemca plm_base_verbose 1 --map-by ppr:1:node hostname" 2>&1)"
    check "with the helper missing, it says so and ssh launches instead" \
        'echo "$out" | grep -q "cannot run the program it starts" && [ "$(echo "$out" | grep -cx "node[0-9]*")" = 3 ]'

    PA free --tag dvm >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# What the launcher exists for: PBS charges the job for the CPU the remote
# nodes used. The control is the same job launched with ssh.
test_accounting() {
    banner "accounting"
    cleanup_cluster
    local j k n m spin
    spin="timeout 10 sh -c 'while :; do :; done'; exit 0"

    j="$(QJOB acct-pbs "select=3:ncpus=1" <<EOF
#!/bin/bash
prterun --bind-to none --map-by ppr:1:node sh -c "$spin"
sleep 4
EOF
)"
    n="$(cput_secs "$j")"
    check "a 3-node PRRTE job is charged for all three nodes (cput ${n}s of ~30s)" '[ "$n" -ge 21 ]'

    k="$(QJOB acct-ssh "select=3:ncpus=1" <<EOF
#!/bin/bash
prterun --prtemca plm ssh --bind-to none --map-by ppr:1:node sh -c "$spin"
sleep 4
EOF
)"
    m="$(cput_secs "$k")"
    echo "  for comparison, launched with ssh: cput ${m}s (PBS sees only the head node)"
    check "the ssh launch is charged for less than the PBS launch" '[ "$m" -lt "$n" ]'
}

# ---------------------------------------------------------------------------
# qdel: deleting the job must take every daemon with it.
test_qdel() {
    banner "qdel"
    cleanup_cluster
    if ! PA new --tag dvm --nodes 3 --ncpus 1 >/dev/null 2>&1; then
        bad "held allocation for the phase"; return
    fi
    local nodes jobid i left
    nodes="$(PA nodes --tag dvm)"
    jobid="$(PA jobid --tag dvm)"
    docker exec -d -u prte -w /home/prte "${NODE}$(ms)" bash -lc \
        ". /etc/profile.d/pbs.sh; eval \"\$(pbs-alloc env --tag dvm)\"; prte > /tmp/prte-qdel.out 2>&1"
    for i in $(seq 30); do
        [ "$(prted_count $(nums "$nodes"))" -ge 2 ] && break
        sleep 1
    done
    check "a DVM is up across the allocation" '[ "$(prted_count $(nums "$nodes"))" -ge 2 ]'
    AS 1 "qdel $jobid" >/dev/null 2>&1
    for i in $(seq 30); do
        left="$(prted_count $(nums "$nodes"))"
        [ "$left" = 0 ] && break
        sleep 1
    done
    check "qdel takes every daemon down with the job" '[ "$left" = 0 ]'
    PA free --tag dvm >/dev/null 2>&1
}

preflight
phases="${*:-cluster ras plm accounting qdel}"
for p in $phases; do
    case "$p" in
        cluster)    test_cluster ;;
        ras)        test_ras ;;
        plm)        test_plm ;;
        accounting) test_accounting ;;
        qdel)       test_qdel ;;
        *) echo "unknown phase: $p" >&2; exit 2 ;;
    esac
done
cleanup_cluster

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ]
