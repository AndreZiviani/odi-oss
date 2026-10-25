#!/bin/sh
# The multicast policy file (respond/mcast.c): what the OLT provisioned in
# classes 281, 309 and 310, as igmpd reads it from /var/run/odi-mcast.conf,
# and the class 309 attributes that did not fit the stored row (the profile
# is 90 bytes, the row was 64).
#
# Covered: no profile, no snooping; a full session (profile, subscriber, a
# multicast GEM interworking TP with an address row) giving snooping with
# the profile's timers, the subscriber's group limit and both access control
# lists; the units (tenths of a second to seconds, defaults for zero); a
# function other than snooping turning it off; a delete of the address row
# turning it off; a MIB reset; the file rewritten only when it changed; the
# access control list rows of OMCC 0xB0 (row parts 1 and 2, IPv6 ranges left
# out, an IPv6-only list enforcing nothing); the width of attribute 16 of
# class 309 (3 bytes, G.988); the service package table said not to select a
# profile.
#
# Run from src/omci, inside the toolchain image (make test-omci-mcast):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh mcast-test.sh
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

answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}

send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# policy: the file as written, without the comment line.
policy() {
	grep -v '^#' /var/run/odi-mcast.conf 2>/dev/null
}

# written <log> <n>: the policy has been written n times, which is the end of
# a session as far as the file is concerned (a quiet second after the last
# frame).
written() {
	need "$1" 'multicast policy written' 10 "$2"
}

# table <log> <tci base> <class> <inst> <mask>: Get the table attribute, then
# walk it with Get-Next; the table bytes as hex, empty when it is empty.
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

pad() {
	_p=$1
	while [ "${#_p}" -lt $(($2 * 2)) ]; do _p="${_p}0"; done
	printf '%s' "$_p"
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision \
      /var/run/odi-mcast.conf
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/mcast-gpon

log=/tmp/omcid-mcast.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/mcast-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
written "$log" 1
check "an empty MIB: the file exists and says no snooping" "$(policy)" "snooping=0"
# An OLT that reports OMCC 0xB0 (176) sends ACL rows in parts: the part bits
# of the table control only count from 0xA1 on (mibstore.c).
: > /var/config/omci-identity.on
$Q cli/build/omcli ident test/cfg_omcc_b0.xml test/cfg_report_empty_hs.xml > /dev/null 2>&1
send "$log" 0001 4f 0002 0000 ""                         # MIB reset

# ---------------------------------------------------------- a full session
# Class 309 (0x0135) instance 1, created with the attributes a Create carries
# (1-6, 10-13 and 16): IGMP version 2, function 0, immediate leave on, the
# timers unset. Class 310 (0x0136) instance 1 names it, a limit of 8 groups.
# CTP 5 on GEM 4095, downstream only, and class 281 (0x0119) on it with one
# address row.
# A decoy first: a subscriber 310 (instance 2, no bridge port) that names a
# profile which does not exist and a limit of 99. The subscriber of the UNI is
# the 310 whose instance is a bridge port of the PPTP Ethernet UNI (class 47,
# instance 1, TP type 1), created after it, so taking the first 310 in the MIB
# would pick the wrong one.
send "$log" 0008 44 0136 0002 "00000200630000000000"
send "$log" 0009 44 002f 0001 "00010101010100000001000101"
send "$log" 0010 44 0135 0001 "020001"
send "$log" 0011 44 0136 0001 "00000100080000000000"
send "$log" 0012 44 010c 0005 "0fff00000200000000"
send "$log" 0013 44 0119 0001 "0005"
send "$log" 0014 48 0119 0001 "00800fff0000e0000000efffffff"
written "$log" 2
check "profile, subscriber, address row: snooping with the defaults" "$(policy)" \
"snooping=1
igmp_function=0
immediate_leave=1
robustness=2
query_interval=125
query_max_response=10
last_member_query=1
unauthorized_join=0
max_groups=8"

# Attributes 10 to 15 in one Set: robustness 3, querier 0, query interval
# 60 s, max response 50 tenths, last member interval 20 tenths, unauthorized
# joins 1. They lie past byte 64 of the profile.
send "$log" 0020 48 0135 0001 "007e03000000000000003c000000320000001401"
send "$log" 0021 49 0135 0001 "0010"
check "a Get of the query interval, which did not fit the old row" \
      "$(answer "$log" 0021 | cut -c7-14)" "0000003c"

# Attribute 16, the downstream IGMP and multicast TCI, is 3 bytes (control
# type, TCI): G.988 9.3.27. With attributes 6, 10-15 the Get is 25 attribute
# bytes, which is all a baseline Get carries; one byte more and the response
# would be result 9 (attributes do not fit), so the width shows in the result,
# and the three bytes read back last.
send "$log" 0022 48 0135 0001 "000102abcd"
send "$log" 0023 49 0135 0001 "047f"
check "309 attribute 16: a Get with it as the 25th byte still fits" \
      "$(answer "$log" 0023 | cut -c1-2)" "00"
check "309 attribute 16 reads back the three bytes set" \
      "$(answer "$log" 0023 | cut -c51-56)" "02abcd"

# ACL rows, 24 bytes: control, GEM port, VLAN, source, start, end, bandwidth,
# reserved. Dynamic: 225.0.0.0-225.255.255.255. Static: one address, end 0.
DYN=40010fff000b00000000e1000000e1ffffff000000000000
STA=40010fff000b00000000e201020300000000000000000000
send "$log" 0030 48 0135 0001 "0200$DYN"
send "$log" 0031 48 0135 0001 "0100$STA"
written "$log" 3
check "timers and both access control lists" "$(policy)" \
"snooping=1
igmp_function=0
immediate_leave=1
robustness=3
query_interval=60
query_max_response=5
last_member_query=2
unauthorized_join=1
max_groups=8
allow=225.0.0.0-225.255.255.255
static=226.1.2.3-226.1.2.3"

# The contract with igmpd: the file just written, read by igmpd itself
# (igmpd -r replays a transcript through the daemon's own decision code, with
# the policy file this suite produced). The timers, the limit, both lists and
# the immediate leave arrive as omcid wrote them, so a change of the format on
# either side fails here and not on a stick. Build it first: make
# test-omci-build.
IGMPD=../igmp/build/igmpd
cp /var/run/odi-mcast.conf /tmp/mcast-policy-snap
cat > /tmp/mcast-replay.in <<'EOF2'
0 policy-file /tmp/mcast-policy-snap
0 tick
1 v2join 1 0 192.168.1.10 225.2.2.2
2 v2join 1 0 192.168.1.10 225.3.3.3
3 v2join 1 0 192.168.1.10 239.1.1.1
4 v2leave 1 0 192.168.1.10 225.3.3.3
5 state
EOF2
if [ -x "$IGMPD" ]; then
	$Q "$IGMPD" -r /tmp/mcast-replay.in > /tmp/mcast-replay.out 2>&1
	rep() { grep -v '^>' /tmp/mcast-replay.out; }
	check "igmpd reads the file: timers, limit, lists, immediate leave" \
	      "$(rep | grep '^policy:')" \
	      "policy: snooping, function 0, immediate leave, 3/60/2 s, limit 8, 1 allow + 1 static ranges"
	check "igmpd holds the static group as a row, the UNI and the PON members" \
	      "$(rep | grep -c 'set vid 0 01:00:5e:01:02:03 ports 0x5')" "1"
	check "igmpd programs a group the dynamic list allows" \
	      "$(rep | grep -c 'set vid 0 01:00:5e:02:02:02 ports 0x5')" "1"
	check "igmpd refuses a group outside both lists: no row" \
	      "$(rep | grep -c '01:00:5e:01:01:01 ports')" "0"
	check "igmpd honours the immediate leave: the row goes on the leave" \
	      "$(rep | grep -c 'del vid 0 01:00:5e:03:03:03')" "1"
	# What igmpd tells omcid back: its state file, as it formats it, for the
	# ME 311 checks further down.
	sed '1,/^> end$/d' /tmp/mcast-replay.out > /tmp/mcast-state-from-igmpd
else
	check "igmpd is built for the contract test (make test-omci-build)" "missing" "built"
	: > /tmp/mcast-state-from-igmpd
fi

# A Set that changes nothing the file holds writes nothing.
n=$(grep -c 'multicast policy written' "$log")
send "$log" 0040 48 0135 0001 "004003"
sync_send "$log"
sleep 1.5
check "a Set of the policy's own value leaves the file alone" \
      "$(grep -c 'multicast policy written' "$log")" "$n"

# ----------------------------------------------------- what turns it off
send "$log" 0050 48 0135 0001 "400001"
written "$log" $((n + 1))
check "IGMP function 1 (snooping with proxy reporting): not snooping" \
      "$(policy | sed -n 1,2p)" "snooping=0
igmp_function=1"
send "$log" 0051 48 0135 0001 "400000"
written "$log" $((n + 2))
check "function 0 again: snooping" "$(policy | sed -n 1p)" "snooping=1"
send "$log" 0052 48 0119 0001 "00800fff0000"
written "$log" $((n + 3))
check "the address row deleted: no multicast GEM port carries groups" \
      "$(policy | sed -n 1p)" "snooping=0"

# More ranges than fit: the file says so, and lists none past the cap. Row
# keys are the low bits of the control word, so each is a row of its own.
send "$log" 0060 48 0119 0001 "00800fff0000e0000000efffffff"
i=1
while [ "$i" -le 34 ]; do
	row=$(printf '%04x0fff000b00000000e1%06x' $((0x4000 + i)) "$i")
	send "$log" "01$(printf '%02x' "$i")" 48 0135 0001 "0200${row}e1ffffff000000000000"
	i=$((i + 1))
done
written "$log" $((n + 4))
check "34 ranges: the first 32 are listed" "$(policy | grep -c '^allow=')" "32"
check "34 ranges: the overflow is flagged" "$(policy | grep -c '^allow_overflow=1')" "1"

# --------------------------- row parts, IPv6 rows, the ACL of OMCC 0xB0
# The table is cleared (control 11), then logical rows are written as the
# parts G.988 9.3.27 defines: part 0 (0x4000 | key) carries the IPv4 ranges,
# part 1 (0x4800 | key) the leading 12 bytes of an IPv6 source and the preview
# fields, part 2 (0x5000 | key) the leading 12 bytes of an IPv6 destination.
# A prefix of ten zero bytes and ffff is the IPv4 form; any other is IPv6.
# Key 1: IPv4, with a part 1 in the IPv4 form and preview fields. Key 2: a
# part 2 with an IPv6 group. Key 3: a part 1 with an IPv6 source. Only key 1
# is a range igmpd can enforce; the part rows are not ranges of their own.
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0200 48 0135 0001 "0200c000"
K1P0=$(pad 40010fff000b00000000e1010000e10100ff 24)
K1P1=$(pad 480100000000000000000000ffff00ff000a 24)
K2P0=$(pad 40020fff000b00000000e2000000e20000ff 24)
K2P2=$(pad 5002ff3e00000000000000001234 24)
K3P0=$(pad 40030fff000b00000000e3000000e30000ff 24)
K3P1=$(pad 480320010db8000000000000abcd00ff000a 24)
t=0201
for row in $K1P0 $K1P1 $K2P0 $K2P2 $K3P0 $K3P1; do
	send "$log" "$t" 48 0135 0001 "0200$row"
	t=$(printf '%04x' $((0x$t + 1)))
done
written "$log" $((m + 1))
check "ACL parts: the table holds all six rows" \
      "$(table "$log" b20 0135 0001 0200 | wc -c)" "288"
check "ACL parts: only the IPv4 row is a range; part rows and IPv6 rows are not" \
      "$(policy | grep '^allow')" "allow=225.1.0.0-225.1.0.255"

# An OLT that provisions only IPv6 rows: nothing to enforce, so no ACL at
# all, not an IPv4 list that denies every join.
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0210 48 0135 0001 "0200c000"
K5P0=$(pad 40050fff000b00000000e5000000e50000ff 24)
K5P2=$(pad 5005ff3e00000000000000005678 24)
send "$log" 0211 48 0135 0001 "0200$K5P0"
send "$log" 0212 48 0135 0001 "0200$K5P2"
written "$log" $((m + 1))
check "an IPv6-only ACL: no IPv4 range is listed" "$(policy | grep -c '^allow')" "0"

# ------------------------------------- ME 310 service package table
# Said once when the OLT provisions it, not on every write; the profile is
# still the pointer's (the policy keeps its timers).
check "no service package table: nothing said" \
      "$(grep -c 'service package table' "$log")" "0"
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0220 48 0136 0001 "0400$(pad 4001ffff 20)"
send "$log" 0221 48 0136 0001 "0400$(pad 4002ffff 20)"
sync_send "$log"
sleep 1.5
check "a service package table: said once as not selecting by VID" \
      "$(grep -c 'does not select a profile by VID' "$log")" "1"

# ------------------------------------- ME 311, from what igmpd writes
# Class 311 (0x0137) instance 1. The state file is igmpd's; written here the
# way it writes it.
send "$log" 0070 44 0137 0001 ""
check "311 with no state file: no joins, no groups" \
      "$(send "$log" 0071 49 0137 0001 "2000" && answer "$log" 0071 | cut -c7-14)" "00000000"
check "311 with no state file: the group table is empty" \
      "$(table "$log" b10 0137 0001 0800)" ""
printf 'joins=42\ndenied=3\ngroups=2\ngroup=239.1.2.3\ngroup=225.200.7.9\n' > /var/run/odi-mcast.state
check "311 JoinMsgCounter is igmpd's join count" \
      "$(send "$log" 0072 49 0137 0001 "2000" && answer "$log" 0072 | cut -c7-14)" "0000002a"
G1=$(pad 000000000000ef010203 24)
G2=$(pad 000000000000e1c80709 24)
check "311 active group list: one 24-byte row a group, the group in the destination field" \
      "$(table "$log" b11 0137 0001 0800)" "$G1$G2"
# The same file as igmpd itself writes it (the replay's final state: two
# accepted joins, one refused and not counted, 225.2.2.2 held and 225.3.3.3 left): one test of the
# format between the two processes, in place of two halves with a fixture
# each.
cp /tmp/mcast-state-from-igmpd /var/run/odi-mcast.state
check "311 JoinMsgCounter from igmpd's own state file" \
      "$(send "$log" 0073 49 0137 0001 "2000" && answer "$log" 0073 | cut -c7-14)" "00000002"
check "311 active group list from igmpd's own state file: the group still held" \
      "$(table "$log" b12 0137 0001 0800)" "$(pad 000000000000e1020202 24)"
check "311 reads igmpd's own file with its refusal line: the group list is unchanged" \
      "$(table "$log" b15 0137 0001 0800)" "$(pad 000000000000e1020202 24)"
# The refusal counters and ring that igmpd adds (state.h): header keys, then
# the group lines, then the ring. omcid reads joins and group= lines and
# skips every other key; and a file cut off inside a ring line (a reader
# with a short buffer) still gives the joins and the groups before the cut.
printf 'joins=5\ndenied=2\ngroups=1\nrefused_acl=1\nrefused_limit=1\ngroup=239.1.2.3\nrefused=acl 239.9.9.9 vid=0 at=5 n=1\nrefused=limit 239.9.9.8 vid=10 at=9 n=1\n' > /var/run/odi-mcast.state
check "311 JoinMsgCounter with refusal counters and a ring in the file" \
      "$(send "$log" 0074 49 0137 0001 "2000" && answer "$log" 0074 | cut -c7-14)" "00000005"
check "311 active group list with a ring after the groups" \
      "$(table "$log" b13 0137 0001 0800)" "$G1"
printf 'joins=6\ndenied=2\ngroups=1\nrefused_acl=1\ngroup=239.1.2.3\nrefused=acl 239.9.9.9 vid=0 at=5 n=1\nrefused=limit 239.9' > /var/run/odi-mcast.state
check "311 JoinMsgCounter from a file cut off inside a refusal line" \
      "$(send "$log" 0075 49 0137 0001 "2000" && answer "$log" 0075 | cut -c7-14)" "00000006"
check "311 active group list from a file cut off inside a refusal line" \
      "$(table "$log" b14 0137 0001 0800)" "$G1"
rm -f /var/run/odi-mcast.state

# --------------------------------------------------------------- MIB reset
send "$log" 0070 4f 0002 0000 ""
written "$log" $((m + 1))
check "a MIB reset: no snooping" "$(policy)" "snooping=0"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" mcast-test.log
if [ "$fail" -eq 0 ]; then echo "mcast-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
