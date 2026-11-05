#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The stack protector of src/harden.mk, end to end, under qemu-user in the
# freestanding toolchain container (make -C src/diag ssp-test):
#
#   1. the guard is seeded from AT_RANDOM: two runs give two different
#      values, each with its first byte zero, neither the fallback;
#   2. an overrun of a local array ends the process with SIGABRT and the
#      message of __stack_chk_fail, within a bound, before the function
#      returns; the same call without an overrun returns normally;
#   3. tools/check-hardening.sh passes the protected build and refuses the
#      same program built with -fno-stack-protector.
#
# Run from src/diag, with build/ssp_selftest and build/ssp_selftest_off built.
set -u

BIN=build/ssp_selftest
OFF=build/ssp_selftest_off
CHECK=../../tools/check-hardening.sh
fail=0
bad() { echo "ssp-test: $*" >&2; fail=1; }

# 1. Seeding.
g1=$(timeout 10 qemu-mips-static "$BIN" guard)
g2=$(timeout 10 qemu-mips-static "$BIN" guard)
for g in "$g1" "$g2"; do
	case $g in
	0x00000aff) bad "guard is the fallback value: AT_RANDOM was not used" ;;
	0x00??????) ;;
	*) bad "guard $g: expected 0x00 followed by six hex digits" ;;
	esac
done
[ "$g1" != "$g2" ] || bad "two runs gave the same guard ($g1): not seeded per process"

# 2. The overrun. 128 + 6: the shell reports a death by SIGABRT as 134,
# and qemu-user passes the guest signal through as its own.
out=$(timeout 10 qemu-mips-static "$BIN" smash 64 2>build/ssp-smash.err)
rc=$?
err=$(cat build/ssp-smash.err)
[ "$rc" -eq 134 ] || bad "overrun: exit status $rc, expected 134 (SIGABRT)"
case $err in
*"stack smashing detected"*) ;;
*) bad "overrun: no stack smashing message on stderr (got: $err)" ;;
esac
case $out in
*returned*) bad "overrun: the smashed function returned" ;;
esac

out=$(timeout 10 qemu-mips-static "$BIN" smash 8)
rc=$?
[ "$rc" -eq 0 ] && [ "$out" = returned ] \
	|| bad "no overrun: exit status $rc, output \"$out\"; expected 0 and returned"

# 3. The ELF check, both ways.
sh "$CHECK" mips-linux-gnu- "$BIN" >/dev/null \
	|| bad "check-hardening.sh refused the protected build"
if sh "$CHECK" mips-linux-gnu- "$OFF" >/dev/null 2>&1; then
	bad "check-hardening.sh passed a build without -fstack-protector-strong"
fi

[ "$fail" -eq 0 ] || exit 1
echo "stack protector: guard seeded per run ($g1, $g2), overrun aborts with SIGABRT, check-hardening refuses an unprotected build"
