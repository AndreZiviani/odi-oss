#!/usr/bin/env bash
#
# Assemble a flashable image: stage the rootfs, squash it, and tar it with the
# kernel, our fwu.sh and the md5.txt the flasher checks.
#
# The output has the same five members as the vendor image, because the on-device
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

# Read off the stock image superblock rather than assumed: squashfs 4.0,
# LZMA, 1 MiB blocks. LZMA (not XZ) at version 4.0 is unusual -- mainline
# squashfs dropped it -- but the vendor kernel this device shipped with read
# it that way. Upstream squashfs has no LZMA, so xz is what this (6.18)
# kernel reads; BS is smaller than the vendor 1 MiB because 6.18 squashfs
# keeps caches of one block each.
COMP=${COMP:-xz}
BS=${BS:-262144}

TOOLS=odi-oss-image
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

[ -n "$KERNEL" ] && [ -f "$KERNEL" ] ||
	die "no kernel: run 'make kernel', or pass KERNEL=<uImage>"
[ -f "$BUSYBOX" ] || die "no busybox at $BUSYBOX -- run 'make busybox'"

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
	# and inittab want /sbin/init, and our own scripts spell out /sbin/mdev,
	# /sbin/telnetd, /sbin/ifconfig and /sbin/dropbear.
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

# /tmp is a SYMLINK, not a directory, and this is the stock vendor layout.
# A real directory here lands in the squashfs and is therefore read-only for
# the life of the image -- every temp file any program writes fails with
# EROFS. /var is the ramfs rcS mounts, and rcS creates /var/tmp before it
# starts anything, so the only window in which this link dangles is between
# the kernel handing off to init and rcS line 21, and nothing writes a temp
# file in it. Proven by test/rootfs_chroot_test.sh.
#
# Made AFTER the skeleton copy and over the top of whatever is there: an empty
# rootfs/skeleton/tmp is invisible to git, so a stale one can exist in a
# working tree and not in a fresh clone, and `cp -a` onto a symlink fails with
# "Not a directory". A build must not depend on which of the two you have.
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
# confd execve()s /bin/omcicli for its OMCI page (odi-ui confd.h
# OMCICLI_PATH); omcli speaks the vendor queue protocol with the vendor argv
# (mib get, dump ...), so the vendor name resolves to it.
[ -f "$STAGE/bin/omcli" ] && ln -sf omcli "$STAGE/bin/omcicli"


# confd reads /etc/confd/ for every page it serves and for its four .tsv data
# files, so the binary alone is a daemon that answers 404 to everything. Refuse
# that combination rather than ship it: a UI that listens and serves nothing is
# harder to diagnose than no UI at all.
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

# The GPON datapath has no kernel modules to stage any more: CONFIG_ODI_SWITCH=y
# (kernel/618/config, unconditional) builds odi_switch.c/odi_switch_{cmd,dal,tbl}.c
# straight into vmlinux, and src/omci/respond/apply.c own omci_drv_call() reaches
# it over its own ODI_OMCI_OP_CMD netlink socket. Nothing this repo builds can
# ever produce a loadable .ko for this datapath again.
say "kernel modules"
echo "  none -- odi_switch is built into vmlinux (CONFIG_ODI_SWITCH=y, kernel/618/config)"

# The upstream packages. dropbear is a MULTI build that dispatches on argv[0],
# so the two names it answers to have to be links to it -- etc/init.d/services
# calls /sbin/dropbearkey and then /sbin/dropbear by absolute path.
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

# What this image is made of, on the image. Two independent records, because
# they answer different questions and can disagree:
#
#   /proc/config.gz     what the RUNNING kernel was built from. Authoritative,
#                       produced by the kernel itself (CONFIG_IKCONFIG_PROC),
#                       and correct even if the rootfs came from another build.
#   /etc/kernel-config  what THIS image shipped a kernel built from. Readable
#                       without booting, and a mismatch against /proc/config.gz
#                       is exactly the signal that the two partitions were
#                       flashed from different builds.
#
# The stock kernel has neither: working out its configuration meant counting
# strings in the decompressed image.
say "build manifest"
{
	# key=value lines first: the exporter turns every line with an equals
	# sign into a gpon_image_info label (metrics_body.h metric_image_info),
	# and image verification tooling reads image=. The descriptive lines
	# below carry no equals sign and are skipped by both.
	echo "image=$VERSION"
	echo "built=$(date -u '+%Y-%m-%dT%H:%MZ')"
	# The kernel release (6.18.53), read from the config header the kernel build
	# writes; the exporter publishes it as gpon_image_info{kernel=...}. Falls
	# back to the uImage file name when no config is staged.
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

# /etc/version, in the vendor format, because things read it.
#
# confd serves the running firmware version from this file and reads only up to
# the first space or newline (firmware.c:35-41). Without it the firmware page
# shows an empty string, which during a trial boot reads as "the UI is broken"
# rather than "nobody wrote the file". The vendor line is
#
#     ODI-260912-33520c6 -- Sat Sep 12 00:39:55 UTC 2026
#
# so the same shape keeps anything else that parses it working.
echo "$VERSION -- $(date -u '+%a %d %b %Y %H:%M:%S %Z')" > "$STAGE/etc/version"
echo "  /etc/version: $(cat "$STAGE/etc/version")"

if [ -n "$KCONFIG" ]; then
	cp "$KCONFIG" "$STAGE/etc/kernel-config"
	echo "  + /etc/kernel-config"
elif [ "${ALLOW_NO_KCONFIG:-0}" = 1 ]; then
	echo "  no kernel .config found beside the kernel -- ALLOW_NO_KCONFIG=1, shipping with no /etc/kernel-config (rcS will skip platform init and omcid)" >&2
else
	die "no kernel .config found beside the kernel -- pass KCONFIG=<file> (or build with KERNEL=\$PWD/build/kernel*/uImage so the config sits beside it); /etc/kernel-config gates rcS platform init and omcid, so a build without it silently loses both -- set ALLOW_NO_KCONFIG=1 if that is really what you want"
fi

# The root account. A shared default password baked into a public build is
# worth less than nothing, so one is generated per build and printed; pass
# ROOT_PW to choose your own, or ROOT_PW=none for an empty password.
#
# MD5 crypt ($1$), not SHA-512: the libc here is uClibc 0.9.30.3 from 2010 and
# its crypt() does DES and MD5 only. A $6$ hash would never match and the
# account would simply be unloginable.
say "accounts"
mkdir -p "$OUT"
PW=${ROOT_PW:-}
if [ "$PW" = none ]; then
	echo 'root::0:0:root:/root:/bin/sh' > "$STAGE/etc/passwd"
	rm -f "$OUT/root-password.txt"
	echo "  root has NO password (ROOT_PW=none)"
else
	gen=
	if [ -z "$PW" ]; then
		# Bounded read, then cut: `tr < /dev/urandom | head -c` gives tr
		# a SIGPIPE, and under `set -o pipefail` that kills the build.
		PW=$(head -c 256 /dev/urandom | LC_ALL=C tr -dc 'a-zA-Z0-9' | cut -c1-14)
		[ ${#PW} -eq 14 ] || die "could not generate a password"
		gen=" (generated)"
	fi
	HASH=$(run openssl passwd -1 "$PW")
	echo "root:$HASH:0:0:root:/root:/bin/sh" > "$STAGE/etc/passwd"
	printf '%s\n' "$PW" > "$OUT/root-password.txt"
	chmod 600 "$OUT/root-password.txt"
	# And a copy named for this version: the file above is overwritten by the
	# next build, and on 2026-09-16 that locked us out of the image that was
	# still running on the stick (p15, two hours into a soak) the moment p16
	# was built. Same mode, same directory, never cleaned by the build.
	printf '%s\n' "$PW" > "$OUT/root-password-$VERSION.txt"
	chmod 600 "$OUT/root-password-$VERSION.txt"
	echo "  root password$gen -> out/image/root-password.txt"
fi
chmod 644 "$STAGE/etc/passwd"

# The same device nodes again, as a script this time.
#
# rcS mounts a tmpfs on /dev, which HIDES the nodes mksquashfs bakes in from
# the same file -- so without this the running image has no /dev/null, no
# /dev/console, no /dev/ttyS0 and no /dev/urandom, and dropbearkey fails
# silently into a redirection that also fails. Generated rather than
# hand-written so the list cannot drift from devices.pseudo.
say "generating make-devices.sh from devices.pseudo"
{
	echo "#!/bin/sh"
	echo "#"
	echo "# GENERATED by image/build.sh from rootfs/devices.pseudo. Do not edit."
	echo "#"
	echo "# Run by rcS after the tmpfs is mounted on /dev, because that mount"
	echo "# hides the identical nodes baked into the squashfs. mdev -s cannot"
	echo "# do this job on Linux 2.6.30: it names every node MAJOR:MINOR"
	echo "# because the kernel emits no DEVNAME in the uevent."
	echo "#"
	echo "# No set -e: a node that cannot be made must not stop the rest."
	echo ""
	echo "# /dev/null FIRST, and WITHOUT a redirection. Every line below ends"
	echo "# 2>/dev/null, and on the empty tmpfs rcS has just mounted that"
	echo "# redirection CREATES /dev/null as an ordinary empty file before"
	echo "# mknod runs -- after which the guard on the real line is satisfied"
	echo "# and the node is never made. uClibc daemon() then fstats it, finds"
	echo "# it is not a character device and fails with ENODEV, which is how"
	echo "# dropbear died silently on every image built since 2026-09-14."
	echo "[ -c /dev/null ] || { rm -f /dev/null; mknod -m 666 /dev/null c 1 3; }"
	echo ""
	# -c and -b, not -e. A regular file satisfies -e, which is what let the
	# above go unnoticed: the guard could not tell a missing node from a
	# file standing where the node should be.
	awk '/^\/dev\// {
		t = ($2 == "b") ? "-b" : "-c"
		printf "[ %s %s ] || mknod -m %s %s %s %s %s 2>/dev/null\n", \
		       t, $1, $3, $1, $2, $6, $7
	}' "$ROOT/rootfs/devices.pseudo"
	echo ""
	echo "# Report, so the caller in rcS is not checking an exit code that is"
	echo "# always 0. These four are the ones whose absence is silent and fatal:"
	echo "# no urandom means no host key and no ssh, no console means init has"
	echo "# no stdio, no null breaks every redirection in services."
	echo "missing=\"\""
	echo "for n in console null urandom ttyS0; do"
	echo "	[ -c \"/dev/\$n\" ] || missing=\"\$missing \$n\""
	echo "done"
	echo "if [ -n \"\$missing\" ]; then"
	echo "	echo \"make-devices: MISSING:\$missing\" >&2"
	echo "	exit 1"
	echo "fi"
	echo ""
	echo "exit 0"
} > "$STAGE/etc/scripts/make-devices.sh"
chmod 755 "$STAGE/etc/scripts/make-devices.sh"
n=$(grep -c mknod "$STAGE/etc/scripts/make-devices.sh")
[ "$n" -ge 30 ] || die "make-devices.sh has only $n nodes, devices.pseudo did not parse"
echo "  $n device nodes"

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

# md5.txt covers every member except itself. That is the format fwu.sh reads,
# and it is the format the vendor flasher reads.
( cd "$PACK" && : > md5.txt
  for f in fwu.sh fwu_ver rootfs uImage; do md5sum "$f" >> md5.txt; done )
sed 's/^/  /' "$PACK/md5.txt"

TAR=$OUT/$VERSION.tar
# GNU tar, in the container -- NOT the host's. macOS ships bsdtar, and busybox
# 1.12.4 on the stock vendor image REFUSES its POSIX ustar headers outright:
#
#     tar: corrupted octal value in tar header
#
# and extracts nothing at all. Measured against the stock busybox under
# qemu-mips; a GNU-made tarball extracts cleanly on that same binary. The
# difference is the numeric fields -- GNU writes `0000644\0`, bsdtar writes
# `000755 \0`, and bsdtar leaves size and mtime terminated by a bare space --
# together with `ustar\0`/`00` where busybox wants `ustar `/` \0`.
#
# This is the tarball the user unpacks on the stick before running fwu.sh, so
# a host-format tarball makes the whole procedure impossible, on an image that
# otherwise looks finished. test/image_tar_format_test.sh is the gate.
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
