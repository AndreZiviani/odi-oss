#!/usr/bin/env bash
# omci_defaults_test.sh -- the generated defaults table (src/omci/generated/
# omci_defaults.c) carries capability values only: no environment-specific
# value of the stick it was captured from, and no hex value turned into text.
#
#   1. no numeric attribute holds the printable ASCII "0x" (0x30 0x78): a
#      0x-prefixed value of a dump is a number, and was once stored as its own
#      characters (the reserved priority queues);
#   2. none of the attributes omcid answers from live state or from the
#      configuration is in the table (circuit pack serial, version and vendor
#      id; ONU-G OntState; ONU2-G SysUpTime; ANI-G optical levels; UNI sensed
#      type and duplex; private VLAN type and mode), nor any attribute the
#      OLT can write, outside the verified list;
#   3. the generator, run on a small synthetic dump: a 0x value is read as a
#      number, 0x garbage is refused, a skipped attribute does not come out.
#   4. T-CONT Policy (262/3) is 1 on all 16 T-CONTs, whatever the dump says.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GEN="$ROOT/src/omci/generated"
TOOL="$ROOT/src/omci/tools/mib-defaults.py"

python3 -I - "$GEN" <<'PY'
import json, re, sys
gen = sys.argv[1]
model = {e['table']['classId']: e for e in json.load(open(gen + '/omci_mib.json'))
         if e['table']['classId'] is not None}
live = {(6, 3), (6, 4), (6, 5), (256, 14), (257, 10), (263, 10), (263, 14),
        (11, 2), (11, 7), (65535, 1), (65535, 2), (65535, 3), (65535, 4),
        (277, 7), (256, 6)}
bad = []
for l in open(gen + '/omci_defaults.c'):
    m = re.match(r'\s*\{ (\d+), 0x[0-9a-f]+, (\d+), (\d+), \{ ([^}]*) \} \}', l)
    if not m:
        continue
    cls, k, w = int(m[1]), int(m[2]), int(m[3])
    data = [int(x, 16) for x in m[4].split(',')]
    a = model[cls]['attrs'][k]
    if a['type'] != 'string' and any(data[i] == 0x30 and data[i + 1] == 0x78
                                     for i in range(len(data) - 1)):
        bad.append('numeric attribute with ASCII 0x: ' + l.strip())
    if (cls, k) in live:
        bad.append('live or configured attribute in the table: ' + l.strip())
# T-CONT Policy is the reviewed exception to "no OLT-writable attribute": 1 on
# every one of the 16 T-CONTs, 0x8000..0x800f.
pol = [l for l in open(gen + '/omci_defaults.c')
       if re.match(r'\s*\{ 262, 0x80[0-9a-f]{2}, 3, 1, \{ 0x01 \} \}', l)]
if len(pol) != 16:
    bad.append('T-CONT Policy rows: %d, expected 16 at value 1' % len(pol))
if bad:
    print('\n'.join(bad)); sys.exit(1)
print('ok    defaults table: no ASCII 0x in a numeric attribute, no live attribute')
PY

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# ANI-G 0x8001 is an autonomous instance. GemBlkLen (3) is an ONU-initialised
# value that is kept, SDThreshold (7) too; SFThreshold (6) is given a bad hex
# value; OpticalSignalLevel (10) is skipped; ARC (8) has a value no table row
# may carry through a malformed hex.
cat > "$tmp/dump.txt" <<'DUMP'
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
Anig
EntityID: 0x8001
GemBlkLen: 0x0030
SFThreshold: 0x0z
SDThreshold: 0x09
OpticalSignalLevel: 0xd2e3
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
Tcont
EntityID: 0x8000
AllocID: 0x0123
Policy: 0
DUMP
mkdir -p "$tmp/out"
python3 -I "$TOOL" "$tmp/dump.txt" --c "$tmp/out" 2> "$tmp/err" || { cat "$tmp/err"; exit 1; }
rows=$(grep -E '^[[:space:]]*\{ [0-9]+, 0x' "$tmp/out/omci_defaults.c" | sed 's/[[:space:]]*\/\*.*//; s/^[[:space:]]*//')
want='{ 262, 0x8000, 3, 1, { 0x01 } },
{ 263, 0x8001, 3, 2, { 0x00, 0x30 } },
{ 263, 0x8001, 7, 1, { 0x09 } },'
if [ "$rows" != "$want" ]; then
	echo "FAIL: generator output on the synthetic dump" >&2
	printf 'expected:\n%s\ngot:\n%s\n' "$want" "$rows" >&2
	exit 1
fi
echo "ok    generator: a 0x value is a number, 0x garbage is refused, a live attribute is skipped, T-CONT Policy is forced to 1"
