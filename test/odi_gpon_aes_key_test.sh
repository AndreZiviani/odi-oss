#!/usr/bin/env bash
# odi_gpon_aes_key_test.sh -- compiles and runs odi_gpon_aes_key_test.c, the
# Request_Key path through the FSM and the register leaves against the
# register mock: a new key per request, the same bytes upstream and in the
# key engine. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_aes_key_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_aes_key_test.c"
"$BIN"
