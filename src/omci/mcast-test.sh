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
# function other than snooping turning it off; the upstream IGMP tag control,
# TCI and rate exported, and read by igmpd; a delete of the address row
# turning it off; a MIB reset; the file rewritten only when it changed; the
# access control list rows of OMCC 0xB0 (row parts 1 and 2, IPv6 ranges left
# out, an IPv6-only list enforcing nothing); the width of attribute 16 of
# class 309 (3 bytes, G.988); the service package table said not to select a
# profile; the IGMP version and the querier address exported; the rows of
# the access control lists carrying their VLAN, source, GEM port, bandwidth
# and preview fields, and igmpd reading them; the allowed preview groups table
# of ME 310; class 309 Set and Create refusing a value G.988 does not define
# for version, function, immediate leave, tag control and unauthorized join,
# and accepting function 1 and 2.
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
igmp_version=2
immediate_leave=1
robustness=2
query_interval=125
query_max_response=10
last_member_query=1
unauthorized_join=0
us_tag_control=0
us_tci=0
us_rate=0
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
igmp_version=2
immediate_leave=1
robustness=3
query_interval=60
query_max_response=5
last_member_query=2
unauthorized_join=1
us_tag_control=0
us_tci=0
us_rate=0
max_groups=8
allow=225.0.0.0-225.255.255.255 v=11 g=4095
static=226.1.2.3-226.1.2.3 v=11 g=4095"

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

# The upstream IGMP tag control, its TCI and the rate (attributes 5, 4, 6;
# mask 0x1c00): tag control 1, TCI 0xa1a4 (priority 5, VID 420), 2 messages a
# second. With them igmpd takes the reports and sends each up itself, with
# that tag (uplink.h); the same contract, read by igmpd.
send "$log" 0034 48 0135 0001 "1c00a1a40100000002"
written "$log" 4
check "the upstream tag control, TCI and rate are exported as set" \
      "$(policy | grep '^us_')" "us_tag_control=1
us_tci=41380
us_rate=2"
if [ -x "$IGMPD" ]; then
	cp /var/run/odi-mcast.conf /tmp/mcast-policy-snap
	$Q "$IGMPD" -r /tmp/mcast-replay.in > /tmp/mcast-replay.out 2>&1
	check "igmpd takes the reports under that profile" \
	      "$(rep | grep -c '^snooping lease: take host IGMP')" "1"
	check "igmpd sends each report and the leave up with VID 420 priority 5" \
	      "$(rep | grep -c 'up: send with vid 420 pri 5 (tag control 1)')" "4"
fi
send "$log" 0035 48 0135 0001 "1c0000000000000000"
written "$log" 5
check "and back to none: no tag, no rate" "$(policy | grep '^us_')" "us_tag_control=0
us_tci=0
us_rate=0"

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
      "$(policy | grep '^allow')" "allow=225.1.0.0-225.1.0.255 v=11 g=4095 p=255,10,0,0"

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

# These come last: the checks above count the policy writes from the start, and these write
# it many times over.
# ---------------------------------------------- G.988 values, class 309
# A Set or a Create of the few-valued attributes refuses a value G.988 9.3.27
# does not define: result 9 and the attribute in the failed mask, the others
# of the Set stored, while IGMP_SNOOPING is 1. Every defined value is accepted whether or not it is
# done: function 1 and 2 (proxy) are stored and the multicast stays
# transparent, so an OLT that provisions a proxy gets its Set accepted.
# flush: the policy file has caught up with the Sets sent so far.
flush() {
	sync_send "$log"
	sleep 1.5
}
# Without the opt-in key (IGMP_SNOOPING=1) nothing of it applies: the profile is
# stored as the OLT sends it, as it always was.
send "$log" 0098 48 0135 0001 "800009"
check "309 Set: with IGMP_SNOOPING off, version 9 is stored as before" \
      "$(answer "$log" 0098 | cut -c1-2)" "00"
send "$log" 0099 48 0135 0001 "800002"
printf 'IGMP_SNOOPING=1\n' > /var/config/odi.conf
send "$log" 0100 48 0135 0001 "800009"
check "309 Set: IGMP version 9 is refused, result 9 and the failed mask" \
      "$(answer "$log" 0100 | cut -c1-2,7-10)" "098000"
send "$log" 0101 49 0135 0001 "8000"
check "309 Set: and the version is still 2" "$(answer "$log" 0101 | cut -c7-8)" "02"
send "$log" 0102 48 0135 0001 "800003"
check "309 Set: IGMP version 3 is accepted" "$(answer "$log" 0102 | cut -c1-2)" "00"
flush
check "309 Set: and exported" "$(policy | grep '^igmp_version')" "igmp_version=3"
send "$log" 0103 48 0135 0001 "800011"
check "309 Set: MLD version 17 is defined, accepted" "$(answer "$log" 0103 | cut -c1-2)" "00"
send "$log" 0104 48 0135 0001 "800002"
send "$log" 0105 48 0135 0001 "400002"
check "309 Set: IGMP function 2 (proxy) is accepted" "$(answer "$log" 0105 | cut -c1-2)" "00"
flush
check "309 Set: function 2 is exported as set, and the multicast is transparent" \
      "$(policy | sed -n 1,2p)" "snooping=0
igmp_function=2"
send "$log" 0106 48 0135 0001 "400003"
check "309 Set: IGMP function 3 is refused" "$(answer "$log" 0106 | cut -c1-2,7-10)" "094000"
send "$log" 0107 48 0135 0001 "400000"
flush
check "309 Set: function 0 again: snooping" "$(policy | sed -n 1p)" "snooping=1"
send "$log" 0108 48 0135 0001 "080004"
check "309 Set: upstream tag control 4 is refused" "$(answer "$log" 0108 | cut -c1-2,7-10)" "090800"
send "$log" 0109 48 0135 0001 "000202"
check "309 Set: unauthorized join behaviour 2 is refused" "$(answer "$log" 0109 | cut -c1-2,7-10)" "090002"
send "$log" 0110 48 0135 0001 "200002"
check "309 Set: immediate leave 2 is refused" "$(answer "$log" 0110 | cut -c1-2,7-10)" "092000"
send "$log" 0111 48 0135 0001 "60000703"
check "309 Set: function 7 and leave 3, both refused" \
      "$(answer "$log" 0111 | cut -c1-2,7-10)" "096000"
# One bad value does not take the others of the Set with it: function 7 is
# refused, immediate leave off is stored.
send "$log" 0112 48 0135 0001 "60000700"
check "309 Set: function 7 refused, immediate leave 0 beside it" \
      "$(answer "$log" 0112 | cut -c1-2,7-10)" "094000"
send "$log" 0113 49 0135 0001 "2000"
check "309 Set: and the immediate leave was stored" "$(answer "$log" 0113 | cut -c7-8)" "00"
send "$log" 0114 48 0135 0001 "200001"
send "$log" 0115 44 0135 0002 "090001"
check "309 Create: IGMP version 9 is refused, result 9 and the attribute in the execution mask" \
      "$(answer "$log" 0115 | cut -c1-6)" "098000"
send "$log" 0117 44 0135 0002 "020201"
check "309 Create: function 2 is accepted (and the refused Create made no instance: not 7)" \
      "$(answer "$log" 0117 | cut -c1-2)" "00"
send "$log" 0118 49 0135 0002 "4000"
check "309 Create: and reads back as 2" "$(answer "$log" 0118 | cut -c7-8)" "02"
send "$log" 0119 46 0135 0002 ""
send "$log" 0120 44 0135 0002 "020103"
check "309 Create: immediate leave 3 is refused" "$(answer "$log" 0120 | cut -c1-6)" "092000"
send "$log" 0121 48 0135 0001 "800002"
flush
check "309: the refused writes and the restored ones leave the policy as it was" \
      "$(policy | sed -n 1,4p)" "snooping=1
igmp_function=0
igmp_version=2
immediate_leave=1"

rm -f /var/config/odi.conf

# The querier address (attribute 11, mask 0x0020), exported only when set.
send "$log" 0130 48 0135 0001 "00200a010203"
flush
check "the querier address is exported when set" "$(policy | grep '^querier')" "querier=10.1.2.3"
send "$log" 0131 48 0135 0001 "002000000000"
flush
check "and not when it is 0.0.0.0" "$(policy | grep -c '^querier')" "0"

# ---------------------- the rows' VLAN, source, GEM port, bandwidth, preview
# acl0 <control> <gem> <vlan> <src> <start> <end> <bw>: row part 0 (the
# address fields in hex); acl1 <control> <length> <repeat> <count> <reset>:
# part 1 in the IPv4 form (ten zero bytes, ffff) with the preview fields.
# Key 1 names everything: 226.0.1.0-226.0.1.255, VLAN 1410, source 10.0.0.5,
# GEM 4000, 123456 bytes/s, and a preview of 30 s, 60 s apart, 3 times. Key 2
# says nothing (GEM port 0 and VLAN 0, an untagged downstream flow, source
# 0.0.0.0, "ignored"): a bare range. Key 3 has a part 1 with a preview length of 0:
# fully authorized, so no preview. Key 4 has VLAN 4097 (tagged, any VID) and
# GEM 0xffff (unspecified), which say nothing about a join either.
acl0() {
	printf '%04x%04x%04x%s%s%s%s0000' "$1" "$2" "$3" "$4" "$5" "$6" "$7"
}
acl1() {
	printf '%04x00000000000000000000ffff%04x%04x%04x%04x0000' "$1" "$2" "$3" "$4" "$5"
}
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0300 48 0135 0001 "0200c000"
t=0301
for row in "$(acl0 $((0x4001)) 4000 1410 0a000005 e2000100 e20001ff 0001e240)" \
           "$(acl1 $((0x4801)) 30 60 3 0)" \
           "$(acl0 $((0x4002)) 0 0 00000000 e3000100 e30001ff 00000000)" \
           "$(acl0 $((0x4003)) 0 0 00000000 e4000100 e40001ff 00000000)" \
           "$(acl1 $((0x4803)) 0 60 3 0)" \
           "$(acl0 $((0x4004)) 65535 4097 00000000 e5000100 e50001ff 00000000)"; do
	send "$log" "$t" 48 0135 0001 "0200$row"
	t=$(printf '%04x' $((0x$t + 1)))
done
written "$log" $((m + 1))
check "ACL rows carry their VLAN, source, GEM port, bandwidth and preview, when they say something" \
      "$(policy | grep '^allow' | sort)" "allow=226.0.1.0-226.0.1.255 v=1410 s=10.0.0.5 g=4000 b=123456 p=30,60,3,0
allow=227.0.1.0-227.0.1.255
allow=228.0.1.0-228.0.1.255
allow=229.0.1.0-229.0.1.255"

# The allowed preview groups table (ME 310 attribute 7, mask 0x0200): two rows
# of 22 bytes under one key. Part 0 (0x4000 | key): source (16 bytes), VLAN on
# the ANI side, on the UNI side. Part 1 (0x4800 | key): destination (16 bytes),
# duration in minutes, time left. Z16 is an all-zero IPv4-form address.
Z16=00000000000000000000000000000000
agrant0() { printf '%04x%s%04x%04x' "$1" "$2" "$3" "$4"; }
agrant1() { printf '%04x%s%04x0000' "$1" "$2" "$3"; }
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0310 48 0136 0001 "0200$(agrant0 $((0x4001)) $Z16 100 0)"
send "$log" 0311 48 0136 0001 "0200$(agrant1 $((0x4801)) 000000000000000000000000ef090505 0)"
written "$log" $((m + 1))
check "an allowed preview group: IPv4, the VLANs, no end" \
      "$(policy | grep '^grant')" "grant=239.9.5.5 a=100 u=0"
send "$log" 0312 48 0136 0001 "0200$(agrant1 $((0x4801)) 000000000000000000000000ef090505 90)"
written "$log" $((m + 2))
check "an allowed preview group: 90 minutes" \
      "$(policy | grep '^grant')" "grant=239.9.5.5 a=100 u=0 d=90"
send "$log" 0313 48 0136 0001 "0200$(agrant0 $((0x4002)) $Z16 0 0)"
flush
check "a half row (a part 0 with no part 1) is not listed" \
      "$(policy | grep -c '^grant')" "1"
send "$log" 0314 48 0136 0001 "0200$(agrant0 $((0x4003)) $Z16 0 0)"
send "$log" 0315 48 0136 0001 "0200$(agrant1 $((0x4803)) ff3e0000000000000000000000001234 0)"
flush
check "an IPv6 row is not listed" "$(policy | grep -c '^grant')" "1"

# igmpd reads what omcid just wrote (the contract): the join rules line, and
# the groups of a bare row and of a preview row.
if [ -x "$IGMPD" ]; then
	cp /var/run/odi-mcast.conf /tmp/mcast-policy-snap
	cat > /tmp/mcast-replay2.in <<'EOF2'
0 policy-file /tmp/mcast-policy-snap
0 tick
1 v2join 1 0 192.168.1.10 227.0.1.5
2 v2join 1 0 192.168.1.10 226.0.1.5
3 state
EOF2
	$Q "$IGMPD" -r /tmp/mcast-replay2.in > /tmp/mcast-replay2.out 2>&1
	check "igmpd reads the rows: version, VLAN, source, preview and allowed preview group" \
	      "$(grep 'join rules' /tmp/mcast-replay2.out)" \
	      "  join rules: IGMP version 2 at most (a message above it is only counted); 1 of 4 rows name a VLAN; 1 name a source; 1 are previews; 1 allowed preview groups"
	check "igmpd lets in a bare row's group, and the first preview of a preview row" \
	      "$(grep '^group=' /tmp/mcast-replay2.out | sort -u)" "group=226.0.1.5
group=227.0.1.5"
fi

# --------------------------------------------------------------- MIB reset
m=$(grep -c 'multicast policy written' "$log")
send "$log" 0070 4f 0002 0000 ""
written "$log" $((m + 1))
check "a MIB reset: no snooping" "$(policy)" "snooping=0"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" mcast-test.log
if [ "$fail" -eq 0 ]; then echo "mcast-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
