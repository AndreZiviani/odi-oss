#!/bin/sh
# Values omcid answers from live state or from the configuration, not from the
# generated defaults table (generated/omci_defaults.c): each one has to follow
# its source, so a stick other than the one the table was captured from does
# not report that stick.
#
#   circuit pack (6): serial and vendor id follow the configured serial, the
#     version follows the active software image
#   ONU-G (256): OntState follows the PON state
#   ONU2-G (257): SysUpTime moves, in 10 ms units
#   ANI-G (263): optical signal and transmit levels follow the DDM reads
#   PPTP Ethernet UNI (11): the configuration indication follows the link
#   priority queue (277): PktDropQThold on the reserved queues reads zero
#   T-CONT (262): Policy reads 1 on every T-CONT before the OLT writes anything
#
# The serial, the PON state and the link come in through omcid -g and -l (a
# file in the shape of /proc/odi_gpon/status and the host_link parameter); the
# optical levels come from the DDM stub of the traced build (respond/drv.c).
#
# Run from src/omci, inside the toolchain image:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh live-test.sh
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

# attr <class> <instance> <attribute>: the value of one attribute, as hex
attr() {
	$Q cli/build/omcli mib getattr "$1" "$2" "$3" 2>&1 | sed 's/^.* = //'
}

# gpon <file> <state> <sn hex>: the /proc/odi_gpon/status shape
gpon() {
	printf 'state %s (O%s)\nonu_id 26\nsn %s\neqd multiframe 0 inframe 0\nploam ds_rx 0 us_tx 0\nalloc_ids 1 348\n' \
		"$2" "$2" "$3" > "$1"
}

mkdir -p /var/config /var/run
rm -f /var/config/odi.conf /var/config/omci-identity.on /var/run/omcid-swimage \
      /var/run/omcid-swimage.tmp /var/run/omcid-mib.snap /var/run/omcid-resume-decision
# The identity keys of test/cfg_report.xml plus a manual VLAN: type 1, mode 1,
# VID 25, priority 3.
cat > /var/config/lastgood.xml <<XML
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="OMCI_SW_VER1" Value="ODI-TEST-A"/>
		<Value Name="OMCI_SW_VER2" Value="ODI-TEST-B"/>
		<Value Name="OMCC_VER" Value="160"/>
		<Value Name="OMCI_VENDOR_PRODUCT_CODE" Value="4660"/>
		<Value Name="VLAN_CFG_TYPE" Value="1"/>
		<Value Name="VLAN_MANU_MODE" Value="1"/>
		<Value Name="VLAN_MANU_TAG_VID" Value="25"/>
		<Value Name="VLAN_MANU_TAG_PRI" Value="3"/>
		<Value Name="LOID" Value="ODITESTLOID"/>
		<Value Name="LOID_OLD" Value="ODITESTLOID"/>
		<Value Name="LOID_PASSWD" Value="ODIPW"/>
		<Value Name="LOID_PASSWD_OLD" Value="ODIPW"/>
	</Dir>
</Config>
XML
rm -f /var/run/odi-slot
cp test/cfg_report_hs.xml /var/config/lastgood_hs.xml

# A serial with a vendor id no stick carries: ABCD, then 000000aa.
SN=41424344000000aa
gpon /tmp/live-gpon 3 $SN
printf '1\n' > /tmp/live-link

start() {   # start <binary> <log> [args]: sets pid
	bin=$1; log=$2; shift 2
	$Q "$bin" -d "$@" > "$log" 2>&1 &
	pid=$!
	wait_count "$log" "answering as" 1 20
}
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }

# ----------------------------------------------------------- the plain daemon
start respond/build/omcid /tmp/live-a.log -g /tmp/live-gpon -l /tmp/live-link

# The three instances of the circuit pack the ONU creates: 0x0101, 0x0106, 0x0180.
for inst in 257 262 384; do
	check "circuit pack $inst: the vendor id is the configured serial's" \
	      "$(attr 6 $inst 5)" "41424344"
	check "circuit pack $inst: the serial is the configured serial" \
	      "$(attr 6 $inst 3)" "$SN"
done
Z9=000000000000000000
check "circuit pack: the version is 0.0.0 while no image version is reported" \
      "$(attr 6 257 4)" "302e302e30$Z9"

check "ONU-G OntState follows the PON state (O3)" "$(attr 256 0 14)" "03"
gpon /tmp/live-gpon 5 $SN
check "and moves with it (O5)" "$(attr 256 0 14)" "05"

up1=$(attr 257 0 10)
sleep 2
up2=$(attr 257 0 10)
if [ "$up1" != "$up2" ] && [ $((0x$up2)) -gt $((0x$up1)) ] && [ $((0x$up2 - 0x$up1)) -ge 150 ] \
   && [ $((0x$up2 - 0x$up1)) -le 400 ]; then
	echo "ok    ONU2-G SysUpTime moves, about 100 units a second (10 ms units)"
else
	echo "FAIL  ONU2-G SysUpTime moves, about 100 units a second: $up1 then $up2"
	fail=$((fail + 1))
fi
check "SysUpTime is no longer the captured 0x00000a8c" \
      "$([ "$up1" != 00000a8c ] && echo yes)" "yes"

check "UNI with a link: 1000 Mbit/s full duplex (3)" "$(attr 11 257 7)" "03"
printf '0\n' > /tmp/live-link
check "UNI without a link: unknown (0)" "$(attr 11 257 7)" "00"
printf '1\n' > /tmp/live-link
check "the sensed type is the slot card type" "$(attr 11 257 2)" "2f"

# Reserved queues: no ASCII noise in the thresholds.
check "reserved queue 0xff00: PktDropQThold is zero" \
      "$(attr 277 65280 13)" "0000000000000000"
check "reserved queue 0xff3f: PktDropQThold is zero" \
      "$(attr 277 65343 13)" "0000000000000000"

# Provisioning the OLT never wrote reads as the power-on value, not as what
# another stick's OLT wrote.
check "priority queue 0x8000: SchedulerPtr is unset (0)" "$(attr 277 32768 7)" "0000"
check "priority queue 0x8008: SchedulerPtr is unset (0)" "$(attr 277 32776 7)" "0000"
# T-CONT Policy is the one OLT-writable value the ONU reports on its own: some
# OLTs provision the upstream queues only against a non-null policy.
for tc in 32768 32769 32775 32776 32783; do
	check "T-CONT $tc: Policy is 1 (reported by the ONU)" "$(attr 262 $tc 3)" "01"
done
check "TR-069 server: locked until the OLT unlocks it" "$(attr 340 1537 1)" "01"
check "TR-069 server: no ACS address yet (0xffff, the stock default row)" "$(attr 340 1537 2)" "ffff"
check "TR-069 server: no associated tag yet (0xffff)" "$(attr 340 1537 3)" "ffff"
# The LOID as the stock MIB reset fills it, in ONU-G and in the CTC entity.
check "ONU-G logical ONU ID is the configured LOID" "$(attr 256 0 10 | cut -c1-24)" \
      "4f4449544553544c4f494400"
check "ONU-G logical password is the configured one" "$(attr 256 0 11)" \
      "4f4449505700000000000000"
check "CTC LOID authentication: OperationId is \"CTC \"" "$(attr 65530 0 1)" "43544320"
check "CTC LOID authentication: the same LOID" "$(attr 65530 0 2 | cut -c1-24)" \
      "4f4449544553544c4f494400"
# An OLT-set MAC ageing time is seconds; cmd 62 takes tenths (100 s -> 1000).
$Q cli/build/omcli --inject "0c03480a002d00010040$(printf '00000064%052d' 0)00000028$(printf '%08d' 0)" > /dev/null 2>&1
wait_count /tmp/live-a.log "ageing time 1000" 1 10
check "MAC bridge ageing time 100 s is sent as 1000 tenths" \
      "$(grep -c 'ageing time 1000' /tmp/live-a.log)" "1"
check "VEIP: locked until the OLT unlocks it" "$(attr 329 1537 1)" "01"
check "VEIP: and disabled while locked" "$(attr 329 1537 2)" "01"
# Set VEIP 0x0601 AdminState to 0 (unlocked), as an OLT does.
$Q cli/build/omcli --inject "0c01480a014906018000$(printf '00%058d' 0)00000028$(printf '%08d' 0)" > /dev/null 2>&1
sleep 1
check "VEIP: the OLT unlocked it, so it is unlocked" "$(attr 329 1537 1)" "00"
check "VEIP: and enabled" "$(attr 329 1537 2)" "00"
check "TR-069 server: the VEIP write did not touch it" "$(attr 340 1537 1)" "01"

# The private VLAN configuration is the stored manual VLAN.
check "private VLAN config: type from VLAN_CFG_TYPE" "$(attr 65535 0 1)" "01"
check "private VLAN config: mode from VLAN_MANU_MODE" "$(attr 65535 0 2)" "01"
check "private VLAN config: VID from VLAN_MANU_TAG_VID" "$(attr 65535 0 3)" "0019"
check "private VLAN config: priority from VLAN_MANU_TAG_PRI" "$(attr 65535 0 4)" "03"

# The CTC ONU capability entity (65529) exists only with OMCI_CUSTOM_ME bit
# 0x40, which this store does not set.
check "no OMCI_CUSTOM_ME: no ONU capability entity (65529)" \
      "$($Q cli/build/omcli mib 65529 2>&1 | grep -c '^65529 ')" "0"

# ONU power shedding attribute 3 is a 2-byte interval: the plugin copies the
# descriptor of attribute 2 onto it, which the model used to miss (width 0,
# so a Set of it was dropped).
$Q cli/build/omcli --inject "0c02480a008500002000$(printf '1234%056d' 0)00000028$(printf '%08d' 0)" > /dev/null 2>&1
sleep 1
check "power shedding: a Set of the voice interval is kept" "$(attr 133 0 3)" "1234"
stop

# Any VLAN_CFG_TYPE but 1 reaches the stock omci_app as type 0 with the unset
# sentinels (/etc/runomci.sh); bit 0x40 of OMCI_CUSTOM_ME creates 65529.
sed -i 's/"VLAN_CFG_TYPE" Value="1"/"VLAN_CFG_TYPE" Value="2"/; s#</Dir>#\t<Value Name="OMCI_CUSTOM_ME" Value="64"/>\n\t</Dir>#' \
	/var/config/lastgood.xml
start respond/build/omcid /tmp/live-v.log -g /tmp/live-gpon -l /tmp/live-link
check "VLAN_CFG_TYPE 2: reported as type 0" "$(attr 65535 0 1)" "00"
check "VLAN_CFG_TYPE 2: the mode is passed through" "$(attr 65535 0 2)" "01"
check "VLAN_CFG_TYPE 2: VID unset (65535)" "$(attr 65535 0 3)" "ffff"
check "VLAN_CFG_TYPE 2: priority unset (255)" "$(attr 65535 0 4)" "ff"
check "OMCI_CUSTOM_ME 64: the ONU capability entity exists" \
      "$($Q cli/build/omcli mib 65529 2>&1 | grep -c '^65529 ')" "1"
stop
sed -i 's/"VLAN_CFG_TYPE" Value="2"/"VLAN_CFG_TYPE" Value="1"/; /OMCI_CUSTOM_ME/d' /var/config/lastgood.xml

# The software image flags are the flash slots the boot recorded.
rm -f /var/run/omcid-swimage
printf 'running=1\nsw_active=1\nprimary_sw_commit=0\nuncommitted=1\n' > /var/run/odi-slot
start respond/build/omcid /tmp/live-s.log -g /tmp/live-gpon -l /tmp/live-link
check "running slot 1, committed slot 0: image 0 is committed" "$(attr 7 0 2)" "01"
check "image 0 is not active" "$(attr 7 0 3)" "00"
check "image 1 is not committed" "$(attr 7 1 2)" "00"
check "image 1 is active" "$(attr 7 1 3)" "01"
stop
printf 'running=0\nsw_active=0\nprimary_sw_commit=0\nuncommitted=0\n' > /var/run/odi-slot
start respond/build/omcid /tmp/live-t.log -g /tmp/live-gpon -l /tmp/live-link
check "running and committed slot 0: image 0 is active and committed" \
      "$(attr 7 0 2)$(attr 7 0 3)$(attr 7 1 2)$(attr 7 1 3)" "01010000"
stop
rm -f /var/run/odi-slot

# The version follows the reported version of the ACTIVE image.
: > /var/config/omci-identity.on
start respond/build/omcid /tmp/live-b.log -g /tmp/live-gpon -l /tmp/live-link
check "identity on, image 0 active: the circuit pack version is OMCI_SW_VER1" \
      "$(attr 6 257 4)" "4f44492d544553542d4100000000"
stop
# Image 1 active and committed: "SWI1", then committed/active/valid of each.
printf 'SWI1\000\000\001\001\001\001' > /var/run/omcid-swimage
start respond/build/omcid /tmp/live-c.log -g /tmp/live-gpon -l /tmp/live-link
check "image 1 active: the circuit pack version is OMCI_SW_VER2" \
      "$(attr 6 257 4)" "4f44492d544553542d4200000000"
check "and it is the version of software image 1" "$(attr 7 1 1)" "4f44492d544553542d4200000000"
stop
rm -f /var/config/omci-identity.on /var/run/omcid-swimage

# ------------------------------------------------------------ the traced daemon
# The DDM stub answers Rx power word 1000 (100 uW) and Tx 5000 (500 uW):
# -10 dBm and about -3 dBm, in 0.002 dB steps.
start respond/build/omcid-drvtrace /tmp/live-d.log -g /tmp/live-gpon -l /tmp/live-link
check "ANI-G optical signal level follows the Rx DDM word (-10 dBm)" \
      "$(attr 263 32769 10)" "ec8d"
check "ANI-G transmit optical level follows the Tx DDM word (-3 dBm)" \
      "$(attr 263 32769 14)" "fa22"
stop

if [ "$fail" -ne 0 ]; then
	echo "$fail check(s) FAILED"
	exit 1
fi
echo "all checks passed"
