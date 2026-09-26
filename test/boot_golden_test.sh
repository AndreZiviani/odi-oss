#!/usr/bin/env bash
# boot_golden_test.sh -- the production boot register stream:
# builds boot_golden_test.c, runs it for ISP1 and ISP2 and compares each
# run with the committed golden. Phases 1-6 (board replay to the first
# gponact) are the same on both lines and live in boot-golden-common.txt;
# phase 7, the driver commands, in boot-golden-isp1.txt and -isp2.txt.
# A change to the stream shows as a unified diff. UPDATE=1 rewrites the
# goldens instead, for a change that is meant to move them.
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIX=$ROOT/test/fixtures
DIR=$(mktemp -d -t boot_golden_test.XXXXXX)
trap 'rm -rf "$DIR"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" \
	-o "$DIR/bin" "$ROOT/test/boot_golden_test.c"

fail=0
for isp in isp1 isp2; do
	"$DIR/bin" "$isp" "$DIR/$isp.txt" > "$DIR/$isp.log"
	# The first line names the ISP; the common part starts after it and
	# ends where the driver-command phase begins.
	sed -n '2,/^# phase: ISP/p' "$DIR/$isp.txt" | sed '$d' > "$DIR/$isp.common"
	sed -n '/^# phase: ISP/,$p' "$DIR/$isp.txt" > "$DIR/$isp.cmds"
	if [ "${UPDATE:-0}" = 1 ]; then
		cp "$DIR/$isp.common" "$FIX/boot-golden-common.txt"
		cp "$DIR/$isp.cmds" "$FIX/boot-golden-$isp.txt"
		continue
	fi
	for part in common "$isp"; do
		src=$DIR/$isp.cmds
		[ "$part" = common ] && src=$DIR/$isp.common
		if ! diff -u "$FIX/boot-golden-$part.txt" "$src" > "$DIR/diff"; then
			echo "boot_golden_test: $isp differs from boot-golden-$part.txt:"
			head -n 60 "$DIR/diff"
			fail=1
		fi
	done
done
[ "${UPDATE:-0}" = 1 ] && { echo "boot_golden_test: goldens rewritten"; exit 0; }
[ "$fail" = 0 ] || exit 1
echo "boot_golden_test: ok ($(grep -vc '^#' "$FIX/boot-golden-common.txt") common, $(grep -vc '^#' "$FIX/boot-golden-isp1.txt") ISP1, $(grep -vc '^#' "$FIX/boot-golden-isp2.txt") ISP2 entries)"
