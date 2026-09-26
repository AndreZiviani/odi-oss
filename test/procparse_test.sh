#!/usr/bin/env bash
# procparse_test.sh -- the /proc parsers our userland tools read the 6.18
# drivers with: src/omci/procparse.h (redirect holder, GPON serial) and
# src/diag/src/gpon_status.c (ONU state, LOS sample, alarm bits). Host cc
# only. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=$(mktemp -d -t procparse_test.XXXXXX)
trap 'rm -rf "$DIR"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I "$ROOT/src/omci" -o "$DIR/procparse" \
	"$ROOT/test/procparse_test.c"
"$DIR/procparse"
cc -std=c11 -Wall -Wextra -Werror -I "$ROOT/src/diag/src" -o "$DIR/gpon_status" \
	"$ROOT/src/diag/test/gpon_status_test.c" "$ROOT/src/diag/src/gpon_status.c"
"$DIR/gpon_status"
