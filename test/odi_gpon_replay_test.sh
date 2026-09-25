#!/usr/bin/env bash
# odi_gpon_replay_test.sh -- compiles and runs odi_gpon_replay_test.c, then
# diffs its write log against test/fixtures/gpon-react-260922-excerpt.txt
# with tools/regtrace/gpon_replay_compare.py. Part of `make test-host`.
# No kernel, no target toolchain.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_replay_test.XXXXXX)
OUT=$(mktemp -t odi_gpon_replay_test_out.XXXXXX)
trap 'rm -f "$BIN" "$OUT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_replay_test.c"
"$BIN" "$OUT"

python3 "$ROOT/tools/regtrace/gpon_replay_compare.py" \
	"$ROOT/test/fixtures/gpon-react-260922-excerpt.txt" "$OUT"
