#!/bin/sh
# Exercises tools/regtrace/mksdkinit.py against a small synthetic capture:
# the per-verb window split (`== ... rtk_init <verb>` / `== ... pon step
# <verb>` markers), the TBL_ACCESS 0x012000-0x01202c exclusion, a plain
# single-row table write, and -- the one shape neither mkmodload.py nor
# mkgponinit.py ever needed -- a table run whose closing R arrives AFTER
# one or more plain W lines that neither extend nor close it, expanded at
# the R own position with the run own opening row emitted immediately
# (this file own docstring has the full reasoning; the real capture
# `vlan` verb is exactly this shape). Also checks that a verb present with
# zero events gets no records, that records come out grouped by verb in
# the fixed verb order, that two runs of the generator against the same
# input produce byte-identical output, and that the blob written to a file
# dumps (tools/regtrace/replayblob.py) to exactly the text printed without
# one.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cat > "$T/capture.txt" <<'EOF'
== 1.00 rtk_init intr
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
1 W 0x00099999 0x00000001
== 1.01 rtk_init switch
# regtrace on=1 rw=0 total=3 dropped=0 entries=3 armonly=0 armed=0 skipped=0 tables=0 runs=0
2 W 0x00011018 0x000007ef
3 W 0x00012008 0xdeadbeef
4 W 0x0001200c 0xdeadbeef
== 1.02 rtk_init svlan
# regtrace on=1 rw=0 total=0 dropped=0 entries=0 armonly=0 armed=0 skipped=0 tables=0 runs=0
== 1.03 rtk_init stp
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
5 T 0x00170005 0x00000000
6 D 0x00000017 0x00000ff0
== 1.04 rtk_init oam
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
7 T 0x00170000 0x00000000
8 D 0x00000017 0x000000f0
9 W 0x00013004 0x00000001
10 W 0x0002a000 0x00000000
11 R 0x00170000 0x00000004
12 T 0x00170004 0x00000000
13 D 0x00000017 0x000003f8
== 1.05 rtk_init acl
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
15 w 0xb800063c 0x00000030
16 r 0xb800063c 0x00000030
17 M 0x00000001 0x00000000
18 W 0x00099998 0x00000009
== 1.06 pon step omcimods
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
14 W 0x00088888 0x00000002
EOF

out=$(python3 tools/regtrace/mksdkinit.py "$T/capture.txt")

# Without an output path the generator prints the blob it would write as
# text (tools/regtrace/replayblob.py dump format): one header comment, then
# one line per record, `<verb> REG|SOC off=.. val=..` or `<verb> TABLE
# table=.. idx=.. n=.. words=..`.

# 1. intr is never generated, even though its own marker and a write exist
# right before the `switch` own window starts -- confirms window-splitting
# does not leak the intr write into switch (0x00099999 is intr own write).
echo "$out" | grep -q "^intr " && { echo "intr must never be generated: $out"; exit 1; }
echo "$out" | grep -q "0x00099999" && { echo "intr write leaked into another verb: $out"; exit 1; }

# 2. switch gets its own register events, TBL_ACCESS (0x012008/0x01200c)
# dropped, MAX_FRAME_LEN_1-equivalent write kept.
echo "$out" | grep -qx "switch REG off=0x00011018 val=0x000007ef" \
	|| { echo "switch register event missing: $out"; exit 1; }
echo "$out" | grep -q "deadbeef" && { echo "TBL_ACCESS plumbing write leaked into switch: $out"; exit 1; }

# 3. svlan marker exists but its own span has zero entries -- no records.
echo "$out" | grep -q "^svlan " && { echo "svlan has zero events, no record should exist: $out"; exit 1; }

# 4. stp: one plain table row, no run (a length-1 "run" never gets an R).
echo "$out" | grep -qx "stp TABLE table=23 idx=0x00000005 n=1 words=0x00000ff0" \
	|| { echo "stp table row missing: $out"; exit 1; }

# 5. oam: the run-across-W shape. Opening row (idx 0, 0xf0) emitted FIRST,
# then the two W lines (unaffected by the run), then the run own remaining
# rows (idx 1..3, run_len 4 from the R) expanded in order, THEN the
# differing row at idx 4 (a fresh table op, not a run continuation).
oam_block=$(echo "$out" | grep "^oam ")
expected_oam="oam TABLE table=23 idx=0x00000000 n=1 words=0x000000f0
oam REG off=0x00013004 val=0x00000001
oam REG off=0x0002a000 val=0x00000000
oam TABLE table=23 idx=0x00000001 n=1 words=0x000000f0
oam TABLE table=23 idx=0x00000002 n=1 words=0x000000f0
oam TABLE table=23 idx=0x00000003 n=1 words=0x000000f0
oam TABLE table=23 idx=0x00000004 n=1 words=0x000003f8"
[ "$oam_block" = "$expected_oam" ] \
	|| { echo "oam: expected the opening row, 2 W, 3 run continuations and the differing row, in that order, got: $oam_block"; exit 1; }

# 5b. acl: a kind-w (SoC write) line becomes a SOC record, IN POSITION,
# carrying the full physical address -- the exact fix for the bug that
# silently dropped the PON-PBO IP-enable write. A kind-r (SoC read) and a
# kind-M (command mark) right after it are both recognised and produce no
# record at all, and the plain W after them still applies normally -- so
# this verb has exactly 2 records, not 4.
acl_block=$(echo "$out" | grep "^acl ")
expected_acl="acl SOC off=0xb800063c val=0x00000030
acl REG off=0x00099998 val=0x00000009"
[ "$acl_block" = "$expected_acl" ] \
	|| { echo "acl: expected the SoC write then the plain W, nothing else, got: $acl_block"; exit 1; }

# 6. omcimods is not one of VERBS_ORDER -- excluded entirely.
echo "$out" | grep -q "0x00088888" && { echo "omcimods must never be generated: $out"; exit 1; }

# 7. Records are grouped by verb in VERBS_ORDER own order (switch, stp,
# oam, acl for this fixture), and the header counts every record.
order=$(echo "$out" | grep -v "^#" | cut -d' ' -f1 | uniq | tr '\n' ' ')
[ "$order" = "switch stp oam acl " ] || { echo "verbs out of order: $order"; exit 1; }
echo "$out" | head -1 | grep -q "^# odi replay blob: table=sdkinit version=1 records=11 " \
	|| { echo "header line wrong: $(echo "$out" | head -1)"; exit 1; }

# --- determinism: two runs against the same input are byte-identical ----
out2=$(python3 tools/regtrace/mksdkinit.py "$T/capture.txt")
[ "$out" = "$out2" ] || { echo "mksdkinit.py is not deterministic across two runs"; exit 1; }

python3 tools/regtrace/mksdkinit.py "$T/capture.txt" "$T/out_a.bin" 2>/dev/null
python3 tools/regtrace/mksdkinit.py "$T/capture.txt" "$T/out_b.bin" 2>/dev/null
cmp -s "$T/out_a.bin" "$T/out_b.bin" || { echo "mksdkinit.py is not deterministic across two out-file runs"; exit 1; }

# --- out-file mode: the stderr summary line, and the written blob dumps to
# exactly the text the stdout mode printed (and so validates) ------------
python3 tools/regtrace/mksdkinit.py "$T/capture.txt" "$T/out.bin" 2> "$T/stderr.txt"
[ -s "$T/out.bin" ] || { echo "mksdkinit.py did not write out.bin"; exit 1; }
grep -q "25 verbs written to $T/out.bin" "$T/stderr.txt" \
	|| { echo "stderr summary wrong: $(cat "$T/stderr.txt")"; exit 1; }
[ "$(python3 tools/regtrace/replayblob.py dump "$T/out.bin")" = "$out" ] \
	|| { echo "the written blob does not dump to the stdout text"; exit 1; }

# --- a table row wider than MAX_WORDS is refused, not silently truncated -
cat > "$T/badrow.txt" <<'EOF'
== 2.00 rtk_init acl
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
1 T 0x00080000 0x00000000
2 D 0x00000000 0x00000000
3 D 0x00000000 0x00000000
4 D 0x00000000 0x00000000
5 D 0x00000000 0x00000000
6 D 0x00000000 0x00000000
7 D 0x00000000 0x00000000
== 2.01 rtk_init qos
# regtrace on=1 rw=0 total=0 dropped=0 entries=0 armonly=0 armed=0 skipped=0 tables=0 runs=0
EOF
rc=0
python3 tools/regtrace/mksdkinit.py "$T/badrow.txt" > /dev/null 2>"$T/badrow.err" || rc=$?
[ "$rc" -ne 0 ] || { echo "mksdkinit.py should refuse a table row wider than MAX_WORDS"; exit 1; }

# --- an unrecognised kind is a fatal error, not a silent skip -- this is
# the exact shape of bug that dropped the PON-PBO IP-enable write (kind w)
# undetected for as long as it did: a kind this generator has never seen
# must stop the generation, not vanish quietly the way the old
# uppercase-only LINE_RE let it.
cat > "$T/badkind.txt" <<'EOF'
== 3.00 rtk_init acl
# regtrace on=1 rw=0 total=1 dropped=0 entries=1 armonly=0 armed=0 skipped=0 tables=0 runs=0
1 z 0x00000001 0x00000002
== 3.01 rtk_init qos
# regtrace on=1 rw=0 total=0 dropped=0 entries=0 armonly=0 armed=0 skipped=0 tables=0 runs=0
EOF
rc=0
python3 tools/regtrace/mksdkinit.py "$T/badkind.txt" > /dev/null 2>"$T/badkind.err" || rc=$?
[ "$rc" -ne 0 ] || { echo "mksdkinit.py should refuse an unrecognised kind (z), not skip it: $(cat "$T/badkind.err")"; exit 1; }
grep -q "kind 'z'" "$T/badkind.err" \
	|| { echo "badkind error message did not name the offending kind: $(cat "$T/badkind.err")"; exit 1; }

echo "mksdkinit_test: ok"
