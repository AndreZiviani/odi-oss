#!/usr/bin/env sh
#
# dropbear for the RTL9602C.
#
# The stock firmware ships dropbear 0.48 (2007), which offers only
# diffie-hellman-group1-sha1 / hmac-sha1 / ssh-rsa -- all refused by default by
# OpenSSH 8.8 and later, so the vendor image cannot be reached from a current
# client without downgrading it. This build brings curve25519, ed25519,
# chacha20-poly1305 and sntrup761.
#
# Built with OUR toolchain (gcc 16.2.0, uClibc-ng 1.0.59) and audited
# afterwards, like everything else here that links a libc.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)

# 2026.94 confirmed still the newest release on
# https://matt.ucc.asn.au/dropbear/releases/ at the time of this build.
VERSION=2026.94
URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-$VERSION.tar.bz2
# The upstream host is one person machine. dropbear.nl carries the same
# releases and answered on 2026-09-15; the sha256 below means it does not
# matter which one we get it from.
ALT_URLS="https://dropbear.nl/mirror/releases/dropbear-$VERSION.tar.bz2"
export ALT_URLS
SHA256=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81

# Built against our sysroot, in the uclibc toolchain image (toolchain/images.env);
# oss-env.sh pulls it on first use and says what to do when it cannot.
IMAGE=$(../oss-env.sh)

tar=$(../fetch.sh "$URL" "$SHA256")

# Incremental: unpacking again on every run turns each one-line fix into a full
# rebuild, configure included. FRESH=1 forces a clean tree. The stamp is
# version AND toolchain, for the reason busybox has one.
work=$ROOT/build/dropbear
stamp="dropbear-$VERSION oss"
if [ "${FRESH:-0}" = 1 ] || [ "$(cat "$work/.built-with" 2>/dev/null || true)" != "$stamp" ]; then
	rm -rf "$work"; mkdir -p "$work"
	tar xf "$tar" -C "$work" --strip-components=1
	printf '%s\n' "$stamp" > "$work/.built-with"
fi

# The published toolchain image is linux/amd64; an arm64 host runs it under
# emulation, or builds it natively as toolchain/README.md describes.
docker run --rm \
	-v "$work:/src" \
	-v "$ROOT/packages/dropbear:/cfg:ro" \
	-w /src "$IMAGE" bash -c '
set -e
export PATH=/opt/oss/bin:$PATH
SYSROOT=/opt/oss/mips-linux-uclibc/sysroot

# mips2 rather than -march=5281, the vendor prebuilt toolchain spelling of
# this core. ll/sc/sync are legal here, measured on the device; beql, bnel and
# the teq emitted for divide-by-zero are not, and the other two flags suppress
# them.
LEXRA_CFLAGS="-march=mips2 -mno-branch-likely -mdivide-breaks -Os"

cp /cfg/localoptions.h localoptions.h

# The libc provides openpty() (libutil) and the stack-protector runtime
# (UCLIBC_HAS_SSP) since 2026-09-21; both used to be worked around here with a
# --disable-openpty and a constant-canary ssp-compat.c. Checked rather than
# assumed, so a toolchain built from an older script fails loudly:
for sym in openpty __stack_chk_fail; do
	mips-linux-uclibc-nm "$SYSROOT"/usr/lib/*.a 2>/dev/null | grep -qw "T $sym" ||
		{ echo "libc has no $sym -- the toolchain image predates it (toolchain/images.env)" >&2; exit 1; }
done
echo "==> openpty and __stack_chk_fail: provided by the libc"

# configure is the expensive half of this build, so only run it once.
if [ ! -f config.status ]; then
	# What else is turned off, and why. Every one is a property of the
	# device, not a convenience:
	#
	#   --disable-lastlog, --disable-utmp/utmpx, --disable-wtmp/wtmpx,
	#   --disable-pututline, --disable-pututxline
	#       Login accounting. uClibc-ng here is built without utmpx, and
	#       there is no /var/log on a read-only squashfs to write records
	#       to anyway. busybox is configured the same way.
	#
	#   --disable-zlib
	#       Optional compression nothing here needs, and a library we would
	#       otherwise have to cross-build and carry for it.
	./configure --host=mips-linux-uclibc \
		--disable-zlib \
		--disable-lastlog --disable-utmp --disable-utmpx \
		--disable-wtmp --disable-wtmpx \
		--disable-pututline --disable-pututxline \
		CC=mips-linux-uclibc-gcc \
		CFLAGS="$LEXRA_CFLAGS" \
		> /src/configure.log 2>&1 || {
		echo "CONFIGURE FAILED -- last errors:"; tail -25 /src/configure.log; exit 1; }
fi

# MULTI=1: one binary with symlinks, because two copies of the crypto does not
# fit a 2.5 MB partition. STATIC=1 for the same reason busybox is static --
# this image guarantees nothing about a shared libc.
# scp is the dropbear one: the legacy scp protocol only, no SFTP subsystem,
# so a modern OpenSSH client (9.0+) must ask for it with `scp -O`.
make -j4 PROGRAMS="dropbear dropbearkey scp" MULTI=1 STATIC=1 \
	> /src/build.log 2>&1 || {
	echo "BUILD FAILED -- last errors:"; grep -nE "Error|error:|undefined" /src/build.log | tail -25; exit 1; }
mips-linux-uclibc-strip dropbearmulti
if mips-linux-uclibc-readelf -l dropbearmulti | grep -q INTERP; then
	echo "REFUSING: dropbearmulti has a PT_INTERP -- it is not static" >&2
	exit 1
fi
ls -la dropbearmulti
'
mkdir -p "$ROOT/out"
cp "$work/dropbearmulti" "$ROOT/out/dropbearmulti"

../isa-audit.sh "$ROOT/out/dropbearmulti"

raw=$(wc -c < "$ROOT/out/dropbearmulti" | tr -d ' ')
xz=$(xz -9 -c "$ROOT/out/dropbearmulti" | wc -c | tr -d ' ')
echo "dropbearmulti at out/dropbearmulti: $raw bytes, $xz compressed (xz -9)"
