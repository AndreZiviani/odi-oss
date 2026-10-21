#!/usr/bin/env bash
# odi_gpon_init_clean_test.sh -- compiles and runs odi_gpon_init_clean_test.c,
# the GPON MAC reset wait and the boot clean of the GEM-port CAM, the
# Alloc-ID CAM and the flow type words (odi_gpon_init.c), with the host cc
# against the register mock and a model of the two CAMs. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_init_clean_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_init_clean_test.c"
"$BIN"
