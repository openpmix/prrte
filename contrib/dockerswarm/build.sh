#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Build the prte-elastic:latest image used by the DVM test swarm (README.md).
#
# It exports this repo's committed PRRTE tree (default: HEAD) into a clean build
# context via `git archive` -- so the image contains exactly your committed code,
# with no host build artifacts and no architecture mismatch -- then builds the
# Dockerfile, which clones PMIx and compiles both into /usr/local.
#
# Test a different committed state with PRRTE_REF, or a specific PMIx with
# PMIX_REF / PMIX_REPO.  Examples:
#   ./build.sh
#   PRRTE_REF=topic/lnch ./build.sh
#   PMIX_REF=v6.1.0 ./build.sh
#
# Requires: docker, git, and network access during the build (for PMIx + apt).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)"

IMAGE="${IMAGE:-prte-elastic:latest}"
PRRTE_REF="${PRRTE_REF:-HEAD}"
PMIX_REPO="${PMIX_REPO:-https://github.com/openpmix/openpmix.git}"
PMIX_REF="${PMIX_REF:-master}"

ctx="$here/_work/context"
rm -rf "$ctx"
mkdir -p "$ctx"

echo ">>> exporting PRRTE source ($PRRTE_REF) from $root"
git -C "$root" archive --prefix=prrte/ "$PRRTE_REF" | tar -x -C "$ctx"

# git archive omits submodule contents, so add config/oac (needed by autogen's
# m4) separately.  Make sure it is checked out on the host first.
echo ">>> adding config/oac submodule contents"
git -C "$root" submodule update --init -- config/oac >/dev/null 2>&1 || true
git -C "$root/config/oac" archive --prefix=prrte/config/oac/ HEAD | tar -x -C "$ctx"

# Drop .gitmodules so the in-container autogen.pl skips its submodule check,
# which would otherwise run `git submodule status` and fail (no .git here).
rm -f "$ctx/prrte/.gitmodules"

cp "$here/Dockerfile" "$here/elastic.c" "$ctx/"

echo ">>> docker build $IMAGE (PMIx $PMIX_REF)"
docker build -t "$IMAGE" \
    --build-arg PMIX_REPO="$PMIX_REPO" \
    --build-arg PMIX_REF="$PMIX_REF" \
    "$ctx"

rm -rf "$here/_work"
echo ">>> done: built $IMAGE"
echo ">>> next: docker compose up -d   (then see README.md)"
