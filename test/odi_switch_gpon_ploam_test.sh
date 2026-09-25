#!/usr/bin/env bash
# odi_switch_gpon_ploam_test.sh -- compiles and runs
# odi_switch_gpon_ploam_test.c ("open stack by default").
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_gpon_ploam_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_gpon_ploam_test.c"
"$BIN"
