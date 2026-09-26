#!/usr/bin/env bash
# odi_reg_test.sh -- compiles and runs odi_reg_test.c with the host cc.
# No kernel, no target toolchain: same unity-build shape as
# odi_switch_dal_test.sh. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_reg_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_reg_test.c"
"$BIN"
