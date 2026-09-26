#!/usr/bin/env bash
# odi_switch_sdkinit_replay_test.sh -- compiles odi_switch_sdkinit_test.c,
# runs its unit checks (unknown verb, zero-event verb, the SoC enable
# order), then replays EVERY verb in odi_switch_sdkinit_verbs[] own order
# through the mock (one bracket per verb) and diffs the result,
# bracket by bracket, against test/fixtures/
# isp1-260923-g4-sdkinit-filtered.txt with tools/regtrace/compare.py --
# "SDK-init replay": replaying odi_switch_sdkinit_verbs[]
# reproduces the capture own per-verb event sequence exactly, W/T/D/R all
# three, one bracket per verb in verb-id order.
#
# The fixture wraps each verb own filtered raw lines (TBL_ACCESS plumbing
# already dropped, same filter mksdkinit.py itself applies) in a synthetic
# mark bracket tagged with that verb own index -- a verb the reference
# capture recorded zero events for (trunk, gpio) gets an EMPTY bracket, and
# compare.py own bracket diff treats two empty sequences as a pass, not a
# skip: this is still a real assertion (that verb truly applies nothing).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIXTURE="$ROOT/test/fixtures/isp1-260923-g4-sdkinit-filtered.txt"

if [ ! -f "$FIXTURE" ]; then
	echo "odi_switch_sdkinit_replay_test: fixture not found at $FIXTURE" >&2
	exit 1
fi

BIN=$(mktemp -t odi_switch_sdkinit_test.XXXXXX)
OUT=$(mktemp -t odi_switch_sdkinit_test_out.XXXXXX)
RESULT=$(mktemp -t odi_switch_sdkinit_test_result.XXXXXX)
trap 'rm -f "$BIN" "$OUT" "$RESULT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_sdkinit_test.c"

# Unit checks first.
"$BIN"

# Then the full per-verb replay + compare.py bracket diff.
"$BIN" dump "$OUT"

set +e
python3 "$ROOT/tools/regtrace/compare.py" "$FIXTURE" "$OUT" > "$RESULT"
set -e
cat "$RESULT"

if grep -q 'DIFF' "$RESULT"; then
	echo "odi_switch_sdkinit_replay_test: FAILED (the replay diverged from the capture)"
	exit 1
fi
if ! grep -q '^summary: 25 brackets, 25 pass, 0 fail$' "$RESULT"; then
	echo "odi_switch_sdkinit_replay_test: FAILED (expected 25 brackets -- one per verb -- all passing)"
	exit 1
fi

echo "odi_switch_sdkinit_replay_test: ok (all 25 verbs replay bracket-for-bracket identical to the capture)"
