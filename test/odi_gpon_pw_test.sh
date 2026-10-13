#!/usr/bin/env bash
# odi_gpon_pw_test.sh -- compiles and runs odi_gpon_pw_test.c with the host cc.
# odi_gpon_pw.c has no kernel dependency. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_pw_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
D=$ROOT/kernel/extra/drivers/net/ethernet/odi
cc -std=c99 -Wall -Wextra -Werror -I "$D" -o "$BIN" \
	"$ROOT/test/odi_gpon_pw_test.c" "$D/odi_gpon_pw.c"
"$BIN"
