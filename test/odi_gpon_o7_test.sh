#!/usr/bin/env bash
# odi_gpon_o7_test.sh -- compiles and runs odi_gpon_o7_test.c: the module's
# TX-disable line asserted in Emergency Stop (O7) and released on leaving
# it, and left alone on Deactivate_ONU-ID and LOS. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_o7_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_o7_test.c"
"$BIN"
