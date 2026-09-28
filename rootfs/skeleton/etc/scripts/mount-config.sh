#!/bin/sh
#
# The config partition itself is mounted declaratively now: /etc/fstab has
# `mtd:config /var/config jffs2 defaults 0 0`, and rcS's `mount -a` (second
# pass, once /var/config exists as a directory) is what actually mounts it
# -- by MTD partition NAME (get_mtd_device_nm, core MTD), no /dev/mtdblockN
# node and no /proc/mtd lookup, which is what this script used to do by
# hand, mknod included.
#
# The ONE thing left that is not a mount option: /etc is read-only
# squashfs, so /etc/config is a symlink baked into the image pointing at
# $DIR, and nothing mounts a symlink into existence -- this just checks it
# actually resolves, and says so if it does not, rather than leaving every
# later `[ -f /etc/config/... ]` silently false. rcS still runs this under
# `sh -x` into /tmp/mount-config.trace, so a failure here leaves something
# in the ramlog.
#
# Nothing to flag here on success: rcS's config_mounted() (rootfs/skeleton/
# etc/init.d/rcS) checks the live mount table plus this same symlink, on
# every call, rather than trusting a flag written once here and never
# rechecked.
set -eu

DIR=${1:-/var/config}

if [ ! -d /etc/config ]; then
	echo "mount-config: /etc/config does not resolve to $DIR -- feature switches and" >&2
	echo "              the ssh host key will not be found" >&2
	exit 1
fi
