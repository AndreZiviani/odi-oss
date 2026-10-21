#!/usr/bin/env bash
# odi_switch_dssteer_test.sh -- compiles and runs odi_switch_dssteer_test.c:
# the priority the downstream CF rows of a bridge connection assign, from the
# queue its GEM port points at, and so the queue its frames take. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_dssteer_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_dssteer_test.c"
"$BIN"
