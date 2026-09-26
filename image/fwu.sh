#!/bin/sh
#
# Flash one image slot. This is OURS, not the vendor version: their fwu.sh is
# what the stock upgrade path runs, and this reproduces its contract -- same
# two arguments, same member names, same md5.txt format -- so an image built
# here is accepted by the same mechanism.
#
#     fwu.sh <slot> <tarball>
#
# <slot> is 0 or 1 and names the partition PAIR (k0+r0, or k1+r1). The stick
# has two of everything and boots whichever the bootloader was told to; that
# is the whole safety net, so this writes the slot it was given and never the
# running one.
#
# Three things the vendor version gets wrong, fixed here:
#
#   1. It sets `set -e` and then checks `$?` after diff. With `set -e` a
#      failing diff kills the script before the check runs, so the friendly
#      "md5_sum inconsistent" message is unreachable. We test directly.
#   2. It writes the image version with `nv setenv` unconditionally. `nv` is
#      not on every image; a missing one must not fail a flash that already
#      succeeded.
#   3. It leaves the partition half-written if the write fails, with nothing
#      said. We report which partition is in that state, because it decides
#      whether the stick can still boot.
#
# And four things this script itself got wrong, fixed here after a review
# before the first flash:
#
#   4. It said "this writes the slot it was given and never the running one"
#      and had no such check. Erasing the running slot kills the box mid-flash
#      AND destroys the partition the bootloader would fall back to. Now it
#      derives the running slot from /proc/cmdline and refuses.
#   5. It checked the md5s but never the SIZES, so an image too big for the
#      partition was found out after the erase.
#   6. It called flash_eraseall without checking it exists. Our own busybox
#      did not have it, so on our own image the erase failed with "applet not
#      found" and the script announced the slot was UNBOOTABLE -- which was
#      false, because nothing had been erased.
#   7. It wrote sw_version$slot with `nv setenv` for a cosmetic version
#      string. That is an erase-and-rewrite of the environment partition, and
#      a power cut in that window can leave the stick with no bootable
#      environment at all. Opt-in now.
#
# The config partition is deliberately NOT touched. Host keys, the UI
# credential and the feature switches live there and survive an upgrade.

set -u

# Overridable only so the refusal logic can be tested off the device; on the
# stick these are always the real files. See test/fwu_guard_test.sh.
PROC_MTD=${PROC_MTD:-/proc/mtd}
PROC_CMDLINE=${PROC_CMDLINE:-/proc/cmdline}

slot=${1:-}
tarball=${2:-}

case "$slot" in
0|1) ;;
*) echo "usage: $0 <0|1> <image.tar>" >&2; exit 1 ;;
esac
[ -f "$tarball" ] || { echo "fwu: no such image: $tarball" >&2; exit 1; }
# md5.txt is read from the working directory, which is where the upgrade path
# unpacks the tarball. Say so rather than failing inside check().
[ -f md5.txt ] || { echo "fwu: no md5.txt here (run from the unpacked image)" >&2; exit 1; }

# Is a command available? `command -v` is NOT in the stock image's shell --
# busybox 1.12.4's ash answers "ash: command: not found" -- and there is no
# `which`, so walk PATH with the test builtin. Verified against the stock
# busybox under qemu-mips; see the note on the read-back below for the rest of
# what that image does and does not have.
have_cmd() {
	case $1 in
	*/*)
		[ -x "$1" ] && return 0
		return 1
		;;
	esac
	_hc_ifs=$IFS
	IFS=:
	for _hc_d in $PATH; do
		if [ -x "${_hc_d:-.}/$1" ]; then
			IFS=$_hc_ifs
			return 0
		fi
	done
	IFS=$_hc_ifs
	return 1
}

# Find the partitions by NAME in /proc/mtd, never by a fixed number: this board
# has fourteen and the numbering is not a promise.
mtd_of() {
	sed -n "s/^\(mtd[0-9]*\):.*\"$1\"\$/\1/p" "$PROC_MTD" | sed -n '1p'
}

k_mtd=$(mtd_of "k$slot")
r_mtd=$(mtd_of "r$slot")
[ -n "$k_mtd" ] || { echo "fwu: no MTD partition named k$slot" >&2; exit 1; }
[ -n "$r_mtd" ] || { echo "fwu: no MTD partition named r$slot" >&2; exit 1; }
echo "fwu: slot $slot -> kernel /dev/$k_mtd, rootfs /dev/$r_mtd"

# The slot this stick is running from, and the one thing this script must never
# touch. U-Boot passes root= as a device number: 31 is mtdblock, and the minor
# is the partition index, so r0 and r1 name the slot directly. Read from the
# device rather than assumed, because the index depends on the mtdparts string
# U-Boot passes and that is not ours to fix.
running=
r0_idx=$(mtd_of "r0" | sed "s/^mtd//")
r1_idx=$(mtd_of "r1" | sed "s/^mtd//")
root=$(sed -n "s/.*root=31:\([0-9]*\).*/\1/p" "$PROC_CMDLINE" 2>/dev/null)
if [ -n "$root" ]; then
	[ "$root" = "$r0_idx" ] && running=0
	[ "$root" = "$r1_idx" ] && running=1
fi
if [ -z "$running" ]; then
	# Second opinion, and the only other one on the device: U-Boot's own
	# record of what it last booted.
	running=$(nv getenv sw_active 2>/dev/null | sed -n "s/^sw_active=//p")
	case "$running" in 0|1) ;; *) running= ;; esac
fi
if [ -z "$running" ]; then
	echo "fwu: cannot tell which slot is running (no root=31:N in" >&2
	echo "     /proc/cmdline and no usable sw_active). REFUSING." >&2
	echo "     Set FWU_FORCE=1 only if you are certain slot $slot is idle." >&2
	[ "${FWU_FORCE:-0}" = 1 ] || exit 1
	echo "fwu: FWU_FORCE=1 -- proceeding without knowing the running slot" >&2
elif [ "$running" = "$slot" ]; then
	echo "fwu: slot $slot is the one this stick is RUNNING FROM. REFUSING." >&2
	echo "     Erasing it kills the box mid-flash and destroys the partition" >&2
	echo "     the bootloader would fall back to. Flash the other slot." >&2
	exit 1
else
	echo "fwu: running slot is $running, target is $slot -- ok"
fi

# The OTHER half of the safety property, which nothing checked.
#
# Refusing the running slot protects the box while it is being written. What
# protects it AFTERWARDS is the revert, and the revert target is sw_commit,
# not sw_active -- bootcmd sends any sw_tryactive of 2 to boot_by_commit, which
# reads sw_commit alone. If sw_commit does not name the slot still carrying
# known-good firmware, a failed trial reverts INTO the experiment.
#
# They agree on both reference sticks today, which is luck and not design. The
# reason to check is isp2: its two env copies disagree -- env2 is the active
# one and says sw_commit=1, env is stale and says 0 -- so a fallback to the
# stale copy sends boot_by_commit to slot 0, the slot being flashed here, and
# that path never runs en_wdt, so a hung image hangs forever instead of being
# reset.
commit=$(nv getenv sw_commit 2>/dev/null | sed -n "s/^sw_commit=//p")
case "$commit" in
0|1)
	if [ "$commit" = "$slot" ]; then
		echo "fwu: sw_commit is $slot, the slot being written. REFUSING." >&2
		echo "     A failed trial would revert into this image rather than" >&2
		echo "     away from it. Point sw_commit at the running slot first:" >&2
		echo "       nv setenv sw_commit $running" >&2
		[ "${FWU_FORCE:-0}" = 1 ] || exit 1
		echo "fwu: FWU_FORCE=1 -- writing anyway, with no good revert target" >&2
	else
		echo "fwu: sw_commit is $commit, the revert target is intact -- ok"
	fi
	;;
*)
	echo "fwu: cannot read sw_commit -- the revert target is UNKNOWN." >&2
	echo "     Check it by hand before spending a trial boot." >&2
	;;
esac

# The SECOND answer, and the one the check above cannot give. `nv getenv`
# reports the copy U-Boot uses TODAY. If that copy is ever left invalid -- an
# interrupted `nv setenv` is how it happens, since setenv erases the partition
# whole -- U-Boot uses the other one, and on isp2 the two disagree: env2 wins
# and says 1, env is stale and says 0.
#
# Our nv answers for the loser with `fallback`. The VENDOR nv, which is what
# runs the FIRST flash, has no such verb and prints its usage instead, so an
# empty answer means NOT CHECKABLE and must not read as safe.
alt=$(nv fallback sw_commit 2>/dev/null | sed -n "s/^sw_commit=//p")
case "$alt" in
0|1)
	if [ "$alt" = "$slot" ]; then
		echo "fwu: the FALLBACK env copy says sw_commit=$slot, the slot being" >&2
		echo "     written. The active copy is fine, so this is not today's" >&2
		echo "     boot -- it is the one after an interrupted nv setenv, and" >&2
		echo "     boot_by_commit never runs en_wdt, so a hung image hangs" >&2
		echo "     forever rather than being reset. REFUSING." >&2
		echo "     Make the two copies agree first -- a plain setenv writes" >&2
		echo "     back the copy it read, so name the other one:" >&2
		echo "       nv setenv -c <1|2> sw_commit $running" >&2
		[ "${FWU_FORCE:-0}" = 1 ] || exit 1
		echo "fwu: FWU_FORCE=1 -- writing anyway, with a bad fallback" >&2
	else
		echo "fwu: the fallback env copy says $alt too -- ok"
	fi
	;;
*)
	echo "fwu: the fallback env copy was not readable -- NOT checked." >&2
	echo "     Expected on the vendor nv, which has no fallback verb." >&2
	;;
esac

# The tool that does the erasing. Checked BEFORE anything is verified or
# written: a missing applet here used to surface as an erase failure, and the
# message for that says the slot is unbootable.
if ! have_cmd flash_eraseall; then
	echo "fwu: flash_eraseall is not on this image. Nothing has been" >&2
	echo "     touched. Flash from an image that has it." >&2
	exit 1
fi

# Sizes, before the erase rather than after it. /proc/mtd is in hex.
part_size() {
	sed -n "s/^$1: \([0-9a-f]*\) .*/\1/p" "$PROC_MTD" | sed -n '1p'
}
fits() {
	have=$(tar -xf "$tarball" "$1" -O | cksum | cut -d' ' -f2)
	cap=$((0x$(part_size "$2")))
	if [ "$have" -gt "$cap" ]; then
		echo "fwu: $1 is $have bytes and /dev/$2 holds $cap. REFUSING." >&2
		return 1
	fi
	echo "fwu: $1 $have of $cap bytes"
}
fits uImage "$k_mtd" || exit 1
fits rootfs "$r_mtd" || exit 1

# Verify BOTH members before writing EITHER. A tarball whose rootfs is corrupt
# must not get as far as erasing the kernel partition.
check() {
	want=$(sed -n "s/^\([0-9a-f]*\) [ *]\{0,1\}$1\$/\1/p" md5.txt | sed -n '1p')
	if [ -z "$want" ]; then
		echo "fwu: md5.txt has no line for $1" >&2
		return 1
	fi
	got=$(tar -xf "$tarball" "$1" -O | md5sum | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then
		echo "fwu: $1 md5 mismatch -- expected $want, got $got" >&2
		return 1
	fi
	echo "fwu: $1 ok ($got)"
}

check uImage || exit 1
check rootfs || exit 1

# From here a failure leaves a partition unbootable, so say which.
write() {
	dev=/dev/$2
	echo "fwu: erasing $dev"
	flash_eraseall "$dev" || {
		echo "fwu: ERASE FAILED on $dev -- slot $slot is now UNBOOTABLE" >&2
		exit 1
	}
	echo "fwu: writing $1 to $dev"
	tar -xf "$tarball" "$1" -O > "$dev" || {
		echo "fwu: WRITE FAILED on $dev -- slot $slot is now UNBOOTABLE" >&2
		exit 1
	}
	# Read it back. The NOR write path in this kernel has never been
	# exercised on this part -- the driver's three JEDEC probes match none
	# of it and it falls through to a generic profile -- so "the write
	# returned success" is not the same as "the flash holds what we sent".
	# Compare only the bytes written: the rest of the partition is 0xff.
	# This was `dd | head -c $len | md5sum`, and NEITHER dd NOR head is on
	# the stock image this has to run from. `cmp` against the device needs
	# no length at all: the member ends first, so cmp stops there and says
	# so. Measured on the stock busybox 1.12.4 under qemu-mips, all three
	# outcomes:
	#
	#   prefix matches, device longer   rc=1  "cmp: EOF on - after byte N"
	#   identical length and content    rc=0  no output
	#   a byte differs                  rc=1  "- /dev/mtdN differ: char N"
	#
	# so EOF-on-stdin is the success case and anything else is not. Note
	# the match is on "EOF on -", naming stdin: an EOF on the DEVICE would
	# mean the partition is shorter than the member, which fits() already
	# refused, and must not be read as success.
	rb=$(tar -xf "$tarball" "$1" -O | cmp - "$dev" 2>&1) && rb=""
	case $rb in
	"" | *"EOF on -"*)
		;;
	*)
		echo "fwu: READ-BACK MISMATCH on $dev -- $rb" >&2
		echo "     slot $slot is NOT trustworthy. Do not boot it." >&2
		exit 1
		;;
	esac
	echo "fwu: $1 verified on $dev"
}

write uImage "$k_mtd"
write rootfs "$r_mtd"

# Record the version, if this image carries a tool that can. Failing here does
# not undo a successful flash, so it is a warning.
#
# OFF by default, and that is the change: `nv setenv` erases the environment
# partition and rewrites it, and a power cut in that window can leave the stick
# with no bootable environment at all. Spending that risk on a cosmetic version
# string is a bad trade. FWU_RECORD_VERSION=1 restores the old behaviour.
if [ "${FWU_RECORD_VERSION:-0}" = 1 ] &&
   tar -xf "$tarball" fwu_ver -O > /tmp/fwu_ver 2>/dev/null; then
	ver=$(cat /tmp/fwu_ver)
	if have_cmd nv; then
		nv setenv "sw_version$slot" "$ver" ||
			echo "fwu: could not record the version (image is written)" >&2
	else
		echo "fwu: no nv on this image; version $ver not recorded" >&2
	fi
	rm -f /tmp/fwu_ver
fi

echo "fwu: slot $slot written."
echo "fwu: it is NOT active yet. 'nv setenv sw_tryactive $slot' boots it once,"
echo "     with the watchdog armed, and reverts by itself if it does not come up."
