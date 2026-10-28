#!/usr/bin/env bash
# odi_sfp_mirror_test.sh -- compiles and runs odi_sfp_mirror_test.c with the
# host cc: the host-side SFP EEPROM (odi_sfp_mirror.c) over odi_i2c.c and
# odi_ddm.c, unity build against the register model. The work item and the
# kernel parameters are kernel-only. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_sfp_mirror_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT
cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_sfp_mirror_test.c"
"$BIN"
