#!/usr/bin/env bash
# Build Linux 6.18 for the RTL9602C -- the only kernel this repo builds.
#
# Pristine tree from kernel/618/mainline (kernel/618/fetch.sh, a plain
# GPG-verified cdn.kernel.org tarball), our edits to mainline files from
# kernel/618/patches, every file of our own from the kernel/extra overlay
# (board, CPU cache code, irqchip, timer, SPI NOR and the odi drivers, each
# at its path in the tree), config seeded from kernel/618/config and
# completed by olddefconfig, uImage in the U-Boot format the stick boots.
# Compiled with
# our toolchain (gcc 16 and binutils 2.47 with the three Lexra opcodes the
# tree uses), in the uclibc toolchain image pinned in toolchain/images.env.
# The toolchain is inside the image at /opt/oss. The vendor prebuilt
# toolchain needed no -march at all (its gcc 4.4.5 defaulted to the Lexra
# 5281 and predefined _MIPS_ISA_MIPS1); ours gets the userland ISA flags, packages/isa-audit.sh
# has the why:
#     -march=mips2 -mno-branch-likely -mdivide-breaks
#
# Nothing here flashes anything.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=$("$ROOT/packages/oss-env.sh")
TCBIN=/opt/oss/bin; XC=mips-linux-uclibc-
# -mno-check-zero-division is already on the kernel line, so no teq is
# emitted for divisions; -mdivide-breaks is belt and braces. gcc 14 made
# four C diagnostics errors by default and older code in the tree trips
# them; back to warnings, counted in the summary.
# -march=mips2 also makes gcc predefine _MIPS_ISA as _MIPS_ISA_MIPS2, and
# arch/mips/include/asm/bug.h then builds BUG_ON() on tne, a trap
# instruction this core does not implement. The -U/-D pair tells the
# kernel C code it is MIPS I, so bug.h takes the asm-generic BUG_ON()
# (if (cond) BUG()) instead; the only other reader of _MIPS_ISA in the
# tree, kgdb.h, treats MIPS I and MIPS II alike. Code generation is still
# -march=mips2. The isa-audit gate would catch a tne that slipped through.
KCFLAGS="-march=mips2 -mno-branch-likely -mdivide-breaks -fno-PIE
         -U_MIPS_ISA -D_MIPS_ISA=_MIPS_ISA_MIPS1
         -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
         -Wno-error=return-mismatch -Wno-error=declaration-missing-parameter-type
         -Wno-error=date-time
         -Wno-array-bounds -Wno-address-of-packed-member -Wno-stringop-overflow
         -Wno-stringop-truncation -Wno-format-truncation -Wno-format-overflow
         -Wno-dangling-pointer -Wno-unused-but-set-variable -Wno-misleading-indentation
         -Wno-enum-int-mismatch -Wno-attribute-alias -Wno-restrict"
# shellcheck disable=SC2086 # collapse the continuation lines into one
KCFLAGS=$(echo $KCFLAGS)

# KVER=6.18 is the only value accepted; kept as a variable (rather than
# dropped outright) so an explicit KVER=6.18 in an existing command line, or
# in kernel/618/fetch.sh own comment, keeps working unchanged.
KVER=${KVER:-6.18}
[ "$KVER" = 6.18 ] || { echo "kernel/build.sh: unknown KVER=$KVER (want 6.18)" >&2; exit 1; }
SEED="$ROOT/kernel/618/config"
[ -s "$SEED" ] || { echo "no seed config at $SEED" >&2; exit 1; }
WORK=${WORK:-$ROOT/build/kernel-618}
VOL=${VOL:-odi-kbuild-618}
mkdir -p "$WORK"
# shellcheck disable=SC2329 # invoked indirectly via trap, next line
on_exit_618() { rc=$?; [ "$rc" -ne 0 ] && echo "kernel/build.sh: FAILED, exit $rc -- tail $WORK/build.log"; exit "$rc"; }
trap on_exit_618 EXIT
# The pristine tree, fetched first on a clean clone (kernel/tree.sh has the
# why: the toolchain build reads the same tarball, before this script can run).
"$ROOT/kernel/tree.sh" "$WORK/tree.tar"
cp "$SEED" "$WORK/seed.config"
hash_of() { (sha256sum 2>/dev/null || shasum -a 256) | cut -d' ' -f1; }
# The pinned kernel release (the tree.tar stamp kernel/tree.sh writes) is
# part of the script stamp, so a pin bump re-extracts /build/src instead of
# building the old sources the volume still holds.
SCRIPTHASH=$(cat "$0" "$SEED" "$WORK/tree.tar.version" | hash_of)
# The overlay file list (names, not contents) is part of the patch stamp: a
# file added to or removed from kernel/extra re-extracts the tree, so a
# deleted overlay file does not linger in /build/src. A content edit alone
# stays incremental (the cmp -s copy below).
# CRUMBS_CORE=1 also applies kernel/618/debug/*.patch, the early-boot
# crumbs in core files that no board hook reaches (head.S tail,
# start_kernel, cpu_probe); they store only with CONFIG_ODI_EARLY_CRUMBS=y.
# Off by default; the flag and those patches are part of the stamp.
CRUMBS_CORE=${CRUMBS_CORE:-0}
PATCHHASH=$({ cat "$ROOT"/kernel/618/patches/*.patch 2>/dev/null
	[ "$CRUMBS_CORE" = 1 ] && { echo CRUMBS_CORE; cat "$ROOT"/kernel/618/debug/*.patch; }
	(cd "$ROOT/kernel/extra" && find . -type f ! -name README.md | LC_ALL=C sort); } | hash_of)
# The build id the ramlog stamps into every boot (odi_ramlog.h): the image
# VERSION when one is set for this build, else the default image/build.sh
# would pick today. Up to 39 characters from a safe set, because it lands
# in a C string literal on the compiler command line.
GITREV=$(git -C "$ROOT" describe --always --dirty --abbrev=7 2>/dev/null || echo nogit)
ODI_BUILD_ID=${ODI_BUILD_ID:-${VERSION:-odi-oss-$(date +%y%m%d)-$GITREV}}
ODI_BUILD_ID=$(printf '%s' "$ODI_BUILD_ID" | tr -cd 'A-Za-z0-9._+-' | cut -c1-39)
echo "--- build id: $ODI_BUILD_ID"
# Reproducibility: scripts/mkcompile_h embeds the build timestamp, user and
# host into every vmlinux unless told otherwise, so two builds of the same
# commit a minute apart -- or on two different runners -- never matched
# bit-for-bit (found by the ci.yml reproducible job the first time it ran for
# real). SOURCE_DATE_EPOCH is the committer date of HEAD: identical for any
# two checkouts of the same commit, unlike "now". KBUILD_BUILD_USER/HOST are
# fixed strings for the same reason -- the actual build host and account
# are exactly the kind of stamp this build is not supposed to carry.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)}
KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-$(date -u -d "@$SOURCE_DATE_EPOCH" 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH")}
docker run --rm -v "$VOL":/build \
	-v "$WORK:/out" -v "$ROOT/kernel/618/patches:/patches:ro" \
	-v "$ROOT/kernel/618/debug:/debug:ro" -e CRUMBS_CORE="$CRUMBS_CORE" \
	-v "$ROOT/kernel/extra:/extra:ro" \
	-w /build \
	-e SCRIPTHASH="$SCRIPTHASH" -e PATCHHASH="$PATCHHASH" -e FRESH="${FRESH:-0}" \
	-e ODI_BUILD_ID="$ODI_BUILD_ID" \
	-e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
	-e KBUILD_BUILD_TIMESTAMP="$KBUILD_BUILD_TIMESTAMP" \
	-e KBUILD_BUILD_USER=odi-oss -e KBUILD_BUILD_HOST=reproducible \
	-e TCBIN="$TCBIN" -e XC="$XC" -e KCFLAGS="$KCFLAGS" -e COMPILE_ONLY="${COMPILE_ONLY:-0}" "$IMAGE" bash -c '
set -e
STAMP=/build/.stamp
OLD_SCRIPT=""; OLD_PATCH=""
[ -f "$STAMP" ] && { OLD_SCRIPT=$(sed -n 1p "$STAMP"); OLD_PATCH=$(sed -n 2p "$STAMP"); }
if [ "$FRESH" != 1 ] && [ -d /build/src/arch ] && [ "$OLD_SCRIPT" = "$SCRIPTHASH" ] && [ "$OLD_PATCH" = "$PATCHHASH" ]; then
	echo "--- unchanged, incremental build ---"
else
	rm -rf /build/src "$STAMP"; mkdir -p /build/src
	tar xf /out/tree.tar -C /build/src --exclude="*.o" --exclude=".*.cmd" --exclude="*.a"
	for pat in /patches/*.patch; do
		[ -f "$pat" ] || continue
		echo "  patch: $(basename "$pat")"
		patch -d /build/src -p1 --batch --forward < "$pat"
	done
	if [ "$CRUMBS_CORE" = 1 ]; then
		for pat in /debug/*.patch; do
			echo "  patch (CRUMBS_CORE): $(basename "$pat")"
			patch -d /build/src -p1 --batch --forward < "$pat"
		done
	fi
fi
# The overlay: every file under kernel/extra but the README.md files, copied
# to the same relative path under /build/src. Copied every run (cheap cmp -s
# check), independent of the patch-incremental branch above, so an edit to
# an overlay file alone (no patch, no seed change) still lands without
# FRESH=1. The overlay only adds files; the few mainline lines that make
# them reachable (Kconfig source, Makefile obj- lines) are in /patches.
(cd /extra && find . -type f ! -name README.md) | while read -r f; do
	f=${f#./}
	if ! cmp -s "/extra/$f" "/build/src/$f"; then
		mkdir -p "/build/src/$(dirname "$f")"
		cp "/extra/$f" "/build/src/$f"; echo "  extra: $f"
	fi
done
cd /build/src
export PATH=$TCBIN:$PATH
export KCFLAGS
echo "--- compiler: $(${XC}gcc -dumpfullversion) ${XC}gcc, $(${XC}ld --version | head -1); KCFLAGS=$KCFLAGS"
cp /out/seed.config .config
make ARCH=mips CROSS_COMPILE=$XC olddefconfig > /out/olddefconfig.log 2>&1 || { tail -30 /out/olddefconfig.log; exit 1; }
echo "--- config: $(grep -c "^CONFIG_" .config) on, $(grep -c "is not set" .config) off"
echo "--- key symbols after olddefconfig ---"
grep -E "^CONFIG_(MACH_RTL8686|CPU_R3000|CPU_HAS_SYNC|CPU_HAS_WB|CPU_R3K_TLB|IRQ_MIPS_CPU|DMA_NONCOHERENT|RTL8686_UART0|RTL8686_NOR|MTD|JFFS2_FS|SQUASHFS|ODI_)[= ]" .config || true
echo "--- CONFIG_ODI_* on ---"
grep -E "^CONFIG_ODI_[A-Z_]*=y" .config || echo "(none)"
cp .config /out/config
make ARCH=mips CROSS_COMPILE=$XC -j4 V=2 vmlinux > /out/build.log 2>&1 || {
	echo "BUILD FAILED"; grep -n "Error [0-9]\|error:" /out/build.log | head -30; exit 1; }
echo "BUILD OK: $(grep -c "warning:" /out/build.log) warnings"
[ "$COMPILE_ONLY" = 1 ] && { cp vmlinux /out/vmlinux; cp System.map /out/System.map; cp .config /out/config; exit 0; }
${XC}objcopy -O binary -R .note -R .comment -S vmlinux linux.bin
lzma -z -c -9 --format=lzma linux.bin > linux.bin.lzma 2>/dev/null || lzma -z -c -9 linux.bin > linux.bin.lzma
# The entry point is kernel_entry, not the load address: on 6.18 it sits
# far into the image (0x8031ce08 in the first build), and an entry of
# 0x80000000 jumps U-Boot into other code (isp1 trial 618d hung before
# prom_init). Taken from System.map every build.
ENTRY=$(sed -n "s/^\([0-9a-f]*\) T kernel_entry\$/\1/p" System.map)
[ -n "$ENTRY" ] || { echo "BUILD FAILED: no kernel_entry in System.map"; exit 1; }
echo "entry point: $ENTRY"
mkimage -A mips -O linux -T kernel -C lzma -a 80000000 -e "$ENTRY" -n "Linux Kernel Image 6.18" -d linux.bin.lzma uImage >/dev/null
echo "--- sizes ---"; stat -c "vmlinux %s" vmlinux; stat -c "linux.bin %s" linux.bin; stat -c "uImage %s" uImage
echo "k0/k1 partition 1359872"
{ echo "$SCRIPTHASH"; echo "$PATCHHASH"; } > "$STAMP"
cp vmlinux /out/vmlinux
cp uImage /out/uImage
cp System.map /out/System.map
cp .config /out/config
'
