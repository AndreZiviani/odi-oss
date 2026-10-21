#!/usr/bin/env bash
# odi_switch_mcast_test.sh -- compiles and runs odi_switch_mcast_test.c: the
# ACL rule that copies the host port's IGMP to the CPU (what it matches and
# what it must not), the drop of unregistered IPv4 multicast from the PON,
# and the lease that takes both back with the rows the snooper wrote.
# No kernel, no target toolchain. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_mcast_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_switch_mcast_test.c"
"$BIN"
