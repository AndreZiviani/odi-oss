#!/usr/bin/env bash
# odi_gpon_flow_test.sh -- compiles and runs odi_gpon_flow_test.c, the
# per-flow GEM counter sampler against a clear-on-read register model, with
# the host cc. UPDATE=1 rewrites test/fixtures/odi_gpon_flows.txt. Part of
# `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_flow_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_flow_test.c"
"$BIN" "$ROOT/test/fixtures"
