#!/usr/bin/env bash
# odi_gpon_us_ploam_test.sh -- compiles and runs odi_gpon_us_ploam_test.c,
# the upstream PLOAM push against the register mock: pushed only when the
# urgent queue has room within the bounded wait, counted as dropped
# otherwise. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_us_ploam_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_us_ploam_test.c"
"$BIN"
