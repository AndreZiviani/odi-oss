#!/usr/bin/env sh
#
# busybox for the RTL9602C.
#
# This is /bin/sh, so every init script runs through it -- and on a CPU that
# traps on instructions GCC emits by default, a miscompiled busybox is not a
# broken tool, it is a stick that will not boot. Hence the ISA audit at the
# end, which is a gate and not a report.
#
# Built with OUR toolchain (gcc 16.2.0, uClibc-ng 1.0.59) rather than the
# vendor toolchain. That is what pays for the applets the 2010 libc could not link; see
# config.fragment.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)

VERSION=1.38.0
URL=https://busybox.net/downloads/busybox-$VERSION.tar.bz2
SHA256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2

# Built against our sysroot, in the uclibc toolchain image (toolchain/images.env);
# oss-env.sh pulls it on first use and says what to do when it cannot.
IMAGE=$(../oss-env.sh)

tar=$(../fetch.sh "$URL" "$SHA256")

# Incremental: unpacking again on every run turns each one-line config fix into
# a full rebuild. FRESH=1 forces a clean tree.
#
# The stamp is version AND toolchain, because neither can be probed from the
# tree. An object file built by the vendor toolchain looks exactly like one built by our
# gcc until the link fails on a libc symbol -- or, worse, does not fail and
# produces a binary linked against two libcs.
work=$ROOT/build/busybox
stamp="busybox-$VERSION oss"
if [ "${FRESH:-0}" = 1 ] || [ "$(cat "$work/.built-with" 2>/dev/null || true)" != "$stamp" ]; then
	rm -rf "$work"; mkdir -p "$work"
	tar xf "$tar" -C "$work" --strip-components=1
	printf '%s\n' "$stamp" > "$work/.built-with"
fi

# The published toolchain image is linux/amd64; an arm64 host runs it under
# emulation, or builds it natively as toolchain/README.md describes.
docker run --rm \
	-v "$work:/src" \
	-v "$ROOT/packages/busybox:/cfg:ro" \
	-w /src "$IMAGE" bash -c '
set -e
export PATH=/opt/oss/bin:$PATH
export CROSS_COMPILE=mips-linux-uclibc-
export ARCH=mips

make defconfig >/dev/null
# Apply the device fragment: drop any existing setting for each symbol, then
# append the new line. make oldconfig then resolves dependencies.
FRAGS=/cfg/config.fragment
for frag in $FRAGS; do
	grep -E "^(#? ?CONFIG_[A-Z0-9_]+)" "$frag" | while read -r line; do
		sym=$(echo "$line" | sed -E "s/^# ?//; s/[ =].*//")
		sed -i "/^${sym}=/d; /^# ${sym} is not set/d" .config
		echo "$line" >> .config
	done
done

# The Lexra flags come from CONFIG_EXTRA_CFLAGS in config.fragment, not from a
# CFLAGS_EXTRA= on this command line. Passing them here looks right and does
# nothing: busybox kbuild does not thread CFLAGS_EXTRA into the per-file
# compile, which `make V=1` shows, and the flags were silently dropped.
make oldconfig >/dev/null 2>&1 || true

# Report what oldconfig actually did with the fragment. A symbol whose
# dependencies are unmet is dropped silently, and a dropped applet is
# indistinguishable from one that was never asked for -- which is precisely
# how the missing-applet trap on this device works in the first place.
echo "==> fragment symbols as they landed in .config"
grep -E "^(#? ?CONFIG_[A-Z0-9_]+)" /cfg/config.fragment | while read -r line; do
	sym=$(echo "$line" | sed -E "s/^# ?//; s/[ =].*//")
	got=$(grep -E "^(${sym}=|# ${sym} is not set)" .config || echo "  ${sym} ABSENT")
	want=$(echo "$line" | sed -E "s/^# ?//")
	case "$got" in
		*ABSENT*) echo "    DROPPED  $want" ;;
	esac
done

make -j4 busybox > /src/build.log 2>&1 || {
	echo "BUILD FAILED -- last errors:"; grep -nE "Error|error:|undefined" /src/build.log | tail -25; exit 1; }
tail -3 /src/build.log
mips-linux-uclibc-strip busybox
ls -la busybox

# Static is a build requirement, not a preference: this image guarantees
# nothing about a shared libc on the target. A dynamic busybox would boot
# until the first missing .so, which on this device means never.
if mips-linux-uclibc-readelf -l busybox | grep -q INTERP; then
	echo "REFUSING: busybox has a PT_INTERP -- it is not static" >&2
	exit 1
fi

# The applets the vendor libc (uClibc 0.9.30.3) and kernel (2.6.30) could not
# provide are the return on the toolchain and kernel work, so prove each one
# twice rather than trusting the .config: the object exists (it compiled) and
# its name is in the applet table of the stripped binary (reachable at run
# time).
echo "==> applets the vendor image could not have"
fail=0
for pair in networking/nslookup.o:nslookup util-linux/fallocate.o:fallocate \
            util-linux/unshare.o:unshare util-linux/nsenter.o:nsenter \
            coreutils/sync.o:sync miscutils/seedrng.o:seedrng coreutils/mktemp.o:mktemp; do
	obj=${pair%%:*}; name=${pair##*:}
	if [ -f "$obj" ] && strings busybox | grep -qx "$name"; then
		echo "    ok       $name"
	else
		echo "    MISSING  $name (object $obj)"
		fail=1
	fi
done
[ "$fail" = 0 ] || { echo "REFUSING: an applet this build exists to restore did not land" >&2; exit 1; }
if true; then
	echo "==> applets a newer kernel unlocks over the vendor 2.6.30 one"
	for pair in util-linux/nsenter.o:nsenter coreutils/sync.o:sync miscutils/seedrng.o:seedrng; do
		obj=${pair%%:*}; name=${pair##*:}
		if [ -f "$obj" ] && strings busybox | grep -qx "$name"; then echo "    ok       $name"; else echo "    MISSING  $name"; fail=1; fi
	done
	grep -q "^CONFIG_FEATURE_SYNC_FANCY=y" .config && echo "    ok       sync -d/-f (FEATURE_SYNC_FANCY)" || { echo "    MISSING  sync -d/-f"; fail=1; }
	[ "$fail" = 0 ] || { echo "REFUSING: nsenter / sync -f did not land on this build" >&2; exit 1; }
fi

echo "==> applets enabled: $(grep -cE "^CONFIG_[A-Z0-9_]+=y" .config)"
'
mkdir -p "$ROOT/out"
OUTBB=$ROOT/out/busybox
cp "$work/busybox" "$OUTBB"
../isa-audit.sh "$OUTBB"

raw=$(wc -c < "$OUTBB" | tr -d ' ')
xz=$(xz -9 -c "$OUTBB" | wc -c | tr -d ' ')
echo "busybox at $OUTBB: $raw bytes, $xz compressed (xz -9)"
