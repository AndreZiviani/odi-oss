#!/usr/bin/env bash
# odi_switch_cmd10_test.sh -- compiles and runs odi_switch_cmd10_test.c with
# the host cc, against the same register mock as odi_switch_cmd_test.sh.
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_cmd10_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_cmd10_test.c"
"$BIN"
