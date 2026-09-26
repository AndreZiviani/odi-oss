#!/usr/bin/env bash
# odi_wdt_test.sh -- compiles and runs odi_wdt_test.c with the host cc.
# odi_wdt.c is #included directly by the test (unity build, same posture
# as odi_gpon_irq_test.c): no kernel, no target toolchain, its portable core
# and the allowlisted register accessors both have a host-side path with
# no __KERNEL__ dependency. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_wdt_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_wdt_test.c"
"$BIN"
