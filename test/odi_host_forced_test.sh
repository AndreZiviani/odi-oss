#!/usr/bin/env bash
# odi_host_forced_test.sh -- compiles and runs odi_host_forced_test.c: the
# forced host port of odi_switch.host_forced in each direction, cmd 32 on a
# forced port, the ISP1 provisioning with it, and the platform default,
# against the register mock. No kernel, no target toolchain. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_host_forced_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_host_forced_test.c"
"$BIN"
