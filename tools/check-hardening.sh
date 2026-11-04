#!/bin/sh
#
# check-hardening.sh <cross-prefix> <elf>...
#
# Fails when a freestanding binary from src/ has lost a mitigation of
# src/harden.mk (docs/CROSS-COMPILING.md, "Hardening"). Run by every
# Makefile in src/ on the binary it ships, after the link and before the
# strip, because the stack protector checks need the symbol table. It runs
# in the freestanding toolchain container, with that toolchain readelf and
# objdump; POSIX sh, since that container may not have bash.
#
# Per binary:
#   - PT_GNU_STACK is present and not executable (-z noexecstack, and the
#     .note.GNU-stack of start.S);
#   - no LOAD segment is both writable and executable;
#   - __stack_chk_guard and __stack_chk_fail are defined, _start calls the
#     routine that seeds the guard, and at least one function calls
#     __stack_chk_fail: a build without -fstack-protector-strong has none.
set -eu

[ $# -ge 2 ] || { echo "usage: $0 <cross-prefix> <elf>..." >&2; exit 2; }
cross=$1
shift

fail=0
for elf in "$@"; do
	name=$(basename "$elf")
	bad() { echo "check-hardening: $name: $*" >&2; fail=1; }

	phdrs=$("${cross}readelf" -lW "$elf")
	stack=$(printf '%s\n' "$phdrs" | awk '$1 == "GNU_STACK" { print $(NF-1) }')
	case $stack in
	"") bad "no PT_GNU_STACK: the kernel would map every readable page executable" ;;
	*E*) bad "PT_GNU_STACK is $stack: executable stack" ;;
	esac
	# The flags of a LOAD line are the columns between MemSiz and Align:
	# "R E", "RW ", "RWE". Rebuilt from the fields, since awk splits "R E".
	wx=$(printf '%s\n' "$phdrs" | awk '$1 == "LOAD" {
		f = ""; for (i = 7; i < NF; i++) f = f $i
		if (f ~ /W/ && f ~ /E/) print $2 }')
	[ -z "$wx" ] || bad "a LOAD segment is writable and executable (offset $wx)"

	syms=$("${cross}readelf" -sW "$elf")
	for sym in __stack_chk_guard __stack_chk_fail; do
		printf '%s\n' "$syms" | awk -v s="$sym" '$8 == s && $7 != "UND" { f = 1 } END { exit !f }' \
			|| bad "$sym is not defined: the runtime of diag/src/start.S is not linked in"
	done

	dis=$("${cross}objdump" -d --no-show-raw-insn "$elf")
	printf '%s\n' "$dis" | grep -q 'jal[[:space:]].*<ssp_seed>' \
		|| bad "_start does not seed the guard (no call to ssp_seed)"
	calls=$(printf '%s\n' "$dis" | grep -c 'jal[[:space:]].*<__stack_chk_fail>' || true)
	[ "$calls" -gt 0 ] || bad "no function calls __stack_chk_fail: built without -fstack-protector-strong"

	[ "$fail" -ne 0 ] || echo "check-hardening: $name: $calls protected returns, stack ${stack}, no W+X segment"
done
exit "$fail"
