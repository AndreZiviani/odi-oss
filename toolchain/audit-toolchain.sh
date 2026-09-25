#!/usr/bin/env bash
#
# Audit the built toolchain LIBRARIES, not a test program.
#
# This is the check the previous attempt did not have. A crosstool-NG build
# with the Lexra flags applied only to the application compile still shipped
# 64 teq in libgcc.a and 95 in libc.a -- the application was clean and the
# binary SIGILLed on hardware anyway, because the runtime library sets the ISA
# floor, not the compiler flag.
#
# So: disassemble every object in every static library the toolchain will link
# into our binaries, and refuse on any instruction this core traps on.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)

TARGET=mips-linux-uclibc
VOLUME=${VOLUME:-odi-oss-toolchain-318}
IMAGE=odi-oss-toolchain-builder

# The toolchain lives in a Docker volume, so the audit runs there too.
if [ "${IN_CONTAINER:-0}" != 1 ]; then
	docker volume inspect "$VOLUME" >/dev/null 2>&1 ||
		{ echo "no toolchain volume -- run build-oss-toolchain.sh first" >&2; exit 1; }
	exec docker run --rm -v "$VOLUME:/opt/oss" -v "$ROOT/toolchain:/src:ro" \
		-e IN_CONTAINER=1 "$IMAGE" bash /src/audit-toolchain.sh
fi

PREFIX=${PREFIX:-/opt/oss}
SYSROOT=$PREFIX/$TARGET/sysroot
OBJDUMP=$PREFIX/bin/$TARGET-objdump

[ -x "$OBJDUMP" ] || { echo "no toolchain at $PREFIX -- build it first" >&2; exit 1; }

# Measured by executing each one on the
# device. beqzl/bnezl are assembler aliases for beql/bnel against $zero:
# omitting them once under-reported a binary by 62 instructions.
ILLEGAL='^(mul|clz|teq|beql|bnel|beqzl|bnezl)$'

# hardcfr.o is GCC's hardened control-flow redundancy runtime, pulled in only by
# -fharden-control-flow-redundancy, which nothing here compiles with. Its one
# `teq zero,zero` is __builtin_trap(): the MIPS trap pattern is `teq $0,$0`
# whenever ISA_HAS_COND_TRAP, which mips2 satisfies, and `break` at mips1. The
# instruction is real and illegal on this core, but it is in an archive member
# the linker never pulls in -- and the binding gate is the image-level audit
# over LINKED binaries in packages/isa-audit.sh, not this one.
#
# If it ever needs to go away rather than be excused, building libgcc at
# -march=mips1 turns that pattern into `break`; mips1 code runs fine on this
# core and only uClibc-ng actually needs the mips2 ll/sc.
SKIP_MEMBERS='hardcfr[.]o'

rc=0
total=0
found=0
for lib in \
	"$SYSROOT"/usr/lib/*.a "$SYSROOT"/lib/*.a \
	"$PREFIX/lib/gcc/$TARGET"/*/libgcc.a
do
	[ -f "$lib" ] || continue
	total=$((total + 1))
	# Disassemble per member so an excused one can be dropped by name.
	n=$("$OBJDUMP" -d "$lib" 2>/dev/null \
	  | awk -v skip="$SKIP_MEMBERS" -F'\t' '
		/^[^ \t].*\.o:/ { member = $0; next }
		NF>=3 {
			if (skip != "" && member ~ skip) next
			gsub(/ /,"",$3); print $3
		}' \
	  | grep -cE "$ILLEGAL" || true)
	printf '  %-40s %s\n' "$(basename "$lib")" "$n"
	if [ "$n" != 0 ]; then
		found=$((found + n))
		rc=1
	fi
done

[ "$total" = 0 ] && { echo "no libraries found to audit -- is the build finished?" >&2; exit 1; }

echo
if [ "$rc" = 0 ]; then
	echo "$total libraries, zero instructions the RLX5281 traps on."
else
	echo "REFUSING: $found trapping instructions across the target libraries." >&2
	echo "The flags did not reach them. That is what sank the last attempt." >&2
fi
exit $rc
