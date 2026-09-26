#!/usr/bin/env bash
# odi_init_test.sh -- compiles and runs odi_init_test.c with the
# host cc. odi_init.c is #included directly by the test (unity build,
# same posture as odi_gpon_irq_test.c): no kernel, no target toolchain, its
# portable core (odi_init_apply()) has no __KERNEL__ dependency, and
# the test itself stubs the two functions it calls out to. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_init_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" \
   -I "$ROOT/kernel/extra/drivers/net/ethernet/odi" \
   -o "$BIN" "$ROOT/test/odi_init_test.c"
"$BIN"
