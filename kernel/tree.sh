#!/usr/bin/env bash
# The pristine Linux 6.18 tree as one tarball: build/kernel-618/tree.tar.
#
# kernel/build.sh reads it to compile the tree in the prebuilt toolchain
# image. The toolchain itself no longer needs it -- it is a container image
# from odi-toolchain, built against its own kernel headers -- so this step
# is a plain prerequisite of the kernel build now, kept as its own script
# because kernel/build.sh calls it too (it does nothing once the tarball
# exists for the pinned release).
#
# A tarball rather than a bind mount of kernel/618/mainline: macOS collapses
# the pairs of paths in this tree that differ only in case, and a tar does not.
#
# Fetches the tree first (kernel/618/fetch.sh, SHA-256 and GPG checked) when
# kernel/618/mainline is not there yet, or holds a different release than
# the VERSION pinned in fetch.sh. Skips everything when the tarball exists
# and its stamp (<tar>.version) names the pinned release; FRESH=1 writes it
# again from the tree. Without the version checks a pin bump was silently
# ignored by any checkout that already had a tree: it kept building the old
# kernel.
#
#     kernel/tree.sh [<output tar>]    # default build/kernel-618/tree.tar
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPO="$ROOT/kernel/618/mainline"
OUT=${1:-$ROOT/build/kernel-618/tree.tar}

WANT=$(sed -n 's/^VERSION=//p' "$ROOT/kernel/618/fetch.sh" | head -n 1)
[ -n "$WANT" ] || { echo "kernel/tree.sh: no VERSION= in kernel/618/fetch.sh" >&2; exit 1; }

# The release a tree holds, from its top-level Makefile (6.18.55 style).
tree_version() {
	awk -F' = ' '$1 == "VERSION" { v = $2 } $1 == "PATCHLEVEL" { p = $2 }
		$1 == "SUBLEVEL" { print v "." p "." $2; exit }' "$1/Makefile" 2>/dev/null || true
}

if [ -s "$OUT" ] && [ "${FRESH:-0}" != 1 ] &&
   [ "$(cat "$OUT.version" 2>/dev/null)" = "$WANT" ]; then
	exit 0
fi
have=$(tree_version "$REPO")
if [ "$have" != "$WANT" ]; then
	[ -z "$have" ] || echo "kernel/tree.sh: $REPO holds linux-$have, fetch.sh pins $WANT: fetching it" >&2
	"$ROOT/kernel/618/fetch.sh"
fi
[ "$(tree_version "$REPO")" = "$WANT" ] ||
	{ echo "kernel/tree.sh: no linux-$WANT tree at $REPO after the fetch" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
# Written by rename: an interrupted tar must not leave a truncated tarball
# that the checks above would take for a finished one. The stamp goes last.
rm -f "$OUT.version"
tar cf "$OUT.part" -C "$REPO" .
mv "$OUT.part" "$OUT"
printf '%s\n' "$WANT" > "$OUT.version"
echo "kernel tree: $OUT, linux-$WANT ($(du -h "$OUT" | cut -f1))"
