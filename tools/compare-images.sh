#!/usr/bin/env bash
#
# Byte-for-byte comparison of two odi-oss image tarballs (the output of image/build.sh,
# output), for the reproducibility check in ci.yml: build the same VERSION
# twice, in two clean workspaces, and prove the results match except for
# the documented build stamps.
#
# Tolerated differences, because each is a stamp the build deliberately
# writes at build time rather than derives from the sources:
#   - etc/version, etc/odi-build   the build timestamp (image/build.sh)
#   - bin/busybox                  busybox compiles its own --help banner
#                                   with __DATE__/__TIME__; two builds a
#                                   second apart never match bit-for-bit
#   - etc/passwd                   only differs under ROOT_PW modes that
#                                   generate a random password; the
#                                   reproducibility job itself always
#                                   builds ROOT_PW=locked (deterministic),
#                                   so this tolerance is a safety net, not
#                                   the expected path
#   - md5.txt                      derived from the members above, so it
#                                   changes whenever they do; comparing it
#                                   would only restate the same finding
# Anything else that differs is a real reproducibility bug: something in
# the build depends on wall-clock time, PID, hostname, filesystem
# iteration order, or another source of non-determinism nobody has
# documented yet.
#
# Usage: tools/compare-images.sh <tar1> <tar2>
set -euo pipefail
TAR1=${1:?usage: compare-images.sh <tar1> <tar2>}
TAR2=${2:?usage: compare-images.sh <tar1> <tar2>}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
DIRA=$WORK/a DIRB=$WORK/b
mkdir -p "$DIRA" "$DIRB"
tar -xf "$TAR1" -C "$DIRA"
tar -xf "$TAR2" -C "$DIRB"

fail=0

members1=$(cd "$DIRA" && find . -maxdepth 1 -type f | sort)
members2=$(cd "$DIRB" && find . -maxdepth 1 -type f | sort)
if [ "$members1" != "$members2" ]; then
	echo "compare-images.sh: member list differs" >&2
	diff <(echo "$members1") <(echo "$members2") >&2 || true
	fail=1
fi

for member in fwu.sh fwu_ver uImage; do
	[ -f "$DIRA/$member" ] && [ -f "$DIRB/$member" ] || continue
	if ! cmp -s "$DIRA/$member" "$DIRB/$member"; then
		echo "compare-images.sh: $member differs between the two builds" >&2
		fail=1
	fi
done

# rootfs is a squashfs image: compare the trees inside it, not the
# compressed bytes (the squashfs metadata ordering mksquashfs itself picks is not at issue
# here; the tolerated-file list above is).
unsquashfs -d "$WORK/rootfs-a" "$DIRA/rootfs" >/dev/null
unsquashfs -d "$WORK/rootfs-b" "$DIRB/rootfs" >/dev/null

rootfs_diff=$(diff -rq "$WORK/rootfs-a" "$WORK/rootfs-b" || true)
unexpected=$(echo "$rootfs_diff" | grep -vE \
	'^Files .*/etc/version and .*/etc/version differ$|^Files .*/etc/odi-build and .*/etc/odi-build differ$|^Files .*/bin/busybox and .*/bin/busybox differ$|^Files .*/etc/passwd and .*/etc/passwd differ$' \
	| sed '/^$/d' || true)
if [ -n "$rootfs_diff" ]; then
	echo "compare-images.sh: rootfs differences (tolerated ones filtered out below the line, if any):" >&2
	echo "$rootfs_diff" >&2
fi
if [ -n "$unexpected" ]; then
	echo "compare-images.sh: rootfs differs outside the documented build stamps:" >&2
	echo "$unexpected" >&2
	fail=1
fi

[ "$fail" = 0 ] && echo "compare-images.sh: identical, modulo the documented build stamps"
exit "$fail"
