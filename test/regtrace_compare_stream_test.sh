#!/bin/sh
# compare.py --stream ignores brackets/marks entirely and diffs two dumps as
# final-state write maps: one per register address, one per (table, row
# index) with a closed run expanded back into one entry per row it covers.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# Self-compare: identical dumps must report zero diffs and exit 0, even
# with marks and command brackets mixed in (--stream ignores them, not
# rejects them -- a full-stream capture still has marks from the mechanisms
# regtrace_mark_enabled() gates independently of regtrace-all).
cat > "$T/a.txt" <<'EOF'
# regtrace on=1 rw=0 total=9 dropped=0 tables=1 runs=1
1000 M 0x0000000d 0x00000000
1001 W 0x000001d0 0x00000008
1002 T 0x00100005 0x00000000
1003 D 0x00000010 0xdeadbeef
1004 R 0x00100005 0x00000003
1005 M 0x8000000d 0x00000000
1006 W 0x00009999 0x00000001
1007 W 0x000001d0 0x0000000a
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/a.txt" --stream) || rc=$?
[ "$rc" -eq 0 ] || { echo "self-compare should exit 0: $out"; exit 1; }
echo "$out" | grep -q "^registers: 2 compared, 0 differ, 0 only in .*, 0 only in .*$" \
	|| { echo "self-compare registers summary wrong: $out"; exit 1; }
echo "$out" | grep -q "^table rows: 3 compared, 0 differ, 0 only in .*, 0 only in .*$" \
	|| { echo "self-compare table-rows summary wrong: $out"; exit 1; }

# A register written to different final values in the two dumps (last
# write wins in each) is reported once, with both final values -- not once
# per intermediate write.
cat > "$T/b.txt" <<'EOF'
2000 W 0x000001d0 0x00000008
2001 T 0x00100005 0x00000000
2002 D 0x00000010 0xdeadbeef
2003 R 0x00100005 0x00000002
2004 W 0x00009999 0x00000001
2005 W 0x000001d0 0x0000000b
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/b.txt" --stream) || rc=$?
[ "$rc" -ne 0 ] || { echo "differing final register value should exit non-zero: $out"; exit 1; }
echo "$out" | grep -q "^register 0x000001d0: .*a\.txt=0x0000000a .*b\.txt=0x0000000b$" \
	|| { echo "final register-value diff not reported: $out"; exit 1; }
echo "$out" | grep -qv "^register 0x00009999:" \
	|| { echo "identical register 0x9999 should not be reported: $out"; exit 1; }

# A run that closes at a different length between two otherwise-identical
# dumps must surface as one-sided table rows for the indices only the
# longer run covers -- the whole point of expanding runs in this mode,
# unlike bracket mode which compares the R entry unexpanded.
rc=0
out=$(python3 tools/regtrace/compare.py "$T/a.txt" "$T/b.txt" --stream) || rc=$?
echo "$out" | grep -q "^table 16 idx 7: only in .*a\.txt, final 0xdeadbeef$" \
	|| { echo "run-length-only row not reported: $out"; exit 1; }
echo "$out" | grep -q "^table rows: 3 compared, 0 differ, 1 only in .*a\.txt, 0 only in .*b\.txt$" \
	|| { echo "table-rows summary with one-sided row wrong: $out"; exit 1; }

# A register written in only one dump is reported as one-sided with its
# final value, and counted in the summary; same for a table row.
cat > "$T/c.txt" <<'EOF'
3000 W 0x0000aaaa 0x00000005
3001 T 0x00200000 0x00000000
3002 D 0x00000020 0x00000042
EOF
cat > "$T/d.txt" <<'EOF'
4000 W 0x0000bbbb 0x00000006
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/c.txt" "$T/d.txt" --stream) || rc=$?
[ "$rc" -ne 0 ] || { echo "one-sided writes should exit non-zero: $out"; exit 1; }
echo "$out" | grep -q "^register 0x0000aaaa: only in .*c\.txt, final 0x00000005$" \
	|| { echo "register only in c.txt not reported: $out"; exit 1; }
echo "$out" | grep -q "^register 0x0000bbbb: only in .*d\.txt, final 0x00000006$" \
	|| { echo "register only in d.txt not reported: $out"; exit 1; }
echo "$out" | grep -q "^table 32 idx 0: only in .*c\.txt, final 0x00000042$" \
	|| { echo "table row only in c.txt not reported: $out"; exit 1; }
echo "$out" | grep -q "^table rows: 1 compared, 0 differ, 1 only in .*c\.txt, 0 only in .*d\.txt$" \
	|| { echo "table-rows summary for one-sided row wrong: $out"; exit 1; }

# A plain register read (kind R with no table op open, only possible under
# an `on rw` capture) is not a write and must not appear in either map.
cat > "$T/e.txt" <<'EOF'
5000 W 0x0000cccc 0x00000007
5001 R 0x0000dddd 0x00000009
EOF
rc=0
out=$(python3 tools/regtrace/compare.py "$T/e.txt" "$T/e.txt" --stream) || rc=$?
[ "$rc" -eq 0 ] || { echo "plain-read self-compare should exit 0: $out"; exit 1; }
echo "$out" | grep -q "^registers: 1 compared" \
	|| { echo "plain register read should not be counted as a write: $out"; exit 1; }

echo "regtrace_compare_stream_test: ok"
