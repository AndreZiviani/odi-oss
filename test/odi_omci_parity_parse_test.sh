#!/usr/bin/env bash
# odi_omci_parity_parse_test.sh -- compiles and runs
# odi_omci_parity_parse_test.c against the two real parity tables this
# repo ships. No kernel, no target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_omci_parity_parse_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc -std=c99 -Wall -Wextra -Werror -o "$BIN" "$ROOT/test/odi_omci_parity_parse_test.c"
"$BIN" "$ROOT/tools/regdump/parity-v6.table" "$ROOT/tools/regdump/parity-v6-untried.table"
