#!/usr/bin/env bash
# odi_host_pause_test.sh -- compiles and runs odi_host_pause_test.c: the
# TX_PAUSE force on the host port after the platform settings, after the
# ISP1 provisioning and with odi_switch.host_pause toggled, against the
# register mock. No kernel, no target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_host_pause_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_host_pause_test.c"
"$BIN"
