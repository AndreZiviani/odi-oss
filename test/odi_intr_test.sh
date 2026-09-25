#!/usr/bin/env bash
# odi_intr_test.sh -- compiles and runs odi_intr_test.c with the host cc.
# odi_intr.c is #included directly by the test (unity build, same posture
# as odi_switch_sdkinit_test.c): no kernel, no target toolchain, its
# portable core (odi_intr_register()/_enable()/dispatch_once()) has no
# __KERNEL__ dependency. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_intr_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_intr_test.c"
"$BIN"
