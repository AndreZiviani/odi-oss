#!/usr/bin/env sh
#
# The ISA gate: refuse a binary that contains instructions this CPU lacks.
#
#     isa-audit.sh <binary> [<binary>...]
#
# The RLX5281 is a Lexra MIPS core missing instructions every modern toolchain
# emits by default. A binary carrying one builds clean, links clean, and traps
# on the device the first time that code path runs -- which is why this is a
# build gate that exits non-zero, not a report.
#
# beqzl/bnezl are assembler aliases for beql/bnel against $zero. Omitting them
# once under-reported a binary by 62 instructions, so they are listed
# explicitly.
#
# The objdump is ours: binutils 2.47, mips-linux-uclibc-objdump, in the
# odi-oss-toolchain-318 volume, run in a container native to the host.
#
# Only executable sections are disassembled. A raw vmlinux carries sections
# that are not code at all -- .notes measured as flagged AX (SHF_EXECINSTR)
# by our own linker script on 2026-09-21, which made objdump decode note
# bytes as instructions and report a phantom beql. Filtering on the X flag
# alone is not enough, since that is the exact section that trips it; the
# gate also excludes anything of readelf section Type NOTE, flag or no flag.
# This applies to vmlinux and to .ko modules alike -- readelf -S is run per
# file and the section list it yields differs (a module has .text.unlikely,
# vmlinux does not), which is why the section list is derived, not hardcoded.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OSS_VOLUME=${OSS_VOLUME:-odi-oss-toolchain-318}

[ $# -gt 0 ] || { echo "usage: isa-audit.sh <binary> [<binary>...]" >&2; exit 1; }

docker volume inspect "$OSS_VOLUME" >/dev/null 2>&1 || { echo "no toolchain volume $OSS_VOLUME -- make toolchain" >&2; exit 1; }

# Stage the binaries into one directory so a single container sees them all:
# starting the container costs more than the audit does.
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
for b in "$@"; do
	[ -f "$b" ] || { echo "no such binary: $b" >&2; exit 1; }
	cp "$b" "$stage/$(basename "$b")"
done

# Build the docker arguments as positional parameters rather than one string:
# an unquoted variable of arguments splits on the shell IFS, which is exactly
# the kind of quiet breakage a path with a space would cause.
IMAGE=$("$ROOT/packages/oss-env.sh")
set -- -v "$OSS_VOLUME:/opt/oss:ro" \
	-e OBJDUMP=/opt/oss/bin/mips-linux-uclibc-objdump \
	-e READELF=/opt/oss/bin/mips-linux-uclibc-readelf

echo "==> ISA gate (our objdump)" >&2

docker run --rm -v "$stage:/audit:ro" "$@" "$IMAGE" bash -c '
set -e
rc=0
for f in /audit/*; do
	# A file objdump cannot parse used to print an error and then report
	# zero, which reads exactly like a pass. Anything that is not an ELF
	# here is a mistake by the caller, so say so and fail.
	if ! "$OBJDUMP" -f "$f" >/dev/null 2>&1; then
		echo "$(basename "$f"): NOT AN ELF -- cannot be audited" >&2
		rc=1
		continue
	fi
	# Only the executable sections get disassembled -- readelf, not
	# objdump -h, because objdump reports its own CODE/DATA guess rather
	# than the section header flags, and it is the section header flag
	# that misidentifies .notes. A section of Type NOTE is excluded even
	# if it carries the X flag: it is never code, whatever the flag says.
	jargs=$("$READELF" -S "$f" 2>/dev/null | sed "s/\[ */[/" | awk "
	  \$1 ~ /^\[[0-9]+\]\$/ {
	    name = \$2; type = \$3;
	    flg = (NF == 11) ? \$8 : \"\";
	    if (type != \"NULL\" && type != \"NOTE\" && index(flg, \"X\") > 0)
	      printf \"-j %s \", name
	  }")
	if [ -z "$jargs" ]; then
		echo "$(basename "$f"): no executable sections found -- cannot be audited" >&2
		rc=1
		continue
	fi
	# -m mips:isa32 so the SPECIAL2 encodings decode as mul and clz rather
	# than as an unknown word. Disassembling at the target ISA would hide
	# exactly the instructions this gate exists to find.
	# teq was measured illegal; the rest of the conditional-trap family is
	# the same encoding class and the kernel headers do reach for tne
	# (asm/bug.h), so all of it is refused until measured otherwise.
	# shellcheck disable=SC2086 # jargs is a deliberate list of -j NAME pairs
	n=$("$OBJDUMP" -d -m mips:isa32 $jargs "$f" \
	  | awk -F"\t" "NF>=3 {gsub(/ /,\"\",\$3); print \$3}" \
	  | grep -cE "^(mul|clz|clo|teq|tne|tge|tgeu|tlt|tltu|teqi|tnei|tgei|tgeiu|tlti|tltiu|beql|bnel|beqzl|bnezl)$" || true)
	echo "$(basename "$f"): instructions the RLX5281 traps on: $n"
	[ "$n" = 0 ] || rc=1
done
[ "$rc" = 0 ] || echo "REFUSING: see above -- a trapping instruction, or a file that could not be audited" >&2
exit $rc
'
