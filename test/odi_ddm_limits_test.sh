#!/usr/bin/env bash
# odi_ddm_limits_test.sh -- compiles and runs odi_ddm_limits_test.c with the
# host cc: the module-limit and monitor-type selectors of odi_ddm.c and the
# consistent 16-bit read, against test/odi_optics_model.h.
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_ddm_limits_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/diag/src" \
	-o "$BIN" "$ROOT/test/odi_ddm_limits_test.c" -lm
"$BIN"
