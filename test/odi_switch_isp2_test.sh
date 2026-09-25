#!/usr/bin/env bash
# odi_switch_isp2_test.sh -- cmd 51 on the isp2 service profile (PPPoE over
# C-TAG VID 10, VEIP ingress, plus the multicast rule). Two checks:
#
#   1. odi_switch_isp2_test.c: the CF rows the derivation writes, pinned
#      exactly and checked field by field against what the isp2 stock
#      image has in its classification table.
#   2. The tail of our second cmd 51 bracket against the tail of the isp2
#      stock bracket (test/fixtures/isp2-260921-cmd51-tail.txt): from VLAN
#      sweep row 3107 on, every switch-core register write and every VLAN
#      row, in order -- the plain-register template and the row-by-row
#      VLAN pass, where VID 10 must be 0x15 and every other row 0.
#
# Both sides go through the same transform the fixture header describes,
# so the comparison is write for write.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIXTURE="$ROOT/test/fixtures/isp2-260921-cmd51-tail.txt"

BIN=$(mktemp -t odi_switch_isp2_test.XXXXXX)
OUT=$(mktemp -t odi_switch_isp2_test_out.XXXXXX)
trap 'rm -f "$BIN" "$OUT"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -I "$ROOT/src/omci" -o "$BIN" "$ROOT/test/odi_switch_isp2_test.c"
"$BIN" "$OUT"

python3 - "$OUT" "$FIXTURE" <<'PY'
import sys

VLAN = 0x17        # enum odi_sw_table index of the VLAN table in the host log
START = 3107       # first sweep row the stock window still holds


def keep(a):
    return (0x11000 <= a < 0x1d000 and not 0x12000 <= a <= 0x1202c) or 0x2a000 <= a < 0x2b000


def ours(path):
    """Second bracket of the ring dump, as ('T', row, value) / ('W', addr, value)."""
    ents = [l.split() for l in open(path) if l[:1].isdigit()]
    ends = [i for i, e in enumerate(ents) if e[1] == 'M' and int(e[2], 16) == 0x80000033]
    starts = [i for i, e in enumerate(ents) if e[1] == 'M' and int(e[2], 16) == 0x33]
    body = ents[starts[1] + 1:ends[1]]
    out, i = [], 0
    while i < len(body):
        k, a, v = body[i][1], int(body[i][2], 16), int(body[i][3], 16)
        if k == 'W':
            if keep(a):
                out.append(('W', a, v))
            i += 1
            continue
        if k == 'T':
            table, row = a >> 16, a & 0xffff
            words, j = [], i + 1
            while j < len(body) and body[j][1] == 'D':
                words.append(int(body[j][3], 16))
                j += 1
            # A run closes with an R when the next table op arrives, so
            # plain writes made meanwhile can sit between the D and its R.
            run, n = 1, j
            while n < len(body) and body[n][1] == 'W':
                n += 1
            if n < len(body) and body[n][1] == 'R' and int(body[n][2], 16) == a:
                run = int(body[n][3], 16)
                del body[n]
            if table == VLAN:
                for r in range(row, row + run):
                    out.append(('T', r, words[0]))
            i = j
            continue
        i += 1
    first = next(n for n, e in enumerate(out) if e[0] == 'T' and e[1] == START)
    return out[first:]


def fold(seq):
    res = []
    for e in seq:
        if e[0] == 'T' and res and res[-1][0] == 'T' and res[-1][2] + 1 == e[1] and res[-1][3] == e[2]:
            res[-1][2] = e[1]
            continue
        res.append(['T', e[1], e[1], e[2]] if e[0] == 'T' else ['W', e[1], e[2]])
    lines = []
    for r in res:
        if r[0] == 'T':
            rows = '%d..%d' % (r[1], r[2]) if r[1] != r[2] else '%d' % r[1]
            lines.append('T VLAN %s 0x%08x' % (rows, r[3]))
        else:
            lines.append('W 0x%08x 0x%08x' % (r[1], r[2]))
    return lines


got = fold(ours(sys.argv[1]))
want = [l.strip() for l in open(sys.argv[2]) if l.strip() and not l.startswith('#')]
for n, (g, w) in enumerate(zip(got, want)):
    if g != w:
        print('isp2 tail: DIFF at line %d: expected %s got %s' % (n + 1, w, g))
        sys.exit(1)
if len(got) != len(want):
    print('isp2 tail: DIFF in length: expected %d lines, got %d' % (len(want), len(got)))
    sys.exit(1)
print('isp2 tail: PASS (%d lines, VLAN row 10 = 0x15, template identical)' % len(want))
PY
echo "odi_switch_isp2_test: ok"
