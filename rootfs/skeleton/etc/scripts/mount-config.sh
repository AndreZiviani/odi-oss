#!/bin/sh
#
# Mount the jffs2 config partition at /var/config, and point /etc/config at it.
#
# Everything that must survive a reflash lives there: the ssh host key, the UI
# credential, the feature .off switches. `fwu.sh` writes only the kernel and
# rootfs partitions, so this one is untouched by an upgrade — which is what
# makes it the right home for per-device state, and also means a bad value
# written here is NOT undone by flashing a new image.
#
# The partition is found by NAME in /proc/mtd, never by a fixed number: this
# board has fourteen and the numbering is not a promise.
#
# Found with grep + sed rather than awk: awk here SIGBUSed at boot (before
# the network came up) on every image since n4, but ran fine over ssh on the
# same boot. Root cause is being tracked separately; this just removes awk
# from the boot path so the partition mounts regardless.
set -eu

NAME=${1:-config}
DIR=${2:-/var/config}

n=$(grep -F "\"$NAME\"" /proc/mtd | sed -n 's/^mtd\([0-9]*\):.*/\1/p' | head -n 1)
if [ -z "$n" ]; then
	echo "mount-config: no MTD partition named $NAME" >&2
	echo "mount-config: no MTD partition named $NAME; /proc/mtd: $(tr "\n" ";" < /proc/mtd 2>&1)" > /tmp/mount-config.err 2>/dev/null
	exit 1
fi

mkdir -p "$DIR"

# mdev may not have made the node yet; jffs2 needs the BLOCK device.
if [ ! -b "/dev/mtdblock$n" ] && ! mknod "/dev/mtdblock$n" b 31 "$n"; then
	echo "mount-config: mknod /dev/mtdblock$n failed; $(set -- /dev/mtd*; echo $#) mtd nodes, $(grep " /dev " /proc/mounts 2>&1)" > /tmp/mount-config.err 2>/dev/null
	exit 1
fi

if ! err=$(mount -t jffs2 "/dev/mtdblock$n" "$DIR" 2>&1); then
	echo "mount-config: /dev/mtdblock$n would not mount as jffs2: $err" >&2
	# Earlier boots failed here with the reason on a serial
	# console nobody has; printk survives the revert.
	echo "mount-config: /dev/mtdblock$n would not mount as jffs2: $err; $(ls -la /dev/mtdblock$n 2>&1); $(grep config /proc/mtd 2>&1)" > /tmp/mount-config.err 2>/dev/null
	[ -w /dev/kmsg ] && cat /tmp/mount-config.err > /dev/kmsg
	exit 1
fi

# /etc is read-only squashfs, so /etc/config is a symlink baked into the image
# pointing here. Nothing to do but check it, and say so if it is wrong rather
# than leaving every later `[ -f /etc/config/... ]` silently false.
if [ ! -d /etc/config ]; then
	echo "mount-config: /etc/config does not resolve -- feature switches and" >&2
	echo "              the ssh host key will not be found" >&2
	exit 1
fi

# Say so, for anything that has to tell "the switch is off" apart from "the
# switch could not be read". Without this every `[ -f /etc/config/X.off ]` is
# false when the partition did not mount, which turns every protective flag
# into a no-op exactly when the machine is already in an unexpected state.
mkdir -p /var/run
: > /var/run/config-mounted
