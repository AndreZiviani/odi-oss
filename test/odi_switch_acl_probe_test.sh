#!/usr/bin/env bash
# odi_switch_acl_probe_test.sh -- compiles and runs
# odi_switch_acl_probe_test.c: the hand-installed ACL rules of diag
# acl-probe (what each matches and does, its counter and unit, what is
# refused, and that removing them leaves the switch as found). No kernel, no
# target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_acl_probe_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_acl_probe_test.c"
"$BIN"
