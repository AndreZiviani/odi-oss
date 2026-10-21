#!/usr/bin/env bash
# odi_gpon_los_test.sh -- compiles and runs odi_gpon_los_test.c with the
# host cc: a fibre pull in O5 with no PLOAM traffic, TO2, the release of the
# activation rows and the re-ranging, against the register mock (the file
# header has the scenarios). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_los_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_los_test.c"
"$BIN"
