#!/usr/bin/env bash
# odi_gpon_cam_test.sh -- compiles and runs odi_gpon_cam_test.c: the GPON
# CAM deletes and the search before a write wait for DONE, and a CAM write
# or delete that does not complete moves no bookkeeping (Alloc-ID rows,
# the OMCC GEM port, the cmd 25 DS slot and flow). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_cam_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_cam_test.c"
"$BIN"
