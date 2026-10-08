#!/usr/bin/env bash
# rtl8686_time_test.sh -- compiles and runs rtl8686_time_test.c with the host
# cc. time.c is #included directly (unity build) against stub kernel headers
# generated here into a temp dir: each one just pulls in the test own model
# of the kernel API and of the TIMER0/TIMER1 register blocks. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d -t rtl8686_time_test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/linux" "$TMP/asm"
for h in linux/clockchips.h linux/clocksource.h linux/init.h linux/interrupt.h \
	linux/io.h linux/math64.h linux/sched_clock.h linux/linkage.h asm/time.h; do
	printf '#include "kstub.h"\n' > "$TMP/$h"
done
cc -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-I "$TMP" -I "$ROOT/test" -I "$ROOT/kernel/extra/arch/mips/include" \
	-I "$ROOT/kernel/extra/arch/mips/rtl8686" \
	-o "$TMP/t" "$ROOT/test/rtl8686_time_test.c"
"$TMP/t"
