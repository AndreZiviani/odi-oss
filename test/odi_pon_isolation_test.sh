#!/usr/bin/env bash
# odi_pon_isolation_test.sh -- compiles and runs odi_pon_isolation_test.c:
# the PON port's isolation mask from the reset words a stick reads, a
# forwarding model of every frame kind from the fibre, and the NIC's PON
# drop on descriptor words logged on a stick. No kernel, no target
# toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_pon_isolation_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_pon_isolation_test.c"
"$BIN"
