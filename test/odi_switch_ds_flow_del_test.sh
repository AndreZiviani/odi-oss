#!/usr/bin/env bash
# odi_switch_ds_flow_del_test.sh -- compiles and runs
# odi_switch_ds_flow_del_test.c: cmd 25 downstream with ena 0 deletes the
# flow (CAM row cleared and read back clear, flow type word 0, slot record
# and readback row dropped), a delete that does not complete or a row that
# does not clear is an error that keeps the flow, cmd 0 deletes every
# downstream flow and leaves the OMCC row, and /proc/odi_gpon/cam reads the
# GEM rows back. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_ds_flow_del_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_ds_flow_del_test.c"
"$BIN"
