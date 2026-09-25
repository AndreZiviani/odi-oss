#!/usr/bin/env bash
# odi_gpon_test.sh -- compiles and runs odi_gpon_test.c with the host cc.
# No kernel, no target toolchain: odi_gpon_hw.h, odi_gpon_ploam.c and
# odi_gpon_fsm.c all have no kernel dependency (guarded by
# #ifdef __KERNEL__ / else). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_gpon_test.c" \
	"$ROOT/kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c" \
	"$ROOT/kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
"$BIN"
