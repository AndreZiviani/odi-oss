#!/usr/bin/env bash
# odi_switch_dsq_test.sh -- compiles and runs odi_switch_dsq_test.c: the
# schedule cmd 23 gives the UNI's downstream queues (strict or weighted, the
# weight, the priority map) and the MIB reset that puts it back. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_dsq_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_dsq_test.c"
"$BIN"
