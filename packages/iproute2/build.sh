#!/usr/bin/env sh
#
# iproute2 for the RTL9602C: the real `ip`, plus `bridge`.
#
# busybox's cut-down `ip` is switched off in ../busybox/config.fragment, so
# this is the only `ip` the image has. `bridge` comes with it, and is why no bridge-utils package exists
# here; see ../bridge-utils/README.md.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)

# 7.2.0, and the cap that held this at 4.9.0 for its whole life is gone.
#
# The old note here recorded a -fsyntax-only sweep of lib/, ip/ and bridge/
# against the vendor prebuilt toolchain gcc 4.4.5: 4.9.0 was the last release where 0 of 91 files
# failed, and 7.2.0 failed 18. That residue was never a libc problem and could
# not be configured away -- gcc 4.4.5 has no C11 anonymous struct or union
# members, and modern uapi headers are full of them (linux/bpf.h:108 among
# others). ip/ip.c includes bpf_util.h and ip/iplink_bridge.c fails on an
# anonymous-member initialiser, so on that toolchain there was no modern
# iproute2 with or without patches we were willing to carry.
#
# gcc 16.2.0 compiles all of it. This is the ceiling the toolchain work was
# supposed to lift, and it lifted.
VERSION=7.2.0
URL=https://mirrors.edge.kernel.org/pub/linux/utils/net/iproute2/iproute2-$VERSION.tar.xz
SHA256=4c2fa124c2cf0afd7ca34d1eeacba6ba048a56f6374e2aab93dafbdbd4eea9c0

# Built against our sysroot, in the uclibc toolchain image (toolchain/images.env);
# oss-env.sh pulls it on first use and says what to do when it cannot.
IMAGE=$(../oss-env.sh)

tar=$(../fetch.sh "$URL" "$SHA256")

# Incremental; FRESH=1 forces a clean tree. The stamp is version AND toolchain
# for the reason busybox has one: an object built by the other compiler looks
# identical until the link fails, or does not fail.
work=$ROOT/build/iproute2
stamp="iproute2-$VERSION oss"
if [ "${FRESH:-0}" = 1 ] || [ "$(cat "$work/.built-with" 2>/dev/null || true)" != "$stamp" ]; then
	rm -rf "$work"; mkdir -p "$work"
	tar xf "$tar" -C "$work" --strip-components=1
	printf '%s\n' "$stamp" > "$work/.built-with"
fi

# The published toolchain image is linux/amd64; an arm64 host runs it under
# emulation, or builds it natively as toolchain/README.md describes.
docker run --rm \
	-v "$work:/src" \
	-v "$ROOT/packages/iproute2:/cfg:ro" \
	-w /src "$IMAGE" bash -c '
set -e
export PATH=/opt/oss/bin:$PATH

# mips2 rather than -march=5281, the vendor prebuilt toolchain spelling of
# this core. ll/sc/sync are legal here, measured on the device, so mips2 buys
# native atomics; beql, bnel and the teq emitted for divide-by-zero are the
# three MIPS-II instructions that are not, and the other two flags suppress
# them. ../isa-audit.sh is what proves it.
LEXRA_CFLAGS="-march=mips2 -mno-branch-likely -mdivide-breaks -Os -pipe"

# NOTHING is defined on top of the headers any more.
#
# The 4.9.0 build needed -DSOCK_CLOEXEC=02000000 -DSOCK_NONBLOCK=0x0080,
# because libnetlink.c uses both unconditionally and uClibc 0.9.30.3s headers
# had neither. uClibc-ng 1.0.59 declares them in bits/socket_type.h, and
# SOCK_NONBLOCK is 00000200 there -- the MIPS value, which is the one that
# matters: the generic 04000 copied from another architecture sets the wrong
# bit and nothing complains.
#
# unshare, setns and name_to_handle_at come from the libc now (this
# sysroot kernel headers define their syscall numbers, so uClibc-ng
# builds the wrappers);
# nftw comes from the libc too (UCLIBC_HAS_FTW). The two headers this SYSROOT
# still does not have sit on the include path rather than in the tree:
#   netinet/icmp6.h     uClibc-ng built without IPv6, because this kernel has
#                       none either
#   kernel-compat.h     __kernel_old_time_t, a type Linux 5.6 renamed into
#                       existence, force-included
# One line on purpose: a newline inside this variable survives into CFLAGS and
# splits the compile recipe in two, which fails as "no input files".
COMPAT_DEFS="-I/cfg/compat -include /cfg/compat/kernel-compat.h"

# PKG_CONFIG=/bin/false is the point of the whole configure line. iproute2
# probes with the HOST pkg-config, and left alone it happily finds the
# containers x86 libtirpc and writes -ltirpc into the config for a MIPS link.
# With it forced to fail, every optional dependency comes out off -- which is
# what we want anyway:
#   libelf, libbpf   BPF/XDP. Nothing on an ONU loads BPF programs.
#   libmnl           only used by devlink/rdma/tc, none of which we build.
#   libtirpc         RPC, for tc. Not built.
#   SELinux, libcap, Berkeley DB  not on this device, and not wanted on it.
# configure is the expensive half, so only run it once.
if [ ! -f config.mk ]; then
	CC=mips-linux-uclibc-gcc AR=mips-linux-uclibc-ar \
	PKG_CONFIG=/bin/false \
		./configure > /src/configure.log 2>&1 || {
		echo "CONFIGURE FAILED -- last errors:"; tail -25 /src/configure.log; exit 1; }
fi
echo "==> configure results"
grep -vE "^#" config.mk | grep -E "=" | sed "s/^/    /"

# SUBDIRS: lib, ip and bridge only. tc needs flex/bison and a kernel with
# traffic-control modules this one is configured without; genl, misc, tipc,
# devlink, rdma and dcb are netlink families this kernel does not have.
# SHARED_LIBS=n drops -ldl and -Wl,-export-dynamic and turns on iproute2 own
# static symbol table -- required, because a static link cannot dlopen.
# Built by hand and handed to the linker through LDFLAGS rather than through
# iproute2 ADDLIB: ADDLIB is accumulated with += in the top Makefile
# (dnet/ipx/mpls ntop+pton), so setting it on the command line REPLACES that
# list and the link dies on five unrelated undefined symbols.
make -j4 SUBDIRS="lib ip bridge" SHARED_LIBS=n \
	CCOPTS="$LEXRA_CFLAGS $COMPAT_DEFS" \
	LDFLAGS="-static" \
	> /src/build.log 2>&1 || {
	echo "BUILD FAILED -- last errors:"; grep -nE "Error|error:|undefined" /src/build.log | tail -25; exit 1; }

for b in ip/ip bridge/bridge; do
	mips-linux-uclibc-strip "$b"
	# Static is a build requirement, not a preference: this image guarantees
	# nothing about a shared libc on the target.
	if mips-linux-uclibc-readelf -l "$b" | grep -q INTERP; then
		echo "REFUSING: $b has a PT_INTERP -- it is not static" >&2
		exit 1
	fi
	ls -la "$b"
done
'
mkdir -p "$ROOT/out"
cp "$work/ip/ip" "$ROOT/out/ip"
cp "$work/bridge/bridge" "$ROOT/out/bridge"

../isa-audit.sh "$ROOT/out/ip" "$ROOT/out/bridge"

for b in ip bridge; do
	raw=$(wc -c < "$ROOT/out/$b" | tr -d ' ')
	xz=$(xz -9 -c "$ROOT/out/$b" | wc -c | tr -d ' ')
	echo "$b at out/$b: $raw bytes, $xz compressed (xz -9)"
done
