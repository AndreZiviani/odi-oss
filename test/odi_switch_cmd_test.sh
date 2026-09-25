#!/usr/bin/env bash
# odi_switch_cmd_test.sh -- compiles odi_switch_cmd_test.c, replays the 91
# command brackets of isp1-260922-boot5.txt through odi_switch_cmd(), and
# runs tools/regtrace/compare.py against the reference trace directly.
# Every bracket must PASS. cmd 23 instances
# #10-13 used to be an exemption here (a write-order swap between
# PONQ_COUNT_MASK+208 and +212/+213 that odi_sw_ponmac_queue_add_ext() wrote
# in one fixed order) -- boot5's five full-shape instances gave an exact
# rule for the swap (it tracks whether this is the first queue, n==0, or
# a later one), fixed in odi_switch_dal.c (the cmd 23 #208/#212-213 write
# order); the exemption is gone, all 91 brackets PASS.
# odi_sw_cf_add_table cmd 51 table payload landed during
# this task (odi_switch_dal.c) and all 12 cmd 51 instances PASS too.
#
# The reference trace is vendored as test/fixtures/isp1-260922-boot5.txt
# (a verbatim copy of the original captured trace), same as the existing
# dal-cmd*.txt fixtures are excerpts of boot3 -- so test-host stays
# self-contained and portable, per this repo own CLAUDE.md ("these scripts
# must always exit 0 ... a failure here must never break a session start
# or an install", and CI runs test-host on every machine, not just this
# one). ODI_SWITCH_CMD_TEST_BOOT5 overrides the path, e.g. to re-run
# against a fresher capture.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOOT5="${ODI_SWITCH_CMD_TEST_BOOT5:-$ROOT/test/fixtures/isp1-260922-boot5.txt}"

if [ ! -f "$BOOT5" ]; then
	echo "odi_switch_cmd_test: reference trace not found at $BOOT5" >&2
	echo "odi_switch_cmd_test: set ODI_SWITCH_CMD_TEST_BOOT5 to its path" >&2
	exit 1
fi

BIN=$(mktemp -t odi_switch_cmd_test.XXXXXX)
OUT=$(mktemp -t odi_switch_cmd_test_out.XXXXXX)
RESULT=$(mktemp -t odi_switch_cmd_test_result.XXXXXX)
trap 'rm -f "$BIN" "$OUT" "$RESULT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/omci" -o "$BIN" "$ROOT/test/odi_switch_cmd_test.c"
"$BIN" "$OUT"

set +e
python3 "$ROOT/tools/regtrace/compare.py" "$BOOT5" "$OUT" > "$RESULT"
set -e
cat "$RESULT"

fail=0

# Every bracket must PASS.
if grep -q 'DIFF' "$RESULT"; then
	echo "odi_switch_cmd_test: FAILED (a bracket did not PASS)"
	fail=1
fi

total=$(grep -c ': PASS\|: DIFF' "$RESULT" || true)
if [ "$total" -ne 91 ]; then
	echo "odi_switch_cmd_test: FAILED (expected 91 brackets, found $total -- did the bracket list change?)"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "odi_switch_cmd_test: ok (91 of 91 brackets PASS bracket-for-bracket against boot5)"
