#!/usr/bin/env bash
# odi_switch_uni_limits_test.sh -- compiles and runs odi_switch_uni_limits_test.c:
# the driver commands for the learning limit (53), the UNI rates (67), the
# bridge table of ME 50 (61 and its page command), 63 and 40, and the MIB
# reset that puts the limits back. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_uni_limits_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_uni_limits_test.c"
"$BIN"
