#!/usr/bin/env bash
# odi_gpon_hold_test.sh -- compiles and runs odi_gpon_hold_test.c: the kept
# Emergency Stop (GPON_KEEP_DISABLE) in the GPON leaf. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_hold_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_hold_test.c"
"$BIN"
