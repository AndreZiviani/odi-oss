#!/usr/bin/env bash
# odi_igmp_rx_test.sh -- compiles and runs odi_igmp_rx_test.c: which host-port
# frames (IGMP and MLD) the kernel hands igmpd, and that the parsers of igmpd
# read the delivery the kernel builds. No kernel, no target toolchain. Part
# of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_igmp_rx_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/src/diag/src" -o "$BIN" \
	"$ROOT/test/odi_igmp_rx_test.c" "$ROOT/src/igmp/mld.c" "$ROOT/src/igmp/policy.c" \
	"$ROOT/src/igmp/entry.c"
"$BIN"
