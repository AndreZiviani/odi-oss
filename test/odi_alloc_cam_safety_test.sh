#!/usr/bin/env bash
# odi_alloc_cam_safety_test.sh -- compiles and runs odi_alloc_cam_safety_test.c
# with the host cc: the Alloc-ID CAM holds only what the OLT assigned by PLOAM
# in the current activation, through LOS, TO2, Deactivate_ONU-ID, O7,
# gpondeact and omcid's command 21 (the file header has the G.984.3 rules).
# Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_alloc_cam_safety_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_alloc_cam_safety_test.c"
"$BIN"
