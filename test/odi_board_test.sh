#!/usr/bin/env bash
# odi_board_test.sh -- compiles odi_board_test.c, runs its unit checks
# (replay order + kind against odi_board_init_events[] itself, the two
# board-init SoC addresses on the replay allowlist), then dumps a replay
# (wrapped in one synthetic command-bracket mark pair) and diffs that single
# bracket, write for write, against test/fixtures/
# isp1-260923-g4-board-init-filtered.txt with tools/regtrace/compare.py --
# "the board replay order": odi_board_init_events[] reproduces the reference
# capture own "== 2.94 boot" window exactly, entry for entry, REG and SOC
# kinds alike, not just self-consistently against the array the test binary
# was linked with.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIXTURE="$ROOT/test/fixtures/isp1-260923-g4-board-init-filtered.txt"

if [ ! -f "$FIXTURE" ]; then
	echo "odi_board_test: fixture not found at $FIXTURE" >&2
	exit 1
fi

BIN=$(mktemp -t odi_board_test.XXXXXX)
OUT=$(mktemp -t odi_board_test_out.XXXXXX)
RESULT=$(mktemp -t odi_board_test_result.XXXXXX)
trap 'rm -f "$BIN" "$OUT" "$RESULT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_board_test.c"

# Unit checks first (replay order/kinds, SoC allowlist membership).
"$BIN"

# Then the single-bracket dump + compare.py bracket diff.
"$BIN" dump "$OUT"

set +e
python3 "$ROOT/tools/regtrace/compare.py" "$FIXTURE" "$OUT" > "$RESULT"
set -e
cat "$RESULT"

if grep -q 'DIFF' "$RESULT"; then
	echo "odi_board_test: FAILED (the replay diverged from the capture)"
	exit 1
fi
if ! grep -q '^summary: 1 brackets, 1 pass, 0 fail$' "$RESULT"; then
	echo "odi_board_test: FAILED (expected exactly one bracket, passing)"
	exit 1
fi

echo "odi_board_test: ok (all 88 board-init events replay in the capture own order)"
