#!/usr/bin/env bash
# odi_hwmon_test.sh -- compiles and runs odi_hwmon_test.c with the host cc.
# Only the pure decode and alarm logic of odi_hwmon.h are covered; the hwmon registration is
# kernel-only. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_hwmon_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -o "$BIN" "$ROOT/test/odi_hwmon_test.c"
"$BIN"
