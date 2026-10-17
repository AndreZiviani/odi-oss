#!/usr/bin/env bash
# odi_switch_flood_test.sh -- compiles and runs odi_switch_flood_test.c:
# cmd 64, the lookup-miss flood masks, against the register mock: one
# port's bit per call, by mask type, the CPU kept out of unknown-unicast
# flooding, and the refused arguments. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_flood_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_flood_test.c"
"$BIN"
