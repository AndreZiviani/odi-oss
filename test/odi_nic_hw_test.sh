#!/usr/bin/env bash
# odi_nic_hw_test.sh -- compiles and runs odi_nic_hw_test.c with the host cc.
# No kernel, no target toolchain: odi_nic_hw.h has no kernel dependency
# (guarded by #ifdef __KERNEL__ / else). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_nic_hw_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -o "$BIN" "$ROOT/test/odi_nic_hw_test.c"
"$BIN"
