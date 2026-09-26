#!/usr/bin/env bash
# odi_ddm_test.sh -- compiles and runs odi_ddm_test.c with the host cc.
# Unity build of odi_i2c.c, odi_ddm.c (kernel/extra) and src/diag/src/
# ddm.c together: none of the three has a __KERNEL__-only dependency
# odi_i2c.c's own host hook (odi_i2c_mock_byte()) does not already cover.
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_ddm_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/diag/src" \
	-o "$BIN" "$ROOT/test/odi_ddm_test.c" -lm
"$BIN"
