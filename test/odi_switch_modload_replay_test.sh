#!/usr/bin/env bash
# odi_switch_modload_replay_test.sh -- compiles odi_switch_modload_test.c,
# runs its mask-selection unit checks, then replays the full generated
# odi_switch_modload_events[] table (mask = ODI_SWITCH_INIT_MODLOAD_ITEM_
# ALL) through the mock and diffs the resulting dump against test/fixtures/
# isp1-260922-v7-modload-filtered.txt with tools/regtrace/compare.py:
# replaying the table through the mock reproduces the capture
# bracket-free, W and T/D entries both.
#
# "bracket-free": the source capture (the stock OMCI kernel modules loading) has no
# OMCI command brackets at all -- it happens before omcid ever runs -- so
# both the fixture and the replay dump are wrapped in one synthetic
# command-0 bracket (odi_mock_mark(0)/odi_mock_mark(0x80000000)) purely so
# compare.py, which only ever compares within a bracket, has one bracket
# to align. The fixture is a filtered copy of the real capture (test/
# fixtures/isp1-260922-v7-modload-filtered.txt's own header explains the
# filter), same vendoring precedent as test/fixtures/isp1-260922-boot5.txt
# -- so test-host stays self-contained, no dependency on anything outside
# this repo.
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

# Unit checks first (mask selection, category isolation, the pinned
# category counts and the pinned headline-rule words).
"$BIN"

# Then the full replay + compare.py diff.
"$BIN" dump "$OUT"

set +e
python3 "$ROOT/tools/regtrace/compare.py" "$FIXTURE" "$OUT" > "$RESULT"
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
