#!/usr/bin/env bash
# odi_host_autoneg_test.sh -- compiles and runs odi_host_autoneg_test.c: the
# HOST_FIBER_CTRL writes of odi_switch.host_autoneg in each direction, the
# platform step, and the host link read, against the register mock. No kernel, no target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_host_autoneg_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_host_autoneg_test.c"
"$BIN"
