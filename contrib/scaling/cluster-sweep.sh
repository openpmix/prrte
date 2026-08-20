#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# cluster-sweep.sh -- measure what the DVM collectives cost on real hardware.
#
# This is the cluster counterpart of contrib/dockerswarm/scaletest.sh.  That
# one runs a swarm of containers on one host, which is fine for correctness
# and useless for this measurement: a collective's cost is dominated by terms
# that need independent nodes and independent network links, and a single host
# has neither.  Everything here exists because the container harness could not
# answer the question.
#
# WHAT WE ARE TRYING TO LEARN
#
#   The DVM moves a fence on the RML routing tree: contributions roll UP to
#   the HNP, and the result is broadcast back DOWN.  In the broadcast
#   direction a daemon with r children transmits r copies of the whole modex,
#   so the cost carries a factor of the routing radix.  The default radix is
#   64.  Whether lowering it is worth the extra tree depth is the central
#   question, and it cannot be answered without real links.
#
#   The sweep also prices three things we found dominate on a single host and
#   need confirming (or refuting) on a cluster: the PMIx GDS component, the
#   worker threads, and how much of a fence's cost is payload at all.
#
# HOW TO RUN IT
#
#   Get an allocation, then run this once.  It finds the nodes itself.
#
#       salloc -N 128 ...           # or qsub -l nodes=128, or --hostfile
#       ./cluster-sweep.sh
#
#   It writes everything into a results directory and prints a summary.  Send
#   back the whole directory (it tars itself up at the end).
#
#       ./cluster-sweep.sh --dry-run        # print the plan and an estimate
#       ./cluster-sweep.sh --quick          # a much shorter sweep
#       ./cluster-sweep.sh --phases radix,barrier
#
# WHAT IT NEEDS
#
#   prte, prun, pterm and prte_info on PATH, a PMIx that PRRTE was built
#   against, a C compiler, and an allocation.  Nothing else -- no containers,
#   no root, no scheduler-specific launcher.
#
# THE TRAPS THIS SCRIPT IS BUILT AROUND
#
#   Every one of these was learned by getting a wrong answer first.
#
#   * A repeated fence is NOT a repeated sample.  The client publishes new
#     keys each iteration, so the daemon's store grows and iteration k is a
#     fence over k rounds of data.  Measured on a container swarm, the minimum
#     was iteration 1 in all 36 configurations and iteration 10 cost 5-20x
#     iteration 1.  So this script runs ONE measured fence per job and repeats
#     the JOB, which is the only way to get independent samples of the thing a
#     real application actually pays.
#   * A DVM that came up with fewer nodes than you asked for reports a
#     perfectly plausible number for the wrong scale.  Every DVM is verified
#     by counting distinct hostnames before anything is measured.
#   * A run whose client failed still prints timings for the phases that did
#     run.  Rows are only recorded when the client reported every phase.
#   * The absolute numbers depend on the build.  --enable-debug roughly
#     doubles them, so the manifest records the build flags; compare ratios
#     within one run, not across builds.
#
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# defaults
# ---------------------------------------------------------------------------

# Node counts are powers of two up to the allocation, filled in once we know
# how big it is.  Radices bracket the question: 2 and 3 are the low end the
# cost model likes, 64 is today's default, 16 is a plausible compromise.
RADICES_DEFAULT="2,3,4,8,16,64"
PPN_DEFAULT="1"
# A modex contribution is nkeys*size per rank.  64 B is OFI/TCP territory,
# 1-8 KB is UCX with several transports, 64 KB is deliberately past anything
# real so the byte term is unmistakable.
NKEYS_DEFAULT="8"
SIZES_DEFAULT="8,128,1024,8192"
REPS_DEFAULT=5
PHASES_DEFAULT="env,radix,payload,barrier,gds,threads"

radices="$RADICES_DEFAULT"
ppn_list="$PPN_DEFAULT"
nkeys="$NKEYS_DEFAULT"
sizes="$SIZES_DEFAULT"
reps="$REPS_DEFAULT"
phases="$PHASES_DEFAULT"
nodes_list=""
hostfile=""
outdir=""
dry=0
quick=0
job_timeout=300
dvm_timeout=300

usage() {
    sed -n '10,52p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --nodes A,B,C     DVM sizes to sweep      (default: powers of 2 up to the allocation)
  --radix A,B,C     routing radices         (default: 2,3,4,8,16,64)
  --ppn N,M         procs per node          (default: 1)
  --nkeys N         keys each rank publishes(default: 8)
  --sizes A,B,C     bytes per key           (default: 8,128,1024,8192)
  --reps N          independent jobs per configuration (default: 5)
  --phases a,b      env,radix,payload,barrier,gds,threads
  --hostfile PATH   use this hostfile instead of detecting the allocation
  --out DIR         results directory       (default: ./scaling-results-<host>-<date>)
  --job-timeout S   seconds before a single job is abandoned (default: 300)
  --quick           a much shorter sweep -- use this first to shake out the setup
  --dry-run         print the plan and a time estimate, run nothing
  -h, --help        this text
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --nodes)        nodes_list=$2; shift 2 ;;
        --radix)        radices=$2; shift 2 ;;
        --ppn)          ppn_list=$2; shift 2 ;;
        --nkeys)        nkeys=$2; shift 2 ;;
        --sizes)        sizes=$2; shift 2 ;;
        --reps)         reps=$2; shift 2 ;;
        --phases)       phases=$2; shift 2 ;;
        --hostfile)     hostfile=$2; shift 2 ;;
        --out)          outdir=$2; shift 2 ;;
        --job-timeout)  job_timeout=$2; shift 2 ;;
        --quick)        quick=1; shift ;;
        --dry-run)      dry=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo ">>> $*"; }

has_phase() { case ",$phases," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

# ---------------------------------------------------------------------------
# find the allocation
# ---------------------------------------------------------------------------
#
# Three ways, in priority order: an explicit hostfile, SLURM, PBS.  We build
# our OWN host list rather than letting prte discover the allocation, because
# a sweep needs to run a 4-node DVM inside a 128-node allocation and that
# means naming the nodes.

ALLOC_KIND="unknown"
ALL_NODES=()

discover_nodes() {
    local n
    if [ -n "$hostfile" ]; then
        [ -r "$hostfile" ] || die "cannot read hostfile $hostfile"
        ALLOC_KIND="hostfile:$hostfile"
        while read -r n _; do
            case "$n" in ''|\#*) continue ;; esac
            ALL_NODES+=("$n")
        done < "$hostfile"
    elif [ -n "${SLURM_JOB_NODELIST:-}" ]; then
        ALLOC_KIND="slurm"
        if command -v scontrol >/dev/null 2>&1; then
            while read -r n; do [ -n "$n" ] && ALL_NODES+=("$n"); done \
                < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
        else
            die "SLURM_JOB_NODELIST is set but scontrol is not on PATH"
        fi
    elif [ -n "${PBS_NODEFILE:-}" ] && [ -r "${PBS_NODEFILE}" ]; then
        ALLOC_KIND="pbs"
        while read -r n _; do
            case "$n" in ''|\#*) continue ;; esac
            ALL_NODES+=("$n")
        done < <(sort -u "$PBS_NODEFILE")
    else
        die "no allocation found -- run inside salloc/sbatch or qsub, or pass --hostfile"
    fi

    # de-duplicate while preserving order (a PBS nodefile lists a node once
    # per slot, and a hostfile may too)
    local seen=" " uniq=() h
    for h in "${ALL_NODES[@]}"; do
        case "$seen" in *" $h "*) continue ;; esac
        seen="$seen$h "
        uniq+=("$h")
    done
    ALL_NODES=("${uniq[@]}")
}

discover_nodes
NNODES=${#ALL_NODES[@]}
[ "$NNODES" -ge 2 ] || die "need at least 2 nodes, found $NNODES"

# Default node sweep: powers of two up to the allocation, always including
# the full allocation as the last point (that is the one that matters most).
if [ -z "$nodes_list" ]; then
    l=""; k=2
    while [ "$k" -le "$NNODES" ]; do l="${l:+$l,}$k"; k=$((k * 2)); done
    last=${l##*,}
    [ "$last" = "$NNODES" ] || l="${l:+$l,}$NNODES"
    nodes_list=$l
fi

if [ "$quick" -eq 1 ]; then
    # Enough to prove the setup works and to see the radix effect, if any.
    radices="3,64"
    sizes="1024"
    reps=3
    phases="env,radix,barrier"
    # smallest, middle and full allocation
    mid=$((NNODES / 2)); [ "$mid" -lt 2 ] && mid=2
    nodes_list="2,$mid,$NNODES"
fi

# ---------------------------------------------------------------------------
# results directory and manifest
# ---------------------------------------------------------------------------

[ -n "$outdir" ] || outdir="$PWD/scaling-results-$(hostname -s)-$(date +%Y%m%d-%H%M%S)"
RAW="$outdir/samples.csv"
LOG="$outdir/run.log"
MANIFEST="$outdir/manifest.txt"

# Everything the numbers cannot be interpreted without.  We have been burned
# by not knowing the core count and the filesystem of the machine a number
# came from, so this is deliberately generous.
write_manifest() {
    {
        echo "# cluster-sweep.sh manifest"
        echo "date:            $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "host:            $(hostname -f 2>/dev/null || hostname)"
        echo "allocation:      $ALLOC_KIND, $NNODES nodes"
        echo "nodes:           ${ALL_NODES[*]}"
        echo "uname:           $(uname -a)"
        echo "cores/node:      $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?')"
        echo "cpu model:       $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null | sed 's/^ *//')"
        echo "memory:          $(awk '/MemTotal/{print $2/1048576 " GiB"}' /proc/meminfo 2>/dev/null || echo '?')"
        echo "TMPDIR:          ${TMPDIR:-/tmp}"
        echo "tmp filesystem:  $(df -T "${TMPDIR:-/tmp}" 2>/dev/null | tail -1 || df "${TMPDIR:-/tmp}" | tail -1)"
        echo
        echo "sweep:"
        echo "  nodes:   $nodes_list"
        echo "  radix:   $radices"
        echo "  ppn:     $ppn_list"
        echo "  nkeys:   $nkeys"
        echo "  sizes:   $sizes"
        echo "  reps:    $reps"
        echo "  phases:  $phases"
        echo
        echo "== prte_info =="
        prte_info --all 2>/dev/null | grep -iE "prte:version|repo rev|pmix:version|configure command|c compiler:|build cflags" || true
        echo
        echo "== gds components =="
        pmix_info 2>/dev/null | grep -i "MCA gds" || true
        echo
        echo "== network interfaces =="
        (ip -o addr show 2>/dev/null || ifconfig -a 2>/dev/null) | head -40
    } > "$MANIFEST" 2>&1
}

# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------

BIN=""
TIMEOUT=""

preflight() {
    local t
    for t in prte prun pterm prte_info; do
        command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
    done
    if command -v timeout >/dev/null 2>&1; then
        TIMEOUT="timeout -k 10"
    else
        echo "WARNING: 'timeout' not found -- a hung job will stall the sweep" >&2
    fi

    # Build the client against the PMIx this PRRTE uses.  prte_info reports
    # the prefix it was configured with; fall back to pkg-config, then to a
    # bare compile in case the headers are already on the default path.
    local pmix_prefix cflags ldflags
    pmix_prefix=$(prte_info --all 2>/dev/null \
        | awk -F: '/with-pmix=/{ sub(/.*with-pmix=/,""); sub(/[^A-Za-z0-9_\/.\-].*/,""); print; exit }')
    if [ -n "$pmix_prefix" ] && [ -d "$pmix_prefix/include" ]; then
        cflags="-I$pmix_prefix/include"
        ldflags="-L$pmix_prefix/lib -Wl,-rpath,$pmix_prefix/lib"
    elif pkg-config --exists pmix 2>/dev/null; then
        cflags=$(pkg-config --cflags pmix)
        ldflags=$(pkg-config --libs-only-L pmix)
    else
        cflags=""; ldflags=""
    fi

    BIN="$outdir/scaletest"
    say "building the client: ${CC:-cc} $cflags -> $BIN"
    # shellcheck disable=SC2086
    ${CC:-cc} -O2 -g -o "$BIN" "$here/scaletest.c" $cflags $ldflags -lpmix 2>"$outdir/build.log" \
        || { sed -n '1,20p' "$outdir/build.log" >&2; die "could not build scaletest.c (see $outdir/build.log)"; }
}

# ---------------------------------------------------------------------------
# DVM lifecycle
# ---------------------------------------------------------------------------

URI=""
DVM_UP=0

hostspec() {   # <nodes> <slots>
    local n=$1 slots=$2 i out=""
    for ((i = 0; i < n; i++)); do out="${out:+$out,}${ALL_NODES[$i]}:$slots"; done
    echo "$out"
}

dvm_start() {  # <nodes> <slots> <radix> <extra mca...>
    local n=$1 slots=$2 radix=$3; shift 3
    local extra="$*" tries seen

    URI="$outdir/dvm.uri"
    rm -f "$URI"
    # shellcheck disable=SC2086
    prte --daemonize --report-uri "$URI" \
         --prtemca rml_base_radix "$radix" $extra \
         --host "$(hostspec "$n" "$slots")" >> "$LOG" 2>&1

    for ((tries = 0; tries < dvm_timeout; tries++)); do
        [ -s "$URI" ] && break
        sleep 1
    done
    [ -s "$URI" ] || { echo "    DVM never reported a URI" >&2; return 1; }

    # A DVM that came up short reports a believable number for the wrong
    # scale, so count the daemons that actually answer before measuring.
    seen=$($TIMEOUT 120 prun --dvm-uri "file:$URI" --map-by ppr:1:node -n "$n" hostname 2>/dev/null \
           | sort -u | grep -c . || true)
    if [ "${seen:-0}" -ne "$n" ]; then
        echo "    DVM came up with ${seen:-0} of $n nodes -- skipping" >&2
        dvm_stop
        return 1
    fi
    DVM_UP=1
    return 0
}

dvm_stop() {
    [ -n "$URI" ] && [ -s "$URI" ] && $TIMEOUT 60 pterm --dvm-uri "file:$URI" >> "$LOG" 2>&1
    DVM_UP=0
    rm -f "$URI"
}

trap 'echo; echo "interrupted -- shutting down the DVM"; dvm_stop; exit 130' INT TERM

# ---------------------------------------------------------------------------
# one measured job
# ---------------------------------------------------------------------------
#
# ONE measured fence, then the job exits.  Repeating the job rather than the
# iteration is the whole point: the client publishes new keys per iteration,
# so a second iteration measures a fence over twice the data.  A fresh job is
# a fresh namespace and therefore a fresh modex, which is what an application
# actually pays.
#
# The recorded value is the largest per-rank duration, not the wall clock
# across ranks, so that skew in the preceding phase is not charged here.

parse_sample() {   # <capture-file> -> "put,collect,barrier" in microseconds
    awk '
        $0 ~ /^SCALE / {
            it = -1
            for (i = 1; i <= NF; i++) {
                if ($i == "ITER")     { it = $(i+1) }
                if ($i == "PUT")      { ps = $(i+1); pe = $(i+2) }
                if ($i == "COLLECT")  { cs = $(i+1); ce = $(i+2) }
                if ($i == "BARRIER")  { bs = $(i+1); be = $(i+2) }
            }
            if (it != 0) { next }
            n++
            p = (pe - ps)/1000.0; c = (ce - cs)/1000.0; b = (be - bs)/1000.0
            if (p > mp) mp = p
            if (c > mc) mc = c
            if (b > mb) mb = b
        }
        END { if (n > 0) printf "%.1f,%.1f,%.1f", mp, mc, mb }' "$1"
}

run_job() {   # <phase> <nodes> <radix> <ppn> <nkeys> <size> <gds> <threads> <rep>
    local phase=$1 n=$2 radix=$3 ppn=$4 k=$5 s=$6 gds=$7 thr=$8 rep=$9
    local nprocs=$((n * ppn)) cap tag vals mca=""

    [ "$gds" != "default" ] && mca="$mca --pmixmca gds $gds"
    cap=$(mktemp "${TMPDIR:-/tmp}/csweep-XXXXXX")
    tag="${phase}-n${n}r${radix}p${ppn}k${k}s${s}"

    # shellcheck disable=SC2086
    if ! $TIMEOUT "$job_timeout" prun --dvm-uri "file:$URI" $mca \
             --map-by "ppr:$ppn:node" --bind-to none -n "$nprocs" \
             "$BIN" --tag "$tag" --nkeys "$k" --sizes "$s" \
                    --iters 1 --warmup 0 --entropy >"$cap" 2>&1; then
        echo "    FAILED $tag rep$rep: $(tail -2 "$cap" | tr '\n' ' ')" >> "$LOG"
        rm -f "$cap"; return 1
    fi
    vals=$(parse_sample "$cap")
    if [ -z "$vals" ]; then
        echo "    NO DATA $tag rep$rep: $(tail -2 "$cap" | tr '\n' ' ')" >> "$LOG"
        rm -f "$cap"; return 1
    fi
    rm -f "$cap"

    local put col bar
    IFS=, read -r put col bar <<< "$vals"
    printf '%s,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%d,%s,%s,%s,%.1f\n' \
        "$phase" "$n" "$radix" "$ppn" "$nprocs" "$k" "$s" $((k * s)) $((k * s * nprocs)) \
        "$gds" "$thr" "$rep" "$put" "$col" "$bar" \
        "$(awk -v a="$col" -v b="$bar" 'BEGIN{print a-b}')" >> "$RAW"
    return 0
}

# Every configuration that shares a DVM runs inside one start/stop.
run_reps() {  # <phase> <nodes> <radix> <ppn> <nkeys> <size> <gds> <threads>
    local r
    for ((r = 1; r <= reps; r++)); do
        run_job "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$r" || true
    done
}

# ---------------------------------------------------------------------------
# the sweep
# ---------------------------------------------------------------------------

max_ppn() { local p m=1; for p in ${ppn_list//,/ }; do [ "$p" -gt "$m" ] && m=$p; done; echo $((m + 1)); }

count_plan() {
    local dvms=0 jobs=0 n r p s
    for n in ${nodes_list//,/ }; do
        for r in ${radices//,/ }; do
            has_phase radix   && { dvms=$((dvms+1)); for p in ${ppn_list//,/ }; do jobs=$((jobs+reps)); done; }
            has_phase barrier && { dvms=$((dvms+1)); jobs=$((jobs+reps)); }
        done
        if has_phase payload; then
            for r in 3 64; do dvms=$((dvms+1))
                for s in ${sizes//,/ }; do jobs=$((jobs+reps)); done
            done
        fi
        has_phase gds     && { dvms=$((dvms+2)); jobs=$((jobs+2*reps)); }
        has_phase threads && { dvms=$((dvms+2)); jobs=$((jobs+2*reps)); }
    done
    echo "$dvms $jobs"
}

sweep() {
    local n r p s slots
    slots=$(max_ppn)

    for n in ${nodes_list//,/ }; do
        [ "$n" -le "$NNODES" ] || { say "skipping nodes=$n: allocation has $NNODES"; continue; }

        # --- radix: the primary question, at one payload -----------------
        if has_phase radix; then
            for r in ${radices//,/ }; do
                say "radix phase: nodes=$n radix=$r"
                if dvm_start "$n" "$slots" "$r"; then
                    run_job warm "$n" "$r" 1 "$nkeys" 1024 default 0 0 >/dev/null 2>&1 || true
                    for p in ${ppn_list//,/ }; do
                        run_reps radix "$n" "$r" "$p" "$nkeys" 1024 default 0
                    done
                    dvm_stop
                fi
            done
        fi

        # --- barrier: no payload at all ----------------------------------
        if has_phase barrier; then
            for r in ${radices//,/ }; do
                say "barrier phase: nodes=$n radix=$r"
                if dvm_start "$n" "$slots" "$r"; then
                    run_job warm "$n" "$r" 1 0 1 default 0 0 >/dev/null 2>&1 || true
                    run_reps barrier "$n" "$r" 1 0 1 default 0
                    dvm_stop
                fi
            done
        fi

        # --- payload: where the byte term takes over ----------------------
        if has_phase payload; then
            for r in 3 64; do
                say "payload phase: nodes=$n radix=$r"
                if dvm_start "$n" "$slots" "$r"; then
                    run_job warm "$n" "$r" 1 "$nkeys" 1024 default 0 0 >/dev/null 2>&1 || true
                    for s in ${sizes//,/ }; do
                        run_reps payload "$n" "$r" 1 "$nkeys" "$s" default 0
                    done
                    dvm_stop
                fi
            done
        fi

        # --- gds: shmem3 (default) against hash ---------------------------
        if has_phase gds; then
            for g in default hash; do
                say "gds phase: nodes=$n gds=$g"
                if dvm_start "$n" "$slots" 64 "$([ "$g" = hash ] && echo '--pmixmca gds hash')"; then
                    run_job warm "$n" 64 1 "$nkeys" 1024 "$g" 0 0 >/dev/null 2>&1 || true
                    run_reps gds "$n" 64 1 "$nkeys" 1024 "$g" 0
                    dvm_stop
                fi
            done
        fi

        # --- threads: the worker thread pool off against on ---------------
        if has_phase threads; then
            for t in 0 $((64 / 4)); do
                say "threads phase: nodes=$n worker_threads=$t"
                if dvm_start "$n" "$slots" 64 "--prtemca prte_num_worker_threads $t"; then
                    run_job warm "$n" 64 1 "$nkeys" 1024 default "$t" 0 >/dev/null 2>&1 || true
                    run_reps threads "$n" 64 1 "$nkeys" 1024 default "$t"
                    dvm_stop
                fi
            done
        fi
    done
}

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

summarize() {
    local out="$outdir/summary.txt"
    awk -F, '
        NR == 1 { next }
        {
            key = $1 "|" $2 "|" $3 "|" $4 "|" $7 "|" $10 "|" $11
            c[key, ++n[key]] = $16 + 0        # data cost (collect - barrier)
            b[key, ++m[key]] = $15 + 0        # barrier
            seen[key] = 1
        }
        function med(arr, key, cnt,   i, j, t, a) {
            for (i = 1; i <= cnt; i++) a[i] = arr[key, i]
            for (i = 1; i < cnt; i++) for (j = i+1; j <= cnt; j++)
                if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
            return (cnt % 2) ? a[(cnt+1)/2] : (a[cnt/2] + a[cnt/2+1]) / 2
        }
        END {
            printf "%-8s %6s %6s %5s %8s %8s %8s %5s %11s %11s\n", \
                   "phase","nodes","radix","ppn","B/rank","gds","threads","reps", \
                   "barrier_us","datacost_us"
            for (k in seen) {
                split(k, f, "|")
                printf "%-8s %6d %6d %5d %8d %8s %8s %5d %11.0f %11.0f\n", \
                       f[1], f[2], f[3], f[4], f[5], f[6], f[7], n[k], \
                       med(b, k, m[k]), med(c, k, n[k])
            }
        }' "$RAW" | (read -r h; echo "$h"; sort -k1,1 -k2,2n -k3,3n -k5,5n) > "$out"

    echo
    say "summary (medians over reps):"
    echo
    column -t < "$out" 2>/dev/null || cat "$out"
    echo
    say "raw samples : $RAW"
    say "summary     : $out"
    say "manifest    : $MANIFEST"
    say "log         : $LOG"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

read -r plan_dvms plan_jobs <<< "$(count_plan)"

if [ "$dry" -eq 1 ]; then
    echo "allocation : $ALLOC_KIND, $NNODES nodes"
    echo "nodes      : $nodes_list"
    echo "radix      : $radices"
    echo "ppn        : $ppn_list"
    echo "sizes      : $sizes  (nkeys=$nkeys)"
    echo "reps       : $reps"
    echo "phases     : $phases"
    echo
    echo "plan       : $plan_dvms DVM start/stop cycles, $plan_jobs measured jobs"
    echo "estimate   : roughly $(( (plan_dvms * 45 + plan_jobs * 10) / 60 )) minutes"
    echo "             (45 s a DVM cycle, 10 s a job -- both are guesses until"
    echo "              you have run --quick once and can substitute real ones)"
    exit 0
fi

mkdir -p "$outdir" || die "cannot create $outdir"
: > "$LOG"
echo "phase,nodes,radix,ppn,nprocs,nkeys,size,bytes_per_rank,total_bytes,gds,worker_threads,rep,put_us,collect_us,barrier_us,data_cost_us" > "$RAW"

say "results  -> $outdir"
say "plan     -> $plan_dvms DVM cycles, $plan_jobs jobs (~$(( (plan_dvms * 45 + plan_jobs * 10) / 60 )) min estimated)"
preflight
has_phase env && write_manifest
say "manifest -> $MANIFEST"

start=$(date +%s)
sweep
end=$(date +%s)
say "sweep took $(( (end - start) / 60 )) min"

summarize

tarball="$outdir.tar.gz"
tar czf "$tarball" -C "$(dirname "$outdir")" "$(basename "$outdir")" 2>/dev/null \
    && say "send this back: $tarball"
