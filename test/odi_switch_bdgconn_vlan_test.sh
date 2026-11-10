#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# odi_switch_bdgconn_vlan_test.sh -- cmd 51 for the class 84 forward
# operation rules: their CF match and the VLAN rows they leave.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_bdgconn_vlan_test.XXXXXX)
OUT=$(mktemp -t odi_switch_bdgconn_vlan_test_out.XXXXXX)
trap 'rm -f "$BIN" "$OUT"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_bdgconn_vlan_test.c"
rc=0
"$BIN" > "$OUT" || rc=$?
cat "$OUT"
# The switch log of a refused downstream EtherType row: the line after the
# marker the test prints.
want="odi_switch_cmd: bdgconn: no free row of the second layout for an EtherType rule"
got=$(awk '/^-- log of the refused row:$/ { getline; print; exit }' "$OUT")
if [ "$got" = "$want" ]; then
	printf '  %-66s ok\n' "the log names the second layout"
else
	printf '  %-66s FAIL\n' "the log names the second layout"
	printf '    want: %s\n    got:  %s\n' "$want" "$got"
	rc=1
fi
exit "$rc"
