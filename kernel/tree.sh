#!/usr/bin/env bash
# The pristine Linux 6.18 tree as one tarball: build/kernel-618/tree.tar.
#
# kernel/build.sh reads it to compile the tree in the prebuilt toolchain
# image. The toolchain itself no longer needs it -- it is a container image
# from odi-toolchain, built against its own kernel headers -- so this step
# is a plain prerequisite of the kernel build now, kept as its own script
# because kernel/build.sh calls it too (it does nothing once the tarball
# exists).
#
# A tarball rather than a bind mount of kernel/618/mainline: macOS collapses
# the pairs of paths in this tree that differ only in case, and a tar does not.
#
# Fetches the tree first (kernel/618/fetch.sh, SHA-256 and GPG checked) when
# kernel/618/mainline is not there yet. Skips everything when the tarball
# exists; FRESH=1 writes it again from the tree.
#
#     kernel/tree.sh [<output tar>]    # default build/kernel-618/tree.tar
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPO="$ROOT/kernel/618/mainline"
OUT=${1:-$ROOT/build/kernel-618/tree.tar}

if [ -s "$OUT" ] && [ "${FRESH:-0}" != 1 ]; then
	exit 0
fi
[ -d "$REPO/arch" ] || "$ROOT/kernel/618/fetch.sh"
[ -d "$REPO/arch" ] || { echo "kernel/tree.sh: no 6.18 tree at $REPO after the fetch" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
# Written by rename: an interrupted tar must not leave a truncated tarball
# that the size check above would take for a finished one.
tar cf "$OUT.part" -C "$REPO" .
mv "$OUT.part" "$OUT"
echo "kernel tree: $OUT ($(du -h "$OUT" | cut -f1))"
