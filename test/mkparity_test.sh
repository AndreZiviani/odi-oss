#!/bin/sh
# Exercises tools/regdump/mkparity.py against a small synthetic diff.py
# capture: the address-range exclusion (snapshot latches, thermal, SerDes,
# PTP, EPON), the full-
# physical-address-to-bare-offset translation, the full-word mask, the
# diff's own line order preserved, and a line diff.py did not produce
# (the header/summary/family-count lines) silently ignored. Runs entirely
# against a fixture -- no dependency on the real regdump-v6-vs-s7.txt
# capture.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cat > "$T/diff.txt" <<'EOF'
Configuration registers that differ (v6 working / s7 broken):

    1b011018 MAX_FRAME_LEN_1          000007ef / 00000000

1b000000 PHY_ACCESS_DATA: a=00003a00 b=0000fffb xor=0000c5fb
1b011018 MAX_FRAME_LEN_1: a=000007ef b=00000000 xor=000007ef
1b013004 VLAN_INGRESS_CHECK: a=0000000f b=0000000d xor=00000002
1b00013c PIN_GPIO_SELECT|SW_0x00013c: a=00400042 b=003f0041 xor=007f0003
1b01b008 SW_0x01b008: a=00000000 b=00000001 xor=00000001
1b021a00 SW_0x021a00: a=00000027 b=00000023 xor=00000004
1b028044 SW_0x028040: a=07442504 b=848220b6 xor=83c605b2
1b028148 SW_0x028140: a=00521280 b=02811050 xor=02d302d0
1b02a070 SW_0x02a06c|SW_0x02a070: a=27000546 b=27000032 xor=00000574
1b0360f4 EPON_LOCAL_CLOCK: a=23e7ca37 b=0268c019 xor=218f0a2e

10 addresses differ
         1  PHY
EOF

out=$(python3 tools/regdump/mkparity.py "$T/diff.txt")

# PHY_ACCESS_DATA survives (not in any excluded family); offset translated
# from the full physical address 0x1b000000 down to the bare MMIO offset
# 0x00000000, value is the "a=" (v6) column, mask is always ffffffff.
echo "$out" | grep -qx "00000000 00003a00 ffffffff PHY_ACCESS_DATA" \
	|| { echo "PHY_ACCESS_DATA data line wrong: $out"; exit 1; }
echo "$out" | grep -qx "# PHY_ACCESS_DATA" \
	|| { echo "PHY_ACCESS_DATA comment line missing: $out"; exit 1; }

# MAX_FRAME_LEN_1: offset 0x1b011018 -> 0x00011018, value 0x7ef (the
# working image's own value, not s7's 0).
echo "$out" | grep -qx "00011018 000007ef ffffffff MAX_FRAME_LEN_1" \
	|| { echo "MAX_FRAME_LEN_1 data line wrong: $out"; exit 1; }

# VLAN_INGRESS_CHECK survives too (not excluded).
echo "$out" | grep -qx "00013004 0000000f ffffffff VLAN_INGRESS_CHECK" \
	|| { echo "VLAN_INGRESS_CHECK data line wrong: $out"; exit 1; }

# The thermal word at 0x00013c (even under its PIN_GPIO_SELECT collision
# name), SW_0x01b008 (PTP), SW_0x021a00 (SerDes window), SW_0x028040 and
# SW_0x028140 (snapshot latches), 0x02a070 (via its collision name) and
# EPON_LOCAL_CLOCK are all excluded, by address -- none of their names
# appear anywhere in the output.
for name in SW_0x00013c SW_0x01b008 SW_0x021a00 SW_0x028040 SW_0x028140 SW_0x02a070 EPON_LOCAL_CLOCK; do
	echo "$out" | grep -q "$name" && { echo "$name should have been excluded: $out"; exit 1; }
done

# Exactly 3 surviving entries (PHY_ACCESS_DATA, MAX_FRAME_LEN_1, VLAN_INGRESS_CHECK),
# 6 comment lines skipped from the count, the header line says 3 kept / 7
# excluded, and the diff's own order is preserved (PHY_ACCESS_DATA before
# MAX_FRAME_LEN_1 before VLAN_INGRESS_CHECK -- the fixture lists them in
# that order).
data_lines=$(echo "$out" | grep -vc "^#")
[ "$data_lines" -eq 3 ] || { echo "expected 3 data lines, got $data_lines: $out"; exit 1; }
echo "$out" | grep -q "^# 3 entries (7 excluded" \
	|| { echo "entry-count header wrong: $out"; exit 1; }
order=$(echo "$out" | grep -v "^#" | cut -d' ' -f1)
expected="00000000
00011018
00013004"
[ "$order" = "$expected" ] || { echo "diff order not preserved: got [$order]"; exit 1; }

# The header/table/summary lines in the fixture (none of them diff.py's
# own "<addr> <name>: a=.. b=.. xor=.." shape) are silently ignored, not
# mistaken for a 4th entry.
[ "$data_lines" -eq 3 ]

# --- out-file mode, and the stderr summary line ---------------------------
python3 tools/regdump/mkparity.py "$T/diff.txt" "$T/out.table" 2> "$T/stderr.txt"
[ -s "$T/out.table" ] || { echo "mkparity.py did not write out.table"; exit 1; }
grep -q "3 entries written to $T/out.table (7 excluded, 10 total)" "$T/stderr.txt" \
	|| { echo "stderr summary wrong: $(cat "$T/stderr.txt")"; exit 1; }

# --- a file with no diff.py-shaped lines at all is refused, not silently
# turned into an empty table --------------------------------------------
cat > "$T/empty.txt" <<'EOF'
nothing useful here
EOF
rc=0
python3 tools/regdump/mkparity.py "$T/empty.txt" > /dev/null 2>"$T/empty.err" || rc=$?
[ "$rc" -ne 0 ] || { echo "mkparity.py should refuse a file with no matching lines"; exit 1; }

# --- opportunistic: a real capture, if the developer running this test
# points ODI_REAL_DIFF at one on their own machine -------------------------
REAL_DIFF="${ODI_REAL_DIFF:-}"
if [ -n "$REAL_DIFF" ] && [ -f "$REAL_DIFF" ]; then
	real_out=$(python3 tools/regdump/mkparity.py "$REAL_DIFF")
	real_count=$(echo "$real_out" | grep -vc "^#")
	[ "$real_count" -eq 39 ] || { echo "real diff: expected 39 surviving entries, got $real_count"; exit 1; }
	echo "$real_out" | grep -v "^#" | cut -d' ' -f1 | python3 -c '
import sys
sys.path.insert(0, "tools/regdump")
import mkparity
bad = [o for o in sys.stdin.read().split() if mkparity.excluded(0x1B000000 + int(o, 16))]
sys.exit(1 if bad else 0)' \
		|| { echo "real diff: an excluded address leaked into the table"; exit 1; }
	echo "mkparity_test: real diff sanity ok ($real_count entries)"
else
	echo "mkparity_test: real diff not present at $REAL_DIFF, skipping that check"
fi

echo "mkparity_test: ok"
