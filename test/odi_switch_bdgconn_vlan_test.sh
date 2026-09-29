#!/usr/bin/env bash
# odi_switch_bdgconn_vlan_test.sh -- cmd 51 for the class 84 forward
# operation rules: their CF match and the VLAN rows they leave.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_bdgconn_vlan_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_bdgconn_vlan_test.c"
"$BIN"
