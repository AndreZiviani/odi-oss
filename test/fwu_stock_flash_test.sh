#!/usr/bin/env bash
#
# The FIRST flash, run for real: the stock vendor busybox as the shell, the
# stock /bin as the only tools, the actual image tarball, and fwu.sh carried
# all the way through the erase and the write.
#
# This is the one path that cannot be exercised any other way. The other fwu
# harnesses each supply an interpreter whose completeness is the thing in
# question -- fwu_guard_test.sh runs the script under the HOST's bash, and
# flash_harness_test.sh runs it under OUR busybox inside QEMU. Both were green
# while fwu.sh called `head`, `wc`, `dd` and `command -v`, none of which exists
# on the image it is flashed FROM, and while the tarball itself could not be
# unpacked there at all.
#
# Two things are deliberate:
#
#   * The fake partitions are LOOP DEVICES, not regular files. Writing to a
#     regular file truncates it, so the partition would shrink to the size of
#     what was written -- and the read-back's `cmp` would take its equal-size
#     arm instead of the real one, where the member ends first and the tail is
#     still erased 0xff.
#   * flash_eraseall and nv are stubs. Everything else is the vendor's own.
#
# It needs the stock rootfs, which is disposable scratch, and a privileged
# container for losetup and binfmt_misc. When either
# is missing this SKIPS -- loudly, and with a non-zero note in the summary, so
# a skip can never be read as a pass.
set -u
cd "$(dirname "$0")/.." || exit 1

STICK=${STICK_ROOTFS:-${STOCK_ROOTFS:-stock-rootfs}}
TARDIR=$PWD/out/image

skip() { printf 'SKIP  %s\n' "$1"; printf '\nskipped -- this path was NOT verified\n'; exit 0; }

[ -x "$STICK/bin/busybox" ] ||
	skip "no stock rootfs at $STICK (set STICK_ROOTFS); the first-flash path is unverified"
command -v docker >/dev/null 2>&1 ||
	skip "docker is not available; the first-flash path is unverified"

TAR=""
for f in "$TARDIR"/*.tar; do
	case $f in
	*smoketest*) continue ;;
	*'*'*) continue ;;
	esac
	[ -z "$TAR" ] || [ "$f" -nt "$TAR" ] && TAR=$f
done
[ -n "$TAR" ] || skip "no image tarball in $TARDIR -- run image/build.sh first"

echo "stock rootfs: $STICK"
echo "tarball:      $TAR"

docker run --rm --privileged --platform linux/amd64 \
	-v "$STICK:/stick:ro" -v "$TARDIR:/tars:ro" -v "$PWD/test:/t:ro" \
	debian:bookworm-slim bash /t/fwu_stock_flash_inner.sh "$(basename "$TAR")"
rc=$?

if [ "$rc" -eq 0 ]; then
	printf '\nthe first flash works on the stock image\n'
else
	printf '\nFAILED -- the first flash does not work on the stock image\n'
fi
exit "$rc"
