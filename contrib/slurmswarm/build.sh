#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Build PRRTE for the real-SLURM test cluster (AGENTS.md) from your *live*
# working tree -- no commit required, never stale.
#
#   ./build.sh          # or 'linux': build in a container into the shared
#                       #   /opt/prte volume that the cluster nodes mount
#   ./build.sh image    # (re)build just the base container image
#
# This is contrib/dockerswarm/build.sh with a different image, a different
# volume, and two extra configure arguments.  It has no 'macos' mode: the
# whole subject here is a scheduler that does not run on a Mac.  For the
# native host build, use the sibling harness -- `../dockerswarm/build.sh
# macos` builds the same source tree, out of tree, into <repo>/vpath-macos.
#
# Two clones on one host: set PRTE_SLURM_SWARM (see below) in the whole shell,
# and each gets its own containers, volume, and network.
#
# Distcleaning the source tree
# ----------------------------
# An out-of-tree build cannot share a source tree with an in-tree build: VPATH
# configure refuses to run against one, and (worse) automake sets VPATH=srcdir,
# so an incremental out-of-tree make will happily link objects it finds in the
# SOURCE tree -- which for a Linux container reading a macOS host tree are not
# even the same architecture.  So this script distcleans whenever it finds an
# in-tree build, not just on the first run.  The full reasoning, including why
# it cannot be narrowed to "only when configure has to run", is in
# ../dockerswarm/AGENTS.md, "When a distclean is actually needed" -- it is one
# source tree and the rule is identical for both harnesses.
#
#   --distclean      force it
#   --no-distclean   skip it -- an assertion that the tree really is clean.
#                    If it is not, this script STOPS rather than building a
#                    tree it has just been told to poison.
#
# NOTE that this harness and contrib/dockerswarm build the SAME source tree
# into two different volumes.  That is supported and is the point of building
# out of tree -- but they each hold their own VPATH directory with its own
# configure arguments, so running one after the other rebuilds nothing in the
# other's volume.  What they must not do is fight over an in-tree build; see
# above, and do not keep one.
#
# Optional: point PMIX_SRC at a local openpmix checkout to build PMIx from
# source too (covering both code bases); otherwise the baked-in PMIx is used.
#
# Requires: docker, git, and a working autotools toolchain.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)"

# Which cluster this is.  Every global name the harness claims -- the compose
# project, the ten container names, the build volume, the docker network -- is
# derived from it, so two clones on one host can each drive their own cluster
# without touching the other's containers, install, or SLURM.  Unset, it is
# "prteslurm".  Set it for the whole shell, because `docker compose`
# interpolates docker-compose.yml itself:
#
#   export PRTE_SLURM_SWARM=alt
#   ./build.sh && docker compose up -d && ./run-tests.sh
#
# Deliberately NOT $PRTE_SWARM: this harness and contrib/dockerswarm are meant
# to be drivable from one shell, and sharing the variable would point both at
# the same ten container names.  The image is deliberately not per-cluster
# (see docker-compose.yml).
PRTE_SLURM_SWARM="${PRTE_SLURM_SWARM:-prteslurm}"
# Rejected here rather than left to docker.  Filtering through LC_ALL=C tr,
# not a shell [a-z] range: in a UTF-8 locale that range follows collation
# order and matches 'B', so the obvious form of this test accepts names
# docker then refuses.  Same reason the leading-character check is a literal
# set.
case "$PRTE_SLURM_SWARM" in [_-]*) PRTE_SLURM_SWARM="" ;; esac
if [ -z "$PRTE_SLURM_SWARM" ] || \
   [ "$PRTE_SLURM_SWARM" != "$(printf '%s' "$PRTE_SLURM_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PRTE_SLURM_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi

IMAGE="${IMAGE:-prte-slurm-swarm:latest}"
VOLUME="${VOLUME:-$PRTE_SLURM_SWARM-build}"
PMIX_REF="${PMIX_REF:-master}"          # baked-image PMIx branch
PMIX_REPO="${PMIX_REPO:-https://github.com/openpmix/openpmix.git}"
PMIX_SRC="${PMIX_SRC:-}"                # optional openpmix checkout to build

mode=linux
distclean=auto                          # auto | always | never
for arg in "$@"; do
    case "$arg" in
        linux|image)    mode="$arg" ;;
        --distclean)    distclean=always ;;
        --no-distclean) distclean=never ;;
        *) echo "usage: $0 [linux|image] [--distclean|--no-distclean]" >&2; exit 2 ;;
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
# -P symbol prefix lives in the sibling Makefile.am AM_LFLAGS.
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
        echo ">>> make distclean -- the source tree holds an in-tree build," \
             "which would poison the out-of-tree build via VPATH"
        echo ">>> NOTE: this removes your in-tree build; reconfigure before" \
             "'make check' or 'make -C test/offline check-offline'"
        echo ">>>       (or build the host side out-of-tree: ../dockerswarm/build.sh macos)"
        make -C "$root" distclean >/dev/null 2>&1 || true
        find "$root/src" -name '*.lo' -delete 2>/dev/null || true
        find "$root/src" -name '.libs' -type d -exec rm -rf {} + 2>/dev/null || true
    elif srcdir_has_intree; then
        # Refuse rather than warn: --no-distclean asserts "the tree really is
        # clean", it is not, and proceeding builds exactly the poisoned tree
        # the check exists to prevent.  The expensive failure is the one that
        # SUCCEEDS -- same platform, different --with-pmix -- and quietly
        # tests a library nobody chose.
        echo ">>> ERROR: --no-distclean, but the source tree holds an in-tree" \
             "build." >&2
        echo ">>>        VPATH=srcdir means this build would link objects out" \
             "of the SOURCE tree, and pick up its stale prte_config.h ahead of" \
             "the one configure just wrote." >&2
        echo ">>>        Drop --no-distclean, or clean the tree yourself" \
             "(make distclean at $root)." >&2
        exit 2
    fi

    # configure.ac is not the only input.  Every config/*.m4 is one too, and so
    # is config/oac -- a submodule, so an ordinary `git submodule update` (or a
    # merge that moves the pointer) rewrites m4 files with no other trace.  A
    # source tree stale that way is not a slow build, it is a dead one: PRRTE
    # builds in maintainer mode, so `make` inside the container tries to
    # regenerate aclocal.m4 itself and dies on
    #
    #     config/missing: line 85: aclocal-1.18: command not found
    #
    # because the container does not have the exact autotools the host used.
    # build.sh then stops with the previous install still in the volume, and
    # nothing in that message mentions a submodule.  Regenerate on the host,
    # where the right autotools are, whenever any input is newer.
    autogen_needed() {
        [ -x "$root/configure" ] || return 0
        [ "$root/configure.ac" -nt "$root/configure" ] && return 0
        [ -n "$(find "$root/config" -name '*.m4' -newer "$root/configure" \
                     -print -quit 2>/dev/null)" ] && return 0
        return 1
    }

    if autogen_needed; then
        echo ">>> autogen.pl"
        ( cd "$root" && ./autogen.pl )
    fi

    gen_lex "$root"
}

# --- (re)build the base image if needed -------------------------------------
build_image() {
    if [ "${1:-}" = force ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo ">>> docker build $IMAGE (SLURM from the distribution, baked PMIx $PMIX_REF)"
        docker build -t "$IMAGE" \
            --build-arg PMIX_REPO="$PMIX_REPO" \
            --build-arg PMIX_REF="$PMIX_REF" \
            "$here"
    else
        echo ">>> using existing image $IMAGE (./build.sh image to rebuild)"
    fi
}

# --- the build (in a builder container, into the shared volume) -------------
build_linux() {
    build_image
    docker volume create "$VOLUME" >/dev/null
    prep_srcdir

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
            # reuses the arguments of whatever run created them.  Three ways
            # a persistent build dir goes stale: never configured; configured
            # with different arguments (point PMIX_SRC at a checkout after a
            # plain build and PRRTE otherwise keeps the baked PMIx, with
            # nothing in the output to say so); or configured before the
            # build system was regenerated, which sends an incremental make
            # into maintainer-mode regeneration INSIDE the container and dies
            # with "aclocal-N.NN: command not found".
            #
            # $1 = build dir, $2 = argument string, $3 = srcdir
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
            # --with-slurm is redundant on Linux (the components build by
            # default there) and is passed anyway so that a build for THIS
            # harness fails loudly if slurm support is ever made conditional
            # on something this image does not have -- a silently missing
            # plm/slurm would leave the whole suite launching over ssh and
            # passing.
            #
            # --with-jansson is not redundant: it is off by default, and
            # without it ras/slurm compiles ras_slurm_jansson_stub.c instead
            # of the real parser -- so the entire "scontrol show job --json"
            # surface, which is what the elastic phases here exercise against
            # a real scheduler, is not even compiled.
            prte_args="--prefix=/opt/prte/prte --with-pmix=$PMIX_PREFIX --with-slurm --with-jansson --enable-debug"
            if reconfigure_needed . "$prte_args" /prrte-src; then
                echo ">>>> (re)configuring PRRTE: $prte_args"
                /prrte-src/configure $prte_args
                echo "$prte_args" > .configure-args
            fi
            # show_help GOLDEN RULE: prte_show_help_content.c embeds every
            # help-*.txt in the tree, but its make rule depends only on the
            # converter script -- so an edited help file is NOT picked up by
            # an incremental build, and the daemons serve stale (or missing)
            # text while the .txt looks correct.  This build dir persists in
            # the volume across runs, so drop the generated file every time.
            # The .deps entry has to go too: if the SOURCE tree held a copy
            # when an earlier build ran, VPATH resolved it there and the
            # recorded prerequisite is the srcdir path, which a later
            # distclean removes.
            rm -f src/util/prte_show_help_content.c src/util/prte_show_help_content.lo \
                  src/util/.deps/prte_show_help_content.Plo
            make -j"$jobs"
            make install

            # The elastic client, borrowed from the sibling harness rather
            # than copied: it is the same program, and the alternative is two
            # of them drifting apart.  It is the only probe this suite needs
            # that PRRTE does not install -- everything else here is driven
            # through prun/prte and the SLURM commands themselves.
            #
            # NOTE the -Wl,-rpath.  An application launched onto a NON-head
            # node gets an EMPTY LD_LIBRARY_PATH -- the head node inherits
            # the login shell that sourced env.sh, the others do not -- so
            # without an rpath this binary loads the PMIx baked into the
            # image (/usr/local/lib) instead of the one this script just
            # built.  That is silent: same soname, same version, different
            # code.
            # NOTE: this whole block is inside bash -c ..., so an apostrophe
            # anywhere in it (even in a comment) ends the script.
            echo ">>>> elastic test client (from ../dockerswarm)"
            gcc -O0 -g -o /opt/prte/prte/bin/elastic \
                /prrte-src/contrib/dockerswarm/elastic.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -Wl,-rpath,"$PMIX_PREFIX/lib" -lpmix

            # The recording wrapper around salloc/scontrol/scancel.  It goes
            # under its OWN prefix rather than into the install bin/ that the
            # node entrypoint puts on every PATH: a case has to opt in by
            # putting this directory first, so nothing else in the suite can
            # be perturbed by it.  It dispatches on argv[0], hence the links.
            echo ">>>> slurm shim -> /opt/prte/slurmshim/bin"
            mkdir -p /opt/prte/slurmshim/bin
            install -m 0755 /prrte-src/contrib/slurmswarm/slurm-shim.py \
                /opt/prte/slurmshim/bin/slurm-shim
            for t in salloc scontrol scancel; do
                ln -sf slurm-shim /opt/prte/slurmshim/bin/$t
            done

            # runtime env for login shells (node-entrypoint handles ld.so)
            printf "export PATH=/opt/prte/prte/bin:\$PATH\nexport LD_LIBRARY_PATH=/opt/prte/prte/lib:%s/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\n" \
                "$PMIX_PREFIX" > /opt/prte/env.sh

            # Written LAST, and only on success.  The install lives in a
            # volume that outlives any one build, so a build that fails
            # part-way leaves the PREVIOUS install standing, and run-tests.sh
            # would happily exercise it and report on code that was never
            # built.  The stamp is what lets the suite refuse.
            date -u +%Y-%m-%dT%H:%M:%SZ > /opt/prte/.build-stamp
            echo ">>>> done: install in /opt/prte/prte"
        '
    echo ">>> build complete."
    if [ "$PRTE_SLURM_SWARM" = prteslurm ]; then
        echo ">>> next: docker compose up -d && ./run-tests.sh"
    else
        # Say it with the variable: compose reads PRTE_SLURM_SWARM from the
        # environment of the compose command, and a `docker compose up -d`
        # without it brings up the DEFAULT cluster against the default
        # volume, leaving this build sitting in $VOLUME unused.
        echo ">>> next: PRTE_SLURM_SWARM=$PRTE_SLURM_SWARM docker compose up -d &&" \
             "PRTE_SLURM_SWARM=$PRTE_SLURM_SWARM ./run-tests.sh"
        echo ">>>       (cluster '$PRTE_SLURM_SWARM': containers" \
             "$PRTE_SLURM_SWARM-node1..10, volume $VOLUME)"
    fi
}

case "$mode" in
    linux) build_linux ;;
    # the image is built from this directory alone and never reads the source
    # tree, so it needs no distclean at all
    image) build_image force ;;
esac
