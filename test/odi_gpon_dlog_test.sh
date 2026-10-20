#!/usr/bin/env bash
# odi_gpon_dlog_test.sh -- compiles and runs odi_gpon_dlog_test.c, the held
# log lines of the GPON interrupt (odi_gpon_dlog.h). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_dlog_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_dlog_test.c"
"$BIN"
