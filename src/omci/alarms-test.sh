#!/bin/sh
# Alarms and attribute value changes (respond/alarm.c): what omcid sends the
# OLT on its own, and Get All Alarms / Get All Alarms Next.
#
# omcid-drvtrace runs with -a, so the driver calls (command 11, the signal
# thresholds) are in the log; its DDM stub reads /tmp/omcid-ddm ("<rx> <tx>"
# in 0.1 uW) when it exists. The PON state and the link come in through -g
# and -l, as in live-test.sh. With no line side an autonomous message is
# printed as "=> <frame>" (a reply as "-> ").
#
# Run from src/omci, inside the toolchain image (make test-omci-alarms):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh alarms-test.sh
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

# frame <tci> <type> <class> <inst> <contents hex>, contents zero-padded.
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}
send() {   # send <tci> <type> <class> <inst> <contents>
	inject_wait "$log" "$(frame "$1" "$2" "$3" "$4" "$5")"
}
answer() {  # the contents of the last answer to a tci
	grep -- "^-> $1" "$log" | tail -1 | cut -c20-83
}
gpon() {    # gpon <state>
	printf 'state %s (O%s)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' \
		"$1" "$1" > /tmp/alarms-gpon
}
Z27=000000000000000000000000000000000000000000000000000000
Z30=000000000000000000000000000000000000000000000000000000000000
# alarm <class> <inst> <first bitmap byte> <seq>: the notification, as printed
alarm() { printf '=> 0000100a%s%s%s' "$1" "$2" "${3:-}"; [ -n "${4:-}" ] && printf '%s%s' "$Z30" "$4"; return 0; }
# avc <class> <inst> <mask> <value>
avc() { printf '=> 0000110a%s%s%s%s' "${1:-}" "${2:-}" "${3:-}" "${4:-}"; }
count() { grep -c -- "^$1" "$log"; }

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision \
      /tmp/omcid-ddm
gpon 5
printf '1\n' > /tmp/alarms-link

log=/tmp/omcid-alarms.log
: > "$log"
$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -g /tmp/alarms-gpon -l /tmp/alarms-link > "$log" 2>&1 &
pid=$!
omcid_up "$log"
sleep 3
check "nothing is sent while nothing changes" "$(count '=> ')" "0"

# The host link goes: the PPTP port's operational state (attribute 6, mask
# 0400) becomes disabled, and LAN-LOS (alarm 0, the top bit) is declared,
# alarm sequence number 1.
printf '0\n' > /tmp/alarms-link
need "$log" "^$(alarm 000b 0101 80 01)" 10
check "link down: the AVC of the operational state, disabled" \
      "$(count "$(avc 000b 0101 0400 01)")" "1"
check "link down: LAN-LOS declared, sequence number 1" \
      "$(count "$(alarm 000b 0101 80 01)")" "1"

# Get All Alarms: one entity; Next 0 is it; Next 1 is past the end.
send 0101 4b 0002 0000 ""
check "Get All Alarms counts the entity with an alarm" \
      "$(answer 0101 | cut -c1-4)" "0001"
send 0102 4c 0002 0000 "0000"
check "Get All Alarms Next 0: the PPTP port and its bitmap" \
      "$(answer 0102)" "000b010180$Z27"
send 0103 4c 0002 0000 "0001"
check "Get All Alarms Next past the end is a parameter error" \
      "$(answer 0103 | cut -c1-2)" "03"

# The link comes back: enabled, LAN-LOS cleared. Get All Alarms reset the
# sequence number, so this one is 1 again.
printf '1\n' > /tmp/alarms-link
need "$log" "^$(alarm 000b 0101 00 01)" 10
check "link up: the AVC of the operational state, enabled" \
      "$(count "$(avc 000b 0101 0400 00)")" "1"
check "link up: LAN-LOS cleared, the sequence restarted by Get All Alarms" \
      "$(count "$(alarm 000b 0101 00 01)")" "1"

# Received power 0.5 uW (-33 dBm), under the default low threshold (-28 dBm):
# ANI-G 0x8001 alarm 0, sequence 2.
printf '5 5000\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 80 02)" 10
check "low received power: ANI-G alarm 0" "$(count "$(alarm 0107 8001 80 02)")" "1"
# Transmit power 50 uW (-13 dBm) too, under -8 dBm: alarm 4 joins it.
printf '5 500\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 88 03)" 10
check "low transmit power: ANI-G alarm 4, the bitmap carries both" \
      "$(count "$(alarm 0107 8001 88 03)")" "1"
printf '5 5000\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 80 04)" 10
# 1.7 uW (-27.7 dBm) is inside the threshold but within its 0.5 dB band:
# the alarm holds. 3.5 uW (-24.6 dBm) clears it.
printf '17 5000\n' > /tmp/omcid-ddm
sleep 3
check "a level just inside the threshold does not clear it (0.5 dB band)" \
      "$(count "$(alarm 0107 8001 00)")" "0"
printf '35 5000\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 00 05)" 10
check "back above the band: cleared" "$(count "$(alarm 0107 8001 00 05)")" "1"

# A threshold the OLT set wins over the default: low Rx threshold 0x28
# (-20 dBm, attribute 11, mask 0020), and 3.5 uW (-24.6 dBm) is under it.
send 0110 48 0107 8001 "002028"
need "$log" "^$(alarm 0107 8001 80 06)" 10
check "an OLT low Rx threshold of -20 dBm raises the alarm at -24.6 dBm" \
      "$(count "$(alarm 0107 8001 80 06)")" "1"

# ARC on (attribute 8 and its interval 9, mask 0180; 255 = indefinitely):
# the alarm table still moves, the notification does not go.
send 0111 48 0107 8001 "018001ff"
printf '1000 5000\n' > /tmp/omcid-ddm
sleep 3
check "ARC on: a cleared alarm is not notified" \
      "$(count "$(alarm 0107 8001 00 07)")" "0"
send 0112 4b 0002 0000 ""
check "but the alarm table moved: Get All Alarms finds nothing" \
      "$(answer 0112 | cut -c1-4)" "0000"
# ARC off, then on again with interval 0: it ends at the next second, ARC
# reads off again and the AVC of ARC (mask 0100) says so.
send 0113 48 0107 8001 "010000"
sleep 2
send 0114 48 0107 8001 "01800100"
need "$log" "^$(avc 0107 8001 0100 00)" 10
check "an ARC interval that ends is an AVC of ARC, off" \
      "$(count "$(avc 0107 8001 0100 00)")" "1"
send 0115 49 0107 8001 "0100"
check "and ARC reads off" "$(answer 0115 | cut -c1-8)" "00010000"

# The OLT unlocks the VEIP (attribute 1): its operational state (2, mask
# 4000), which follows, is an AVC; the AdminState it wrote is not.
send 0120 48 0149 0601 "800000"
need "$log" "^$(avc 0149 0601 4000 00)" 10
check "VEIP unlocked: the AVC of its operational state" \
      "$(count "$(avc 0149 0601 4000 00)")" "1"
check "and none of the AdminState the OLT wrote" \
      "$(count "$(avc 0149 0601 8000)")" "0"

# Signal fail / degrade thresholds go to the kernel: driver command 11,
# {4, SF} on a Set of attribute 6 (mask 0400), {5, SD} of 7 (0200).
send 0130 48 0107 8001 "06000607"
check "a Set of the SF and SD thresholds sends both to the kernel" \
      "$(grep -c '^drv 11 8 0000000400000006$' "$log")$(grep -c '^drv 11 8 0000000500000007$' "$log")" "11"
# The kernel's signal lines raise alarms 2 (SF) and 3 (SD).
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\nsignal sf 1 threshold 6\nsignal sd 1 threshold 7\n' > /tmp/alarms-gpon
need "$log" "^$(alarm 0107 8001 30 01)" 10
check "the kernel's SF and SD lines: ANI-G alarms 2 and 3" \
      "$(count "$(alarm 0107 8001 30 01)")" "1"

# Laser bias (ANI-G alarm 6, bit 0x02 of the first bitmap byte): the module's
# own flags, A2h byte 112, the third field of the stub file. A temperature
# flag (0x80) is not a bias flag and raises nothing; the bias low flag (0x04)
# raises alarm 6, the high flag (0x08) keeps it, no flag clears it.
n=$(count '=> ')
printf '1000 5000 128\n' > /tmp/omcid-ddm
sleep 3
check "a module flag that is not about the bias raises nothing" "$(count '=> ')" "$n"
printf '1000 5000 4\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 32 02)" 10
check "the module's bias low flag: ANI-G alarm 6 joins the standing ones"       "$(count "$(alarm 0107 8001 32 02)")" "1"
printf '1000 5000 8\n' > /tmp/omcid-ddm
sleep 3
check "the bias high flag instead: still raised, nothing new sent" "$(count '=> ')" "$((n + 1))"
printf '1000 5000 0\n' > /tmp/omcid-ddm
need "$log" "^$(alarm 0107 8001 30 03)" 10
check "no flag: alarm 6 cleared" "$(count "$(alarm 0107 8001 30 03)")" "1"

# Not O5: nothing is sent, the link goes unreported. Back in O5 the standing
# LAN-LOS is declared; the operational state that moved meanwhile is taken
# as it is, not sent.
gpon 3
sleep 2
n=$(count '=> ')
printf '0\n' > /tmp/alarms-link
sleep 3
check "outside O5 nothing is sent" "$(count '=> ')" "$n"
gpon 5
need "$log" "^$(alarm 000b 0101 80 04)" 10
check "back in O5: the standing LAN-LOS is declared" \
      "$(count "$(alarm 000b 0101 80 04)")" "1"
check "and the operational state that moved outside O5 is not an AVC" \
      "$(count "$(avc 000b 0101 0400 01)")" "1"

# MIB reset: the thresholds back to their defaults in the kernel, the
# bitmaps cleared, the sequence back to 1, the standing LAN-LOS declared
# again; and no AVC flood.
n=$(count "$(avc '')")
send 0140 4f 0002 0000 ""
check "MIB reset: the default SF and SD thresholds go to the kernel" \
      "$(grep -c '^drv 11 8 0000000400000005$' "$log")$(grep -c '^drv 11 8 0000000500000009$' "$log")" "11"
need "$log" "^$(alarm 000b 0101 80 01)" 10 2
check "MIB reset: LAN-LOS declared again, sequence 1" \
      "$(count "$(alarm 000b 0101 80 01)")" "2"
check "MIB reset: no AVC flood" "$(count "$(avc '')")" "$n"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" alarms-test.log
if [ "$fail" -eq 0 ]; then echo "alarms-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
exit "$fail"
