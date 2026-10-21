#!/usr/bin/env bash
# odi_switch_bdgconn_extvlan_test.sh -- cmd 51 for the extended VLAN rules of
# class 171, from the table entry to the classifier row (the plan of omcid
# included, built for the host).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_bdgconn_extvlan_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/omci" \
	-I "$ROOT/kernel/extra/drivers/net/ethernet/odi/uapi" \
	-o "$BIN" "$ROOT/test/odi_switch_bdgconn_extvlan_test.c"
"$BIN"
