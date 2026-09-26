#!/usr/bin/env bash
#
# Assemble a flashable image: stage the rootfs, squash it, and tar it with the
# kernel, our fwu.sh and the md5.txt the flasher checks.
#
# The output has the same five members as the stock image, because the on-device
# upgrade path is what unpacks it:
#
#     fwu.sh  fwu_ver  md5.txt  rootfs  uImage
#
# Nothing here flashes anything, and the result is inert until someone runs
# `nv setenv sw_tryactive <slot>` on the stick. Read docs/FLASHING.md first.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)

# r0/r1 and k0/k1 from /proc/mtd on the DFP-34X-2C2. Exceeding either is a hard
# stop: a partition that does not fit cannot be flashed, and finding that out
# on the device costs a recovery.
ROOTFS_PART=$((0x274000))     # 2,572,288
KERNEL_PART=1359872

# The stock image is squashfs 4.0 with LZMA and 1 MiB blocks (read off its
# superblock). Mainline squashfs has no LZMA, so this image is xz; the blocks
# are smaller because 6.18 squashfs keeps caches of whole blocks, and RAM is
# 32 MB.
COMP=${COMP:-xz}
BS=${BS:-262144}

# TOOLS_IMAGE names the container (image/Dockerfile), for a build that must
# not share the default tag with another one on the same Docker host.
TOOLS=${TOOLS_IMAGE:-odi-oss-image}
# Date plus the short commit: two builds on one day stay apart, and an image
# names the tree that produced it (a dirty tree gets -dirty).
GITREV=$(git -C "$ROOT" describe --always --dirty --abbrev=7 2>/dev/null || echo nogit)
VERSION=${VERSION:-odi-oss-$(date +%y%m%d)-$GITREV}
BUSYBOX=${BUSYBOX:-$ROOT/out/busybox}
# The kernel .config that produced KERNEL, if the build left one beside it.
KCONFIG=${KCONFIG:-}
# Where to look for our own daemons and CLIs (src/build.sh leaves them in
# out/bin). A variable rather than a path baked in here, so a build can point
# it at binaries built elsewhere.
BIN_DIR=${BIN_DIR:-$ROOT/out/bin}
ASSET_DIR=${ASSET_DIR:-$ROOT/out/confd-assets}
# Where packages/*/build.sh leaves its output.
PKG_DIR=${PKG_DIR:-$ROOT/out}

KERNEL=${KERNEL:-}
if [ -z "$KERNEL" ]; then
	[ -f "$ROOT/build/kernel-618/uImage" ] && KERNEL=$ROOT/build/kernel-618/uImage
fi
if [ -z "$KCONFIG" ]; then
	KERNEL_DIR=$(dirname "$KERNEL")
	[ -f "$KERNEL_DIR/config" ] && KCONFIG=$KERNEL_DIR/config
fi

STAGE=$ROOT/build/rootfs
OUT=$ROOT/out/image

say()  { printf '\n== %s\n' "$*"; }
die()  { echo "$*" >&2; exit 1; }
size() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }
md5_of() { (md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1); }

# Every input this script does not build itself, checked up front and named
# together: on a fresh tree `make image` alone has only src/ behind it, and
# finding the missing pieces one failed run at a time is how a first build
# goes wrong. make image-all builds all of them, in order.
missing=""
[ -n "$KERNEL" ] && [ -f "$KERNEL" ] || missing="$missing
  kernel         make kernel (or KERNEL=<uImage>)"
[ -f "$BUSYBOX" ] || missing="$missing
  busybox        make busybox (at $BUSYBOX)"
if [ "${ALLOW_PARTIAL:-0}" != 1 ]; then
	for p in dropbearmulti ip bridge; do
		[ -f "$PKG_DIR/$p" ] || missing="$missing
  $p$(printf '%*s' $((15 - ${#p})) '')make packages (at $PKG_DIR)"
	done
	for b in metricsd confd; do
		[ -f "$BIN_DIR/$b" ] || missing="$missing
  $b$(printf '%*s' $((15 - ${#b})) '')make releases (at $BIN_DIR)"
	done
fi
[ -z "$missing" ] ||
	die "image/build.sh: missing inputs:$missing
On a fresh tree, make image-all builds every one of them in order.
ALLOW_PARTIAL=1 builds without the packages and releases."

say "build environment"
docker build -q -t "$TOOLS" -f "$ROOT/image/Dockerfile" "$ROOT/image" >/dev/null
run() { docker run --rm -v "$ROOT:/work" -w /work "$TOOLS" "$@"; }

say "staging the rootfs"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,sbin,lib,etc,dev,proc,sys,var,mnt,root}
# /dev is a real directory here and its CONTENTS come from
# rootfs/devices.pseudo. Do not also declare the directory in that file:
# mksquashfs warns and ignores the pseudo entry when the real path exists.

install -m 755 "$BUSYBOX" "$STAGE/bin/busybox"

# The applet list comes from the binary rather than a checked-in list: the
# config fragment decides which applets exist, and a stale list produces a
# symlink to an applet that is not there -- which fails at run time, inside a
# boot script, as "applet not found".
say "linking busybox applets"
APPLETS=$(run qemu-mips-static "${BUSYBOX#"$ROOT/"}" --list)
[ -n "$APPLETS" ] || die "busybox --list produced nothing"
n=0
for a in $APPLETS; do
	[ "$a" = busybox ] && continue
	case "$a" in
	# /sbin, because something looks these up by absolute path: the kernel
	# and inittab want /sbin/init, and our own scripts spell out
	# /sbin/ifconfig and /sbin/devmem.
	init|telnetd|mdev|ifconfig|route|reboot|halt|poweroff|klogd|syslogd|\
	insmod|rmmod|lsmod|modprobe|switch_root|sysctl|logread|watchdog|\
	start-stop-daemon|udhcpc|arp|nameif|vconfig|brctl|devmem|setconsole)
		ln -sf ../bin/busybox "$STAGE/sbin/$a" ;;
	*)
		ln -sf busybox "$STAGE/bin/$a" ;;
	esac
	n=$((n + 1))
done
echo "  $n applets"
[ -e "$STAGE/bin/sh" ]    || die "busybox has no sh applet -- nothing would boot"
[ -e "$STAGE/sbin/init" ] || die "busybox has no init applet -- nothing would boot"

say "applying the skeleton"
cp -a "$ROOT/rootfs/skeleton/." "$STAGE/"
chmod 755 "$STAGE"/etc/init.d/* "$STAGE"/etc/scripts/*

# /tmp is a link to /var/tmp, as in the stock image: a real directory would be
# in the read-only squashfs, and every temp file would fail with EROFS. /var is
# the ramfs rcS mounts first thing, before anything writes a temp file
# (test/rootfs_chroot_test.sh). Made after the skeleton copy, over whatever is
# there: an empty rootfs/skeleton/tmp is invisible to git, so a working tree
# can have one a fresh clone does not, and `cp -a` onto a link fails.
rm -rf "$STAGE/tmp"
ln -s /var/tmp "$STAGE/tmp"

# Our own binaries, if they have been built. Optional on purpose: the image is
# a working Linux without them, and saying which are missing beats failing.
say "our binaries (from $BIN_DIR)"
found=0
for b in diag omcid omcli omciprobe omcicap nv igmpd metricsd confd; do
	if [ -f "$BIN_DIR/$b" ]; then
		install -m 755 "$BIN_DIR/$b" "$STAGE/bin/$b"
		printf '  %-10s %8d\n' "$b" "$(size "$BIN_DIR/$b")"
		found=$((found + 1))
	fi
done
[ "$found" = 0 ] &&
	echo "  none -- this image boots to a shell and runs no service of ours"
# confd runs /bin/omcicli for its OMCI page; omcli speaks the stock queue
# protocol with the stock arguments (mib get, dump ...), so the stock name
# resolves to it.
[ -f "$STAGE/bin/omcli" ] && ln -sf omcli "$STAGE/bin/omcicli"

# confd reads /etc/confd/ for every page and its .tsv tables; without them it
# answers 404 to everything, which is harder to diagnose than no UI at all, so
# that combination is refused.
if [ -f "$STAGE/bin/confd" ]; then
	if [ -d "$ASSET_DIR" ] && [ -f "$ASSET_DIR/index.html" ]; then
		mkdir -p "$STAGE/etc/confd"
		cp "$ASSET_DIR"/* "$STAGE/etc/confd/"
		printf '  %-10s %8d KB in %s files\n' etc/confd \
			"$(du -sk "$STAGE/etc/confd" | cut -f1)" \
			"$(find "$STAGE/etc/confd" -type f | wc -l | tr -d ' ')"
	elif [ "${ALLOW_PARTIAL:-0}" = 1 ]; then
		echo "  confd is installed but $ASSET_DIR has no assets -- ALLOW_PARTIAL=1, shipping confd with no UI"
	else
		die "confd is installed but $ASSET_DIR has no assets -- run src/fetch-releases.sh, or set ALLOW_PARTIAL=1 to ship without it"
	fi
fi

# No kernel modules: every odi driver is built into vmlinux (CONFIG_ODI_SWITCH=y
# and the others, kernel/618/config), and omcid reaches the switch driver over
# netlink.
say "kernel modules"
echo "  none -- odi_switch is built into vmlinux (CONFIG_ODI_SWITCH=y, kernel/618/config)"

# The upstream packages. dropbear is a multi build that dispatches on argv[0];
# etc/init.d/services calls /sbin/dropbearkey and /sbin/dropbear by path.
say "upstream packages (from $PKG_DIR)"
pkg=0
if [ -f "$PKG_DIR/dropbearmulti" ]; then
	install -m 755 "$PKG_DIR/dropbearmulti" "$STAGE/sbin/dropbearmulti"
	ln -sf dropbearmulti "$STAGE/sbin/dropbear"
	ln -sf dropbearmulti "$STAGE/sbin/dropbearkey"
	# The server side of a copy is `scp -t` or `scp -f` found on PATH by
	# the login shell, so the name lives in /bin. Legacy scp protocol only:
	# an OpenSSH 9+ client needs `scp -O`.
	ln -sf ../sbin/dropbearmulti "$STAGE/bin/scp"
	printf '  %-10s %8d  (+ dropbear, dropbearkey, scp)\n' dropbearmulti \
		"$(size "$PKG_DIR/dropbearmulti")"
	pkg=$((pkg + 1))
elif [ "${ALLOW_PARTIAL:-0}" = 1 ]; then
	echo "  dropbearmulti  MISSING at $PKG_DIR -- ALLOW_PARTIAL=1, shipping without it (no ssh)"
else
	die "dropbearmulti missing at $PKG_DIR -- run 'make packages', or set ALLOW_PARTIAL=1 to build without it"
fi
for b in ip bridge; do
	if [ -f "$PKG_DIR/$b" ]; then
		install -m 755 "$PKG_DIR/$b" "$STAGE/sbin/$b"
		printf '  %-10s %8d\n' "$b" "$(size "$PKG_DIR/$b")"
		pkg=$((pkg + 1))
	elif [ "${ALLOW_PARTIAL:-0}" = 1 ]; then
		echo "  $b  MISSING at $PKG_DIR -- ALLOW_PARTIAL=1, shipping without it"
	else
		die "$b missing at $PKG_DIR -- run 'make packages', or set ALLOW_PARTIAL=1 to build without it"
	fi
done
[ "$pkg" = 0 ] && echo "  none -- run make packages"

# What this image is made of, on the image. /etc/odi-build lists every piece
# with its size and md5; /etc/kernel-config is the .config this image shipped
# a kernel built from, readable without booting. /proc/config.gz, from the
# running kernel itself, can disagree with it, and a mismatch means the kernel
# and rootfs partitions came from different builds.
say "build manifest"
{
	# key=value lines first: the exporter turns every line with an equals
	# sign into a gpon_image_info label, and image verification reads image=.
	# The descriptive lines below carry no equals sign.
	echo "image=$VERSION"
	echo "built=$(date -u '+%Y-%m-%dT%H:%MZ')"
	# The kernel release, from the header of the .config; the uImage file
	# name when no config is staged.
	kver=$(sed -n "s/^# Linux\/[^ ]* \([0-9][^ ]*\) Kernel Configuration.*/\1/p" "${KCONFIG:-/dev/null}" 2>/dev/null | head -n 1)
	echo "kernel=${kver:-$(basename "$KERNEL")}"
	if [ -f "$BIN_DIR/releases.env" ]; then
		# shellcheck disable=SC1091
		. "$BIN_DIR/releases.env"
		echo "exporter=${METRICSD_TAG:-unknown}"
		echo "confd=${CONFD_TAG:-unknown}"
	fi
	echo "version:   $VERSION"
	echo "built:     $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	echo "kernel:    $(basename "$KERNEL")  $(size "$KERNEL") bytes  md5 $(md5_of "$KERNEL")"
	echo "busybox:   $(size "$BUSYBOX") bytes  md5 $(md5_of "$BUSYBOX")"
	if [ -n "$KCONFIG" ]; then
		echo "kconfig:   $(basename "$KCONFIG")  md5 $(md5_of "$KCONFIG")"
	else
		echo "kconfig:   (none shipped -- read /proc/config.gz on the device)"
	fi
	for b in diag omcid omcli omciprobe omcicap nv igmpd metricsd confd; do
		[ -f "$STAGE/bin/$b" ] &&
			echo "bin:       $b  $(size "$STAGE/bin/$b") bytes  md5 $(md5_of "$STAGE/bin/$b")"
	done
	for b in dropbearmulti ip bridge; do
		[ -f "$STAGE/sbin/$b" ] &&
			echo "pkg:       $b  $(size "$STAGE/sbin/$b") bytes  md5 $(md5_of "$STAGE/sbin/$b")"
	done
} > "$STAGE/etc/odi-build"
sed 's/^/  /' "$STAGE/etc/odi-build"

# /etc/version, in the shape of the stock one
# (`ODI-260912-33520c6 -- Sat Sep 12 00:39:55 UTC 2026`): confd shows the
# running version from it, up to the first space.
echo "$VERSION -- $(date -u '+%a %d %b %Y %H:%M:%S %Z')" > "$STAGE/etc/version"
echo "  /etc/version: $(cat "$STAGE/etc/version")"

if [ -n "$KCONFIG" ]; then
	cp "$KCONFIG" "$STAGE/etc/kernel-config"
	echo "  + /etc/kernel-config"
elif [ "${ALLOW_NO_KCONFIG:-0}" = 1 ]; then
	echo "  no kernel .config found beside the kernel -- ALLOW_NO_KCONFIG=1, shipping with no /etc/kernel-config" >&2
else
	die "no kernel .config found beside the kernel -- pass KCONFIG=<file> (or build with KERNEL=\$PWD/build/kernel*/uImage so the config sits beside it); /etc/kernel-config records on the device what its kernel was built with -- set ALLOW_NO_KCONFIG=1 to ship without it"
fi

# The root account. A shared default password baked into a public build is
# worth less than nothing, so one is generated per build and printed; pass
# ROOT_PW to choose your own, ROOT_PW=none for an empty password, or
# ROOT_PW=locked for no password at all -- the shape a public release image
# ships with, since a release tarball cannot carry a secret (docs/BUILDING.md,
# "The root password and image versioning"). File writing for all three
# lives in image/gen-root-account.sh, so it is testable on its own
# (test/root_pw_test.sh).
#
# SHA-512 crypt ($6$): the toolchain builds uClibc-ng with the SHA-512 crypt
# (the uclibc toolchain image, toolchain/README.md), and both verifiers on
# the stick use that libc crypt(): dropbear directly, busybox login and su
# by default. Not yescrypt ($y$): uClibc-ng has none, and the yescrypt
# busybox bundles covers only its own applets, so dropbear would see the
# account locked, and ssh is the only way in (docs/ACCESS.md). Rounds stay
# at the default 5000, about 0.2 s per login check on this core (a crypt()
# benchmark under qemu, scaled to its measured BogoMIPS): no case for more
# or fewer.
say "accounts"
PW=${ROOT_PW:-}
case "$PW" in
none)
	"$ROOT/image/gen-root-account.sh" "$STAGE" "$OUT" "$VERSION" none
	echo "  root has NO password (ROOT_PW=none)"
	;;
locked)
	"$ROOT/image/gen-root-account.sh" "$STAGE" "$OUT" "$VERSION" locked
	echo "  root password LOCKED (ROOT_PW=locked) -- keys only, see docs/FLASHING.md"
	;;
*)
	gen=
	if [ -z "$PW" ]; then
		# Bounded read, then cut: `tr < /dev/urandom | head -c` gives tr
		# a SIGPIPE, and under `set -o pipefail` that kills the build.
		PW=$(head -c 256 /dev/urandom | LC_ALL=C tr -dc 'a-zA-Z0-9' | cut -c1-14)
		[ ${#PW} -eq 14 ] || die "could not generate a password"
		gen=" (generated)"
	fi
	HASH=$(run openssl passwd -6 "$PW")
	"$ROOT/image/gen-root-account.sh" "$STAGE" "$OUT" "$VERSION" password "$PW" "$HASH"
	echo "  root password$gen -> out/image/root-password.txt"
	;;
esac

# Every ELF that is about to be squashed, not just the ones we remember to
# name. A binary carrying an instruction this core lacks links clean and traps
# the first time that path runs, on a device with no console -- so this is a
# gate on the image, not a check on the packages.
say "ISA audit of every ELF in the image"
elves=""
while IFS= read -r f; do
	case $(od -An -N4 -tx1 < "$f" | tr -d ' ') in
	7f454c46*) elves="$elves $f" ;;
	esac
done <<EOF
$(find "$STAGE" -type f)
EOF
[ -n "$elves" ] || die "no ELF found in the staged rootfs at all"
# shellcheck disable=SC2086  # deliberate word splitting: one argument per file
"$ROOT/packages/isa-audit.sh" $elves | sed "s/^/  /"
# And the same set the other way round: every mnemonic must be one CONFIRMED
# to execute on this core. The deny list above can only find what it already
# names; this one reports anything new as unverified.
# shellcheck disable=SC2086
"$ROOT/packages/isa-allowlist.sh" $elves | sed "s/^/  /"

say "squashing"
run mksquashfs build/rootfs build/rootfs.img \
	-comp "$COMP" -b "$BS" \
	-pf rootfs/devices.pseudo \
	-always-use-fragments -no-xattrs -noappend -no-sparse -all-root \
	-processors 1 >/dev/null

RSIZE=$(size "$ROOT/build/rootfs.img")
KSIZE=$(size "$KERNEL")
printf '  %-8s %10d / %10d bytes  (%d spare)\n' \
	rootfs "$RSIZE" "$ROOTFS_PART" "$((ROOTFS_PART - RSIZE))"
printf '  %-8s %10d / %10d bytes  (%d spare)\n' \
	uImage "$KSIZE" "$KERNEL_PART" "$((KERNEL_PART - KSIZE))"
over=0
[ "$RSIZE" -gt "$ROOTFS_PART" ] &&
	{ echo "REFUSING: rootfs over the partition by $((RSIZE - ROOTFS_PART)) bytes" >&2; over=1; }
[ "$KSIZE" -gt "$KERNEL_PART" ] &&
	{ echo "REFUSING: uImage over the partition by $((KSIZE - KERNEL_PART)) bytes" >&2; over=1; }
[ "$over" = 1 ] && exit 1

say "packing"
PACK=$ROOT/build/pack
rm -rf "$PACK"; mkdir -p "$PACK"
cp "$ROOT/build/rootfs.img" "$PACK/rootfs"
cp "$KERNEL"                "$PACK/uImage"
install -m 755 "$ROOT/image/fwu.sh" "$PACK/fwu.sh"
printf '%s\n' "$VERSION" > "$PACK/fwu_ver"

# md5.txt covers every member but itself, the format fwu.sh and the stock
# flasher read.
( cd "$PACK" && : > md5.txt
  for f in fwu.sh fwu_ver rootfs uImage; do md5sum "$f" >> md5.txt; done )
sed 's/^/  /' "$PACK/md5.txt"

TAR=$OUT/$VERSION.tar
# GNU tar, in the container, not the host tar. The busybox 1.12.4 of the stock
# image refuses the ustar headers bsdtar (macOS) writes ("corrupted octal value
# in tar header": GNU writes `0000644\0`, bsdtar `000755 \0`) and extracts
# nothing, and this is the tarball unpacked on the stick before fwu.sh.
# test/image_tar_format_test.sh is the gate.
run sh -c "cd '${PACK#"$ROOT"/}' && tar -cf '/work/${TAR#"$ROOT"/}' \
	fwu.sh fwu_ver md5.txt rootfs uImage"
say "done"
ls -l "$TAR"
cat <<EOF

This image is inert when flashed. On the stick:
    ./fwu.sh <slot> $(basename "$TAR")    # writes the slot it is given
    nv setenv sw_tryactive <slot>             # boots it ONCE, self-reverting

Never write sw_commit up front: the one-shot boot is the only free safety net.
EOF
