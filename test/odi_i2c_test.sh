#!/usr/bin/env bash
# odi_i2c_test.sh -- compiles and runs odi_i2c_test.c with the host cc.
# odi_i2c.c is #included directly by the test (unity build, same posture
# as odi_gpon_irq_test.sh): no kernel, no target toolchain, its portable core
# (odi_i2c_read_bytes()) has no __KERNEL__ dependency. Part of `make
# test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_i2c_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_i2c_test.c"
"$BIN"
