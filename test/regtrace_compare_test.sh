#!/bin/sh
# compare.py aligns command brackets between two ring dumps by command
# number and occurrence order, diffs their write sequences, and exits
# non-zero if any bracket differs.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cat > "$T/a.txt" <<'EOF'
# regtrace on=1 rw=0 total=6 dropped=0
1000 M 0x0000000d 0x00000000
1001 W 0x000001d0 0x00000008
1002 W 0x00701014 0x0000ff02
1003 M 0x8000000d 0x00000000
1004 M 0x00000007 0x00000000
1005 W 0x00009999 0x00000001
1006 M 0x80000007 0x00000000
EOF

# Identical dump: every bracket must PASS, exit 0.
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/a.txt")
rc=0
python3 tools/regtrace/compare.py "$T/a.txt" "$T/a.txt" >/dev/null || rc=$?
[ "$rc" -eq 0 ] || { echo "self-compare should exit 0: $out"; exit 1; }
echo "$out" | grep -q "^cmd 13 #1: PASS" || { echo "cmd 13 should PASS: $out"; exit 1; }
echo "$out" | grep -q "^cmd 7 #1: PASS" || { echo "cmd 7 should PASS: $out"; exit 1; }
echo "$out" | grep -q "^summary: 2 brackets, 2 pass, 0 fail" || { echo "summary wrong: $out"; exit 1; }

# A copy with one value changed inside a bracket, and one write removed
# from another bracket, must report both as DIFF and exit non-zero.
cat > "$T/b.txt" <<'EOF'
# regtrace on=1 rw=0 total=5 dropped=0
2000 M 0x0000000d 0x00000000
2001 W 0x000001d0 0x00000009
2002 W 0x00701014 0x0000ff02
2003 M 0x8000000d 0x00000000
2004 M 0x00000007 0x00000000
2005 M 0x80000007 0x00000000
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/b.txt") || rc=$?
[ "$rc" -ne 0 ] || { echo "mutated compare should exit non-zero: $out"; exit 1; }
echo "$out" | grep -q "^cmd 13 #1: DIFF at write 0: expected 0x000001d0 0x00000008 got 0x000001d0 0x00000009" \
	|| { echo "changed value not reported: $out"; exit 1; }
echo "$out" | grep -q "^cmd 7 #1: DIFF at write 0: expected 0x00009999 0x00000001 got -- --" \
	|| { echo "removed write not reported: $out"; exit 1; }
echo "$out" | grep -q "^summary: 2 brackets, 0 pass, 2 fail" || { echo "summary wrong: $out"; exit 1; }

# --addr-only ignores the value difference but still catches the missing
# write.
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/b.txt" --addr-only) || true
echo "$out" | grep -q "^cmd 13 #1: PASS" || { echo "addr-only should pass the value-only diff: $out"; exit 1; }
echo "$out" | grep -q "^cmd 7 #1: DIFF" || { echo "addr-only should still catch the missing write: $out"; exit 1; }

# --ignore drops a listed register from both sides before comparing.
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/b.txt" --ignore 0x1d0) || true
echo "$out" | grep -q "^cmd 13 #1: PASS" || { echo "--ignore should drop the differing register: $out"; exit 1; }

# T/t/D entries (table write/read op + data words) are part of the bracket
# sequence too, alongside W -- a changed data word must be caught exactly
# like a changed register value.
cat > "$T/c.txt" <<'EOF'
# regtrace on=1 rw=0 total=4 dropped=0 tables=1
3000 M 0x00000033 0x00000000
3001 T 0x00100005 0x00000000
3002 D 0x00000010 0xdeadbeef
3003 M 0x80000033 0x00000000
EOF
cat > "$T/d.txt" <<'EOF'
# regtrace on=1 rw=0 total=4 dropped=0 tables=1
4000 M 0x00000033 0x00000000
4001 T 0x00100005 0x00000000
4002 D 0x00000010 0xcafef00d
4003 M 0x80000033 0x00000000
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/c.txt" "$T/c.txt") || rc=$?
[ "$rc" -eq 0 ] || { echo "table bracket self-compare should exit 0: $out"; exit 1; }
echo "$out" | grep -q "^cmd 51 #1: PASS" || { echo "table bracket should PASS against itself: $out"; exit 1; }
rc=0
out=$(python3 tools/regtrace/compare.py "$T/c.txt" "$T/d.txt") || rc=$?
[ "$rc" -ne 0 ] || { echo "changed table data word should fail: $out"; exit 1; }
echo "$out" | grep -q "^cmd 51 #1: DIFF at write 1: expected 0x00000010 0xdeadbeef got 0x00000010 0xcafef00d" \
	|| { echo "changed D word not reported: $out"; exit 1; }

# R (a table run's closing entry) is part of the bracket sequence too,
# compared in its run form -- never expanded. Two dumps whose run closed at
# the same length PASS; a run that closed at a different length (same T/D,
# different R val) is a DIFF like any other changed entry.
cat > "$T/e.txt" <<'EOF'
5000 M 0x00000033 0x00000000
5001 T 0x00100003 0x00000000
5002 D 0x00000010 0xdeadbeef
5003 R 0x00100003 0x00000005
5004 M 0x80000033 0x00000000
EOF
cat > "$T/f.txt" <<'EOF'
6000 M 0x00000033 0x00000000
6001 T 0x00100003 0x00000000
6002 D 0x00000010 0xdeadbeef
6003 R 0x00100003 0x00000003
6004 M 0x80000033 0x00000000
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/e.txt" "$T/e.txt") || rc=$?
[ "$rc" -eq 0 ] || { echo "run bracket self-compare should exit 0: $out"; exit 1; }
echo "$out" | grep -q "^cmd 51 #1: PASS" || { echo "run bracket should PASS against itself: $out"; exit 1; }
rc=0
out=$(python3 tools/regtrace/compare.py "$T/e.txt" "$T/f.txt") || rc=$?
[ "$rc" -ne 0 ] || { echo "a differently-closed run should fail: $out"; exit 1; }
echo "$out" | grep -q "^cmd 51 #1: DIFF at write 2: expected 0x00100003 0x00000005 got 0x00100003 0x00000003" \
	|| { echo "changed run length not reported: $out"; exit 1; }

# Real capture: boot3 against itself must be entirely PASS. This only
# runs if a real capture happens to be on disk at the path below --
# supply your own boot3 register-trace capture there to exercise it.
BOOT3=${BOOT3:-boot3.txt}
if [ -f "$BOOT3" ]; then
	rc=0
	out=$(python3 tools/regtrace/compare.py "$BOOT3" "$BOOT3") || rc=$?
	[ "$rc" -eq 0 ] || { echo "boot3 vs itself should exit 0"; echo "$out" | grep DIFF | head -5; exit 1; }
	echo "$out" | grep -q "0 fail" || { echo "boot3 vs itself should report 0 fail: $(echo "$out" | tail -1)"; exit 1; }
fi

echo "regtrace_compare_test: ok"
