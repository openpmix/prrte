#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Build PRRTE for the DVM test swarm (README.md) from your *live* working tree
# -- no commit required, never stale.
#
# The tree is built OUT OF TREE (VPATH) so the source stays pristine and can be
# shared by two independent builds:
#
#   ./build.sh          # or 'linux': build in a container into the shared
#                       #   /opt/prte volume that the swarm nodes mount
#   ./build.sh macos    # build natively on this host into <repo>/vpath-macos
#   ./build.sh image    # (re)build just the base container image
#
# Two clones on one host: set PRTE_SWARM (see below) in the whole shell, and
# each gets its own containers, volume, and network.  The macOS build is
# already per-clone -- it lives in <repo>/vpath-macos.
#
# Distcleaning the source tree
# ----------------------------
# An out-of-tree build cannot share a source tree with an in-tree build: VPATH
# configure refuses to run against one, and (worse) automake sets VPATH=srcdir,
# so an incremental out-of-tree make will happily link objects it finds in the
# SOURCE tree -- which for a Linux container reading a macOS host tree are not
# even the same architecture.  So this script distcleans whenever it finds an
# in-tree build, not just on the first run.
#
# It is tempting to narrow this to "only when the out-of-tree build will have
# to run configure" -- an incremental build has all its own objects, so VPATH
# is never searched for one.  That was tried, and it does not work.  The
# blocker is prte_config.h: src/include/constants.h does #include
# "prte_config.h", and BOTH files live in src/include, so the compiler's
# "directory of the including file first" rule for quoted includes finds the
# SOURCE tree's stale prte_config.h before any -I or -iquote path.  No flag
# ordering can override that, which is why configure.ac putting the build
# tree ahead of the source tree -- correct, and it does fix files that
# include prte_config.h directly -- still leaves this case.  The give-away is
# an "OAC_HAVE_APPLE redefined" error when the two trees were configured for
# different platforms; same-platform, different --with-pmix is the quiet and
# much nastier version.
#
# So the distclean cannot be conditioned on anything cheaper than "is there an
# in-tree build".  The way to stop paying for it is to not keep one -- see
# AGENTS.md, "When a distclean is actually needed".
#
# That costs you the in-tree build: the next `make check`, `make -C test/offline
# check-offline` or `make install` has to reconfigure and rebuild first.  If you
# run those often, keep the host side out of tree too (./build.sh macos) and
# there is nothing to clean.
#
#   --distclean      force it
#   --no-distclean   skip it -- an assertion that the tree really is clean.
#                    If it is not, this script STOPS rather than building a
#                    tree it has just been told to poison.
#
# See AGENTS.md, "When a distclean is actually needed".
#
# Optional: point PMIX_SRC at a local openpmix checkout to build PMIx from
# source too (covering both code bases); otherwise the baked-in PMIx (Linux) or
# an installed PMIx (macOS, override with PMIX_HOME) is used.
#
# Requires: docker (for linux/image), git, and a working autotools toolchain.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)"

# Which swarm this is.  Every global name the harness claims -- the compose
# project, the ten container names, the build volume, the docker network --
# is derived from it, so two clones on one host can each drive their own
# swarm without touching the other's containers, install, or /tmp.  Unset, it
# is "prte" and every name is what it has always been.  Set it for the whole
# shell, because `docker compose` interpolates docker-compose.yml itself:
#
#   export PRTE_SWARM=alt
#   ./build.sh && docker compose up -d && ./run-tests.sh linux
#
# The image is deliberately NOT per-swarm (see docker-compose.yml).
PRTE_SWARM="${PRTE_SWARM:-prte}"
# Rejected here rather than left to docker.  Filtering through LC_ALL=C tr,
# not a shell [a-z] range: in a UTF-8 locale that range follows collation
# order and matches 'B', so the obvious form of this test accepts names
# docker then refuses.  Same reason the leading-character check is a literal
# set.
case "$PRTE_SWARM" in [_-]*) PRTE_SWARM="" ;; esac
if [ -z "$PRTE_SWARM" ] || \
   [ "$PRTE_SWARM" != "$(printf '%s' "$PRTE_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PRTE_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi

IMAGE="${IMAGE:-prte-swarm:latest}"
VOLUME="${VOLUME:-$PRTE_SWARM-build}"
PMIX_REF="${PMIX_REF:-master}"          # baked-image PMIx branch
PMIX_REPO="${PMIX_REPO:-https://github.com/openpmix/openpmix.git}"
PMIX_SRC="${PMIX_SRC:-}"                # optional openpmix checkout to build
PMIX_HOME="${PMIX_HOME:-}"              # optional installed PMIx prefix (macOS)

mode=linux
distclean=auto                          # auto | always | never
for arg in "$@"; do
    case "$arg" in
        linux|macos|image) mode="$arg" ;;
        --distclean)       distclean=always ;;
        --no-distclean)    distclean=never ;;
        *) echo "usage: $0 [linux|macos|image] [--distclean|--no-distclean]" >&2; exit 2 ;;
    esac
done

# Does the source tree hold an in-tree build?  config.status/Makefile are what
# make a VPATH configure refuse to run, but the object files are the bigger
# problem -- see prep_srcdir.
srcdir_has_intree() {
    [ -f "$root/config.status" ] || [ -f "$root/Makefile" ] || \
    [ -n "$(find "$root/src" -name '*.lo' -print -quit 2>/dev/null)" ]
}

# --- generate the flex output the builder cannot generate itself ------------
# In a pristine checkout a *.l has no companion *.c: automake produces it at
# build time, and ylwrap writes it into the SOURCE directory next to the .l --
# but the builder mounts both source trees READ-ONLY, so the build dies with
#
#     config/ylwrap: line 204: .../keyval_lex.c: Read-only file system
#
# A tree that has been built in place at least once already carries the file,
# which is why this only ever bites a *fresh* clone -- precisely what PMIX_SRC
# usually points at.  Generate it here on the host the way automake would; the
# -P symbol prefix lives in the sibling Makefile.am AM_LFLAGS.  These are
# git-ignored maintainer files, so writing them into the checkout is exactly
# what building it normally does.
gen_lex() {
    local tree="$1" lfile cfile prefix
    while IFS= read -r lfile; do
        [ -n "$lfile" ] || continue
        cfile="${lfile%.l}.c"
        if [ -f "$cfile" ]; then
            continue
        fi
        if ! command -v flex >/dev/null 2>&1; then
            echo ">>> WARNING: $lfile has no generated .c and flex is not" \
                 "installed; the read-only builder cannot make one"
            return 0
        fi
        prefix="$(sed -n 's/^AM_LFLAGS *= *-P//p' "$(dirname "$lfile")/Makefile.am" \
                  2>/dev/null | head -1)"
        echo ">>> flex $lfile (the builder mounts this tree read-only)"
        if [ -n "$prefix" ]; then
            flex "-P$prefix" -o "$cfile" "$lfile"
        else
            flex -o "$cfile" "$lfile"
        fi
    done <<EOF
$(find "$tree" -name '*.l' 2>/dev/null)
EOF
}

# --- make the source tree VPATH-ready (idempotent) --------------------------
prep_srcdir() {
    if srcdir_has_intree && [ "$distclean" != never ]; then
        # Two separate reasons, and the second is the one that bites:
        #
        #  1. a VPATH configure refuses to run at all while the source tree
        #     holds an in-tree build; and
        #  2. even when configure is skipped (incremental rebuild), automake
        #     sets VPATH = srcdir, so make happily resolves an object target
        #     out of the SOURCE tree.  An in-tree *.lo / .libs/*.o left there
        #     is then linked into the out-of-tree build -- and for the Linux
        #     container against a macOS host tree those objects are not even
        #     the same architecture, so the build dies with
        #     "'foo.lo' is not a valid libtool object".
        #
        # So this is not a first-run-only cost: any in-tree build present when
        # an out-of-tree build runs has to go.  The durable fix is to stop
        # keeping one -- see AGENTS.md, "When a distclean is actually needed".
        echo ">>> make distclean -- the source tree holds an in-tree build," \
             "which would poison the out-of-tree build via VPATH"
        echo ">>> NOTE: this removes your in-tree build; reconfigure before" \
             "'make check' or 'make -C test/offline check-offline'"
        echo ">>>       (or build the host side out-of-tree too: ./build.sh macos)"
        make -C "$root" distclean >/dev/null 2>&1 || true
        # distclean leaves the *.lo behind if the tree was already half-cleaned
        find "$root/src" -name '*.lo' -delete 2>/dev/null || true
        find "$root/src" -name '.libs' -type d -exec rm -rf {} + 2>/dev/null || true
    elif srcdir_has_intree; then
        # Refuse rather than warn.  --no-distclean asserts "the tree really is
        # clean"; it is not, so the assertion is false and proceeding builds
        # exactly the poisoned tree everything above exists to prevent.  The
        # cheap failure is stopping here.  The expensive one is a build that
        # succeeds -- same platform, different --with-pmix -- and quietly
        # tests a library nobody chose, with nothing in the output to say so.
        # A warning does not stop that, because the run continues and the next
        # thing anyone reads is the test results.
        echo ">>> ERROR: --no-distclean, but the source tree holds an in-tree" \
             "build." >&2
        echo ">>>        VPATH=srcdir means this build would link objects out" \
             "of the SOURCE tree, and pick up its stale prte_config.h ahead of" \
             "the one configure just wrote." >&2
        echo ">>>        Drop --no-distclean, or clean the tree yourself" \
             "(make distclean at $root)." >&2
        exit 2
    fi

    if [ ! -x "$root/configure" ] || [ "$root/configure.ac" -nt "$root/configure" ]; then
        echo ">>> autogen.pl"
        ( cd "$root" && ./autogen.pl )
    fi

    gen_lex "$root"
}

# --- (re)build the base image if needed -------------------------------------
build_image() {
    if [ "${1:-}" = force ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo ">>> docker build $IMAGE (baked PMIx $PMIX_REF)"
        docker build -t "$IMAGE" \
            --build-arg PMIX_REPO="$PMIX_REPO" \
            --build-arg PMIX_REF="$PMIX_REF" \
            "$here"
    else
        echo ">>> using existing image $IMAGE (./build.sh image to rebuild)"
    fi
}

# --- Linux build (in a builder container, into the shared volume) -----------
build_linux() {
    # image and volume first: prep_srcdir asks them whether the out-of-tree
    # build has already been configured before deciding to distclean
    build_image
    docker volume create "$VOLUME" >/dev/null
    prep_srcdir linux

    local pmix_mount=()
    if [ -n "$PMIX_SRC" ]; then
        gen_lex "$(cd "$PMIX_SRC" && pwd)"
        pmix_mount=(-v "$(cd "$PMIX_SRC" && pwd)":/pmix-src:ro)
    fi

    echo ">>> building PRRTE (and PMIx if PMIX_SRC set) into volume $VOLUME"
    docker run --rm \
        -v "$root":/prrte-src:ro \
        -v "$VOLUME":/opt/prte \
        ${pmix_mount[@]+"${pmix_mount[@]}"} \
        "$IMAGE" bash -euo pipefail -c '
            jobs=$(nproc)

            # These VPATH build dirs live in the volume and persist across
            # runs, so "configure only if there is no config.status" quietly
            # reuses the arguments of whatever run created them.  Point
            # PMIX_SRC at a checkout after a plain build and PRRTE keeps the
            # baked PMIx, with nothing in the output to say so -- a build
            # that looks like it tested your change and did not.
            #
            # $1 = build dir, $2 = argument string, $3 = srcdir
            #
            # Three ways a persistent build dir can be stale:
            #   - never configured at all;
            #   - configured with different arguments (the PMIX_SRC trap
            #     above);
            #   - configured before the build system was regenerated.
            #     Editing configure.ac or a config/*.m4 and re-running
            #     autogen.pl leaves configure and Makefile.in newer than the
            #     config.status here, and an incremental make then walks into
            #     maintainer-mode regeneration INSIDE the container -- which
            #     needs the exact aclocal/automake version the host used, and
            #     dies with "aclocal-N.NN: command not found", leaving the
            #     PREVIOUS install standing with no build stamp.
            reconfigure_needed() {
                [ -f "$1/config.status" ] || return 0
                [ -f "$1/.configure-args" ] || return 0
                [ "$(cat "$1/.configure-args")" = "$2" ] || return 0
                [ "$3/configure" -nt "$1/config.status" ] && return 0
                return 1
            }

            if [ -d /pmix-src ]; then
                PMIX_PREFIX=/opt/prte/pmix
                echo ">>>> PMIx from bind-mounted /pmix-src -> $PMIX_PREFIX"
                mkdir -p /opt/prte/vpath-linux-pmix && cd /opt/prte/vpath-linux-pmix
                pmix_args="--prefix=$PMIX_PREFIX"
                if reconfigure_needed . "$pmix_args" /pmix-src; then
                    echo ">>>> (re)configuring PMIx: $pmix_args"
                    /pmix-src/configure $pmix_args
                    echo "$pmix_args" > .configure-args
                fi
                make -j"$jobs"
                make install
            else
                PMIX_PREFIX=/usr/local
                echo ">>>> PMIx: using baked $PMIX_PREFIX"
            fi

            # invalidate the freshness stamp up front: from here until the
            # build finishes, whatever is installed is NOT this source tree
            rm -f /opt/prte/.build-stamp

            echo ">>>> PRRTE VPATH build -> /opt/prte/prte"
            mkdir -p /opt/prte/vpath-linux && cd /opt/prte/vpath-linux
            # --with-jansson is deliberate: it is off by default, and without
            # it ras/slurm compiles ras_slurm_jansson_stub.c instead of the
            # real ras_slurm_jansson.c -- so the ~1000 lines that parse
            # "scontrol --json" (the whole elastic extend/release surface) are
            # never even compiled.  libjansson-dev is baked into the image.
            prte_args="--prefix=/opt/prte/prte --with-pmix=$PMIX_PREFIX --with-jansson --enable-debug"
            if reconfigure_needed . "$prte_args" /prrte-src; then
                echo ">>>> (re)configuring PRRTE: $prte_args"
                /prrte-src/configure $prte_args
                echo "$prte_args" > .configure-args
            fi
            # show_help GOLDEN RULE: prte_show_help_content.c embeds every
            # help-*.txt in the tree, but its make rule depends only on the
            # converter script -- so an edited help file is NOT picked up by an
            # incremental build, and the daemons serve stale (or missing) text
            # while the .txt looks correct.  This build dir persists in the
            # volume across runs, so drop the generated file every time.
            # The .deps entry has to go too.  This file is generated, and if
            # the SOURCE tree happened to hold a copy when an earlier build
            # ran (an in-tree build between swarm runs is enough), VPATH
            # resolved it there and the recorded prerequisite is the srcdir
            # path.  The next distclean removes that copy and make then stops
            # with "No rule to make target
            # '/prrte-src/src/util/prte_show_help_content.c'" -- a build dir
            # poisoned by a file that no longer exists.
            rm -f src/util/prte_show_help_content.c src/util/prte_show_help_content.lo \
                  src/util/.deps/prte_show_help_content.Plo
            make -j"$jobs"
            make install

            # NOTE the -Wl,-rpath on every helper below.  An application
            # launched onto a NON-head node gets an EMPTY LD_LIBRARY_PATH --
            # the head node inherits the login shell that sourced env.sh, the
            # others do not -- so without an rpath these binaries load the
            # PMIx baked into the image (/usr/local/lib) instead of the one
            # this script just built.  That is silent: same soname, same
            # version, different code.  Anything testing a PMIx change would
            # pass on node1 and quietly test the wrong library everywhere
            # else.
            echo ">>>> elastic test client"
            gcc -O0 -g -o /opt/prte/prte/bin/elastic \
                /prrte-src/contrib/dockerswarm/elastic.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # jobinfo: a bare PMIx client used to drive the direct-modex
            # paths in the daemon.  A job-level (wildcard) Get of data
            # belonging to ANOTHER job is answered by the daemon itself, and
            # is only reachable when the asking client sits on a daemon that
            # hosts none of the procs of the target job -- i.e. never on one
            # node.  (No apostrophes here: see the note further down.)
            echo ">>>> jobinfo (direct-modex) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/jobinfo \
                /prrte-src/contrib/dockerswarm/jobinfo.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # dataserver: a bare PMIx client for the publish/lookup service
            # in src/runtime/data_server.  The store is a single array on
            # the HNP and every client reaches it through its own daemon, so
            # the publisher proxy vs requestor proxy distinction that
            # PMIX_RANGE_LOCAL turns on only exists across nodes.
            # (No apostrophes here: see the note further down.)
            echo ">>>> dataserver (publish/lookup) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/dataserver \
                /prrte-src/contrib/dockerswarm/dataserver.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # proctable: a bare PMIx client that queries the proc table and
            # the server URI.  Those two queries are the only callers of
            # prte_pmix_convert_state(), and the local-vs-global proc table
            # split plus a hostname-qualified server URI have no meaning at
            # all on a single host.
            # (No apostrophes here: see the note further down.)
            echo ">>>> proctable (query/state translation) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/proctable \
                /prrte-src/contrib/dockerswarm/proctable.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # groupcon: a bare PMIx client that drives a group construct and
            # destruct.  The interesting half of a group collective runs in
            # grp_release on EVERY daemon: it registers the result with its
            # own PMIx server and only then releases the local participants.
            # On one host there is one daemon and it is also the HNP, so the
            # down-tree release and the per-daemon registration are never
            # exercised against a daemon that merely received the broadcast.
            # (No apostrophes here: see the note further down.)
            echo ">>>> groupcon (group construct/destruct) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/groupcon \
                /prrte-src/contrib/dockerswarm/groupcon.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # faulty: a PMIx client that fails on purpose, once per way the
            # errmgr has a policy branch for (abort, non-zero exit, signal,
            # exit without finalize).  Both errmgr components are involved in
            # every one of those -- the prted that owns the proc classifies
            # and reports it, the HNP decides the fate of the job and whether
            # the survivors are notified -- and on one host there is only one
            # process playing both roles, so the report never crosses a wire.
            # (No apostrophes here: see the note further down.)
            echo ">>>> faulty (deliberate process failures) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/faulty \
                /prrte-src/contrib/dockerswarm/faulty.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # slowcat: a deliberately slow stdin reader (no PMIx at all).  The
            # stdin bugs in iof live in the back-pressure path, and a normal
            # "cat" drains its pipe as fast as the daemon fills it, so the
            # daemon short-write path is never taken.  slowcat keeps the pipe
            # saturated so every write the daemon attempts is a partial one.
            # (No apostrophes here: see the note further down.)
            echo ">>>> slowcat (slow stdin reader) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/slowcat \
                /prrte-src/contrib/dockerswarm/slowcat.c

            # examples/dynamic.c is the PMIx_Spawn example shipped in this
            # tree: rank 0 spawns "client" from its cwd as a CHILD JOB.  It
            # is the only way this harness can produce a parent/child job
            # pair, which is what the report-child-jobs-separately test
            # needs -- that policy decides whether a CHILD job exit status
            # reaches the launcher, and no single-job test can show it.
            # NOTE: this whole block is inside bash -c '...', so an
            # apostrophe anywhere in it (even in a comment) ends the script.
            echo ">>>> dynamic (PMIx_Spawn) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/dynamic \
                /prrte-src/examples/dynamic.c -I/prrte-src/examples \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # sessionctrl: the PMIx_Session_control example shipped in this
            # tree.  Session control is a DVM-master operation whose whole
            # point is a set of nodes, so the parts that matter -- carving a
            # reservation out of the pool, a request relayed from a NON-master
            # daemon, a signal reaching the jobs on every node of the session
            # -- do not exist on one host.
            # NOTE: this whole block is inside bash -c ..., so an apostrophe
            # anywhere in it (even in a comment) ends the script.
            echo ">>>> sessionctrl (PMIx_Session_control) test client"
            gcc -O0 -g -o /opt/prte/prte/bin/sessionctrl \
                /prrte-src/examples/sessionctrl.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # The fake SLURM control plane, installed under its own prefix --
            # NOT into the install bin/, which the node entrypoint symlinks
            # onto the default PATH of every node. ras/slurm gates on SLURM_JOBID,
            # so a stray scontrol on PATH is harmless, but a test that has to
            # opt in by prepending this directory is the one that cannot
            # perturb any other test in the suite.
            echo ">>>> fake SLURM stubs -> /opt/prte/fakeslurm/bin"
            mkdir -p /opt/prte/fakeslurm/bin
            install -m 0755 /prrte-src/contrib/dockerswarm/fake-slurm.py \
                /opt/prte/fakeslurm/bin/fake-slurm
            for t in sbatch scontrol scancel; do
                ln -sf fake-slurm /opt/prte/fakeslurm/bin/$t
            done

            # runtime env for login shells (node-entrypoint handles ld.so)
            printf "export PATH=/opt/prte/prte/bin:\$PATH\nexport LD_LIBRARY_PATH=/opt/prte/prte/lib:%s/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\n" \
                "$PMIX_PREFIX" > /opt/prte/env.sh

            # Written LAST, and only on success.  The install lives in a
            # volume that outlives any one build, so a build that fails
            # part-way -- configure rejecting the baked PMIx is the usual way
            # -- leaves the PREVIOUS install standing, and run-tests.sh will
            # happily exercise it and report on code that was never built.
            # The stamp is what lets the suite say so.  It is removed first so
            # a build that dies midway cannot leave a stale one behind.
            date -u +%Y-%m-%dT%H:%M:%SZ > /opt/prte/.build-stamp
            echo ">>>> done: install in /opt/prte/prte"
        '
    echo ">>> Linux build complete."
    if [ "$PRTE_SWARM" = prte ]; then
        echo ">>> next: docker compose up -d && ./run-tests.sh linux"
    else
        # Say it with the variable: compose reads PRTE_SWARM from the
        # environment of the compose command, and a `docker compose up -d`
        # without it brings up the DEFAULT swarm against the default volume,
        # leaving this build sitting in $VOLUME unused.
        echo ">>> next: PRTE_SWARM=$PRTE_SWARM docker compose up -d &&" \
             "PRTE_SWARM=$PRTE_SWARM ./run-tests.sh linux"
        echo ">>>       (swarm '$PRTE_SWARM': containers $PRTE_SWARM-node1..10, volume $VOLUME)"
    fi
}

# --- macOS build (native, on this host) -------------------------------------
build_macos() {
    prep_srcdir macos
    local pmix_arg=""
    if [ -n "$PMIX_SRC" ]; then
        local psrc pfx
        psrc="$(cd "$PMIX_SRC" && pwd)"
        pfx="$root/vpath-macos-pmix/install"
        echo ">>> PMIx native VPATH build from $psrc -> $pfx"
        ( cd "$psrc" && { [ -x configure ] || ./autogen.pl; } )
        mkdir -p "$root/vpath-macos-pmix" && cd "$root/vpath-macos-pmix"
        [ -f config.status ] || "$psrc/configure" --prefix="$pfx"
        make -j"$(sysctl -n hw.ncpu)"
        make install
        pmix_arg="--with-pmix=$pfx"
    elif [ -n "$PMIX_HOME" ]; then
        pmix_arg="--with-pmix=$PMIX_HOME"
    else
        echo ">>> PMIX_SRC/PMIX_HOME unset; letting configure autodetect PMIx"
    fi

    echo ">>> PRRTE native VPATH build -> $root/vpath-macos/install"
    mkdir -p "$root/vpath-macos" && cd "$root/vpath-macos"
    # EXTRA_CONFIGURE_ARGS lets you pass host-specific dep paths, e.g.
    #   EXTRA_CONFIGURE_ARGS="--with-libevent=... --with-hwloc=..."
    # (values with spaces, like an -isysroot in CFLAGS, should be exported as
    # CFLAGS/CPPFLAGS in the environment instead -- configure inherits them.)
    #
    # Reconfigure whenever those arguments change. An incremental build that
    # silently keeps an older configuration is worse than a slow one: PRRTE
    # uses PMIx internals, so a tree left pointing at a different PMIx than
    # you asked for builds cleanly and then dies at startup, which reads as
    # "this host is flaky" rather than "you built against the wrong PMIx".
    local cfg_args="--prefix=$root/vpath-macos/install $pmix_arg --enable-debug ${EXTRA_CONFIGURE_ARGS:-}"
    if [ -f config.status ] && [ "$(cat .prte-configure-args 2>/dev/null)" != "$cfg_args" ]; then
        echo ">>> configure arguments changed - reconfiguring"
        rm -f config.status
    fi
    # shellcheck disable=SC2086
    if [ ! -f config.status ]; then
        "$root/configure" $cfg_args
        printf '%s' "$cfg_args" > .prte-configure-args
    fi
    # show_help GOLDEN RULE -- see the note in build_linux
    rm -f src/util/prte_show_help_content.c src/util/prte_show_help_content.lo
    make -j"$(sysctl -n hw.ncpu)"
    make install
    echo ">>> macOS build complete: $root/vpath-macos/install"
    # Report what the tools actually resolved against. A mismatch here is the
    # first thing to check when the macOS suite starts skipping everything.
    if command -v otool >/dev/null 2>&1; then
        echo ">>> linked dependencies:"
        otool -L "$root/vpath-macos/install/bin/prted" 2>/dev/null \
            | grep -iE "libpmix|libevent|libhwloc" | sed 's/^/>>>>   /'
    fi
    echo ">>> next: ./run-tests.sh macos"
}

case "$mode" in
    linux) build_linux ;;
    macos) build_macos ;;
    # the image is built from contrib/dockerswarm alone and never reads the
    # source tree, so it needs no distclean at all
    image) build_image force ;;
esac
