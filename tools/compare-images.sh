#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# Byte-for-byte comparison of two odi-oss image tarballs (the output of
# image/build.sh), for the reproducibility check in ci.yml: build the same
# commit twice, in two clean workspaces, and prove the tarballs are identical.
#
# Nothing is excluded or normalised. Two builds of one commit with the same
# VERSION and ROOT_PW=locked (or a fixed ROOT_PW) must match to the byte, and
# when they do not, this names the component that differs rather than hiding
# it:
#
#   - the tar itself   member order, mode, owner, mtime, size
#   - each member      fwu.sh, fwu_ver, md5.txt, uImage, rootfs
#   - uImage           the first differing offsets (offsets 0-63 are the
#                      mkimage header: magic, header CRC, timestamp)
#   - rootfs           the squashfs: every inode (name, mode, owner, time,
#                      device numbers) and then the content of every file,
#                      by path, busybox and its applets included
#
# A random root password (ROOT_PW unset) makes etc/passwd differ on purpose;
# that is not a build bug, so use ROOT_PW=locked for the comparison.
#
# Usage: tools/compare-images.sh <tar1> <tar2>
# Exit 0 only when the two files are identical.
set -euo pipefail
TAR1=${1:?usage: compare-images.sh <tar1> <tar2>}
TAR2=${2:?usage: compare-images.sh <tar1> <tar2>}

sha() { sha256sum "$1" | cut -d' ' -f1; }

if cmp -s "$TAR1" "$TAR2"; then
	echo "compare-images.sh: identical, byte for byte"
	echo "  sha256 $(sha "$TAR1")"
	exit 0
fi

echo "compare-images.sh: the tarballs DIFFER" >&2
echo "  $TAR1  sha256 $(sha "$TAR1")" >&2
echo "  $TAR2  sha256 $(sha "$TAR2")" >&2

WORK=$(mktemp -d)
# unsquashfs below can leave root-owned files under $WORK when it runs under
# sudo, and a plain rm -rf by the caller could not remove them.
asroot() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }
trap 'asroot rm -rf "$WORK"' EXIT
# unsquashfs from the host when there is one, else from the image tools
# container (image/Dockerfile), which has it at the version that wrote the
# rootfs. Device nodes in the image need root either way.
unsq() {
	if command -v unsquashfs >/dev/null 2>&1; then
		asroot unsquashfs "$@"
	else
		docker run --rm -v "$WORK:$WORK" "${TOOLS_IMAGE:-odi-oss-image}" unsquashfs "$@"
	fi
}
DIRA=$WORK/a DIRB=$WORK/b
mkdir -p "$DIRA" "$DIRB"
tar -xf "$TAR1" -C "$DIRA"
tar -xf "$TAR2" -C "$DIRB"

differ=()

# The tar container: names in archive order with mode, owner and time.
tar_listing() { tar --numeric-owner -tvf "$1"; }
if ! diff <(tar_listing "$TAR1") <(tar_listing "$TAR2") >"$WORK/tar.diff"; then
	echo "compare-images.sh: tar members differ in order, mode, owner, time or size:" >&2
	sed 's/^/  /' "$WORK/tar.diff" >&2
	differ+=("tar headers")
fi

members=$( (cd "$DIRA" && ls; cd "$DIRB" && ls) | LC_ALL=C sort -u)
for member in $members; do
	if [ ! -f "$DIRA/$member" ] || [ ! -f "$DIRB/$member" ]; then
		echo "compare-images.sh: member $member is in only one tarball" >&2
		differ+=("$member (missing)")
		continue
	fi
	cmp -s "$DIRA/$member" "$DIRB/$member" && continue
	echo "compare-images.sh: member $member differs ($(wc -c < "$DIRA/$member") and $(wc -c < "$DIRB/$member") bytes)" >&2
	differ+=("$member")
	case $member in
	uImage)
		echo "  first differing offsets (1-based byte, octal values; 1-64 is the mkimage header):" >&2
		cmp -l "$DIRA/$member" "$DIRB/$member" 2>/dev/null | head -n 8 | sed 's/^/    /' >&2 || true
		echo "  header: A $(od -An -tx1 -j0 -N64 "$DIRA/$member" | tr -d '\n')" >&2
		echo "  header: B $(od -An -tx1 -j0 -N64 "$DIRB/$member" | tr -d '\n')" >&2
		;;
	rootfs)
		# Superuser, not a fallback: rootfs/devices.pseudo puts real device
		# nodes in the image, which unsquashfs can only recreate as root.
		ra=$WORK/rootfs-a rb=$WORK/rootfs-b
		la=$WORK/ll-a lb=$WORK/ll-b
		# The inode table first: name, mode, owner, time, device numbers.
		unsq -ll "$DIRB/rootfs" >"$lb" 2>/dev/null || true
		unsq -ll "$DIRA/rootfs" >"$la" 2>/dev/null || true
		if ! diff "$la" "$lb" >"$WORK/ll.diff"; then
			echo "  rootfs inodes differ (name, mode, owner, time, device numbers):" >&2
			head -n 40 "$WORK/ll.diff" | sed 's/^/    /' >&2
		else
			echo "  rootfs inode listings match; the difference is in file contents or in the superblock" >&2
		fi
		unsq -d "$ra" "$DIRA/rootfs" >/dev/null
		unsq -d "$rb" "$DIRB/rootfs" >/dev/null
		asroot chmod -R a+rX "$ra" "$rb"
		# dev/ nodes are special files, which diff reports as different
		# without reading them; the -ll listing above already covers them.
		if diff -rq -x dev "$ra" "$rb" >"$WORK/files.diff" 2>&1; then
			echo "  rootfs file contents match" >&2
		else
			echo "  rootfs files that differ:" >&2
			sed 's/^/    /' "$WORK/files.diff" >&2
		fi
		;;
	*)
		diff -u "$DIRA/$member" "$DIRB/$member" 2>/dev/null | head -n 30 | sed 's/^/    /' >&2 || true
		;;
	esac
done

if [ ${#differ[@]} -eq 0 ]; then
	# Same members, same bytes in each, same listing, yet the files differ:
	# padding or trailing data in the tar stream.
	echo "compare-images.sh: members and headers match, yet the tarballs differ (archive padding or trailer)" >&2
	differ+=("tar stream")
fi
printf 'compare-images.sh: differing components:' >&2
printf ' %s;' "${differ[@]}" >&2
echo >&2
exit 1
