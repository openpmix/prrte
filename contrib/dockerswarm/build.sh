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
# Because a VPATH configure refuses to run while the source tree still has an
# in-tree build, this script runs `make distclean` (once) at the repo root and
# then `./autogen.pl`.  After that, ALL builds are out-of-tree and your
# top-level source dir stays clean.
#
# Optional: point PMIX_SRC at a local openpmix checkout to build PMIx from
# source too (covering both code bases); otherwise the baked-in PMIx (Linux) or
# an installed PMIx (macOS, override with PMIX_HOME) is used.
#
# Requires: docker (for linux/image), git, and a working autotools toolchain.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)"

IMAGE="${IMAGE:-prte-swarm:latest}"
VOLUME="${VOLUME:-prte-build}"
PMIX_REF="${PMIX_REF:-master}"          # baked-image PMIx branch
PMIX_REPO="${PMIX_REPO:-https://github.com/openpmix/openpmix.git}"
PMIX_SRC="${PMIX_SRC:-}"                # optional openpmix checkout to build
PMIX_HOME="${PMIX_HOME:-}"              # optional installed PMIx prefix (macOS)

mode="${1:-linux}"

# --- make the source tree VPATH-ready (idempotent) --------------------------
prep_srcdir() {
    if [ -f "$root/config.status" ] || [ -f "$root/Makefile" ]; then
        echo ">>> make distclean (source tree had an in-tree build)"
        make -C "$root" distclean >/dev/null 2>&1 || true
    fi
    if [ ! -x "$root/configure" ] || [ "$root/configure.ac" -nt "$root/configure" ]; then
        echo ">>> autogen.pl"
        ( cd "$root" && ./autogen.pl )
    fi
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
    prep_srcdir
    build_image
    docker volume create "$VOLUME" >/dev/null

    local pmix_mount=()
    [ -n "$PMIX_SRC" ] && pmix_mount=(-v "$(cd "$PMIX_SRC" && pwd)":/pmix-src:ro)

    echo ">>> building PRRTE (and PMIx if PMIX_SRC set) into volume $VOLUME"
    docker run --rm \
        -v "$root":/prrte-src:ro \
        -v "$VOLUME":/opt/prte \
        ${pmix_mount[@]+"${pmix_mount[@]}"} \
        "$IMAGE" bash -euo pipefail -c '
            jobs=$(nproc)
            if [ -d /pmix-src ]; then
                PMIX_PREFIX=/opt/prte/pmix
                echo ">>>> PMIx from bind-mounted /pmix-src -> $PMIX_PREFIX"
                mkdir -p /opt/prte/vpath-linux-pmix && cd /opt/prte/vpath-linux-pmix
                [ -f config.status ] || /pmix-src/configure --prefix="$PMIX_PREFIX"
                make -j"$jobs" && make install
            else
                PMIX_PREFIX=/usr/local
                echo ">>>> PMIx: using baked $PMIX_PREFIX"
            fi

            echo ">>>> PRRTE VPATH build -> /opt/prte/prte"
            mkdir -p /opt/prte/vpath-linux && cd /opt/prte/vpath-linux
            [ -f config.status ] || /prrte-src/configure \
                --prefix=/opt/prte/prte --with-pmix="$PMIX_PREFIX" --enable-debug
            make -j"$jobs" && make install

            echo ">>>> elastic test client"
            gcc -O0 -g -o /opt/prte/prte/bin/elastic \
                /prrte-src/contrib/dockerswarm/elastic.c \
                -I"$PMIX_PREFIX/include" -L"$PMIX_PREFIX/lib" -lpmix

            # runtime env for login shells (node-entrypoint handles ld.so)
            printf "export PATH=/opt/prte/prte/bin:\$PATH\nexport LD_LIBRARY_PATH=/opt/prte/prte/lib:%s/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\n" \
                "$PMIX_PREFIX" > /opt/prte/env.sh
            echo ">>>> done: install in /opt/prte/prte"
        '
    echo ">>> Linux build complete."
    echo ">>> next: docker compose up -d && ./run-tests.sh linux"
}

# --- macOS build (native, on this host) -------------------------------------
build_macos() {
    prep_srcdir
    local pmix_arg=""
    if [ -n "$PMIX_SRC" ]; then
        local psrc pfx
        psrc="$(cd "$PMIX_SRC" && pwd)"
        pfx="$root/vpath-macos-pmix/install"
        echo ">>> PMIx native VPATH build from $psrc -> $pfx"
        ( cd "$psrc" && { [ -x configure ] || ./autogen.pl; } )
        mkdir -p "$root/vpath-macos-pmix" && cd "$root/vpath-macos-pmix"
        [ -f config.status ] || "$psrc/configure" --prefix="$pfx"
        make -j"$(sysctl -n hw.ncpu)" && make install
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
    # shellcheck disable=SC2086
    [ -f config.status ] || "$root/configure" \
        --prefix="$root/vpath-macos/install" $pmix_arg --enable-debug ${EXTRA_CONFIGURE_ARGS:-}
    make -j"$(sysctl -n hw.ncpu)" && make install
    echo ">>> macOS build complete: $root/vpath-macos/install"
    echo ">>> next: ./run-tests.sh macos"
}

case "$mode" in
    linux) build_linux ;;
    macos) build_macos ;;
    image) prep_srcdir; build_image force ;;
    *) echo "usage: $0 [linux|macos|image]" >&2; exit 2 ;;
esac
