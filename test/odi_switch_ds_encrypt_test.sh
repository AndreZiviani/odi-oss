#!/usr/bin/env bash
# odi_switch_ds_encrypt_test.sh -- compiles and runs
# odi_switch_ds_encrypt_test.c: odi_switch_gpon_encrypt_port(), the AES
# bit of a downstream GEM port, set and cleared through its slot, the
# read-modify-write preserving every other DSF_GEM_FLOW_TYPE.FLAGS bit,
# and the 0x02 -> 0x12 transition a capture recorded for the ISP1 data
# GEM ports.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_ds_encrypt_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_ds_encrypt_test.c"
"$BIN"
