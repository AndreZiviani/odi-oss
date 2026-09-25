#!/usr/bin/env bash
# odi_switch_mmio_bounds_test.sh -- compiles and runs
# odi_switch_mmio_bounds_test.c. No kernel, no target toolchain. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_mmio_bounds_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_mmio_bounds_test.c"
"$BIN"
