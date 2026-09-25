#!/bin/sh
# sequence.py turns a decoded trace phase into a compact replay script,
# collapsing indirect-table fill loops into one "t" line plus a poll.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# Two table writes (tbl_type 1, entries 0 and 1, method 1, spa 0) around a
# plain register write, raw trace shape (== marker, header, entries).
cat > "$T/trace.txt" <<'EOF'
== 1.00 rtk_init test
# regtrace on=1 rw=0 total=13 dropped=0 entries=13
1000 W 0x00012008 0x00000001
1001 W 0x0001200c 0x00000002
1002 W 0x00012010 0x00000003
1003 W 0x00012014 0x00000004
1004 W 0x00012018 0x00000005
1005 W 0x00012000 0x80000019
1006 W 0x00001234 0x00000005
1007 W 0x00012008 0x00000011
1008 W 0x0001200c 0x00000012
1009 W 0x00012010 0x00000013
1010 W 0x00012014 0x00000014
1011 W 0x00012018 0x00000015
1012 W 0x00012000 0x80000219
== 2.00 rtk_init next
1013 W 0x00009999 0x00000001
EOF

out=$(python3 tools/regtrace/sequence.py "$T/trace.txt" --phase "rtk_init test")

n=$(echo "$out" | grep -c "^t 1 0x000 ")
[ "$n" -eq 1 ] || { echo "expected one t 1 0x000 line, got $n: $out"; exit 1; }
n=$(echo "$out" | grep -c "^t 1 0x001 ")
[ "$n" -eq 1 ] || { echo "expected one t 1 0x001 line, got $n: $out"; exit 1; }
echo "$out" | grep -q "^t 1 0x000 0x00000001 0x00000002 0x00000003 0x00000004 0x00000005 0x80000019$" \
	|| { echo "data words or raw ctrl out of order: $out"; exit 1; }
echo "$out" | grep -q "^t 1 0x001 0x00000011 0x00000012 0x00000013 0x00000014 0x00000015 0x80000219$" \
	|| { echo "second entry data or raw ctrl wrong: $out"; exit 1; }
echo "$out" | grep -q "^w 0x00001234 0x00000005$" || { echo "plain write lost: $out"; exit 1; }
echo "$out" | grep -q "^p " && { echo "no poll lines expected (see sequence.py to_script docstring): $out"; exit 1; }

# The plain write must sit between the two table writes, not before both.
first_t=$(echo "$out" | grep -n "^t 1 0x000" | cut -d: -f1)
w_line=$(echo "$out" | grep -n "^w 0x00001234" | cut -d: -f1)
second_t=$(echo "$out" | grep -n "^t 1 0x001" | cut -d: -f1)
[ "$first_t" -lt "$w_line" ] && [ "$w_line" -lt "$second_t" ] \
	|| { echo "plain write not between the two table writes: $out"; exit 1; }

sum=$(python3 tools/regtrace/sequence.py "$T/trace.txt" --phase "rtk_init test" --summary)
echo "$sum" | grep -q "^table 1: 2 entries 0x000..0x001$" || { echo "summary wrong: $sum"; exit 1; }

# WR_DATA holds its value across CTRL writes on real hardware (isp1
# 260921 baseline, rtk_init vlan): a clear loop writes it once then issues
# many CTRL-only writes. pending must NOT reset after a t line, or those
# entries replay as zero regardless of what was actually recorded.
cat > "$T/clearloop.txt" <<'EOF'
== 1.00 rtk_init clearloop
# regtrace on=1 rw=0 total=7 dropped=0 entries=7
2000 W 0x00012008 0x00000000
2001 W 0x0001200c 0x00000000
2002 W 0x00012010 0x00000000
2003 W 0x00012014 0x00000000
2004 W 0x00012018 0x00000000
2005 W 0x00012000 0x00000019
2006 W 0x00012000 0x00000219
EOF
out2=$(python3 tools/regtrace/sequence.py "$T/clearloop.txt" --phase "rtk_init clearloop")
echo "$out2" | grep -q "^t 1 0x001 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000219$" \
	|| { echo "WR_DATA not held across CTRL writes: $out2"; exit 1; }

echo "regtrace_sequence_test: ok"
