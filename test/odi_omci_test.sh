#!/usr/bin/env bash
# odi_omci_test.sh -- compiles and runs odi_omci_test.c with the host cc.
# No kernel, no target toolchain: odi_omci_wire.h has no kernel dependency
# (guarded by #ifdef __KERNEL__ / else). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_omci_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -o "$BIN" "$ROOT/test/odi_omci_test.c"
"$BIN"
