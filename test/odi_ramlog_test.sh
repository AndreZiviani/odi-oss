#!/usr/bin/env bash
# odi_ramlog_test.sh -- compiles and runs odi_ramlog_test.c with the host cc.
# odi_ramlog.c is #included directly by the test (unity build, same posture
# as odi_wdt_test.c): no kernel, no target toolchain, its whole format is
# plain C with no __KERNEL__ dependency. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_ramlog_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_ramlog_test.c"
"$BIN"
