#!/usr/bin/env bash
# odi_gpon_eqd_test.sh -- compiles and runs odi_gpon_eqd_test.c, the EqD
# split, pre-assigned delay and protection path rules of odi_gpon_hw.c, with the host cc against the register
# mock. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_eqd_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_eqd_test.c"
"$BIN"
