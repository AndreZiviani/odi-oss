#!/bin/sh
# decode.py joins trace lines to the register map and decodes fields.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/map.txt" <<'EOF'
# sample map
   0  0x000001d0  width  32  array 0..0    port 0..0   PON_SERDES_MODE
         0  RESERVED                     lsp   4  len  28
         1  LINK_MODE                     lsp   0  len   4
   1  0x00701010  width  32  array 0..1    port 0..0   DSF_ONU_STATE
         0  HI_BYTE                    lsp   8  len   8
         1  LO_BYTE                       lsp   0  len   8
EOF
cat > "$T/trace.txt" <<'EOF'
== 4.52 pon step ponmac
# regtrace on=1 rw=0 total=3 dropped=0 entries=3
1000 W 0x000001d0 0x00000008
1001 W 0x00701014 0x0000ff02
1002 W 0x00009999 0x00000001
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/trace.txt")
echo "$out" | grep -q "^== 4.52 pon step ponmac" || { echo "marker lost"; exit 1; }
echo "$out" | grep -q "0x000001d0 0x00000008 PON_SERDES_MODE LINK_MODE=0x8" || { echo "field decode wrong: $out"; exit 1; }
echo "$out" | grep -q "0x00701014 0x0000ff02 DSF_ONU_STATE+1 HI_BYTE=0xff LO_BYTE=0x2" || { echo "array index wrong: $out"; exit 1; }
echo "$out" | grep -q "0x00009999 0x00000001 UNKNOWN" || { echo "unknown not marked: $out"; exit 1; }
sum=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/trace.txt" --summary)
echo "$sum" | grep -q "pon step ponmac: 3 entries, 3 registers, 1 unknown" || { echo "summary wrong: $sum"; exit 1; }

# Command markers (kind M): tag bit 31 is phase, bits 0..30 the command
# number. cmd=7 before is 0x00000007, after is 0x80000007. One write inside
# the pair attributes to cmd 7; one write outside goes to unattributed.
cat > "$T/mark.txt" <<'EOF'
2000 M 0x00000007 0x00000000
2001 W 0x000001d0 0x00000008
2002 M 0x80000007 0x00000000
2003 W 0x00701014 0x0000ff02
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/mark.txt")
echo "$out" | grep -q "^== cmd 7 ==" || { echo "mark before lost: $out"; exit 1; }
echo "$out" | grep -q "^== end cmd 7 ==" || { echo "mark after lost: $out"; exit 1; }
sum=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/mark.txt" --summary)
echo "$sum" | grep -q "^cmd 7: tag before=0x00000007 after=0x80000007, 1 writes, 1 distinct registers, groups \[PON_SERDES_MODE\], 0 unknown" \
	|| { echo "per-command summary wrong: $sum"; exit 1; }
echo "$sum" | grep -q "^unattributed: 1 writes, 0 unknown" || { echo "unattributed summary wrong: $sum"; exit 1; }

# Header line fields (total, dropped, armonly, skipped) are echoed at the
# top of --summary output, so a stick run can be checked without re-reading
# the raw trace.
cat > "$T/header.txt" <<'EOF'
# regtrace on=1 rw=0 total=301689 dropped=285305 entries=16384 armonly=1 armed=0 skipped=16150 tables=41
2000 M 0x0000000d 0x00000000
2001 M 0x8000000d 0x00000000
EOF
sum=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/header.txt" --summary)
echo "$sum" | grep -q "^header: total=301689 dropped=285305 armonly=1 skipped=16150 tables=41" \
	|| { echo "header line wrong: $sum"; exit 1; }

# A command bracket with zero writes inside it (a polling command such as
# cmd 13 that only reads) still gets one line, instead of vanishing.
echo "$sum" | grep -q "^cmd 13: 0 writes" || { echo "zero-write block missing: $sum"; exit 1; }

# --collapse merges consecutive brackets of the same command number with
# the identical write sequence into one line with a repeat count; a
# differing sequence, or a different command number, breaks the run.
cat > "$T/collapse.txt" <<'EOF'
3000 M 0x0000000d 0x00000000
3001 W 0x000001d0 0x00000008
3002 M 0x8000000d 0x00000000
3003 M 0x0000000d 0x00000000
3004 W 0x000001d0 0x00000009
3005 M 0x8000000d 0x00000000
3006 M 0x0000000d 0x00000000
3007 W 0x00701014 0x0000ff02
3008 M 0x8000000d 0x00000000
3009 M 0x00000007 0x00000000
3010 W 0x000001d0 0x00000008
3011 M 0x80000007 0x00000000
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/collapse.txt" --summary --collapse)
echo "$out" | grep -q "^cmd 13: 1 writes (repeated 2x)" || { echo "collapse run wrong: $out"; exit 1; }
echo "$out" | grep -q "^cmd 13: 1 writes$" || { echo "differing sequence should not collapse: $out"; exit 1; }
echo "$out" | grep -q "^cmd 7: 1 writes$" || { echo "different command should not collapse: $out"; exit 1; }
uncollapsed=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/collapse.txt" --summary)
echo "$uncollapsed" | grep -c "^cmd 13: 1 writes$" | grep -q "^3$" \
	|| { echo "without --collapse the run should print separately: $uncollapsed"; exit 1; }

# Kind T/t entries (table writes and reads) carry table id and row
# index packed as table<<16|index; each is followed by one kind-D entry per
# data word, packed as word-index<<24|table. decode.py folds a T/t and its
# D's into one "table <name> idx N: w0 w1 ..." line, names the table from
# decode.py TABLE_NAMES order (16 = CLS_RULE_A), and counts T/t (not D) as
# one "table op" per bracket.
cat > "$T/table.txt" <<'EOF'
5000 M 0x00000033 0x00000000
5001 T 0x00100003 0x00000000
5002 D 0x00000010 0xdeadbeef
5003 D 0x01000010 0xcafef00d
5004 M 0x80000033 0x00000000
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/table.txt")
echo "$out" | grep -q "^table CLS_RULE_A idx 3: 0xdeadbeef 0xcafef00d" \
	|| { echo "table T/D folding wrong: $out"; exit 1; }
sum=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/table.txt" --summary)
echo "$sum" | grep -q "^cmd 51: tag before=0x00000033 after=0x80000033, 0 writes, 0 distinct registers, groups \[\], 0 unknown, 1 table ops" \
	|| { echo "per-command table-op count wrong: $sum"; exit 1; }
echo "$sum" | grep -q "^ *1  table CLS_RULE_A" || { echo "per-command table listing missing: $sum"; exit 1; }
echo "$sum" | grep -q "^cmd 51: 0 writes, 1 table ops" || { echo "blocks-in-order table-op count wrong: $sum"; exit 1; }

# An out-of-range table id falls back to TABLE_<id> instead of crashing.
cat > "$T/table-unknown.txt" <<'EOF'
6000 M 0x00000001 0x00000000
6001 T 0x03e80000 0x00000000
6002 D 0x000003e8 0x00000001
6003 M 0x80000001 0x00000000
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/table-unknown.txt")
echo "$out" | grep -q "^table TABLE_1000 idx 0: 0x00000001" \
	|| { echo "unknown table id fallback wrong: $out"; exit 1; }

# Run-length coding (kernel-side): a table-clearing loop of identical rows
# is encoded as one T/D group plus a trailing kind-R entry (addr =
# table<<16|first_index, val = run length). decode.py folds all of it into
# one "table X idx A..B (run N): words" line, and counts it as 1 table op
# (the T) + 1 run (the R) for the per-command and blocks-in-order sections.
cat > "$T/run.txt" <<'EOF'
7000 M 0x00000033 0x00000000
7001 T 0x00100003 0x00000000
7002 D 0x00000010 0xdeadbeef
7003 D 0x01000010 0xcafef00d
7004 R 0x00100003 0x00000005
7005 M 0x80000033 0x00000000
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/run.txt")
echo "$out" | grep -q "^table CLS_RULE_A idx 3..7 (run 5): 0xdeadbeef 0xcafef00d" \
	|| { echo "run folding wrong: $out"; exit 1; }
sum=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/run.txt" --summary)
echo "$sum" | grep -q "^cmd 51: tag before=0x00000033 after=0x80000033, 0 writes, 0 distinct registers, groups \[\], 0 unknown, 1 table ops, 1 runs" \
	|| { echo "per-command run count wrong: $sum"; exit 1; }
echo "$sum" | grep -q "^cmd 51: 0 writes, 1 table ops, 1 runs" || { echo "blocks-in-order run count wrong: $sum"; exit 1; }

# Kind R is dual-purpose (see decode.py's docstring): a table run's closing
# entry only when it immediately follows a T/t's D's (pending set). A
# standalone R with nothing pending -- the ordinary shape of a genuine
# register read in an `on rw` capture -- must still decode as a normal
# register, not vanish into a run or an ORPHAN-R line.
cat > "$T/plain-r.txt" <<'EOF'
8000 R 0x000001d0 0x00000008
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/plain-r.txt")
echo "$out" | grep -q "0x000001d0 0x00000008 PON_SERDES_MODE LINK_MODE=0x8" \
	|| { echo "standalone R misparsed as a run: $out"; exit 1; }
echo "$out" | grep -q "ORPHAN-R" && { echo "standalone R wrongly flagged orphan: $out"; exit 1; }

# The regtrace.act captures of an earlier rcS concatenate their dumps, one
# "# regtrace-act iter=N uptime=U" marker line per round ahead of each
# round's ring contents -- the "== phase" + dump shape of /tmp/regtrace.txt,
# with a "#" marker instead of "==".
# decode.py's main loop checks `line.startswith("#")` before ever trying to
# split a line into us/kind/addr/val, so a marker line must pass straight
# through unparsed rather than being rejected or crashing the whole decode.
cat > "$T/act.txt" <<'EOF'
== 60.10 pon step gponact
# regtrace-act iter=1 uptime=61.30
9000 W 0x000001d0 0x00000008
# regtrace-act iter=2 uptime=63.31
9001 W 0x000001d0 0x00000009
EOF
out=$(python3 tools/regtrace/decode.py "$T/map.txt" "$T/act.txt")
echo "$out" | grep -q "^# regtrace-act iter=1 uptime=61.30" \
	|| { echo "regtrace-act marker (round 1) lost: $out"; exit 1; }
echo "$out" | grep -q "^# regtrace-act iter=2 uptime=63.31" \
	|| { echo "regtrace-act marker (round 2) lost: $out"; exit 1; }
echo "$out" | grep -q "0x000001d0 0x00000008 PON_SERDES_MODE LINK_MODE=0x8" \
	|| { echo "regtrace-act: entry before a marker line misdecoded: $out"; exit 1; }
echo "$out" | grep -q "0x000001d0 0x00000009 PON_SERDES_MODE LINK_MODE=0x9" \
	|| { echo "regtrace-act: entry after a marker line misdecoded: $out"; exit 1; }

echo "regtrace_decode_test: ok"
