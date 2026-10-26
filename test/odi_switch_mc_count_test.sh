#!/usr/bin/env bash
# odi_switch_mc_count_test.sh -- compiles and runs
# odi_switch_mc_count_test.c: the multicast group counters igmpd meters its
# groups with (what each rule matches, that it only counts, its counter, the
# folded total, what is refused, and that removing them leaves the switch as
# found). No kernel, no target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_mc_count_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_mc_count_test.c"
"$BIN"
