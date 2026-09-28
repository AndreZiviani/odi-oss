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
#   - busybox, and every applet    busybox compiles its own --help banner
#     hardlinked to it             with __DATE__/__TIME__, so the one
#                                   busybox binary never matches bit-for-bit
#                                   between two builds -- and every applet
#                                   name in bin/ and sbin/ is a hardlink to
#                                   that same binary, so they all show as
#                                   differing files for the same one reason.
#                                   Found and identified by the inode they
#                                   share with bin/busybox on each side, not
#                                   by name, so a new applet needs no edit
#                                   here.
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
# sudo unsquashfs below leaves root-owned files under $WORK; a plain rm -rf,
# run as the non-root caller of this script, cannot remove them.
trap 'sudo rm -rf "$WORK"' EXIT
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
# here; the tolerated-file list above is). rootfs/devices.pseudo puts real
# character/block device nodes in the image, which unsquashfs can only
# recreate as an actual superuser -- sudo, not a fallback, on any runner
# where this script runs unprivileged.
sudo unsquashfs -d "$WORK/rootfs-a" "$DIRA/rootfs" >/dev/null
sudo unsquashfs -d "$WORK/rootfs-b" "$DIRB/rootfs" >/dev/null
sudo chmod -R a+rX "$WORK/rootfs-a" "$WORK/rootfs-b"

# Delete busybox and every applet hardlinked to it, on both sides, before
# the tree diff: found by inode, not by name, so this does not need
# updating when the applet list in packages/busybox/build.sh changes.
for side in a b; do
	bb="$WORK/rootfs-$side/bin/busybox"
	[ -f "$bb" ] || continue
	inum=$(stat -c %i "$bb")
	sudo find "$WORK/rootfs-$side" -inum "$inum" -delete
done

# dev/ is checked separately, not by plain diff: every node there is a
# character or block special file (rootfs/devices.pseudo), and GNU diff
# reports two special files as "differ" on sight, whether or not they
# actually match, because it never reads their content. What identifies a
# device node -- name, type, major:minor -- is checked explicitly instead;
# dev/ is then excluded from the general diff below so that harmless "differ"
# noise cannot mask a real difference elsewhere in the same run.
dev_listing() { (cd "$1/dev" && find . | LC_ALL=C sort | xargs -I{} stat --format='%n %F %t:%T' {}); }
dev_diff=$(diff <(dev_listing "$WORK/rootfs-a") <(dev_listing "$WORK/rootfs-b") || true)
if [ -n "$dev_diff" ]; then
	echo "compare-images.sh: dev/ differs (name, type or major:minor -- not tolerated):" >&2
	echo "$dev_diff" >&2
	fail=1
fi

rootfs_diff=$(diff -rq -x dev "$WORK/rootfs-a" "$WORK/rootfs-b" 2>/dev/null || true)
unexpected=$(echo "$rootfs_diff" | grep -vE \
	'^Files .*/etc/version and .*/etc/version differ$|^Files .*/etc/odi-build and .*/etc/odi-build differ$|^Files .*/etc/passwd and .*/etc/passwd differ$' \
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
