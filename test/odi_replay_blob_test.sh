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
# The replay tables the test loads, built from tools/regtrace/tables/.
python3 "$ROOT/tools/regtrace/replayblob.py" build-tables
FW="$ROOT/out/firmware/odi"
BIN=$(mktemp -t odi_replay_blob_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc ${ODI_SAN_CFLAGS-} -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_replay_blob_test.c"
"$BIN"

for spec in sdkinit:1801 modload:725 gpon_init:800; do
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

# trim-modload-gpon: the shipped modload.bin writes nothing in the GPON MAC
# window; a GPON MAC write that gpon_init repeats after its reset is
# dropped, the rest kept, and one it does not repeat is refused.
modload = open(sys.argv[2].replace("gpon_init.bin", "modload.bin"), "rb").read()
_t, _c, recs = rb.unpack(modload)
for r in recs:
    kind, _cat, _grp, _verb, _tbl, _n, off, _v, *_w = rb._RECORD_SWITCH.unpack(r)
    assert not (kind == rb.KIND_REG and rb.GPON_MAC_FIRST <= off < rb.GPON_MAC_END), \
        "shipped modload.bin writes 0x%06x" % off
gi = rb.pack(rb.TABLE_GPON_INIT, [rb.gpon_reg(0x70000c, 1), rb.gpon_reg(0x70000c, 0),
                                  rb.gpon_reg(0x700040, 0x22)])
other = rb.switch_reg(0x1c024, 0xf)
ml = rb.pack(rb.TABLE_MODLOAD, [rb.switch_reg(0x70000c, 0x101), other,
                                rb.switch_reg(0x700040, 0x22)])
_t, _c, out = rb.unpack(rb.trim_modload_gpon(ml, gi))
assert out == [other], "trim-modload-gpon kept %d records, want 1" % len(out)
try:
    rb.trim_modload_gpon(rb.pack(rb.TABLE_MODLOAD, [rb.switch_reg(0x700044, 1)]), gi)
except ValueError:
    pass
else:
    raise AssertionError("trim-modload-gpon dropped a write gpon_init does not repeat")
print("odi_replay_blob_test: trim-modload-gpon ok")
EOF
