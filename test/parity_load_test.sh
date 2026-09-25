#!/bin/sh
# Exercises rootfs/skeleton/etc/scripts/parity-load.sh's --dry-run mode:
# the table-file parsing (comments and blank lines skipped, malformed
# lines skipped with a warning, not a crash), the parity_add/init_parity
# lines it would write, and the 64-entry cap refusing anything past it.
# Runs entirely against fixtures, no /proc/odi_omci, no kernel, no stick.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
LOADER=rootfs/skeleton/etc/scripts/parity-load.sh

# --- comments, blank lines, and the "name" field are all skipped/ignored,
# only the first 3 fields go into each parity_add line ----------------------
cat > "$T/table.txt" <<'EOF'
# v6 parity load table -- a fixture, not the real one
# MAX_FRAME_LEN_1

00011018 000007ef ffffffff MAX_FRAME_LEN_1

# VLAN_INGRESS_CHECK
00013004 0000000f ffffffff VLAN_INGRESS_CHECK
EOF

out=$(sh "$LOADER" --dry-run "$T/table.txt")
echo "$out" | grep -qx "parity_add 00011018 000007ef ffffffff" \
	|| { echo "first entry line wrong: $out"; exit 1; }
echo "$out" | grep -qx "parity_add 00013004 0000000f ffffffff" \
	|| { echo "second entry line wrong: $out"; exit 1; }
echo "$out" | grep -qx "init_parity" \
	|| { echo "trailing bare init_parity missing: $out"; exit 1; }
n=$(echo "$out" | wc -l)
[ "$n" -eq 3 ] || { echo "expected exactly 3 output lines (2 parity_add + init_parity), got $n: $out"; exit 1; }

# --- a malformed line (missing fields) is skipped, with a warning, not
# fatal to the rest of the file ---------------------------------------------
cat > "$T/malformed.txt" <<'EOF'
00011018 000007ef ffffffff MAX_FRAME_LEN_1
00013004 not_enough_fields
00017014 00000001 ffffffff SW_0x017008
EOF
out=$(sh "$LOADER" --dry-run "$T/malformed.txt" 2>"$T/malformed.err")
n=$(echo "$out" | grep -c "^parity_add ")
[ "$n" -eq 2 ] || { echo "expected 2 accepted parity_add lines, got $n: $out"; exit 1; }
grep -q "malformed line, skipped" "$T/malformed.err" \
	|| { echo "no warning for the malformed line: $(cat "$T/malformed.err")"; exit 1; }

# --- more than 64 entries: the first 64 are loaded, the rest refused with
# a warning, not silently dropped or silently truncated ---------------------
: > "$T/big.txt"
i=0
while [ "$i" -lt 70 ]; do
	printf '%08x 00000001 ffffffff ENTRY%d\n' "$((0x11000 + i * 4))" "$i" >> "$T/big.txt"
	i=$((i + 1))
done
out=$(sh "$LOADER" --dry-run "$T/big.txt" 2>"$T/big.err")
n=$(echo "$out" | grep -c "^parity_add ")
[ "$n" -eq 64 ] || { echo "expected exactly 64 accepted entries (the cap), got $n"; exit 1; }
grep -q "more than 64 entries" "$T/big.err" \
	|| { echo "no over-cap warning: $(cat "$T/big.err")"; exit 1; }
echo "$out" | grep -qx "init_parity" || { echo "trailing bare init_parity missing after the cap: $out"; exit 1; }

# --- a file with nothing but comments/blanks loads nothing, and is not
# treated as an error (rcS should not crumb a failure for an intentionally
# empty table) ---------------------------------------------------------------
cat > "$T/empty.txt" <<'EOF'
# nothing here
EOF
out=$(sh "$LOADER" --dry-run "$T/empty.txt" 2>"$T/empty.err")
[ -z "$out" ] || { echo "empty table should produce no parity_add/init_parity lines: $out"; exit 1; }
grep -q "nothing loaded" "$T/empty.err" || { echo "no nothing-loaded notice: $(cat "$T/empty.err")"; exit 1; }

# --- the real reference table, if it is present in this checkout, loads
# cleanly (39 parity_add lines, one trailing init_parity, no warnings) ------
REAL_TABLE=tools/regdump/parity-v6.table
if [ -f "$REAL_TABLE" ]; then
	out=$(sh "$LOADER" --dry-run "$REAL_TABLE" 2>"$T/real.err")
	n=$(echo "$out" | grep -c "^parity_add ")
	[ "$n" -eq 39 ] || { echo "parity-v6.table: expected 39 parity_add lines, got $n"; exit 1; }
	echo "$out" | grep -qx "init_parity" || { echo "parity-v6.table: trailing bare init_parity missing"; exit 1; }
	grep -qE "malformed|more than|nothing loaded" "$T/real.err" \
		&& { echo "parity-v6.table: unexpected warning: $(cat "$T/real.err")"; exit 1; }
	echo "parity_load_test: real reference table sanity ok ($n entries)"
else
	echo "parity_load_test: $REAL_TABLE not present, skipping that check"
fi

echo "parity_load_test: ok"
