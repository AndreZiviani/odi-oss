#!/usr/bin/env bash
# odi_reregister_test.sh -- compiles and runs odi_reregister_test.c with the
# host cc: one boot on ISP1, two more registrations in the same boot, and the
# OMCC upstream path plus the whole upstream queue state checked after each
# (the file header has the scenario). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_reregister_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_reregister_test.c"
"$BIN"
