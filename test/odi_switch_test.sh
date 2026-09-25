#!/usr/bin/env bash
# odi_switch_test.sh -- compiles and runs odi_switch_test.c with the host cc.
# No kernel, no target toolchain: odi_switch_hw.h, odi_switch_tbl.c and
# odi_switch_mock.h all have no kernel dependency (guarded by
# #ifdef __KERNEL__ / else). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_test.c"
"$BIN"
