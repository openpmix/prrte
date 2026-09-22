#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Prepare a real cluster to run the multi-node suite (`run-tests.sh cluster`).
#
#   ./cluster-setup.sh --prefix /path/to/prrte-install
#
# Two jobs, and they are separable:
#
#   BUILD    the suite's helper clients -- bare PMIx programs that put the
#            runtime in states no ordinary application reaches -- into the
#            install's bindir, the way contrib/dockerswarm/build.sh does for
#            the container swarm.  Without them most of the suite has nothing
#            to run.
#   CHECK    that the cluster can actually host the suite: every node
#            reachable, every node carrying the same install, a scratch
#            directory that is NOT shared, and a `prterun` that works.
#
# It never installs PRRTE itself and never touches your source tree -- build
# and install PRRTE the ordinary way first (see docs/install.rst).
#
# Full account: docs/testing/cluster.rst.

set -uo pipefail

PREFIX=""; PMIX=""; BINDIR=""; NODES=""; HOSTFILE=""
RSH="${PRTE_CLUSTER_RSH:-ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new}"
WORKDIR="${PRTE_CLUSTER_WORKDIR:-}"
DO_BUILD=1; DO_CHECK=1
CC_BIN="${CC:-cc}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    sed -n '10,28p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'USAGE'

Options:
  --prefix DIR     the PRRTE installation to prepare (default: from `prte` on PATH)
  --pmix DIR       the PMIx it was built against (default: asked of pkg-config)
  --bindir DIR     where the helper clients go (default: <prefix>/bin)
  --nodes a,b,c    the nodes to check, head node first
  --hostfile FILE  ...or read them from a file, one per line
  --rsh CMD        how to reach a node (default: ssh -o BatchMode=yes ...)
  --workdir DIR    per-node scratch to check (default: /tmp/prte-cluster-$USER)
  --check-only     do not build anything, just check
  --no-check       build the clients and stop
  -h, --help       this
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)   PREFIX=$2; shift 2 ;;
        --pmix)     PMIX=$2; shift 2 ;;
        --bindir)   BINDIR=$2; shift 2 ;;
        --nodes)    NODES=$2; shift 2 ;;
        --hostfile) HOSTFILE=$2; shift 2 ;;
        --rsh)      RSH=$2; shift 2 ;;
        --workdir)  WORKDIR=$2; shift 2 ;;
        --check-only) DO_BUILD=0; shift ;;
        --no-check)   DO_CHECK=0; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die()  { echo "cluster-setup: $*" >&2; exit 1; }
say()  { printf '>>> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }

########################################################################
# Where things are
########################################################################

if [ -z "$PREFIX" ]; then
    PREFIX=$(command -v prte 2>/dev/null) || true
    PREFIX=${PREFIX%/bin/prte}
fi
[ -n "$PREFIX" ] && [ -x "$PREFIX/bin/prte" ] \
    || die "no PRRTE install found; pass --prefix, or put prte on PATH"
PREFIX=$(cd "$PREFIX" && pwd)
BINDIR=${BINDIR:-$PREFIX/bin}

if [ -z "$PMIX" ]; then
    PMIX=$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
           pkg-config --variable=prefix pmix 2>/dev/null)
fi
[ -n "$PMIX" ] || PMIX=$PREFIX
[ -r "$PMIX/include/pmix.h" ] \
    || die "no pmix.h under $PMIX -- pass --pmix <prefix of the PMIx PRRTE was built against>"

WORKDIR=${WORKDIR:-/tmp/prte-cluster-$(id -un)}

say "PRRTE  $PREFIX"
say "PMIx   $PMIX"
say "clients-> $BINDIR"

########################################################################
# Build the helper clients
########################################################################
#
# Same set, same flags, as build.sh compiles into the container swarm's
# install.  The -Wl,-rpath is not decoration: an application launched onto a
# node other than the head node gets an EMPTY LD_LIBRARY_PATH -- it inherits
# the daemon's environment, not your login shell's -- so without it a client
# loads whatever libpmix the system linker finds, which on a cluster with a
# packaged PMIx is a different library with the same soname.  That is silent:
# same version string, different code.

CLIENTS="
elastic    contrib/dockerswarm/elastic.c
jobinfo    contrib/dockerswarm/jobinfo.c
devinfo    contrib/dockerswarm/devinfo.c
spawnloop  contrib/dockerswarm/spawnloop.c
dataserver contrib/dockerswarm/dataserver.c
proctable  contrib/dockerswarm/proctable.c
groupcon   contrib/dockerswarm/groupcon.c
connector  contrib/dockerswarm/connector.c
groupinv   contrib/dockerswarm/groupinv.c
fencer     contrib/dockerswarm/fencer.c
pmixloop   contrib/dockerswarm/pmixloop.c
faulty     contrib/dockerswarm/faulty.c
envspawn   contrib/dockerswarm/envspawn.c
peerinfo   contrib/dockerswarm/peerinfo.c
slotinfo   contrib/dockerswarm/slotinfo.c
scaletest  contrib/scaling/scaletest.c
dynamic    examples/dynamic.c
sessionctrl examples/sessionctrl.c
"

build_clients() {
    local name src rc=0 built=0
    command -v "$CC_BIN" >/dev/null 2>&1 || die "no C compiler ($CC_BIN); set CC="
    [ -w "$BINDIR" ] || die "$BINDIR is not writable -- install PRRTE into a prefix you own,
                 or pass --bindir <a directory on every node's PATH>"
    say "building the suite's helper clients"
    while read -r name src; do
        [ -n "$name" ] || continue
        [ -r "$ROOT/$src" ] || { warn "missing source $src -- skipping $name"; continue; }
        # examples/dynamic.c includes a header beside itself
        local extra=""
        case "$src" in examples/*) extra="-I$ROOT/examples" ;; esac
        if "$CC_BIN" -O0 -g -o "$BINDIR/$name" "$ROOT/$src" $extra \
                -I"$PMIX/include" -L"$PMIX/lib" -Wl,-rpath,"$PMIX/lib" -lpmix 2>&1 \
           | sed "s/^/    [$name] /"; then
            built=$((built+1))
        else
            warn "failed to build $name"; rc=1
        fi
    done <<< "$CLIENTS"

    # slowcat links nothing: it is a deliberately slow stdin reader, and the
    # point of it is the daemon's write path, not PMIx.
    if "$CC_BIN" -O0 -g -o "$BINDIR/slowcat" "$ROOT/contrib/dockerswarm/slowcat.c"; then
        built=$((built+1))
    else
        warn "failed to build slowcat"; rc=1
    fi
    say "built $built client(s) into $BINDIR"
    return $rc
}

########################################################################
# Check the cluster can host the suite
########################################################################

discover_nodes() {
    {
        if [ -n "$NODES" ]; then
            printf '%s\n' "$NODES" | tr ',' '\n' | tr ' ' '\n'
        elif [ -n "$HOSTFILE" ]; then
            sed -e 's/#.*//' -e 's/[[:space:]].*//' "$HOSTFILE"
        elif [ -n "${SLURM_JOB_NODELIST:-}" ] && command -v scontrol >/dev/null 2>&1; then
            scontrol show hostnames "$SLURM_JOB_NODELIST"
        elif [ -n "${PBS_NODEFILE:-}" ] && [ -r "${PBS_NODEFILE:-}" ]; then
            cat "$PBS_NODEFILE"
        elif [ -n "${LSB_DJOB_HOSTFILE:-}" ] && [ -r "${LSB_DJOB_HOSTFILE:-}" ]; then
            cat "$LSB_DJOB_HOSTFILE"
        elif [ -n "${LSB_HOSTS:-}" ]; then
            printf '%s\n' ${LSB_HOSTS}
        fi
    } 2>/dev/null | awk 'NF && !seen[$1]++ {print $1}'
}

# Run a command on one node.  A node that is THIS machine is driven
# directly -- a cluster that lets you ssh out to the compute nodes does not
# always let you ssh back to yourself -- and the test is against what this
# machine is actually called, not against "is it the head node".  Those are
# different questions the moment you run this from a login node that is not
# in the list at all, and answering the second one there sends every command
# to the wrong machine.
ME_SHORT=$(hostname 2>/dev/null); ME_FQ=$(hostname -f 2>/dev/null)
is_me() {
    case "$1" in "$ME_SHORT"|"$ME_FQ") return 0 ;; esac
    [ "${1%%.*}" = "${ME_SHORT%%.*}" ]
}
on_node() {   # host cmd...
    local h=$1; shift
    if is_me "$h"; then bash -c "$*"; else $RSH "$h" bash -s <<< "$*"; fi
}

check_cluster() {
    local hosts h n=0 rc=0 probe second marker
    hosts=$(discover_nodes)
    if [ -z "$hosts" ]; then
        warn "no node list: pass --nodes/--hostfile, or run inside an allocation."
        warn "Skipping the cluster checks; the clients above are still built."
        return 0
    fi
    HEAD=$(printf '%s\n' "$hosts" | head -1)
    second=$(printf '%s\n' "$hosts" | sed -n 2p)
    say "checking $(printf '%s\n' "$hosts" | wc -l | tr -d ' ') node(s), head $HEAD"

    for h in $hosts; do
        n=$((n+1))
        probe=$(on_node "$h" "command -v prted >/dev/null && echo TOOLS-OK" 2>&1)
        case "$probe" in
            *TOOLS-OK*) printf '    ok   %s\n' "$h" ;;
            *) printf '    FAIL %s: %s\n' "$h" \
                   "$(echo "$probe" | tr '\n' ' ' | tail -c 100)"; rc=1 ;;
        esac
    done
    [ "$rc" = 0 ] || {
        warn "every node needs $PREFIX/bin on its non-interactive PATH."
        warn "The usual fix is to build PRRTE with --enable-prte-prefix-by-default,"
        warn "which makes a daemon be launched by its full path."
    }

    # The scratch directory has to be node-local.  Several cases prove a file
    # crossed the wire by its ABSENCE on the target node; shared, they cannot
    # fail, they simply stop testing anything.
    if [ -n "$second" ]; then
        marker="prte-cluster-setup-probe.$$"
        on_node "$HEAD" "mkdir -p '$WORKDIR' && date > '$WORKDIR/$marker'" >/dev/null 2>&1
        if on_node "$second" "test -e '$WORKDIR/$marker'" >/dev/null 2>&1; then
            warn "$WORKDIR is SHARED between $HEAD and $second."
            warn "The file-staging cases will skip.  Point --workdir (and"
            warn "PRTE_CLUSTER_WORKDIR) at a node-local path to cover them."
        else
            printf '    ok   %s is node-local\n' "$WORKDIR"
        fi
        on_node "$HEAD" "rm -f '$WORKDIR/$marker'" >/dev/null 2>&1
    fi

    # ...and one real launch, which is the only check that means anything.
    say "smoke test: prterun across the first two nodes"
    if [ -n "$second" ]; then
        probe=$(on_node "$HEAD" "export PATH='$PREFIX/bin':\$PATH
            export LD_LIBRARY_PATH='$PREFIX/lib':'$PMIX/lib':\${LD_LIBRARY_PATH:-}
            prterun --host '$HEAD':1,'$second':1 -np 2 --map-by node hostname" 2>&1)
        if [ "$(printf '%s\n' "$probe" | grep -c .)" -ge 2 ]; then
            printf '    ok   %s\n' "$(printf '%s' "$probe" | tr '\n' ' ')"
        else
            printf '    FAIL %s\n' "$(printf '%s' "$probe" | tr '\n' ' ' | tail -c 300)"
            rc=1
        fi
    fi

    echo
    if [ "$rc" = 0 ]; then
        say "ready.  Run the suite with:"
    else
        say "NOT ready -- fix the failures above.  The suite is run with:"
    fi
    # Everything the run needs, including the two the suite could rediscover
    # on its own -- printed anyway, because a run whose log records what it
    # was pointed at can be compared with another one and a run that
    # rediscovered it cannot.
    echo
    echo "    export PRTE_CLUSTER_PREFIX=$PREFIX"
    echo "    export PRTE_CLUSTER_PMIX_PREFIX=$PMIX"
    echo "    export PRTE_CLUSTER_NODES=$(printf '%s' "$hosts" | tr '\n' ',' | sed 's/,$//')"
    echo "    export PRTE_CLUSTER_WORKDIR=$WORKDIR"
    [ "$RSH" = "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new" ] || \
        echo "    export PRTE_CLUSTER_RSH='$RSH'"
    echo "    $ROOT/contrib/dockerswarm/run-tests.sh cluster"
    echo
    return $rc
}

rc=0
[ "$DO_BUILD" = 1 ] && { build_clients || rc=1; }
[ "$DO_CHECK" = 1 ] && { check_cluster || rc=1; }
exit $rc
