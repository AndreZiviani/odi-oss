#!/usr/bin/env bash
# odi_optics_model_test.sh -- compiles and runs odi_optics_model_test.c with
# the host cc: odi_i2c.c, odi_ddm.c (kernel/extra), src/diag/src/ddm.c and
# the scriptable SFF-8472 model (odi_optics_model.h), unity-built together
# the same way odi_ddm_test.sh already builds the first three. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_optics_model_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/diag/src" \
	-o "$BIN" "$ROOT/test/odi_optics_model_test.c" -lm
"$BIN"
