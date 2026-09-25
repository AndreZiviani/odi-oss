#!/usr/bin/env bash
# odi_switch_dal_test.sh -- compiles odi_switch_dal_test.c, dumps one
# bracket per implemented leaf/instance, and checks each against its
# boot5 fixture with tools/regtrace/compare.py. Part of `make
# test-host`. No kernel, no target
# toolchain. Every test/fixtures/dal-cmd*.txt drives one check -- new
# fixtures need no edit here, the fixture directory is the source of truth.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_dal_test.XXXXXX)
OUT_DIR=$(mktemp -d -t odi_switch_dal_test_out.XXXXXX)
trap 'rm -f "$BIN"; rm -rf "$OUT_DIR"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_dal_test.c"
"$BIN" "$OUT_DIR"

fail=0
for fixture in "$ROOT"/test/fixtures/dal-cmd*.txt; do
	name=$(basename "$fixture" .txt)
	actual="$OUT_DIR/${name}.txt"
	echo "-- ${name} --"
	if [ ! -f "$actual" ]; then
		echo "${name}: DIFF no actual dump produced ($actual missing)"
		fail=1
		continue
	fi
	if ! python3 "$ROOT/tools/regtrace/compare.py" "$fixture" "$actual"; then
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "odi_switch_dal_test: FAILED"
	exit 1
fi
echo "odi_switch_dal_test: ok"
