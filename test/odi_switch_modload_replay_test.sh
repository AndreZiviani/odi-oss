#!/usr/bin/env bash
# odi_switch_modload_replay_test.sh -- compiles odi_switch_modload_test.c,
# runs its unit checks, then replays the shipped modload.bin through the
# mock and compares the result with the module-load capture
# (test/fixtures/isp1-260922-v7-modload-filtered.txt) using
# tools/regtrace/compare.py: W and T/D entries both, in order.
#
# The shipped blob carries the production selection (replayblob.py
# filter): the three LUT flood masks include the CPU port, 0xf where the
# capture has its own value. The capture is compared with that one
# substitution applied, plus the unknown-multicast mask the kernel writes
# back to 0x7 after the replay, so any other difference still fails.
#
# The capture has no OMCI command brackets (the stock modules load before
# omcid runs), so both sides are wrapped in one synthetic command-0
# bracket, the one compare.py aligns.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIXTURE="$ROOT/test/fixtures/isp1-260922-v7-modload-filtered.txt"

if [ ! -f "$FIXTURE" ]; then
	echo "odi_switch_modload_replay_test: fixture not found at $FIXTURE" >&2
	exit 1
fi

BIN=$(mktemp -t odi_switch_modload_test.XXXXXX)
OUT=$(mktemp -t odi_switch_modload_test_out.XXXXXX)
RESULT=$(mktemp -t odi_switch_modload_test_result.XXXXXX)
trap 'rm -f "$BIN" "$OUT" "$RESULT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_modload_test.c"

# Unit checks first.
"$BIN"

# Then the full replay + compare.py diff.
"$BIN" dump "$OUT"

WANT=$(mktemp -t odi_switch_modload_test_want.XXXXXX)
trap 'rm -f "$BIN" "$OUT" "$RESULT" "$WANT"' EXIT
sed -E 's/^([0-9]+ W 0x0001c02[048]) 0x[0-9a-f]{8}$/\1 0x0000000f/' "$FIXTURE" > "$WANT"
# And one write after the replay: odi_switch_init_modload() takes the CPU
# out of the unknown-multicast flood mask again, the capture's 0x7.
# Before the closing bracket mark, the fixture's last line.
awk '$2 == "M" && $3 == "0x80000000" { print t " W 0x0001c024 0x00000007" } $2 != "M" { t = $1 } { print }' \
	"$WANT" > "$WANT.new" && mv "$WANT.new" "$WANT"

set +e
python3 "$ROOT/tools/regtrace/compare.py" "$WANT" "$OUT" > "$RESULT"
set -e
cat "$RESULT"

if grep -q 'DIFF' "$RESULT"; then
	echo "odi_switch_modload_replay_test: FAILED (the replay diverged from the capture)"
	exit 1
fi
if ! grep -q '^cmd 0 #1: PASS$' "$RESULT"; then
	echo "odi_switch_modload_replay_test: FAILED (expected exactly one bracket, cmd 0 #1: PASS)"
	exit 1
fi

echo "odi_switch_modload_replay_test: ok (replay matches the capture, W and T/D both)"
