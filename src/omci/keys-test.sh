#!/bin/sh
# The two keys that decide which entities the ONU creates for itself:
#
#   OMCI_VEIP_SLOT_ID (config store, the stock key; effective only with bit
#     0x100 of OMCI_CUSTOM_ME, as on the stock firmware): the slot of the VEIP,
#     6 unless set. The VEIP, its UNI-G and the TR-069 server are at
#     (slot << 8) | 1, the cardholder and circuit pack at 0x0100 | slot.
#   OMCI_PRIVATE_ME (odi.conf): 0 leaves out the vendor-private entities 350,
#     370 and 373, which the ONU creates by default.
#
# Each case starts omcid on a config store, asks for the entities and the MIB
# upload count (the log line of a MIB upload), and stops it.
#
# Run from src/omci, inside the toolchain image:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh keys-test.sh
set -u
Q=qemu-mips-static
fail=0

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

wait_count() {
	i=0
	while [ "$i" -lt $(( $4 * 10 )) ]; do
		[ "$(grep -c -F -- "$2" "$1")" -ge "$3" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

mkdir -p /var/config /var/run
rm -f /var/run/omcid-swimage /var/run/omcid-mib.snap /var/run/omcid-resume-decision

# store <slot key> <custom me>: the config store; an empty argument leaves the
# key out.
store() {
	{
		printf '<Config Name="ROOT">\n\t<Dir Name="MIB_TABLE">\n'
		[ -n "$1" ] && printf '\t\t<Value Name="OMCI_VEIP_SLOT_ID" Value="%s"/>\n' "$1"
		[ -n "$2" ] && printf '\t\t<Value Name="OMCI_CUSTOM_ME" Value="%s"/>\n' "$2"
		printf '\t</Dir>\n</Config>\n'
	} > /var/config/lastgood.xml
}

# run <log>: omcid in the background on the current store and odi.conf
run() {
	rm -f "$1"
	$Q respond/build/omcid -d > "$1" 2>&1 &
	pid=$!
	wait_count "$1" "answering as" 1 20
}
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }

# inst <class>: the instance ids of a class, decimal, comma separated
inst() {
	$Q cli/build/omcli mib get "$1" --vendor 2>&1 | sed -n 's/^'"$1"' .* \([0-9][0-9]*\)$/\1/p' | tr '\n' ,
}
# upload <log>: the entity count a MIB upload announces
printf '0d014d0a00020000%064d00000028%08d\n' 0 0 > /tmp/keys-upload.txt
upload() {
	$Q cli/build/omcli --inject-file /tmp/keys-upload.txt > /dev/null 2>&1
	wait_count "$1" "upload records" 1 10
	sed -n 's/^-> \([0-9]*\) upload records.*/\1/p' "$1" | tail -1
}

# 0x0601 = 1537, 0x0106 = 262, 0x0e01 = 3585, 0x010e = 270.

# --------------------------------------------------------------- the VEIP
rm -f /var/config/odi.conf
store "" ""
run /tmp/keys-1.log
check "no key: the VEIP is in slot 6 (0x0601)" "$(inst 329)" "1537,"
check "no key: the UNI-G and the TR-069 server are at 0x0601" "$(inst 264)$(inst 340)" "257,1537,1537,"
check "no key: the circuit pack is at 0x0106" "$(inst 6)" "257,262,384,"
check "no key: and the cardholder" "$(inst 5)" "257,262,384,"
check "no key: ME 373 is created" "$(inst 373)" "0,"
base=$(upload /tmp/keys-1.log)
# 399 records, as the stock firmware's own upload of this configuration
# (a decoded ISP2 session of the stock image: 205 entities, 399 records).
# Without bit 64 of OMCI_CUSTOM_ME there is no CTC ONU capability entity
# (class 65529), as on the stock firmware.
check "no key: the MIB upload announces 399 records, as the stock ONU" "$base" "399"
# ONU2-G (class 257) is records 51 and 52; the second carries what the
# first has no room for -- queue, scheduler and GEM port counts, uptime,
# connectivity and QoS flexibility -- under its own mask, as the stock
# upload does. omcid used to drop all of it.
printf '0f054e0a000200000034%060d00000028%08d\n' 0 0 > /tmp/keys-next.txt
$Q cli/build/omcli --inject-file /tmp/keys-next.txt > /dev/null 2>&1
wait_count /tmp/keys-1.log "-> 0f052e0a" 1 10
check "ONU2-G's second upload record: priority queues 128, schedulers 16, mode 1, GEM ports 64" \
      "$(grep -o -- '-> 0f052e0a000200000101000007fc008010010040' /tmp/keys-1.log | head -1)" \
      "-> 0f052e0a000200000101000007fc008010010040"
stop

store 14 ""
run /tmp/keys-2.log
check "slot key without OMCI_CUSTOM_ME bit 256: the VEIP stays at 0x0601" "$(inst 329)" "1537,"
check "but the VEIP reports the key as its customized slot" \
      "$($Q cli/build/omcli mib getattr 329 1537 6)" "class 329 entity 1537 attr 6 = 0e"
stop

store 14 65792
run /tmp/keys-3.log
check "slot 14 with the bit: the VEIP is at 0x0e01" "$(inst 329)" "3585,"
check "the UNI-G and the TR-069 server follow" "$(inst 264)$(inst 340)" "257,3585,3585,"
check "the circuit pack is at 0x010e" "$(inst 6)" "257,270,384,"
check "and the cardholder" "$(inst 5)" "257,270,384,"
check "the VEIP customized slot attribute is 14" \
      "$($Q cli/build/omcli mib getattr 329 3585 6)" "class 329 entity 3585 attr 6 = 0e"
check "the circuit pack at the new slot has the VEIP card type (0x30)" \
      "$($Q cli/build/omcli mib getattr 6 270 1)" "class 6 entity 270 attr 1 = 30"
check "the MIB upload count does not change" "$(upload /tmp/keys-3.log)" "$base"
stop

for bad in 128 1 255 0 abc 300; do
	store "$bad" 65792
	run /tmp/keys-bad.log
	check "slot key $bad is ignored: the VEIP stays at 0x0601" "$(inst 329)" "1537,"
	stop
done

# ---------------------------------------------------------- the private MEs
store "" ""
printf 'OMCI_PRIVATE_ME=0\n' > /var/config/odi.conf
run /tmp/keys-4.log
check "OMCI_PRIVATE_ME=0: no ME 373" "$(inst 373)" ""
check "OMCI_PRIVATE_ME=0: no ME 350" "$(inst 350)" ""
check "OMCI_PRIVATE_ME=0: no ME 370" "$(inst 370)" ""
# 350 takes two records, 370 one, 373 fifteen (its fifteen 25-byte parts).
check "OMCI_PRIVATE_ME=0: the MIB upload announces their 18 records fewer" \
      "$(upload /tmp/keys-4.log)" "$((base - 18))"
check "OMCI_PRIVATE_ME=0: the VEIP is untouched" "$(inst 329)" "1537,"
stop

for v in 1 "" yes; do
	printf 'OMCI_PRIVATE_ME=%s\n' "$v" > /var/config/odi.conf
	run /tmp/keys-5.log
	check "OMCI_PRIVATE_ME=$v: the three entities are created" \
	      "$(inst 350)$(inst 370)$(inst 373)" "0,257,0,"
	stop
done

# ------------------------------------------------------------ no VEIP at all
store "" ""
printf 'OMCI_VEIP=0\n' > /var/config/odi.conf
run /tmp/keys-6.log
check "OMCI_VEIP=0: no VEIP" "$(inst 329)" ""
check "OMCI_VEIP=0: no TR-069 server" "$(inst 340)" ""
check "OMCI_VEIP=0: the UNI-G of the PPTP Ethernet UNI stays" "$(inst 264)" "257,"
check "OMCI_VEIP=0: the cardholder and circuit pack of the VEIP slot are gone" \
      "$(inst 5)$(inst 6)" "257,384,257,384,"
check "OMCI_VEIP=0: the PPTP Ethernet UNI stays" "$(inst 11)" "257,"
# Cardholder 2 records, circuit pack 4, VEIP 3, UNI-G 1, TR-069 server 1.
check "OMCI_VEIP=0: the MIB upload announces the 11 records of five entities fewer" \
      "$(upload /tmp/keys-6.log)" "$((base - 11))"
stop
store 14 65792
run /tmp/keys-7.log
check "OMCI_VEIP=0 with a slot key: still no VEIP" "$(inst 329)" ""
stop
store "" ""
for v in 1 ""; do
	printf 'OMCI_VEIP=%s\n' "$v" > /var/config/odi.conf
	run /tmp/keys-8.log
	check "OMCI_VEIP=$v: the VEIP is created" "$(inst 329)" "1537,"
	stop
done

# The tables the ONU keeps for each MAC bridge port (classes 49, 50 and 79)
# are created when the OLT creates the port, each at the port's entity id,
# and go with it; a decoded stock MIB after provisioning has them under every
# port, and the stock upload after a reset has none. 0e70 creates class 47
# instance 7 (bridge 1, port 7, type 3, pointer 1), 0e71 deletes it.
store "" ""
rm -f /var/config/odi.conf
run /tmp/keys-9.log
check "no bridge port yet: no class 49, 50 or 79" "$(inst 49)$(inst 50)$(inst 79)" ""
base=$(upload /tmp/keys-9.log)
printf '0e70440a002f0007%s%052d00000028%08d\n' 000107030001 0 0 > /tmp/keys-bp-create.txt
$Q cli/build/omcli --inject-file /tmp/keys-bp-create.txt > /dev/null 2>&1
wait_count /tmp/keys-9.log "created" 1 10
check "a Create of class 47 creates its filter table (49)" "$(inst 49)" "7,"
check "and its bridge table (50)" "$(inst 50)" "7,"
check "and its filter preassign table (79)" "$(inst 79)" "7,"
# One record for the port, one each for the three tables: 49 and 50 hold
# only a table attribute (no upload attribute, an empty mask), 79 ten bytes.
check "the next MIB upload lists the port and its three tables (4 records)" \
      "$(upload /tmp/keys-9.log)" "$((base + 4))"
printf '0e71460a002f0007%064d00000028%08d\n' 0 0 > /tmp/keys-bp-delete.txt
$Q cli/build/omcli --inject-file /tmp/keys-bp-delete.txt > /dev/null 2>&1
wait_count /tmp/keys-9.log "deleted" 1 10
check "a Delete of the port takes the three tables with it" \
      "$(inst 47)$(inst 49)$(inst 50)$(inst 79)" ""
stop

# A PPTP-only ONU still builds its bridge. ISP1 session (a PPTP Ethernet UNI
# port and a VEIP port) against the traced daemon, once whole with the VEIP
# and once without the Create of the VEIP port (the OLT of a PPTP-only ONU
# makes none), with and without OMCI_VEIP=0: the connections of the PPTP port
# are built either way.
CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440
FIX=../../test/fixtures
printf 'state 5 (O5)\nonu_id 26\nsn 4f44495400000001\neqd multiframe 0 inframe 0\nploam ds_rx 0 us_tx 0\nalloc_ids 5 282 794 1050 1306 538\n' > /tmp/keys-gpon
# the session without the bridge port whose termination point is the VEIP
# (class 47, termination point type 11)
awk '!(substr($0, 9, 4) == "002f" && substr($0, 23, 2) == "0b")' "$FIX/omci-session-isp1.txt" > /tmp/keys-session-pptp.txt
bdg() {   # bdg <session> <odi.conf text> <log>: the bridge connections the session builds
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	printf '%s' "$2" > /var/config/odi.conf
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/keys-gpon > "$3" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$1" > /dev/null 2>&1
	wait "$pid"
	grep -c '^drv 51 ' "$3"
}
whole=$(bdg "$FIX/omci-session-isp1.txt" "" /tmp/keys-b1.log)
pptp=$(bdg /tmp/keys-session-pptp.txt "" /tmp/keys-b2.log)
pptp_nov=$(bdg /tmp/keys-session-pptp.txt 'OMCI_VEIP=0
' /tmp/keys-b3.log)
if [ "$pptp" -gt 0 ] && [ "$pptp" -lt "$whole" ] && [ "$pptp_nov" = "$pptp" ]; then
	echo "ok    PPTP only: $pptp_nov bridge connections built with OMCI_VEIP=0, the same as with the VEIP present ($whole with the VEIP port)"
else
	echo "FAIL  PPTP only: $pptp_nov connections with OMCI_VEIP=0, $pptp with the VEIP present, $whole with the VEIP port"
	fail=$((fail + 1))
fi
rm -f /var/config/odi.conf

if [ "$fail" -ne 0 ]; then
	echo "$fail check(s) FAILED"
	exit 1
fi
echo "all checks passed"
