#!/usr/bin/env bash
#
# Build a cross toolchain for this device from current free software:
# binutils, gcc and uClibc-ng, replacing the vendor prebuilt toolchain (gcc 4.4.5 and uClibc
# 0.9.30.3, both from 2010).
#
# WHY, since the vendor toolchain works: its libc has holes that cost real functionality.
# busybox had to give up nslookup, fallocate, nsenter, unshare and sync -F for
# missing symbols; dropbear needs openpty disabled; iproute2 needed a
# hand-written unshare wrapper; and root passwords are stuck on MD5 crypt
# because that libc has no SHA-512. gcc 4.4.5 is a second, independent ceiling:
# it cannot compile C11 anonymous struct or union members at all, which is what
# stops iproute2 newer than 5.10.
#
# THE TARGET ISA, which is the whole difficulty. The RLX5281 is a Lexra core
# that implements ISA levels in pieces. Measured by executing each instruction
# on the device:
#
#     ok:       lwl lwr swl swr, movz movn, ll sc sync, bltzl, madd
#     ILLEGAL:  mul, clz, teq, beql, bnel
#
# So the target is mips2 -- which gives native ll/sc atomics -- with the three
# illegal MIPS-II instructions suppressed by flags:
#
#     -march=mips2        no mul, no clz (those are SPECIAL2/mips32)
#     -mno-branch-likely  no beql/bnel
#     -mdivide-breaks     break instead of teq for divide-by-zero
#
# THE TRAP THAT SANK THE LAST ATTEMPT: those flags must reach the TARGET
# LIBRARIES, not just the final compile. A crosstool-NG build with the flags
# applied only to the application left 64 teq in libgcc.a and 95 in libc.a.
# Here they are baked into the gcc configuration (--with-arch) AND passed as
# CFLAGS_FOR_TARGET, and toolchain/audit-toolchain.sh checks the built
# libraries rather than trusting either.
#
# Kernel headers come from the device kernel tree itself, so no header can
# declare a syscall this kernel does not have.
#
# Runs natively -- unlike the vendor prebuilt toolchain, whose binaries are 32-bit x86 and need an
# emulated amd64 container.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)

# Everything below runs in a Linux container, for three reasons, all of which
# were learned by running it on the host first:
#
#  1. headers_install builds scripts/unifdef, whose strlcpy prototype
#     collides with the one in the macOS SDK.
#  2. uClibc-ng expects a Linux build host throughout.
#  3. A Darwin-hosted cross compiler is not reproducible for anyone else, and
#     every consumer of this toolchain already builds in a container.
#
# The prefix lives in a named VOLUME rather than a bind mount: the build writes
# hundreds of thousands of small files, and a macOS bind mount is both slow and
# case-insensitive.
# The sysroot is built against the 6.18 kernel headers (kernel/build.sh
# leaves the tree tarball in build/kernel-618/tree.tar). The volume keeps its
# historical name; it is only a Docker volume.
VOLUME=${VOLUME:-odi-oss-toolchain-318}
IMAGE=odi-oss-toolchain-builder

if [ "${IN_CONTAINER:-0}" != 1 ]; then
	KTAR=${KTAR:-$ROOT/build/kernel-618/tree.tar}
	[ -f "$KTAR" ] || {
		echo "no kernel tree tarball at $KTAR" >&2
		echo "run kernel/build.sh first -- it makes one" >&2
		exit 1
	}
	docker build -q -t "$IMAGE" -f "$ROOT/toolchain/Dockerfile.oss" \
		"$ROOT/toolchain" >/dev/null
	docker volume create "$VOLUME" >/dev/null
	mkdir -p "$ROOT/build/toolchain/dl"
	exec docker run --rm \
		-v "$VOLUME:/opt/oss" \
		-v "$ROOT/toolchain:/src:ro" \
		-v "$ROOT/build/toolchain/dl:/dl" \
		-v "$KTAR:/tree.tar:ro" \
		-e IN_CONTAINER=1 -e "JOBS=${JOBS:-4}" \
		-w /work "$IMAGE" bash /src/build-oss-toolchain.sh
fi

BINUTILS=2.47
GCC=16.2.0
UCLIBC=1.0.59

TARGET=mips-linux-uclibc
PREFIX=${PREFIX:-/opt/oss}
SYSROOT=$PREFIX/$TARGET/sysroot
WORK=${WORK:-/opt/oss/work}   # in the volume, so a failed run resumes
JOBS=${JOBS:-4}
KTREE=$WORK/ktree

# The target flags, in one place. TFLAGS reaches every target library.
TARCH="-march=mips2 -mno-branch-likely -mdivide-breaks"
TFLAGS="$TARCH -Os -mabi=32 -EB -msoft-float"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { echo "$*" >&2; exit 1; }
have() { [ -e "$1" ]; }

mkdir -p "$WORK" "$PREFIX" "$SYSROOT" /dl
export PATH=$PREFIX/bin:$PATH

fetch() {
	url=$1; sha=$2; f=/dl/$(basename "$url")
	mkdir -p /dl
	if [ ! -f "$f" ]; then
		echo "fetching $(basename "$url")" >&2
		curl -fL --retry 3 -o "$f.part" "$url"
		mv "$f.part" "$f"
	fi
	if [ -n "$sha" ]; then
		got=$( (sha256sum "$f" 2>/dev/null || shasum -a 256 "$f") | cut -d' ' -f1)
		[ "$got" = "$sha" ] || die "sha256 mismatch for $f: $got"
	fi
	echo "$f"
}

# ---------------------------------------------------------------- 1. binutils
if ! have "$PREFIX/bin/$TARGET-as"; then
	say "binutils $BINUTILS"
	t=$(fetch "https://ftpmirror.gnu.org/gnu/binutils/binutils-$BINUTILS.tar.xz" "")
	rm -rf "$WORK/binutils"; mkdir -p "$WORK/binutils"
	tar xf "$t" -C "$WORK/binutils" --strip-components=1
	# The Lexra opcodes the RTL9602C kernel needs (patches/binutils/).
	for pat in /src/patches/binutils/*.patch; do
		[ -f "$pat" ] || continue
		echo "  patch: $(basename "$pat")"
		patch -d "$WORK/binutils" -p1 --batch --forward < "$pat"
	done
	mkdir -p "$WORK/build-binutils"; cd "$WORK/build-binutils"
	"$WORK/binutils/configure" \
		--target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
		--disable-nls --disable-werror --disable-multilib
	make -j"$JOBS"
	make install
	cd /work
fi

# ------------------------------------------------------- 2. kernel headers
if ! have "$SYSROOT/usr/include/linux/unistd.h"; then
	say "kernel headers from the device tree"
	# Extracted here rather than bind-mounted: macOS collapses the 28 pairs of
	# paths in this tree that differ only in case, and the tarball does not.
	if [ ! -d "$KTREE" ]; then
		mkdir -p "$WORK/ktree"
		tar xf /tree.tar -C "$WORK/ktree"
	fi
	[ -d "$KTREE" ] || die "no kernel tree at $KTREE"
	mkdir -p "$SYSROOT/usr"
	# headers_install wants a writable tree.
	make -C "$KTREE" ARCH=mips INSTALL_HDR_PATH="$SYSROOT/usr" \
		headers_install >/dev/null
fi

# -------------------------------------------------------- 3. gcc, stage one
gcc_src() {
	have "$WORK/gcc/configure" && return
	t=$(fetch "https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC/gcc-$GCC.tar.xz" "")
	rm -rf "$WORK/gcc"; mkdir -p "$WORK/gcc"
	tar xf "$t" -C "$WORK/gcc" --strip-components=1
	# gmp, mpfr, mpc and isl come from the container (Dockerfile.oss), not
	# from download_prerequisites: one less network fetch that can fail.
}

GCC_COMMON="--target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT
	--with-arch=mips2 --with-abi=32 --with-float=soft --with-endian=big
	--disable-multilib --disable-nls --disable-libssp --disable-libgomp
	--disable-libmudflap --disable-libquadmath --disable-libsanitizer"

if ! have "$PREFIX/bin/$TARGET-gcc"; then
	say "gcc $GCC, stage one (C only, no libc yet)"
	gcc_src
	mkdir -p "$WORK/build-gcc1"; cd "$WORK/build-gcc1"
	# shellcheck disable=SC2086  # GCC_COMMON is a deliberate word list
	"$WORK/gcc/configure" $GCC_COMMON \
		--enable-languages=c --without-headers --with-newlib \
		--disable-shared --disable-threads --disable-libatomic \
		CFLAGS_FOR_TARGET="$TFLAGS"
	make -j"$JOBS" all-gcc all-target-libgcc
	make install-gcc install-target-libgcc
	cd /work
fi

# ------------------------------------------------------------- 4. uClibc-ng
if ! have "$SYSROOT/usr/lib/libc.a"; then
	say "uClibc-ng $UCLIBC"
	t=$(fetch "https://downloads.uclibc-ng.org/releases/$UCLIBC/uClibc-ng-$UCLIBC.tar.xz" "")
	rm -rf "$WORK/uclibc"; mkdir -p "$WORK/uclibc"
	tar xf "$t" -C "$WORK/uclibc" --strip-components=1
	# Patches against upstream, each with its reasoning in the header. They
	# exist because this kernel is old enough to take code paths nobody
	# exercises any more, not because of anything local.
	for pat in /src/patches/uclibc-ng/*.patch; do
		[ -f "$pat" ] || continue
		echo "  patch: $(basename "$pat")"
		patch -d "$WORK/uclibc" -p1 --batch --forward < "$pat"
	done
	cd "$WORK/uclibc"
	make ARCH=mips defconfig >/dev/null

	set_cfg() {
		sed -i "/^$1=/d; /^# $1 is not set/d" .config
		printf '%s\n' "$2" >> .config
	}
	set_cfg TARGET_ARCH            'TARGET_ARCH="mips"'
	set_cfg CONFIG_MIPS_O32_ABI    'CONFIG_MIPS_O32_ABI=y'
	set_cfg ARCH_BIG_ENDIAN        'ARCH_BIG_ENDIAN=y'
	set_cfg ARCH_WANTS_BIG_ENDIAN  'ARCH_WANTS_BIG_ENDIAN=y'
	set_cfg UCLIBC_HAS_FPU         '# UCLIBC_HAS_FPU is not set'
	# 64-bit time_t is ON by default in 1.0.59 for mips o32, and its stat
	# path goes through statx -- a syscall added in Linux 4.11. Left off
	# here, unrevisited since the 6.18 switch: the headers this sysroot now
	# builds against do carry __NR_statx, but flipping this changes the
	# width of time_t across the whole userland ABI, which is its own
	# review, not a change that rides along with removing the 3.18/2.6.30
	# kernel paths. This userland stays Y2038-limited until that review
	# happens.
	set_cfg UCLIBC_USE_TIME64      '# UCLIBC_USE_TIME64 is not set'
	# defconfig leaves DO_C99_MATH off while UCLIBC_HAS_FLOATS is on, and on
	# MIPS that combination cannot link: printf float formatting
	# (_fpmaxtostr) calls __signbit, whose out-of-line definition is built
	# only with C99 math. Architectures carrying a bits/mathinline.h --
	# i386, sparc, m68k, alpha, ia64, x86_64, csky -- resolve it inline and
	# never notice. MIPS has no mathinline.h.
	set_cfg DO_C99_MATH            'DO_C99_MATH=y'
	# pref is MIPS-IV; this core is not.
	set_cfg UCLIBC_USE_MIPS_PREFETCH '# UCLIBC_USE_MIPS_PREFETCH is not set'
	set_cfg KERNEL_HEADERS         "KERNEL_HEADERS=\"$SYSROOT/usr/include\""
	set_cfg RUNTIME_PREFIX         'RUNTIME_PREFIX="/"'
	set_cfg DEVEL_PREFIX           'DEVEL_PREFIX="/usr"'
	set_cfg CROSS_COMPILER_PREFIX  "CROSS_COMPILER_PREFIX=\"$TARGET-\""
	set_cfg UCLIBC_EXTRA_CFLAGS    "UCLIBC_EXTRA_CFLAGS=\"$TARCH\""
	set_cfg DOSTRIP                '# DOSTRIP is not set'
	# The holes in the 2010 libc that motivated all of this.
	set_cfg UCLIBC_HAS_RESOLVER_SUPPORT 'UCLIBC_HAS_RESOLVER_SUPPORT=y'
	set_cfg UCLIBC_HAS_LIBRESOLV_STUB   'UCLIBC_HAS_LIBRESOLV_STUB=y'
	set_cfg UCLIBC_HAS_CRYPT            'UCLIBC_HAS_CRYPT=y'
	set_cfg UCLIBC_HAS_CRYPT_IMPL       'UCLIBC_HAS_CRYPT_IMPL=y'
	set_cfg UCLIBC_HAS_SHA256_CRYPT_IMPL 'UCLIBC_HAS_SHA256_CRYPT_IMPL=y'
	set_cfg UCLIBC_HAS_SHA512_CRYPT_IMPL 'UCLIBC_HAS_SHA512_CRYPT_IMPL=y'
	set_cfg UCLIBC_HAS_PTY              'UCLIBC_HAS_PTY=y'
	set_cfg UNIX98PTY_ONLY              '# UNIX98PTY_ONLY is not set'
	set_cfg ASSUME_DEVPTS               '# ASSUME_DEVPTS is not set'
	# Four features the packages used to shim around in their own trees
	# (busybox mktemp, dropbear openpty and the stack-protector runtime,
	# iproute2 nftw). Provided by the libc now, so the shims are gone:
	#   SUSV3_LEGACY   mktemp(3), the one legacy call busybox still makes
	#   LIBUTIL        openpty/forkpty, so dropbear allocates ptys the normal way
	#   SSP            __stack_chk_fail and a per-process random guard, so
	#                  -fstack-protector-strong in dropbear links against the
	#                  libc rather than a constant-canary stand-in
	#   FTW, NFTW      nftw(), which iproute2 lib/cg_map.c calls
	set_cfg UCLIBC_SUSV3_LEGACY         'UCLIBC_SUSV3_LEGACY=y'
	set_cfg UCLIBC_SUSV4_LEGACY         'UCLIBC_SUSV4_LEGACY=y'
	set_cfg UCLIBC_HAS_FTW              'UCLIBC_HAS_FTW=y'
	set_cfg UCLIBC_HAS_NFTW             'UCLIBC_HAS_NFTW=y'
	set_cfg UCLIBC_HAS_LIBUTIL          'UCLIBC_HAS_LIBUTIL=y'
	set_cfg UCLIBC_HAS_SSP              'UCLIBC_HAS_SSP=y'
	set_cfg SSP_QUICK_CANARY            '# SSP_QUICK_CANARY is not set'
	set_cfg PROPOLICE_BLOCK_ABRT        'PROPOLICE_BLOCK_ABRT=y'
	set_cfg UCLIBC_BUILD_SSP            '# UCLIBC_BUILD_SSP is not set'

	# `< /dev/null`, never `yes '' |`: kconfig takes the default for every new
	# symbol on EOF just the same, and a `yes` producer gets SIGPIPE the moment
	# make exits -- which under `set -o pipefail` kills the whole script with
	# 141 and no message at all.
	make ARCH=mips oldconfig </dev/null >/dev/null
	make -j"$JOBS" \
		CROSS_COMPILE="$TARGET-" ARCH=mips \
		PREFIX="$SYSROOT" install >/dev/null
	cd /work
fi

# -------------------------------------------------------- 5. gcc, stage two
# Stamped rather than probed. The obvious guards do not work: we build C only,
# so libstdc++.a and $TARGET-g++ never exist and the old condition was
# permanently true -- gcc stage two rebuilt on every single run, ~15 minutes
# each. libgcc.a exists after stage ONE, so it cannot distinguish them either.
STAGE2_STAMP=$PREFIX/.stage2-complete
if ! have "$STAGE2_STAMP"; then
	say "gcc $GCC, stage two (against uClibc-ng)"
	gcc_src
	rm -rf "$WORK/build-gcc2"; mkdir -p "$WORK/build-gcc2"; cd "$WORK/build-gcc2"
	# shellcheck disable=SC2086  # GCC_COMMON is a deliberate word list
	# --disable-threads matches the libc: uClibc-ng defconfig selects
	# HAS_NO_THREADS for this target, and nothing this image ships needs
	# them -- busybox, dropbear, iproute2 and our own binaries are all
	# single-threaded or fork-per-connection. Enabling NPTL here would mean
	# exercising uClibc-ng thread support on MIPS o32 against this
	# kernel, which is precisely the unexercised-combination territory that
	# every other problem in this build came from. It is one config line to
	# revisit if something ever needs it.
	"$WORK/gcc/configure" $GCC_COMMON \
		--enable-languages=c --enable-shared --disable-threads \
		--disable-libatomic \
		CFLAGS_FOR_TARGET="$TFLAGS"
	make -j"$JOBS"
	make install
	touch "$STAGE2_STAMP"
	cd /work
fi

say "built"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
echo "prefix:  $PREFIX"
echo "sysroot: $SYSROOT"
echo
echo "Now audit it -- the flags reaching the target LIBRARIES is the thing that"
echo "went wrong last time:  ./toolchain/audit-toolchain.sh"
