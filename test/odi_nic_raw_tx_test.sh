#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# odi_nic_raw_tx_test.sh -- compiles and runs odi_nic_raw_tx_test.c: the raw
# transmit path (odi_nic_xmit_raw, as odi_omci.c reaches it) against a NIC
# with every interface down, and the order odi_ndo_stop() takes it down in.
# The functions under test are cut out of odi_nic.c as they stand, so the
# test follows the driver. No kernel, no target toolchain. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/kernel/extra/drivers/net/ethernet/odi/odi_nic.c
DIR=$(mktemp -d -t odi_nic_raw_tx_test.XXXXXX)
trap 'rm -rf "$DIR"' EXIT

# One function definition: from its signature line to the closing brace in
# column 0. A prototype (the signature ends in a semicolon before any
# brace) is skipped.
extract() {
	awk -v name="$1" '
		!on && $0 ~ ("^static [a-z_ ]*[ *]" name "\\(") { on = 1; buf = ""; brace = 0 }
		on {
			buf = buf $0 "\n"
			if ($0 ~ /\{$/) brace = 1
			if (!brace && $0 ~ /;$/) { on = 0; next }
			if (brace && $0 == "}") { printf "%s", buf; exit }
		}' "$SRC"
}

for f in odi_tx_full_locked odi_nic_xmit_raw odi_ndo_stop; do
	extract "$f" > "$DIR/$f.inc"
	[ -s "$DIR/$f.inc" ] || { echo "odi_nic_raw_tx_test: $f not found in odi_nic.c" >&2; exit 1; }
done

cc ${ODI_SAN_CFLAGS-} -std=gnu99 -Wall -Wextra -Werror -I "$DIR" -o "$DIR/test" \
	"$ROOT/test/odi_nic_raw_tx_test.c"
"$DIR/test"
