#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# odi_replay_blob_test.sh -- compiles and runs odi_replay_blob_test.c (the
# blob parser and the GPON init replay, unity build against the mock, no
# kernel), then has tools/regtrace/replayblob.py validate and dump each
# table the image ships, so the Python writer and the C reader are checked
# against the same bytes. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FW="$ROOT/rootfs/skeleton/lib/firmware/odi"
BIN=$(mktemp -t odi_replay_blob_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_replay_blob_test.c"
"$BIN"

for spec in sdkinit:7429 modload:3619 gpon_init:800; do
	name=${spec%%:*}
	want=${spec#*:}
	head=$(python3 "$ROOT/tools/regtrace/replayblob.py" dump "$FW/$name.bin" | sed -n 1p)
	case "$head" in
	"# odi replay blob: table=$name version=1 records=$want "*) ;;
	*) echo "odi_replay_blob_test: $name.bin: unexpected header: $head" >&2; exit 1 ;;
	esac
done
echo "odi_replay_blob_test: replayblob.py agrees on all three tables"

# trim-gpon: the shipped gpon_init.bin holds none of the trimmed sweeps,
# and a blob that holds each once loses exactly those records, the rest
# kept in order. A sweep cut short, or found twice, is refused.
python3 - "$ROOT/tools/regtrace" "$FW/gpon_init.bin" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import replayblob as rb

shipped = open(sys.argv[2], "rb").read()
_t, _c, recs = rb.unpack(shipped)
regs = [rb._gpon_plain_reg(r) for r in recs]
for name, rows, row_writes in rb.GPON_INIT_TRIMMED:
    first = list(row_writes(0))
    assert not any(regs[i:i + len(first)] == first for i in range(len(regs))), \
        "shipped gpon_init.bin still starts the %s" % name

keep = [rb.gpon_reg(0x70000c, 1), rb.gpon_reg(0x70000c, 0), rb.gpon_reg(0x700040, 0x22)]
sweeps = [[rb.gpon_reg(o, v) for r in range(rows) for o, v in row_writes(r)]
          for _n, rows, row_writes in rb.GPON_INIT_TRIMMED]
blob = rb.pack(rb.TABLE_GPON_INIT, keep[:1] + sweeps[0] + keep[1:2] + sweeps[1]
               + sweeps[2] + keep[2:] + sweeps[3])
_t, _c, out = rb.unpack(rb.trim_gpon_init(blob))
assert out == keep, "trim-gpon kept %d records, want the 3 others" % len(out)

for bad, why in ((keep + sweeps[0][:-1] + sweeps[1] + sweeps[2] + sweeps[3], "cut short"),
                 (keep + sweeps[0] * 2 + sweeps[1] + sweeps[2] + sweeps[3], "twice")):
    try:
        rb.trim_gpon_init(rb.pack(rb.TABLE_GPON_INIT, bad))
    except ValueError:
        continue
    raise AssertionError("trim-gpon took a sweep %s" % why)
print("odi_replay_blob_test: trim-gpon ok")
EOF
