#!/usr/bin/env bash
# build-initramfs.sh -- pack odi-oss OWN rootfs (busybox, inittab,
# services, dropbear, confd, metricsd, sysctls, everything under
# rootfs/skeleton) into a cpio initramfs for the qemu full-system harness
# (make test-qemu, docs/HACKING.md).
#
# Deliberately NOT image/build.sh: that script also requires a built
# uImage (our own RTL9602C kernel) before it will stage anything, and the
# whole point of this harness is booting the real rootfs on a STOCK
# malta kernel from odi-toolchain, never building our own kernel in CI.
# So this repeats the staging steps that do not depend on the kernel
# (busybox applets, the skeleton, our binaries, dropbear/ip/bridge,
# confd assets) rather than reusing that script outright.
#
# What is DIFFERENT from a flashed image, and why (see docs/HACKING.md,
# "make test-qemu", for the full list):
#   - /bin/diag is replaced with test/qemu/diag-stub.sh: the real diag
#     talks to hardware qemu does not emulate.
#   - /etc/qemu-test/exporter.golden is added, for the stub to serve.
#   - A test-only ssh key (test/qemu/id_test.pub) is authorized for root,
#     baked into this initramfs only -- never into rootfs/skeleton.
# Nothing else in the rootfs is modified: busybox init, inittab, services,
# rcS, sysctls and every daemon are the real ones, unpatched.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/qemu-initramfs}
BUSYBOX=${BUSYBOX:-$ROOT/out/busybox}
BIN_DIR=${BIN_DIR:-$ROOT/out/bin}
ASSET_DIR=${ASSET_DIR:-$ROOT/out/confd-assets}
PKG_DIR=${PKG_DIR:-$ROOT/out}
DIAG_IMAGE=${DIAG_IMAGE:-$("$ROOT/toolchain/image.sh" diag)}

say() { printf '\n== %s\n' "$*"; }
die() { echo "build-initramfs.sh: $*" >&2; exit 1; }

[ -f "$BUSYBOX" ] || die "no busybox at $BUSYBOX -- run 'make busybox' first"

STAGE=$OUT/rootfs
rm -rf "$OUT"
mkdir -p "$STAGE"/{bin,sbin,lib,etc,dev,proc,sys,var,mnt,root}

install -m 755 "$BUSYBOX" "$STAGE/bin/busybox"

say "linking busybox applets"
APPLETS=$(docker run --rm -v "$ROOT:/work" -w /work "$DIAG_IMAGE" \
	qemu-mips-static "${BUSYBOX#"$ROOT/"}" --list)
[ -n "$APPLETS" ] || die "busybox --list produced nothing"
for a in $APPLETS; do
	[ "$a" = busybox ] && continue
	case "$a" in
	init|telnetd|mdev|ifconfig|route|reboot|halt|poweroff|klogd|syslogd|\
	insmod|rmmod|lsmod|modprobe|switch_root|sysctl|logread|watchdog|\
	start-stop-daemon|udhcpc|arp|nameif|vconfig|brctl|devmem|setconsole)
		ln -sf ../bin/busybox "$STAGE/sbin/$a" ;;
	*)
		ln -sf busybox "$STAGE/bin/$a" ;;
	esac
done
[ -e "$STAGE/bin/sh" ]    || die "busybox has no sh applet"
[ -e "$STAGE/sbin/init" ] || die "busybox has no init applet"

say "applying the skeleton (the real rootfs, unmodified)"
cp -a "$ROOT/rootfs/skeleton/." "$STAGE/"
chmod 755 "$STAGE"/etc/init.d/* "$STAGE"/etc/scripts/*
rm -rf "$STAGE/tmp"
ln -s /var/tmp "$STAGE/tmp"

# /etc/passwd: image/build.sh normally writes this (image/gen-root-account.sh),
# a step this script does not otherwise run at all since it skips the
# kernel/squashfs/uImage machinery entirely. Without it there is no root
# account: /bin/login respawns instantly, forever, since a build with no
# /etc/passwd is not one image/build.sh (or this script) is meant to
# produce. locked: keys-only, matching the test-only ssh key below --
# dropbear starts with -s (services), no password prompt offered at all.
"$ROOT/image/gen-root-account.sh" "$STAGE" "$OUT" test-qemu locked

say "our binaries (from $BIN_DIR)"
for b in diag omcid omcli omciprobe omcicap nv igmpd metricsd confd; do
	[ -f "$BIN_DIR/$b" ] && install -m 755 "$BIN_DIR/$b" "$STAGE/bin/$b"
done
[ -f "$STAGE/bin/omcli" ] && ln -sf omcli "$STAGE/bin/omcicli"
if [ -f "$STAGE/bin/confd" ] && [ -d "$ASSET_DIR" ] && [ -f "$ASSET_DIR/index.html" ]; then
	mkdir -p "$STAGE/etc/confd"
	cp "$ASSET_DIR"/* "$STAGE/etc/confd/"
fi

say "upstream packages (from $PKG_DIR)"
if [ -f "$PKG_DIR/dropbearmulti" ]; then
	install -m 755 "$PKG_DIR/dropbearmulti" "$STAGE/sbin/dropbearmulti"
	ln -sf dropbearmulti "$STAGE/sbin/dropbear"
	ln -sf dropbearmulti "$STAGE/sbin/dropbearkey"
	ln -sf ../sbin/dropbearmulti "$STAGE/bin/scp"
fi
for b in ip bridge; do
	[ -f "$PKG_DIR/$b" ] && install -m 755 "$PKG_DIR/$b" "$STAGE/sbin/$b"
done

for need in bin/busybox sbin/init sbin/dropbearmulti bin/metricsd bin/confd; do
	[ -e "$STAGE/$need" ] || die "missing $need -- run 'make busybox packages src releases' first"
done

say "harness /init: mounts devtmpfs, then execs the real /sbin/init"
# CONFIG_DEVTMPFS_MOUNT automatic /dev populate (the comment in rcS itself: "/dev
# is devtmpfs, mounted by the kernel before init") only fires on the
# NORMAL root-mount path (do_mounts.c mount_root()) -- a pure initramfs
# boot (-initrd + rdinit=, this harness, no root= device) skips that path
# entirely, so /dev stays empty and the real rcS never touches it (it
# assumes devtmpfs already happened). Measured: without this, /dev/null,
# /dev/urandom, /dev/console and /dev/kmsg do not exist, and a shell
# redirection like `> /dev/kmsg` then silently CREATES them as empty
# regular files instead of erroring -- which is how dropbear key
# generation, seedrng and every crumb looked like they worked while
# actually writing into fake files. This wrapper is the only rootfs
# addition that is not "the real rootfs, unmodified": one exec, nothing a
# flashed image would ever run.
cat > "$STAGE/init" <<'HARNESSINIT'
#!/bin/busybox sh
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null
exec /sbin/init
HARNESSINIT
chmod 755 "$STAGE/init"

say "qemu-mode overlay: diag stub, exporter fixture, test ssh key, memhog"
mkdir -p "$STAGE/etc/qemu-test"
cp "$ROOT/src/diag/test/exporter.golden" "$STAGE/etc/qemu-test/exporter.golden"
install -m 755 "$ROOT/test/qemu/diag-stub.sh" "$STAGE/bin/diag"
install -m 755 "$ROOT/test/qemu/memhog.sh" "$STAGE/etc/qemu-test/memhog.sh"

# docs/ACCESS.md: dropbear is started with -D /etc/config/dropbear.d, so it
# reads /etc/config/dropbear.d/authorized_keys for root -- but /etc/config
# is the symlink rootfs/skeleton keeps to /var/config, and /var is fresh tmpfs by
# the time rcS gets there (mounted before anything else, and there is no
# real jffs2 config partition here), so a key staged under $STAGE/var
# would already be gone by the time dropbear starts.
#
# rcS.dev (rootfs/skeleton/etc/init.d/rcS.dev) already ships in every image
# and already defines dev_hook, the sanctioned override point rcS calls
# after the config-partition step -- so the real dev aids stay wired in,
# this WRAPS dev_hook rather than replacing it: rename the shipped function
# to dev_hook_stock, then define a new dev_hook that calls it first and
# adds the test key install on the "config" case.
TESTKEY_PUB=$ROOT/test/qemu/id_test.pub
if [ -f "$TESTKEY_PUB" ]; then
	cp "$TESTKEY_PUB" "$STAGE/etc/qemu-test/authorized_keys"
	sed -i.bak 's/^dev_hook() {/dev_hook_stock() {/' "$STAGE/etc/init.d/rcS.dev"
	rm -f "$STAGE/etc/init.d/rcS.dev.bak"
	cat >> "$STAGE/etc/init.d/rcS.dev" <<'DEVHOOK'

# ---- qemu test harness addition (test/qemu/build-initramfs.sh) --------
dev_hook() {
	dev_hook_stock "$@"
	case "$1" in
	config)
		# The kernel own initramfs root defaults to 1777 (tmpfs-style,
		# like /tmp) since this harness runs straight from the initramfs
		# with no switch_root to a "real" (squashfs, 0755) rootfs.
		# dropbear refuses pubkey auth outright when / is group/other
		# writable ("/ must be owned by user or root, and not writable by
		# group or others") -- measured, not a guess. Harmless on a real
		# device, whose root really is 0755 squashfs already.
		chmod 0755 / 2>/dev/null || true
		if [ -f /etc/qemu-test/authorized_keys ]; then
			mkdir -p /var/config/dropbear.d
			cp /etc/qemu-test/authorized_keys /var/config/dropbear.d/authorized_keys
			chmod 600 /var/config/dropbear.d/authorized_keys
			crumb "qemu harness: test ssh key installed"
		fi
		# the OVERRIDE network.sh reads (/etc/config/lan-ip): address the box inside
		# QEMU user-mode networking OWN default subnet (10.0.2.0/24,
		# gateway 10.0.2.2) rather than this device real 192.168.x.1
		# default. hostfwd only delivers to an address SLIRP itself
		# considers reachable; a custom net=/host= override on the qemu
		# command line to match the device own subnet was tried and did
		# NOT deliver packets to the guest at all (measured), while the
		# plain default subnet did, bridged, immediately.
		echo 10.0.2.15 > /etc/config/lan-ip
		;;
	esac
}
DEVHOOK
else
	echo "  no $TESTKEY_PUB -- ssh key auth in the harness will not work" >&2
fi

say "packing $OUT/initramfs.cpio.gz"
# -R 0:0 (both GNU and bsdcpio accept this form; the "root" name form
# fails on macOS, whose root group is "wheel"): this build stages files
# as the host user, not root, and dropbear refuses to even try pubkey
# auth when /etc is not root-owned ("/etc must be owned by user or
# root, and not writable by group or others") -- measured, not a guess.
( cd "$STAGE" && find . -mindepth 1 -print0 | cpio --null -o -H newc -R 0:0 2>/dev/null ) | gzip -9 > "$OUT/initramfs.cpio.gz"
echo "  $(du -h "$OUT/initramfs.cpio.gz" | cut -f1)"
