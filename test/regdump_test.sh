#!/bin/sh
# Exercises the mklist.py packing model (narrow single-word, wide
# per-item-word, wide multi-word), the exclusion table, the "|"-joined
# collision handling, and the >MAX_ADDRESSES priority cap; then diff.py
# against two small synthetic dump.sh captures. Runs entirely against
# fixtures written here -- the real register listing is generated from
# the stock binary and not kept in this repository (tools/regtrace/
# README.md),
# so test-host stays true to the promise the Makefile makes that it
# "needs only bash and the repo". A last section runs mklist.py against
# the real map too, opportunistically, when a developer points this test
# at one -- skipped, not failed, otherwise.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cat > "$T/map.txt" <<'EOF'
# test fixture -- not the real map
   0  0x000000  width  32  array 0..0    port 0..0   SW_0x000000
         0  F_31_0                       lsp   0  len  32

   1  0x000004  width   1  array 0..0    port 0..3   SW_0x000004
         0  F_0                          lsp   0  len   1

   2  0x000200  width   1  array 0..40   port 0..0   SW_0x000200
         0  F_0                          lsp   0  len   1

   3  0x000028  width  64  array 0..0    port 0..0   SW_0x000028
         0  F_63_0                       lsp   0  len  64

   4  0x000004  width  32  array 0..0    port 0..0   LINK_COLLIDE
         0  F_31_0                       lsp   0  len  32

   5  0x700000  width  32  array 0..0    port 0..0   GPON_0x700000
         0  F_31_0                       lsp   0  len  32

   6  0xf00000  width  32  array 0..0    port 0..0   PONQ_0xf00000
         0  F_31_0                       lsp   0  len  32

   7  0x000014  width  32  array 0..0    port 0..0   SW_0x000014
         0  F_31_0                       lsp   0  len  32

   8  0x000008  width  32  array 0..0    port 0..0   SW_0x000008
         0  F_16                         lsp  16  len   1

   9  0x000040  width   1  array 0..0    port 0..0   VLAN_SETUP
         0  F_0                          lsp   0  len   1

  10  0x000050  width   1  array 0..0    port 0..0   SW_0x000050
         0  F_0                          lsp   0  len   1
EOF

# --- packing model, exclusions, collisions ---------------------------------
out=$(python3 tools/regdump/mklist.py "$T/map.txt")

# SW_0x000000: width 32 * count 1 <= 32 -> narrow, one word at the base.
echo "$out" | grep -qx "1b000000 SW_0x000000" \
	|| { echo "narrow single-item register wrong: $out"; exit 1; }

# SW_0x000004 (width 1, port 0..3, count 4, 4<=32 -> narrow, ONE word, not
# 4) lands on the same word as LINK_COLLIDE (width 32, count 1) -- an
# overlap the map itself declares. Both names must survive, joined,
# neither silently dropped, in file order.
echo "$out" | grep -qx "1b000004 SW_0x000004|LINK_COLLIDE" \
	|| { echo "narrow-register collision join wrong: $out"; exit 1; }
echo "$out" | grep -qx "1b000008 SW_0x000004+1" \
	&& { echo "narrow register must not get a per-item address: $out"; exit 1; }

# SW_0x000200: width 1, array 0..40, count 41, 41 > 32 -> wide, one word
# PER ITEM (the decode.py model, correct here) -- 41 distinct words.
n=$(echo "$out" | grep -c "SW_0x000200")
[ "$n" -eq 41 ] || { echo "wide narrow-width register should get 41 words, got $n: $out"; exit 1; }
echo "$out" | grep -qx "1b000200 SW_0x000200" || { echo "item 0 missing: $out"; exit 1; }
echo "$out" | grep -qx "1b0002a0 SW_0x000200" || { echo "item 40 (last) missing: $out"; exit 1; }

# SW_0x000028: width 64 > 32, count 1 -> ceil(64/32)=2 consecutive words,
# the same "high word" resolution odi_switch_hw.h used by hand for the
# 64-bit register at 0x01a014.
echo "$out" | grep -qx "1b000028 SW_0x000028" || { echo "64-bit low word missing: $out"; exit 1; }
echo "$out" | grep -qx "1b00002c SW_0x000028" || { echo "64-bit high word missing: $out"; exit 1; }

# GPON block (>= 0x700000) and counter block (>= 0xF00000) are excluded
# whole, regardless of name.
echo "$out" | grep -q "GPON_0x700000" && { echo "GPON block register leaked: $out"; exit 1; }
echo "$out" | grep -q "PONQ_0xf00000" && { echo "counter block register leaked: $out"; exit 1; }

# The listed exclusions go by address even inside the core range: 0x000014
# (a status word) and 0x000008 (a read-data latch) are in mklist.py's
# EXCLUDED_REGISTERS, whatever the listing calls them.
echo "$out" | grep -q "SW_0x000014" && { echo "listed status register leaked: $out"; exit 1; }
echo "$out" | grep -q "SW_0x000008" && { echo "listed read-data latch leaked: $out"; exit 1; }

# Total distinct addresses: 0x0(1) + 0x4(1, collided) + 0x200(41) +
# 0x28(2) + VLAN_SETUP(1) + 0x50(1) = 47.
addr_count=$(echo "$out" | grep -vc "^#")
[ "$addr_count" -eq 47 ] || { echo "expected 47 addresses, got $addr_count: $out"; exit 1; }
echo "$out" | grep -q "^# 47 addresses" || { echo "address-count header wrong: $out"; exit 1; }
echo "$out" | grep -q "^# .*excluded (gpon 1, counter/mib 1, listed 2)" \
	|| { echo "exclusion-count header wrong: $out"; exit 1; }

# --- >MAX_ADDRESSES cap, curated names first -------------------------------
# REGDUMP_MAX_ADDRESSES overrides the 3000 default for exactly this test;
# a real run never sets it. Only VLAN_SETUP and the LINK_COLLIDE collision
# carry a curated (non-default) name among the 47 addresses in this
# fixture, so a cap of 3 must keep those two plus the first remaining
# address in map order (SW_0x000000).
capped=$(REGDUMP_MAX_ADDRESSES=3 python3 tools/regdump/mklist.py "$T/map.txt")
echo "$capped" | grep -q "^# CAPPED at 3 addresses" || { echo "cap header missing: $capped"; exit 1; }
capped_addrs=$(echo "$capped" | grep -vc "^#")
[ "$capped_addrs" -eq 3 ] || { echo "expected exactly 3 addresses under cap, got $capped_addrs: $capped"; exit 1; }
echo "$capped" | grep -q "VLAN_SETUP" || { echo "curated VLAN_SETUP dropped by cap: $capped"; exit 1; }
echo "$capped" | grep -qx "1b000004 SW_0x000004|LINK_COLLIDE" || { echo "curated collision dropped by cap: $capped"; exit 1; }
echo "$capped" | grep -qx "1b000000 SW_0x000000" || { echo "fill-after-priority order wrong: $capped"; exit 1; }
echo "$capped" | grep -q "SW_0x000050" && { echo "cap should have dropped SW_0x000050 before reaching it: $capped"; exit 1; }

python3 tools/regdump/mklist.py "$T/map.txt" "$T/list.txt" >/dev/null
[ -s "$T/list.txt" ] || { echo "mklist.py did not write out.txt"; exit 1; }

# --- diff.py -----------------------------------------------------------
cat > "$T/names.txt" <<'EOF'
# fixture namelist
1b000000 ALPHA_REG
1b000004 BETA_ITEM_EN|BETA_COLLIDE
1b000040 VLAN_SETUP
1b000044 ODD_ONE
EOF

cat > "$T/dump_a.txt" <<'EOF'
1b000000 = 00000001
1b000004 = 00000002
1b000040 = 00000003
1b000044 = 00000004
EOF

cat > "$T/dump_b.txt" <<'EOF'
1b000000 = 000000ff
1b000004 = 00000002
1b000040 = 000000ee
1b000044 = 00000004
EOF

out=$(python3 tools/regdump/diff.py "$T/names.txt" "$T/dump_a.txt" "$T/dump_b.txt")
rc=0
python3 tools/regdump/diff.py "$T/names.txt" "$T/dump_a.txt" "$T/dump_b.txt" >/dev/null || rc=$?
[ "$rc" -eq 0 ] || { echo "diff.py with no missing addresses should exit 0, got $rc"; exit 1; }

echo "$out" | grep -qx "1b000000 ALPHA_REG: a=00000001 b=000000ff xor=000000fe" \
	|| { echo "ALPHA diff line wrong: $out"; exit 1; }
echo "$out" | grep -qx "1b000040 VLAN_SETUP: a=00000003 b=000000ee xor=000000ed" \
	|| { echo "VLAN_SETUP diff line wrong: $out"; exit 1; }
echo "$out" | grep -q "1b000004 " && { echo "unchanged BETA address should not be listed: $out"; exit 1; }
echo "$out" | grep -qx "2 addresses differ" || { echo "diff count wrong: $out"; exit 1; }
echo "$out" | grep -qE "^ +1 +ALPHA" || { echo "ALPHA family count missing: $out"; exit 1; }
echo "$out" | grep -qE "^ +1 +VLAN" || { echo "VLAN family count missing: $out"; exit 1; }

# A capture that stopped early (missing address) is reported, and diff.py
# exits non-zero, instead of silently comparing a partial set.
cat > "$T/dump_c.txt" <<'EOF'
1b000000 = 00000001
1b000004 = 00000002
1b000040 = 00000003
EOF
out=$(python3 tools/regdump/diff.py "$T/names.txt" "$T/dump_a.txt" "$T/dump_c.txt") || true
rc=0
python3 tools/regdump/diff.py "$T/names.txt" "$T/dump_a.txt" "$T/dump_c.txt" >/dev/null || rc=$?
[ "$rc" -eq 1 ] || { echo "diff.py with a missing address should exit 1, got $rc"; exit 1; }
echo "$out" | grep -q "missing from $T/dump_c.txt: 1 addresses" \
	|| { echo "missing-address report wrong: $out"; exit 1; }
echo "$out" | grep -q "1b000044 ODD_ONE" || { echo "missing-address name lookup wrong: $out"; exit 1; }

# --- dump.sh, against a fake memprobe (busybox sh, only sh + the probe) ----
cat > "$T/memprobe" <<'EOF'
#!/bin/sh
# stand-in for tools/memprobe/memprobe reg: same output shape, no /dev/mem.
[ "$1" = reg ] || { echo "unexpected memprobe invocation: $*" >&2; exit 1; }
printf '%s = 000000%02x\n' "$2" "$(( 0x${2#1b} % 256 ))"
EOF
chmod +x "$T/memprobe"
sh tools/regdump/dump.sh "$T/list.txt" "$T/memprobe" > "$T/dump_out.txt"
list_addrs=$(grep -vc "^#" "$T/list.txt")
dump_lines=$(wc -l < "$T/dump_out.txt")
[ "$dump_lines" -eq "$list_addrs" ] || { echo "dump.sh line count mismatch: $dump_lines vs $list_addrs"; exit 1; }
head -1 "$T/dump_out.txt" | grep -qE "^[0-9a-f]{8} = [0-9a-f]{8}$" \
	|| { echo "dump.sh output not in the memprobe addr = val shape: $(head -1 "$T/dump_out.txt")"; exit 1; }

# --- opportunistic: a real map, if the developer running this test
# points ODI_REAL_MAP at one on their own machine --------------------------
REAL_MAP="${ODI_REAL_MAP:-}"
if [ -n "$REAL_MAP" ] && [ -f "$REAL_MAP" ]; then
	real_out=$(python3 tools/regdump/mklist.py "$REAL_MAP")
	real_count=$(echo "$real_out" | grep -vc "^#")
	[ "$real_count" -gt 0 ] || { echo "real map produced zero addresses"; exit 1; }
	[ "$real_count" -le 3000 ] || { echo "real map produced $real_count addresses, over the cap without a CAPPED header"; exit 1; }
	echo "$real_out" | python3 -c '
import sys
bad = 0
for line in sys.stdin:
    if line.startswith("#") or not line.strip():
        continue
    addr = int(line.split()[0], 16)
    if addr >= 0x1B700000:
        print("GPON block leaked:", line.rstrip())
        bad = 1
sys.exit(bad)
' || { echo "real map: GPON block leaked into the list"; exit 1; }
	echo "regdump_test: real map sanity ok ($real_count addresses)"
else
	echo "regdump_test: real map not present at $REAL_MAP, skipping that check"
fi

echo "regdump_test: ok"
