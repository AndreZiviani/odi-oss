#!/usr/bin/env bash
# odi_gpon_irq_test.sh -- compiles and runs odi_gpon_irq_test.c, the switch
# interrupt line of odi_gpon_isr.c, with the host cc against the register
# mock. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_gpon_irq_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_gpon_irq_test.c"
"$BIN"
