#!/usr/bin/env bash
# odi_switch_flows_test.sh -- compiles and runs odi_switch_flows_test.c with
# the host cc, against the same register mock as odi_switch_cmd_test.sh.
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_flows_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/omci" -o "$BIN" \
	"$ROOT/test/odi_switch_flows_test.c"
"$BIN"
