#!/usr/bin/env bash
# odi_switch_bdgconn_uplink_test.sh -- the upstream classifier rows read back
# for the IGMP reports igmpd takes and sends up itself: which row a frame
# matches, its tag treatment and flow, and the row that carries a VLAN up.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_bdgconn_uplink_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/omci" \
	-I "$ROOT/kernel/extra/drivers/net/ethernet/odi/uapi" \
	-o "$BIN" "$ROOT/test/odi_switch_bdgconn_uplink_test.c"
"$BIN"
