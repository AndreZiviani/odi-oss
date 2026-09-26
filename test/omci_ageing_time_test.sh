#!/usr/bin/env bash
# omci_ageing_time_test.sh -- "open stack by default": the cmd 62
# register (L2_LOOKUP_SETUP.AGE_TICKS) is tenths of a second, not seconds. apply.c
# used to send the bare OMCI-attribute default, 300 (seconds), straight
# through as the cmd 62 argument; boot5 shows the vendor writing 3000.
#
# apply.c is not part of test-host: it #includes sys.h/io.h, which only
# exist inside the qemu diag-toolchain container (src/omci/qemu-test.sh),
# not in this tree. So this test does not build apply.c -- it checks the
# same thing test-omci's brackets would, two other ways, against real data:
#
#   1. decode the boot5 fixture's own cmd 62 write (test/fixtures/
#      dal-cmd62.txt) and mask it down to the AGE_TICKS field
#      (odi_switch_hw.h ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_{GET,SET}, bits [20:0]) --
#      confirming 3000 is what real hardware actually wants, independent of
#      apply.c's own source;
#   2. grep apply.c's two ageing-time call sites for the *10 scaling this
#      task added, as a regression guard -- so a future edit that quietly
#      drops back to the bare "300" (as this task found it) fails here
#      instead of only on a stick.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

fixture="$ROOT/test/fixtures/dal-cmd62.txt"
apply_c="$ROOT/src/omci/respond/apply_uni.c"

# 1: boot5's own register write, masked to AGE_TICKS (bits [20:0], 0x1fffff --
# odi_switch_hw.h ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_GET).
write_line=$(grep -E '^[0-9a-f]+ W 0x00017000 ' "$fixture" | tr -d '\r')
raw_val=$(awk '{print $NF}' <<<"$write_line")
age_spd=$(( raw_val & 0x1fffff ))
if [ "$age_spd" -ne 3000 ]; then
	echo "FAIL: boot5 dal-cmd62.txt AGE_TICKS field is $age_spd, expected 3000 ($write_line)" >&2
	exit 1
fi

# 2: apply.c's two ageing-time call sites still scale by 10 (seconds ->
# tenths of a second) before calling omci_setAgeingTime().
if ! grep -q 'omci_setAgeingTime(300 \* 10);' "$apply_c"; then
	echo "FAIL: apply.c class-45 delete arm no longer scales ageing time (expected omci_setAgeingTime(300 * 10))" >&2
	exit 1
fi
if ! grep -q 'age = 300 \* 10;' "$apply_c"; then
	echo "FAIL: apply.c class-45 Set/Create zero-attribute default no longer scales ageing time (expected age = 300 * 10)" >&2
	exit 1
fi

echo "ok: cmd 62 default ageing time is 3000 (tenths of a second), matching boot5 and apply.c's *10 scaling"
