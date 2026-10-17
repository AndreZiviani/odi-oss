#!/bin/sh
# Table attributes (respond/mibstore.c, tbl_defs[]): rows that a Set adds,
# replaces or deletes one at a time, read back whole through Get and
# Get-Next, and the downstream multicast flag that class 281's address table
# decides (respond/apply_qos.c, ctp_mcast_rows).
#
# Covered: class 309's dynamic ACL (write, replace by key, delete by key,
# clear, the row part masked under OMCC version 0xA0), class 281's IPv4
# address table (add, replace by key, delete by zero addresses) and the
# multicast flag of its CTP's downstream flow following it, class 310's
# service package table, class 49's MAC filter table, and a respawn that
# resumes the rows from the snapshot.
#
# Run from src/omci, inside the toolchain image (make test-omci-tables):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh tables-test.sh
set -u
Q=qemu-mips-static
fail=0
. ./testlib.sh

check() {
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		echo "FAIL  $1"
		echo "--- expected"; printf '%s\n' "$3"
		echo "--- got";      printf '%s\n' "$2"
		fail=$((fail + 1))
	fi
}

# frame <tci> <type> <class> <inst> <contents hex>: one baseline frame, the
# contents padded with zeros to 32 bytes; trailer, zero CRC (not checked).
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}

# answer <log> <tci>: the contents of the last answer to that tci (64 hex).
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}

# send <log> <tci> <type> <class> <inst> <contents>: inject, wait for the answer.
send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# table <log> <tci base> <class> <inst> <mask>: Get the table attribute, then
# walk it with Get-Next; prints the table bytes as hex, empty when it is empty.
# The tci base is three hex digits; the Get uses ...0 and the Next ones ...1-f.
table() {
	_l=$1; _b=$2; _cl=$3; _in=$4; _m=$5
	send "$_l" "${_b}0" 49 "$_cl" "$_in" "$_m"
	_a=$(answer "$_l" "${_b}0")
	_len=$((0x$(echo "$_a" | cut -c7-14)))
	_out=""
	_seq=0
	while [ $((_seq * 29)) -lt "$_len" ]; do
		send "$_l" "${_b}$((_seq + 1))" 5a "$_cl" "$_in" "$_m$(printf '%04x' "$_seq")"
		_n=$((_len - _seq * 29))
		[ "$_n" -gt 29 ] && _n=29
		_out="$_out$(answer "$_l" "${_b}$((_seq + 1))" | cut -c7-$((6 + _n * 2)))"
		_seq=$((_seq + 1))
	done
	printf '%s' "$_out"
}

# pad <hex> <bytes>: the hex padded with zeros to that many bytes.
pad() {
	_p=$1
	while [ "${#_p}" -lt $(($2 * 2)) ]; do _p="${_p}0"; done
	printf '%s' "$_p"
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/tables-gpon

log=/tmp/omcid-tables.log
$Q respond/build/omcid -d -g /tmp/tables-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset

# ------------------------------------------------- class 309, dynamic ACL
# Rows are 24 bytes: the table control word, then 22 bytes. Write key 1,
# write key 2, replace key 1, delete key 2, clear. The control bits (15-14)
# of a stored row read back 00.
R1=$(pad 000100fa0064e0000001e00000ff 24)
R2=$(pad 000200fa0064e0000002e00000ff 24)
R1B=$(pad 000100fa0065e0000003e0000003 24)
w() { printf '%s%s' "$1" "${2#????}"; }      # w <control word> <row>
send "$log" 0010 44 0135 0001 ""
send "$log" 0011 48 0135 0001 "0200$(w 4001 "$R1")"
send "$log" 0012 48 0135 0001 "0200$(w 4002 "$R2")"
got=$(table "$log" b01 0135 0001 0200)
check "309 ACL: two writes are two rows, newest first" "$got" "$R2$R1"
check "309 ACL: the Get reports 48 bytes" \
      "$(answer "$log" b010 | cut -c1-14)" "00020000000030"
send "$log" 0013 48 0135 0001 "0200$(w 4001 "$R1B")"
check "309 ACL: a write of an existing key replaces that row" \
      "$(table "$log" b02 0135 0001 0200)" "$R1B$R2"
send "$log" 0014 48 0135 0001 "02008002"
check "309 ACL: a delete removes the row with that key" \
      "$(table "$log" b03 0135 0001 0200)" "$R1B"
# Row part 1 of key 1: under OMCC version 0x80 (the default) the part bits do
# not count, as in the stock stack, so this replaces key 1.
send "$log" 0015 48 0135 0001 "0200$(w 4801 "$R1")"
check "309 ACL: a row part under OMCC 0x80 is the same row" \
      "$(table "$log" b04 0135 0001 0200)" "$R1"
send "$log" 0016 48 0135 0001 "0200c000"
check "309 ACL: clear empties the table" "$(table "$log" b05 0135 0001 0200)" ""
check "309 ACL: an empty table reports 0 bytes" \
      "$(answer "$log" b050 | cut -c1-14)" "00020000000000"
check "309 static ACL: untouched by the dynamic table's rows" \
      "$(table "$log" b06 0135 0001 0100)" ""

# ----------------------------- class 281, IPv4 table and the multicast flag
# CTP 5 / GEM 4095, downstream only; class 281 instance 1 on it.
send "$log" 0030 44 010c 0005 "0fff00000200000000"
check "a CTP with no multicast address table is sent without the flag" \
      "$(grep -c 'gem 4095 ds flow 0 .* mcast 0 ' "$log")" "1"
send "$log" 0031 44 0119 0001 "0005"
A1=0fff0000e0000000efffffff
A2=0fff0001e8000000e8ffffff
A1B=0fff0000e0000000e00000ff
send "$log" 0032 48 0119 0001 "0080$A1"
check "the first address row sends the flow again, flagged multicast" \
      "$(grep -c 'gem 4095 ds flow 0 .* mcast 1 ' "$log")" "1"
send "$log" 0033 48 0119 0001 "0080$A2"
send "$log" 0034 48 0119 0001 "0080$A1B"
check "281 IPv4: rows keyed by GEM port and secondary key, newest first" \
      "$(table "$log" b07 0119 0001 0080)" "$A1B$A2"
check "a second row does not send the flow again" \
      "$(grep -c 'gem 4095 ds flow 0 ' "$log")" "2"
send "$log" 0035 48 0119 0001 "0080$(pad 0fff0000 12)"
send "$log" 0036 48 0119 0001 "0080$(pad 0fff0001 12)"
check "281 IPv4: zero addresses delete; the table is empty" \
      "$(table "$log" b08 0119 0001 0080)" ""
check "the last row gone, the flow is sent again without the flag" \
      "$(grep -c 'gem 4095 ds flow 0 .* mcast 0 ' "$log")" "2"
send "$log" 0037 48 0119 0001 "0080$A1"
send "$log" 0038 46 0119 0001 ""                       # Delete 281/1
check "deleting the class 281 clears the flag" \
      "$(grep -c 'gem 4095 ds flow 0 .* mcast 0 ' "$log")" "3"

# ------------------------------------------- class 310, service packages
P1=$(pad 000100190002000f4240 20)
P2=$(pad 000200640004000186a0 20)
send "$log" 0050 44 0136 0001 ""
send "$log" 0051 48 0136 0001 "0400$(w 4001 "$P1")"
send "$log" 0052 48 0136 0001 "0400$(w 4002 "$P2")"
check "310 packages: two rows" "$(table "$log" b09 0136 0001 0400)" "$P2$P1"
send "$log" 0053 48 0136 0001 "04008001"
check "310 packages: delete by row key" "$(table "$log" b0a 0136 0001 0400)" "$P2"

# --------------------------------------------- class 49, MAC filter table
# Byte 0 the index, byte 1 bits 7-6: 10 add, 00 remove, 01 clear.
send "$log" 0060 48 0031 0099 "80000180001122334455"
send "$log" 0061 48 0031 0099 "80000280665544332211"
send "$log" 0062 48 0031 0099 "80000180aabbccddeeff"
check "49 MAC filter: rows keyed by index, an add replaces" \
      "$(table "$log" b0b 0031 0099 8000)" "0180aabbccddeeff0280665544332211"
send "$log" 0063 48 0031 0099 "80000200665544332211"
check "49 MAC filter: a remove takes the index out" \
      "$(table "$log" b0c 0031 0099 8000)" "0180aabbccddeeff"
send "$log" 0064 48 0031 0099 "80000040"
check "49 MAC filter: clear" "$(table "$log" b0d 0031 0099 8000)" ""

# ------------------------------------------------------ respawn and resume
# A 309 row and a 281 row survive a respawn through the snapshot.
send "$log" 0070 48 0135 0001 "0200$(w 4002 "$R2")"
send "$log" 0071 44 0119 0002 "0005"
send "$log" 0072 48 0119 0002 "0080$A2"
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
log2=/tmp/omcid-tables2.log
$Q respond/build/omcid -d -s 5 -g /tmp/tables-gpon > "$log2" 2>&1 &
pid=$!
omcid_up "$log2"
check "the respawn resumed from the snapshot" \
      "$(grep -c 'snapshot loaded' "$log2")" "1"
check "309 ACL: the row is back" "$(table "$log2" b0e 0135 0001 0200)" "$R2"
check "281 IPv4: the row is back" "$(table "$log2" b0f 0119 0002 0080)" "$A2"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || { keep_log "$log" tables-test.log; keep_log "$log2" tables-test2.log; }
if [ "$fail" -eq 0 ]; then echo "tables-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
exit "$fail"
