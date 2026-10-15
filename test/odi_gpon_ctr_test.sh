#!/usr/bin/env bash
# odi_gpon_ctr_test.sh -- compiles and runs odi_gpon_ctr_test.c, the GPON
# link counter sampler against a clear-on-read register model, with the
# host cc. UPDATE=1 rewrites test/fixtures/odi_gpon_ctr_status.txt and
# odi_gpon_ctr_counters.txt. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_ctr_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_ctr_test.c"
"$BIN" "$ROOT/test/fixtures"
