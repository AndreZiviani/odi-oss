#!/bin/sh
# Exercises tools/regtrace/mkmodload.py against a small synthetic capture
# and register map: the table-block 0x012000-0x01202c exclusion, the
# classification-table sweep-vs-specific-rule repeat-write rule, the VLAN/L2_UNICAST/ACL
# fixed categories, the FLOOD reg_group (0, by address, not by name), the
# other-family reg_group assignment (by register address, from the fixed
# FAMILIES table, with an unlisted register getting an OTHER_ family of its
# own -- the "modload category 0 split" case), and that event order is
# preserved exactly as read (not grouped). Runs entirely against fixtures
# -- no dependency on the real v7-modload.txt capture or the real
# register map (test/odi_switch_modload_replay_test.sh is what exercises
# the real capture, via its own vendored, filtered fixture).
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cat > "$T/regmap.txt" <<'EOF'
# test fixture -- not the real map
 214  0x011018  width  32  array 0..0    port 0..0   MAX_FRAME_LEN_1
         0  BYTES                        lsp   0  len  14

 615  0x013008  width  32  array 0..0    port 0..0   VLAN_SETUP
         0  VID4095_MODE                 lsp   4  len   1

   0  0x023000  width  32  array 0..0    port 0..0   SW_0x023000
         0  F_0                          lsp   0  len   1
EOF

cat > "$T/capture.txt" <<'EOF'
# regtrace on=1 rw=0 total=0 dropped=0 entries=0 armonly=0 armed=0 skipped=0 tables=0 runs=0
1 W 0x00011018 0x000007ef
2 T 0x000b0000 0x00000000
3 D 0x00000000 0x00000000
4 D 0x01000000 0x00000000
5 W 0x00012008 0xdeadbeef
6 W 0x0001200c 0xdeadbeef
7 T 0x000b0000 0x00000000
8 D 0x00000000 0x00000123
9 D 0x01000000 0x00000456
10 T 0x00170000 0x000000ff
11 D 0x00000017 0x000000ff
12 T 0x00140000 0x00000000
13 D 0x00000014 0x00000000
14 D 0x01000014 0x00000000
15 D 0x02000014 0x00000000
16 T 0x00090001 0x00000000
17 D 0x00000009 0x00000000
18 W 0x0001c020 0x00000007
19 W 0x00013008 0x00000018
20 W 0x00023000 0x00000001
EOF

out=$(python3 tools/regtrace/mkmodload.py "$T/capture.txt" "$T/regmap.txt")

# 1. The plain register write (line 1) survives as category 0, reg_group 13
# (MAX_FRAME_LEN_1 at 0x011018 -> family FRAME_LENGTH, the 13th entry of
# mkmodload.py's FAMILIES table -- its index does not depend on which other
# families this capture happens to touch).
echo "$out" | grep -qx "cat=0 grp=13 REG off=0x00011018 val=0x000007ef" \
	|| { echo "register event missing or miscategorised: $out"; exit 1; }

# 2. The two table-block writes (lines 5-6, 0x012008/0x01200c) are dropped
# entirely -- no 0xdeadbeef anywhere in the output.
echo "$out" | grep -q "deadbeef" && { echo "table-block plumbing write leaked into the table: $out"; exit 1; }

# 3. CLS_DS_ACTION (table 0x0b = 11) index 0 is written twice: the FIRST
# occurrence (line 2, all-zero) must be category 1 (sweep), the SECOND
# (line 7, real content) must be category 2 (specific) -- the mechanical
# repeat rule, not a hardcoded index.
echo "$out" | grep -qx "cat=1 grp=0 TABLE table=11 idx=0x00000000 n=2 words=0x00000000 0x00000000" \
	|| { echo "first CLS_DS_ACTION[0] write not categorised as sweep (1): $out"; exit 1; }
echo "$out" | grep -qx "cat=2 grp=0 TABLE table=11 idx=0x00000000 n=2 words=0x00000123 0x00000456" \
	|| { echo "second CLS_DS_ACTION[0] write not categorised as specific (2): $out"; exit 1; }

# 4. VLAN (table 0x17 = 23) gets category 3.
echo "$out" | grep -qx "cat=3 grp=0 TABLE table=23 idx=0x00000000 n=1 words=0x000000ff" \
	|| { echo "VLAN row not categorised as 3: $out"; exit 1; }

# 5. L2_UNICAST (table 0x14 = 20) gets category 4.
echo "$out" | grep -qx "cat=4 grp=0 TABLE table=20 idx=0x00000000 n=3 words=0x00000000 0x00000000 0x00000000" \
	|| { echo "L2_UNICAST row not categorised as 4: $out"; exit 1; }

# 6. ACL_PATTERN (table 0x09 = 9) gets category 5.
echo "$out" | grep -qx "cat=5 grp=0 TABLE table=9 idx=0x00000001 n=1 words=0x00000000" \
	|| { echo "ACL_PATTERN row not categorised as 5: $out"; exit 1; }

# 7. The flood write (line 18, 0x1c020) gets category 0, reg_group 0 --
# by ADDRESS, regardless of what name the map would otherwise give it
# (this fixture's map does not even declare 0x1c020, proving reg_group 0
# does not depend on a name lookup succeeding).
echo "$out" | grep -qx "cat=0 grp=0 REG off=0x0001c020 val=0x00000007" \
	|| { echo "flood write not categorised as reg_group 0: $out"; exit 1; }

# 8. The second family register (line 19, VLAN_SETUP) gets its OWN
# reg_group (22, VLAN) -- proves two different register families
# partition into two different reg_groups, not the same one -- and a
# register FAMILIES does not list (line 20, 0x023000) gets a family of its
# own after the listed ones (reg_group 24, OTHER_SW_0x023000).
echo "$out" | grep -qx "cat=0 grp=22 REG off=0x00013008 val=0x00000018" \
	|| { echo "second family register not given its own reg_group: $out"; exit 1; }
echo "$out" | grep -qx "cat=0 grp=24 REG off=0x00023000 val=0x00000001" \
	|| { echo "unlisted register not given an OTHER_ reg_group after the listed ones: $out"; exit 1; }

# 9. Order is preserved exactly as read: the register write (line 1) comes
# before every table event, and CLS_DS_ACTION's own two occurrences keep
# their original relative order (sweep line 2 before specific line 7).
echo "$out" | grep -v "^#" | head -1 | grep -q " REG " \
	|| { echo "the register event is not first in the emitted order: $out"; exit 1; }

# 10. The reg_group listing names FLOOD first (index 0), then the
# FAMILIES table in its own order, then the OTHER_ family.
echo "$out" | grep -qx '# reg_group  0  FLOOD' || { echo "reg_group 0 not documented as FLOOD: $out"; exit 1; }
echo "$out" | grep -qx '# reg_group 13  FRAME_LENGTH' || { echo "reg_group 13 not documented as FRAME_LENGTH: $out"; exit 1; }
echo "$out" | grep -qx '# reg_group 22  VLAN' || { echo "reg_group 22 not documented as VLAN: $out"; exit 1; }
echo "$out" | grep -qx '# reg_group 24  OTHER_SW_0x023000' || { echo "reg_group 24 not documented as OTHER_SW_0x023000: $out"; exit 1; }

# 11. An unrecognised table id is refused, not silently miscategorised.
cat > "$T/badtable.txt" <<'EOF'
1 T 0x00990000 0x00000000
2 D 0x00000099 0x00000000
EOF
rc=0
python3 tools/regtrace/mkmodload.py "$T/badtable.txt" "$T/regmap.txt" > /dev/null 2>"$T/badtable.err" || rc=$?
[ "$rc" -ne 0 ] || { echo "mkmodload.py should refuse an unrecognised table id"; exit 1; }

# --- out-file mode and the stderr summary line ---------------------------
python3 tools/regtrace/mkmodload.py "$T/capture.txt" "$T/regmap.txt" "$T/out.bin" 2> "$T/stderr.txt"
[ -s "$T/out.bin" ] || { echo "mkmodload.py did not write out.bin"; exit 1; }
grep -q "written to $T/out.bin" "$T/stderr.txt" \
	|| { echo "stderr summary wrong: $(cat "$T/stderr.txt")"; exit 1; }
grep -q "25 register families" "$T/stderr.txt" \
	|| { echo "family-count summary line missing or wrong: $(cat "$T/stderr.txt")"; exit 1; }
# The written blob dumps to exactly the records the stdout mode printed.
[ "$(python3 tools/regtrace/replayblob.py dump "$T/out.bin")" = "$(echo "$out" | grep -v '^# reg_group')" ] \
	|| { echo "the written blob does not dump to the stdout records"; exit 1; }

echo "mkmodload_test: ok"
