#!/usr/bin/env sh
#
# The ISA gate, the other way round: refuse any instruction NOT confirmed to
# execute on this CPU.
#
#     isa-allowlist.sh <binary> [<binary>...]
#
# isa-audit.sh is a DENY list -- it names the instructions known to trap and
# counts them. That shape is not
# enough: an audit of exactly that form passed a dropbear carrying 147 `teq`,
# because nobody had put `teq` on the list yet. A check that only looks for
# problems already known cannot find a new one.
#
# This one keeps the list of mnemonics CONFIRMED PRESENT -- MIPS-I, plus the
# handful of later instructions the KB note measured by executing them on an
# ODI DFP-34X-2C2 -- and reports everything else. On 2026-09-15 the whole
# image came to 64 distinct mnemonics, every one on this list; a new one
# appearing is not automatically wrong, but it is automatically unverified,
# and the answer is to execute it on the device and extend the list, never to
# extend the list from a datasheet.
#
# Same container arrangement as isa-audit.sh, and the same reason for
# -m mips:isa32: disassembling at the target ISA would hide exactly the
# encodings this gate exists to find.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OSS_VOLUME=${OSS_VOLUME:-odi-oss-toolchain-318}
[ $# -gt 0 ] || { echo "usage: isa-allowlist.sh <binary> [<binary>...]" >&2; exit 1; }

# MIPS-I, as binutils spells it, plus the measured extras. `move`, `li`, `b`,
# `bal`, `negu`, `nop` and `beqz`/`bnez` are assembler idioms for MIPS-I
# encodings and appear as such in the disassembly.
ALLOW='^(add|addi|addiu|addu|and|andi|b|bal|beq|beqz|bgez|bgezal|bgtz|blez|bltz|bltzal|bne|bnez|break|div|divu|j|jal|jalr|jr|lb|lbu|lh|lhu|li|lui|lw|lwl|lwr|mfhi|mflo|move|mthi|mtlo|mult|multu|negu|nop|nor|or|ori|sb|sh|sll|sllv|slt|slti|sltiu|sltu|sra|srav|srl|srlv|sub|subu|sw|swl|swr|syscall|xor|xori|movz|movn|ll|sc|sync|madd|bltzl|mfc0|mtc0|eret|tlbwi|tlbwr|tlbp|tlbr|cache|wait|rfe)$'

tc=${ISA_TOOLCHAIN:-}
if [ -z "$tc" ]; then
	docker volume inspect "$OSS_VOLUME" >/dev/null 2>&1 || { echo "no toolchain volume $OSS_VOLUME -- make toolchain" >&2; exit 1; }
	tc=oss
fi
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
for b in "$@"; do
	[ -f "$b" ] || { echo "no such binary: $b" >&2; exit 1; }
	cp "$b" "$stage/$(basename "$b")"
done
case $tc in
oss)
	IMAGE=$("$ROOT/packages/oss-env.sh")
	set -- -v "$OSS_VOLUME:/opt/oss:ro" -e OBJDUMP=/opt/oss/bin/mips-linux-uclibc-objdump ;;
esac

echo "==> ISA allowlist ($tc toolchain)" >&2
docker run --rm -v "$stage:/audit:ro" -e ALLOW="$ALLOW" "$@" "$IMAGE" bash -c '
set -e
rc=0
for f in /audit/*; do
	if ! "$OBJDUMP" -f "$f" >/dev/null 2>&1; then
		echo "$(basename "$f"): NOT AN ELF -- cannot be audited" >&2
		rc=1; continue
	fi
	# Mnemonic column only, then everything the allowlist does not match.
	# An empty disassembly is a failure, not a pass: it means objdump found
	# no code, and a gate that passes on nothing is the false pass again.
	all=$("$OBJDUMP" -d -m mips:isa32 "$f" | awk -F"\t" "NF>=3 {gsub(/ /,\"\",\$3); print \$3}" | sort -u)
	[ -n "$all" ] || { echo "$(basename "$f"): no instructions decoded" >&2; rc=1; continue; }
	bad=$(printf "%s\n" "$all" | grep -vE "$ALLOW" || true)
	n=$(printf "%s\n" "$all" | grep -c .)
	if [ -z "$bad" ]; then
		echo "$(basename "$f"): $n distinct mnemonics, all confirmed present"
	else
		echo "$(basename "$f"): UNVERIFIED instructions: $(printf "%s" "$bad" | tr "\n" " ")"
		rc=1
	fi
done
[ "$rc" = 0 ] || echo "REFUSING: an instruction not confirmed to execute on the RLX5281 -- execute it on the device and extend the list, or remove it" >&2
exit $rc
'
