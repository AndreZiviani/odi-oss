#!/usr/bin/env bash
# odi_switch_init_platform_test.sh -- compiles and runs
# odi_switch_init_platform_test.c (the exact write-sequence assertion),
# then re-runs odi_switch_cmd_test.sh with
# ODI_SWITCH_CMD_TEST_INIT_PLATFORM=1 to confirm running
# odi_switch_init_platform() before the 91-bracket boot5 replay does not
# disturb that comparison. No kernel, no target toolchain. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_init_platform_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_init_platform_test.c"
"$BIN"

echo "-- re-running odi_switch_cmd_test.sh with init_platform() first --"
ODI_SWITCH_CMD_TEST_INIT_PLATFORM=1 bash "$ROOT/test/odi_switch_cmd_test.sh"
