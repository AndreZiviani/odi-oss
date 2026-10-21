#!/usr/bin/env bash
# odi_igmp_rx_test.sh -- compiles and runs odi_igmp_rx_test.c: which host-port
# frames the kernel hands igmpd, and that igmpd's own parser reads the
# delivery the kernel builds. No kernel, no target toolchain. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_igmp_rx_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -o "$BIN" "$ROOT/test/odi_igmp_rx_test.c"
"$BIN"
