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
# turning it off; a MIB reset; the file rewritten only when it changed.
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

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision \
      /var/run/odi-mcast.conf
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/mcast-gpon

log=/tmp/omcid-mcast.log
$Q respond/build/omcid -d -g /tmp/mcast-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
written "$log" 1
check "an empty MIB: the file exists and says no snooping" "$(policy)" "snooping=0"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset

# ---------------------------------------------------------- a full session
# Class 309 (0x0135) instance 1, created with the attributes a Create carries
# (1-6, 10-13 and 16): IGMP version 2, function 0, immediate leave on, the
# timers unset. Class 310 (0x0136) instance 1 names it, a limit of 8 groups.
# CTP 5 on GEM 4095, downstream only, and class 281 (0x0119) on it with one
# address row.
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

# --------------------------------------------------------------- MIB reset
send "$log" 0070 4f 0002 0000 ""
written "$log" $((n + 5))
check "a MIB reset: no snooping" "$(policy)" "snooping=0"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" mcast-test.log
if [ "$fail" -eq 0 ]; then echo "mcast-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
